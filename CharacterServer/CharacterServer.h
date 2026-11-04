#pragma once
// ==========================================================================
// CCharacterServer — 캐릭터 서버: 조립과 수명만 맡는다
//
//   코어 · 부품을 값으로 들고 처리표를 등록한다. 판정은 CCharacterRules, SQL 은 CCharacterDb,
//   흐름은 CCharLoginFlow · CRosterFlow · CSelectFlow · CZoneLinkFlow 다.
//   상태는 로직 스레드(CLobbyLoop) 하나만 만진다 — 부품에 락이 없다.
//
//   포트: 웹(모든 주소) · 서버 간(ServerLink.ini [Character] 주소에서만, 존 서버가 접속) · 지표.
//   계정 서버로는 내가 접속한다(CLinkConnector) — 어느 쪽을 먼저 켜도 된다.
// ==========================================================================

#include "IOCPServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서는 이 헤더가 맡는다
#include "MonitorManager.h"

#include <Data/JobTable.h>
#include <Db/DbPipeline.h>
#include <Link/LinkConnector.h>
#include <Link/LinkPeers.h>
#include <Link/LinkRequests.h>
#include <Link/ServerLinkConfig.h>
#include <Lobby/LobbyLoop.h>
#include <Lobby/PacketTable.h>
#include <Monitoring/CoreMetrics.h>
#include <Monitoring/MetricsEndpoint.h>

#include "CharLoginFlow.h"
#include "CharSessionTable.h"
#include "CharacterMetrics.h"
#include "CharacterServerConfig.h"
#include "LookTable.h"
#include "RosterFlow.h"
#include "SelectFlow.h"
#include "ZoneDirectory.h"
#include "ZoneLinkFlow.h"

class CCharacterServer
{
public:
    CCharacterServer(const CharacterServerConfig& config, const ServerLinkConfig& link);

    CCharacterServer(const CCharacterServer&) = delete;
    CCharacterServer& operator=(const CCharacterServer&) = delete;

    bool Start();   // false = 표 파일 · 처리표 · DB · 포트 중 하나가 안 됨 — 까닭은 로그에
    void Stop();

private:
    bool LoadTables();
    bool RegisterRoutes();
    CPacketTable::Handler LoggedInOnly(CPacketTable::Handler handler);
    void Periodic(int64_t nowMs);
    std::string BuildMetricsText();

    CharacterServerConfig _config;
    ServerLinkConfig      _link;

    CMonitorManager _monitor;
    CIOCPServer     _network;
    CDbPipeline     _db;
    CLobbyLoop      _loop;
    CLinkConnector  _account;
    CLinkPeers      _peers;
    CLinkRequests   _requests;

    CJobTable  _jobs;
    CLookTable _looks;

    CCharacterMetrics _metrics;
    CCharSessionTable _sessions;
    CZoneDirectory    _zones;
    CCharLoginFlow    _loginFlow;
    CRosterFlow       _rosterFlow;
    CSelectFlow       _selectFlow;
    CZoneLinkFlow     _zoneFlow;

    CPacketTable _webTable;
    CPacketTable _accountTable;
    CPacketTable _zoneTable;

    int64_t _nextSweepMs = 0;
    bool    _started = false;

    CCoreMetrics     _coreMetrics;
    CMetricsEndpoint _endpoint;   // 맨 뒤에 둔다 — 먼저 내려가야 위 멤버를 쓰는 핸들러가 안전하다
};
