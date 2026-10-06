// ==========================================================================
// CGameMonitor — 게임 컨텐츠 지표 저장소
//
// [책임]
//  - 게임 컨텐츠(전투 등)의 지표를 담는다. 담기만 하고 노출은 CMonitorServer가 한다
//
// [왜 코어와 나눴나]
//  CMonitorManager는 ServerCore 서브모듈에 있다. 게임 지표를 거기 넣으면
//  코어를 쓰는 다른 프로젝트까지 쓰지도 않는 필드를 함께 지게 된다.
//  그래서 그릇을 게임 쪽에 따로 두고 상속·포함 없이 독립시킨다 — 코어는 이 파일을 모른다.
//  (Counter만 코어에서 빌려 쓴다. 지표가 아니라 원자 카운터 타입이다)
//
// [소유권]
//  - main()이 값으로 소유, CGameServer/CMonitorServer가 레퍼런스로 참조
//    (CMonitorManager와 같은 방식)
// ==========================================================================
#pragma once

#include "BuildConfig.h"      // USE_MONSTER — 몬스터 카운터 묶음을 토글 안에 둔다
#include "MonitorManager.h"   // Counter — 원자 카운터 타입만 빌려 쓴다 (코어 지표는 참조하지 않음)
#include "Inventory.h"        // USE_INVENTORY — 넣기 결과 사유(InvAddResult) 수만큼 칸을 잡는다
#include "GroundItem.h"       // USE_DROP — 땅에서 빠진 까닭(GroundItemRemoval, 땅 아이템 관리자의 어휘) 수만큼 칸을 잡는다

// [사유 enum 의 집 — 규칙 하나] 사유는 그것을 정하는 층의 헤더에 산다(10-06, 아키텍처 전수검사 7번).
//   존·엔티티 층이 정하는 것: GroundItemRemoval·PickupReject(GroundItem.h) · ZoneChangeReject(MapManager.h) · InvAddResult(Inventory.h)
//   게임 서버가 정하는 것: UseItemFail·EquipFail·PickupFail(와이어 enum 그대로 — 정하는 쪽이 패킷을 아는 층이라 변환이 없다) ·
//                        InvLoadSkip·ChatCommandReject(계수기만 쓰는 사유라 여기)
//   여기(계수기)는 COUNT 로 칸을 잡고 이름표를 두는 자리일 뿐, 사유를 정하지 않는다.

#if USE_DROP
// 줍기 거절 사유의 지표 라벨 — 와이어 enum(Protocol.h PickupFail) 그대로 센다. 존의 판정(PickupReject)은 게임 서버가
//   와이어 값으로 바꾸고, 가방이 찬 것(TAB_FULL)은 게임 서버가 정하므로 세는 자리가 게임 서버(RecvPickup) 하나다.
inline const char* PickupFailName(PickupFail r)
{
    static_assert(static_cast<int>(PickupFail::COUNT) == 4, "PickupFail 을 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case PickupFail::DEAD:      return "dead";
    case PickupFail::NO_ITEM:   return "no_item";
    case PickupFail::NOT_OWNER: return "not_owner";
    case PickupFail::TAB_FULL:  return "tab_full";
    case PickupFail::COUNT:     break;
    }
    return "?";
}
#endif

#if USE_INVENTORY
// 채팅 명령 거절 사유 — 넣기 거절(InvAddResult)과 따로 센다. 명령이 틀린 것과 가방이 찬 것은 다른 사실이다.
enum class ChatCommandReject : uint8_t
{
    NOT_ADMIN,         // 운영자가 아니다
    UNKNOWN_COMMAND,   // 모르는 명령
    BAD_ARGS,          // 명령은 맞는데 인자가 틀렸다
    COUNT
};

inline const char* ChatCommandRejectName(ChatCommandReject r)
{
    static_assert(static_cast<int>(ChatCommandReject::COUNT) == 3, "사유를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case ChatCommandReject::NOT_ADMIN:       return "not_admin";
    case ChatCommandReject::UNKNOWN_COMMAND: return "unknown_command";
    case ChatCommandReject::BAD_ARGS:        return "bad_args";
    case ChatCommandReject::COUNT:           break;
    }
    return "?";
}

