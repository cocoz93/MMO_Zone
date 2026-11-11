#include "MailHandler.h"

#if USE_MAIL

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include "MailSystem.h"
#include "MailDraft.h"
#include "SessionHub.h"
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

    CSerialBuffer* MakeMailResult(MailAction action, MailResult result, int64_t mailId)
    {
        const char16_t noName[CHAR_NAME_MAX_LEN] = {};   // OTHER_CHARACTER(계정 우편 · 8단계)에만 이름이 실린다
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_MAIL_RESULT::TYPE);
        *buf << static_cast<uint8_t>(action);
        *buf << static_cast<uint8_t>(result);
        *buf << static_cast<int64_t>(mailId);
        WriteNameCells(buf, noName);
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
        const MailResult door = _mailSystem.CheckReadDoor(player);
        SendResult(player, MailAction::LIST, door != MailResult::OK ? door : MailResult::ACC_OFFLINE, 0);   // 계정 우편 — 8단계
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
        const MailResult door = _mailSystem.CheckReadDoor(player);
        SendResult(player, MailAction::READ, door != MailResult::OK ? door : MailResult::ACC_OFFLINE, mailId);
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
    // 계정 우편 받기 — 8단계. 그때 열 문(가방이 바뀌는 동작이라 CheckItemDoor)을 먼저 보고 연결이 없다고 답한다
    const MailResult door = _mailSystem.CheckItemDoor(player);
    SendResult(player, MailAction::ACC_TAKE, door != MailResult::OK ? door : MailResult::ACC_OFFLINE, recvMsg.mailId);
}

void CMailHandler::RecvDelete(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_MAIL_DELETE recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    const int64_t mailId = recvMsg.mailId;
    if (recvMsg.tab == static_cast<uint8_t>(MailTab::ACCOUNT))
    {
        const MailResult door = _mailSystem.CheckReadDoor(player);
        SendResult(player, MailAction::ACC_ERASE, door != MailResult::OK ? door : MailResult::ACC_OFFLINE, mailId);
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

    // 계정 우편 탭 — 3단계엔 읽을 길이 없다(8단계에 월드 서버로 묻는다)
    SendCount(player, MailTab::ACCOUNT, MailCountKind::UNKNOWN, 0);

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
    case DBMailOp::FIND_MOVED:                                  // 계정 우편(8단계) — Waiting 이 거짓이라 여기 안 온다
    case DBMailOp::FIND_NAME:
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

void CMailHandler::OnMailSaved(CPlayer* player, const DBMailStmt& stmt)
{
    if (player == nullptr)
        return;   // 떠난 기록으로 확정 — 보내기 · 받기는 알릴 사람이 없다(계정 우편 받기의 DONE 은 8단계에 여기서 보낸다)
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
    case DBMailStmtKind::ACC_TAKE_INSERT:                                                                    // 8단계 — Confirm 이 거짓이라 여기 안 온다
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
void CMailHandler::SendResult(CPlayer* player, MailAction action, MailResult result, int64_t mailId)
{
    if (result != MailResult::OK)
        _gameMonitor._mail._rejected[static_cast<int>(result)].Inc();
    _sessionHub.SendPacket(player, MakeMailResult(action, result, mailId));
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

#endif // USE_MAIL
