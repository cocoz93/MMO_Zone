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
    MONEY_FULL,     // 돈 아이템 — 받으면 들 수 있는 돈(Item.h MONEY_MAX)을 넘는다 — 아무것도 안 바꿨다(반만 받지 않는다)
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
// [돈] 칸이 아니라 숫자 하나(_money · 0~MONEY_MAX)다. 땅·드랍에서는 「돈」 아이템(아이템 표 store = MONEY) 한 덩이로 다니다가
//        넣기(TryAdd)가 담는 자리를 보고 칸 대신 돈에 더한다 — 줍기·보상·명령에 돈 갈래가 따로 없다. 상한을 넘으면 아무것도 안 바꾸고 MONEY_FULL.
//        화면용 표시는 「돈 바뀜」 하나(FlushInventory 가 S2C_MONEY 를 보내며 지운다). 저장 표시는 따로 없다 — 돈은 캐릭터 행의 열이라
//        저장소가 마지막으로 실은 값과 견준다(CCharacterStore::ShouldSave · Player.h _dbSaved — 체력·경험치와 같은 규칙).
//
// [바뀐 칸] 가방이 스스로 적어 둔다(칸 61개 비트 두 벌 — 착용 칸 포함). 넣기든 쓰기든 끼우기든 같다(MarkChanged 한 곳).
//        화면용(_changed)은 CItemHandler::FlushInventory 가 칸 패킷을 내보내며 지우고, 저장용(_unsaved)은 캐릭터 저장소가
//        묶음에 실을 때 가져간다(DB 가 실패를 회신하면 되돌려 놓는다). 칸을 바꾸는 길이 늘어도 패킷·저장을 따로 안 짠다.
//        FlushInventory 가 그 틱 끝 저장도 요청한다 — 빠뜨려도 저장 표시는 남아 다음 주기 판정이 싣는다.
//
// [거래 잠금] 거래창에 올린 칸·돈을 묶어 둔다(잠그고 푸는 쪽 = CTradeSystem · trade-plan.md 구조 결정 ②). 비트 한 벌과 돈 숫자 하나뿐이다.
//        잠긴 칸에는 넣기(TryAdd)가 안 쌓는다 — 올린 물약 칸에 줍기가 몰래 붙어 올린 개수가 바뀌지 않게.
//        줄이기·맞바꾸기(Remove · SwapSlots)는 부르는 쪽이 IsLocked 로 먼저 거절한다(쓰기·끼우기·팔기의 IN_TRADE — 어기면 단언).
//        잠근 돈은 쓸 수 있는 돈(SpendableMoney)에서 빠진다 — 사기·더 올리기가 이 값으로 판정한다. 들 수 있는 돈 상한은 그대로 Money() 로 본다.
//        잠금은 화면·저장 표시가 아니다 — DB 에 안 실린다(끊기면 거래가 취소되며 풀린다). 가방은 거래가 있다는 것을 모른다.
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
    //   ★ 돈 아이템(def.store = MONEY)이면 칸이 아니라 돈에 더한다 — 위 [돈]. 상한을 넘으면 MONEY_FULL(아무것도 안 바뀜).
    //   ★ 거래로 잠긴 칸에는 안 쌓는다(위 [거래 잠금]) — 자리 셈에서도 뺀다. 잠긴 칸은 늘 차 있어 빈 칸 자리에는 안 걸린다.
    InvAddResult TryAdd(const ItemDef& def, int32_t count);

    // 불러올 때 칸을 채운다 — 탭·개수가 안 맞으면 안 넣고 사유를 돌려준다. 바뀐 칸 표시를 안 켠다(DB 와 같은 값이다).
    InvLoadResult LoadSlot(int index, const ItemDef& def, int32_t count);

    // count 개 줄인다 — 0 이 되면 칸을 비운다(itemId 0). 되는지(빈 칸 · 개수)는 부르는 쪽이 이미 봤다(어기면 단언).
    //   물약 쓰기는 1 개, 상점에 팔기는 count 개.
    void Remove(int index, int32_t count);

    // ── 돈 ── 위 [돈]. 되는지(상한 · 모자람)는 부르는 쪽이 판정에서 이미 봤다 — 더하기만 상한을 스스로 본다(넣기 갈래가 여럿이라)
    int64_t Money() const { return _money; }
    bool    TryAddMoney(int64_t amount);   // amount ≥ 1 · 상한을 넘으면 false(아무것도 안 바뀜)
    void    SpendMoney(int64_t amount);    // amount ≥ 1 · 가진 돈 이하(어기면 단언)
    void    LoadMoney(int64_t money);      // 불러올 때 — 범위는 부르는 쪽이 봤다(망가진 값은 안 넣음 — 저장소). 「돈 바뀜」을 안 켠다(DB 와 같은 값)
    // 「돈 바뀜」을 돌려주고 지운다 — FlushInventory 만 부른다(화면용 — S2C_MONEY)
    bool    TakeMoneyChange() { const bool c = _moneyChanged; _moneyChanged = false; return c; }

    // 두 칸의 내용을 맞바꾼다 — 끼우기(가방 칸 ↔ 무기 칸)·빼기(무기 칸 ↔ 장비 탭의 빈 칸)가 이것 하나다.
    //   한쪽이 비었으면 "옮기기"가 된다. 되는지(장비인가·죽었나·빈 칸이 있나)는 부르는 쪽이 이미 봤다(CItemSystem::CheckEquip).
    //   두 칸 다 바뀐 칸으로 적는다 — 칸 패킷 둘(FlushInventory) · DB 는 그 틱 끝 묶음 하나에 두 줄이 같이(트랜잭션 하나).
    void SwapSlots(int a, int b);

    // 그 탭의 첫 빈 칸(칸 번호 순) 색인. 없으면 -1.
    int FirstEmptyInTab(InvTab tab) const;

    const Item& At(int index) const { return _slots[index]; }
    static uint16_t SlotNumberOf(int index);   // 배열 색인 → 칸 번호
    int CountNonEmpty() const;

    // 바뀐 칸마다 fn(index) 를 부르고 표시를 지운다 — CItemHandler::FlushInventory 만 부른다(화면용 표시).
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

    // 저장 안 된 칸 — 화면용 표시(_changed)와 따로 든다. 저장소가 묶음에 실을 때 가져가고(TakeUnsaved),
    //   DB 가 실패를 회신하면 되돌려 놓는다(MarkUnsaved). 화면 패킷이 나갔다고 저장된 것이 아니다(save-unify-plan.md 「표시」).
    using SlotBits = std::bitset<INVENTORY_ALL_SLOTS>;
    SlotBits TakeUnsaved() { const SlotBits b = _unsaved; _unsaved.reset(); return b; }
    void MarkUnsaved(const SlotBits& bits) { _unsaved |= bits; }
    bool HasUnsaved() const { return _unsaved.any(); }

    // ── 거래 잠금 ── 위 [거래 잠금]. 되는지(빈 칸 · 착용 칸 · 이미 잠김 · 쓸 수 있는 돈)는 부르는 쪽(CTradeSystem)이 판정에서 봤다(어기면 단언)
    void    LockSlot(int index);
    void    LockMoney(int64_t amount);    // amount ≥ 1 · SpendableMoney 이하 — 잠근 돈에 더한다(거래창의 돈은 늘리기만)
    void    UnlockAll();                  // 칸·돈 잠금을 다 푼다 — 거래가 끝날 때(성사 · 취소)
    bool    IsLocked(int index) const { return _locked.test(static_cast<size_t>(index)); }
    bool    HasLocks() const { return _locked.any() || _lockedMoney != 0; }
    int64_t LockedMoney() const { return _lockedMoney; }
    int64_t SpendableMoney() const { return _money - _lockedMoney; }   // 가진 돈 − 거래창에 올려 잠근 돈

