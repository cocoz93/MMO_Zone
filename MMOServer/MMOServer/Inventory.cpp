#include "Inventory.h"

#if USE_INVENTORY

#include <cassert>

#include "ItemTable.h"   // ItemDef

namespace
{
    constexpr unsigned TAB_COUNT = static_cast<unsigned>(InvTab::COUNT);

    // 탭의 배열 시작 색인 — 앞 탭들의 칸 수 합. 표에서 그때그때 센다(탭 셋이라 싸다).
    int TabBaseIndex(InvTab tab)
    {
        int base = 0;
        for (unsigned i = 0; i < static_cast<unsigned>(tab); ++i)
            base += INV_TAB_RANGES[i].slotCount;
        return base;
    }
}

const char* InvAddResultName(InvAddResult r)
{
    // 값마다 한 줄 — 새 사유를 넣고 여기를 빠뜨리면 아래 단언이 막는다(라벨 없는 계수기가 안 생기게).
    static_assert(static_cast<int>(InvAddResult::COUNT) == 4, "InvAddResult 를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case InvAddResult::OK:           return "ok";
    case InvAddResult::TAB_FULL:     return "tab_full";
    case InvAddResult::UNKNOWN_ITEM: return "unknown_item";
    case InvAddResult::BAD_COUNT:    return "bad_count";
    case InvAddResult::COUNT:        break;
    }
    return "?";
}

const char* UseItemFailName(UseItemFail r)
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

const char* InvLoadSkipName(InvLoadSkip r)
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

bool FindInvSlot(uint16_t slot, InvTab& outTab, int& outIndex)
{
    int base = 0;
    for (unsigned i = 0; i < TAB_COUNT; ++i)
    {
        const InvTabRange& r = INV_TAB_RANGES[i];
        if (slot >= r.firstSlot && slot < r.firstSlot + r.slotCount)
        {
            outTab   = static_cast<InvTab>(i);
            outIndex = base + (slot - r.firstSlot);
            return true;
        }
        base += r.slotCount;
    }
    return false;
}

uint16_t CInventory::SlotNumberOf(int index)
{
    assert(index >= 0 && index < INVENTORY_TOTAL_SLOTS);
    int base = 0;
    for (unsigned i = 0; i < TAB_COUNT; ++i)
    {
        const InvTabRange& r = INV_TAB_RANGES[i];
        if (index < base + r.slotCount)
            return static_cast<uint16_t>(r.firstSlot + (index - base));
        base += r.slotCount;
    }
    return 0;   // 닿지 않는다 — 색인이 총 칸 수 미만이면 위에서 반드시 걸린다
}

int CInventory::CountNonEmpty() const
{
    int n = 0;
    for (const Item& s : _slots)
    {
        if (s.itemId != 0)
            ++n;
    }
    return n;
}

InvAddResult CInventory::TryAdd(const ItemDef& def, int32_t count)
{
    if (count < 1)
        return InvAddResult::BAD_COUNT;

    assert(def.maxStack >= 1);   // 표 로드가 막는다(ItemTable.cpp ⑷)
    const int begin = TabBaseIndex(def.tab);
    const int end   = begin + INV_TAB_RANGES[static_cast<unsigned>(def.tab)].slotCount;

    // ⑴ 먼저 다 들어가는지만 본다 — 여기서는 한 칸도 안 바꾼다(전부 들어갈 때만 넣는다)
    int64_t room = 0;
    for (int i = begin; i < end; ++i)
    {
        const Item& s = _slots[i];
        if (s.itemId == def.itemId)
            room += def.maxStack - s.count;   // 같은 아이템이 든 칸의 남은 자리
        else if (s.itemId == 0)
            room += def.maxStack;              // 빈 칸 하나에 한도만큼
    }
    if (room < count)
        return InvAddResult::TAB_FULL;

    // ⑵ 덜 찬 칸부터(칸 번호 순) 채운다
    int32_t left = count;
    for (int i = begin; i < end && left > 0; ++i)
    {
        Item& s = _slots[i];
        if (s.itemId != def.itemId || s.count >= def.maxStack)
            continue;
        const int32_t put = (def.maxStack - s.count < left) ? def.maxStack - s.count : left;
        s.count += put;
        left    -= put;
        _changed.set(static_cast<size_t>(i));
    }

    // ⑶ 남으면 빈 칸(칸 번호 순)에 새로 놓는다
    for (int i = begin; i < end && left > 0; ++i)
    {
        Item& s = _slots[i];
        if (s.itemId != 0)
            continue;
        const int32_t put = (def.maxStack < left) ? def.maxStack : left;
        s.itemId = def.itemId;
        s.count  = put;
        left    -= put;
        _changed.set(static_cast<size_t>(i));
    }

    assert(left == 0);   // ⑴ 에서 자리를 셌으므로 반드시 다 들어간다
    return InvAddResult::OK;
}

void CInventory::ConsumeOne(int index)
{
    assert(index >= 0 && index < INVENTORY_TOTAL_SLOTS);
    Item& s = _slots[index];
    assert(s.itemId != 0 && s.count > 0);   // 부르는 쪽(RecvUseItem)이 빈 칸을 이미 거절했다
    if (--s.count <= 0)
    {
        s.itemId = 0;   // 마지막 한 개 — 칸을 비운다. 칸 패킷은 itemId 0 으로 나간다
        s.count  = 0;
    }
    _changed.set(static_cast<size_t>(index));
}

void CInventory::LoadSlot(int index, int32_t itemId, int32_t count)
{
    assert(index >= 0 && index < INVENTORY_TOTAL_SLOTS);
    _slots[index].itemId = itemId;
    _slots[index].count  = count;
    // 바뀐 칸 표시를 안 켠다 — DB 에서 온 값 그대로라 다시 쓸 일도, 클라에 칸 패킷을 보낼 일도 없다
    //   (입장 때 전체 목록 한 장이 간다).
}

#endif // USE_INVENTORY
