// ==========================================================================
// Room — 파티 전용 방 (USE_PARTY · MyDocs/claude-plans/room-plan.md)
//
// [무엇] 방 맵(MapTable.csv kind = instance)의 채널 하나가 파티 하나의 방이다. 파티장이 그 맵으로 가는 포털을 쓰면
//        전원이 새 방으로 같이 옮겨 간다. 방에는 보스가 한 번만 깔리고(스폰 표 regenSec 0), 잡으면 방 안 사람 각자가 받는다.
//
// [여기 있는 것 둘]
//  - CheckRoomEntry — 들어가도 되나(순수 함수: 읽기만 하고 아무것도 안 바꾼다). 사유와 막는 사람을 돌려준다
//  - CRoomLocks     — 다시 들어가기 잠금(계정 · 방 맵 → 풀리는 틱). 서버 메모리에만 — 재시작하면 풀린다(room-plan.md 「사람이 정할 것」 2)
//
// [누가 시키나] 파티·방 조각(CPartyHandler — 게임 서버 나누기 7 단계)이다 — 게임 서버의 포털 요청(RecvPortalUse)이 목적지가 방 맵이면
//        넘기고(EnterRoom), 조각이 판정하고, 통과하면 맵 관리자에게 방을 만들게 하고(CMapManager::CreateRoom) 전원을 옮긴다.
//        보상을 줄 때 잠금을 넣는 것도 조각 함수다(LockRoomReentry — 방 보상이 부른다). 방 안에서 도는 일(전투·몬스터)은 방 밖을 직접 만지지 않는다 —
//        방 밖과 닿는 곳은 세 자리(입장 판정 · 보상의 잠금 기록 — 파티·방 조각 · 나가는 포털 — 게임 서버의 포털 요청)뿐이다.
//
// [왜 여기 따로] 판정은 파티(CPartySystem — 파티원 목록)와 잠금 표를 같이 본다. 파티 규칙 클래스에 넣으면 파티가 방을 알게 되고,
//        맵 관리자에 넣으면 맵이 파티를 알게 된다. 둘 다 아는 것은 파티·방 조각(파티 규칙을 빌리고 잠금 표를 든다)뿐이라 판정은 둘을 인자로 받는 순수 함수로 둔다.
// ==========================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>        // std::hash — 열쇠 해시
#include <unordered_map>

#include "BuildConfig.h"     // USE_PARTY — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include "PartySystem.h"     // Party — 판정이 파티원 목록(가입 순서)을 읽는다

#if USE_PARTY

class CPlayer;

// 방 들어가기 거절 사유 — 판정 순서 그대로(앞에서 걸리면 거기서 끝). 지표 칸(mmo_room_entries_total{result})이기도 하다.
//   와이어 값으로 바꾸는 곳은 파티·방 조각 한 곳(CPartyHandler::SendRoomReject) — PartyNotice 셋 · 파티 거절 둘 · 존 이동 실패 1.
enum class RoomEntryReject : uint8_t
{
    NONE,          // 들어감 — 지표에선 「방을 만들어 옮김」 칸
    NOT_IN_PARTY,  // 누른 사람이 파티에 없다
    NOT_LEADER,    // 누른 사람이 파티장이 아니다
    NOT_GATHERED,  // 파티원 중 누가 파티장과 다른 존(다른 맵 · 같은 맵 다른 채널)에 있다
    DEAD,          // 파티원 중 누가 쓰러져 있다
    LOCKED,        // 파티원 중 누가 그 방 맵의 잠금 중이다
    FULL,          // 방 번호가 다 찼다 — 판정 함수가 아니라 파티·방 조각이 정한다(CMapManager::CreateRoom 이 nullptr)
    COUNT
};

struct RoomEntryResult
{
    RoomEntryReject reject  = RoomEntryReject::NONE;
    const CPlayer*  blocker = nullptr;   // 막는 파티원(NOT_GATHERED · DEAD · LOCKED) — 가입 순서로 처음 걸린 사람. 나머지 사유는 nullptr
};

class CRoomLocks
{
public:
    // 잠그기 — 같은 계정·맵이 이미 있으면 풀리는 틱을 새 값으로 덮는다(보상을 또 받은 것)
    void Lock(int64_t accountId, int32_t mapId, uint64_t untilFrame);

    // 잠겨 있나 — 풀리는 틱이 지났으면 false(훑기 전이라 표에 남아 있어도)
    bool IsLocked(int64_t accountId, int32_t mapId, uint64_t frame) const;

    // 풀린 것 빼기 — 파티·방 조각이 틱에서 가끔 부른다(CPartyHandler::TickRoomLocks — 초마다). 표는 「잠금 시간 동안 보상을 받은 계정 수」로 묶인다
    void Sweep(uint64_t frame);

    size_t Size() const { return _untilFrame.size(); }

private:
    // 열쇠 = 계정 · 맵 — 맵 번호 상한이 1000(CMapTable::MAX_MAP_ID)이라 계정 번호에 곱해 합치지 않고 둘을 그대로 둔다
    struct Key
    {
        int64_t accountId;
        int32_t mapId;
        bool operator==(const Key& o) const { return accountId == o.accountId && mapId == o.mapId; }
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const
        {
            return std::hash<int64_t>()(k.accountId) ^ (std::hash<int32_t>()(k.mapId) * 0x9E3779B97F4A7C15ull);
        }
    };
    std::unordered_map<Key, uint64_t, KeyHash> _untilFrame;
};

// 들어가도 되나 — me = 포털을 쓴 사람(살아 있고 포털 반경 안이라는 것은 게임 서버가 이미 봤다), party = me 의 파티(없으면 nullptr),
//   roomMapId = 포털이 가는 방 맵. 순서 = 파티 없음 → 파티장 아님 → 다른 존 → 쓰러짐 → 잠금. 사유마다 파티원을 가입 순서로 다 훑고 다음 사유로 간다
//   (그래서 「다른 존에도 있고 쓰러진 사람도 있다」면 다른 존이 먼저 보인다 — 모이는 것이 먼저다).
RoomEntryResult CheckRoomEntry(const CPlayer* me, const Party* party, const CRoomLocks& locks, int32_t roomMapId, uint64_t frame);

#endif // USE_PARTY