private:
    // 칸이 바뀌었다 — 화면용·저장용 표시를 같이 켠다. 칸을 바꾸는 함수는 이것만 부른다(둘 중 하나를 빠뜨릴 길을 없앤다).
    void MarkChanged(int index)
    {
        _changed.set(static_cast<size_t>(index));
        _unsaved.set(static_cast<size_t>(index));
    }

    Item _slots[INVENTORY_ALL_SLOTS];            // 가방 60 + 착용 칸(색인 WORN_BASE_INDEX~)
    std::bitset<INVENTORY_ALL_SLOTS> _changed;   // 화면에 아직 안 보낸 칸(FlushInventory 가 지움)
    SlotBits                         _unsaved;   // DB 에 아직 확정 안 된 칸(저장소가 묶음에 실을 때 가져감)
    int64_t                          _money = 0;            // 가진 돈(전) — 0~MONEY_MAX
    bool                             _moneyChanged = false; // 화면에 아직 안 보낸 돈(FlushInventory 가 지움) — 저장 표시는 아니다(위 [돈])
    SlotBits                         _locked;               // 거래창에 올려 잠근 칸(위 [거래 잠금])
    int64_t                          _lockedMoney = 0;      // 거래창에 올려 잠근 돈 — 0~_money
};

#endif // USE_INVENTORY
