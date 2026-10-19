#include "GameServer.h"
#include "Player.h"
#include "MonsterAI.h"   // USE_MONSTER — 몬스터 표 속도 대조의 상한(MAX_SPEED)
#include "CooldownBuffBlob.h" // USE_DB_WORKER — 쿨타임·버프 열의 바이트 형식(토글을 안 보는 헤더)
#include <Protocol/Protocol.h>
#include "SerialBuffer.h"
#include <Common/ErrorLog.h>
#include "CoreAffinity.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cassert>  // 불변식 검증(틱 끝 이동 표시 · 받은 패킷 머리) — 직렬화 크기 검증은 조립 함수와 같이 조각들로 갔다
#include <string>

// ==========================================================================
// S2C 직렬화 헬퍼 (파일 로컬)
//
// 와이어 = payload 영역 바이트 그대로 (WSASend가 GetReadBufferPtr()+GetDataSize()만 전송).
// pack(1)이라 패딩이 없으므로 필드를 선언 순서/폭대로 쓰면 구조체와 바이트 동일.
// 스칼라는 <<, 고정배열/문자열은 SetData(raw) — << 문자열 연산자는 길이접두(2B)가 끼어
// 와이어가 깨지므로 사용 금지.
//
// [소유권 계약] 빌더는 RefCount=1(소유권 1개)인 버퍼를 반환하고,
//   송신 함수(BroadcastAroundSector/SendPacket의 CSerialBuffer* 오버로드)가 이를 소비한다.
// ==========================================================================
namespace
{
    // 머리 쓰기·닫기(BeginPacket · FinalizePacket)와 등장 조각(WriteSnapshot · WriteCreateEntry)은 PacketBuild.h 로 옮겼다
    //   — 여러 조각이 같이 쓴다(게임 서버 나누기 0-②). 강제 인라인(CREATE_FORCEINLINE)도 거기서 정한다.

    // MakeMoveStart · MakeMoveStop · MakeSyncPosition(이동 셋) 은 SightBroadcaster.cpp — 시야 방송 조각만 부른다(게임 서버 나누기 3 단계)

    // MakeZoneInfo · MakeCreateMyPlayer(존 정보 · 내 캐릭터 생성) 는 PlayerPlacement.cpp — 배치 조각만 부른다(게임 서버 나누기 5 단계)

    // MakeCreateOtherPlayer · MakeDeletePlayer(남의 캐릭터 생성·삭제) 는 SightBroadcaster.cpp — 시야 방송 조각만 부른다(게임 서버 나누기 3 단계)

    // MakeAttack · MakeDamage · MakeDeath(휘두름 · 피해 · 죽음) 는 CombatHandler.cpp — 전투 조각만 부른다(게임 서버 나누기 9 단계)

    // MakeRespawn 은 PacketBuild.h — 본인 알림(도착 때 본인)과 전투(부활 방송) 둘이 부른다

    // MakeExp(경험치) 는 SelfNotifier.cpp — 본인 알림 조각만 부른다(게임 서버 나누기 2 단계)

    // MakeActorStats 는 PacketBuild.h — 전투 · 처치 보상 · 가방 · 운영 명령이 같이 부른다

    // MakeMyStats(내 최종 공격력·방어력) 는 SelfNotifier.cpp — 본인 알림 조각만 부른다(게임 서버 나누기 2 단계)

    // MakeZoneChangeOk · MakeZoneChangeFail(존 이동 성공·실패) 는 PlayerPlacement.cpp — 배치 조각만 부른다(게임 서버 나누기 5 단계)

    // ToZoneChangeFailReason(존 이동 거절 사유 → 와이어) 은 EntryHandler.cpp — 입장 조각만 부른다(게임 서버 나누기 12 단계)

    // 땅 아이템·줍기·쓰기·장착 거절 사유 → 와이어 값(ToGroundItemGone · ToPickupFail · ToUseItemFail · ToEquipFail) 은 ItemHandler.cpp
    //   — 가방·땅 조각만 부른다(게임 서버 나누기 6 단계)

    // MakeAdminLoginOk · MakeAdminLoginFail · MakeChat(운영자 로그인 성공·실패 · 채팅) 는 ChatHandler.cpp — 채팅·운영 조각만 부른다(게임 서버 나누기 13 단계)

    // MakeError(알림 한 줄) · MakeInventory(가방 전체) 는 SelfNotifier.cpp(2 단계) · MakeInventorySlot(칸 하나) · MakeUseItemFail · MakeEquipFail(쓰기·장착 거절) 는
    //   ItemHandler.cpp(6 단계) — 각각 그 조각만 부른다(게임 서버 나누기)

    // MakeActorBuff(지속 효과 통보 — 본인에게만) 는 SelfNotifier.cpp — 본인 알림 조각만 부른다(게임 서버 나누기 2 단계)

    // ToSkillFail(스킬 거절 사유 → 와이어) · MakeSkillCast · MakeSkillFail(시전 · 스킬 거절) 는 CombatHandler.cpp — 전투 조각만 부른다(게임 서버 나누기 9 단계)

    // MakeSkillCooldownRemain(남은 스킬 쿨타임) · MakeActorMp(마력) 는 SelfNotifier.cpp — 본인 알림 조각만 부른다(게임 서버 나누기 2 단계)

    // MakePartyAsk · MakePartyAskEnd · MakePartyNotice · MakePartyInfo · MakePartyMember(파티 패킷) 는 PartyHandler.cpp
    //   — 파티·방 조각만 부른다(게임 서버 나누기 7 단계)

