#include "ShopSystem.h"

#if USE_SHOP

#include "Player.h"          // 생사 · 가방(칸 · 돈) · 자리
#include "Character.h"       // 말 걸기 칸(Talk) · NPC 의 지금 자리
#include "GameDataFiles.h"   // 아이템 표 · 상점 표 · NPC 줄(NpcDef)
#include "GameMonitor.h"     // 사고팔기 · 돈 지표
#include "Reach.h"           // 손 닿는 범위 식 — 포털과 같은 한 곳
#include <cassert>

CShopSystem::CShopSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor)
    : _tables(tables)
    , _gameMonitor(gameMonitor)
{
}

int64_t CShopSystem::SellPriceOf(const ItemDef& def)
{
    return static_cast<int64_t>(def.price) / 2;   // 정수 나눗셈 = 내림(값은 0 이상 — 표 읽기가 막는다)
}

// ==========================================================================
// 공통 앞 둘 — 쓰러짐 · NPC(있나 · 상점이 있나 · 손이 닿나)
// ==========================================================================
ShopReject CShopSystem::CheckNpc(const CPlayer* player, const CCharacter* npc, int32_t& outShopId) const
{
    if (player->_health.IsDead())
        return ShopReject::DEAD;

    // 사고파는 상대는 타입(CNpc)이 아니라 말 걸기 칸으로 가른다 — 싸우는 NPC 가 생겨도 같은 판정(플랜 「목표 구조」)
    const NpcDef* talk = (npc != nullptr) ? npc->Talk() : nullptr;
    if (talk == nullptr || talk->shopId == 0 || !_tables.Shops().Has(talk->shopId))
        return ShopReject::NO_NPC;

    // 반경은 NPC 의 지금 자리에서 — 표 자리가 아니라(움직이는 NPC 가 생겨도 같은 식)
    if (!Reach::InRadius(player->_x, player->_y, npc->_x, npc->_y, static_cast<float>(talk->radius)))
        return ShopReject::NOT_IN_REACH;

    outShopId = talk->shopId;
    return ShopReject::COUNT;
}

// ==========================================================================
// 사기
// ==========================================================================
ShopReject CShopSystem::CheckBuy(const CPlayer* player, const CCharacter* npc, int32_t itemId, int32_t count,
                                 const ItemDef*& outDef, int64_t& outCost) const
{
    int32_t shopId = 0;
    const ShopReject npcReject = CheckNpc(player, npc, shopId);
    if (npcReject != ShopReject::COUNT)
        return npcReject;

    // 표에 없는 번호도 여기로 떨어진다 — 상점 표 읽기가 「아이템 표에 있는 번호 · 돈 아님 · 값 > 0」을 막아서
    //   Sells 가 참이면 아이템 정의가 있다(아래 단언)
    if (!_tables.Shops().Sells(shopId, itemId))
        return ShopReject::NOT_SOLD_HERE;
    outDef = _tables.Item().Find(itemId);
    assert(outDef != nullptr && outDef->store == ItemStore::SLOT && outDef->price > 0 && "상점 표 읽기가 막는다(ShopTable.cpp)");

    if (count < 1 || count > outDef->maxStack)
        return ShopReject::BAD_COUNT;

    // 값 × 개수 — 개수가 maxStack(표 상한 99) 이하 · 값이 1억 이하라 int64 에서 안 넘친다
    outCost = static_cast<int64_t>(outDef->price) * count;
    if (outCost > player->_inventory.Money())
        return ShopReject::NOT_ENOUGH_MONEY;

    return ShopReject::COUNT;   // 통과 — BAG_FULL 은 넣어 봐야 안다(Buy)
}

ShopReject CShopSystem::Buy(CPlayer* player, const CCharacter* npc, int32_t itemId, int32_t count)
{
    const ItemDef* def = nullptr;
    int64_t cost = 0;
    ShopReject reject = CheckBuy(player, npc, itemId, count, def, cost);
    if (reject == ShopReject::COUNT)
    {
        // 넣기가 먼저, 돈 빼기가 나중 — 넣기는 「전부 아니면 아무것도」라, 안 들어가면 돈도 그대로다
        const InvAddResult added = player->_inventory.TryAdd(*def, count);
        if (added != InvAddResult::OK)
        {
            assert(added == InvAddResult::TAB_FULL && "개수·번호·담는 자리는 판정이 봤다 — 남는 거절은 자리 모자람뿐");
            reject = ShopReject::BAG_FULL;
        }
    }
    if (reject != ShopReject::COUNT)
    {
        _gameMonitor._shop._rejected[static_cast<int>(reject)].Inc();
        return reject;
    }

    player->_inventory.SpendMoney(cost);
    _gameMonitor._shop._bought.Inc();
    _gameMonitor._inventory._itemsAdded.Add(count);   // /item · 줍기 · 방 보상과 같은 계수기 — 가방에 들어간 개수
    _gameMonitor._inventory._moneyOut[static_cast<int>(MoneySink::BUY)].Add(cost);
    return ShopReject::COUNT;
}

// ==========================================================================
// 팔기
// ==========================================================================
ShopReject CShopSystem::CheckSell(const CPlayer* player, const CCharacter* npc, uint16_t slot, int32_t count,
                                  int& outIndex, int64_t& outGain) const
{
    int32_t shopId = 0;
    const ShopReject npcReject = CheckNpc(player, npc, shopId);
    if (npcReject != ShopReject::COUNT)
        return npcReject;

    // 칸 — 범위 밖 · 착용 칸(끼운 무기는 빼고 판다) · 빈 칸
    InvTab tab;
    if (!FindInvSlot(slot, tab, outIndex) || IsWornIndex(outIndex))
        return ShopReject::BAD_SLOT;
    const Item& s = player->_inventory.At(outIndex);
    if (s.itemId == 0)
        return ShopReject::BAD_SLOT;

    // 표에서 사라진 번호(패치로 지운 아이템)도 「안 사들인다」로 떨어진다. 사들이는 값이 0(파는 값 0 · 1)이면 돈이 안 들어오니 거절
    const ItemDef* def = _tables.Item().Find(s.itemId);
    if (def == nullptr || !_tables.Shops().Buys(shopId, s.itemId) || SellPriceOf(*def) <= 0)
        return ShopReject::NOT_BOUGHT_HERE;

    if (count < 1 || count > s.count)
        return ShopReject::BAD_COUNT;

    // 받을 돈 — 상한을 넘으면 아무것도 안 바꾼다(반만 팔지 않는다 — 넣기의 MONEY_FULL 과 같은 규칙). 넘침 없이 견준다
    outGain = SellPriceOf(*def) * count;
    if (outGain > MONEY_MAX - player->_inventory.Money())
        return ShopReject::MONEY_FULL;

    return ShopReject::COUNT;
}

ShopReject CShopSystem::Sell(CPlayer* player, const CCharacter* npc, uint16_t slot, int32_t count)
{
    int index = -1;
    int64_t gain = 0;
    const ShopReject reject = CheckSell(player, npc, slot, count, index, gain);
    if (reject != ShopReject::COUNT)
    {
        _gameMonitor._shop._rejected[static_cast<int>(reject)].Inc();
        return reject;
    }

    player->_inventory.Remove(index, count);
    const bool added = player->_inventory.TryAddMoney(gain);
    assert(added && "상한은 판정이 봤다");
    (void)added;
    _gameMonitor._shop._sold.Inc();
    _gameMonitor._inventory._moneyIn[static_cast<int>(MoneySource::SELL)].Add(gain);
    return ShopReject::COUNT;
}

#endif // USE_SHOP
