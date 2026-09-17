#pragma once

// ==========================================================================
// ServerConfig — INI 파일 기반 서버 설정 로더
//
// [사용법]
//  ServerConfig config;
//  config.Load();  // 실행 파일 옆의 MMOServerConfig.ini 로드
//
// [INI 포맷]
//  [Server]
//  Mode=GameServer
//  Port=6000
//  MaxClients=1000
//  MonitorPort=9090
//  MapCount=1
//
//  [MapDefault]
//  Width=120
//  Height=120
//  SectorSize=20
//  MaxPlayersPerChannel=100
//
//  [Map1]              ← 개별 오버라이드 (없으면 MapDefault 적용)
//  Width=600
// ==========================================================================

#include <string>
#include <vector>
#include <iostream>
#include <cstdlib>      // strtol (ServerCores 범위 파싱)

#include "IniFile.h"              // 플랫폼 독립 INI 리더 (GetPrivateProfile* 대체)
#include "Platform/Platform.h"    // Platform::GetExecutableDir
#include "Common.h"
#include "MapManager.h"
#include <Common/ErrorLog.h>

struct ServerConfig
{
    ServerMode  mode         = ServerMode::GameServer;
    int         port         = 6000;
    int         maxClients   = 1000;
    int         monitorPort  = 9090;
    bool        monitorEnabled = false;
    unsigned long long affinityMask = 0;   // 프로세스를 묶을 CPU 코어 마스크 (0=미적용)
    int         workerThreads = 0;         // IOCP 워커 스레드 수 (0=서버 affinity 코어 수로 자동 산정)
    int         sendWorkers   = 0;         // 전용 송신 워커 수 (0/1=단일, 2+=uniqueId%K 워커 풀; A/B 실험용)
    int         rioWorkers    = 0;         // RIO 전송 워커 수 (USE_RIO_TRANSPORT=1 빌드 전용, 0=자동 2)
    int         completionBatch = 0;       // 완료 수거 방식 (0=GQCS 1건씩[기본], N>0=GQCSEx 최대 N건; IOCP 빌드 전용)
    int         sendDepth     = 1;         // 세션당 동시 송신 제출 상한 (1=기존 1-pending[기본], 2/4/8; 양팔 공통)

    // 게임 이벤트 큐(Worker→Game)가 이 길이를 넘으면 신규 accept 거절 (0=끄기).
    //   기본 60,000 = 한 틱 유입 추정 3,000의 20배 ≈ 게임 루프가 20틱(0.8초) 밀린 상태(메모리 약 88MB).
    //   순간 버스트로 몇 틱 밀리는 건 정상이라 그 위로 잡았다.
    //   ※ 기준 3,000은 추정 — 정상 큐 길이 미측정. mmo_event_queue_size 관측 후 재설정할 것.
    int         eventQueueAcceptLimit = 60000;

    // 게임스레드 코어 격리 (실험용) — GameCore INI에서 도출. 빈값/부적합이면 gameCore=-1, 마스크 0(=off).
    int                gameCore     = -1;  // 게임루프 전용 물리코어 (-1=격리 off). ServerCores 안의 코어여야 함.
    unsigned long long gameCoreMask = 0;   // 게임코어 HT 논리쌍 마스크
    unsigned long long ioCoreMask   = 0;   // ServerCores에서 게임코어를 뺀 나머지(=I/O 스레드용)

    // [DB] 저장 파이프라인 설정 (값은 USE_DB_WORKER 토글과 무관하게 항상 로드)
    std::string dbHost              = "127.0.0.1";
    int         dbPort              = 3306;
    std::string dbUser              = "root";
    std::string dbPassword;
    std::string dbDatabase          = "gamedb";
    int         dbWorkers           = 1;    // 1단계는 단일 워커
    int         dbSavePeriodSec     = 10;   // dirty 저장 주기(초)
    int         dbConnectTimeoutSec = 3;
    int         dbRwTimeoutSec      = 5;    // 읽기/쓰기 소켓 타임아웃(초) — DB 무응답 시 재연결 판단
    int         dbQueueMax          = 20000; // 백프레셔: 워커당 큐 상한(초과분 드롭)

    std::vector<MapConfig> maps;

