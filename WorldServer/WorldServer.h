#pragma once
// ==========================================================================
// CWorldServer — 월드 서버: 조립과 수명, 패킷 분배만 맡는다
//
//   캐릭터 서버 = 들어가기 전의 일, 월드 서버 = 게임에 들어간 뒤의 일, 존 서버 = 한 대 안의 게임.
//   DB 를 쓰지 않는다. 상태는 로직 스레드(CLobbyLoop) 하나만 만진다 — 부품에 락이 없다.
//
//   포트: 서버 간(ServerLink.ini [World] 주소에서만, 존 서버가 접속) · 지표.
//   코어가 늘 여는 포트([Server] Port)에는 받을 손님이 없어 들어오면 바로 끊는다.
//   계정 서버로는 내가 접속한다(CLinkConnector) — 어느 쪽을 먼저 켜도 된다.
// ==========================================================================

#include "IOCPServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서는 이 헤더가 맡는다
#include "MonitorManager.h"

#include <Link/LinkConnector.h>
#include <Link/LinkPeers.h>
#include <Link/ServerLinkConfig.h>
#include <Lobby/LobbyLoop.h>
#include <Lobby/PacketTable.h>
#include <Monitoring/CoreMetrics.h>
#include <Monitoring/MetricsEndpoint.h>

#include "WorldMetrics.h"
#include "WorldPresence.h"
#include "WorldServerConfig.h"

class CWorldServer
{
public:
    CWorldServer(const WorldServerConfig& config, const ServerLinkConfig& link);

    CWorldServer(const CWorldServer&) = delete;
    CWorldServer& operator=(const CWorldServer&) = delete;

    bool Start();   // false = 처리표 · 포트 중 하나가 안 됨 — 까닭은 로그에
    void Stop();

private:
    bool RegisterRoutes();
    void Periodic(int64_t nowMs);
    std::string BuildMetricsText();

    void OnZoneRegister(SessionId sessionId, CSerialBuffer* msg);
    void OnOnlineList(SessionId sessionId, CSerialBuffer* msg);
    void OnCharEnter(SessionId sessionId, CSerialBuffer* msg);
    void OnCharLeave(SessionId sessionId, CSerialBuffer* msg);
    bool CheckRegistered(SessionId sessionId);
    void SendWorldRegister();

    WorldServerConfig _config;
    ServerLinkConfig  _link;

    CMonitorManager _monitor;
    CIOCPServer     _network;
    CLobbyLoop      _loop;
    CLinkConnector  _account;
    CLinkPeers      _peers;

    CWorldMetrics  _metrics;
    CWorldPresence _presence;

    CPacketTable _accountTable;
    CPacketTable _zoneTable;

    int64_t _nextSweepMs = 0;
    bool    _started = false;

    CCoreMetrics     _coreMetrics;
    CMetricsEndpoint _endpoint;   // 맨 뒤에 둔다 — 먼저 내려가야 위 멤버를 쓰는 핸들러가 안전하다
};
