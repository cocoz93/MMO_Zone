#include "AccountMailFlow.h"

#include "MonitorManager.h"

#include <cstring>
#include <string>

#include <Common/ErrorLog.h>
#include <Db/DbPipeline.h>
#include <Lobby/PacketSend.h>
#include <Protocol/ServerLinkProtocol.h>
#include <Text/TextUtf.h>

#include "AccountDb.h"
#include "AccountMetrics.h"

namespace
{
    // 와이어 글자 칸 → UTF-8. 칸 규칙(첫 0 뒤는 전부 0 · 대리쌍 짝 · 제어 문자)을 어기거나 minLength 보다 짧으면 false.
    //   패킷 칸은 줄이 안 맞아 참조로 못 넘긴다 — 부르는 쪽이 줄 맞은 칸에 복사해 넘긴다
    template <int N>
    bool WireText(const char16_t (&cells)[N], bool allowNewline, int minLength, std::string& out)
    {
        const int length = TextUtf::CellLength(cells, N);
        if (length < minLength)   // -1(끝 표시 뒤에 글자)도 여기서 걸린다
            return false;
        if (!TextUtf::PairsOk(cells, length) || TextUtf::HasControl(cells, length, allowNewline))
            return false;
        return TextUtf::ToUtf8(cells, length, out);
    }

    // 결과만 담는 답 — 거절(BUSY) · 실패에 쓴다
    template <typename TAck>
    TAck ResultAck(uint64_t requestId, LinkMailResult result)
    {
        TAck ack{};
        ack.requestId = requestId;
        ack.result = static_cast<uint8_t>(result);
        return ack;
    }

    LinkMailResult FoundResult(CAccountDb::Found found)
    {
        switch (found)
        {
        case CAccountDb::Found::Yes:    return LinkMailResult::OK;
        case CAccountDb::Found::No:     return LinkMailResult::GONE;
        case CAccountDb::Found::Failed: return LinkMailResult::DB_ERROR;
        }
        return LinkMailResult::DB_ERROR;
    }
}

template <typename TPacket>
bool CAccountMailFlow::Accept(SessionId peer, CSerialBuffer* msg, TPacket& out, const char* what)
{
    if (!PacketSend::Read(msg, out))
    {
        _monitor._packetErrors.Inc();
        return false;
    }
    // 계정 서버의 서버 간 포트에는 캐릭터 서버도 붙는다 — 계정 우편은 월드 서버만 묻는다
    if (peer != _worldSession)
    {
        _monitor._packetErrors.Inc();
        SLOG_WARN("[AccountMail] {} 요청이 월드 서버가 아닌 연결에서 옴 — 버림 (session={} · 월드 서버 session={})",
                  what, ToInt(peer), ToInt(_worldSession));
        return false;
    }
    return true;
}

void CAccountMailFlow::Malformed(SessionId peer, const char* what, int64_t account, int64_t mailId)
{
    _monitor._packetErrors.Inc();
    SLOG_ERROR("[AccountMail] {} 요청 형식이 틀림 — 존 서버가 거르는 값이다. 답 없이 버림 (session={} · 계정 {} · 우편 {})",
               what, ToInt(peer), account, mailId);
}

void CAccountMailFlow::OnCount(SessionId peer, CSerialBuffer* msg)
{
    LINK_ACCMAIL_COUNT_REQ request;
    if (!Accept(peer, msg, request, "수 세기"))
        return;
    const uint64_t requestId = request.requestId;
    const int64_t account = request.accountId;
    const int64_t character = request.characterId;
    if (account <= 0 || character <= 0)
        return Malformed(peer, "수 세기", account, 0);

    const bool queued = _db.Run<CAccountDb::MailCount>(account, CAccountDb::OP_MAIL_COUNT,
        [account, character](CDbConnection& conn) { return CAccountDb::CountMail(conn, account, character); },
        [this, peer, requestId](CAccountDb::MailCount& row)
        {
            const LinkMailResult result = row.ok ? LinkMailResult::OK : LinkMailResult::DB_ERROR;
            LINK_ACCMAIL_COUNT_ACK ack = ResultAck<LINK_ACCMAIL_COUNT_ACK>(requestId, result);
            ack.count = row.count;
            ack.claimMailId = row.claimMailId;
            _metrics.CountMail(AccMailOp::Count, result);
            _peers.Send(peer, ack);
        });
    if (!queued)
    {
        _metrics.CountMail(AccMailOp::Count, LinkMailResult::BUSY);
        _peers.Send(peer, ResultAck<LINK_ACCMAIL_COUNT_ACK>(requestId, LinkMailResult::BUSY));
    }
}

