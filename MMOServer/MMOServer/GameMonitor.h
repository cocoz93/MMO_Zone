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
#include "ItemSystem.h"       // USE_INVENTORY — 쓰기·장착 거절 사유(UseItemReject·EquipReject) 수만큼 칸을 잡는다
#include "SkillSystem.h"      // USE_SKILL — 스킬 거절 사유(SkillReject) 수만큼 칸을 잡는다
#include "Buff.h"             // USE_COMBAT — 지속 효과 종류(BuffKind)·끝난 사유(BuffEnd) 수만큼 칸을 잡는다
#include "SkillTable.h"       // USE_SKILL — 스킬 번호 수(MAX_SKILL_ID)만큼 시전 칸을 잡는다
#include "MapManager.h"       // 존 이동 요청 결과(ZoneChangeReject — 맵 관리자의 어휘) 수만큼 칸을 잡는다
#include "PartySystem.h"      // USE_PARTY — 빠진 까닭(PartyLeave)·요청이 끝난 까닭(PartyRequestEnd) 수만큼 칸을 잡는다
#include "Room.h"             // USE_PARTY — 방 들어가기 결과(RoomEntryReject) 수만큼 칸을 잡는다

// [사유 enum 의 집 — 규칙 하나] 사유는 그것을 정하는 층의 헤더에 산다(10-06, 아키텍처 전수검사 7번).
//   존·엔티티 층이 정하는 것: GroundItemRemoval·PickupReject(GroundItem.h) · ZoneChangeReject(MapManager.h) · InvAddResult(Inventory.h)
//   규칙 클래스가 정하는 것: UseItemReject·EquipReject(ItemSystem.h — 10-08 판정이 게임 서버에서 CItemSystem 으로 옮겨 감) ·
//                          SkillReject(SkillSystem.h)
//   게임 서버가 정하는 것: PickupFail(와이어 enum 그대로 — 정하는 쪽이 패킷을 아는 층이라 변환이 없다) ·
//                        InvLoadSkip·ChatCommandReject·PortalReject(계수기만 쓰는 사유라 여기)
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

// 존 이동 요청 결과의 지표 라벨 — 사유 enum 은 맵 관리자가 정한다(MapManager.h ZoneChangeReject). NONE 칸이 성공이다 —
//   성공·거절을 한 배열로 세야 「요청 수 = 칸의 합」이 되고, 한 갈래를 빼먹으면 합이 안 맞아 드러난다. 세는 곳은 RecvZoneChange 한 곳.
inline const char* ZoneChangeResultName(ZoneChangeReject r)
{
    static_assert(static_cast<int>(ZoneChangeReject::COUNT) == 5, "ZoneChangeReject 를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case ZoneChangeReject::NONE:      return "ok";
    case ZoneChangeReject::NOT_FOUND: return "not_found";
    case ZoneChangeReject::FULL:      return "full";
    case ZoneChangeReject::SAME_ZONE: return "same_zone";
    case ZoneChangeReject::NOT_ALLOWED: return "not_allowed";
    case ZoneChangeReject::COUNT:     break;
    }
    return "?";
}
constexpr int ZONE_CHANGE_RESULT_COUNT = static_cast<int>(ZoneChangeReject::COUNT);

// 포털 사용 거절 사유 — 게임 서버(RecvPortalUse)가 정하고 계수기만 쓴다(와이어 값은 S2C_ZONE_CHANGE_FAIL 의 4·5 로 묶인다 —
//   클라에겐 「없음」과 「다른 맵의 포털」이 같은 답이지만, 세는 쪽에선 갈라 둬야 클라 버그인지 사람 조작인지 읽힌다).
enum class PortalReject : uint8_t
{
    NO_PORTAL,   // 포털 표에 없는 번호
    WRONG_MAP,   // 지금 맵의 포털이 아님
    TOO_FAR,     // 반경 밖
    DEAD,        // 죽은 채 — 답을 안 보낸다(존 이동 요청과 같다)
    ROOM,        // 방 맵 포털인데 방 판정에 걸림 — 사유는 mmo_room_entries_total(Room.h RoomEntryReject). 파티를 끈 빌드는 방 맵 포털이 늘 여기
    COUNT
};