    // MakeGroundItemCreate · MakeGroundItemDelete(땅 아이템 보이기·지우기) 는 SightBroadcaster.cpp(3 단계) · MakePickupFail(줍기 거절) 은 ItemHandler.cpp(6 단계)

    // MakeSectorUpdates(섹터 묶음) · MakeCreatePlayerBatch · MakeDeletePlayerBatch(인바운드 멤버십 묶음 · 크기 static_assert 포함) 는
    //   SightBroadcaster.cpp — 시야 방송 조각만 부른다(게임 서버 나누기 3 단계)
}

CGameServer::CGameServer(CMonitorManager& monitor, CGameMonitor& gameMonitor)
    : _mode(ServerMode::GameServer)
    , _monitor(monitor)
    , _gameMonitor(gameMonitor)
    , _running(false)
{
}

CGameServer::~CGameServer()
{
    Stop();
}

bool CGameServer::Init(ServerMode mode, int port, int maxClients)
{
    return Init(mode, port, maxClients, nullptr, 0);
}

bool CGameServer::Init(ServerMode mode, int port, int maxClients,
                        const MapConfig* maps, int32_t mapCount, int workerThreads, int sendWorkers,
                        int rioWorkers, int completionBatch, int sendDepth)
{
    _mode = mode;

    if (!RegisterRoutes())
    {
        SLOG_ERROR("[GameServer] 패킷 분기 표 등록 실패 — 표 범위 밖 번호이거나 같은 번호를 두 번 등록했다 (RegisterRoutes)");
        return false;
    }

    _network = std::make_unique<NetIoModel>(port, maxClients, _mode, _monitor,
                                            workerThreads, sendWorkers, rioWorkers, completionBatch, sendDepth);

    // 게임 서버 모드일 때만 맵 등록
    if (_mode == ServerMode::GameServer)
    {
        if (maps == nullptr || mapCount <= 0)
            return false;

        for (int32_t i = 0; i < mapCount; ++i)
        {
            // 섹터 묶음이 좌표를 눈금(uint16)으로 실어 나른다 — 맵이 상한(POS_QUANT_MAX_MAP_SIZE)을 넘으면 좌표가
            //   조용히 깨지므로 등록 전에 막는다. 와이어 사정이라 패킷을 모르는 맵 관리자가 아니라 여기서 본다(10-06).
            if (maps[i].mapWidth > POS_QUANT_MAX_MAP_SIZE || maps[i].mapHeight > POS_QUANT_MAX_MAP_SIZE)
            {
                SLOG_ERROR("[GameServer] 맵 {} 크기 {}×{} — 와이어 좌표 눈금이 실을 수 있는 {} 이하여야 합니다 (MapTable.csv)",
                           maps[i].mapId, maps[i].mapWidth, maps[i].mapHeight, POS_QUANT_MAX_MAP_SIZE);
                return false;
            }
            if (!_mapManager.RegisterMap(maps[i]))
                return false;
        }

        // 첫 번째 맵을 기본 접속 맵으로 설정
        _defaultMapId = maps[0].mapId;

        // 프레임 재사용 컨테이너 reserve (워밍업 realloc 방지)
        // 전체 서버 범위 버퍼도 maxPerChannel 기준 — 초과 시 자연 증가 후 capacity 유지
        int32_t maxPerChannel = maps[0].maxPlayersPerChannel;
        _sessionHub.Init(_network.get(), maxClients);   // 세션 칸 maxClients 개(전부 빈 칸) · 보내기에 쓸 네트워크
        _entryHandler.Init(maxClients);   // 대기표 — 위 칸과 같은 크기·같은 색인 (불변식 공유 · DB 를 켠 빌드만 잡는다 — 입장 조각)
        _sightBroadcaster.Init(maxPerChannel, maxClients);   // 주변 9섹터 단위 버퍼 둘 · 전체 서버 dirty (이동/sync 묶음 대상)
    }

    return true;
}

bool CGameServer::LoadGameDataFiles(const std::string& dir)
{
    if (!_gameDataFiles.LoadAll(dir))
        return false;

#if USE_MONSTER
    // 몬스터 속도는 AI 판단 주기가 전제하는 상한 안이어야 한다 — 상한은 AI 사정이라 표만으로는 못 본다(MonsterAI.h MAX_SPEED).
    if (!_gameDataFiles.Monsters().CheckSpeed(MonsterAI::MAX_SPEED))
        return false;
#endif
    return true;
}

bool CGameServer::Start()
{
    if (!_network->Start())
        return false;

    // 게임 서버 모드일 때만 게임 루프 스레드 시작
    // 에코 테스트는 CIOCPServer 내부에서 자체 처리
    if (_mode == ServerMode::GameServer)
    {
        _running = true;
        _gameThread = std::thread(&CGameServer::GameLoopThread, this);
    }

    const char* modeName = "Unknown";
    switch (_mode)
    {
    case ServerMode::GameCodiEchoTest:    modeName = "GameCodiEchoTest";    break;
    case ServerMode::NetWorkLib_EchoTest: modeName = "NetWorkLib_EchoTest"; break;
    case ServerMode::GameServer:          modeName = "GameServer";          break;
    }
    SLOG_INFO("[GameServer] Started - Mode: {}", modeName);

    return true;
}