void CAccountMailFlow::OnList(SessionId peer, CSerialBuffer* msg)
{
    LINK_ACCMAIL_LIST_REQ request;
    if (!Accept(peer, msg, request, "목록"))
        return;
    const uint64_t requestId = request.requestId;
    const int64_t account = request.accountId;
    const int64_t before = request.beforeMailId;
    if (account <= 0 || before < 0)
        return Malformed(peer, "목록", account, before);

    const bool queued = _db.Run<CAccountDb::MailList>(account, CAccountDb::OP_MAIL_LIST,
        [account, before](CDbConnection& conn) { return CAccountDb::ListMail(conn, account, before); },
        [this, peer, requestId](CAccountDb::MailList& page)
        {
            const LinkMailResult result = page.ok ? LinkMailResult::OK : LinkMailResult::DB_ERROR;
            LINK_ACCMAIL_LIST_ACK ack = ResultAck<LINK_ACCMAIL_LIST_ACK>(requestId, result);
            if (page.ok)
            {
                ack.more = page.more ? 1 : 0;
                ack.count = static_cast<uint8_t>(page.count);
                std::memcpy(ack.lines, page.lines, sizeof(LinkAccMailLine) * static_cast<size_t>(page.count));
            }
            _metrics.CountMail(AccMailOp::List, result);
            _peers.Send(peer, ack);
        });
    if (!queued)
    {
        _metrics.CountMail(AccMailOp::List, LinkMailResult::BUSY);
        _peers.Send(peer, ResultAck<LINK_ACCMAIL_LIST_ACK>(requestId, LinkMailResult::BUSY));
    }
}

void CAccountMailFlow::OnRead(SessionId peer, CSerialBuffer* msg)
{
    LINK_ACCMAIL_READ_REQ request;
    if (!Accept(peer, msg, request, "읽기"))
        return;
    const uint64_t requestId = request.requestId;
    const int64_t account = request.accountId;
    const int64_t mailId = request.mailId;
    if (account <= 0 || mailId <= 0)
        return Malformed(peer, "읽기", account, mailId);

    const bool queued = _db.Run<CAccountDb::MailOne>(account, CAccountDb::OP_MAIL_READ,
        [account, mailId](CDbConnection& conn) { return CAccountDb::ReadMail(conn, account, mailId); },
        [this, peer, requestId](CAccountDb::MailOne& one)
        {
            const LinkMailResult result = FoundResult(one.found);
            LINK_ACCMAIL_READ_ACK ack = ResultAck<LINK_ACCMAIL_READ_ACK>(requestId, result);
            if (result == LinkMailResult::OK)
                ack.mail = one.mail;
            _metrics.CountMail(AccMailOp::Read, result);
            _peers.Send(peer, ack);
        });
    if (!queued)
    {
        _metrics.CountMail(AccMailOp::Read, LinkMailResult::BUSY);
        _peers.Send(peer, ResultAck<LINK_ACCMAIL_READ_ACK>(requestId, LinkMailResult::BUSY));
    }
}