inline const char* PortalRejectName(PortalReject r)
{
    static_assert(static_cast<int>(PortalReject::COUNT) == 5, "PortalReject 를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case PortalReject::NO_PORTAL: return "no_portal";
    case PortalReject::WRONG_MAP: return "wrong_map";
    case PortalReject::TOO_FAR:   return "too_far";
    case PortalReject::DEAD:      return "dead";
    case PortalReject::ROOM:      return "room";
    case PortalReject::COUNT:     break;
    }
    return "?";
}
constexpr int PORTAL_REJECT_COUNT = static_cast<int>(PortalReject::COUNT);

#if USE_PARTY
// 파티 지표 라벨 — 넷 다 파티가 정하는 어휘다. 빠진 까닭·요청이 끝난 까닭은 규칙 클래스(PartySystem.h)가,
//   요청 종류·알림 번호는 와이어 enum(Protocol.h — 규칙 클래스가 그대로 쓴다, PartySystem.h [와이어 enum 을 그대로 쓴다])이 정한다.
inline const char* PartyLeaveName(PartyLeave r)
{
    static_assert(static_cast<int>(PartyLeave::COUNT) == 4, "PartyLeave 를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case PartyLeave::LEAVE:        return "leave";
    case PartyLeave::KICKED:       return "kicked";
    case PartyLeave::DISCONNECTED: return "disconnected";
    case PartyLeave::DISBANDED:    return "disbanded";
    case PartyLeave::COUNT:        break;
    }
    return "?";
}

inline const char* PartyRequestEndName(PartyRequestEnd r)
{
    static_assert(static_cast<int>(PartyRequestEnd::COUNT) == 4, "PartyRequestEnd 를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case PartyRequestEnd::ACCEPTED:  return "accepted";
    case PartyRequestEnd::DECLINED:  return "declined";
    case PartyRequestEnd::EXPIRED:   return "expired";
    case PartyRequestEnd::CANCELLED: return "cancelled";
    case PartyRequestEnd::COUNT:     break;
    }
    return "?";
}

inline const char* PartyAskKindName(PartyAskKind r)
{
    static_assert(static_cast<int>(PartyAskKind::COUNT) == 3, "PartyAskKind 를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case PartyAskKind::INVITE:  return "invite";
    case PartyAskKind::APPLY:   return "apply";
    case PartyAskKind::SUGGEST: return "suggest";
    case PartyAskKind::COUNT:   break;
    }
    return "?";
}

// 방 들어가기 결과 — 판정(Room.h CheckRoomEntry)과 파티·방 조각(번호 다 참 — CPartyHandler::EnterRoom)이 정한다. NONE 칸 = 방을 만들어 옮김
inline const char* RoomEntryResultName(RoomEntryReject r)
{
    static_assert(static_cast<int>(RoomEntryReject::COUNT) == 7, "RoomEntryReject 를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case RoomEntryReject::NONE:         return "entered";
    case RoomEntryReject::NOT_IN_PARTY: return "not_in_party";
    case RoomEntryReject::NOT_LEADER:   return "not_leader";
    case RoomEntryReject::NOT_GATHERED: return "not_gathered";
    case RoomEntryReject::DEAD:         return "dead";
    case RoomEntryReject::LOCKED:       return "locked";
    case RoomEntryReject::FULL:         return "full";
    case RoomEntryReject::COUNT:        break;
    }
    return "?";
}

