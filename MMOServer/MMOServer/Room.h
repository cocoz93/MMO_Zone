// ==========================================================================
// Room — 파티 전용 방 (USE_PARTY)
//
// [무엇] 방 맵(MapTable.csv kind = instance)의 채널 하나가 파티 하나의 방이다. 파티장이 그 맵으로 가는 포털을 쓰면
//        전원이 새 방으로 같이 옮겨 간다. 방에는 보스가 한 번만 깔리고(스폰 표 regenSec 0), 잡으면 방 안 사람 각자가 받는다.
//
// [여기 있는 것 둘]
//  - CheckRoomEntry — 들어가도 되나(순수 함수: 읽기만 하고 아무것도 안 바꾼다). 사유와 막는 사람을 돌려준다
//  - LockRoom · IsRoomLocked — 다시 들어가기 잠금(캐릭터 데이터 CPlayer::_roomLockouts — 방 맵 → 끝나는 실제 시각). DB 에 저장돼 어느 존 서버로 옮겨도 따라온다(world-server-plan.md 5.10)
//
// [누가 시키나] CPartyHandler(게임 서버 나누기 7 단계)다 — CEntryHandler::RecvPortalUse(포털 요청)가 목적지가 방 맵이면
//        넘기고(EnterRoom), CPartyHandler 가 판정하고, 통과하면 맵 관리자에게 방을 만들게 하고(CMapManager::CreateRoom) 전원을 옮긴다.
//        보상을 줄 때 잠금을 넣는 것은 방 보상(CRewardHandler::RewardRoom → LockRoom)이다. 방 안에서 도는 일(전투·몬스터)은 방 밖을 직접 만지지 않는다 —
//        방 밖과 닿는 곳은 세 자리(입장 판정 — CPartyHandler · 보상의 잠금 기록 — CRewardHandler · 나가는 포털 — CEntryHandler 의 포털 요청)뿐이다.
//
// [왜 여기 따로] 판정은 파티(CPartyCopy — 파티원 목록의 사본)와 파티원의 잠금을 같이 본다. 파티 규칙 클래스에 넣으면 파티가 방을 알게 되고,
//        맵 관리자에 넣으면 맵이 파티를 알게 된다. 둘 다 아는 것은 CPartyHandler(파티 사본 CPartyCopy 를 들고 방 입장을 맡는다)뿐이라 판정은 둘을 인자로 받는 순수 함수로 둔다.
// ==========================================================================
#pragma once

#include <Identity/GameIds.h>
#include <cstddef>
#include <cstdint>

#include "BuildConfig.h"     // USE_PARTY — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include "PartyCopy.h"       // Party — 판정이 파티원 목록(가입 순서)을 읽는다

#if USE_PARTY

class CPlayer;

// 방 들어가기 거절 사유 — 판정 순서 그대로(앞에서 걸리면 거기서 끝). 지표 칸(mmo_room_entries_total{result})이기도 하다.
//   와이어 값으로 바꾸는 곳은 CPartyHandler::SendRoomReject 한 곳 — PartyNotice 셋 · 파티 거절 둘 · 존 이동 실패 1.
enum class RoomEntryReject : uint8_t
{
    NONE,          // 들어감 — 지표에선 「방을 만들어 옮김」 칸
    NOT_IN_PARTY,  // 누른 사람이 파티에 없다
    NOT_LEADER,    // 누른 사람이 파티장이 아니다
    NOT_GATHERED,  // 파티원 중 누가 파티장과 다른 존(다른 맵 · 같은 맵 다른 채널)에 있다
    DEAD,          // 파티원 중 누가 쓰러져 있다
    LOCKED,        // 파티원 중 누가 그 방 맵의 잠금 중이다
    FULL,          // 방 번호가 다 찼다 — 판정 함수가 아니라 CPartyHandler 가 정한다(CMapManager::CreateRoom 이 nullptr)
    COUNT
};

struct RoomEntryResult
{
    RoomEntryReject reject  = RoomEntryReject::NONE;
    const Party::Member* blocker = nullptr;   // 막는 파티원(NOT_GATHERED · DEAD · LOCKED) — 가입 순서로 처음 걸린 사람. 나머지 사유는 nullptr
};

// 잠그기 — 같은 맵이 이미 있으면 끝나는 시각을 새 값으로 덮는다(보상을 또 받은 것). 끝난 칸은 이참에 뺀다.
void LockRoom(CPlayer* player, MapId mapId, int64_t untilMs, int64_t nowMs);

// 잠겨 있나 — 끝나는 시각이 지났으면 false
bool IsRoomLocked(const CPlayer* player, MapId mapId, int64_t nowMs);

// 들어가도 되나 — me = 포털을 쓴 사람(살아 있고 포털 반경 안이라는 것은 게임 서버가 이미 봤다), party = me 의 파티(없으면 nullptr),
//   roomMapId = 포털이 가는 방 맵. 순서 = 파티 없음 → 파티장 아님 → 다른 존 → 쓰러짐 → 잠금. 사유마다 파티원을 가입 순서로 다 훑고 다음 사유로 간다
//   (그래서 「다른 존에도 있고 쓰러진 사람도 있다」면 다른 존이 먼저 보인다 — 모이는 것이 먼저다).
RoomEntryResult CheckRoomEntry(const CPlayer* me, const Party* party, MapId roomMapId, int64_t nowMs);

#endif // USE_PARTY