void CAccountMailFlow::OnClaim(SessionId peer, CSerialBuffer* msg)
{
    LINK_ACCMAIL_CLAIM_REQ request;
    if (!Accept(peer, msg, request, "받는 중 표시"))
        return;
    const uint64_t requestId = request.requestId;
    const int64_t account = request.accountId;
    const int64_t mailId = request.mailId;
    const int64_t character = request.characterId;
    if (account <= 0 || mailId <= 0 || character <= 0)
        return Malformed(peer, "받는 중 표시", account, mailId);

    const bool queued = _db.Run<CAccountDb::MailOne>(account, CAccountDb::OP_MAIL_CLAIM,
        [account, mailId, character](CDbConnection& conn) { return CAccountDb::ClaimMail(conn, account, mailId, character); },
        [this, peer, requestId, account, mailId, character](CAccountDb::MailOne& one)
        {
            // 다시 읽은 행이 받는 중이면 OK — 적힌 캐릭터가 요청한 캐릭터인지는 존 서버가 본다(남이면 OTHER_CHARACTER · 6절 받기 2번)
            //   지금 문장으로는 「안 받음」이 다시 읽힐 길이 없다 — 표시가 0행인 안 받음은 기한이 지난 것뿐이고 SelectOne 은 살아 있는 행만 고른다.
            //   아래 상태 검사는 그 두 문장이 바뀔 때를 위한 문이다(이것을 빼는 돌연변이가 a0 에서 안 걸리는 까닭)
            LinkMailResult result = FoundResult(one.found);
            if (result == LinkMailResult::OK && one.mail.state != static_cast<uint8_t>(AccMailState::CLAIMING))
                result = LinkMailResult::GONE;
            LINK_ACCMAIL_CLAIM_ACK ack = ResultAck<LINK_ACCMAIL_CLAIM_ACK>(requestId, result);
            if (result == LinkMailResult::OK)
            {
                ack.mail = one.mail;
                SLOG_INFO("[AccountMail] 받는 중 — 계정 {} 우편 {} · 요청 캐릭터 {} · 적힌 캐릭터 {}",
                          account, mailId, character, static_cast<int64_t>(one.mail.claimCharacterId));
            }
            _metrics.CountMail(AccMailOp::Claim, result);
            _peers.Send(peer, ack);
        });
    if (!queued)
    {
        _metrics.CountMail(AccMailOp::Claim, LinkMailResult::BUSY);
        _peers.Send(peer, ResultAck<LINK_ACCMAIL_CLAIM_ACK>(requestId, LinkMailResult::BUSY));
    }
}

void CAccountMailFlow::OnDone(SessionId peer, CSerialBuffer* msg)
{
    LINK_ACCMAIL_DONE request;
    if (!Accept(peer, msg, request, "받음"))
        return;
    const int64_t account = request.accountId;
    const int64_t mailId = request.mailId;
    const int64_t character = request.characterId;
    if (account <= 0 || mailId <= 0 || character <= 0)
        return Malformed(peer, "받음", account, mailId);

    // 답이 없다 — 0행 · 실패 · 거절이면 행이 받는 중으로 남고 그 캐릭터의 입장 때 다시 처리가 「이미 옮겼나」로 다시 보낸다(6절)
    const bool queued = _db.Run<CAccountDb::MailWrite>(account, CAccountDb::OP_MAIL_DONE,
        [account, mailId, character](CDbConnection& conn) { return CAccountDb::DoneMail(conn, account, mailId, character); },
        [this, account, mailId, character](CAccountDb::MailWrite& write)
        {
            const LinkMailResult result = !write.ok ? LinkMailResult::DB_ERROR : write.rows > 0 ? LinkMailResult::OK : LinkMailResult::GONE;
            _metrics.CountMail(AccMailOp::Done, result);
            if (result == LinkMailResult::OK)
                SLOG_INFO("[AccountMail] 받음 — 계정 {} 우편 {} · 캐릭터 {}", account, mailId, character);
            else if (result == LinkMailResult::GONE)
                SLOG_WARN("[AccountMail] 받음 0행 — 계정 {} 우편 {} 이 캐릭터 {} 의 받는 중이 아님(이미 받음이거나 다른 캐릭터)", account, mailId, character);
            else
                SLOG_WARN("[AccountMail] 받음 실패 — 계정 {} 우편 {} · 캐릭터 {}. 받는 중으로 남아 입장 때 이어진다", account, mailId, character);
        });
    if (!queued)
    {
        _metrics.CountMail(AccMailOp::Done, LinkMailResult::BUSY);
        SLOG_WARN("[AccountMail] 받음을 DB 큐에 못 넣음 — 계정 {} 우편 {} · 캐릭터 {}. 받는 중으로 남아 입장 때 이어진다", account, mailId, character);
    }
}