inline const char* PartyNoticeName(PartyNotice r)
{
    static_assert(static_cast<int>(PartyNotice::COUNT) == 25, "PartyNotice 를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case PartyNotice::SENT_INVITE:   return "sent_invite";
    case PartyNotice::SENT_APPLY:    return "sent_apply";
    case PartyNotice::SENT_SUGGEST:  return "sent_suggest";
    case PartyNotice::NO_TARGET:     return "no_target";
    case PartyNotice::SELF:          return "self";
    case PartyNotice::SAME_PARTY:    return "same_party";
    case PartyNotice::BOTH_IN_PARTY: return "both_in_party";
    case PartyNotice::FULL:          return "full";
    case PartyNotice::NOT_LEADER:    return "not_leader";
    case PartyNotice::NOT_IN_PARTY:  return "not_in_party";
    case PartyNotice::NOT_MEMBER:    return "not_member";
    case PartyNotice::TARGET_BUSY:   return "target_busy";
    case PartyNotice::SELF_BUSY:     return "self_busy";
    case PartyNotice::NO_REQUEST:    return "no_request";
    case PartyNotice::DECLINED:      return "declined";
    case PartyNotice::EXPIRED:       return "expired";
    case PartyNotice::CANCELLED:     return "cancelled";
    case PartyNotice::JOINED:        return "joined";
    case PartyNotice::LEFT:          return "left";
    case PartyNotice::KICKED:        return "kicked";
    case PartyNotice::LEADER:        return "leader";
    case PartyNotice::DISBANDED:     return "disbanded";
    case PartyNotice::ROOM_NOT_GATHERED: return "room_not_gathered";
    case PartyNotice::ROOM_DEAD:     return "room_dead";
    case PartyNotice::ROOM_LOCKED:   return "room_locked";
    case PartyNotice::COUNT:         break;
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
    DEAD,              // 쓰러진 채다 — /gm 이 체력을 가득 채우면 부활 절차 없이 되살아난다
    COUNT
};

inline const char* ChatCommandRejectName(ChatCommandReject r)
{
    static_assert(static_cast<int>(ChatCommandReject::COUNT) == 4, "사유를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case ChatCommandReject::NOT_ADMIN:       return "not_admin";
    case ChatCommandReject::UNKNOWN_COMMAND: return "unknown_command";
    case ChatCommandReject::BAD_ARGS:        return "bad_args";
    case ChatCommandReject::DEAD:            return "dead";
    case ChatCommandReject::COUNT:           break;
    }
    return "?";
}

// 불러올 때 건너뛴 줄의 사유 — DB 행이 망가졌거나 표가 바뀌어 지금 규칙에 안 맞는 줄. 평소엔 전부 0 이다.
//   칸·아이템 번호 사유는 게임 서버(LoadInventoryRows)가, 탭·개수 사유는 가방(LoadSlot → InvLoadResult)이 정한다.
//   두 층의 사유를 한 계수기로 세므로 라벨은 여기 두고, 가방 사유는 게임 서버가 옮긴다(10-07).
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

// 쓰기·장착 거절의 지표 라벨 — 사유 enum 은 CItemSystem 이 정한다(ItemSystem.h). 와이어 값으로는 게임 서버가 바꾼다.
inline const char* UseItemRejectName(UseItemReject r)
{
    static_assert(static_cast<int>(UseItemReject::COUNT) == 7, "UseItemReject 를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case UseItemReject::BAD_SLOT:   return "bad_slot";
    case UseItemReject::EMPTY:      return "empty";
    case UseItemReject::NOT_USABLE: return "not_usable";
    case UseItemReject::DEAD:       return "dead";
    case UseItemReject::COOLDOWN:   return "cooldown";
    case UseItemReject::FULL_HP:    return "full_hp";
    case UseItemReject::FULL_MP:    return "full_mp";
    case UseItemReject::COUNT:      break;
    }
    return "?";
}

inline const char* EquipRejectName(EquipReject r)
{
    static_assert(static_cast<int>(EquipReject::COUNT) == 6, "EquipReject 를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case EquipReject::BAD_SLOT:       return "bad_slot";
    case EquipReject::EMPTY:          return "empty";
    case EquipReject::NOT_EQUIPPABLE: return "not_equippable";
    case EquipReject::DEAD:           return "dead";
    case EquipReject::COOLDOWN:       return "cooldown";
    case EquipReject::TAB_FULL:       return "tab_full";
    case EquipReject::COUNT:          break;
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

#if USE_COMBAT
// 지속 효과의 지표 라벨 둘 — 종류(BuffKind, 와이어 enum — Protocol.h)와 끝난 사유(BuffEnd — Buff.h).
//   종류 이름은 스킬 표의 effect 글자 「buff_<종류>」 뒤쪽과 같게 둔다(웹 proto.js BUFF_KIND 도 같은 이름).
inline const char* BuffKindName(BuffKind k)
{
    static_assert(static_cast<int>(BuffKind::COUNT) == 1, "BuffKind 를 늘렸으면 이름도 붙일 것");
    switch (k)
    {
    case BuffKind::ATTACK_MUL: return "attack_mul";
    case BuffKind::COUNT:      break;
    }
    return "?";
}

inline const char* BuffEndName(BuffEnd e)
{
    static_assert(static_cast<int>(BuffEnd::COUNT) == 3, "BuffEnd 를 늘렸으면 이름도 붙일 것");
    switch (e)
    {
    case BuffEnd::EXPIRED: return "expired";
    case BuffEnd::DEATH:   return "death";
    case BuffEnd::LEAVE:   return "leave";
    case BuffEnd::COUNT:   break;
    }
    return "?";
}
#endif

#if USE_SKILL
// 스킬 거절의 지표 라벨 — 사유 enum 은 CSkillSystem 이 정한다(SkillSystem.h). 와이어 값으로는 게임 서버가 바꾼다.
//   ★ 위 USE_INVENTORY 블록 밖이어야 한다 — 스킬은 가방과 따로 켜고 끈다(가방을 끈 빌드에서 이름표가 사라졌던 판, 10-10 토글 빌드).
inline const char* SkillRejectName(SkillReject r)
{
    static_assert(static_cast<int>(SkillReject::COUNT) == 7, "SkillReject 를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case SkillReject::BAD_SKILL: return "bad_skill";
    case SkillReject::NOT_FOR_CASTER: return "not_for_caster";
    case SkillReject::DEAD:      return "dead";
    case SkillReject::COOLDOWN:  return "cooldown";
    case SkillReject::LOW_HP:    return "low_hp";
    case SkillReject::LOW_MP:    return "low_mp";
    case SkillReject::NO_TARGET: return "no_target";
    case SkillReject::COUNT:     break;
    }
    return "?";
}
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
        // 부활 자리 — 둘의 합 = _respawnsTotal(TickRespawn 한 곳이 셋을 같이 센다). 제자리 = 죽은 맵에서 그대로,
        //   마을로 = 다른 맵(마을)으로 옮겨 감
        Counter _respawnInPlace;
        Counter _respawnToTown;

        // 피해가 최소 1 로 올려진 타격 — 공격력 − 방어력이 1 미만이었다. ★ 위 값들과 달리 때린 쪽을 안 가른다
        //   (뱀이 방어력 높은 사람을 문 것도 센다). 지금 레벨 표로는 안 닿아 평소 0 이다.
        Counter _damageFloored;

        // S2C_MY_STATS 보낸 수 — SendMyStats 한 곳에서만 센다. 입장마다 1 이라 더미 시험의 분모다.
        Counter _myStatsSent;
    } _combat;

#if USE_COMBAT
    // ══════════════════════════════════════════════════════════════
    // 지속 효과 (USE_COMBAT) — 세는 곳은 CBuffSystem 안이다(걸림·되감김 = Apply, 되살림 = Restore, 끝남 = Tick·ClearAll·Forget).
    //   ★ 등식(조용해진 뒤): Σ _applied + Σ _restored = Σ _ended + _active — 끝나는 자리가 셋이라 사유마다 한 칸이다(드랍 등식과 같은 꼴).
    //     켜지는 자리도 둘이다(스킬로 걸림 · 입장 때 DB 저장값으로 되살림) — 되살림을 안 세면 그 효과가 끝날 때 오른쪽만 늘어 등식이 깨진다.
    //     한 자리라도 안 세면 등식이 영영 안 맞는다. _refreshed 는 등식 밖이다(칸이 이미 켜져 있어 _active 가 안 변한다).
    //   토글 안에 두는 까닭 — 칸 수(BuffKind·BuffEnd)가 Buff.h 의 토글 안에 있다(스킬 계수기와 같은 꼴).
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) BuffCounters
    {
        Counter _applied[BUFF_KIND_COUNT];                      // 새로 걸림(빈 칸이 켜짐), 종류별
        Counter _refreshed[BUFF_KIND_COUNT];                    // 켜진 칸을 덮음(되감기 — 값·시간 다시), 종류별
        Counter _restored[BUFF_KIND_COUNT];                     // 입장 때 DB 저장값으로 빈 칸을 채움(CBuffSystem::Restore), 종류별 — 걸림과 따로 센다
        Counter _ended[static_cast<int>(BuffEnd::COUNT)];      // 끝남, 사유별(만료·죽음·퇴장)
        Counter _active;                                        // [게이지] 지금 켜져 있는 칸 수(전원 합계) — 켤 때 +1, 끝낼 때 −1

        // S2C_ACTOR_BUFF 보낸 수 — SendActorBuff 한 곳에서만 센다(_myStatsSent 와 같은 꼴). 걸림·되감김·만료·죽음·존 이동마다 1.
        Counter _actorBuffSent;
    } _buff;
#endif

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

        Counter _stateTransitions;  // AI 상태 전이 총수 — 한 상태가 나머지를 삼키면 여기가 먼저 멈춘다
        Counter _thinkCalls;        // 판단을 실제로 돌린 횟수 (0 이면 AI 가 아예 안 도는 것)

        Counter _spawnedTotal;          // 깐 몬스터 수 — 처음 깔기와 주기 리젠·되살리기 공통.
                                        //   깐 수 − 죽은 수(_deathsTotal) = 지금 몬스터 수(빈 채널 정리로 같이 사라진 것과
                                        //   방 클리어로 사라진 것 — mmo_room_minions_cleared_total — 만 예외).
                                        //   지운 수는 따로 안 센다 — 죽는 순간 지우므로 죽은 수와 늘 같다
                                        //   (사람 근처를 피해 다시 뽑던 계수기 둘은 그 검사와 함께 없앴다 — 10-09)
        Counter _revivedTotal;          // 되살리기 스킬(SkillEffect::REVIVE)로 깐 수 — _spawnedTotal 에도 든다. 세는 곳은 몬스터 관리자의 리젠(RegenDue)
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
        Counter _gmToggled[2];                                                // /gm 으로 바꾼 수 — [0] 끔 · [1] 켬(바뀐 뒤 상태로 센다)
        Counter _slotUpdatesSent;                                             // 칸 패킷(S2C_INVENTORY_SLOT) 보낸 수
        Counter _snapshotsSent;                                               // 전체 목록(S2C_INVENTORY) 보낸 수 — 입장마다 1
        Counter _itemsUsed;                                                   // 쓴 아이템 개수(성공)
        Counter _useRejected[static_cast<int>(UseItemReject::COUNT)];         // 쓰기 거절, 사유별

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
    //   세는 곳은 CItemSystem::Equip 한 곳이다(끼우기·빼기가 같은 길을 탄다).
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) EquipCounters
    {
        Counter _equipped;                                          // 끼우기 성공
        Counter _unequipped;                                        // 빼기 성공
        Counter _rejected[static_cast<int>(EquipReject::COUNT)];    // 거절, 사유별
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

#if USE_SKILL
    // ══════════════════════════════════════════════════════════════
    // 스킬 (USE_SKILL) — 시전·맞힌 수는 스킬 번호마다, 거절은 사유마다 한 칸(배열).
    //   세는 곳은 CSkillSystem 안이다(시전·거절 = UseSkill, 맞힌 수 = ApplyEffect). 시전 = 비용을 뗀 수(모든 검사를 지났다), 거절 = 아무것도 안 뗀 수.
    //   죽인 수는 여기서 안 센다 — 맞힌 뒤 처리(ApplyHit)가 일반 공격과 같은 죽음 지표로 센다.
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) SkillCounters
    {
        Counter _cast[MAX_SKILL_ID + 1];                            // 시전 성공, 스킬 번호별([0] 칸은 안 쓴다)
        Counter _rejected[static_cast<int>(SkillReject::COUNT)];    // 거절, 사유별
        Counter _hits[MAX_SKILL_ID + 1];                            // 스킬로 맞힌 대상 수, 스킬 번호별 — 범위 스킬은 한 시전에 여럿([0] 칸은 안 쓴다). 피해 스킬만(지속 효과는 _buff 가 센다)

        // 몬스터(보스)가 쓴 몫 — 위 셋과 같은 칸 모양이다. 위 셋은 **사람 몫** 그대로 두고(이름·뜻 불변 — 대시보드 호환) 몬스터 것은 따로 센다.
        //   안 가르면 보스의 시전이 사람 시전 수에 섞인다(SkillSystem.cpp UseSkill 의 약속, 10-13). 가르는 곳은 CSkillSystem 한 곳(시전자 종류).
        Counter _monsterCast[MAX_SKILL_ID + 1];
        Counter _monsterRejected[static_cast<int>(SkillReject::COUNT)];
        Counter _monsterHits[MAX_SKILL_ID + 1];

        // S2C_ACTOR_MP 보낸 수 — SendActorMp 한 곳에서만 센다(_combat._myStatsSent 와 같은 꼴). 입장·존 이동·레벨업·마력 소모·마력 물약마다 1.
        //   마력은 스킬 토글 소유라 여기 둔다.
        Counter _actorMpSent;

        // S2C_SKILL_COOLDOWN_REMAIN 보낸 수 — SendActiveSkillCooldowns 한 곳에서만 센다. 입장·존 이동 때 돌고 있는 쿨타임 칸마다 1.
        Counter _cooldownRemainSent;
    } _skill;
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

        // [2단계] 등식 밖의 여섯 — 위 등식에 더하지 않는다.
        Counter _fallback;       // 찾았지만 맵·좌표가 못 쓸 값이라 그 맵의 들어오는 자리(시작 구역 · 없으면 무작위)에 넣었다 (_found 의 부분집합)
        Counter _deadRespawn;    // 찾았지만 죽은 채 저장된 행(체력 0)이라 부활 규칙대로 가득 채워 넣었다 — 마을 밖이면 마을 시작 구역 (_found 의 부분집합)
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

        // [쿨타임·버프 되살리기] 칸 단위 — 위 등식과 무관하다(행 하나에 칸이 여럿). 세는 곳은 CCharacterStore::RestoreCooldownsAndBuffs 한 곳.
        //   버프를 되살린 수는 여기가 아니라 _buff._restored 다 — 버프 등식(켜짐 = 끝남 + 지금)의 한 항이라 거기서 센다.
        //   죽은 채 저장된 행의 버프는 풀지 않고 버린다 — 칸 단위로 안 세고 행 단위 _deadRespawn 이 센다.
        Counter _cooldownRestored;   // 되살린 쿨타임 칸
        Counter _timerExpired;       // 저장값이 이미 지나 버린 칸(쿨타임·버프) — 접속하지 않은 동안 끝났다(정상)
        Counter _timerSkipped;       // 내용이 틀려 건너뛴 칸 — 표에 없는 스킬 번호 · 범위 밖 버프 종류·값 · 같은 칸이 둘 · 버프 남은 시간이 상한 초과
        Counter _timerBad;           // 형식이 틀려 통째로 버린 열(쿨타임 열·버프 열 따로 1씩) — 버전 모름 · 길이 불일치 · 버퍼보다 긴 값(잘림)

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
        Counter _zoneChangeCount;       // 존 이동 횟수 — 옮기는 한 곳(TransferPlayer)이 센다. 까닭(요청·포털·부활)을 안 가린다

        // 존 이동 요청(C2S_ZONE_CHANGE) 결과 — 칸 = ZoneChangeReject(NONE = 성공). 칸의 합 = 목적지를 고른 요청 수(RecvZoneChange 한 곳).
        //   그 앞에서 버린 요청(죽은 채 · 세션·존 없음)은 답도 안 가고 여기에도 안 든다
        Counter _zoneChangeResults[ZONE_CHANGE_RESULT_COUNT];

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

    // ══════════════════════════════════════════════════════════════
    // 포털 — 세는 곳은 RecvPortalUse 한 곳. 칸의 합(_used + Σ _rejected) = 받은 C2S_PORTAL_USE 수(방어 갈래 둘 빼고 — 그 함수 주석).
    //   토글 없음 — 맵 표처럼 세계 데이터다(PortalTable.h). 포털 표가 비면 늘 0 이다.
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) PortalCounters
    {
        Counter _used;                              // 포털로 옮겨 감
        Counter _rejected[PORTAL_REJECT_COUNT];     // 거절, 사유별(PortalReject)
    } _portal;