// 불러올 때 건너뛴 줄의 사유 — DB 행이 망가졌거나 표가 바뀌어 지금 규칙에 안 맞는 줄. 평소엔 전부 0 이다.
//   정하는 쪽은 게임 서버(LoadInventoryRows)이고 가방(CInventory)은 이 값을 만들지 않는다 — 그래서 가방 헤더가 아니라 여기(10-06).
//   ★ 깎아서 넣지 않는다(수량 초과를 한도로 줄이는 식) — 깎으면 무엇이 사라졌는지 아무도 모른다. 세고 건너뛴다.
enum class InvLoadSkip : uint8_t
{
    BAD_SLOT,        // 칸 번호가 어느 탭 구간에도 없다
    UNKNOWN_ITEM,    // 아이템 표에 없는 번호(패치로 지운 아이템)
    WRONG_TAB,       // 아이템의 탭과 칸의 탭이 다르다
    BAD_COUNT,       // 개수가 0 이하이거나 쌓임 한도를 넘는다
    TOO_MANY_ROWS,   // 결과에 다 못 실은 줄(DB_INV_MAX_ROWS 초과)
    COUNT
};

inline const char* InvLoadSkipName(InvLoadSkip r)
{
    static_assert(static_cast<int>(InvLoadSkip::COUNT) == 5, "InvLoadSkip 을 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case InvLoadSkip::BAD_SLOT:      return "bad_slot";
    case InvLoadSkip::UNKNOWN_ITEM:  return "unknown_item";
    case InvLoadSkip::WRONG_TAB:     return "wrong_tab";
    case InvLoadSkip::BAD_COUNT:     return "bad_count";
    case InvLoadSkip::TOO_MANY_ROWS: return "too_many_rows";
    case InvLoadSkip::COUNT:         break;
    }
    return "?";
}

// 쓰기·장착 거절의 지표 라벨 — 사유 enum 은 게임 서버가 정하고 와이어에 실려 Protocol.h 에 있다(CheckUseItem·CheckEquip).
inline const char* UseItemFailName(UseItemFail r)
{
    static_assert(static_cast<int>(UseItemFail::COUNT) == 6, "UseItemFail 을 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case UseItemFail::BAD_SLOT:   return "bad_slot";
    case UseItemFail::EMPTY:      return "empty";
    case UseItemFail::NOT_USABLE: return "not_usable";
    case UseItemFail::DEAD:       return "dead";
    case UseItemFail::COOLDOWN:   return "cooldown";
    case UseItemFail::FULL_HP:    return "full_hp";
    case UseItemFail::COUNT:      break;
    }
    return "?";
}

inline const char* EquipFailName(EquipFail r)
{
    static_assert(static_cast<int>(EquipFail::COUNT) == 6, "EquipFail 을 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case EquipFail::BAD_SLOT:       return "bad_slot";
    case EquipFail::EMPTY:          return "empty";
    case EquipFail::NOT_EQUIPPABLE: return "not_equippable";
    case EquipFail::DEAD:           return "dead";
    case EquipFail::COOLDOWN:       return "cooldown";
    case EquipFail::TAB_FULL:       return "tab_full";
    case EquipFail::COUNT:          break;
    }
    return "?";
}

// 아이템 저장 계수기 — DB 워커 스레드가 센다(DBWorker.h 가 앞 선언으로 가리킨다).
//   ★ 게임 스레드가 세는 _inventory 와 따로 둔다 — 두 스레드가 같은 캐시 줄을 번갈아 쓰지 않게(alignas(64)).
struct alignas(64) DBItemSaveCounters
{
    Counter _saved;     // 칸 저장 성공
    Counter _failed;    // 재연결 뒤 다시 해 봐도 실패 — 0 이어야 한다(0 이 아니면 그 칸은 다음 변화 전까지 DB 와 다르다)
    Counter _retried;   // 재연결하고 다시 써 본 수(성공·실패 무관) — DB 가 한 번 끊겼다 살아난 흔적
};
#endif

// DB 저장 파이프라인 지표 — DB 워커 스레드가 센다(CDBWorker 가 생성자로 참조를 받는다). 위 DBItemSaveCounters 와 같은 쪽.
//   10-06 코어(CMonitorManager::_db*)에서 옮김 — DB 워커는 게임 프로젝트 구성요소라 그릇도 게임 쪽이어야 한다
//   (아키텍처 전수검사 8번). 노출 이름(mmo_db_*)은 그대로다 — 기존 게이트(mmo_db_dropped_total 0 · queue_depth 지속 상승)가 안 흐려진다.
//   USE_DB_WORKER=0 이어도 남겨 0 으로 노출된다(입장 지표와 같은 이유).
struct alignas(64) DBSaveCounters
{
    static constexpr int MAX_WORKERS = 16;   // 큐 깊이 게이지 배열 상한 — CDBWorker::Start 가 워커 수를 여기에 맞춰 자른다

