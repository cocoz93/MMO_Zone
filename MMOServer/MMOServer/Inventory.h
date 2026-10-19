#pragma once

#include <bitset>
#include <cstdint>

#include "BuildConfig.h"
#include <Protocol/Protocol.h>   // InvTab · INV_TAB_RANGES · INVENTORY_TOTAL_SLOTS · EquipSlot · INVENTORY_ALL_SLOTS
#include "Item.h"                // 가방 칸 하나 = Item (땅의 아이템도 같은 타입을 든다)

#if USE_INVENTORY

struct ItemDef;

// 넣기 결과 — 거절은 사유마다 계수기가 하나씩이라(GameMonitor) enum 과 이름 표로 둔다.
//   ※ UNKNOWN_ITEM 은 TryAdd 가 돌려주지 않는다 — 표에 없는 번호는 부르는 쪽이 먼저 가른다.
//     그래도 여기 두는 이유는 "넣기가 왜 안 됐나"를 한 벌의 사유로 세기 위해서다.
enum class InvAddResult : uint8_t
{
    OK,
    TAB_FULL,       // 그 탭에 다 들어갈 자리가 없다 — 아무것도 안 넣었다
    UNKNOWN_ITEM,   // 아이템 표에 없는 번호
    BAD_COUNT,      // 개수가 1 미만
    COUNT
};
const char* InvAddResultName(InvAddResult r);   // 지표 라벨(tab_full 등)
//   가방이 정하지 않는 사유는 여기 두지 않는다(10-06) — 쓰기·장착 거절과 불러오기 지표 라벨(InvLoadSkip)은 게임 서버 몫이라
//   GameMonitor.h 에 있다. 이 파일은 패킷도 DB 도 모른다([모르는 것] 참조).

// 불러오기 결과 — 「이 칸에 이 아이템이 이만큼 들어가도 되나」는 가방 규칙이라 가방이 정한다. 지표 라벨(InvLoadSkip)로는 게임 서버가 옮긴다.
enum class InvLoadResult : uint8_t
{
    OK,
    WRONG_TAB,   // 아이템 탭 ≠ 칸 탭 (무기 칸에 물약 등)
    BAD_COUNT,   // 0 이하이거나 쌓임 한도 초과 — 깎아 넣지 않는다
};

// 칸 번호 → 탭 + 가방 배열 색인. 구간 밖이면 false. INV_TAB_RANGES 를 돈 다음 착용 구간을 본다(탭마다 if 없음).
//   ★ 착용 칸(900~)이면 탭을 EQUIP 으로 돌려준다 — "그 칸에 드는 아이템의 탭"이다. 그래서 부르는 두 곳이 안 바뀐다:
//     불러오기(LoadSlot)의 탭 대조가 "무기 칸에 물약" 을 WRONG_TAB 으로 거르고, 물약 쓰기는 NOT_USABLE 로 떨어진다.
//     COUNT 를 돌려주면 불러오기가 무기 칸 줄을 전부 WRONG_TAB 으로 버려 장착이 DB 에서 조용히 안 살아난다.
//   가방 칸이냐 착용 칸이냐는 색인으로 가른다(IsWornIndex).
bool FindInvSlot(uint16_t slot, InvTab& outTab, int& outIndex);

// 착용 칸의 배열 색인 — 가방 칸 60 뒤에 이어 붙는다(색인 60 = 무기 칸 900).
constexpr int WORN_BASE_INDEX = INVENTORY_TOTAL_SLOTS;
constexpr int WornIndexOf(EquipSlot s) { return WORN_BASE_INDEX + static_cast<int>(s); }
constexpr bool IsWornIndex(int index) { return index >= WORN_BASE_INDEX && index < INVENTORY_ALL_SLOTS; }