    // 실행 파일 경로 기준으로 MMOServerConfig.ini 로드
    bool Load()
    {
        // 실행 파일 디렉토리 기준 INI 경로 (플랫폼 독립)
        std::string iniPath = Platform::GetExecutableDir() + "MMOServerConfig.ini";

        CIniFile ini;
        if (!ini.Load(iniPath))
        {
            SLOG_INFO("[ServerConfig] MMOServerConfig.ini not found. Using defaults.");
            SetDefaultMaps();
            return false;
        }

        // [Server] 섹션
        mode        = ParseServerMode(ini.GetString("Server", "Mode", "GameServer"));
        port        = ini.GetInt("Server", "Port", 6000);

        // INI에 음수를 적으면 파서(strtol)가 그대로 음수로 넘긴다.
        //   이 값이 GameServer::Init의 vector::assign에 들어가면 size_t로 확대되어 length_error를
        //   던지고, catch가 없어 "Init failed" 로그 대신 크래시 덤프를 남기며 죽는다.
        //   Start()의 범위 검사는 Init보다 뒤라 여기서 막지 않으면 도달하지 못한다.
        constexpr int MAX_CLIENTS_LIMIT   = 65535;  // 세션 인덱스가 SessionID 상위 16bit
        constexpr int MAX_CLIENTS_DEFAULT = 1000;

        maxClients  = ini.GetInt("Server", "MaxClients", MAX_CLIENTS_DEFAULT);
        if (maxClients <= 0 || maxClients > MAX_CLIENTS_LIMIT)
        {
            SLOG_ERROR("[ServerConfig] MaxClients({}) out of range (1~{}). Using default {}.",
                       maxClients, MAX_CLIENTS_LIMIT, MAX_CLIENTS_DEFAULT);
            maxClients = MAX_CLIENTS_DEFAULT;
        }
        monitorPort = ini.GetInt("Server", "MonitorPort", 9090);
        monitorEnabled = (ini.GetInt("Server", "MonitorEnabled", 0) != 0);

        // ServerCores: 물리코어 범위("0-5")를 받아 논리코어 비트마스크로 변환
        // (HT 형제 자동 포함 — 한 물리코어를 서버/클라가 쪼개 쓰는 격리 깨짐을 방지)
        affinityMask = ParsePhysicalCoreMask(ini.GetString("Server", "ServerCores", ""));

        // GameCore: 게임루프를 고정할 물리코어 (빈값=격리 off). ServerCores 안의 코어여야 하며,
        //   나머지 코어로 I/O 스레드를 몰아 게임스레드 L2 캐시 간섭을 차단한다. (마스크는 아래서 도출)
        DeriveGameCoreIsolation(ini.GetString("Server", "GameCore", ""));

        // WorkerThreads: IOCP 워커 스레드 수 (0=서버 affinity 코어 수로 자동)
        workerThreads = ini.GetInt("Server", "WorkerThreads", 0);

        // SendWorkers: 전용 송신 워커 수 (0/1=단일 스레드, 2+=uniqueId%K 워커 풀)
        //   분배 키가 sessionId 였을 때는 인덱스 비트가 쏠려 워커 한쪽만 일했다 — uniqueId 로 고쳤다.
        sendWorkers = ini.GetInt("Server", "SendWorkers", 0);

        // RioWorkers: RIO 전송 워커 수 (USE_RIO_TRANSPORT=1 빌드에서만 사용, 0=자동 2)
        rioWorkers = ini.GetInt("Server", "RioWorkers", 0);

        // CompletionBatch: 완료 수거 방식 A/B (IOCP 빌드 전용, 재빌드 없이 팔 전환)
        //   0 = GetQueuedCompletionStatus — 완료 1건마다 syscall 1회 (기존 동작, 기본값)
        //   N>0 = GetQueuedCompletionStatusEx — 한 번에 최대 N건 수거 (상한은 IOCPServer::Start에서 clamp)
        completionBatch = ini.GetInt("Server", "CompletionBatch", 0);

        // SendDepth: 세션당 동시 송신 제출 상한 A/B (양팔 공통, 재빌드 없이 깊이 전환)
        //   1=기존 1-pending. 2의 거듭제곱만 유효하고(1/2/4/8) 그 외 값은 서버가 아래쪽으로 내린다.
        sendDepth = ini.GetInt("Server", "SendDepth", 1);

        // EventQueueAcceptLimit: 게임 이벤트 큐 과부하 시 신규 accept 차단 임계 (0=끄기)
        eventQueueAcceptLimit = ini.GetInt("Server", "EventQueueAcceptLimit", 60000);
        if (eventQueueAcceptLimit < 0)
            eventQueueAcceptLimit = 0;

        // [DB] 섹션 — DB 저장 파이프라인 (값은 파서가 narrow std::string으로 반환)
        dbHost              = ini.GetString("DB", "Host", "127.0.0.1");
        dbPort              = ini.GetInt("DB", "Port", 3306);
        dbUser              = ini.GetString("DB", "User", "root");
        dbPassword          = ini.GetString("DB", "Password", "");
        dbDatabase          = ini.GetString("DB", "Database", "gamedb");
        dbWorkers           = ini.GetInt("DB", "Workers", 1);
        dbSavePeriodSec     = ini.GetInt("DB", "SavePeriodSec", 10);
        dbConnectTimeoutSec = ini.GetInt("DB", "ConnectTimeoutSec", 3);
        dbRwTimeoutSec      = ini.GetInt("DB", "RwTimeoutSec", 5);
        dbQueueMax          = ini.GetInt("DB", "QueueMax", 20000);

        int mapCount = ini.GetInt("Server", "MapCount", 3);

        // [MapDefault] 섹션 로드
        MapConfig defaultMap = {};
        defaultMap.mapWidth             = ini.GetInt("MapDefault", "Width", 120);
        defaultMap.mapHeight            = ini.GetInt("MapDefault", "Height", 120);
        defaultMap.sectorSize           = ini.GetInt("MapDefault", "SectorSize", 20);
        defaultMap.maxPlayersPerChannel = ini.GetInt("MapDefault", "MaxPlayersPerChannel", 100);

        // [Map0] ~ [MapN-1] 섹션 순회 (없으면 디폴트 적용)
        maps.clear();
        for (int i = 0; i < mapCount; ++i)
        {
            std::string section = "Map" + std::to_string(i);

            MapConfig mc;
            mc.mapId                = i;
            mc.mapWidth             = ini.GetInt(section, "Width", defaultMap.mapWidth);
            mc.mapHeight            = ini.GetInt(section, "Height", defaultMap.mapHeight);
            mc.sectorSize           = ini.GetInt(section, "SectorSize", defaultMap.sectorSize);
            mc.maxPlayersPerChannel = ini.GetInt(section, "MaxPlayersPerChannel", defaultMap.maxPlayersPerChannel);
            maps.push_back(mc);
        }

        PrintConfig();
        return true;
    }

private:
    // "0-5" 또는 "3" 형태의 물리코어 범위 → 논리코어 비트마스크.
    // 가정: 물리코어 k = 논리코어 2k, 2k+1 (Intel HT 표준 매핑).
    // 빈 문자열·숫자 아님 → 0(미적용).
    static unsigned long long ParsePhysicalCoreMask(const std::string& str)
    {
        const char* s = str.c_str();
        char* end = nullptr;
        long first = std::strtol(s, &end, 10);
        if (end == s) return 0;                          // 숫자로 시작 안 함
        long last = first;
        if (*end == '-') last = std::strtol(end + 1, &end, 10);
        if (first < 0 || last < first) return 0;
        if (last > 31) last = 31;                        // 64비트 마스크 상한 (물리코어 0~31)
        unsigned long long mask = 0;
        for (long k = first; k <= last; ++k)
            mask |= (0x3ull << (2 * k));                 // HT 형제 2논리코어
        return mask;
    }