void CAccountMailFlow::OnUnclaim(SessionId peer, CSerialBuffer* msg)
{
    LINK_ACCMAIL_UNCLAIM request;
    if (!Accept(peer, msg, request, "받는 중 풀기"))
        return;
    const int64_t account = request.accountId;
    const int64_t mailId = request.mailId;
    const int64_t character = request.characterId;
    if (account <= 0 || mailId <= 0 || character <= 0)
        return Malformed(peer, "받는 중 풀기", account, mailId);

    // 답이 없다 — 못 풀면 받는 중으로 남고 그 캐릭터의 입장 때 다시 처리가 다시 판정한다(6절 「어디서 죽어도 안전한 까닭」)
    const bool queued = _db.Run<CAccountDb::MailWrite>(account, CAccountDb::OP_MAIL_UNCLAIM,
        [account, mailId, character](CDbConnection& conn) { return CAccountDb::UnclaimMail(conn, account, mailId, character); },
        [this, account, mailId, character](CAccountDb::MailWrite& write)
        {
            const LinkMailResult result = !write.ok ? LinkMailResult::DB_ERROR : write.rows > 0 ? LinkMailResult::OK : LinkMailResult::GONE;
            _metrics.CountMail(AccMailOp::Unclaim, result);
            if (result == LinkMailResult::OK)
                SLOG_INFO("[AccountMail] 받는 중 풀림 — 계정 {} 우편 {} · 캐릭터 {}", account, mailId, character);
            else if (result == LinkMailResult::GONE)
                SLOG_WARN("[AccountMail] 풀기 0행 — 계정 {} 우편 {} 이 캐릭터 {} 의 받는 중이 아님", account, mailId, character);
            else
                SLOG_WARN("[AccountMail] 풀기 실패 — 계정 {} 우편 {} · 캐릭터 {}. 받는 중으로 남아 입장 때 이어진다", account, mailId, character);
        });
    if (!queued)
    {
        _metrics.CountMail(AccMailOp::Unclaim, LinkMailResult::BUSY);
        SLOG_WARN("[AccountMail] 풀기를 DB 큐에 못 넣음 — 계정 {} 우편 {} · 캐릭터 {}. 받는 중으로 남아 입장 때 이어진다", account, mailId, character);
    }
}

void CAccountMailFlow::OnErase(SessionId peer, CSerialBuffer* msg)
{
    LINK_ACCMAIL_ERASE_REQ request;
    if (!Accept(peer, msg, request, "지우기"))
        return;
    const uint64_t requestId = request.requestId;
    const int64_t account = request.accountId;
    const int64_t mailId = request.mailId;
    if (account <= 0 || mailId <= 0)
        return Malformed(peer, "지우기", account, mailId);

    const bool queued = _db.Run<CAccountDb::MailWrite>(account, CAccountDb::OP_MAIL_ERASE,
        [account, mailId](CDbConnection& conn) { return CAccountDb::EraseMail(conn, account, mailId); },
        [this, peer, requestId](CAccountDb::MailWrite& write)
        {
            // 0행 = 첨부 · 돈이 남았거나 받는 중이거나 이미 없다 — 셋을 가르지 않는다(6절 「지우기 — 계정 우편」)
            const LinkMailResult result = !write.ok ? LinkMailResult::DB_ERROR : write.rows > 0 ? LinkMailResult::OK : LinkMailResult::CANNOT_DELETE;
            _metrics.CountMail(AccMailOp::Erase, result);
            _peers.Send(peer, ResultAck<LINK_ACCMAIL_ERASE_ACK>(requestId, result));
        });
    if (!queued)
    {
        _metrics.CountMail(AccMailOp::Erase, LinkMailResult::BUSY);
        _peers.Send(peer, ResultAck<LINK_ACCMAIL_ERASE_ACK>(requestId, LinkMailResult::BUSY));
    }
}

