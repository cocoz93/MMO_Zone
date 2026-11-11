#pragma once
// ==========================================================================
// CAccountServer — 계정 서버: 조립과 수명만 맡는다
//
//   코어 · 부품을 값으로 들고 처리표를 등록한다. 판정은 CAccountRules, 토큰은 CAuthTokenLedger,
//   SQL 은 CAccountDb, 흐름은 CAccountLoginFlow · CTokenCheckFlow 다.
//   상태는 로직 스레드(CLobbyLoop) 하나만 만진다 — 부품에 락이 없다.
//
//   포트: 웹(모든 주소) · 서버 간(ServerLink.ini [Account] 주소에서만) · 지표
// ==========================================================================

#include "IOCPServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서는 이 헤더가 맡는다
#include "MonitorManager.h"

#include <Db/DbPipeline.h>
#include <Link/LinkPeers.h>
#include <Link/ServerLinkConfig.h>
#include <Lobby/LobbyLoop.h>
#include <Lobby/PacketTable.h>
#include <Monitoring/CoreMetrics.h>
#include <Monitoring/MetricsEndpoint.h>

#include "AccountLoginFlow.h"
#include "AccountMetrics.h"
#include "AccountServerConfig.h"
#include "AuthTokenLedger.h"
#include "TokenCheckFlow.h"

class CAccountServer
{
public:
    CAccountServer(const AccountServerConfig& config, const ServerLinkConfig& link);

    CAccountServer(const CAccountServer&) = delete;
    CAccountServer& operator=(const CAccountServer&) = delete;

    bool Start();   // false = 처리표 · DB · 포트 중 하나가 안 됨 — 까닭은 로그에
    void Stop();

private:
    bool RegisterRoutes();
    void OnWorldRegister(SessionId sessionId, CSerialBuffer* msg);
    void Periodic(int64_t nowMs);
    std::string BuildMetricsText();

    AccountServerConfig _config;
    ServerLinkConfig    _link;

    CMonitorManager _monitor;
    CIOCPServer     _network;
    CDbPipeline     _db;
    CLobbyLoop      _loop;
    CLinkPeers      _peers;

    CAccountMetrics   _metrics;
    CAuthTokenLedger  _ledger;
    CAccountLoginFlow _loginFlow;
    CTokenCheckFlow   _tokenFlow;

    CPacketTable _webTable;
    CPacketTable _linkTable;

    SessionId _worldSession = NO_SESSION;   // 월드 서버 연결 — 신호는 이 연결로만 보낸다

    int64_t _nextSweepMs = 0;
    bool    _started = false;

    CCoreMetrics     _coreMetrics;
    CMetricsEndpoint _endpoint;   // 맨 뒤에 둔다 — 먼저 내려가야 위 멤버를 쓰는 핸들러가 안전하다
};
