#include "AccountMailClient.h"   // ★ 맨 앞 — 소켓 헤더(NetIoModel.h)가 Windows.h 보다 먼저 와야 한다(서버 간 패킷이 코어 헤더를 끌어온다)
#include "MailHandler.h"

#if USE_MAIL

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <Protocol/ServerLinkProtocol.h>   // 계정 우편 서버 간 답 · LinkMailResult · AccMailState
#include "MailSystem.h"
#include "MailDraft.h"
#include "SessionHub.h"
#include "SelfNotifier.h"    // 운영자 지급의 알림 한 줄
#include "EnterGate.h"       // NowMs — 전 계정 지급 대기 30초
#include "ItemHandler.h"
#include "Player.h"
#include "GameMonitor.h"
#include "PacketRoutes.h"
#include "SerialBuffer.h"
#include "PacketBuild.h"     // 머리 쓰기·닫기 · 이름 칸 쓰기
#include <Protocol/Protocol.h>
#include <Identity/CharacterName.h>
#include <Text/TextUtf.h>
#include <Common/ErrorLog.h>

using namespace PacketBuild;

// ==========================================================================
// 우편 S2C 조립 — 이 클래스만 부른다. 쓰는 법은 PacketBuild.h 머리의 [규칙] 주석과 같다(스칼라는 << · pack(1)).
// ==========================================================================
namespace
{
    template <int N>
    void WriteCells(CSerialBuffer* buf, const char16_t (&cells)[N])
    {
        for (int i = 0; i < N; ++i)
            *buf << static_cast<uint16_t>(cells[i]);
    }

    // DB 의 UTF-8 → 고정 칸. 깨졌으면 칸이 전부 0 이고 false — 부르는 쪽이 로그를 남긴다(행은 남는다)
    template <int N, size_t B>
    bool CellsFrom(const DBBytes<B>& src, char16_t (&out)[N])
    {
        return TextUtf::FromUtf8(reinterpret_cast<const char*>(src.data), src.len, out, N);
    }

    // name = CHAR_NAME_MAX_LEN 칸 · nullptr 이면 빈 칸(OTHER_CHARACTER 에만 이름이 실린다)
    CSerialBuffer* MakeMailResult(MailAction action, MailResult result, int64_t mailId, const char16_t* name)
    {
        char16_t cells[CHAR_NAME_MAX_LEN] = {};
        if (name != nullptr)
            std::memcpy(cells, name, sizeof(cells));
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_MAIL_RESULT::TYPE);
        *buf << static_cast<uint8_t>(action);
        *buf << static_cast<uint8_t>(result);
        *buf << static_cast<int64_t>(mailId);
        WriteNameCells(buf, cells);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_MAIL_RESULT));
        return buf;
    }

    CSerialBuffer* MakeMailCount(MailTab tab, MailCountKind known, uint16_t count)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_MAIL_COUNT::TYPE);
        *buf << static_cast<uint8_t>(tab);
        *buf << static_cast<uint8_t>(known);
        *buf << static_cast<uint16_t>(count);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_MAIL_COUNT));
        return buf;
    }

    CSerialBuffer* MakeMailBody(MailTab tab, int64_t mailId, const char16_t (&body)[MAIL_BODY_MAX])
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_MAIL_BODY::TYPE);
        *buf << static_cast<uint8_t>(tab);
        *buf << static_cast<int64_t>(mailId);
        WriteCells(buf, body);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_MAIL_BODY));
        return buf;
    }

    // 목록 한 줄(Protocol.h MailLine 138B) — 필드 차례대로. 쓰는 첨부 칸 뒤는 0 으로 채운다(줄은 고정 길이 · 장은 줄 수만큼 가변)
    void WriteMailLine(CSerialBuffer* buf, const DBMailBrief& brief, CharacterId owner)
    {
        char16_t fromName[CHAR_NAME_MAX_LEN];
        char16_t title[MAIL_TITLE_MAX];
        if (!CellsFrom(brief.fromName, fromName) || !CellsFrom(brief.title, title))
            SLOG_ERROR("[MAIL] list text broken (character={}, mail={}) — 그 칸을 비워 보낸다", owner, brief.mailId);
        MailAttach::Items attach;
        if (!MailAttach::Read(brief.attach.data, static_cast<int>(brief.attach.len), attach))
            SLOG_ERROR("[MAIL] list attach broken (character={}, mail={}, bytes={}) — 첨부를 비워 보낸다 · 받기는 ITEM_UNKNOWN",
                       owner, brief.mailId, brief.attach.len);

        *buf << static_cast<int64_t>(brief.mailId);
        *buf << static_cast<int64_t>(brief.money);
        WriteCells(buf, fromName);
        WriteCells(buf, title);
        *buf << static_cast<uint32_t>(brief.remainSec > 0 ? brief.remainSec : 0);   // 막 지난 줄이 음수로 오면 0
        *buf << static_cast<uint8_t>(brief.read != 0 ? MAIL_LINE_READ : 0);          // 캐릭터 우편엔 「받는 중」이 없다
        *buf << static_cast<uint8_t>(attach.count);
        for (int i = 0; i < MAIL_MAX_ITEMS; ++i)
        {
            const bool used = i < attach.count;
            *buf << static_cast<int32_t>(used ? attach.items[i].itemId : 0);
            *buf << static_cast<int32_t>(used ? attach.items[i].count : 0);
        }
    }

    // 받는 사람이 해제된 뒤 온 회신 — 세션 번호(재사용 세대 포함)와 캐릭터 번호가 둘 다 맞아야 그 사람이다(CharacterStore 의 회신 찾기와 같은 꼴)
    bool SameOwner(const CPlayer* p, const DBMailReply& reply)
    {
        return p != nullptr && p->_characterId == reply.characterId;
    }

    // ── 계정 우편 ──
    static_assert(LINK_ACCMAIL_LIST_ROWS == MAIL_LIST_PAGE, "서버 간 목록 한 장 = 웹 목록 한 장 — 장을 그대로 옮긴다");
    constexpr int ACC_LIST_MAX_PAGES = MAIL_LIST_MAX / MAIL_LIST_PAGE;   // 10장 = 100통 — 캐릭터 우편 목록 상한과 같다

    // 서버 간 결과 → 플레이어에게 보일 사유(mail-plan.md 9절 「result는 LinkMailResult(…)이고 존 서버가 MailResult로 옮겨 플레이어에게 보낸다」)
    MailResult FromLink(uint8_t result)
    {
        static_assert(LINK_MAIL_RESULT_COUNT == 7, "LinkMailResult 를 늘렸으면 플레이어에게 보일 사유를 여기서 정할 것");
        switch (static_cast<LinkMailResult>(result))
        {
        case LinkMailResult::OK:            return MailResult::OK;
        case LinkMailResult::GONE:          return MailResult::GONE;
        case LinkMailResult::BUSY:          return MailResult::BUSY;            // 계정 서버 DB 큐가 찼다
        case LinkMailResult::DB_ERROR:      return MailResult::DB_ERROR;
        case LinkMailResult::NO_ACCOUNT:    return MailResult::DB_ERROR;        // 지급의 답에만 나온다 — 여기 오면 계정 서버의 답이 틀린 것
        case LinkMailResult::CANNOT_DELETE: return MailResult::CANNOT_DELETE;
        case LinkMailResult::OFFLINE:       return MailResult::ACC_OFFLINE;     // 월드 서버가 계정 서버에 못 넘겼다
        }
        return MailResult::DB_ERROR;   // 모르는 값
    }

    // 계정 우편 목록 한 줄 → MailLine(138B). 보낸 사람 칸은 비운다(웹이 「운영자」로 그린다) · read → 0번 비트 · 받는 중 → 1번 비트(9절)
    void WriteAccMailLine(CSerialBuffer* buf, const LinkAccMailLine& line, CharacterId owner)
    {
        const char16_t noName[CHAR_NAME_MAX_LEN] = {};
        char16_t title[MAIL_TITLE_MAX];
        uint8_t bytes[LINK_MAIL_ATTACH_BYTES];
        std::memcpy(title, line.title, sizeof(title));   // 서버 간 패킷은 pack(1) — 칸을 정렬된 배열로 옮긴다
        std::memcpy(bytes, line.attach, sizeof(bytes));
        const int64_t mailId = line.mailId;
        const int attachLen = line.attachLen;
        MailAttach::Items attach;
        if (!MailAttach::Read(bytes, attachLen, attach))   // 66B 를 넘는 길이도 여기서 거절된다
            SLOG_ERROR("[MAIL] acc list attach broken (character={}, account_mail={}, bytes={}) — 첨부를 비워 보낸다 · 받기는 ITEM_UNKNOWN",
                       owner, mailId, attachLen);
        uint8_t flags = 0;
        if (line.read != 0)
            flags |= MAIL_LINE_READ;
        if (line.state == static_cast<uint8_t>(AccMailState::CLAIMING))
            flags |= MAIL_LINE_CLAIMING;

        *buf << static_cast<int64_t>(mailId);
        *buf << static_cast<int64_t>(line.money);
        WriteCells(buf, noName);
        WriteCells(buf, title);
        *buf << static_cast<uint32_t>(line.remainSec);
        *buf << static_cast<uint8_t>(flags);
        *buf << static_cast<uint8_t>(attach.count);
        for (int i = 0; i < MAIL_MAX_ITEMS; ++i)
        {
            const bool used = i < attach.count;
            *buf << static_cast<int32_t>(used ? attach.items[i].itemId : 0);
            *buf << static_cast<int32_t>(used ? attach.items[i].count : 0);
        }
    }

    // 서버 간 답의 주인 — 세션 번호(재사용 세대 포함)와 캐릭터 번호가 둘 다 맞아야 그 사람이다. 나갔으면 nullptr(정상 — 상태도 사람과 함께 사라졌다)
    CPlayer* FindLinkOwner(CSessionHub& hub, CGameMonitor& monitor, SessionId sessionId, CharacterId characterId)
    {
        CPlayer* p = hub.FindPlayer(sessionId);
        if (p == nullptr || p->_characterId != characterId)
        {
            monitor._mail._lateReplies.Inc();
            return nullptr;
        }
        return p;
    }

    // + 상태가 그 답을 기다렸나 — 답은 요청마다 한 번(답 · 시한 · 끊김 중 하나)이라 어긋나면 있을 수 없는 일이다. 오류 로그를 남기고 버린다
    CPlayer* FindLinkOwner(CSessionHub& hub, CGameMonitor& monitor, SessionId sessionId, CharacterId characterId,
                           CPlayer::MailOp want, const char* what)
    {
        CPlayer* p = FindLinkOwner(hub, monitor, sessionId, characterId);
        if (p != nullptr && p->_mailOp != want)
        {
            monitor._mail._lateReplies.Inc();
            SLOG_ERROR("[MAIL] acc {} answer not awaited (character={}, state={})", what, characterId, static_cast<int>(p->_mailOp));
            return nullptr;
        }
        return p;
    }
}