void CAccountMailFlow::OnSend(SessionId peer, CSerialBuffer* msg)
{
    LINK_ACCMAIL_SEND_REQ request;
    if (!Accept(peer, msg, request, "지급"))
        return;
    const uint64_t requestId = request.requestId;

    CAccountDb::SendRequest send;
    send.targetAccount = request.targetAccount;
    send.batchId = request.batchId;
    send.sentBy = request.sentBy;
    send.money = request.money;
    send.expireDays = _expireDays;

    // 존 서버가 아이템 · 개수 · 돈 상한까지 본 뒤 보낸다(6절 보내기). 여기는 DB 에 넣을 수 있는 꼴인지만 본다
    char16_t title[LINK_MAIL_TITLE_CHARS];
    char16_t body[LINK_MAIL_BODY_CHARS];
    std::memcpy(title, request.title, sizeof(title));
    std::memcpy(body, request.body, sizeof(body));
    const bool formOk =
        send.targetAccount >= 0 && send.batchId >= 0 && send.sentBy > 0 && send.money >= 0 &&
        (send.targetAccount != 0 || send.batchId != 0) &&   // 전 계정은 일괄 번호가 있어야 두 번 안 들어간다
        request.attachLen <= LINK_MAIL_ATTACH_BYTES &&
        WireText(title, false, 1, send.title) &&            // 제목 1 ~ 20자 · 줄바꿈 안 됨
        WireText(body, true, 0, send.body);                 // 본문 0 ~ 200자 · 줄바꿈 됨
    if (!formOk)
        return Malformed(peer, "지급", send.targetAccount, 0);
    send.attach.assign(reinterpret_cast<const char*>(request.attach), request.attachLen);

    const bool all = (send.targetAccount == 0);
    const int64_t key = send.targetAccount;   // 전 계정은 열쇠 0 — 워커 하나를 잠깐 잡는다(15절 관측대기)
    const CAccountDb::Op op = all ? CAccountDb::OP_MAIL_SEND_ALL : CAccountDb::OP_MAIL_SEND_ONE;
    const bool queued = _db.Run<CAccountDb::MailWrite>(key, op,
        [send](CDbConnection& conn) { return CAccountDb::SendMail(conn, send); },
        [this, peer, requestId, send, all](CAccountDb::MailWrite& write)
        {
            LinkMailResult result = LinkMailResult::OK;
            if (!write.ok)
                result = LinkMailResult::DB_ERROR;
            else if (!all && write.rows == 0)
                result = LinkMailResult::NO_ACCOUNT;
            LINK_ACCMAIL_SEND_ACK ack = ResultAck<LINK_ACCMAIL_SEND_ACK>(requestId, result);
            ack.rows = write.ok ? write.rows : 0;
            _metrics.CountMail(AccMailOp::Send, result);
            if (write.ok)
                _metrics.AddMailSentRows(write.rows);
            // 누가 · 무엇을 · 누구에게(6절 보내기 끝 줄) — 존 서버 로그와 account_mail.sent_by · batch_id 에도 남는다
            SLOG_INFO("[AccountMail] 지급 {} — 운영자 캐릭터 {} → {} · 일괄 {} · 돈 {} · 첨부 {}B · 제목 「{}」 · {}행",
                      CAccountRules::MailResultName(result), send.sentBy,
                      all ? std::string("전 계정") : "계정 " + std::to_string(send.targetAccount),
                      send.batchId, send.money, send.attach.size(), send.title, static_cast<int64_t>(ack.rows));
            _peers.Send(peer, ack);
            // 도착 신호 — 행을 넣었을 때만(같은 일괄 번호를 다시 보내 0행이면 새 우편이 없다). 답이 없다(world-server-plan.md 5.6).
            //   지금 등록된 월드 서버로 보낸다 — 요청 뒤 월드 서버가 다시 붙었으면 peer 는 옛 연결이다
            if (write.ok && write.rows > 0)
            {
                LINK_ACCMAIL_ARRIVED signal{};
                signal.accountId = send.targetAccount;   // 0 = 전 계정 — 접속 중인 전원
                _metrics.CountMailSignal(_worldSession != NO_SESSION && _peers.Send(_worldSession, signal));
            }
        });
    if (!queued)
    {
        _metrics.CountMail(AccMailOp::Send, LinkMailResult::BUSY);
        SLOG_WARN("[AccountMail] 지급을 DB 큐에 못 넣음 — 운영자 캐릭터 {} → 계정 {}(0 = 전 계정) · BUSY 로 답함", send.sentBy, send.targetAccount);
        _peers.Send(peer, ResultAck<LINK_ACCMAIL_SEND_ACK>(requestId, LinkMailResult::BUSY));
    }
}