#if USE_PARTY
    // ══════════════════════════════════════════════════════════════
    // 파티 (USE_PARTY) — 세는 곳은 CPartySystem 안(보내기 몫 넷만 파티·방 조각 CPartyHandler 의 알림 함수 한 곳씩).
    //   ★ 등식 셋(조용해진 뒤) — 바꾸는 자리가 여럿이라 한 곳만 안 세도 영영 안 맞는다:
    //     _created = _ended + _active · _memberAdded = Σ _memberRemoved + _members · Σ _requests = Σ _requestsEnded + _pending
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) PartyCounters
    {
        Counter _active;                                                          // [게이지] 지금 파티 수
        Counter _members;                                                         // [게이지] 지금 파티원 수(전 파티 합)
        Counter _created;                                                         // 새 파티(초대가 수락돼 둘이 됨)
        Counter _ended;                                                           // 사라진 파티(해산 · 한 명만 남음)
        Counter _memberAdded;                                                     // 들어온 수 — 새 파티의 둘 포함
        Counter _memberRemoved[static_cast<int>(PartyLeave::COUNT)];              // 빠진 수, 까닭별
        Counter _requests[static_cast<int>(PartyAskKind::COUNT)];                 // 요청 표에 넣은 수, 종류별 — 추천이 승인돼 이어진 초대도 하나
        Counter _requestsEnded[static_cast<int>(PartyRequestEnd::COUNT)];         // 요청이 끝난 수, 까닭별
        Counter _pending;                                                         // [게이지] 지금 요청 표 크기
        Counter _notices[static_cast<int>(PartyNotice::COUNT)];                   // 보낸 알림, 번호별 — 거절 사유(NO_TARGET~NO_REQUEST)도 여기
        Counter _infoSent;                                                        // S2C_PARTY_INFO 보낸 수(게임 서버 한 곳)
        Counter _memberSent;                                                      // S2C_PARTY_MEMBER 보낸 수(게임 서버 한 곳)
        Counter _askSent;                                                         // S2C_PARTY_ASK 보낸 수(게임 서버 한 곳) — Σ _requests 와 같아야 한다
        Counter _askEndSent;                                                      // S2C_PARTY_ASK_END 보낸 수(게임 서버 한 곳)
        // 공격 후보에서 같은 파티라 건너뛴 수 — 후보 함수가 둘이라 둘(세는 자리도 갈래다).
        //   Front = FindFrontTarget(평타·백호참 — 사거리 안의 파티원) · Area = CollectCandidates(범위 스킬 — 모양 판정 전, 훑은 반경 안의 파티원)
        Counter _mateSkippedFront;
        Counter _mateSkippedArea;
    } _party;

    // ══════════════════════════════════════════════════════════════
    // 파티 전용 방 (USE_PARTY — 방은 파티 위에 선다) — 세는 곳은 파티·방 조각(포털 방 갈래 EnterRoom · 잠금 넣기·훑기 — CPartyHandler) ·
    //   처치 보상 조각(방 보상 RewardRoom — CRewardHandler)과 게임 서버(빈 채널 정리 콜백 · 방 클리어 OnDeath · 사라짐 OnMonsterVanish).
    //   ★ 등식 둘: _entries[entered] = _closed + _active · Σ _entries(entered 뺀 칸) = mmo_portal_rejected_total{reason="room"}
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) RoomCounters
    {
        Counter _active;                                                  // [게이지] 지금 열린 방 수
        Counter _entries[static_cast<int>(RoomEntryReject::COUNT)];       // 방 포털 결과, 칸별(RoomEntryReject — NONE 칸 = 방을 만들어 옮김)
        Counter _moved;                                                   // 방으로 옮긴 사람 수(파티장 포함)
        Counter _closed;                                                  // 지운 방(빈 채널 정리)
        Counter _rewards;                                                 // 방 보상을 받은 사람 수 — 보스 한 마리에 방 안 사람 수만큼(RewardRoom)
        Counter _rewardItemsBag;                                          // 방 보상으로 가방에 바로 들어간 아이템 개수
        Counter _rewardItemsGround;                                       // 가방이 차서 그 사람 발밑 땅으로 간 개수(땅 계수 _drop._dropped 에도 들어간다)
        Counter _locks;                                                   // [게이지] 다시 들어가기 잠금 표 크기(넣을 때 · 초마다 훑은 뒤)
        Counter _cleared;                                                 // 클리어된 방 = 방에서 죽은 보스 수(OnDeath 의 방 갈래 — 몬스터 표 boss)
        Counter _minionsCleared;                                          // 클리어로 사라진 몬스터 수(OnMonsterVanish — 방에서 깐 수 = 잡힌 수 + 이것 + 방이 닫힐 때 남은 수)
    } _room;
#endif

    // DB 저장 파이프라인 — 정의는 위 DBSaveCounters(DB 워커가 참조로 받는다)
    DBSaveCounters _db;
};