bool CMailHandler::RegisterRoutes(CPacketRoutes& routes)
{
    bool ok = true;
    // 이 여섯 줄이 없으면 우편 요청이 조용히 버려진다(등록 안 된 번호는 OnReceived 가 조용히 버린다 — 답도 로그도 없다)
    ok &= routes.OnPlayer(MsgType::C2S_MAIL_LIST,    sizeof(MSG_C2S_MAIL_LIST),    [this](CPlayer* p, CSerialBuffer* m) { RecvList(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_MAIL_READ,    sizeof(MSG_C2S_MAIL_READ),    [this](CPlayer* p, CSerialBuffer* m) { RecvRead(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_MAIL_SEND,    sizeof(MSG_C2S_MAIL_SEND),    [this](CPlayer* p, CSerialBuffer* m) { RecvSend(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_MAIL_TAKE,    sizeof(MSG_C2S_MAIL_TAKE),    [this](CPlayer* p, CSerialBuffer* m) { RecvTake(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_ACCMAIL_TAKE, sizeof(MSG_C2S_ACCMAIL_TAKE), [this](CPlayer* p, CSerialBuffer* m) { RecvAccTake(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_MAIL_DELETE,  sizeof(MSG_C2S_MAIL_DELETE),  [this](CPlayer* p, CSerialBuffer* m) { RecvDelete(p, m); });
    return ok;
}

// ==========================================================================
// 받기 — 규칙이 문을 열면 잡을 낸다. 거절은 그 자리에서 결과 한 장
// ==========================================================================
void CMailHandler::RecvList(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_MAIL_LIST recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    if (recvMsg.tab == static_cast<uint8_t>(MailTab::ACCOUNT))
    {
        const MailResult door = _mailSystem.BeginAccList(player);
        if (door != MailResult::OK)
        {
            SendResult(player, MailAction::LIST, door, 0);
            return;
        }
        AskAccListPage(player, 0, 0);   // 첫 장 — 더 있으면 답이 다음 장을 묻는다
        return;
    }
    if (recvMsg.tab != static_cast<uint8_t>(MailTab::CHARACTER))
        return;   // 모르는 탭 — 웹은 안 보낸다. 로그로 남기면 패킷 하나로 로그를 채울 수 있어 조용히 버린다

    const MailResult r = _mailSystem.BeginList(player);
    if (r != MailResult::OK)
    {
        SendResult(player, MailAction::LIST, r, 0);
        return;
    }
    DBMailJob job{};
    job.op = DBMailOp::LIST;
    if (!Ask(player, job))
    {
        _mailSystem.End(player);
        SendResult(player, MailAction::LIST, MailResult::BUSY, 0);
    }
}

void CMailHandler::RecvRead(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_MAIL_READ recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    const int64_t mailId = recvMsg.mailId;
    if (recvMsg.tab == static_cast<uint8_t>(MailTab::ACCOUNT))
    {
        const MailResult door = _mailSystem.BeginAccRead(player);
        if (door != MailResult::OK)
        {
            SendResult(player, MailAction::READ, door, mailId);
            return;
        }
        const SessionId sessionId = player->_sessionId;
        const CharacterId characterId = player->_characterId;
        const bool sent = _accountMailClient.Read(ToInt(player->_accountId), mailId,
            [this, sessionId, characterId, mailId](const LINK_ACCMAIL_READ_ACK* ack) { OnAccReadAck(sessionId, characterId, mailId, ack); });
        if (!sent)
        {
            _mailSystem.End(player);
            SendResult(player, MailAction::READ, MailResult::ACC_OFFLINE, mailId);
        }
        return;
    }
    if (recvMsg.tab != static_cast<uint8_t>(MailTab::CHARACTER))
        return;

    const MailResult r = _mailSystem.BeginRead(player);
    if (r != MailResult::OK)
    {
        SendResult(player, MailAction::READ, r, mailId);
        return;
    }
    DBMailJob job{};
    job.op = DBMailOp::READ_BODY;
    job.mailId = mailId;
    if (!Ask(player, job))
    {
        _mailSystem.End(player);
        SendResult(player, MailAction::READ, MailResult::BUSY, mailId);
    }
}

void CMailHandler::RecvSend(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_MAIL_SEND recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    const MailResult r = _mailSystem.BeginSend(player, recvMsg);
    if (r != MailResult::OK)
    {
        SendResult(player, MailAction::SEND, r, 0);
        return;
    }
    // 받는 이름 — BeginSend 가 이름 규칙을 봤다. 패킷 칸을 정렬된 배열로 옮겨 UTF-8 로(칸 주소를 넘기지 않는다 — pack(1))
    char16_t toName[CHAR_NAME_MAX_LEN];
    std::memcpy(toName, recvMsg.toName, sizeof(toName));
    const std::string name = CharacterName::ToUtf8(toName);

    DBMailJob job{};
    job.op = DBMailOp::FIND_RECEIVER;
    assert(name.size() <= sizeof(job.name.data));
    std::memcpy(job.name.data, name.data(), name.size());
    job.name.len = static_cast<unsigned long>(name.size());
    if (!Ask(player, job))
    {
        _mailSystem.End(player);   // 초안을 버린다 — 아직 아무것도 안 줄였다
        SendResult(player, MailAction::SEND, MailResult::BUSY, 0);
    }
}

void CMailHandler::RecvTake(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_MAIL_TAKE recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    const int64_t mailId = recvMsg.mailId;

    const MailResult r = _mailSystem.BeginTake(player);
    if (r != MailResult::OK)
    {
        SendResult(player, MailAction::TAKE, r, mailId);
        return;
    }
    DBMailJob job{};
    job.op = DBMailOp::READ_ONE;
    job.mailId = mailId;
    if (!Ask(player, job))
    {
        _mailSystem.End(player);
        SendResult(player, MailAction::TAKE, MailResult::BUSY, mailId);
    }
}

void CMailHandler::RecvAccTake(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_ACCMAIL_TAKE recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    const int64_t mailId = recvMsg.mailId;

    // 계정 우편 받기 — 먼저 읽어 그 우편의 상태를 본다(mail-plan.md 6절 받기 1번). 다음 걸음은 답마다 규칙이 정한다(RunAccStep)
    const MailResult r = _mailSystem.BeginAccTake(player);
    if (r != MailResult::OK)
    {
        SendResult(player, MailAction::ACC_TAKE, r, mailId);
        return;
    }
    AskAccTakeRead(player, mailId);
}

void CMailHandler::RecvDelete(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_MAIL_DELETE recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    const int64_t mailId = recvMsg.mailId;
    if (recvMsg.tab == static_cast<uint8_t>(MailTab::ACCOUNT))
    {
        const MailResult door = _mailSystem.BeginAccErase(player);
        if (door != MailResult::OK)
        {
            SendResult(player, MailAction::ACC_ERASE, door, mailId);
            return;
        }
        const SessionId sessionId = player->_sessionId;
        const CharacterId characterId = player->_characterId;
        const bool sent = _accountMailClient.Erase(ToInt(player->_accountId), mailId,
            [this, sessionId, characterId, mailId](const LINK_ACCMAIL_ERASE_ACK* ack) { OnAccEraseAck(sessionId, characterId, mailId, ack); });
        if (!sent)
        {
            _mailSystem.End(player);
            SendResult(player, MailAction::ACC_ERASE, MailResult::ACC_OFFLINE, mailId);
        }
        return;
    }
    if (recvMsg.tab != static_cast<uint8_t>(MailTab::CHARACTER))
        return;

    const MailResult r = _mailSystem.BeginErase(player);
    if (r != MailResult::OK)
    {
        SendResult(player, MailAction::ERASE, r, mailId);
        return;
    }
    DBMailJob job{};
    job.op = DBMailOp::ERASE;
    job.mailId = mailId;
    if (!Ask(player, job))
    {
        _mailSystem.End(player);
        SendResult(player, MailAction::ERASE, MailResult::BUSY, mailId);
    }
}

bool CMailHandler::Ask(CPlayer* player, DBMailJob& job)
{
    job.sessionId = player->_sessionId;
    job.characterId = player->_characterId;
    return _characterStore.RequestMailJob(job);
}

// ==========================================================================
// 입장 때 수
// ==========================================================================
void CMailHandler::OnEntered(CPlayer* player)
{
    if (player->_accountId == NO_ACCOUNT)
        return;   // 시험 입장 길 — 우편이 전부 거절되는 사람이라 수도 안 보낸다(부하 측정 조건을 안 바꾼다 · Protocol.h 우편 절 머리)

    // 계정 우편 탭 — 월드 서버를 거쳐 계정 서버에 묻는다. 답에 이 캐릭터가 받는 중인 행이 있으면 이어서 처리한다(OnAccCountAck)
    AskAccCount(player, 0);

    // 캐릭터 우편 탭 — 상태(_mailOp)를 안 쓴다: 읽기만 하고 회신이 표시 하나만 고친다(mail-plan.md 7절 끝)
    DBMailJob job{};
    job.op = DBMailOp::COUNT_PENDING;
    if (!Ask(player, job))
        SendCount(player, MailTab::CHARACTER, MailCountKind::UNKNOWN, 0);
}

// ==========================================================================
// 저장소의 우편 회신
// ==========================================================================
void CMailHandler::OnMailDbReply(const DBMailReply& reply)
{
    CPlayer* player = _sessionHub.FindPlayer(reply.sessionId);
    if (!SameOwner(player, reply))
    {
        _gameMonitor._mail._lateReplies.Inc();   // 나갔다(정상) — 상태도 사람과 함께 사라졌다(mail-plan.md 7절 「끊김」)
        return;
    }
    if (!_mailSystem.Waiting(player, reply.op))
    {
        // 있을 수 없는 일 — 한 사람에게 우편 잡은 한 번에 하나다(_mailOp). 상태를 건드리지 않고 버린다
        _gameMonitor._mail._lateReplies.Inc();
        SLOG_ERROR("[MAIL] reply not awaited (character={}, op={}, state={}, ok={})",
                   reply.characterId, static_cast<int>(reply.op), static_cast<int>(player->_mailOp), reply.ok);
        return;
    }

    static_assert(static_cast<int>(DBMailOp::COUNT) == 8, "DBMailOp 을 늘렸으면 회신 갈래를 여기에 더할 것");
    switch (reply.op)
    {
    case DBMailOp::LIST:          OnListReply(player, reply);  return;
    case DBMailOp::READ_BODY:     OnBodyReply(player, reply);  return;
    case DBMailOp::ERASE:         OnEraseReply(player, reply); return;
    case DBMailOp::FIND_RECEIVER: OnFindReply(player, reply);  return;
    case DBMailOp::READ_ONE:      OnTakeReply(player, reply);  return;
    case DBMailOp::COUNT_PENDING: OnCountReply(player, reply); return;
    case DBMailOp::FIND_MOVED:    OnMovedReply(player, reply); return;
    case DBMailOp::FIND_NAME:     OnNameReply(player, reply);  return;
    case DBMailOp::COUNT:         break;
    }
    SLOG_ERROR("[MAIL] reply op without handler (character={}, op={})", reply.characterId, static_cast<int>(reply.op));
}

void CMailHandler::OnListReply(CPlayer* player, const DBMailReply& reply)
{
    _mailSystem.End(player);
    if (!reply.ok)
    {
        SendResult(player, MailAction::LIST, MailResult::DB_ERROR, 0);
        return;
    }
    SendList(player, reply.lines);
}

void CMailHandler::OnBodyReply(CPlayer* player, const DBMailReply& reply)
{
    _mailSystem.End(player);
    if (!reply.ok)
    {
        SendResult(player, MailAction::READ, MailResult::DB_ERROR, reply.mailId);
        return;
    }
    if (!reply.found)
    {
        SendResult(player, MailAction::READ, MailResult::GONE, reply.mailId);   // 없음 · 받음 · 기한 지남 · 지움 · 남의 우편 번호
        return;
    }
    char16_t body[MAIL_BODY_MAX];
    if (!CellsFrom(reply.row.body, body))
        SLOG_ERROR("[MAIL] body text broken (character={}, mail={}, bytes={}) — 빈 본문으로 보낸다",
                   reply.characterId, reply.row.mailId, reply.row.body.len);
    _sessionHub.SendPacket(player, MakeMailBody(MailTab::CHARACTER, reply.row.mailId, body));
    _gameMonitor._mail._bodySent.Inc();
}

void CMailHandler::OnEraseReply(CPlayer* player, const DBMailReply& reply)
{
    _mailSystem.End(player);
    if (!reply.ok)
    {
        SendResult(player, MailAction::ERASE, MailResult::DB_ERROR, reply.mailId);
        return;
    }
    if (!reply.found)
    {
        SendResult(player, MailAction::ERASE, MailResult::CANNOT_DELETE, reply.mailId);   // 0행 — 첨부·돈이 남았거나 이미 없다
        return;
    }
    _gameMonitor._mail._erased[static_cast<int>(MailTab::CHARACTER)].Inc();
    SendResult(player, MailAction::ERASE, MailResult::OK, reply.mailId);
}

void CMailHandler::OnFindReply(CPlayer* player, const DBMailReply& reply)
{
    const MailResult r = _mailSystem.FinishSend(player, reply);   // 거절이면 규칙이 초안을 버리고 IDLE 로 뒀다
    if (r != MailResult::OK)
    {
        SendResult(player, MailAction::SEND, r, 0);
        return;
    }
    FlushChanged(player);   // 결과(OK)는 우편 문장이 든 묶음이 확정된 뒤 — OnMailSaved
}

void CMailHandler::OnTakeReply(CPlayer* player, const DBMailReply& reply)
{
    const MailResult r = _mailSystem.FinishTake(player, reply);
    if (r != MailResult::OK)
    {
        SendResult(player, MailAction::TAKE, r, reply.mailId);
        return;
    }
    FlushChanged(player);
}

void CMailHandler::OnCountReply(CPlayer* player, const DBMailReply& reply)
{
    if (!reply.ok)
    {
        SendCount(player, MailTab::CHARACTER, MailCountKind::UNKNOWN, 0);
        return;
    }
    SendCount(player, MailTab::CHARACTER, MailCountKind::EXACT, reply.count);
}

void CMailHandler::OnMovedReply(CPlayer* player, const DBMailReply& reply)
{
    const bool resume = player->_mailResume;   // 규칙이 닫으면(End) 꺼진다 — 먼저 읽는다
    RunAccStep(player, reply.mailId, resume, _mailSystem.AfterAccMoved(player, reply));
}

void CMailHandler::OnNameReply(CPlayer* player, const DBMailReply& reply)
{
    const bool resume = player->_mailResume;
    _mailSystem.End(player);
    if (!reply.ok)
    {
        CloseAccTake(player, reply.mailId, resume, MailResult::DB_ERROR);
        return;
    }
    // 이름이 없으면(그 캐릭터가 지워졌다) 빈 이름으로 알린다 — 「다른 캐릭터가 받는 중」은 그대로 사실이다
    char16_t name[CHAR_NAME_MAX_LEN] = {};
    if (reply.found && !CellsFrom(reply.name, name))
        SLOG_ERROR("[MAIL] acc take claimer name broken (character={}, account_mail={}) — 빈 이름으로 보낸다", reply.characterId, reply.mailId);
    CloseAccTake(player, reply.mailId, resume, MailResult::OTHER_CHARACTER, name);
}

// ==========================================================================
// 계정 우편 — 월드 서버를 거쳐 계정 서버에 묻는다(mail-plan.md 6절 · 9절)
//   요청을 못 보내면(월드 서버 연결 없음 · UseServerLink=0) 그 자리에서 ACC_OFFLINE · 답 없음(시한 · 끊김)은 ACC_TIMEOUT
// ==========================================================================
void CMailHandler::AskAccCount(CPlayer* player, int64_t prevClaim)
{
    const SessionId sessionId = player->_sessionId;
    const CharacterId characterId = player->_characterId;
    const bool sent = _accountMailClient.Count(ToInt(player->_accountId), ToInt(characterId),
        [this, sessionId, characterId, prevClaim](const LINK_ACCMAIL_COUNT_ACK* ack) { OnAccCountAck(sessionId, characterId, prevClaim, ack); });
    if (!sent)
        SendCount(player, MailTab::ACCOUNT, MailCountKind::UNKNOWN, 0);   // 못 읽음 — 웹이 0 을 「없음」으로 읽지 않게
}

void CMailHandler::OnAccCountAck(SessionId sessionId, CharacterId characterId, int64_t prevClaim, const LINK_ACCMAIL_COUNT_ACK* ack)
{
    CPlayer* player = FindLinkOwner(_sessionHub, _gameMonitor, sessionId, characterId);   // 상태를 안 쓴다 — 읽기만 하고 수 하나만 알린다
    if (player == nullptr)
        return;
    if (ack == nullptr || ack->result != static_cast<uint8_t>(LinkMailResult::OK))
    {
        SendCount(player, MailTab::ACCOUNT, MailCountKind::UNKNOWN, 0);
        return;
    }
    SendCount(player, MailTab::ACCOUNT, MailCountKind::EXACT, ack->count);

    const int64_t claim = ack->claimMailId;
    if (claim == 0)
        return;
    if (claim == prevClaim)
    {
        // 방금 이어서 처리한 번호가 또 받는 중 — 이번엔 끝내지 못했다(DONE · UNCLAIM 을 못 보냈거나 시한). 되풀이하지 않고 다음 입장에 맡긴다
        SLOG_WARN("[MAIL] acc resume same mail again (character={}, account_mail={}) — 다음 입장 때 다시 본다", characterId, claim);
        return;
    }
    StartAccResume(player, claim);
}

void CMailHandler::StartAccResume(CPlayer* player, int64_t mailId)
{
    const MailResult r = _mailSystem.BeginAccResume(player);
    if (r != MailResult::OK)
    {
        // 다른 우편 동작 중 · 죽음 · 거래 — 결과 패킷을 안 보낸다(사람이 누른 게 아니다). 다음 입장이나 목록의 받는 중 줄로 이어진다
        SLOG_INFO("[MAIL] acc resume later (character={}, account_mail={}, reason={})", player->_characterId, mailId, MailResultName(r));
        return;
    }
    _gameMonitor._mail._resumed.Inc();
    SLOG_INFO("[MAIL] acc resume start (character={}, account_mail={})", player->_characterId, mailId);
    DBMailJob job{};
    job.op = DBMailOp::FIND_MOVED;
    job.mailId = mailId;
    if (!Ask(player, job))
    {
        _mailSystem.End(player);
        SLOG_WARN("[MAIL] acc resume job refused (character={}, account_mail={}) — 다음 입장 때 다시 본다", player->_characterId, mailId);
    }
}

void CMailHandler::AskAccListPage(CPlayer* player, int64_t beforeMailId, int page)
{
    const SessionId sessionId = player->_sessionId;
    const CharacterId characterId = player->_characterId;
    const bool sent = _accountMailClient.List(ToInt(player->_accountId), beforeMailId,
        [this, sessionId, characterId, page](const LINK_ACCMAIL_LIST_ACK* ack) { OnAccListAck(sessionId, characterId, page, ack); });
    if (!sent)
    {
        _mailSystem.End(player);
        SendResult(player, MailAction::LIST, MailResult::ACC_OFFLINE, 0);
    }
}

void CMailHandler::OnAccListAck(SessionId sessionId, CharacterId characterId, int page, const LINK_ACCMAIL_LIST_ACK* ack)
{
    CPlayer* player = FindLinkOwner(_sessionHub, _gameMonitor, sessionId, characterId, CPlayer::MailOp::ACC_LIST, "list");
    if (player == nullptr)
        return;
    if (ack == nullptr)
    {
        _mailSystem.End(player);
        SendResult(player, MailAction::LIST, MailResult::ACC_TIMEOUT, 0);
        return;
    }
    if (ack->result != static_cast<uint8_t>(LinkMailResult::OK))
    {
        _mailSystem.End(player);
        SendResult(player, MailAction::LIST, FromLink(ack->result), 0);
        return;
    }
    const int count = (std::min)(static_cast<int>(ack->count), LINK_ACCMAIL_LIST_ROWS);
    // 다음 장 — 계정 서버가 더 있다고 했고 · 이 장에 줄이 있고(다음 장의 기준 번호) · 100통 상한 안일 때만
    const bool more = ack->more != 0 && count > 0 && page + 1 < ACC_LIST_MAX_PAGES;
    SendAccListPage(player, *ack, count, page == 0, more);
    if (!more)
    {
        _mailSystem.End(player);
        return;
    }
    const int64_t lastMailId = ack->lines[count - 1].mailId;   // 큰 번호부터 온다 — 이 번호보다 작은 것이 다음 장
    AskAccListPage(player, lastMailId, page + 1);
}

void CMailHandler::SendAccListPage(CPlayer* player, const LINK_ACCMAIL_LIST_ACK& ack, int count, bool first, bool more)
{
    CSerialBuffer* buf = CSerialBuffer::Alloc();
    BeginPacket(buf, MSG_S2C_MAIL_LIST::TYPE);
    *buf << static_cast<uint8_t>(MailTab::ACCOUNT);
    *buf << static_cast<uint8_t>(first ? 1 : 0);
    *buf << static_cast<uint8_t>(more ? 1 : 0);
    *buf << static_cast<uint8_t>(count);
    for (int i = 0; i < count; ++i)
        WriteAccMailLine(buf, ack.lines[i], player->_characterId);
    FinalizePacket(buf);
    assert(buf->GetDataSize() == offsetof(MSG_S2C_MAIL_LIST, lines) + sizeof(MailLine) * static_cast<size_t>(count));
    _sessionHub.SendPacket(player, buf);
    _gameMonitor._mail._listPagesSent.Inc();
}

void CMailHandler::OnAccReadAck(SessionId sessionId, CharacterId characterId, int64_t mailId, const LINK_ACCMAIL_READ_ACK* ack)
{
    CPlayer* player = FindLinkOwner(_sessionHub, _gameMonitor, sessionId, characterId, CPlayer::MailOp::ACC_READ, "read");
    if (player == nullptr)
        return;
    _mailSystem.End(player);
    if (ack == nullptr)
    {
        SendResult(player, MailAction::READ, MailResult::ACC_TIMEOUT, mailId);
        return;
    }
    if (ack->result != static_cast<uint8_t>(LinkMailResult::OK))
    {
        SendResult(player, MailAction::READ, FromLink(ack->result), mailId);
        return;
    }
    static_assert(sizeof(LinkAccMailFull::body) == sizeof(char16_t) * MAIL_BODY_MAX, "서버 간 본문 칸 = 웹 본문 칸");
    char16_t body[MAIL_BODY_MAX];
    std::memcpy(body, ack->mail.body, sizeof(body));   // pack(1) — 정렬된 배열로 옮긴다
    _sessionHub.SendPacket(player, MakeMailBody(MailTab::ACCOUNT, mailId, body));
    _gameMonitor._mail._bodySent.Inc();
}

void CMailHandler::OnAccEraseAck(SessionId sessionId, CharacterId characterId, int64_t mailId, const LINK_ACCMAIL_ERASE_ACK* ack)
{
    CPlayer* player = FindLinkOwner(_sessionHub, _gameMonitor, sessionId, characterId, CPlayer::MailOp::ACC_ERASE, "erase");
    if (player == nullptr)
        return;
    _mailSystem.End(player);
    const MailResult r = ack == nullptr ? MailResult::ACC_TIMEOUT : FromLink(ack->result);
    if (r == MailResult::OK)
        _gameMonitor._mail._erased[static_cast<int>(MailTab::ACCOUNT)].Inc();
    SendResult(player, MailAction::ACC_ERASE, r, mailId);
}

// ── 계정 우편 받기 — 읽기 → (이미 옮겼나) → 받는 중 표시 → 다시 판정 → 가방 + 내 우편 한 줄 → 확정 뒤 DONE (mail-plan.md 6절 받기 1~7번) ──
void CMailHandler::AskAccTakeRead(CPlayer* player, int64_t mailId)
{
    const SessionId sessionId = player->_sessionId;
    const CharacterId characterId = player->_characterId;
    const bool sent = _accountMailClient.Read(ToInt(player->_accountId), mailId,
        [this, sessionId, characterId, mailId](const LINK_ACCMAIL_READ_ACK* ack) { OnAccTakeReadAck(sessionId, characterId, mailId, ack); });
    if (!sent)
    {
        _mailSystem.End(player);
        SendResult(player, MailAction::ACC_TAKE, MailResult::ACC_OFFLINE, mailId);
    }
}

void CMailHandler::OnAccTakeReadAck(SessionId sessionId, CharacterId characterId, int64_t mailId, const LINK_ACCMAIL_READ_ACK* ack)
{
    CPlayer* player = FindLinkOwner(_sessionHub, _gameMonitor, sessionId, characterId, CPlayer::MailOp::ACC_TAKE_READ, "take read");
    if (player == nullptr)
        return;
    const bool resume = player->_mailResume;   // 이어서 처리는 ACC_CHECK_MOVED 에서 시작해 여기 안 오지만, 닫기 전에 읽는 꼴을 갈래마다 같게 둔다
    if (ack == nullptr || ack->result != static_cast<uint8_t>(LinkMailResult::OK))
    {
        _mailSystem.End(player);
        CloseAccTake(player, mailId, resume, ack == nullptr ? MailResult::ACC_TIMEOUT : FromLink(ack->result));
        return;
    }
    RunAccStep(player, mailId, resume, _mailSystem.AfterAccRead(player, ack->mail));
}

bool CMailHandler::AskAccClaim(CPlayer* player, int64_t mailId)
{
    const SessionId sessionId = player->_sessionId;
    const CharacterId characterId = player->_characterId;
    return _accountMailClient.Claim(ToInt(player->_accountId), mailId, ToInt(characterId),
        [this, sessionId, characterId, mailId](const LINK_ACCMAIL_CLAIM_ACK* ack) { OnAccClaimAck(sessionId, characterId, mailId, ack); });
}

void CMailHandler::OnAccClaimAck(SessionId sessionId, CharacterId characterId, int64_t mailId, const LINK_ACCMAIL_CLAIM_ACK* ack)
{
    CPlayer* player = FindLinkOwner(_sessionHub, _gameMonitor, sessionId, characterId, CPlayer::MailOp::ACC_CLAIM, "claim");
    if (player == nullptr)
        return;   // 받는 중 표시가 걸렸을 수 있다 — 이 캐릭터의 다음 입장 때 이어진다
    const bool resume = player->_mailResume;
    if (ack == nullptr || ack->result != static_cast<uint8_t>(LinkMailResult::OK))
    {
        // 시한이면 표시가 걸렸을 수 있다(ACC_TIMEOUT — 다시 열면 이어진다) · GONE 이면 그사이 받았거나 지워졌다
        _mailSystem.End(player);
        CloseAccTake(player, mailId, resume, ack == nullptr ? MailResult::ACC_TIMEOUT : FromLink(ack->result));
        return;
    }
    RunAccStep(player, mailId, resume, _mailSystem.AfterAccClaim(player, ack->mail));
}

void CMailHandler::RunAccStep(CPlayer* player, int64_t mailId, bool resume, const AccTakeDecision& decision)
{
    switch (decision.step)
    {
    case AccTakeStep::CLOSE:
        CloseAccTake(player, mailId, resume, decision.result);
        return;

    case AccTakeStep::DONE_CLOSE:
        // 이미 내 우편으로 옮겼다 — 앞선 DONE 이 안 닿았다. 다시 보내고 OK(못 보내도 옮긴 건 사실이다 — 다음 입장에 또 보낸다)
        SendDone(ToInt(player->_accountId), mailId, ToInt(player->_characterId));
        CloseAccTake(player, mailId, resume, MailResult::OK);
        return;

    case AccTakeStep::UNCLAIM_CLOSE:
        if (_accountMailClient.Unclaim(ToInt(player->_accountId), mailId, ToInt(player->_characterId)))
            _gameMonitor._mail._unclaimed.Inc();
        else
            SLOG_WARN("[MAIL] acc unclaim not sent (character={}, account_mail={}) — 받는 중으로 남아 이 캐릭터의 입장 때 다시 판정한다",
                      player->_characterId, mailId);
        CloseAccTake(player, mailId, resume, decision.result);
        return;

    case AccTakeStep::CLAIM:
        if (!AskAccClaim(player, mailId))
        {
            _mailSystem.End(player);
            CloseAccTake(player, mailId, resume, MailResult::ACC_OFFLINE);
        }
        return;

    case AccTakeStep::CHECK_MOVED:
    case AccTakeStep::OTHER_NAME:
    {
        DBMailJob job{};
        job.op = decision.step == AccTakeStep::CHECK_MOVED ? DBMailOp::FIND_MOVED : DBMailOp::FIND_NAME;
        job.mailId = mailId;   // FIND_MOVED = 계정 우편 번호 · FIND_NAME 은 회신에서 그 번호로 결과를 낸다
        job.otherCharacterId = CharacterId{ decision.otherCharacterId };
        if (!Ask(player, job))
        {
            _mailSystem.End(player);
            CloseAccTake(player, mailId, resume, MailResult::BUSY);
        }
        return;
    }

    case AccTakeStep::SAVE:
        FlushChanged(player);   // 결과(OK)와 DONE 은 내 우편 한 줄이 든 묶음이 확정된 뒤 — OnMailSaved
        return;
    }
    // 모르는 걸음 — 규칙과 이 switch 가 어긋났다. 상태를 닫고 알린다(가방은 SAVE 에서만 바뀐다)
    SLOG_ERROR("[MAIL] acc take step without handler (character={}, step={})", player->_characterId, static_cast<int>(decision.step));
    _mailSystem.End(player);
    CloseAccTake(player, mailId, resume, MailResult::DB_ERROR);
}

void CMailHandler::CloseAccTake(CPlayer* player, int64_t mailId, bool resume, MailResult result, const char16_t* name)
{
    SendResult(player, resume ? MailAction::RESUME : MailAction::ACC_TAKE, result, mailId, name);
    if (resume)
        AskAccCount(player, mailId);   // 수를 다시 알리고, 받는 중인 다음 행이 있으면 이어간다(같은 번호가 또 오면 멈춘다)
}

void CMailHandler::SendDone(int64_t accountId, int64_t mailId, int64_t characterId)
{
    if (!_accountMailClient.Finish(accountId, mailId, characterId))
        SLOG_WARN("[MAIL] acc done not sent (account={}, account_mail={}, character={}) — 받는 중으로 남아 그 캐릭터의 입장 때 「이미 옮겼나」가 다시 보낸다",
                  accountId, mailId, characterId);
}

void CMailHandler::OnMailSaved(CPlayer* player, const DBMailStmt& stmt)
{
    // 계정 우편 받기 — 내 우편 한 줄이 확정됐다. 받는 중 → 받음(DONE)을 사람이 있든 없든 보낸다(떠난 기록으로 확정돼도 · mail-plan.md 6절 7번)
    if (stmt.kind == DBMailStmtKind::ACC_TAKE_INSERT)
        SendDone(stmt.accountId, stmt.fromAccountMailId, stmt.characterId);
    if (player == nullptr)
        return;   // 떠난 기록으로 확정 — 결과를 알릴 사람이 없다
    const bool resume = player->_mailResume;   // Confirm 이 끈다
    if (!_mailSystem.Confirm(player, stmt.kind))
    {
        // 있을 수 없는 일 — 문장은 *_SAVE 상태에서만 대기로 오르고 그 상태는 이 확정으로만 풀린다
        SLOG_ERROR("[MAIL] saved stmt not awaited (character={}, kind={}, state={}, mail={})",
                   player->_characterId, static_cast<int>(stmt.kind), static_cast<int>(player->_mailOp), stmt.mailId);
        return;
    }
    static_assert(static_cast<int>(DBMailStmtKind::COUNT) == 3, "우편 문장 종류를 늘렸으면 확정 뒤 결과를 여기서 정할 것");
    switch (stmt.kind)
    {
    case DBMailStmtKind::SEND_INSERT:     SendResult(player, MailAction::SEND, MailResult::OK, 0);           return;
    case DBMailStmtKind::TAKE_UPDATE:     SendResult(player, MailAction::TAKE, MailResult::OK, stmt.mailId); return;
    case DBMailStmtKind::ACC_TAKE_INSERT: CloseAccTake(player, stmt.fromAccountMailId, resume, MailResult::OK); return;
    case DBMailStmtKind::COUNT:           break;
    }
}

void CMailHandler::FlushChanged(CPlayer* player)
{
    // 저장 요청이 먼저 — 같은 틱의 둘째 요청(FlushInventory 의 BAG)은 무시되어 사유 지표가 mail 로 센다.
    //   돈만 줄고 칸이 그대로인 우편(글 + 수수료)도 이 요청이 묶음을 만든다(ShouldSave 가 대기 문장을 본다)
    _characterStore.RequestSave(player, SaveReason::MAIL);
    _itemHandler.FlushInventory(player);
}

// ==========================================================================
// 결과 패킷
// ==========================================================================
void CMailHandler::SendResult(CPlayer* player, MailAction action, MailResult result, int64_t mailId, const char16_t* name)
{
    if (result != MailResult::OK)
        _gameMonitor._mail._rejected[static_cast<int>(result)].Inc();
    _sessionHub.SendPacket(player, MakeMailResult(action, result, mailId, name));
    _gameMonitor._mail._resultSent.Inc();
}

void CMailHandler::SendCount(CPlayer* player, MailTab tab, MailCountKind known, int64_t count)
{
    const uint16_t shown = static_cast<uint16_t>(std::clamp<int64_t>(count, 0, UINT16_MAX));   // 칸이 u16 — 그 이상은 칸 상한으로 보인다
    _sessionHub.SendPacket(player, MakeMailCount(tab, known, shown));
    _gameMonitor._mail._countSent.Inc();
}

void CMailHandler::SendList(CPlayer* player, const std::vector<DBMailBrief>& lines)
{
    // 10줄씩 — 빈 목록도 한 장(first=1 · more=0)이라 웹이 그 탭을 비운다
    const int total = static_cast<int>((std::min)(lines.size(), static_cast<size_t>(MAIL_LIST_MAX)));
    int sent = 0;
    do
    {
        const int count = (std::min)(MAIL_LIST_PAGE, total - sent);
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_MAIL_LIST::TYPE);
        *buf << static_cast<uint8_t>(MailTab::CHARACTER);
        *buf << static_cast<uint8_t>(sent == 0 ? 1 : 0);
        *buf << static_cast<uint8_t>(sent + count < total ? 1 : 0);
        *buf << static_cast<uint8_t>(count);
        for (int i = 0; i < count; ++i)
            WriteMailLine(buf, lines[static_cast<size_t>(sent + i)], player->_characterId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == offsetof(MSG_S2C_MAIL_LIST, lines) + sizeof(MailLine) * static_cast<size_t>(count));
        _sessionHub.SendPacket(player, buf);
        _gameMonitor._mail._listPagesSent.Inc();
        sent += count;
    } while (sent < total);
}

// ==========================================================================
// 운영자 지급 — /accmail (mail-plan.md 6절 보내기)
// ==========================================================================
namespace
{
    // 1,000 처럼 세 자리마다 쉼표 — 미리 보기 줄의 돈(6절 「전 계정 지급 대기 — 돈 1,000 · …」)
    std::string WithCommas(int64_t v)
    {
        std::string digits = std::to_string(v);
        const size_t start = (!digits.empty() && digits[0] == '-') ? 1 : 0;
        for (size_t i = digits.size(); i > start + 3; i -= 3)
            digits.insert(i - 3, 1, ',');
        return digits;
    }

    std::string CellsText(const char16_t* cells, int capacity)
    {
        std::string out;
        const int len = TextUtf::CellLength(cells, capacity);
        if (len < 0 || !TextUtf::ToUtf8(cells, len, out))
            out = "?";   // ParseAdminMail 이 검사한 칸이라 나올 수 없다 — 로그 · 알림 한 줄만 망가진다
        return out;
    }

    std::string AdminItemsText(const MailAttach::Items& items)
    {
        std::string s;
        for (int i = 0; i < items.count; ++i)
        {
            if (i > 0)
                s += ", ";
            s += std::to_string(items.items[i].itemId) + " x " + std::to_string(items.items[i].count);
        }
        return s;
    }

    std::string AdminTargetText(int64_t target)
    {
        return target == 0 ? std::string("all") : std::to_string(target);
    }
}

bool CMailHandler::RunAdminSend(CPlayer* player, const std::vector<std::string>& words)
{
    const uint64_t now = CEnterGate::NowMs();
    const bool held = _adminBatch.active && now - _adminBatch.createdMs <= ADMIN_BATCH_HOLD_MS;

    // confirm — 같은 운영자의 대기만 보낸다(6절 「같은 운영자의 /accmail confirm만 그 번호로 요청을 보낸다」)
    if (words.size() == 2 && words[1] == "confirm")
    {
        if (!held || _adminBatch.operatorId != player->_characterId)
        {
            _selfNotifier.SendSystemMessage(player, "보낼 전 계정 지급이 없습니다 — /accmail all … 을 친 뒤 30초 안에 confirm");
            return false;
        }
        SendAdminMail(player, _adminBatch.form, _adminBatch.batchId);
        return true;
    }

    AdminMailForm form;
    const std::string reject = _mailSystem.ParseAdminMail(words, form);
    if (!reject.empty())
    {
        _selfNotifier.SendSystemMessage(player, reject);
        return false;
    }

    if (!form.all)
    {
        SendAdminMail(player, form, 0);   // 한 계정 — 번호가 없다. 답을 못 받으면 운영자가 account_mail 을 보고 다시 보낸다(6절)
        return true;
    }

    // 전 계정 — 번호를 만들어 들고 미리 보여 준다. 다시 치면 새 내용 · 새 번호
    int64_t batchId = 0;
    if (!CMailSystem::MakeBatchId(batchId))
    {
        SLOG_ERROR("[MAIL] accmail batch id not made (character={}) — 난수를 못 얻음", player->_characterId);
        _selfNotifier.SendSystemMessage(player, "일괄 번호를 만들지 못했습니다 — 다시 쳐 주십시오");
        return true;
    }
    _adminBatch.active = true;
    _adminBatch.operatorId = player->_characterId;
    _adminBatch.batchId = batchId;
    _adminBatch.createdMs = now;
    _adminBatch.form = form;
    SLOG_INFO("[MAIL] accmail batch held by={} batch={} money {} items=[{}] title=\"{}\"",
              player->_characterId, batchId, form.money, AdminItemsText(form.items), CellsText(form.title, MAIL_TITLE_MAX));
    _selfNotifier.SendSystemMessage(player, "전 계정 지급 대기 — 돈 " + WithCommas(form.money) + " · 아이템 " + std::to_string(form.items.count)
                                          + "칸 · 제목 " + CellsText(form.title, MAIL_TITLE_MAX));
    _selfNotifier.SendSystemMessage(player, "30초 안에 /accmail confirm");
    return true;
}

void CMailHandler::SendAdminMail(CPlayer* player, const AdminMailForm& form, int64_t batchId)
{
    LINK_ACCMAIL_SEND_REQ request{};
    request.targetAccount = form.all ? 0 : form.targetAccount;
    request.batchId = batchId;
    request.sentBy = ToInt(player->_characterId);
    request.money = form.money;
    static_assert(sizeof(request.title) == sizeof(form.title) && sizeof(request.body) == sizeof(form.body),
                  "서버 간 글자 칸 = 존 서버 우편 칸 — 칸 그대로 옮긴다");
    std::memcpy(request.title, form.title, sizeof(form.title));
    std::memcpy(request.body, form.body, sizeof(form.body));
    uint8_t attach[MailAttach::MAX_BYTES] = {};
    const int len = MailAttach::Write(form.items, attach);   // ParseAdminMail 이 칸 수를 0 ~ 8 로 묶었다 — -1 은 나올 수 없다
    static_assert(MailAttach::MAX_BYTES == LINK_MAIL_ATTACH_BYTES, "첨부 바이트 상한 = 서버 간 첨부 칸");
    request.attachLen = static_cast<uint8_t>(len < 0 ? 0 : len);
    std::memcpy(request.attach, attach, sizeof(attach));

    const SessionId sessionId = player->_sessionId;
    const CharacterId characterId = player->_characterId;
    const int64_t target = request.targetAccount;
    // 누가 · 무엇을 · 누구에게(6절 보내기 끝 줄) — 계정 서버 로그 · account_mail.sent_by · batch_id 에도 남는다
    SLOG_INFO("[MAIL] accmail send by={} target={} batch={} money {} items=[{}] title=\"{}\"",
              characterId, AdminTargetText(target), batchId, form.money, AdminItemsText(form.items), CellsText(form.title, MAIL_TITLE_MAX));
    const bool sent = _accountMailClient.Send(request,
        [this, sessionId, characterId, target, batchId](const LINK_ACCMAIL_SEND_ACK* ack) { OnAdminSendAck(sessionId, characterId, target, batchId, ack); });
    if (!sent)
    {
        SLOG_WARN("[MAIL] accmail not sent (by={}, target={}, batch={}) — 월드 서버 연결 없음", characterId, AdminTargetText(target), batchId);
        _selfNotifier.SendSystemMessage(player, "월드 서버와 연결이 없어 지급하지 못했습니다");
    }
}

void CMailHandler::OnAdminSendAck(SessionId sessionId, CharacterId characterId, int64_t target, int64_t batchId, const LINK_ACCMAIL_SEND_ACK* ack)
{
    // 로그는 운영자가 나갔어도 남긴다 — 무엇이 들어갔는지의 기록이다
    const bool answered = (ack != nullptr);
    const LinkMailResult result = answered ? static_cast<LinkMailResult>(ack->result) : LinkMailResult::OK;
    const int64_t rows = answered ? ack->rows : 0;
    if (!answered)
        SLOG_WARN("[MAIL] accmail no answer (by={}, target={}, batch={}) — 다시 보내지 않는다", characterId, AdminTargetText(target), batchId);
    else
        SLOG_INFO("[MAIL] accmail result {} rows={} (by={}, target={}, batch={})",
                  static_cast<int>(ack->result), rows, characterId, AdminTargetText(target), batchId);

    CPlayer* player = FindLinkOwner(_sessionHub, _gameMonitor, sessionId, characterId);
    if (player == nullptr)
        return;

    std::string line;
    if (!answered)
    {
        line = (target == 0)
             ? "결과를 못 받았습니다 — 30초 안에 /accmail confirm 을 다시 쳐도 같은 번호라 두 번 안 들어갑니다"
             : "결과를 못 받았습니다 — account_mail 을 확인한 뒤 다시 보내십시오";
    }
    else if (result == LinkMailResult::OK)
    {
        line = (target == 0)
             ? (rows > 0 ? "전 계정 지급했습니다 — " + WithCommas(rows) + "행" : std::string("이미 들어간 일괄입니다 — 0행"))
             : "계정 " + std::to_string(target) + " 에 지급했습니다";
    }
    else if (result == LinkMailResult::NO_ACCOUNT)
    {
        line = "없는 계정입니다: " + std::to_string(target);
    }
    else
    {
        line = std::string("지급하지 못했습니다 — ") + MailResultName(FromLink(ack->result));
    }
    _selfNotifier.SendSystemMessage(player, line);
}

#endif // USE_MAIL