// ==========================================================================
// CInventory — 한 캐릭터의 가방
//
// [모양] 칸 61개를 빈틈없는 배열로 든다 — 가방 60 + 착용 1(무기). 탭 구간(장비 0~19 · 소비 100~119 · 기타 200~219)과
//        착용 칸 번호(900~)는 와이어와 DB 에만 쓰는 번호이고, 배열 색인은 탭 순서대로 이어 붙인 뒤 착용 칸을 붙인 것이다.
//        착용 칸도 같은 배열이라 바뀐 칸 표시·내보내기(FlushInventory)·전체 목록이 한 길로 돈다.
//        넣기(TryAdd)는 아이템 탭의 구간만 돌아 착용 칸에 저절로 안 들어간다.
//
// [바뀐 칸] 가방이 스스로 적어 둔다(칸 61개 비트 — 착용 칸 포함). 넣기든 쓰기든 끼우기든 같다.
//        내보내기(칸 패킷·DB 저장)는 CItemHandler::FlushInventory 한 곳이다 — 칸을 바꾸는 길이
//        늘어나도 패킷·저장을 따로 안 짠다. 빠뜨릴 수 있는 것은 그 호출 하나뿐이다.
//
// [모르는 것] 패킷도 DB 도 모른다. 아이템 정의는 부르는 쪽이 표에서 찾아 건넨다.
//        죽어도 안 잃는다(죽음 경로가 이 객체를 안 건드린다).
// ==========================================================================
class CInventory
{
public:
    // 칸 하나는 Item 이다(itemId 0 = 빈 칸). 따로 칸 타입을 두지 않는다 — 땅의 아이템과 같은 타입이라
    //   줍기가 필드를 하나씩 옮겨 적지 않는다(Item.h).

    // def 를 count 개 넣는다.
    //   ★ 전부 들어갈 때만 넣는다 — 모자라면 아무것도 안 바꾸고 TAB_FULL.
    //     반만 넣으면 "몇 개 들어갔나"를 요청한 쪽에 알려야 하고, 드랍이면 남은 것을 땅에 둬야 한다.
    //   같은 아이템이 든 덜 찬 칸부터(칸 번호 순) 채우고, 남으면 빈 칸(칸 번호 순)에 새로 놓는다.
    //   쌓임 한도는 def.maxStack 하나로 본다 — 종류로 가르지 않는다(장비는 한도 1이라 저절로 안 쌓인다).
    InvAddResult TryAdd(const ItemDef& def, int32_t count);

    // 불러올 때 칸을 채운다 — 탭·개수가 안 맞으면 안 넣고 사유를 돌려준다. 바뀐 칸 표시를 안 켠다(DB 와 같은 값이다).
    InvLoadResult LoadSlot(int index, const ItemDef& def, int32_t count);

    // 한 개 줄인다 — 0 이 되면 칸을 비운다(itemId 0). 쓸 수 있는지는 부르는 쪽이 이미 봤다(빈 칸이면 단언).
    void ConsumeOne(int index);

    // 두 칸의 내용을 맞바꾼다 — 끼우기(가방 칸 ↔ 무기 칸)·빼기(무기 칸 ↔ 장비 탭의 빈 칸)가 이것 하나다.
    //   한쪽이 비었으면 "옮기기"가 된다. 되는지(장비인가·죽었나·빈 칸이 있나)는 부르는 쪽이 이미 봤다(CItemSystem::CheckEquip).
    //   두 칸 다 바뀐 칸으로 적는다 — 내보내기(FlushInventory)가 칸 패킷 둘 + DB 두 줄로 나간다.
    void SwapSlots(int a, int b);

    // 그 탭의 첫 빈 칸(칸 번호 순) 색인. 없으면 -1.
    int FirstEmptyInTab(InvTab tab) const;

    const Item& At(int index) const { return _slots[index]; }
    static uint16_t SlotNumberOf(int index);   // 배열 색인 → 칸 번호
    int CountNonEmpty() const;

    // 바뀐 칸마다 fn(index) 를 부르고 표시를 지운다 — CItemHandler::FlushInventory 만 부른다.
    template <class Fn>
    void TakeChanges(Fn&& fn)
    {
        if (_changed.none())
            return;
        for (int i = 0; i < INVENTORY_ALL_SLOTS; ++i)
        {
            if (_changed.test(static_cast<size_t>(i)))
                fn(i);
        }
        _changed.reset();
    }
    bool HasChanges() const { return _changed.any(); }

private:
    Item _slots[INVENTORY_ALL_SLOTS];            // 가방 60 + 착용 칸(색인 WORN_BASE_INDEX~)
    std::bitset<INVENTORY_ALL_SLOTS> _changed;
};

#endif // USE_INVENTORY
