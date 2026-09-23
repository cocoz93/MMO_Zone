#pragma once

#include <bitset>
#include <cstdint>

#include "BuildConfig.h"
#include <Protocol/Protocol.h>   // InvTab · INV_TAB_RANGES · INVENTORY_TOTAL_SLOTS

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

// 불러올 때 건너뛴 줄의 사유 — DB 행이 망가졌거나 표가 바뀌어 지금 규칙에 안 맞는 줄. 평소엔 전부 0 이다.
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
const char* InvLoadSkipName(InvLoadSkip r);
const char* UseItemFailName(UseItemFail r);     // 지표 라벨(full_hp 등) — 사유 enum 은 와이어에 실려 Protocol.h 에 있다

// 칸 번호 → 탭 + 가방 배열 색인. 구간 밖이면 false. INV_TAB_RANGES 를 돈다(탭마다 if 없음).
bool FindInvSlot(uint16_t slot, InvTab& outTab, int& outIndex);

// ==========================================================================
// CInventory — 한 캐릭터의 가방
//
// [모양] 칸 60개를 빈틈없는 배열로 든다. 탭 구간(장비 0~19 · 소비 100~119 · 기타 200~219)은
//        와이어와 DB 에만 쓰는 번호이고, 배열 색인은 탭 순서대로 이어 붙인 것이다.
//
// [바뀐 칸] 가방이 스스로 적어 둔다(칸 60개 비트). 넣기든 쓰기든 같다.
//        내보내기(칸 패킷·DB 저장)는 CGameServer::FlushInventory 한 곳이다 — 칸을 바꾸는 길이
//        늘어나도 패킷·저장을 따로 안 짠다. 빠뜨릴 수 있는 것은 그 호출 하나뿐이다.
//
// [모르는 것] 패킷도 DB 도 모른다. 아이템 정의는 부르는 쪽이 표에서 찾아 건넨다.
//        죽어도 안 잃는다(죽음 경로가 이 객체를 안 건드린다).
// ==========================================================================
class CInventory
{
public:
    struct Slot
    {
        int32_t itemId = 0;   // 0 = 빈 칸
        int32_t count  = 0;
    };

    // def 를 count 개 넣는다.
    //   ★ 전부 들어갈 때만 넣는다 — 모자라면 아무것도 안 바꾸고 TAB_FULL.
    //     반만 넣으면 "몇 개 들어갔나"를 요청한 쪽에 알려야 하고, 드랍이면 남은 것을 땅에 둬야 한다.
    //   같은 아이템이 든 덜 찬 칸부터(칸 번호 순) 채우고, 남으면 빈 칸(칸 번호 순)에 새로 놓는다.
    //   쌓임 한도는 def.maxStack 하나로 본다 — 종류로 가르지 않는다(장비는 한도 1이라 저절로 안 쌓인다).
    InvAddResult TryAdd(const ItemDef& def, int32_t count);

    // 불러올 때 칸을 채운다 — 바뀐 칸 표시를 안 켠다(DB 와 같은 값이다). 4단계에서 쓴다.
    void LoadSlot(int index, int32_t itemId, int32_t count);

    // 한 개 줄인다 — 0 이 되면 칸을 비운다(itemId 0). 쓸 수 있는지는 부르는 쪽이 이미 봤다(빈 칸이면 단언).
    void ConsumeOne(int index);

    const Slot& At(int index) const { return _slots[index]; }
    static uint16_t SlotNumberOf(int index);   // 배열 색인 → 칸 번호
    int CountNonEmpty() const;

    // 바뀐 칸마다 fn(index) 를 부르고 표시를 지운다 — CGameServer::FlushInventory 만 부른다.
    template <class Fn>
    void TakeChanges(Fn&& fn)
    {
        if (_changed.none())
            return;
        for (int i = 0; i < INVENTORY_TOTAL_SLOTS; ++i)
        {
            if (_changed.test(static_cast<size_t>(i)))
                fn(i);
        }
        _changed.reset();
    }
    bool HasChanges() const { return _changed.any(); }

private:
    Slot _slots[INVENTORY_TOTAL_SLOTS];
    std::bitset<INVENTORY_TOTAL_SLOTS> _changed;
};

#endif // USE_INVENTORY