void CGameServer::Stop()
{
    _running = false;

    if (_gameThread.joinable())
        _gameThread.join();

#if USE_DB_WORKER
    // 게임 스레드 종료 후(단일 스레드 시점) 전원 최종 저장 → 드레인 → 워커 종료 — 캐릭터 저장소(CCharacterStore::Shutdown)
    _characterStore.Shutdown();
#endif

    if (_network)
    {
        _network->ShutdownServer();

        // 미처리 이벤트의 CSerialBuffer 정리
        NetworkEvent event(NetworkEvent::Type::CONNECTED, 0);
        while (_network->PopNetworkEvent(event))
        {
            if (event.pMsg != nullptr)
                event.pMsg->SubRef();
        }
    }

    // 잔여 플레이어 정리 — 슬롯(unique_ptr)이 주인이라 비우면 해제된다(DB 를 켠 빌드면 최종 저장은 위 저장소 Shutdown 의 SaveAllPlayers 가 이미 했다)
    _sessionHub.RemoveAll();

    // 몬스터는 여기서 안 치운다 — 소유가 존이라 _mapManager 가 소멸할 때 존과 같이 해제된다.
    //   게임 스레드가 이미 끝나(위 join) 그 사이에 몬스터를 읽는 코드가 없다.
}

#if USE_DB_WORKER
// ==========================================================================
// DB 저장 파이프라인 — 워커 띄우기·멈추기 · 저장 세 길 · 저장 대상 판정 · 쿨타임·버프 묶기·되살리기는 캐릭터 저장소 조각으로 옮겼다
//   — CCharacterStore (CharacterStore.cpp · 게임 서버 나누기 4 단계). 입장 대기표와 불러오기 결과 처리(입장 몫)는 입장 조각 — CEntryHandler(12 단계). 여기 남은 것은 공개 입구 InitDB 하나
// ==========================================================================

// 서버의 공개 입구 — main 이 부른다(Start 전). 몸은 저장소가 든다.
bool CGameServer::InitDB(const DBConfig& dbConfig, int savePeriodSec, int workerCount, int queueMax)
{
    return _characterStore.Init(dbConfig, savePeriodSec, workerCount, queueMax);
}

// 입장 대기표(AdjustPendingCount · SetPendingEnter · PromotePendingEnter · TakePendingEnter) · 불러오기 결과(OnCharacterLoaded — ICharacterLoadEvents) 는
//   입장 조각으로 옮겼다 — CEntryHandler (게임 서버 나누기 12 단계)

// 불러오기 요청(RequestCharacterLoad) 은 캐릭터 저장소 조각으로 옮겼다 — CCharacterStore (게임 서버 나누기 4 단계)

// 회신 꺼내기(ProcessDbResults) 는 캐릭터 저장소 조각으로 옮겼다 — 불러오기 결과는 알림(ICharacterLoadEvents)으로 아래 OnCharacterLoaded 에 온다 (게임 서버 나누기 4 단계)
#else
bool CGameServer::InitDB(const DBConfig&, int, int, int) { return true; }   // 토글 OFF: no-op
#endif

// ==========================================================================
// 게임 루프
// ==========================================================================