    Counter _savedJobs;                 // UPSERT 성공 누적
    Counter _failedJobs;                // UPSERT 실패 누적
    Counter _droppedJobs;               // 백프레셔 드롭 누적(슬롯 큐 상한 초과)
    Counter _queueDepth[MAX_WORKERS];   // 워커별 게이지: 마지막 배치 인출량
    Counter _workerCount;               // 노출 루프 상한(CDBWorker::Start 에서 설정)
};

class CGameMonitor
{
public:
    CGameMonitor() = default;
    ~CGameMonitor() = default;

    CGameMonitor(const CGameMonitor&) = delete;
    CGameMonitor& operator=(const CGameMonitor&) = delete;

    // ══════════════════════════════════════════════════════════════
    // 전투 (USE_COMBAT)
    //
    // 갱신은 게임 루프 스레드 한 곳뿐이지만, 코어 카운터와 캐시 라인이 겹치지
    // 않도록 alignas(64)로 띄운다 (CMonitorManager의 덩어리 나눔과 같은 규칙)
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) CombatCounters
    {
        Counter _attacksTotal;              // 판정을 통과해 실제로 휘두른 공격

        // 거부는 사유별로 가른다 — 한 통에 담으면 제일 흔한 하나가 나머지를 삼켜서
        //   "헛스윙이 많은 건지 쿨타임에 막힌 건지"를 못 읽는다.
        Counter _attacksRejectedCooldown;   // 거부: 쿨타임이 안 찼다
        Counter _attacksRejectedDeadSelf;   // 거부: 공격자가 죽어 있다
        Counter _attacksRejectedNoTarget;   // 거부: 사거리·방향 안에 아무도 없다 (헛스윙)
        Counter _attacksRejectedDeadTarget; // 거부: 앞에 있는 것이 이미 시체다

        Counter _deathsTotal;               // HP가 0 이하로 떨어진 횟수
        Counter _respawnsTotal;             // 부활 횟수 (deaths와 같거나 1 적은 것이 정상)

        // 피해가 최소 1 로 올려진 타격 — 공격력 − 방어력이 1 미만이었다. ★ 위 값들과 달리 때린 쪽을 안 가른다
        //   (뱀이 방어력 높은 사람을 문 것도 센다). 지금 레벨 표로는 안 닿아 평소 0 이다.
        Counter _damageFloored;

        // S2C_MY_STATS 보낸 수 — SendMyStats 한 곳에서만 센다. 입장마다 1 이라 더미 시험의 분모다.
        Counter _myStatsSent;
    } _combat;

#if USE_MONSTER
    // ══════════════════════════════════════════════════════════════
    // 몬스터 (USE_MONSTER)
    //
    // ★ 위 _combat 과 나누는 이유 — 위 값들은 기존 A/B 실측의 기준선이다.
    //   몬스터를 같은 통에 섞으면 USE_MONSTER=0/1 사이에서 "같은 이름의 두 값이
    //   다른 것을 세는" 상태가 되어 비교가 성립하지 않는다.
    //
    // ★ 가르는 기준이 둘이다 — 공격은 때린 쪽, 죽음·부활은 당한 쪽으로 간다.
    //   죽음을 때린 쪽으로 가르면 "플레이어가 뱀을 죽인 것"이 플레이어 죽음으로 세어진다.
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) MonsterCounters
    {
        Counter _attacksTotal;      // 몬스터가 휘두른 공격
        Counter _attacksRejected;   // 몬스터 공격이 거부된 수 — 사유를 안 가른다.
                                    //   AI 가 쿨타임·사거리를 먼저 보고 시도하므로 거의 0 이어야 한다.
                                    //   0 이 아니게 되면 그 전제가 깨진 것이고, 그때 사유별로 가른다.
        Counter _deathsTotal;       // 몬스터가 죽은 횟수
        Counter _respawnsTotal;     // 몬스터가 되살아난 횟수

        Counter _stateTransitions;  // AI 상태 전이 총수 — 한 상태가 나머지를 삼키면 여기가 먼저 멈춘다
        Counter _thinkCalls;        // 판단을 실제로 돌린 횟수 (0 이면 AI 가 아예 안 도는 것)

        Counter _spawnRerolls;          // 태어날 자리에 산 사람이 가까워 다시 뽑은 수 (처음 깔 때·부활 때 공통)
        Counter _spawnRerollExhausted;  // 다시 뽑기를 다 써서 마지막 자리에 둔 수 — 사람이 빽빽하면 오른다
    } _monster;