    // GameCore 문자열 → gameCore/gameCoreMask/ioCoreMask 도출. affinityMask(ServerCores)가 선행돼야 함.
    // 빈값·비숫자·범위밖·ServerCores밖·남는코어없음 중 하나라도면 격리 off(전부 0/-1)로 두고 WARN.
    void DeriveGameCoreIsolation(const std::string& str)
    {
        gameCore = -1; gameCoreMask = 0; ioCoreMask = 0;
        if (str.empty())
            return;                                       // 빈값 = 격리 off (기본)

        const char* s = str.c_str();
        char* end = nullptr;
        long core = std::strtol(s, &end, 10);
        if (end == s || core < 0 || core > 31)
        {
            SLOG_WARN("[ServerConfig] GameCore '{}' 무시 — 물리코어 숫자(0~31)가 아님", str);
            return;
        }
        if (affinityMask == 0)
        {
            SLOG_WARN("[ServerConfig] GameCore={} 무시 — ServerCores 미설정(프로세스 미고정이라 격리 불가)", core);
            return;
        }
        const unsigned long long gm = (0x3ull << (2 * core));   // 게임코어 HT 논리쌍
        if ((gm & affinityMask) != gm)
        {
            SLOG_WARN("[ServerConfig] GameCore={} 무시 — ServerCores(0x{:X}) 밖의 코어", core, affinityMask);
            return;
        }
        const unsigned long long io = affinityMask & ~gm;       // 나머지 코어(=I/O)
        if (io == 0)
        {
            SLOG_WARN("[ServerConfig] GameCore={} 무시 — I/O에 남는 코어 없음(ServerCores 물리코어 2개 이상 필요)", core);
            return;
        }
        gameCore = static_cast<int>(core);
        gameCoreMask = gm;
        ioCoreMask   = io;
    }