void CGameServer::GameLoopThread()
{
    using Clock = std::chrono::steady_clock;

    CoreAffinity::PinGameThread();   // 게임루프 전용 코어 고정 (격리 off면 no-op)

    // CPU 점유율 측정용: 자기 실핸들을 복제해 모니터에 등록 (진단정리 6 사각지대 보강)
    // GetCurrentThread()는 의사핸들(호출 스레드 기준)이라 HTTP 스레드에서 못 씀 → 실핸들로 복제
    {
        _monitor._gameLoopThreadHandle = Platform::CaptureCurrentThreadCpu();
    }

    auto prevTime = Clock::now();

    while (_running)
    {
        auto frameStart = Clock::now();

        // deltaTime 계산
        float deltaTime = std::chrono::duration<float>(frameStart - prevTime).count();
        prevTime = frameStart;
        ++_frameCount;   // 이동 예산의 경과 시간 산출 기준 (틱당 FRAME_INTERVAL_MS)

        // 1) 네트워크 이벤트 전부 소비
        _monitor._gameLoop._eventQueueSize.Store(static_cast<int64_t>(_network->GetEventQueueSize()));
        auto phaseT1 = Clock::now();
        ProcessNetworkEvents();

#if USE_DB_WORKER
        // 1-0) DB 회신 소비 — 워커가 돌려준 결과(캐릭터 불러오기)를 통째로 꺼낸다 (락 1회).
        //   네트워크 이벤트 "뒤"에 두는 이유: 같은 틱에 그 세션의 끊김과 결과가 같이 오면 끊김을 먼저
        //   처리해 결과가 주인 없는 것으로 버려지게 한다. 거꾸로면 들여보냈다가 곧바로 지우게 된다.
        //   네트워크 구간(phaseT1~T2) "안"에 두는 이유: 접속 처리(스폰) 비용이 원래 이 구간에 있었다.
        //   결과를 받아 스폰하게 바뀐 뒤에도 _phaseNetworkUs 가 같은 것을 재도록 자리를 맞춘다.
        _characterStore.ProcessDbResults();
#endif
        auto phaseT2 = Clock::now();

#if USE_MONSTER
        // 1-1) 몬스터 — 존마다 관리자가 리젠 주기가 된 구역을 채우고, 몬스터마다 판단한다(CMonsterManager::Tick).
        //   좌표 갱신(2) 앞에 두는 이유: AI 가 정한 방향이 같은 틱에 반영돼야 한 틱씩 밀리지 않는다.
        _monsterHandler.TickMonsters();
#endif

        // 2) 게임 로직 갱신 (좌표 이동 + 섹터 변경 감지 + 경계 클램핑) — 섹터 변경은 존마다 대기열에 쌓인다
        //   경계에 막혀 선 캐릭터는 존이 그 자리에서 알린다(OnCharacterMoveStop — 그 알림은 시야 조각이 받는다)
        _mapManager.TickAll(deltaTime, _sightBroadcaster);

        // 3) 섹터 변경 배치 처리 — 존마다 대기열(이동 패킷 + 존 틱, 넣을 때 중복 차단)을 읽어 보내고 비운다.
        //   존마다 따로 도는 까닭: 대기열의 섹터 좌표는 그 존의 격자이고, 보내는 대상도 그 존 사람뿐이다.
        //   보내기 · 같은 틱 쌍 보정 · 멤버십 구간 계측은 시야 조각(CSightBroadcaster::ProcessSightChanges)
        _sightBroadcaster.ProcessSightChanges();

#if USE_COMBAT
        // 3-2) 부활 대기자 훑기 (게임 로직 페이즈에 계상 — 브로드캐스트를 동반한다)
        //   플레이어만 — 몬스터는 되살리지 않는다(죽는 순간 지우고, 스폰이 새로 채운다 — 1-1).
        _combatHandler.TickRespawn();

        // 3-2b) 지속 효과 만료 — 효과가 걸린 캐릭터 목록만 훑는다(비면 그 자리에서 끝). 풀리면 알림(OnBuffChanged)으로 본인에게 간다.
        //   존마다 돌지 않는다 — 목록이 게임 서버에 하나라 존을 옮긴 사람도 그대로 따라온다(BuffSystem.h [왜 따로인가]).
        _buffSystem.Tick(_frameCount);
#endif

#if USE_PARTY
        // 3-2c) 파티 — 요청 시간 지남 · 파티원 상태 비교(약 0.5초마다). 파티·요청이 없으면 바로 끝난다(부하 시험엔 파티가 없다).
        //   존마다 돌지 않는다 — 파티는 존을 넘는다(목록이 게임 서버에 하나, PartySystem.h [왜 따로인가]).
        _partySystem.Tick(_frameCount);

        // 방 다시 들어가기 잠금 — 초마다 풀린 것 빼기는 파티·방 조각(CPartyHandler::TickRoomLocks · 게임 서버 나누기 7 단계)
        _partyHandler.TickRoomLocks();
#endif

#if USE_DROP
        // 3-3) 땅의 아이템 사라지기 — 섹터 변경 대기열을 비운 뒤라 "같은 틱 이동자" 보정이 필요 없다
        _itemHandler.TickGroundItems();
#endif

        auto phaseT3 = Clock::now();

        // 4) 주기적 위치 동기화 (MOVING 플레이어 → 주변 브로드캐스트) — 시야 조각. 같은 순회에 동접 게이지(_enter._inGame)가 얹혀 있다
        _sightBroadcaster.TickPeriodicSync();

        // 이번 틱 보류물 비우기 — digest(USE_BROADCAST_BUNDLE) 또는 섹터 묶음(USE_SECTOR_AGGREGATION). 아래 flush가 묶어 보냄
        _sightBroadcaster.FlushSends();

        // [coalescing] 이번 틱에 쌓인 송신을 세션당 1회 WSASend로 flush (broadcast_sync 페이즈에 계상)
        // [계측] flush 구간을 별도 측정 → 비용종류별 "송신(WSASend)" 분리. _phaseBroadcastSyncUs는 그대로 둠(이 값을 포함).
        auto flushT0 = Clock::now();
        _network->FlushPendingSends();
        auto flushT1 = Clock::now();

        auto phaseT4 = Clock::now();

        // 구간별 시간 기록 (마이크로초 누적)
        _monitor._gameLoop._phaseNetworkUs.Add(std::chrono::duration_cast<std::chrono::microseconds>(phaseT2 - phaseT1).count());
        _monitor._gameLoop._phaseGameLogicUs.Add(std::chrono::duration_cast<std::chrono::microseconds>(phaseT3 - phaseT2).count());
        _monitor._gameLoop._phaseBroadcastSyncUs.Add(std::chrono::duration_cast<std::chrono::microseconds>(phaseT4 - phaseT3).count());

        // [계측] 비용종류별 — 틱 내 누적분을 1회씩 원자 반영 후 리셋 (송신은 flush 구간 직접 측정)
        //   방송 쪽 다섯(주변 모으기 · 수신자별 처리 · 멤버십 횟수·시간 · 쌍 보정)은 시야 조각이 반영한다
        _sightBroadcaster.ReportTickMetrics();
        _monitor._gameLoop._flushSendUs.Add(std::chrono::duration_cast<std::chrono::microseconds>(flushT1 - flushT0).count());
        _moveHandler.ReportTickMetrics();   // 이동 예산 거부 건수 — 이동 조각(게임 서버 나누기 11 단계)

        // [계측 오버헤드 절감] handle-latency 지역 누적분 → 전역 1회 반영
        _monitor._gameLoop.FlushHandleLatency();

        // 5) 빈 동적 채널 정리
        ++_cleanupFrameCount;
        if (_cleanupFrameCount >= CLEANUP_INTERVAL_FRAMES)
        {
            _cleanupFrameCount = 0;

            // 정리는 틱 끝(5)이라 몬스터를 가리키는 게임 서버 대기열이 없다 — 존을 그대로 지워도 된다.
            //   이동 묶음은 틱 끝 송신이 비웠다. 섹터 이동 대기열은 존 안이라
            //   존과 같이 사라진다. 누가 정리를 틱 앞쪽으로 옮기면 여기서 걸린다.
            assert(!_sightBroadcaster.HasDirtyMovers());
            // 지우기 직전 콜백 — 존 밖에서 이 존의 것을 든 곳을 정리하는 마지막 기회다. 둘 다 토글이 따로라 한 콜백에 합친다
            //   (땅 아이템만 USE_DROP 안에 묶어 두었더니 USE_DROP 0 이면 콜백이 아예 없어 버프 목록 정리도 같이 빠졌을 것이다).
#if USE_DROP
            const GroundItemContext groundCtx = _itemHandler.MakeGroundItemContext();
#endif
#if USE_DROP || USE_COMBAT || USE_PARTY
            _mapManager.CleanupEmptyChannels([&](CZone* zone)
            {
#if USE_DROP
                zone->GetGroundItemManager().ForgetAll(groundCtx);   // 땅의 아이템은 세기만 — 목록은 존과 함께 사라진다(알릴 사람이 없다)
#endif
#if USE_COMBAT
                // 효과 목록은 틱 너머로 주소를 든다 — 이 존의 몬스터가 같이 해제되니 목록에서 먼저 뺀다(BuffSystem.h 「셋째 길」). 사람은 이미 퇴장으로 빠졌다
                _buffSystem.ForgetZone(zone->GetZoneId());
#endif
#if USE_PARTY
                // 빈 방 — 열린 방 수를 내린다(등식: 들어감 = 닫힘 + 열림 — GameMonitor.h _room). 방 번호는 이 뒤 다시 쓸 수 있다(CMapInstance::CreateRoom)
                if (_mapManager.IsRoomMap(zone->GetMapId()))
                {
                    _gameMonitor._room._active.Add(-1);
                    _gameMonitor._room._closed.Inc();
                }
#endif
            });
#else
            _mapManager.CleanupEmptyChannels(nullptr);
#endif
        }

#if USE_DB_WORKER
        // 5-1) DB 주기 저장 — dirty(또는 MOVING) 플레이어만 워커에 enqueue (틱 임계경로 밖 I/O)
        _characterStore.TickPeriodicSave();
#endif

        // 6) Tick 시간 기록 + 프레임 제한
        auto frameEnd = Clock::now();
        double tickMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
        _monitor._gameLoop.RecordTickTime(tickMs);

        int sleepMs = FRAME_INTERVAL_MS - static_cast<int>(tickMs);
        if (sleepMs > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }
}

// ==========================================================================
// 이벤트 디스패치
// ==========================================================================

void CGameServer::ProcessNetworkEvents()
{
    // 프레임 진입 시점 스냅샷을 통째로 스왑 (락 1회). 처리 중 도착분은 다음 프레임으로 이월
    // _localEvents는 직전 프레임에 모두 소비되어 비어 있음 (멤버 재사용으로 deque 블록 보존)
    _network->SwapOutNetworkEvents(_localEvents);

    while (!_localEvents.empty())
    {
        NetworkEvent& event = _localEvents.front();
        switch (event.type)
        {
        case NetworkEvent::Type::CONNECTED:
            _entryHandler.OnConnected(event.sessionId);
            break;

        case NetworkEvent::Type::DISCONNECTED:
            OnDisconnected(event.sessionId);
            break;

        case NetworkEvent::Type::RECEIVED:
            OnReceived(event.sessionId, event.pMsg);
            // handle-latency: enqueue(recv) → 처리완료(응답 송신 포함) 시간.
            // 틱 게이트 대기 + 핸들러 비용 = RTT의 서버 기여분. RECEIVED만 측정.
            if (event.enqueueTimeNs != 0)
            {
                int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                _monitor._gameLoop.RecordHandleLatencyLocal(
                    static_cast<double>(nowNs - event.enqueueTimeNs) / 1.0e6);
            }
            break;
        }
        _localEvents.pop();
    }
}

// 스폰(SpawnPlayer) · 접속(OnConnected) · 같은 계정 걷어내기(KickDuplicateAccount) · 입장 요청(RecvEnterGame) 은 입장 조각으로 옮겼다
//   — CEntryHandler (게임 서버 나누기 12 단계)

void CGameServer::OnDisconnected(int64_t sessionId)
{
    CPlayer* player = _sessionHub.FindPlayer(sessionId);
    if (player == nullptr)
    {
#if USE_DB_WORKER
        // 불러오는 중에 나간 세션 — 플레이어가 아직 없으니 저장도 방송도 없다. 대기표만 비운다.
        //   뒤늦게 오는 결과는 대기표에서 주인을 못 찾아 orphaned 로 세어진다.
        _entryHandler.DropPendingEnter(sessionId);   // 대기표 자리 비우기 — 입장 조각(게임 서버 나누기 12 단계)
#endif
        return;
    }
    _placement.LeaveGame(player);
}

// 떠나보내기(LeaveGame) · 놓기(ReleasePlayer) 는 배치 조각으로 옮겼다 — CPlayerPlacement (게임 서버 나누기 5 단계)

// ── 「존에서 빠짐 · 떠남」 알림 (IPlacementEvents) ── 배치 조각(CPlayerPlacement)이 부른다 — 몸은 옮기기 전 RemoveFromZone · ReleasePlayer 의
//   그 자리 그대로(게임 서버 나누기 5 단계 · 0-③ ⑴⑶). 받는 쪽은 게임 서버로 둔다(9 단계 결정) — 정리할 목록이 전투 조각(부활 대기열) ·
//   지속 효과 규칙 · 파티 규칙 셋에 흩어져 있어, 한 조각이 받으면 그 조각이 남의 목록을 알게 된다. 차례(대기열 → 효과 → 파티)는 여기 한 곳.

// 존에서 빠짐 — 시야 몫 뒤 · CZone::LeaveZone 앞(CPlayerPlacement::RemoveFromZone). 종류를 안 가린다(플레이어·몬스터).
void CGameServer::OnLeavingZone(CCharacter* player)
{
#if USE_COMBAT
    // 부활 대기열에서도 제거 — 위 이동 묶음·아래 LeaveZone 의 섹터 이동 대기열과 같은 이유다(틱 끝 TickRespawn dangling 방지).
    //   죽은 채로 퇴장·존이동하는 경우가 실제 경로다(부활 3초 안에 창을 닫으면 여기로 온다).
    _combatHandler.RemoveFromDeadActors(player);
#endif
    (void)player;   // 전투를 끈 빌드엔 할 일이 없다
}

// 떠남 — 최종 저장 뒤 · 칸 비우기(해제) 앞(CPlayerPlacement::ReleasePlayer). 이 사람에게 보낼 세션은 이미 없다.
void CGameServer::OnLeavingGame(CPlayer* player)
{
#if USE_COMBAT
    // 부활 대기열 회수 — 끊김 길에서는 RemoveFromZone 이 이미 하지만, 존을 못 찾은 갈래는 그걸 건너뛴다.
    //   그 상태로 죽어 있을 수는 없어도, 아래 해제 뒤에 대기열이 그 주소를 들고 있으면
    //   다음 틱이 해제된 메모리를 밟는다. 두 번 불러도 무해하므로 여기서도 뺀다.
    _combatHandler.RemoveFromDeadActors(player);

    // 지속 효과 목록도 같은 까닭으로 여기서 뺀다(알림 없음 — 세션이 없다). RemoveFromZone 에서는 안 뺀다 — 존 이동이 그 길을 지난다.
    _buffSystem.Forget(player);
#endif

#if USE_PARTY
    // 파티원·요청 표도 같은 까닭으로 여기서 뺀다 — 남은 파티원·요청 상대에게는 알림이 간다(이 사람에게는 안 간다 — 세션이 없다).
    //   끊기면 바로 파티에서 빠지는 규칙(10-16 사용자 결정)이 이 한 줄이다. 존 이동·죽음은 이 길을 안 지난다(파티는 존을 넘는다).
    _partySystem.Forget(player);
#endif
    (void)player;   // 전투·파티를 다 끈 빌드엔 할 일이 없다
}

void CGameServer::OnReceived(int64_t sessionId, CSerialBuffer* pMsg)
{
    // [불변식] RECEIVED 생산자는 ParsePackets 한 곳뿐이고, 그 버퍼는 직전에 역참조된 Alloc 결과다.
    assert(pMsg != nullptr);

    // 헤더에서 패킷 타입 읽기
    //   [불변식] PeekData는 항상 성공한다 — RECEIVED 이벤트를 만드는 곳은 ParsePackets 한 군데뿐이고
    //   (IOCPServer.cpp의 PushNetworkEvent(RECEIVED)), 거기 닿으려면 totalPacketSize >= sizeof(MsgHeader)
    //   검증과 그 크기만큼의 적재 성공을 모두 통과해야 하므로 _DataSize >= sizeof(MsgHeader)가 보장된다.
    //   ※ 호출을 assert 안에 넣지 말 것 — header를 채우는 부작용이라, 릴리즈에서 식째로 사라지면
    //     header가 스택 잔여값인 채로 아래 switch를 타 엉뚱한 핸들러가 돈다(원래 결함 그 자체).
    MsgHeader header{};
    const int peekedSize = pMsg->PeekData(reinterpret_cast<char*>(&header), sizeof(header));
    assert(peekedSize == static_cast<int>(sizeof(header)));
    (void)peekedSize;   // 릴리즈(NDEBUG) 미사용 경고 억제

    // 타입별 최소 패킷 크기 검증 — 분기 표에서 찾는다(크기와 처리기가 한 줄이라 둘이 어긋날 수 없다 — 게임 서버 나누기 0-④).
    //   등록 안 된 번호·크기 미달은 버리고 오류로 센다(전과 같다).
    const CPacketRoutes::Route* route = _routes.Find(header.type);
    if (route == nullptr || header.size < route->minSize)
    {
        _monitor._packetErrors.Inc();
        pMsg->SubRef();
        return;
    }

    // 입장 요청만 아래 문턱 "앞"에서 가른다 — 이 패킷이 올 때는 아직 플레이어가 없어서,
    //   문턱 뒤에 두면 FindPlayer 가 nullptr 을 돌려 영영 핸들러에 못 닿는다.
    //   나머지 패킷은 지금까지와 똑같이 플레이어를 찾은 뒤에 간다.
    //   (표에서는 「찾기 전 처리기」가 있는 줄 — 지금은 USE_LOGIN 의 입장 요청 하나뿐이다. RegisterRoutes)
    if (route->onSession)
    {
        route->onSession(sessionId, pMsg);
        pMsg->SubRef();
        return;
    }

    // 경계 계층: sessionId → CPlayer* 변환은 여기 한 번뿐 — 이후 컨텐츠 로직은 세션 ID 를 다시 쓰지 않는다.
    //   핸들러(RecvXxx)는 CPlayer* 를 받고, 브로드캐스트·섹터 쪽은 CCharacter* 로 받는다(몬스터도 같은 길).
    CPlayer* player = _sessionHub.FindPlayer(sessionId);
    if (player == nullptr)
    {
        pMsg->SubRef();
        return;
    }

    // 처리기가 없는 줄 — C2S_HEARTBEAT 등 — 게임 로직 처리 불필요 (타임아웃 갱신은 IOCPServer 수신 시점에서 처리)
    if (route->onPlayer)
        route->onPlayer(player, pMsg);

    // SerialBuffer 해제 (소유권 1 반환 — RefCount=0이면 프리리스트로)
    pMsg->SubRef();
}

// 받은 패킷의 분기 표 — 번호마다 최소 크기와 처리기를 한 줄로(전에는 크기 표 GetExpectedSize 의 switch 와 OnReceived 의 switch 가 따로였다 — 게임 서버 나누기 0-④).
//   크기 줄을 빠뜨려 패킷이 조용히 버려지던 함정(아래 줄마다 남긴 옛 주석)은 처리기와 크기가 한 줄이 되며 구조로 막힌다.
//   토글이 꺼진 컨텐츠는 줄 자체가 없다 — 그 번호는 등록 안 된 번호로 버려지고 _packetErrors 가 오른다(전과 같다).
bool CGameServer::RegisterRoutes()
{
    bool ok = true;
    // 이동 시작 · 멈춤 — 이동 조각이 자기 줄을 등록한다(CMoveHandler::RegisterRoutes · 게임 서버 나누기 11 단계)
    ok &= _moveHandler.RegisterRoutes(_routes);
    // 채팅 · 운영자 로그인 — 채팅·운영 조각이 자기 줄을 등록한다(CChatHandler::RegisterRoutes · 게임 서버 나누기 13 단계)
    ok &= _chatHandler.RegisterRoutes(_routes);
    // 존 이동 · 포털 · 입장 요청(USE_LOGIN — 끄면 크기만) — 입장 조각이 자기 줄을 등록한다(CEntryHandler::RegisterRoutes · 게임 서버 나누기 12 단계)
    ok &= _entryHandler.RegisterRoutes(_routes);
    ok &= _routes.Size(MsgType::C2S_HEARTBEAT, sizeof(MSG_C2S_HEARTBEAT));   // 처리기 없음 — 타임아웃 갱신은 IOCPServer 수신 시점
#if USE_COMBAT
    // 공격 · 스킬(USE_SKILL) — 전투 조각이 자기 줄을 등록한다(CCombatHandler::RegisterRoutes · 게임 서버 나누기 9 단계)
    ok &= _combatHandler.RegisterRoutes(_routes);
#endif
#if USE_INVENTORY
    // 물약 · 끼우기·빼기(USE_EQUIP) · 줍기(USE_DROP) — 가방·땅 조각이 자기 줄을 등록한다(CItemHandler::RegisterRoutes · 게임 서버 나누기 6 단계)
    ok &= _itemHandler.RegisterRoutes(_routes);
#endif
#if USE_PARTY
    // 파티 요청 여섯 — 파티·방 조각이 자기 줄을 등록한다(CPartyHandler::RegisterRoutes · 게임 서버 나누기 7 단계)
    ok &= _partyHandler.RegisterRoutes(_routes);
#endif
    return ok;
}

// ==========================================================================
// 패킷 핸들러
// ==========================================================================

// 이동 시작·멈춤(RecvMoveStart · RecvMoveStop) 은 이동 조각으로 옮겼다 — CMoveHandler (게임 서버 나누기 11 단계)

// 채팅(RecvChat) 은 채팅·운영 조각으로 옮겼다 — CChatHandler (게임 서버 나누기 13 단계)

// 채팅 명령(HandleChatCommand · RunGmCommand · RunItemCommand — USE_INVENTORY) · 낱말 나누기(SplitCommandWords) · 속도 알림 조립(MakeActorSpeed) 은
//   채팅·운영 조각으로 옮겼다 — CChatHandler (게임 서버 나누기 13 단계)

// 바뀐 칸 내보내기(FlushInventory) 는 가방·땅 조각으로 옮겼다 — CItemHandler (게임 서버 나누기 6 단계)

// 불러온 가방 줄 넣기(LoadInventoryRows) 는 캐릭터 저장소 조각으로 옮겼다 — CCharacterStore (게임 서버 나누기 4 단계)

// 가방 전체(SendInventory) · 알림 한 줄(SendSystemMessage) 은 본인 알림 조각으로 옮겼다 — CSelfNotifier (게임 서버 나누기 2 단계)

// 물약 쓰기(RecvUseItem) · 끼우기·빼기(RecvEquip · RecvUnequip · ApplyEquip) 는 가방·땅 조각으로 옮겼다 — CItemHandler (게임 서버 나누기 6 단계)

// ==========================================================================
// 패킷 전송 추상화
// ==========================================================================

// ==========================================================================
// 계층 경계 헬퍼
// ==========================================================================

// 캐릭터 번호 발급(AllocPlayerId)은 세션 허브로 옮겼다 — CSessionHub::AllocActorId (게임 서버 나누기 1 단계)

// 표시 문자 · 색 번호(CalcDisplayChar · CalcColorIndex) 는 입장 조각으로 옮겼다 — CEntryHandler (게임 서버 나누기 12 단계)

// ── 패킷 전송 추상화 ──
// 단일 플레이어에게 패킷 전송(SendPacket)은 세션 허브로 옮겼다 — CSessionHub::SendPacket (게임 서버 나누기 1 단계)

// 주변 방송(BroadcastAroundSector) · 이동 알림 셋(NotifyMoveStart/Stop/Sync) · 존 알림(IZoneEvents) 은 시야 방송 조각으로 옮겼다 — CSightBroadcaster (게임 서버 나누기 3 단계)

// 존 정보 · 내 캐릭터 생성 보내기(SendZoneInfo · SendCreateMyPlayer) 는 배치 조각으로 옮겼다 — CPlayerPlacement (게임 서버 나누기 5 단계)

// 남의 캐릭터 생성·삭제 보내기 · 인바운드 멤버십 묶음 · 내 자리(SendSyncPosition) 는 시야 방송 조각으로 옮겼다 — CSightBroadcaster (게임 서버 나누기 3 단계)

// 존 이동 요청(RecvZoneChange) · 포털 요청(RecvPortalUse) 은 입장 조각으로 옮겼다 — CEntryHandler (게임 서버 나누기 12 단계)

// 방 입장(EnterRoom) · 거절 사유 → 와이어(SendRoomReject) 는 파티·방 조각으로 옮겼다 — CPartyHandler (게임 서버 나누기 7 단계)

// 존에서 존으로 옮기기(TransferPlayer) 는 배치 조각으로 옮겼다 — CPlayerPlacement (게임 서버 나누기 5 단계)

// 운영자 인증(고정 열쇠 ADMIN_KEY · RecvAdminLogin) 은 채팅·운영 조각으로 옮겼다 — CChatHandler (게임 서버 나누기 13 단계)

// 전투(공격·스킬 요청 · 공격·시전·효과 알림 · 부활 대기열 · 스킬 거절 사유)는 전투 조각으로 옮겼다 — CCombatHandler (게임 서버 나누기 9 단계)

// 몬스터 경험치 주기(GrantMonsterExp) 는 처치 보상 조각으로 옮겼다 — CRewardHandler (게임 서버 나누기 8 단계)

// 방 보상(RewardRoom) 은 처치 보상 조각으로 옮겼다 — CRewardHandler (게임 서버 나누기 8 단계)

// 마력 알림(SendActorMp) · 남은 쿨타임(SendActiveSkillCooldowns) 은 본인 알림 조각으로 옮겼다 — CSelfNotifier (게임 서버 나누기 2 단계)

// 스탯·경험치 보내기(SendMyStats · SendExp) 는 본인 알림 조각으로 옮겼다 — CSelfNotifier (게임 서버 나누기 2 단계)

// 효과 칸 보내기(SendActorBuff · SendActiveBuffs) 는 본인 알림 조각으로 옮겼다 — CSelfNotifier (게임 서버 나누기 2 단계)

// 파티 요청 여섯(RecvParty*) · 주변 9섹터에서 찾기(FindPlayerAround) · 파티 알림(IPartyEvents) 은 파티·방 조각으로 옮겼다
//   — CPartyHandler (게임 서버 나누기 7 단계)

// 몬스터 틱(TickMonsters) · 관리자에 넘길 묶음(MakeMonsterContext) · 몬스터 알림(IMonsterEvents — 번호 발급 · 등장 · 사라짐) 은
//   몬스터 조각으로 옮겼다 — CMonsterHandler (게임 서버 나누기 10 단계)

// 드랍 굴림(DropLoot) 은 처치 보상 조각으로 옮겼다 — CRewardHandler (게임 서버 나누기 8 단계)
// 땅 아이템 관리자에 넘길 묶음(MakeGroundItemContext) 은 가방·땅 조각으로 옮겼다 — CItemHandler (게임 서버 나누기 6 단계)

// 땅 아이템 한 사람에게 보이기·지우기(SendGroundItemCreate · SendGroundItemDelete) 는 시야 방송 조각으로 옮겼다 — CSightBroadcaster (게임 서버 나누기 3 단계)

// 땅 아이템 알림(IGroundItemEvents — 번호 발급 · 떨어짐 · 빠짐) · 수명 훑기(TickGroundItems) · 줍기(RecvPickup) 는 가방·땅 조각으로 옮겼다
//   — CItemHandler (게임 서버 나누기 6 단계)

// 존 이동 성공·실패 보내기(SendZoneChangeOk · SendZoneChangeFail) 는 배치 조각으로 옮겼다 — CPlayerPlacement (게임 서버 나누기 5 단계)

// 좌표 수용 예산(ConsumeMoveBudget) 은 이동 조각으로 옮겼다 — CMoveHandler (게임 서버 나누기 11 단계)

// 같은 틱 이동자에게 땅 아이템 DELETE(SendGroundItemDeleteToLateMovers) 는 시야 방송 조각으로 옮겼다 — CSightBroadcaster (게임 서버 나누기 3 단계)

// 도착 차례(NotifyArrival — 존 정보 → 내 캐릭터 → 본인 상태 → 주변과 서로 생성) 는 배치 조각으로 옮겼다 — CPlayerPlacement (게임 서버 나누기 5 단계)

// 존 입장·등장·퇴장 방송(BroadcastEnterZone · BroadcastSpawn · BroadcastLeaveZone) 은 시야 방송 조각으로 옮겼다 — CSightBroadcaster (게임 서버 나누기 3 단계)

// 존에서 빼기(RemoveFromZone) 는 배치 조각으로 옮겼다 — CPlayerPlacement (게임 서버 나누기 5 단계)

// ==========================================================================
// 같은 틱 이동자 쌍 보정 · 섹터 변경 방송 · 멤버십 팬아웃·직송 · 섹터 묶음 · 수신섹터 digest 는 시야 방송 조각으로 옮겼다
//   — CSightBroadcaster (SightBroadcaster.cpp · 게임 서버 나누기 3 단계)
// ==========================================================================
