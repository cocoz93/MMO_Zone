//
#include <iostream>
#include <memory>
#include <atomic>
#include <cstdint>
#include <thread>
#include <chrono>
#include <vector>

#include "GameServer.h"
#include "NetIoModel.h"   // kNetIoModelName — 활성 네트워크 I/O 모델 이름(부팅 로그용)
#include "MapManager.h"
#include "MonitorManager.h"
#include "GameMonitor.h"   // 게임 컨텐츠 지표 — 코어(CMonitorManager)와 별개 객체
#include "MonitorServer.h"
#include "ServerConfig.h"
#include "CoreAffinity.h"       // 게임스레드 코어 격리 마스크 주입
#include "Platform/Platform.h"   // affinity·종료 시그널 격리
#include <Process/ShutdownWait.h>
#include <Link/ServerLinkConfig.h>
#include <Common/Logger.h>
#include <Common/ErrorLog.h>

// 정리 기다림이 상한(CShutdownWait::WAIT_MAX_MS)에 닿았을 때 — 창 닫기·로그오프·윈도 종료라 반환하면 곧 OS 가 죽인다.
//   확정 못 받은 종료 저장(떠난 기록 수)을 로그로 남긴다 — 저장 플랜 C 종료 시험 S4(10-21): DB 가 4.5초 넘게 막히면 저장이 로그 한 줄 없이 사라졌다.
//   로거가 비동기라 직접 내보내고, 실제로 쓸 틈을 조금 준다(OS 유예 약 5초 − 4.5초 안). 콘솔 핸들러 스레드에서 불린다 — 게이지(원자 값)만 읽는다
CGameMonitor* g_gameMonitorForShutdown = nullptr;
void OnShutdownWaitTimeout()
{
    const int64_t pendingLeave = (g_gameMonitorForShutdown != nullptr) ? g_gameMonitorForShutdown->_save._pendingLeave.Load() : -1;
    SLOG_ERROR("[Shutdown] 정리 기다림 {}ms 를 넘김 — OS 가 곧 끈다. 확정 못 받은 종료 저장(떠난 기록) {}개 — 그 캐릭터들의 마지막 저장 뒤 변경은 잃을 수 있다",
               CShutdownWait::WAIT_MAX_MS, pendingLeave);
    if (auto& logger = shared::Logger::Get())
        logger->flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

int main()
{
    // 로거 초기화 (RAII — 소멸자에서 Shutdown)
    shared::LoggerGuard loggerGuard;

    // INI 설정 파일 로드
    ServerConfig config;
    config.Load();

    // CPU 코어 핀(affinity) — Init/Start 전에 걸어야 이후 생성되는
    // worker/send/accept/gameloop 스레드가 전부 이 마스크를 상속한다.
    if (config.affinityMask != 0)
    {
        if (Platform::SetProcessAffinity(config.affinityMask))
            SLOG_INFO("[Affinity] ProcessAffinityMask = 0x{:X}", config.affinityMask);
        else
            SLOG_WARN("[Affinity] SetProcessAffinity failed");
    }

    // 게임스레드 코어 격리 마스크 주입 — 프로세스 affinity 직후, 스레드 생성(Init/Start/monitor) 전에 1회.
    //   config가 GameCore INI에서 도출한다(격리 off면 0,0). 이후 각 스레드가 진입부에서 자가-핀.
    CoreAffinity::SetIsolationMasks(config.gameCoreMask, config.ioCoreMask);
    if (config.gameCoreMask != 0)
        SLOG_INFO("[Affinity] CoreIsolation ON — game=0x{:X} io=0x{:X}",
                  config.gameCoreMask, config.ioCoreMask);

    SLOG_INFO("=== IOCP MMO Server ===");
    SLOG_INFO("Network I/O model: {}", kNetIoModelName);   // 이 빌드가 실제로 도는 모델 (IOCP/epoll/...)
    SLOG_INFO("Port: {}", config.port);
    SLOG_INFO("Max Clients: {}", config.maxClients);

    CMonitorManager monitor;       // 코어 지표 (네트워크·세션·워커)
    CGameMonitor    gameMonitor;   // 게임 컨텐츠 지표 (전투) — 코어와 독립
    std::unique_ptr<CMonitorServer> monitorSvr;
    if (config.monitorEnabled)
        monitorSvr = std::make_unique<CMonitorServer>(monitor, gameMonitor, config.monitorPort);

    CGameServer server(monitor, gameMonitor);

    // 기획 데이터 파일 (맵·레벨·아이템·몬스터·스폰 구역·드랍 표) — 세계(맵·존)를 만드는 Init "앞"에, 한 번에 전부 읽는다.
    //   Init 의 맵 등록이 기본 채널(존)을 바로 만드므로, 존이 생기는 순간 표가 서 있어야 한다(10-08).
    //   DB 접속보다도 앞이라 커넥션을 열기 전에 파일 문제로 끝낼 수 있다.
    //   밸런스 값 없이 도는 서버는 조용히 틀린 게임이 되므로 기본값으로 때우지 않는다.
    //   맵 표는 모든 빌드가 읽는다(MapTable.h) — 에코 시험 모드도 같다.
    if (!server.LoadGameDataFiles(Platform::GetExecutableDir()))
    {
        SLOG_ERROR("[Error] 기획 데이터 파일을 못 읽었습니다 - 위의 [표 이름] 으로 시작하는 줄을 보세요(표마다 이름표가 붙는다 — 표가 늘어도 이 문장은 그대로)");
        return 1;
    }

    // 맵 설정 = 맵 표의 줄(번호·크기 — 기획 데이터) + INI 의 운영 값(섹터 크기·채널 정원). 둘을 엮는 곳은 여기 한 곳이다.
    //   순서는 맵 표 그대로 — 첫 줄이 기본 접속 맵이다(CGameServer::Init).
    std::vector<MapConfig> maps;
    for (const MapDef& def : server.GameData().Maps().All())
    {
        const MapOps ops = config.MapOpsOf(def.mapId);
        maps.push_back({ def.mapId, def.width, def.height, ops.sectorSize, ops.maxPlayersPerChannel, def.start, def.kind });
        SLOG_INFO("[Map] {} — {}×{} · sector {} · channel cap {}{}", def.mapId, def.width, def.height, ops.sectorSize, ops.maxPlayersPerChannel,
                  def.kind == MapKind::INSTANCE ? " · room map (no channel 0)" : "");
    }

    // 서버 간 연결 — 게임 서버 모드에서 UseServerLink=1 일 때만. 에코 모드는 게임 스레드가 없어 사건을 받을 곳이 없다.
    //   ServerLink.ini 가 틀리면 기동을 거절한다 — 조용히 꺼진 채 뜨면 「고르기가 ZONE_DOWN」으로만 보인다(ServerLinkConfig.h).
    //   세션 표는 정원 + 여유 칸. 끄면 정원 그대로라 세션 표 · 스레드 · 지표 줄이 켜기 전과 같다.
    const bool useServerLink = config.useServerLink && config.mode == ServerMode::GameServer;
    ServerLinkConfig linkConfig;
    int sessionSlots = config.maxClients;
    if (useServerLink)
    {
        std::string linkError;
        if (!linkConfig.Load(Platform::GetExecutableDir() + "ServerLink.ini", linkError))
        {
            SLOG_ERROR("[Error] ServerLink.ini: {}", linkError);
            return 1;
        }
        sessionSlots = config.maxClients + linkConfig.sessionReserve;
        if (sessionSlots > ServerConfig::MAX_CLIENTS_LIMIT)
        {
            SLOG_ERROR("[Error] MaxClients({}) + SessionReserve({}) exceeds {}",
                       config.maxClients, linkConfig.sessionReserve, ServerConfig::MAX_CLIENTS_LIMIT);
            return 1;
        }
    }

    if (!server.Init(config.mode, config.port, sessionSlots,
                     maps.data(), static_cast<int32_t>(maps.size()),
                     config.workerThreads, config.sendWorkers, config.rioWorkers,
                     config.completionBatch, config.sendDepth))
    {
        SLOG_ERROR("[Error] Server Init failed");
        return 1;
    }

    // 이벤트 큐 과부하 시 신규 accept 차단 임계 (네트워크 객체는 Init에서 생성된다)
    server.SetEventQueueAcceptLimit(config.eventQueueAcceptLimit);
    server.SetEnterRules(config.enterCodeTtlSec, config.allowDevEnter, config.allowDevAdmin);
    if (useServerLink)
    {
        server.SetUntaggedSessionLimit(config.maxClients);   // 웹 세션이 여유 칸을 못 먹게 — 재접속할 자리가 늘 남는다

        CZoneLink::Config link;
        link.characterIp = linkConfig.character.ip;
        link.characterPort = linkConfig.character.port;
        link.retrySec = linkConfig.retrySec;
        link.heartbeatSec = linkConfig.heartbeatSec;
        link.zoneServerId = config.zoneServerId;
        link.maxClients = config.maxClients;
        link.publicWsAddr = config.publicWsAddr;
        link.enterCodeTtlSec = config.enterCodeTtlSec;
        if (!server.ConfigureZoneLink(link))
        {
            SLOG_ERROR("[Error] Zone link config failed");
            return 1;
        }

        // [World] 가 없으면 월드 서버 없이 돈다 — 연결을 안 켠다
        if (linkConfig.HasWorld())
        {
            CWorldLink::Config world;
            world.worldIp = linkConfig.world.ip;
            world.worldPort = linkConfig.world.port;
            world.retrySec = linkConfig.retrySec;
            world.heartbeatSec = linkConfig.heartbeatSec;
            world.zoneServerId = config.zoneServerId;
            world.maxClients = config.maxClients;
            world.publicWsAddr = config.publicWsAddr;
            if (!server.ConfigureWorldLink(world))
            {
                SLOG_ERROR("[Error] World link config failed");
                return 1;
            }
        }
    }
    server.SetPlayerSpeed(config.playerSpeed);   // 0 = 기본(부하 시험 기준) — ServerConfig [Game]. 몬스터 사양은 표에 있다

    // DB 저장 파이프라인 초기화 (USE_DB_WORKER=0이면 no-op)
    {
#if USE_DB_WORKER
        // 암호를 못 읽었으면 빈 암호로 접속해 보지 않는다 — 「DB init failed」 만으로는 무엇을 고칠지 안 보인다
        if (!config.dbPasswordError.empty())
        {
            SLOG_ERROR("[Error] {}", config.dbPasswordError);
            return 1;
        }
#endif
        DBConfig dbConfig;
        dbConfig.host              = config.dbHost;               // DB 서버 주소 (기본 127.0.0.1)
        dbConfig.port              = config.dbPort;               // DB 포트 (MySQL 기본 3306)
        dbConfig.user              = config.dbUser;               // 접속 계정
        dbConfig.password          = config.dbPassword;           // 접속 비밀번호
        dbConfig.database          = config.dbDatabase;           // 사용할 스키마명 (기본 gamedb)
        dbConfig.connectTimeoutSec = config.dbConnectTimeoutSec;  // 접속 시도 타임아웃(초) — 이 시간 안에 연결 못 하면 실패 반환
        dbConfig.rwTimeoutSec      = config.dbRwTimeoutSec;       // 읽기/쓰기 소켓 타임아웃(초) — DB 무응답 시 쿼리 무한대기 방지
        if (!server.InitDB(dbConfig, config.dbSavePeriodSec, config.dbWorkers, config.dbQueueMax))
        {
            SLOG_ERROR("[Error] DB init failed");
            return 1;
        }
    }

    // Ctrl+C / 콘솔 종료 시 graceful shutdown (server.Stop이 DB 드레인 수행)
    g_gameMonitorForShutdown = &gameMonitor;
    CShutdownWait::Install(OnShutdownWaitTimeout);

    if (!server.Start())
    {
        SLOG_ERROR("[Error] Server Start failed");
        return 1;
    }

    if (monitorSvr)
    {
        monitorSvr->SetLinkMetrics([&server](std::ostringstream& ss) { server.WriteLinkMetrics(ss); });
        monitorSvr->Start();
    }

    // main 스레드는 종료 신호까지 대기
    CShutdownWait::Wait();

    if (monitorSvr) monitorSvr->Stop();
    server.Stop();

    SLOG_INFO("Server shutdown complete");
    CShutdownWait::MarkCleanedUp();   // 콘솔 종료 핸들러 해제
    return 0;
}
