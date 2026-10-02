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
    int32_t  groundId        = 0;     // 땅의 아이템 번호 — 캐릭터 번호와 따로 센다(CGameServer::_nextGroundId)
    Item     item;                    // 무엇이 몇 개 — 가방 칸과 같은 타입이라 줍기가 이것을 그대로 옮긴다
    float    x               = 0.0f;
    float    y               = 0.0f;
    int64_t  ownerAccountId  = 0;     // 막타 친 계정 (0 = 주인 없음)
    uint64_t ownerUntilFrame = 0;     // 이 틱 전까지는 주인만 줍는다
    uint64_t expireFrame     = 0;     // 이 틱이 지나면 사라진다
};

#endif // USE_DROP
