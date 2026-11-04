#include "AccountLoginFlow.h"

#include <Common/ErrorLog.h>
#include <Db/DbPipeline.h>
#include <Lobby/LobbyLoop.h>
#include <Lobby/PacketSend.h>
#include <cstring>
#include <functional>
#include <string>

#include "AccountMetrics.h"
#include "AccountRules.h"
#include "AuthTokenLedger.h"

static_assert(ACCOUNT_TOKEN_BYTES == CAuthTokenLedger::TOKEN_BYTES, "토큰 칸 크기가 어긋났다");

CAccountLoginFlow::CAccountLoginFlow(CIOCPServer& network, CMonitorManager& monitor, CDbPipeline& db,
                                     CAuthTokenLedger& ledger, CAccountMetrics& metrics, bool allowDevLogin)
    : _network(network), _monitor(monitor), _db(db), _ledger(ledger), _metrics(metrics), _allowDevLogin(allowDevLogin)
{
}

void CAccountLoginFlow::OnLogin(SessionId sessionId, CSerialBuffer* msg)
{
    C2S_ACCOUNT_LOGIN request;
    if (!PacketSend::Read(msg, request))
    {
        _monitor._packetErrors.Inc();
        return;
    }

    if (!_allowDevLogin)
    {
        Reply(sessionId, AccountLoginResult::LOGIN_DISABLED, nullptr);
        return;
    }
    if (!CAccountRules::IsValidLoginName(request.loginName))
    {
        Reply(sessionId, AccountLoginResult::BAD_FORM, nullptr);
        return;
    }

    const std::string loginName(request.loginName);
    const int64_t key = static_cast<int64_t>(std::hash<std::string>{}(CAccountRules::LowerName(loginName)));
    const bool queued = _db.Run<CAccountDb::LoginRow>(key, CAccountDb::OP_LOGIN,
        [loginName](CDbConnection& conn) { return CAccountDb::FindOrCreate(conn, loginName); },
        [this, sessionId, loginName](CAccountDb::LoginRow& row) { OnDbReply(sessionId, loginName, row); });
    if (!queued)
        Reply(sessionId, AccountLoginResult::BUSY, nullptr);
}

void CAccountLoginFlow::OnDbReply(SessionId sessionId, const std::string& loginName, const CAccountDb::LoginRow& row)
{
    if (!row.ok)
    {
        Reply(sessionId, AccountLoginResult::DB_FAIL, nullptr);
        return;
    }

    if (row.created)
        _metrics.CountCreated();

    char token[ACCOUNT_TOKEN_BYTES];
    _ledger.Issue(row.accountId, CLobbyLoop::NowMs(), token);
    _metrics.CountIssued();
    SLOG_INFO("[Login] ok name={} account={} created={}", loginName, ToInt(row.accountId), row.created);
    Reply(sessionId, AccountLoginResult::OK, token);
}

void CAccountLoginFlow::Reply(SessionId sessionId, AccountLoginResult result, const char* token)
{
    S2C_ACCOUNT_LOGIN packet{};
    packet.result = static_cast<uint8_t>(result);
    if (token != nullptr)
        std::memcpy(packet.token, token, sizeof(packet.token));
    PacketSend::Send(_network, _monitor, sessionId, packet);   // 웹이 먼저 나갔으면 false — 토큰은 2분 뒤 만료로 사라진다

    _metrics.CountLogin(result);
    _closing.push_back(Closing{ sessionId, CLobbyLoop::NowMs() + CLOSE_DELAY_MS });
}

void CAccountLoginFlow::Tick(int64_t nowMs)
{
    while (!_closing.empty() && nowMs >= _closing.front().dueMs)
    {
        _network.RequestDisconnectSession(ToInt(_closing.front().sessionId));   // 이미 나갔으면 false — 번호 재사용은 코어가 가른다
        _closing.pop_front();
    }
}
