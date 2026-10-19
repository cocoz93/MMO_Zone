#pragma once

#include <cstdint>

#include "BuildConfig.h"

#if USE_DROP

#include "Item.h"

// ==========================================================================
// CGroundItem — 땅에 떨어진 아이템 하나
//
// [값으로 담는다] 땅 아이템 관리자가 섹터 칸마다 vector<CGroundItem> 을 값으로 든다(CGroundItemManager::_items).
//        캐릭터와 달리 포인터로 들지 않는 이유 — 이것을 가리키는 대기열이 하나도 없다(움직이지 않고,
//        섹터 변경도 부활 대기도 없다). 값이면 new/delete 도, 채널이 지워질 때 따로 지우는 일도,
//        가리키던 것이 사라진 포인터도 없다. 벡터 안 자리는 빼면 바뀌므로 자리 번호를 들고 다니지 않는다.
//
// [주인] 막타 친 계정 번호. 캐릭터 번호는 다시 들어오면 바뀐다(접속마다 새로 뽑는다).
//        주인 우선 시간이 지나면 누구나 줍는다. 주인은 와이어에 안 실린다(Protocol.h MSG_S2C_GROUND_ITEM_CREATE).
// ==========================================================================
struct CGroundItem
{
    int32_t  groundId        = 0;     // 땅의 아이템 번호 — 캐릭터 번호와 따로 센다(CItemHandler::_nextGroundId)
    Item     item;                    // 무엇이 몇 개 — 가방 칸과 같은 타입이라 줍기가 이것을 그대로 옮긴다
    float    x               = 0.0f;
    float    y               = 0.0f;
    int64_t  ownerAccountId  = 0;     // 막타 친 계정 (0 = 주인 없음)
    uint64_t ownerUntilFrame = 0;     // 이 틱 전까지는 주인만 줍는다
    uint64_t expireFrame     = 0;     // 이 틱이 지나면 사라진다
};

// ── 사유 enum 은 정하는 쪽의 어휘다(10-06, 아키텍처 전수검사 7번) ──
//   아래 둘은 땅 아이템 관리자가 정한다. 와이어 값(Protocol.h GroundItemGone·PickupFail)으로 바꾸는 건 게임 서버의 일이고
//   (ToZoneChangeFailReason 과 같은 꼴), 계수기(GameMonitor.h)는 COUNT 와 이름표만 빌린다. 이 헤더는 패킷을 모른다.

// 땅에서 빠진 까닭 — 와이어의 GroundItemGone 과 다르다(그쪽의 OUT_OF_SIGHT 는 빠진 게 아니라 안 보이는 것,
//   CLEANED 는 알릴 사람이 없어 와이어에 안 나간다). 빼는 자리마다 한 칸이라 계수기 등식이 한 곳의 누락도 드러낸다.
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

// 줍기 판정 결과(FindPickup) — DEAD → NO_ITEM → NOT_OWNER 순으로 보고 먼저 걸린 하나, 통과면 NONE.
//   가방이 가득 찬 것(TAB_FULL)은 판정이 아니라 가방 넣기의 결과라 여기 없다 — 게임 서버가 와이어 PickupFail 로 바로 센다.
enum class PickupReject : uint8_t
{
    NONE,        // 주울 것이 있다
    DEAD,        // 죽어 있다
    NO_ITEM,     // 반경 안에 아무것도 없다
    NOT_OWNER,   // 있긴 한데 전부 남의 것이다(주인 우선 시간 안)
};

#endif // USE_DROP