#endif

#if USE_EXP
    // ══════════════════════════════════════════════════════════════
    // 경험치·레벨 (USE_EXP)
    //   ★ _levelClamped 는 "행이 표 밖 레벨을 들고 있었다"를 센다. 표를 줄인 패치 뒤에나
    //     오르는 값이라 평소엔 0 이어야 한다. 접근자가 조용히 clamp 하지 않고 여기서 세는 이유다.
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) ExpCounters
    {
        Counter _granted;        // 경험치를 준 횟수 (몬스터를 잡은 막타 수와 같다)
        Counter _grantedAmount;  // 준 경험치 총량
        Counter _levelUps;       // 레벨이 오른 횟수 (한 번에 여러 칸 올라도 1)
        Counter _levelClamped;   // 불러온 레벨이 표 밖이라 내린 수
    } _exp;
#endif

#if USE_INVENTORY
    // ══════════════════════════════════════════════════════════════
    // 인벤토리 (USE_INVENTORY)
    //   ★ 거절은 사유마다 한 칸(배열) — 한 통에 담으면 제일 흔한 하나(가득)가 나머지를 삼킨다.
    //     배열로 둔 이유는 사유가 늘어도 노출 쪽이 표를 돌며 빠짐없이 내보내게 하려는 것이다.
    //   ★ 칸 패킷 수는 FlushInventory 한 곳에서만 센다 — 칸을 바꾸는 길이 몇이든 내보내는 길은 하나다.
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) InventoryCounters
    {
        Counter _itemsAdded;                                                  // 넣은 아이템 개수(칸 수가 아니라 개수)
        Counter _addRejected[static_cast<int>(InvAddResult::COUNT)];          // 넣기 거절, 사유별 ([OK] 칸은 안 쓴다)
        Counter _commandRejected[static_cast<int>(ChatCommandReject::COUNT)]; // 채팅 명령 거절, 사유별
        Counter _slotUpdatesSent;                                             // 칸 패킷(S2C_INVENTORY_SLOT) 보낸 수
        Counter _snapshotsSent;                                               // 전체 목록(S2C_INVENTORY) 보낸 수 — 입장마다 1
        Counter _itemsUsed;                                                   // 쓴 아이템 개수(성공)
        Counter _useRejected[static_cast<int>(UseItemFail::COUNT)];           // 쓰기 거절, 사유별

        // ── 4단계: DB (USE_DB_WORKER) — 게임 스레드가 센다. 워커 쪽은 아래 _dbItem ──
        //   등식(조용해진 뒤): _itemSavesQueued = _dbItem._saved + _dbItem._failed
        Counter _itemSavesQueued;                                             // 칸 저장을 워커 큐에 넣은 수(FlushInventory)
        Counter _itemSaveRejected;                                            // 아이템 저장 상한에 걸려 못 넣은 수 — 0 이어야 한다
        Counter _loadedSlots;                                                 // 불러와 가방에 넣은 칸 수
        Counter _loadSkipped[static_cast<int>(InvLoadSkip::COUNT)];           // 불러올 때 건너뛴 줄, 사유별 — 평소 0
    } _inventory;

    DBItemSaveCounters _dbItem;   // 워커 스레드 몫 — 위 등식의 오른쪽

#if USE_EQUIP
    // ══════════════════════════════════════════════════════════════
    // 장착 (USE_EQUIP) — 성공은 방향마다 한 칸, 거절은 사유마다 한 칸(배열).
    //   세는 곳은 CGameServer::ApplyEquip 한 곳이다(끼우기·빼기가 같은 길을 탄다).
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) EquipCounters
    {
        Counter _equipped;                                          // 끼우기 성공
        Counter _unequipped;                                        // 빼기 성공
        Counter _rejected[static_cast<int>(EquipFail::COUNT)];      // 거절, 사유별
    } _equip;
#endif
#endif

