#include "CharLoginFlow.h"

#include <Common/ErrorLog.h>
#include <Db/DbPipeline.h>
#include <Link/LinkConnector.h>
#include <Link/LinkRequests.h>
#include <Lobby/LobbyLoop.h>
#include <Lobby/PacketSend.h>
#include <cstring>

#include "CharListSend.h"
#include "CharSessionTable.h"
#include "CharacterMetrics.h"

static_assert(CHAR_CODE_BYTES == LINK_CODE_BYTES, "토큰 칸 크기가 어긋났다");

CCharLoginFlow::CCharLoginFlow(CIOCPServer& network, CMonitorManager& monitor, CDbPipeline& db, CCharSessionTable& sessions,
                               CLinkConnector& account, CLinkRequests& requests, CCharacterMetrics& metrics, int graceDays)
    : _network(network), _monitor(monitor), _db(db), _sessions(sessions), _account(account), _requests(requests), _metrics(metrics), _graceDays(graceDays)
{
}

void CCharLoginFlow::OnLogin(SessionId sessionId, CSerialBuffer* msg)
{
    C2S_CHAR_LOGIN request;
    if (!PacketSend::Read(msg, request))
    {
        _monitor._packetErrors.Inc();
        return;
    }

    // 로그인을 두 번 냄 — 정상 웹은 답을 받기 전에 다시 내지 않는다
    if (!_sessions.BeginLogin(sessionId))
    {
        _monitor._packetErrors.Inc();
        _network.RequestDisconnectSession(ToInt(sessionId));
        return;
    }

    const uint64_t requestId = _requests.Begin(LinkRequestKind::TokenCheck, CLobbyLoop::NowMs(),
        [this, sessionId](const void* reply) { OnTokenReply(sessionId, static_cast<const LINK_TOKEN_CHECK_ACK*>(reply)); });

    LINK_TOKEN_CHECK_REQ check{};
    check.requestId = requestId;
    std::memcpy(check.token, request.authToken, sizeof(check.token));
    if (!_account.Send(check))
    {
        _requests.Cancel(requestId);
        Fail(sessionId, CharLoginResult::SERVER_DOWN);
    }
}

void CCharLoginFlow::OnTokenAck(CSerialBuffer* msg)
{
    LINK_TOKEN_CHECK_ACK ack;
    if (!PacketSend::Read(msg, ack))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    _requests.Complete(ack.requestId, CLobbyLoop::NowMs(), &ack);   // 시한이 지난 뒤 온 답은 짝이 없어 버려진다
}

void CCharLoginFlow::OnTokenReply(SessionId sessionId, const LINK_TOKEN_CHECK_ACK* ack)
{
    if (_sessions.Find(sessionId) == nullptr)
        return;   // 웹이 먼저 나갔다

    if (ack == nullptr)
    {
        Fail(sessionId, CharLoginResult::SERVER_DOWN);
        return;
    }

    switch (static_cast<TokenCheckResult>(ack->result))
    {
    case TokenCheckResult::Ok:
    {
        const AccountId accountId{ ack->accountId };
        const int graceDays = _graceDays;
        const bool queued = _db.Run<CCharacterDb::ListResult>(ToInt(accountId), CCharacterDb::OP_LIST,
            [accountId, graceDays](CDbConnection& conn) { return CCharacterDb::LoadList(conn, accountId, graceDays); },
            [this, sessionId, accountId](CCharacterDb::ListResult& list) { OnListLoaded(sessionId, accountId, list); });
        if (!queued)
            Fail(sessionId, CharLoginResult::BUSY);   // 토큰은 이미 지웠다 — 웹이 새로 받는다
        return;
    }
    case TokenCheckResult::BadSize:
    case TokenCheckResult::NotFound:
        Fail(sessionId, CharLoginResult::BAD_TOKEN);
        return;
    case TokenCheckResult::Expired:
        Fail(sessionId, CharLoginResult::EXPIRED);
        return;
    }

    _monitor._packetErrors.Inc();   // 모르는 값 — 와이어에서 온 값이라 switch 밖으로 나올 수 있다
    Fail(sessionId, CharLoginResult::BAD_TOKEN);
}

void CCharLoginFlow::OnListLoaded(SessionId sessionId, AccountId accountId, const CCharacterDb::ListResult& list)
{
    if (_sessions.Find(sessionId) == nullptr)
        return;

    if (!list.ok)
    {
        Fail(sessionId, CharLoginResult::DB_FAIL);
        return;
    }

    const SessionId previous = _sessions.CompleteLogin(sessionId, accountId);
    if (previous != NO_SESSION)
        _network.RequestDisconnectSession(ToInt(previous));   // 지연 닫기 없음 — 옛 창에 줄 회신이 없다

    S2C_CHAR_LOGIN reply{};
    reply.result = static_cast<uint8_t>(CharLoginResult::OK);
    PacketSend::Send(_network, _monitor, sessionId, reply);
    CharListSend::Send(_network, _monitor, sessionId, list.rows);
    _metrics.Count(CharLoginResult::OK);
    SLOG_INFO("[CharLogin] ok account={} rows={}", ToInt(accountId), list.rows.size());
}

void CCharLoginFlow::Fail(SessionId sessionId, CharLoginResult result)
{
    _sessions.FailLogin(sessionId);

    S2C_CHAR_LOGIN reply{};
    reply.result = static_cast<uint8_t>(result);
    PacketSend::Send(_network, _monitor, sessionId, reply);
    _metrics.Count(result);
}
