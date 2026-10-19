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
#include "Combat.h"        // 전투 판정 (순수 함수 — 송신·존·섹터를 모른다)
#include "CombatSystem.h"  // 공격 흐름 — 여기서는 그 알림(ICombatEvents)을 구현한다
#include "StatSystem.h"    // 스탯·레벨 규칙 — 멤버로 들고 부른다
#include "BuffSystem.h"    // 지속 효과 규칙 — 멤버로 들고 부르고, 그 알림(IBuffEvents)을 구현한다
#include "ItemSystem.h"    // 아이템 쓰기·장착 규칙 — 멤버로 들고 부른다
#include "SkillSystem.h"   // 스킬 규칙 — 멤버로 들고 부르고, 그 알림(ISkillEvents)을 구현한다
#include "MonsterManager.h" // 몬스터 — 여기서는 그 알림(IMonsterEvents)을 구현한다
#include "GroundItemManager.h" // 땅 아이템 — 여기서는 그 알림(IGroundItemEvents)을 구현한다
#include "PartySystem.h"   // 파티 규칙 — 멤버로 들고 부르고, 그 알림(IPartyEvents)을 구현한다
#include "Room.h"          // 파티 전용 방 — 들어가기 판정(CheckRoomEntry)과 다시 들어가기 잠금 표(CRoomLocks)
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

class CSerialBuffer;

// 알림 통로 일곱 — 꽂는 것은 이 클래스 자신뿐이라 비공개 상속.
//   (IZoneEvents — 존의 걷기·멈춤 알림 둘 — 은 시야 조각 CSightBroadcaster 가 구현한다 · 게임 서버 나누기 3 단계)
//   (IGroundItemEvents — 땅 아이템 관리자의 알림 둘과 번호 부탁 하나 — 는 가방·땅 조각 CItemHandler 가 구현한다 · 게임 서버 나누기 6 단계)
//   IPlacementEvents  = 배치(CPlayerPlacement)의 알림 둘(존에서 빠짐 · 떠남 — 틱 너머 목록 정리) — 게임 서버 나누기 5 단계(0-③ ⑴⑶) · 토글 없이 늘 있다
//   ICombatEvents     = 공격 흐름(CCombatSystem)의 알림 셋 — 스킬의 피해·죽음도 이 셋으로 온다(ApplyHit)
//   IBuffEvents       = 지속 효과 규칙(CBuffSystem)의 알림 하나(효과가 바뀜 — 걸림·되감김·만료·죽음) · 전투 토글 소유
//   IMonsterEvents    = 몬스터 관리자(CMonsterManager)의 알림 둘(등장·사라짐)과 번호 부탁 하나
//   ISkillEvents      = 스킬 규칙(CSkillSystem)의 알림 둘(시전 · 되살려라)
//   IPartyEvents      = 파티 규칙(CPartySystem)의 알림 다섯(요청·요청 끝·알림 한 줄·파티 전체·파티원 한 줄)
//   ICharacterLoadEvents = 캐릭터 저장소(CCharacterStore)의 불러오기 결과 하나(OnCharacterLoaded) — 게임 서버 나누기 4 단계(0-③ ⑷) · USE_DB_WORKER
// 기반 목록 — 늘 있는 통로(IPlacementEvents)가 첫 줄이라 나머지는 토글마다 「, 」로 붙는다.
//   (3·4 단계 동안은 늘 있던 IZoneEvents 가 빠져 첫 통로를 고르는 #if 사다리를 뒀었다 — 5 단계에서 늘 있는 통로가 생겨 걷어냄)
class CGameServer
    : private IPlacementEvents
#if USE_COMBAT
    , private ICombatEvents
    , private IBuffEvents
#endif
#if USE_MONSTER
    , private IMonsterEvents
#endif
#if USE_SKILL
    , private ISkillEvents
#endif
#if USE_PARTY
    , private IPartyEvents
#endif
#if USE_DB_WORKER
    , private ICharacterLoadEvents