#if USE_DROP
    // ══════════════════════════════════════════════════════════════
    // 드랍 — 땅의 아이템 (USE_DROP)
    //   ★ 등식(조용해진 뒤): _dropped = _removed[PICKED] + _removed[EXPIRED] + _removed[CLEANED] + _groundItems(게이지)
    //     땅에서 빼는 자리가 셋이라 한 곳만 안 세도 등식이 깨진다 — 그래서 빼는 몫은 사유마다 한 칸이다.
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) DropCounters
    {
        Counter _rolls;         // 드랍 표를 굴린 수 = 사람이 몬스터를 잡은 수 (아무것도 안 떨어져도 1)
        Counter _dropped;       // 땅에 생긴 아이템 수 (한 번 굴려 여럿이면 여럿)
        Counter _removed[static_cast<int>(GroundItemRemoval::COUNT)];   // 땅에서 빠진 수, 사유별
        Counter _groundItems;   // [게이지] 지금 땅에 있는 수(전 존 합계) — 존마다 땅 아이템 관리자가 바꾸는 자리에서 더하고 뺀다
        Counter _pickupRejected[static_cast<int>(PickupFail::COUNT)];   // 줍기 거절, 사유별
        Counter _lateMoverDeletes;   // 같은 틱에 섹터를 넘은 사람에게 보정으로 보낸 DELETE 수
    } _drop;
