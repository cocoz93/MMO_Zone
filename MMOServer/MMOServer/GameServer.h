// ==========================================================================
// CGameServer — 서버 메인 컨트롤러
//
// [책임]
//  - 서버 모드에 따라 에코 테스트 / MMO 게임 서버 동작 결정
//  - CIOCPServer로부터 NetworkEvent를 Pop하여 게임 로직 처리
//  - 패킷 파싱 및 타입별 핸들러 호출
//  - 게임 루프 스레드 소유 (고정 프레임 Tick)
//  - CMapManager를 통해 다중 존 관리
//
// [사용 흐름]
//  1. main()에서 CGameServer 생성
//  2. Init(ServerMode, ...) → 모드에 따라 내부 구성
//  3. Start() → 네트워크 시작 + (게임 모드 시 게임 루프 시작)
//  4. Stop() → 게임 루프 종료 + 네트워크 종료
// ==========================================================================
#pragma once

#include <cstdint>
#include <cassert>   // 도달불가 불변식 검증 (릴리즈에서는 사라짐)
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <queue>
#include <unordered_set>
#include <unordered_map>

#include "NetIoModel.h"   // 컴파일타임 I/O 모델 별칭 (IOCP/epoll) — IOCPServer.h를 여기서 흡수
#include <Protocol/Protocol.h>   // 게임 패킷 정의 — 전에는 IOCPServer.h를 통해 딸려왔다
#include "BuildConfig.h"   // USE_COMBAT 등 실험·컨텐츠 토글 — 간접 포함에 기대지 않고 직접 읽는다
#include "MapManager.h"
#include "Zone.h"          // 존 — 존 이동·존 틱에 쓴다(그 알림 IZoneEvents 는 시야 조각이 구현한다 — SightBroadcaster.h)
#include "Player.h"
#include "CombatSystem.h"  // 공격 흐름 — 멤버로 든다(그 알림 ICombatEvents 는 전투 조각이 구현한다 — CombatHandler.h)
#include "StatSystem.h"    // 스탯·레벨 규칙 — 멤버로 들고 부른다
#include "BuffSystem.h"    // 지속 효과 규칙 — 멤버로 들고 부른다(그 알림 IBuffEvents 는 전투 조각이 구현한다)
#include "ItemSystem.h"    // 아이템 쓰기·장착 규칙 — 멤버로 들고 부른다
#include "SkillSystem.h"   // 스킬 규칙 — 멤버로 든다(그 알림 ISkillEvents 는 전투 조각이 구현한다)
#include "GroundItemManager.h" // 땅 아이템 — 여기서는 그 알림(IGroundItemEvents)을 구현한다
#include "PartySystem.h"   // 파티 규칙 — 멤버로 든다(그 알림 IPartyEvents 는 파티·방 조각이 구현한다 — PartyHandler.h)
#include "MonitorManager.h"
#include "GameMonitor.h"   // 게임 컨텐츠 지표 — 코어(CMonitorManager)와 별개 객체
#include "Common.h"
#include "GameDataFiles.h"  // 기획 데이터 파일(맵·레벨·아이템·몬스터·스폰 구역·드랍 표)
#include "DB/DBWorker.h"
#include "PacketRoutes.h"  // 받은 패킷의 분기 표(번호 → 최소 크기 · 처리기) — 게임 서버 나누기 0-④
#include "SessionHub.h"    // 세션 칸 · 플레이어 찾기 · 보내기 · 캐릭터 번호 발급 — 게임 서버 나누기 1 단계(B1)
#include "SelfNotifier.h"  // 본인 알림(내 상태 패킷을 나에게만) — 게임 서버 나누기 2 단계(B5)
#include "SightBroadcaster.h" // 시야 방송(섹터 바뀜 · 주변 방송 · 송신 묶음 · 존 알림) — 게임 서버 나누기 3 단계(B2)
#include "CharacterStore.h"   // 캐릭터 저장소(DB 워커 · 저장 세 길 · 불러오기 — USE_DB_WORKER) — 게임 서버 나누기 4 단계(B3)
#include "PlayerPlacement.h"  // 배치(존에 넣기·옮기기·빼기·떠나보내기 · 도착 차례) — 게임 서버 나누기 5 단계(B4)
#include "ItemHandler.h"      // 가방·땅(물약 · 장착 · 줍기 · 땅 아이템 알림 — USE_INVENTORY) — 게임 서버 나누기 6 단계(C6)
#include "PartyHandler.h"     // 파티·방(파티 요청 · 파티 알림 · 방 입장·거절 · 다시 들어가기 잠금 — USE_PARTY) — 게임 서버 나누기 7 단계(C7)
#include "RewardHandler.h"    // 처치 보상(몬스터 경험치 · 드랍 굴림 · 방 보상 — USE_COMBAT && USE_MONSTER) — 게임 서버 나누기 8 단계(C5)
#include "CombatHandler.h"    // 전투(공격·스킬 요청 · 공격·시전·효과 알림 · 부활 대기열 — USE_COMBAT) — 게임 서버 나누기 9 단계(C3)
#include "MonsterHandler.h"   // 몬스터(몬스터 틱 · 등장·사라짐 알림 · 번호 발급 — USE_MONSTER) — 게임 서버 나누기 10 단계(C4)
#include "MoveHandler.h"      // 이동(이동 시작·멈춤 요청 · 좌표 수용 예산 — 토글 없음) — 게임 서버 나누기 11 단계(C2)
#include "EntryHandler.h"     // 입장·존 이동 요청(접속 · 대기표 · 불러온 캐릭터 세우기 · 중복 접속 · 포털) — 게임 서버 나누기 12 단계(C1)
#include "ChatHandler.h"      // 채팅·운영(채팅 · 운영자 로그인 · 채팅 명령 /item · /gm — 명령만 USE_INVENTORY) — 게임 서버 나누기 13 단계(C8)