#endif
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
        _playerSpeed = playerSpeed;
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
    void OnConnected(int64_t sessionId);
    void OnDisconnected(int64_t sessionId);
    void OnReceived(int64_t sessionId, CSerialBuffer* pMsg);

    // ── 입장(스폰) ──
    // 불러온 결과를 "스폰이 쓸 꼴"로 바꾼 것. 판정하는 쪽(OnCharacterLoaded·OnConnected)이
    //   채우고 SpawnPlayer 가 쓴다. 판정과 스폰을 가른 이유 — 갈래(찾음·없음·맵 없음·좌표 불량)는
    //   넷인데 스폰 절차는 하나다.
    //   ★ USE_DB_WORKER 밖에 둔다 — 토글을 끈 빌드의 접속 처리도 SpawnPlayer 를 부른다.
    //   ★ level·exp 가 여기 있는 이유 — 스폰이 패킷을 보내기 "전"에 값이 서 있어야 한다.
    //     스폰 뒤에 바깥에서 넣으면 그 패킷이 기본값을 싣고 나간다.
    struct EnterPlacement
    {
        bool    hasPos  = false;   // false = 그 맵의 들어오는 자리(CZone::CalcSpawnPos — 시작 구역 · 없으면 무작위)
        float   x       = 0.0f;
        float   y       = 0.0f;
        bool    dbDirty = true;    // 스폰 자리를 다음 주기에 저장할까 (DB 와 같은 값으로 들어왔으면 false)
        int32_t level   = 1;       // 불러온 값. 새 캐릭터·못 읽음이면 기본값 그대로
        int64_t exp     = 0;
        int32_t hp      = DB_HP_MP_UNSAVED;   // 불러온 체력 — 모름(-1)이면 가득. 죽은 채 저장된 0 은 판정하는 쪽이 부활 규칙대로 바꿔 넘긴다
        int32_t mp      = DB_HP_MP_UNSAVED;   // 불러온 마력 — 모름(-1)이면 가득
#if USE_DB_WORKER && USE_INVENTORY
        // 불러온 가방 줄이 든 결과 — 찾음·없음 갈래에서 채운다(없음이어도 가방 줄은 있을 수 있다). nullptr = 가방 없음.
        //   회신 큐의 원소를 가리킨다 — OnCharacterLoaded 가 SpawnPlayer 를 동기로 부르는 동안만 살아 있다.
        const DBResult* loaded = nullptr;
#endif
#if USE_DB_WORKER
        // 불러온 쿨타임·버프 묶음(characters.skill_cooldowns · buffs)이 든 결과 — 찾음 갈래에서만 채운다. nullptr = 되살릴 것 없음(새 캐릭터).
        //   loaded 와 같은 수명이다. 풀기·검사·칸 채우기는 SpawnPlayer 의 RestoreCooldownsAndBuffs 가 한다.
        const DBResult* cooldownsAndBuffs = nullptr;
        bool            dropBuffs         = false;   // 죽은 채 저장된 행 — 버프는 버린다(죽으면 버프가 지워지는 규칙). 쿨타임은 되살린다(죽어도 남는 규칙)
#endif
    };

    // 플레이어를 만들어 존에 넣고 본인·주변에 알린다 (OnConnected 의 몸이 옮겨 온 것).
    //   존은 호출부가 구해서 준다 — 좌표 검사에 그 존의 크기가 필요해서 판정하는 쪽이 이미 들고 있다.
    //   반환 nullptr = zone 이 nullptr(채널을 못 만듦) → 호출부가 세션을 끊는다. 존 입장 자체는 실패가 없다(CZone::EnterZone).
    CPlayer* SpawnPlayer(int64_t sessionId, int32_t actorId, int64_t accountId,
                         CZone* zone, const EnterPlacement& place);

    // 떠나보내기(LeaveGame) · 놓기(ReleasePlayer)는 배치 조각 — _placement (PlayerPlacement.h · 게임 서버 나누기 5 단계).
    //   그 안에서 틱 너머 목록을 정리하라는 알림이 아래 둘로 온다.

    // 「존에서 빠짐 · 떠남」 알림 (IPlacementEvents) — 뜻은 PlayerPlacement.h. 몸은 옮기기 전 RemoveFromZone · ReleasePlayer 의 그 자리 그대로.
    void OnLeavingZone(CCharacter* player) override;   // 부활 대기열에서 빼기 (전투)
    void OnLeavingGame(CPlayer* player) override;      // 부활 대기열 · 지속 효과 목록 · 파티원·요청 표에서 빼기 (전투 · 파티)

    // 패킷 핸들러
    void RecvMoveStart(CPlayer* player, CSerialBuffer* pMsg);
    void RecvMoveStop(CPlayer* player, CSerialBuffer* pMsg);
    void RecvChat(CPlayer* player, CSerialBuffer* pMsg);
    void RecvZoneChange(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPortalUse(CPlayer* player, CSerialBuffer* pMsg);   // 포털 — 판정 뒤 TransferPlayer 로 포털 표의 도착 자리에
    void RecvAdminLogin(CPlayer* player, CSerialBuffer* pMsg);

#if USE_COMBAT
    // ── 전투 (USE_COMBAT) ──
    // 공격 흐름은 CCombatSystem(_combatSystem)이 하고, 여기서는 그 알림을 받아 세계에 적용한다
    //   (송신 · 죽은 목록 등록 · 보상).
    void RecvAttack(CPlayer* player, CSerialBuffer* pMsg);

    // 부활 대기자 훑기 — 틱마다 1회. 존 전체를 돌지 않으려고 죽은 것만 따로 들고 있는다.
    //   플레이어만 — 몬스터는 되살리지 않는다(죽는 순간 지우고, 스폰이 새로 채운다).
    //   마을(기본 맵)이 아닌 맵에서 죽었으면 마을 시작 구역으로 옮겨 되살린다(배치의 TransferPlayer, ArrivalKind::RESPAWN) — 대기열을 다 돈 뒤에.
    void TickRespawn();

    // 부활 자리 규칙 한 곳 — 죽은 맵이 마을(기본 맵)이 아니면 마을 시작 구역으로, 마을이면 제자리.
    //   부르는 곳 둘: TickRespawn(죽고 3초 뒤) · OnCharacterLoaded(죽은 채 저장된 행 — 나가기·부활 전 주기 저장·죽은 채 서버 다운).
    bool RespawnsToTown(int32_t deathMapId) const { return deathMapId != _defaultMapId; }

    // 부활 대기열에서 빼기 — 퇴장 경로가 반드시 부른다(배치의 「존에서 빠짐 · 떠남」 알림 — OnLeavingZone · OnLeavingGame). 안 부르면 다음 틱이 해제된 포인터를 밟는다.
    void RemoveFromDeadActors(CCharacter* player);

    // ── 지속 효과 (USE_COMBAT) ──
    // 걸기·만료·정리는 CBuffSystem(_buffSystem) — 여기는 알림을 받아 본인 알림 조각에 넘기기만. 틱 3-2b 가 만료를 훑고,
    //   사라지는 두 길(놓기의 「떠남」 알림 OnLeavingGame → Forget · OnDeath → ClearAll)이 목록에서 뺀다(BuffSystem.h [목록은 틱 너머로 주소를 든다]).
    // 효과 알림 (IBuffEvents) — 스탯은 이미 다시 선 뒤다. 사람이면 S2C_ACTOR_BUFF(본인) → S2C_MY_STATS(바뀐 공격력). 몬스터면 보낼 세션이 없다.
    void OnBuffChanged(CCharacter* target, BuffKind kind) override;

    // 효과 칸 보내기(SendActorBuff · SendActiveBuffs)는 본인 알림 조각으로 옮겼다 — _selfNotifier (게임 서버 나누기 2 단계)

    // 공격 알림 셋 (ICombatEvents) — 뜻과 순서는 CombatSystem.h. 셋 다 공격자 주변 9섹터에 방송한다.
    void OnSwing(CZone* zone, CCharacter* attacker) override;
    void OnHit(CZone* zone, CCharacter* attacker, CCharacter* target, int32_t damage, int32_t remainHp) override;
    void OnDeath(CZone* zone, CCharacter* attacker, CCharacter* target) override;   // 죽은 목록 → DEATH → 경험치 → 드랍 → 몬스터면 지움

#if USE_EXP && USE_MONSTER
    // 몬스터 경험치 주기 한 곳 — 막타 경로(OnDeath 6)와 방 보상(RewardRoom)이 같이 쓴다. 준 뒤에 세고, 오르면 알리고 바로 저장한다.
    void GrantMonsterExp(CZone* zone, CPlayer* player, int32_t reward);
#endif

#if USE_PARTY && USE_MONSTER
    // 방 보상(Room.h) — 방 맵의 보스(몬스터 표 boss)가 죽으면 OnDeath 가 막타 경로(경험치·드랍) 대신 부른다. 방(존) 안 사람 각자
    //   (쓰러져 부활을 기다리는 사람 포함 — 아직 방에 있다): ⑴ 경험치 전액 ⑵ 드랍 표를 따로 굴려 가방에 바로, 넘치면 그 사람 발밑 땅에 그 사람 것으로
    //   ⑶ 다시 들어가기 잠금(맵 표 lockoutSec) ⑷ 채팅 [System] 줄 — 경험치 한 줄 + 아이템마다 한 줄. 막타·피해 기록은 안 본다(room-plan.md 「정해진 것」).
    void RewardRoom(CZone* zone, CMonster* monster);
#endif
#endif

#if USE_INVENTORY
    // ── 인벤토리 (USE_INVENTORY) ──
    // '/' 로 시작하는 채팅 — 방송하지 않고 명령으로 처리한다. 답은 본인에게 S2C_ERROR 한 줄.
    //   명령은 이름 → 처리 함수 표(GameServer.cpp HandleChatCommand). 지금 둘 다 운영자 전용이다.
    void HandleChatCommand(CPlayer* player, const ChatChar* msg, uint16_t len);
    void RunItemCommand(CPlayer* player, const std::vector<std::string>& words);   // /item <아이템번호> [개수] — 가방에 바로 넣기
    void RunGmCommand(CPlayer* player, const std::vector<std::string>& words);     // /gm — GM 모드 켜고 끄기(Player.h GM 칸)

    // 바뀐 칸 내보내기(FlushInventory) · 물약 쓰기 · 끼우기·빼기는 가방·땅 조각 — _itemHandler (ItemHandler.h · 게임 서버 나누기 6 단계)
    // 가방 전체(SendInventory) · 알림 한 줄(SendSystemMessage)은 본인 알림 조각으로 옮겼다 — _selfNotifier (게임 서버 나누기 2 단계)
    // 불러온 가방 줄 넣기(LoadInventoryRows)는 캐릭터 저장소 조각 — CharacterStore.h (게임 서버 나누기 4 단계)
#endif

#if USE_SKILL
    // ── 스킬 (USE_SKILL) ──
    // 판정·비용·대상·효과는 CSkillSystem(_skillSystem) — 여기는 받기와, 거절이면 사유를 본인에게(와이어 값으로 바꿔) 보내기만.
    //   성공의 결과는 알림으로 돌아온다 — 시전(OnCast: 시전 방송 · 시전자 체력 · 시전자 마력) → 피해·죽음·보상(ICombatEvents, 일반 공격과 같은 함수).
    void RecvUseSkill(CPlayer* player, CSerialBuffer* pMsg);

    // 시전 알림 (ISkillEvents) — 뜻과 순서는 SkillSystem.h. 시전자 주변 9섹터에 방송한다(본인 포함). 마력은 본인에게만.
    void OnCast(CZone* zone, CCharacter* caster, const SkillDef& def) override;

    // 되살려라 (ISkillEvents) — 존의 몬스터 관리자에 요청만 넘긴다(RequestRevive — 깔기는 다음 틱 리젠 · 등장 알림은 OnMonsterSpawn 그대로).
    void OnRevive(CZone* zone, CCharacter* caster, int32_t monsterId) override;

    // 마력(SendActorMp) · 남은 쿨타임(SendActiveSkillCooldowns)은 본인 알림 조각으로 옮겼다 — _selfNotifier (게임 서버 나누기 2 단계)
#endif

#if USE_PARTY
    // ── 파티 (USE_PARTY) ──
    // 규칙(누가 무엇을 할 수 있나 · 요청 표 · 시간 지남 · 파티원 상태 비교)은 CPartySystem(_partySystem) — 여기는 받기와, 알림을 패킷으로 바꿔 보내기만.
    //   받기 여섯 — 요청은 누른 사람을 주변 9섹터에서 찾아(FindPlayerAround) 넘기고, 나머지는 번호를 그대로 넘긴다.
    void RecvPartyRequest(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPartyAnswer(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPartyLeave(CPlayer* player);
    void RecvPartyKick(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPartyLeader(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPartyDisband(CPlayer* player);

    // 내 주변 9섹터(시야 — 내 화면에 그려질 수 있는 곳)의 사람 중 이 번호 — 없으면 nullptr. 몬스터는 안 본다(플레이어 목록만).
    //   서버에 번호 → 사람 표가 없어서(세션 번호로만 찾는다 — FindPlayer) 화면에서 누른 사람을 여기서 찾는다. 「화면에 보이는 사람」 검사를 겸한다.
    CPlayer* FindPlayerAround(CPlayer* player, int32_t actorId);

    // 파티 전용 방(Room.h) — RecvPortalUse 가 목적지가 방 맵이면 여기로 넘긴다(반경·생존은 이미 봤다).
    //   판정(CheckRoomEntry) → 방 만들기(CMapManager::CreateRoom) → 파티장, 그다음 가입 순서로 전원 TransferPlayer(포털 도착 자리).
    //   거절이면 아무도 안 옮기고 누른 사람에게만 알린다(SendRoomReject). 들어가기를 세는 곳은 여기 한 곳(_gameMonitor._room · _portal) —
    //   방이 닫히는 것(열린 방 −1 · 닫힘)은 빈 채널 정리 콜백이 센다(게임 루프 5).
    void EnterRoom(CPlayer* leader, CZone* oldZone, const PortalDef& portal);
    // 거절 사유 → 와이어 — 파티 없음·파티장 아님은 파티 알림의 그 두 번호, 다른 존·쓰러짐·잠금은 방 알림 셋(막는 사람 번호),
    //   번호 다 참은 S2C_ZONE_CHANGE_FAIL 1(「모든 채널 가득 참」). 파티 알림은 파티 규칙의 Notice 로 보낸다(번호별 계수가 거기 한 곳)
    void SendRoomReject(CPlayer* leader, const RoomEntryResult& result);

    // 파티 알림 (IPartyEvents) — 뜻은 PartySystem.h. 보내는 수는 여기서 센다(_gameMonitor._party 의 …Sent).
    void OnPartyAsk(CPlayer* to, uint32_t requestId, PartyAskKind kind, const CPlayer* from, const CPlayer* about, uint32_t remainMs) override;
    void OnPartyAskEnd(CPlayer* to, uint32_t requestId, PartyNotice why) override;
    void OnPartyNotice(CPlayer* to, PartyNotice code, int32_t actorId) override;
    void OnPartyInfo(CPlayer* to, const Party* party) override;
    void OnPartyMember(CPlayer* to, const CPlayer* member) override;
#endif

#if USE_MONSTER
    // ── 몬스터 (USE_MONSTER) ──
    // 몬스터 코드(깔기·판단·벽 앞 재판단)는 존마다 든 CMonsterManager 에 있다.
    //   여기는 틱마다 그걸 부르고, 알림을 받아 송신·번호 발급만 한다.

    // 게임 루프 1-1) — 좌표 갱신(2) 바로 앞. 존마다 관리자 Tick(처음이면 깔고, 몬스터마다 판단).
    //   순서가 뒤면 AI 가 정한 방향이 한 틱씩 밀린다.
    void TickMonsters();

    // 관리자에 넘길 묶음 — 부를 때마다 만든다. 담긴 것이 전부 이 객체가 든 것의 참조라 만드는 값이 싸다.
    //   (예전엔 설정값 둘 — 존당 마릿수·뱀 속도 — 이 Start() 전에 바뀌어 틱마다 만들었다. 둘 다 몬스터 표·스폰 표로 갔다)
    MonsterContext MakeMonsterContext();

    // 몬스터 알림과 번호 부탁 (IMonsterEvents) — 뜻은 MonsterManager.h.
    int32_t AllocActorId() override;                                         // = 세션 허브의 발급기 — 번호 공간이 플레이어와 하나
    void OnMonsterSpawn(CZone* zone, CMonster* monster) override;            // 주변에 CREATE(SPAWN) · 동기화 기준점을 지금 자리로
    void OnMonsterVanish(CZone* zone, CMonster* monster) override;           // 방 클리어 — 효과 목록에서 빼고 존에서 뺀다(주변 DELETE) · 죽은 몬스터 지우기 앞과 같은 길
#endif

#if USE_DROP
    // ── 드랍 (USE_DROP) ──
    // 땅 아이템(목록)과 그 규칙(놓기·수명·주인·줍기 고르기·개수)은 존마다 든 CGroundItemManager 에 있다(GroundItemManager.h).
    //   여기는 무엇이 떨어질지 굴리고, 관리자를 부르고, 알림을 받아 송신·번호 발급을 하고, 줍기의 가방·저장을 한다.

    // 사람이 몬스터를 잡으면 부른다(OnDeath 의 경험치 블록 뒤 — 같은 조건).
    //   표의 줄마다 굴리고, 결과를 존의 관리자에게 넘겨 땅에 놓는다(아이템마다 OnGroundItemDrop 으로 돌아온다).
    //   주인 = 막타 친 계정 — 굴림은 잡은 사람에게 주는 보상이라 경험치와 같은 이 자리에서 한다.
    void DropLoot(CZone* zone, CPlayer* killer, CMonster* monster);

    // 관리자에 넘길 묶음(MakeGroundItemContext) · 땅 아이템 알림(IGroundItemEvents) · 수명 훑기 틱 · 줍기는 가방·땅 조각
    //   — _itemHandler (ItemHandler.h · 게임 서버 나누기 6 단계). 굴림(위 DropLoot)만 여기 남았다(처치 보상 몫).
#endif

#if USE_DB_WORKER
    // DB 저장 파이프라인(저장 세 길 · 저장 대상 판정 · 쿨타임·버프 묶기·되살리기)과 불러오기 요청·회신 꺼내기는
    //   캐릭터 저장소 조각 — _characterStore (CharacterStore.h · 게임 서버 나누기 4 단계).

    // 불러오기 결과 알림 (ICharacterLoadEvents) — 1건 처리: 판정 후 스폰. 회신 꺼내기(CCharacterStore::ProcessDbResults)가 부른다.
    void OnCharacterLoaded(const DBResult& result, int64_t nowNs) override;

    // 대기표 — 접속은 했지만 아직 게임에 못 들어온 세션. 세션은 아래 둘 중 하나이거나 "게임 중"이고,
    //   셋 중 둘일 수는 없다. sessionId == -1 이 빈 자리다(CPlayer::_sessionId 와 같은 약속).
    //   세션 번호 "전체"를 들고 있어야 같은 자리에 다른 사람이 들어왔을 때 옛 결과를 가려낸다.
    //   ★ 접속한 세션은 반드시 이 표에 자리를 잡는다 — 어느 표에도 없는 세션이 생기면
    //     "동접 = 게임 중 + 불러오는 중 + 입장 대기" 등식이 늘 틀린 채로 울려 쓸모가 없어진다.
    enum class EnterState : uint8_t
    {
        WAITING_ENTER,   // 접속함. 입장 요청(C2S_ENTER_GAME)을 기다린다 — USE_LOGIN=1 에서만 생긴다
        LOADING,         // 입장 요청을 받아 조회를 던졌다. 결과를 기다린다
    };
    struct PendingEnter
    {
        int64_t    sessionId = -1;
        int64_t    accountId = 0;   // [입장 대기] 구간에서는 아직 0 이다 — 입장 요청이 와야 정해진다
        int32_t    actorId   = 0;   // 같음. 캐릭터 번호도 입장 요청 때 뽑는다
        EnterState state     = EnterState::WAITING_ENTER;
    };
    // 대기표를 바꾸는 자리는 이 셋뿐이다 — 게이지 둘(mmo_enter_waiting·mmo_enter_pending)도 여기서만 움직인다.
    //   접속·입장요청·결과·끊김 네 곳이 제각기 올리고 내리면 한 곳만 빠져도 게이지가 영영 안 맞는다.
    void SetPendingEnter(int64_t sessionId, int64_t accountId, int32_t actorId, EnterState state);
    bool PromotePendingEnter(int64_t sessionId, int64_t accountId, int32_t actorId);  // [입장 대기] → [불러오는 중]
    bool TakePendingEnter(int64_t sessionId, PendingEnter& out);                      // 세션 번호 전체가 같으면 꺼내고 비운다
    void AdjustPendingCount(EnterState state, int delta);                             // 상태별 수 + 게이지를 한 자리에서 움직인다
#endif

#if USE_LOGIN
    // ── 입장 요청 (USE_LOGIN) ──
    // 이 핸들러만 OnReceived 의 FindPlayer 문턱 "앞"에서 불린다 — 입장 요청이 올 때는 아직
    //   플레이어가 없어서, 문턱 뒤에 두면 영영 여기까지 못 온다.
    void RecvEnterGame(int64_t sessionId, CSerialBuffer* pMsg);

    // 같은 계정으로 이미 들어와 있는 세션을 걷어낸다 (게임 중 / 불러오는 중 양쪽).
    //   새 조회를 던지기 "전"에 불러야 한다 — 정리가 넣는 최종 저장이 그 조회보다 큐에서 앞서야
    //   새 접속이 방금 저장한 자리를 읽는다(같은 계정은 같은 워커를 탄다).
    void KickDuplicateAccount(int64_t accountId, int64_t exceptSessionId);

    // 지정 계정 번호의 하한 = 2^31. 입장 요청의 계정 번호는 0(미지정) 아니면 이 값 이상이어야 한다.
    //   자동 번호는 int32 카운터(CSessionHub::AllocActorId)라 2^31 위로 못 올라오므로, 이 아래로 내려온 지정 번호는
    //   0 으로 들어온 남의 접속과 같은 행을 쓰게 된다. 노션의 "100억 이상"은 운용 권장이고 서버가 강제하는 값이 아니다.
    static constexpr int64_t ACCOUNT_ID_MIN = int64_t{ 1 } << 31;
#endif

    // 섹터 변경 처리(ProcessSectorChange)·같은 틱 쌍 보정(SendSightPairFixes)은 시야 조각으로 옮겼다 — _sightBroadcaster (게임 서버 나누기 3 단계)

    // 도착 종류(ArrivalKind) · 도착 차례(NotifyArrival) · 존에서 존으로 옮기기(TransferPlayer · SpawnAt) · 존에서 빼기(RemoveFromZone)는
    //   배치 조각 — _placement (PlayerPlacement.h · 게임 서버 나누기 5 단계).
    // 존 입장/퇴장 브로드캐스트(BroadcastEnterZone · BroadcastSpawn · BroadcastLeaveZone)는 시야 조각 — SightBroadcaster.h (게임 서버 나누기 3 단계)

    // ── 표시 속성 ── (캐릭터 번호 발급은 세션 허브 — CSessionHub::AllocActorId)

    static uint8_t CalcDisplayChar(int32_t playerId);
    static uint8_t CalcColorIndex(int32_t playerId);

    // ── 패킷 전송 ──
    // 한 사람에게 보내기는 세션 허브(_sessionHub.SendPacket — SessionHub.h). 주변에 보내기 · 이동 알림 · 존 알림(IZoneEvents) ·
    //   섹터 묶음 · 수신섹터 digest · 멤버십 팬아웃·직송 · 남의 캐릭터 생성·삭제 · 내 자리(SendSyncPosition)는 시야 조각
    //   (_sightBroadcaster — SightBroadcaster.h · 게임 서버 나누기 3 단계).

    // 패킷별 전송 함수(존 정보 · 내 캐릭터 생성 · 존 이동 성공·실패)는 배치 조각 — _placement (게임 서버 나누기 5 단계)

    // C2S_MOVE_START 좌표 수용의 이동량 예산 검사 — 경과분 적립 후 통과하면 소비까지 수행.
    // 반환 false면 수용하지 않고 서버 좌표를 유지한다(기존 범위초과 경로와 동일한 처리).
    bool ConsumeMoveBudget(CPlayer* player, float distSq);

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
    static constexpr float MOVE_START_ACCEPT_DIST_SQ = 64.0f; // C2S_MOVE_START 좌표 수용 임계값 제곱 (8타일)

    // 좌표 수용 예산 — 위 임계값은 1회 한도일 뿐 빈도 제한이 없어, STOP/START를 연타하면
    //   매 전이마다 8타일씩 수용돼 사실상 무제한 순간이동이 됐다(수신 빈도 제한은 서버 어디에도 없음).
    //   상한은 위 임계값과 같은 8타일 → 정상 클라의 단발 보정은 지금과 동일하게 통과.
    //   적립은 자기 속도 × 여유계수로 시간 비례 → 연타해도 장기 평균은 적립 속도로 수렴한다.
    //   단 서버 주도 이동(매 틱 speed×dt)은 예산 검사 대상이 아니라 그 위에 얹히므로,
    //   치트 이동량 상한은 speed×(1+SLACK)이다. SLACK 1.25일 때 실측 2.09배 → 0.25로 조여 총합 1.25배.
    static constexpr float MOVE_BUDGET_CAP = 8.0f;     // 예산 상한 (타일) — MOVE_START_ACCEPT_DIST_SQ의 제곱근
    // 적립 여유계수 (네트워크 지터·프레임 흔들림 흡수). 낮출수록 조이지만 정상 클라 오탐 위험이 커진다:
    //   정상 소모 ≈ RTT/2 × speed(예측-서버 좌표 오차), 적립은 speed×SLACK/초.
    //   speed 30·RTT 200ms(오차 3타일/회) 기준 견디는 MOVE_START 빈도 = 초당 2.5회.
    //   루프백 실측은 RTT≈0이라 이 오탐을 못 잡는다 → mmo_move_budget_rejects_total 실네트워크 관측 필요.
    static constexpr float MOVE_BUDGET_SLACK = 0.25f;

    int32_t _defaultMapId = 0;  // 최초 접속 시 입장할 맵 = 맵 표 첫 줄 = 마을(죽으면 여기 시작 구역으로 — TickRespawn)

#if USE_DROP
    // 땅의 아이템 번호(_nextGroundId) · 수명 훑기 세기(_groundItemScanCounter)는 가방·땅 조각 — ItemHandler.h (게임 서버 나누기 6 단계)
    std::vector<Item> _lootBuffer;   // 한 마리분 굴림 결과 (멤버 재사용 — 죽을 때마다 할당하지 않게)
#if USE_PARTY
    std::vector<Item> _roomOverflow; // 방 보상 중 가방이 차서 못 넣은 것 — 그 사람 발밑 땅으로(RewardRoom · 멤버 재사용)
#endif
#endif

#if USE_COMBAT
    // 스탯·레벨 규칙 — 표(_gameDataFiles)를 참조한다.
    //   ★ _gameDataFiles·_gameMonitor 보다 뒤에 선언해야 한다(멤버는 선언 순서로 세워진다 — _combatSystem 주석과 같은 까닭).
    CStatSystem _statSystem{ _gameDataFiles, _gameMonitor };

    // 지속 효과 규칙 — 알림은 이 클래스(IBuffEvents)로 돌아온다. 바뀌면 _statSystem 으로 스탯을 다시 세운다(그래서 그 뒤에 선언).
    //   거는 쪽은 _skillSystem(아래 — 그 참조를 받으므로 이보다 뒤) · 아이템이 걸게 되면 _itemSystem 도 받는다.
    CBuffSystem _buffSystem{ _gameMonitor, _statSystem, *this };
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
    // 캐릭터 저장소 — DB 워커의 주인 · 저장 세 길 · 불러오기 요청과 회신 꺼내기(CharacterStore.h). 불러오기 결과는 이 클래스(ICharacterLoadEvents)로 온다.
    //   ★ _sessionHub · _gameMonitor · _gameDataFiles · _buffSystem · _frameCount 보다 뒤에 선언 — 그 참조를 받는다(멤버는 선언 순서로 세워진다).
    //     *this 는 참조만 넘긴다(생성 중엔 부르지 않는다 — 시스템들에 *this 를 넘기는 것과 같은 규칙).
    CCharacterStore _characterStore{ _sessionHub, _gameMonitor, _gameDataFiles,
#if USE_COMBAT
                                     _buffSystem,
#endif
                                     _frameCount, *this };
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

    // 플레이어 이동 속도 덮어쓰기(칸/초) — 0 이면 생성자의 기본값을 그대로 쓴다(SetPlayerSpeed)
    int _playerSpeed = 0;
#if USE_COMBAT
    // 공격 후보 버퍼는 두지 않는다 — 사거리가 걸치는 섹터의 주민 목록을 참조로 훑어 복사가 없다.
    //   (처음엔 GetAroundPlayers 의 전신이 벡터를 요구해 _attackTargetBuffer 를 뒀었다)

    // 죽어서 부활을 기다리는 플레이어. 여기만 틱 너머로 포인터를 든다 —
    //   퇴장 경로(배치의 「존에서 빠짐 · 떠남」 알림 — OnLeavingZone · OnLeavingGame)가 반드시 회수해야 한다.
    //   몬스터는 여기 안 들어온다 — 죽는 순간 지운다(OnDeath 끝 → 존의 몬스터 관리자 Despawn).
    //   같은 규율이 이미 dirtyMovers(시야 조각) 와 존의 섹터 이동 대기열(CZone::LeaveZone 이 뺀다)에 있다.
    std::vector<CCharacter*> _deadActors;
    // 마을 부활로 옮길 사람과 목적지 채널 — TickRespawn 이 대기열을 다 돈 뒤에 옮긴다(도는 중에 옮기면 퇴장이 대기열을 지운다).
    //   틱 너머로 안 든다(그 함수 안에서 채우고 비운다) — 멤버인 까닭은 틱마다 할당하지 않으려는 것뿐(시야 조각의 _eventAroundBuffer 와 같은 꼴)
    std::vector<std::pair<CPlayer*, CZone*>> _respawnTransferBuffer;

    // 공격 흐름 — 알림은 이 클래스(ICombatEvents)로 돌아온다.
    //   ★ _gameMonitor 보다 뒤에 선언해야 한다 — 멤버는 선언 순서로 세워지므로, 앞에 두면 아직 안 묶인 참조를 넘긴다.
    CCombatSystem _combatSystem{ _gameMonitor, *this };
#endif

#if USE_SKILL
    // 스킬 규칙 — 알림은 이 클래스(ISkillEvents)로 돌아온다.
    //   ★ _combatSystem·_buffSystem 뒤에 선언해야 한다 — 그 참조(대상 찾기·맞힌 뒤 처리 · 지속 효과 걸기)를 받는다. 멤버는 선언 순서로
    //     세워지므로 앞에 두면 아직 안 만들어진 객체의 참조를 넘긴다(위 _combatSystem 주석과 같은 까닭).
    CSkillSystem _skillSystem{ _gameDataFiles, _gameMonitor, _combatSystem, _buffSystem, *this };
#endif

#if USE_PARTY
    // 파티 규칙 — 알림은 이 클래스(IPartyEvents)로 돌아온다. 파티원·요청의 주소를 틱 너머로 들므로 놓기의 「떠남」 알림(OnLeavingGame)이 Forget 을 부른다.
    //   ★ _gameMonitor 보다 뒤에 선언해야 한다(위 _combatSystem 주석과 같은 까닭).
    CPartySystem _partySystem{ _gameMonitor, *this };

    // 방 다시 들어가기 잠금(계정 · 방 맵 → 풀리는 틱) — 보상을 줄 때 넣고(OnDeath 방 갈래) 들어가기 판정이 본다. 서버 메모리만(재시작하면 풀림).
    //   풀린 것은 파티 틱 옆에서 초마다 훑어 뺀다(게임 루프 3-2c)
    CRoomLocks _roomLocks;
#endif

    // 섹터 아웃박스(틱-지연 배칭 상태) · 인바운드 멤버십 수집 버퍼는 시야 조각 — SightBroadcaster.h (게임 서버 나누기 3 단계)

    // 비용종류별 계측 — 틱 내 누적 후 틱 끝 1회 모니터 반영 (단일 게임루프 스레드 전용 → 누적 자체엔 원자연산 불필요)
    //   방송 쪽 다섯(_tickBroadcast* · _tickMembership* · _tickPairFixes)은 시야 조각으로 갔다(ReportTickMetrics) — 여기 남은 것은 이동 예산 하나.
    int64_t _tickMoveBudgetRejects = 0;   // 이동 예산 초과로 거부한 좌표 수용 수 — 틱 끝 _moveBudgetRejects로 반영

#if USE_DB_WORKER
    // DB 워커 · 저장 주기 세기 · 회신 받는 버퍼는 캐릭터 저장소 — CharacterStore.h (게임 서버 나누기 4 단계)

    // 대기표 — 세션 허브의 칸(_sessionHub)과 같은 크기·같은 색인(해시맵 아님). Init 에서 나란히 잡는다.
    //   개수는 배열을 훑지 않으려고 상태별로 따로 센다. 접근은 게임 스레드 단독.
    std::vector<PendingEnter> _pendingEnter;
    int32_t _pendingLoadingCount = 0;   // [불러오는 중] — DB 결과를 기다린다
    int32_t _pendingWaitingCount = 0;   // [입장 대기]  — 입장 요청을 기다린다 (USE_LOGIN=1 에서만 0 이 아니다)
#endif
};
