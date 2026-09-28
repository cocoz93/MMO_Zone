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

#if USE_DROP
// 땅의 아이템이 땅에서 빠진 까닭 — 와이어의 GroundItemGone 과 다르다(그쪽의 OUT_OF_SIGHT 는 빠진 게 아니라 안 보이는 것,
//   CLEANED 는 알릴 사람이 없어 와이어에 안 나간다). 빼는 자리마다 한 칸이라 등식이 한 곳의 누락도 드러낸다.
enum class GroundItemRemoval : uint8_t
{
    PICKED,    // 누가 주웠다
    EXPIRED,   // 오래 안 주워 사라졌다
    CLEANED,   // 사람이 없는 채널이 지워지며 같이 사라졌다
    COUNT
};

inline const char* GroundItemRemovalName(GroundItemRemoval r)
{
    static_assert(static_cast<int>(GroundItemRemoval::COUNT) == 3, "GroundItemRemoval 을 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case GroundItemRemoval::PICKED:  return "picked";
    case GroundItemRemoval::EXPIRED: return "expired";
    case GroundItemRemoval::CLEANED: return "cleaned";
    case GroundItemRemoval::COUNT:   break;
    }
    return "?";
}

// 줍기 거절 사유의 지표 라벨 — 사유 enum 은 와이어에 실려 Protocol.h 에 있다
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

// 아이템 저장 계수기 — DB 워커 스레드가 센다(DBWorker.h 가 앞 선언으로 가리킨다).
//   ★ 게임 스레드가 세는 _inventory 와 따로 둔다 — 두 스레드가 같은 캐시 줄을 번갈아 쓰지 않게(alignas(64)).
struct alignas(64) DBItemSaveCounters
{
    Counter _saved;     // 칸 저장 성공
    Counter _failed;    // 재연결 뒤 다시 해 봐도 실패 — 0 이어야 한다(0 이 아니면 그 칸은 다음 변화 전까지 DB 와 다르다)
    Counter _retried;   // 재연결하고 다시 써 본 수(성공·실패 무관) — DB 가 한 번 끊겼다 살아난 흔적
};
#endif

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
        Counter _groundItems;   // [게이지] 지금 땅에 있는 수 — 바꾸는 자리에서 Store
        Counter _pickupRejected[static_cast<int>(PickupFail::COUNT)];   // 줍기 거절, 사유별
        Counter _lateMoverDeletes;   // 같은 틱에 섹터를 넘은 사람에게 보정으로 보낸 DELETE 수
    } _drop;
#endif

    // ══════════════════════════════════════════════════════════════
    // 입장 — 캐릭터 불러오기 (USE_DB_WORKER)
    //
    // ★ 전부 게임 루프 스레드가 센다. DB 워커는 결과에 코드만 실어 보내고, 세는 일은
    //   게임 루프가 결과를 꺼낼 때 한다 → 쓰는 스레드가 하나고, 코어(ServerCore)의
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
};