class CSerialBuffer;

// 알림 통로 하나 — 꽂는 것은 이 클래스 자신뿐이라 비공개 상속.
//   (IZoneEvents — 존의 걷기·멈춤 알림 둘 — 은 시야 조각 CSightBroadcaster 가 구현한다 · 게임 서버 나누기 3 단계)
//   (IGroundItemEvents — 땅 아이템 관리자의 알림 둘과 번호 부탁 하나 — 는 가방·땅 조각 CItemHandler 가 구현한다 · 게임 서버 나누기 6 단계)
//   (IPartyEvents — 파티 규칙의 알림 다섯 — 은 파티·방 조각 CPartyHandler 가 구현한다 · 게임 서버 나누기 7 단계)
//   (ICombatEvents · IBuffEvents · ISkillEvents — 공격 흐름 · 지속 효과 · 스킬 규칙의 알림 — 은 전투 조각 CCombatHandler 가 구현한다 · 게임 서버 나누기 9 단계)
//   (IMonsterEvents — 몬스터 관리자의 알림 둘(등장·사라짐)과 번호 부탁 하나 — 는 몬스터 조각 CMonsterHandler 가 구현한다 · 게임 서버 나누기 10 단계)
//   (ICharacterLoadEvents — 캐릭터 저장소의 불러오기 결과 하나 — 는 입장 조각 CEntryHandler 가 구현한다 · 게임 서버 나누기 12 단계)
//   IPlacementEvents  = 배치(CPlayerPlacement)의 알림 둘(존에서 빠짐 · 떠남 — 틱 너머 목록 정리) — 게임 서버 나누기 5 단계(0-③ ⑴⑶) · 토글 없이 늘 있다
// 기반은 늘 있는 통로(IPlacementEvents) 하나뿐이다 — 토글마다 「, 」로 붙던 통로는 전부 조각으로 갔다(게임 서버 나누기 3~12 단계).
//   (3·4 단계 동안은 늘 있던 IZoneEvents 가 빠져 첫 통로를 고르는 #if 사다리를 뒀었다 — 5 단계에서 늘 있는 통로가 생겨 걷어냄)
class CGameServer
    : private IPlacementEvents
{
public:
    CGameServer(CMonitorManager& monitor, CGameMonitor& gameMonitor);
    ~CGameServer();

    // 에코 테스트 모드 초기화
    bool Init(ServerMode mode, int port, int maxClients);

    // 게임 서버 모드 초기화 (맵 설정 배열)
    bool Init(ServerMode mode, int port, int maxClients,
              const MapConfig* maps, int32_t mapCount, int workerThreads = 0, int sendWorkers = 0,
              int rioWorkers = 0, int completionBatch = 0, int sendDepth = 1);

    // DB 저장 파이프라인 초기화 (USE_DB_WORKER=0이면 no-op). Start() 전에 호출. 몸은 캐릭터 저장소(CCharacterStore::Init).
    bool InitDB(const DBConfig& dbConfig, int savePeriodSec, int workerCount, int queueMax);

    // 이벤트 큐 과부하 시 신규 accept 차단 임계 (0=끄기). Init() 뒤, Start() 전에 호출.
    void SetEventQueueAcceptLimit(int limit)
    {
        if (_network)
            _network->SetEventQueueAcceptLimit(limit);
    }

    // 플레이어 이동 속도(칸/초) — 0 이면 지금까지의 동작(25~50 무작위). ServerConfig [Game] 참조.
    //   Start() 전에 호출. 새로 만드는 캐릭터부터 적용된다. 몬스터 속도는 몬스터 표(MonsterTable.csv)에 있다.
    void SetPlayerSpeed(int playerSpeed)
    {
        _entryHandler.SetPlayerSpeed(playerSpeed);   // 새로 만드는 캐릭터에 쓰는 값이라 입장 조각이 든다(게임 서버 나누기 12 단계)
    }

    // 기획 데이터 파일(맵 표·레벨 표·아이템 표·몬스터 표·스폰 구역 표·드랍 표)을 읽는다 — 기동 때 한 번, 세계(맵·존)를 만드는 Init "앞"에서 부른다.
    //   표는 존이 생기기 전에 서 있어야 한다(Init 의 맵 등록이 기본 채널을 만든다 — 10-08).
    //   실패하면 false 이고 main 이 서버를 안 띄운다. dir 은 끝에 경로 구분자가 붙은 디렉토리.
    //   표 밖 사실과의 대조도 여기서 — 몬스터 속도는 AI 판단 주기의 상한(MonsterAI::MAX_SPEED)과 견준다.
    //   스폰 구역과 맵 크기의 대조는 표끼리라 읽을 때 끝난다(CSpawnRegionTable::Load). 10-09 전에는 맵 크기가 INI 에 있어 여기서 INI 맵 설정을 받아 대조했다.
    bool LoadGameDataFiles(const std::string& dir);

    // 읽은 기획 데이터 — main 이 맵 표의 줄과 INI 운영 값을 엮어 맵 설정(MapConfig)을 만들 때 읽는다.
    const CGameDataFiles& GameData() const { return _gameDataFiles; }

    // 경험치·스탯 보내기(SendExp · SendMyStats)는 본인 알림 조각으로 옮겼다 — _selfNotifier (SelfNotifier.h · 게임 서버 나누기 2 단계)

    // 서버 시작/종료
    bool Start();
    void Stop();

private:
    // 게임 루프 (별도 스레드, GameServer 모드 전용)
    void GameLoopThread();

    // NetworkEvent 디스패치
    void ProcessNetworkEvents();
    void OnDisconnected(int64_t sessionId);
    void OnReceived(int64_t sessionId, CSerialBuffer* pMsg);

    // 입장 배치 구조체(EnterPlacement) · 스폰(SpawnPlayer) · 접속(OnConnected)은 입장 조각 — _entryHandler (EntryHandler.h · 게임 서버 나누기 12 단계)

    // 떠나보내기(LeaveGame) · 놓기(ReleasePlayer)는 배치 조각 — _placement (PlayerPlacement.h · 게임 서버 나누기 5 단계).
    //   그 안에서 틱 너머 목록을 정리하라는 알림이 아래 둘로 온다.

    // 「존에서 빠짐 · 떠남」 알림 (IPlacementEvents) — 뜻은 PlayerPlacement.h. 몸은 옮기기 전 RemoveFromZone · ReleasePlayer 의 그 자리 그대로.
    void OnLeavingZone(CCharacter* player) override;   // 부활 대기열에서 빼기 (전투)
    void OnLeavingGame(CPlayer* player) override;      // 부활 대기열 · 지속 효과 목록 · 파티원·요청 표에서 빼기 (전투 · 파티)

    // 「존 지우기 직전」 알림 — 빈 채널 정리(CMapManager::CleanupEmptyChannels)가 지울 존마다 부른다. 계약은 MapManager.h.
    void OnErasingZone(CZone* zone);                   // 땅 아이템 세기 · 지속 효과 목록에서 빼기 · 방 닫힘 세기 (땅 · 전투 · 파티)

    // 패킷 핸들러
    // 이동 시작·멈춤(RecvMoveStart · RecvMoveStop)은 이동 조각 — _moveHandler (MoveHandler.h · 게임 서버 나누기 11 단계)
    // 채팅(RecvChat) · 운영자 로그인(RecvAdminLogin)은 채팅·운영 조각 — _chatHandler (ChatHandler.h · 게임 서버 나누기 13 단계)
    // 존 이동 · 포털 요청(RecvZoneChange · RecvPortalUse)은 입장 조각 — _entryHandler (게임 서버 나누기 12 단계)

    // 공격·스킬 요청 · 공격·시전·효과 알림(ICombatEvents · ISkillEvents · IBuffEvents) · 부활 대기열은 전투 조각 — _combatHandler (CombatHandler.h · 게임 서버 나누기 9 단계).
    //   부활 자리 규칙(RespawnsToTown)은 Combat.h 의 순수 함수 — 전투 조각의 부활 틱과 입장(OnCharacterLoaded)이 같이 쓴다.
    // 효과 칸 보내기(SendActorBuff · SendActiveBuffs)는 본인 알림 조각으로 옮겼다 — _selfNotifier (게임 서버 나누기 2 단계)
    // 몬스터 경험치 주기(GrantMonsterExp) · 방 보상(RewardRoom)은 처치 보상 조각 — _rewardHandler (RewardHandler.h · 게임 서버 나누기 8 단계)

    // 채팅 명령(HandleChatCommand · RunItemCommand · RunGmCommand — USE_INVENTORY)은 채팅·운영 조각 — _chatHandler (ChatHandler.h · 게임 서버 나누기 13 단계)

    // 바뀐 칸 내보내기(FlushInventory) · 물약 쓰기 · 끼우기·빼기는 가방·땅 조각 — _itemHandler (ItemHandler.h · 게임 서버 나누기 6 단계)
    // 가방 전체(SendInventory) · 알림 한 줄(SendSystemMessage)은 본인 알림 조각으로 옮겼다 — _selfNotifier (게임 서버 나누기 2 단계)
    // 불러온 가방 줄 넣기(LoadInventoryRows)는 캐릭터 저장소 조각 — CharacterStore.h (게임 서버 나누기 4 단계)

    // 마력(SendActorMp) · 남은 쿨타임(SendActiveSkillCooldowns)은 본인 알림 조각으로 옮겼다 — _selfNotifier (게임 서버 나누기 2 단계)

    // 파티 요청 · 파티 알림(IPartyEvents) · 방 입장·거절 · 다시 들어가기 잠금은 파티·방 조각 — _partyHandler (PartyHandler.h · 게임 서버 나누기 7 단계)

    // 몬스터 틱 · 관리자에 넘길 묶음 · 몬스터 알림(IMonsterEvents)은 몬스터 조각 — _monsterHandler (MonsterHandler.h · 게임 서버 나누기 10 단계)

    // 드랍 굴림(DropLoot)은 처치 보상 조각 — _rewardHandler (RewardHandler.h · 게임 서버 나누기 8 단계) ·
    //   관리자에 넘길 묶음 · 땅 아이템 알림 · 수명 훑기 틱 · 줍기는 가방·땅 조각 — _itemHandler (ItemHandler.h · 게임 서버 나누기 6 단계)

#if USE_DB_WORKER
    // DB 저장 파이프라인(저장 세 길 · 저장 대상 판정 · 쿨타임·버프 묶기·되살리기)과 불러오기 요청·회신 꺼내기는
    //   캐릭터 저장소 조각 — _characterStore (CharacterStore.h · 게임 서버 나누기 4 단계).
    // 불러오기 결과 알림(ICharacterLoadEvents) · 입장 대기표(EnterState · PendingEnter · 바꾸는 함수 넷)는 입장 조각 — _entryHandler (게임 서버 나누기 12 단계)
#endif

    // 입장 요청(RecvEnterGame) · 같은 계정 걷어내기(KickDuplicateAccount) · 지정 계정 번호 하한(ACCOUNT_ID_MIN — USE_LOGIN)은 입장 조각 — _entryHandler (게임 서버 나누기 12 단계)

    // 섹터 변경 처리(ProcessSectorChange)·같은 틱 쌍 보정(SendSightPairFixes)은 시야 조각으로 옮겼다 — _sightBroadcaster (게임 서버 나누기 3 단계)

    // 도착 종류(ArrivalKind) · 도착 차례(NotifyArrival) · 존에서 존으로 옮기기(TransferPlayer · SpawnAt) · 존에서 빼기(RemoveFromZone)는
    //   배치 조각 — _placement (PlayerPlacement.h · 게임 서버 나누기 5 단계).
    // 존 입장/퇴장 브로드캐스트(BroadcastEnterZone · BroadcastSpawn · BroadcastLeaveZone)는 시야 조각 — SightBroadcaster.h (게임 서버 나누기 3 단계)

    // 표시 속성(CalcDisplayChar · CalcColorIndex)은 입장 조각 — _entryHandler (게임 서버 나누기 12 단계) · 캐릭터 번호 발급은 세션 허브(CSessionHub::AllocActorId)

    // ── 패킷 전송 ──
    // 한 사람에게 보내기는 세션 허브(_sessionHub.SendPacket — SessionHub.h). 주변에 보내기 · 이동 알림 · 존 알림(IZoneEvents) ·
    //   섹터 묶음 · 수신섹터 digest · 멤버십 팬아웃·직송 · 남의 캐릭터 생성·삭제 · 내 자리(SendSyncPosition)는 시야 조각
    //   (_sightBroadcaster — SightBroadcaster.h · 게임 서버 나누기 3 단계).

    // 패킷별 전송 함수(존 정보 · 내 캐릭터 생성 · 존 이동 성공·실패)는 배치 조각 — _placement (게임 서버 나누기 5 단계)

    // 좌표 수용 예산 검사(ConsumeMoveBudget)는 이동 조각 — _moveHandler (게임 서버 나누기 11 단계)

    // 받은 패킷의 분기 표를 채운다 — 패킷 번호마다 최소 크기와 처리기 한 줄(전에는 크기 표 GetExpectedSize 와 분기 switch 가 따로였다).
    //   Init 이 맨 앞에서 부른다. 등록이 실패하면(범위 밖 · 두 번 등록) false — 기동 거절.
    bool RegisterRoutes();

private:
    ServerMode _mode;

    CPacketRoutes _routes;   // 받은 패킷의 분기 표 — RegisterRoutes 가 채우고 OnReceived 가 찾는다

    CMonitorManager& _monitor;       // 코어 지표 — 배치 송신 경로에서 코어 대신 세는 자리가 있어 게임도 참조한다
    CGameMonitor&    _gameMonitor;   // 게임 컨텐츠 지표 (전투)
    std::unique_ptr<NetIoModel> _network;

    // 세션 칸 · 플레이어 찾기 · 보내기 · 캐릭터 번호 발급 — 플레이어의 주인이 이 안의 칸이다(SessionHub.h).
    //   ★ _monitor 보다 뒤에 선언 — 그 참조를 받는다(멤버는 선언 순서로 세워진다). 네트워크는 Init 에서 포인터로 넘긴다.
    CSessionHub _sessionHub{ _monitor };

    // 기획 데이터 — 기동 때 한 번 읽고 그 뒤로는 읽기만 한다(게임 스레드가 매 전투·명령에서 본다).
    //   ★ _mapManager 보다 앞에 선언 — 맵 관리자가 이것을 받아 존에 건넨다(멤버는 선언 순서로 세워진다).
    CGameDataFiles _gameDataFiles;
    CMapManager _mapManager{ _gameDataFiles };

    std::thread _gameThread;
    std::atomic<bool> _running;
    // 틱 주기(FRAME_PER_SEC · FRAME_INTERVAL_MS)는 BuildConfig.h — 아래 층 헤더도 같이 쓰는 단위라 여기 두지 않는다.
    static constexpr int CLEANUP_INTERVAL_FRAMES = 30 * FRAME_PER_SEC;  // 30초마다 빈 채널 정리
    // 주기적 위치 동기화 상수 둘(SYNC_*)은 시야 조각 — SightBroadcaster.h (게임 서버 나누기 3 단계)
    // 좌표 수용 한도·예산 상수 셋(MOVE_START_ACCEPT_DIST_SQ · MOVE_BUDGET_CAP · MOVE_BUDGET_SLACK)은 이동 조각 — MoveHandler.h (게임 서버 나누기 11 단계)

    // 기본 접속 맵(_defaultMapId — 맵 표 첫 줄 = 마을)은 맵 관리자 — CMapManager::GetDefaultMapId (게임 서버 나누기 14 단계)

    // 땅의 아이템 번호(_nextGroundId) · 수명 훑기 세기(_groundItemScanCounter)는 가방·땅 조각 — ItemHandler.h (게임 서버 나누기 6 단계)
    // 굴림 결과 버퍼(_lootBuffer) · 방 보상 넘침(_roomOverflow)은 처치 보상 조각 — RewardHandler.h (게임 서버 나누기 8 단계)

#if USE_COMBAT
    // 스탯·레벨 규칙 — 표(_gameDataFiles)를 참조한다.
    //   ★ _gameDataFiles·_gameMonitor 보다 뒤에 선언해야 한다(멤버는 선언 순서로 세워진다 — _combatSystem 주석과 같은 까닭).
    CStatSystem _statSystem{ _gameDataFiles, _gameMonitor };

    // 지속 효과 규칙 — 알림은 전투 조각(_combatHandler · IBuffEvents)으로 간다. 바뀌면 _statSystem 으로 스탯을 다시 세운다(그래서 그 뒤에 선언).
    //   거는 쪽은 _skillSystem(아래 — 그 참조를 받으므로 이보다 뒤) · 아이템이 걸게 되면 _itemSystem 도 받는다.
    //   알림 받는 _combatHandler 는 한참 뒤에 선언된다(그 조각이 이 규칙의 참조를 받는다) — 참조를 묶기만 하고 생성 중엔 안 부른다.
    CBuffSystem _buffSystem{ _gameMonitor, _statSystem, _combatHandler };
#endif

#if USE_INVENTORY
    // 아이템 쓰기·장착 규칙 — 장착 뒤 공격력은 _statSystem 으로 세운다(그래서 그 뒤에 선언).
    CItemSystem _itemSystem{ _gameDataFiles, _gameMonitor, _statSystem };
#endif

    // 프레임 진입 시점 이벤트를 스왑해 받는 로컬 큐 (멤버 재사용 → deque 블록 보존으로 매 프레임 재할당 방지)
    std::queue<NetworkEvent> _localEvents;
    int _cleanupFrameCount = 0;
    uint64_t _frameCount = 0;   // 단조 증가 프레임 번호 (이동 예산의 경과 시간 산출용)

    // 본인 알림 — 그 사람 자신의 상태를 그 사람에게만(SelfNotifier.h). 틱 번호는 읽기만 하도록 빌려 준다(남은 시간 셈의 「지금」).
    //   ★ _sessionHub · _gameMonitor · _gameDataFiles · _frameCount 보다 뒤에 선언 — 그 참조를 받는다(멤버는 선언 순서로 세워진다).
    CSelfNotifier _selfNotifier{ _sessionHub, _gameMonitor, _gameDataFiles, _frameCount };

    // 시야 방송 — 섹터 바뀜 · 주변 방송 · 틱 지연 송신 묶음 · 땅 아이템 보이기 · 존 알림(IZoneEvents — 존 틱·몬스터 묶음에 이것을 넘긴다)
    //   (SightBroadcaster.h). 주기 동기화 세기 · 프레임 재사용 버퍼 셋 · 섹터 아웃박스 · 방송 계측 다섯이 이 안에 있다.
    //   ★ _sessionHub · _mapManager · _monitor · _gameMonitor 보다 뒤에 선언 — 그 참조를 받는다(멤버는 선언 순서로 세워진다).
    CSightBroadcaster _sightBroadcaster{ _sessionHub, _mapManager, _monitor, _gameMonitor };

#if USE_DB_WORKER
    // 캐릭터 저장소 — DB 워커의 주인 · 저장 세 길 · 불러오기 요청과 회신 꺼내기(CharacterStore.h). 불러오기 결과는 입장 조각(_entryHandler · ICharacterLoadEvents)으로 간다.
    //   ★ _sessionHub · _gameMonitor · _gameDataFiles · _buffSystem · _frameCount 보다 뒤에 선언 — 그 참조를 받는다(멤버는 선언 순서로 세워진다).
    //     _entryHandler 는 한참 뒤에 선언된다 — 참조만 넘긴다(생성 중엔 부르지 않는다 — 규칙 셋이 전투 조각을 받는 것과 같은 규칙).
    CCharacterStore _characterStore{ _sessionHub, _gameMonitor, _gameDataFiles,
#if USE_COMBAT
                                     _buffSystem,
#endif
                                     _frameCount, _entryHandler };
#endif

    // 배치 — 존에 넣기·옮기기·빼기·떠나보내기 · 도착 차례(PlayerPlacement.h). 틱 너머 목록 정리는 이 클래스(IPlacementEvents)로 알려 온다.
    //   ★ _sessionHub · _selfNotifier · _sightBroadcaster · _characterStore · _mapManager · _gameMonitor 보다 뒤에 선언 — 그 참조를 받는다.
    //     *this 는 참조만 넘긴다(생성 중엔 부르지 않는다).
    CPlayerPlacement _placement{ _sessionHub, _selfNotifier, _sightBroadcaster,
#if USE_DB_WORKER
                                 _characterStore,
#endif
                                 _mapManager, _gameMonitor, *this };

#if USE_INVENTORY
    // 가방·땅 — 물약·장착·줍기 요청 · 바뀐 칸 내보내기 · 땅 아이템 알림(IGroundItemEvents) · 수명 훑기(ItemHandler.h).
    //   ★ _sessionHub · _selfNotifier · _sightBroadcaster · _characterStore · _itemSystem · _mapManager · _gameMonitor · _gameDataFiles · _frameCount
    //     보다 뒤에 선언 — 그 참조를 받는다(멤버는 선언 순서로 세워진다).
    CItemHandler _itemHandler{ _sessionHub, _selfNotifier, _sightBroadcaster,
#if USE_DB_WORKER
                               _characterStore,
#endif
                               _itemSystem, _mapManager, _gameMonitor, _gameDataFiles, _frameCount };
#endif

    // 플레이어 이동 속도 덮어쓰기(_playerSpeed)는 입장 조각 — EntryHandler.h (게임 서버 나누기 12 단계)
#if USE_COMBAT
    // 공격 후보 버퍼는 두지 않는다 — 사거리가 걸치는 섹터의 주민 목록을 참조로 훑어 복사가 없다.
    //   (처음엔 GetAroundPlayers 의 전신이 벡터를 요구해 _attackTargetBuffer 를 뒀었다)

    // 죽어서 부활을 기다리는 플레이어(_deadActors) · 마을 부활로 옮길 버퍼(_respawnTransferBuffer)는 전투 조각 — CombatHandler.h (게임 서버 나누기 9 단계)

    // 공격 흐름 — 알림은 전투 조각(_combatHandler · ICombatEvents)으로 간다.
    //   ★ _gameMonitor 보다 뒤에 선언해야 한다 — 멤버는 선언 순서로 세워지므로, 앞에 두면 아직 안 묶인 참조를 넘긴다.
    //   알림 받는 _combatHandler 는 거꾸로 이 뒤다(그 조각이 이 규칙의 참조를 받는다) — 참조를 묶기만 하고 생성 중엔 안 부른다.
    CCombatSystem _combatSystem{ _gameMonitor, _combatHandler };
#endif

#if USE_SKILL
    // 스킬 규칙 — 알림은 전투 조각(_combatHandler · ISkillEvents)으로 간다.
    //   ★ _combatSystem·_buffSystem 뒤에 선언해야 한다 — 그 참조(대상 찾기·맞힌 뒤 처리 · 지속 효과 걸기)를 받는다. 멤버는 선언 순서로
    //     세워지므로 앞에 두면 아직 안 만들어진 객체의 참조를 넘긴다(위 _combatSystem 주석과 같은 까닭).
    CSkillSystem _skillSystem{ _gameDataFiles, _gameMonitor, _combatSystem, _buffSystem, _combatHandler };
#endif

#if USE_PARTY
    // 파티·방 — 파티 요청 · 파티 알림(IPartyEvents) · 방 입장·거절 · 다시 들어가기 잠금 표(PartyHandler.h).
    //   ★ _sessionHub · _placement · _mapManager · _gameMonitor · _frameCount 보다 뒤에 선언 — 그 참조를 받는다(멤버는 선언 순서로 세워진다).
    //   _partySystem 만 거꾸로 이 뒤다 — 그 알림 받는 쪽이 이 조각이라 이것이 먼저 세워져야 한다. 조각은 그 참조를 묶기만 하고 생성 중엔 안 쓴다.
    CPartyHandler _partyHandler{ _sessionHub, _placement, _partySystem, _mapManager, _gameMonitor, _frameCount };

    // 파티 규칙 — 알림은 파티·방 조각(_partyHandler)으로 간다. 파티원·요청의 주소를 틱 너머로 들므로 놓기의 「떠남」 알림(OnLeavingGame)이 Forget 을 부른다.
    //   ★ _gameMonitor · _partyHandler 보다 뒤에 선언해야 한다(위 _combatSystem 주석과 같은 까닭).
    CPartySystem _partySystem{ _gameMonitor, _partyHandler };

    // 방 다시 들어가기 잠금 표(_roomLocks)는 파티·방 조각 — PartyHandler.h (게임 서버 나누기 7 단계)
#endif

#if USE_COMBAT && USE_MONSTER
    // 처치 보상 — 몬스터 경험치 · 드랍 굴림 · 방 보상(RewardHandler.h). 처치 알림(OnDeath)이 부른다.
    //   ★ _selfNotifier · _sightBroadcaster · _characterStore · _itemHandler · _partyHandler · _statSystem · _gameMonitor · _gameDataFiles · _frameCount
    //     보다 뒤에 선언 — 그 참조를 받는다(멤버는 선언 순서로 세워진다).
    CRewardHandler _rewardHandler{ _selfNotifier, _sightBroadcaster,
#if USE_DB_WORKER
                                   _characterStore,
#endif
#if USE_DROP
                                   _itemHandler,
#endif
#if USE_PARTY
                                   _partyHandler,
#endif
                                   _statSystem, _gameMonitor, _gameDataFiles, _frameCount };
#endif

#if USE_COMBAT
    // 전투 — 공격·스킬 요청 · 공격·시전·효과 알림(ICombatEvents · ISkillEvents · IBuffEvents) · 부활 대기열(CombatHandler.h).
    //   ★ _sessionHub · _selfNotifier · _sightBroadcaster · _placement · _rewardHandler · _combatSystem · _buffSystem · _skillSystem · _mapManager ·
    //     _gameMonitor · _frameCount 보다 뒤에 선언 — 그 참조를 받는다(멤버는 선언 순서로 세워진다).
    //     규칙 셋(_buffSystem · _combatSystem · _skillSystem)은 거꾸로 이것의 참조를 받는다 — 알림 받는 쪽이라 묶기만 하고 생성 중엔 안 부른다.
    CCombatHandler _combatHandler{ _sessionHub, _selfNotifier, _sightBroadcaster, _placement,
#if USE_MONSTER
                                   _rewardHandler,
#endif
                                   _combatSystem, _buffSystem,
#if USE_SKILL
                                   _skillSystem,
#endif
                                   _mapManager, _gameMonitor, _frameCount };
#endif

#if USE_MONSTER
    // 몬스터 — 존마다 몬스터 틱 · 몬스터 알림(IMonsterEvents — 번호 발급 · 등장 · 사라짐)(MonsterHandler.h).
    //   ★ _sessionHub · _sightBroadcaster · _placement · _combatSystem · _buffSystem · _skillSystem · _mapManager · _gameMonitor · _frameCount
    //     보다 뒤에 선언 — 그 참조를 받는다(멤버는 선언 순서로 세워진다). 알림은 틱마다 만드는 묶음에 실려 가므로 거꾸로 묶는 쪽이 없다.
    CMonsterHandler _monsterHandler{ _sessionHub, _sightBroadcaster, _placement,
#if USE_COMBAT
                                     _combatSystem, _buffSystem,
#endif
#if USE_SKILL
                                     _skillSystem,
#endif
                                     _mapManager, _gameMonitor, _frameCount };
#endif

    // 이동 — 이동 시작·멈춤 요청 · 좌표 수용 예산 · 틱 끝 거부 건수(MoveHandler.h).
    //   ★ _sightBroadcaster · _mapManager · _gameMonitor · _frameCount 보다 뒤에 선언 — 그 참조를 받는다(멤버는 선언 순서로 세워진다).
    CMoveHandler _moveHandler{ _sightBroadcaster, _mapManager, _gameMonitor, _frameCount };

    // 입장·존 이동 요청 — 접속 · 대기표 · 불러오기 결과(ICharacterLoadEvents) · 스폰 · 같은 계정 걷어내기 · 존 이동 · 포털(EntryHandler.h).
    //   ★ _sessionHub · _placement · _characterStore · _partyHandler · _statSystem · _mapManager · _gameMonitor · _gameDataFiles
    //     보다 뒤에 선언 — 그 참조를 받는다(멤버는 선언 순서로 세워진다). 저장소(_characterStore)는 거꾸로 이것의 참조를 받는다 — 묶기만 한다.
    CEntryHandler _entryHandler{ _sessionHub, _placement,
#if USE_DB_WORKER
                                 _characterStore,
#endif
#if USE_PARTY
                                 _partyHandler,
#endif
#if USE_COMBAT
                                 _statSystem,
#endif
                                 _mapManager, _gameMonitor, _gameDataFiles };

    // 채팅·운영 — 채팅 · 운영자 로그인 · 채팅 명령(/item · /gm — USE_INVENTORY)(ChatHandler.h).
    //   ★ _sessionHub · _sightBroadcaster · _mapManager 와 명령이 쓰는 _selfNotifier · _itemHandler · _statSystem · _gameMonitor · _gameDataFiles
    //     보다 뒤에 선언 — 그 참조를 받는다(멤버는 선언 순서로 세워진다).
    CChatHandler _chatHandler{ _sessionHub, _sightBroadcaster,
#if USE_INVENTORY
                               _selfNotifier, _itemHandler, _statSystem, _gameMonitor, _gameDataFiles,
#endif
                               _mapManager };

    // 섹터 아웃박스(틱-지연 배칭 상태) · 인바운드 멤버십 수집 버퍼는 시야 조각 — SightBroadcaster.h (게임 서버 나누기 3 단계)

    // 비용종류별 계측(틱 내 누적 → 틱 끝 1회 반영) — 방송 쪽 다섯은 시야 조각 · 이동 예산 거부(_tickMoveBudgetRejects)는 이동 조각(둘 다 ReportTickMetrics)

#if USE_DB_WORKER
    // DB 워커 · 저장 주기 세기 · 회신 받는 버퍼는 캐릭터 저장소 — CharacterStore.h (게임 서버 나누기 4 단계)

    // 대기표(_pendingEnter)와 상태별 수 둘은 입장 조각 — EntryHandler.h (게임 서버 나누기 12 단계)
#endif
};