    void SetDefaultMaps()
    {
        maps = 
        {
            { 0, 120, 120, 20, 100 },
        };
    }

    static ServerMode ParseServerMode(const std::string& s)
    {
        if (s == "GameCodiEchoTest")    return ServerMode::GameCodiEchoTest;
        if (s == "NetWorkLib_EchoTest") return ServerMode::NetWorkLib_EchoTest;
        if (s == "GameServer")          return ServerMode::GameServer;

        SLOG_WARN("[ServerConfig] Unknown Mode '{}'. Defaulting to GameServer.", s);
        return ServerMode::GameServer;
    }

    void PrintConfig() const
    {
        const char* modeName = "Unknown";
        switch (mode)
        {
        case ServerMode::GameCodiEchoTest:    modeName = "GameCodiEchoTest";    break;
        case ServerMode::NetWorkLib_EchoTest: modeName = "NetWorkLib_EchoTest"; break;
        case ServerMode::GameServer:          modeName = "GameServer";          break;
        }

        SLOG_INFO("[ServerConfig] Loaded from INI");
        SLOG_INFO("  Mode        : {}", modeName);
        SLOG_INFO("  Port        : {}", port);
        SLOG_INFO("  MaxClients  : {}", maxClients);
        SLOG_INFO("  MonitorPort : {}", monitorPort);
        SLOG_INFO("  MonitorOn   : {}", monitorEnabled ? "true" : "false");
        SLOG_INFO("  Affinity    : 0x{:X}", affinityMask);
        SLOG_INFO("  WorkerThr   : {} (0=auto)", workerThreads);
        SLOG_INFO("  SendWkr     : {} (0/1=single)", sendWorkers);
        SLOG_INFO("  RioWkr      : {} (0=auto 2, RIO build only)", rioWorkers);
        SLOG_INFO("  ComplBatch  : {} (0=GQCS one-at-a-time, N=GQCSEx cap)", completionBatch);
        SLOG_INFO("  SendDepth   : {} (1=1-pending, pow2 only: 1/2/4/8)", sendDepth);
        SLOG_INFO("  EvtQAccept  : {} (0=off, 넘으면 신규 accept 거절)", eventQueueAcceptLimit);
        if (gameCore >= 0)
            SLOG_INFO("  CoreIso     : ON GameCore={} game=0x{:X} io=0x{:X}", gameCore, gameCoreMask, ioCoreMask);
        else
            SLOG_INFO("  CoreIso     : off (GameCore 미설정)");
        SLOG_INFO("  DB          : {}:{} db={} user={} workers={} save={}s qmax={}",
                  dbHost, dbPort, dbDatabase, dbUser, dbWorkers, dbSavePeriodSec, dbQueueMax);
        SLOG_INFO("  Maps        : {}", maps.size());
    }
};