#endif

    // ══════════════════════════════════════════════════════════════
    // 입장 — 캐릭터 불러오기 (USE_DB_WORKER)
    //
    // ★ 전부 게임 루프 스레드가 센다. DB 워커는 결과에 코드만 실어 보내고, 세는 일은
    //   게임 루프가 결과를 꺼낼 때 한다 → 쓰는 스레드가 하나고, 저장 지표(_db — 10-06 코어에서 옮김)의
    //   mmo_db_saved_total·mmo_db_failed_total 은 계속 "저장"만 센다(같은 이름이 두 가지를
    //   세면 기존 게이트가 흐려진다).
    //
    // ★ 맞아야 하는 등식 — 조용해진 뒤 requested = found + newCharacter + failed + orphaned.
    //   왼쪽은 요청을 넣는 자리, 오른쪽은 결과를 꺼내는 자리에서 따로 센다. 그래서 워커가
    //   어느 갈래에서 결과를 빠뜨리면 등식이 영영 안 맞는다. 갈래마다 계수기를 둔 이유다 —
    //   한 통에 담으면 제일 흔한 하나(찾음)가 나머지를 삼킨다.
    //
    // USE_DB_WORKER=0 이어도 계수기는 남아 0 으로 노출된다(전투 계수기와 같은 이유 —
    //   "꺼진 것"과 "노출을 빠뜨린 것"이 화면에서 똑같아 보이지 않게).
    //   단 _inGame 하나는 예외다 — 토글과 무관하게 게임 루프가 늘 갱신하므로 실제 인원이 나온다.
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) EnterCounters
    {
        Counter _requested;      // 조회를 워커 큐에 넣은 수
        Counter _rejected;       // 불러오기 상한에 걸려 큐에 못 넣은 수 (requested 에 안 들어간다)

        Counter _found;          // 결과: 행이 있었다
        Counter _newCharacter;   // 결과: 행이 없었다 (새 캐릭터 — 정상)
        Counter _failed;         // 결과: 조회 실패 ("없음"과 섞이면 안 되는 갈래)
        Counter _orphaned;       // 결과가 왔는데 주인(세션)이 이미 나가고 없다 — 코드는 안 본다

        // [2단계] 등식 밖의 다섯 — 위 등식에 더하지 않는다.
        Counter _fallback;       // 찾았지만 맵·좌표가 못 쓸 값이라 무작위 자리에 넣었다 (_found 의 부분집합)
        Counter _duplicate;      // [입장] 같은 계정이 이미 있어 옛 세션을 걷어낸 수 (게임 중·불러오는 중 양쪽)
        Counter _pending;        // [게이지] 지금 불러오는 중인 세션 수. 조용해지면 0 — 안 내려가면 대기표가 샌 것이다
        Counter _waiting;        // [게이지] 접속은 했는데 입장 요청을 아직 안 보낸 세션 수 (USE_LOGIN=1 에서만 0 이 아니다)
                                 //   ★ _pending 과 합치지 않는다 — 막히는 까닭이 다르다(클라 결함 / DB 이상).
                                 //   합치면 아래 등식은 그대로 서지만 "입장 요청을 안 보내는 클라"를 못 본다.
                                 //   네트워크 계층의 유휴 타임아웃은 하트비트만 보내도 갱신돼서 이런 세션을 안 끊는다.
                                 //   그래서 이 게이지가 그 구멍의 탐지기다 — 올라가서 안 내려오면 쓸어내기를 넣는다.
        Counter _inGame;         // [게이지] 지금 게임 중인 플레이어 수 (존 정원 카운터의 합, 약 0.5초마다 갱신)
                                 //   조용해지면 동접(mmo_session_count) = _inGame + _pending + _waiting 이어야 한다.
                                 //   세션은 없는데 플레이어만 남은 것(나간 세션의 결과로 스폰된 유령)은 이 등식으로만 보인다.

        // 요청 → 결과 수령 지연. 평균은 튄 값을 못 보므로 구간별 개수로 담는다(비누적, 노출 때 누적 변환).
        //   개수 합 = 꺼낸 결과 전부 = found + newCharacter + failed + orphaned (등식의 둘째 대조군).
        //   첫 경계가 40ms 가 아니라 50ms 인 이유: 결과는 "다음 틱"에 꺼내므로 정상값이 틱 주기(40ms)
        //   바로 위에 몰린다(실측 평균 40.5ms). 경계를 40 에 두면 같은 정상값이 두 칸으로 갈려 읽을 수 없다.
        //   첫 칸 = "다음 틱에 돌아왔다"로 읽는다.
        static constexpr int     LATENCY_BUCKET_COUNT = 5;   // 마지막 칸 = +Inf
        static constexpr int64_t LATENCY_BOUNDS_US[LATENCY_BUCKET_COUNT - 1] = { 50000, 100000, 500000, 2000000 };
        static constexpr const char* LATENCY_LE[LATENCY_BUCKET_COUNT - 1]    = { "0.05", "0.1", "0.5", "2" };
        Counter _latencyBuckets[LATENCY_BUCKET_COUNT];
        Counter _latencySumUs;

        void RecordLatencyUs(int64_t us)
        {
            if (us < 0)
                us = 0;
            int i = 0;
            while (i < LATENCY_BUCKET_COUNT - 1 && us > LATENCY_BOUNDS_US[i])
                ++i;
            _latencyBuckets[i].Inc();
            _latencySumUs.Add(us);
        }
    } _enter;

    // ══════════════════════════════════════════════════════════════
    // 섹터·존 — 이 게임의 규칙 이름이 든 지표. 10-06 코어 GameLoopCounters 에서 옮김(아키텍처 전수검사 8번).
    //   코어에 남긴 것은 어느 게임 루프에나 있는 값(틱·단계 시간·이벤트 큐·브로드캐스트 호출·비용·분포)뿐이다.
    //   게임 루프 스레드만 쓴다(틱 끝에 한 번 더한다).
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) SectorCounters
    {
        Counter _zoneChangeCount;       // 존 이동 횟수

        // 멤버십 변경(섹터 이동·입장·퇴장) CREATE/DELETE 복사 횟수 — _broadcastTargets(계측 대상 복사)와 비교해
        //   비계측 복사의 비중을 가늠한다.
        Counter _membershipSends;

        // 멤버십 변경 송신 시간(µs) — 틱 끝 섹터 이동 구간 전용. game_logic 단계에 섞여 있던 것을 분리해 잰다.
        //   접속·퇴장(BroadcastEnter/LeaveZone)은 네트워크 단계라 안 들어간다.
        Counter _membershipCostUs;

        // 같은 틱 이동자 쌍 보정 건수 — 틱 끝 일괄 통보가 놓친 CREATE/DELETE 를 보충한 수.
        //   0 에 수렴하면 보정이 불필요한 부하, 지속 증가면 정상(이동자 밀집도에 비례).
        Counter _membershipPairFixes;

        // C2S_MOVE_START 좌표 수용을 이동량 예산 초과로 거부한 횟수 — 정직한 클라는 0.
        //   0 이 아니면 치트 시도이거나 예산 계수(MOVE_BUDGET_SLACK)가 빡빡하다는 신호.
        Counter _moveBudgetRejects;
    } _sector;

    // DB 저장 파이프라인 — 정의는 위 DBSaveCounters(DB 워커가 참조로 받는다)
    DBSaveCounters _db;
};
