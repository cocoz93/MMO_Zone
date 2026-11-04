#pragma once
// ==========================================================================
// CAccountLoginFlow — C2S_ACCOUNT_LOGIN → 이름 판정 → DB → 토큰 발급 → 회신 → 연결 끊기
//
//   접속 상태를 안 갖는다. 로직 스레드 전용(DB 회신도 로직 스레드에서 돈다).
// ==========================================================================

#include "IOCPServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서는 이 헤더가 맡는다

#include <cstdint>
#include <deque>

#include <Identity/GameIds.h>
#include <Protocol/AccountProtocol.h>

#include "AccountDb.h"

class CAccountMetrics;
class CAuthTokenLedger;
class CDbPipeline;
class CMonitorManager;
class CSerialBuffer;

class CAccountLoginFlow
{
public:
    // 회신을 낸 뒤 끊기까지 기다리는 시간. 코어의 끊기는 진행 중인 입출력을 취소하므로(CancelIoEx)
    //   회신 바로 뒤에 끊으면 회신이 못 갈 수 있다. 1초는 근거 없이 고른 값이다 — 웹은 답을 받으면 스스로 닫는다
    static constexpr int64_t CLOSE_DELAY_MS = 1000;

    CAccountLoginFlow(CIOCPServer& network, CMonitorManager& monitor, CDbPipeline& db,
                      CAuthTokenLedger& ledger, CAccountMetrics& metrics, bool allowDevLogin);

    void OnLogin(SessionId sessionId, CSerialBuffer* msg);
    void Tick(int64_t nowMs);   // 회신을 낸 세션 끊기

private:
    struct Closing
    {
        SessionId sessionId;
        int64_t   dueMs;
    };

    void OnDbReply(SessionId sessionId, const std::string& loginName, const CAccountDb::LoginRow& row);
    void Reply(SessionId sessionId, AccountLoginResult result, const char* token);

    CIOCPServer&      _network;
    CMonitorManager&  _monitor;
    CDbPipeline&      _db;
    CAuthTokenLedger& _ledger;
    CAccountMetrics&  _metrics;
    bool              _allowDevLogin;
    std::deque<Closing> _closing;   // 시간 순서대로 들어온다 — 앞만 본다
};
