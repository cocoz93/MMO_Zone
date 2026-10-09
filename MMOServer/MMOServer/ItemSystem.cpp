#include "ItemSystem.h"

#if USE_INVENTORY

#include "Player.h"          // 생사·체력·대기시간 필드 · 가방
#include "GameDataFiles.h"   // 아이템 표
#include "GameMonitor.h"     // 쓰기·장착 지표
#include "StatSystem.h"      // 장착 뒤 공격력 다시 세우기

CItemSystem::CItemSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor, CStatSystem& statSystem)
    : _tables(tables)
    , _gameMonitor(gameMonitor)
    , _statSystem(statSystem)
{
}

// ==========================================================================
// 아이템 쓰기 (지금은 물약만)
// ==========================================================================
UseItemReject CItemSystem::CheckUseItem(const CPlayer* player, uint16_t slot, uint64_t frame, int& outIndex, const ItemDef*& outDef) const
{
    // UseItemReject 의 순서 그대로 본다 — 여럿이 겹치면 먼저 걸린 하나로 답한다(열거 순서가 곧 확인 순서).
    InvTab tab;
    if (!FindInvSlot(slot, tab, outIndex))
        return UseItemReject::BAD_SLOT;

    const Item& s = player->_inventory.At(outIndex);
    if (s.itemId == 0)
        return UseItemReject::EMPTY;

    // 표에서 사라진 번호(패치로 지운 아이템)도 여기서 "못 쓴다"로 떨어진다.
    //   「쓸 수 있나」는 표 쪽 한 곳(ItemDef::IsUsable)에 묻는다 — 표 읽기가 거부하는 기준과 같은 답이다.
    outDef = _tables.Item().Find(s.itemId);
    if (outDef == nullptr || !outDef->IsUsable())
        return UseItemReject::NOT_USABLE;

    if (player->_isDead)
        return UseItemReject::DEAD;

    if (frame < player->_itemUseReadyFrame)
        return UseItemReject::COOLDOWN;

    // 체력이 가득이면 안 줄인다 — 물약만 날리게 두지 않는다
    if (player->_hp >= player->_maxHp)
        return UseItemReject::FULL_HP;

    return UseItemReject::COUNT;   // 통과
}

UseItemReject CItemSystem::UseItem(CPlayer* player, uint16_t slot, uint64_t frame)
{
    int index = -1;
    const ItemDef* def = nullptr;
    const UseItemReject reject = CheckUseItem(player, slot, frame, index, def);
    if (reject != UseItemReject::COUNT)
    {
        _gameMonitor._inventory._useRejected[static_cast<int>(reject)].Inc();
        return reject;
    }

    // ── 성공 — 체력을 채우고(최대치에서 멈춘다) 한 개 줄이고 대기시간을 건다 ──
    const int32_t healed = player->_hp + def->healHp;   // std::min 은 윈도 헤더의 min 매크로에 먹힌다
    player->_hp = (healed < player->_maxHp) ? healed : player->_maxHp;
    player->_inventory.ConsumeOne(index);
    player->_itemUseReadyFrame = frame + ITEM_USE_COOLDOWN_FRAMES;   // 성공했을 때만 — 거절은 대기시간을 안 건다
    _gameMonitor._inventory._itemsUsed.Inc();
    return UseItemReject::COUNT;
}

#if USE_EQUIP
// ==========================================================================
// 장착 — 끼우기·빼기는 "칸 둘 맞바꾸기" 하나다(CInventory::SwapSlots).
//   저장은 지금 가방 길 그대로(부르는 쪽의 FlushInventory — 칸마다 DB 한 줄). ★ 두 줄은 따로 쓴다 — 두 문장 사이에
//   서버가 죽으면 무기 하나를 잃거나 복제될 수 있다(여러 칸 넣기와 같은 틈, 플랜 ㉯ — 거래 때 트랜잭션 레인으로).
// ==========================================================================
EquipReject CItemSystem::CheckEquip(const CPlayer* player, uint16_t slot, bool unequip, uint64_t frame, int& outFrom, int& outTo) const
{
    // EquipReject 의 순서 그대로 본다 — 여럿이 겹치면 먼저 걸린 하나로 답한다(열거 순서가 곧 확인 순서).
    const CInventory& inv = player->_inventory;

    // BAD_SLOT — 끼우기는 "가방 칸", 빼기는 "착용 칸" 이어야 한다
    InvTab tab;
    if (!FindInvSlot(slot, tab, outFrom) || IsWornIndex(outFrom) != unequip)
        return EquipReject::BAD_SLOT;

    // EMPTY
    const Item& s = inv.At(outFrom);
    if (s.itemId == 0)
        return EquipReject::EMPTY;

    // NOT_EQUIPPABLE — 끼우기만. 물약 칸을 보내도 여기서 걸린다(가방 칸이면 탭을 안 가린다 — 선언 주석)
    //   표에서 사라진 번호도 여기로 떨어진다. 무기 칸의 것은 불러오기가 이미 장비만 들였다.
    if (!unequip)
    {
        const ItemDef* def = _tables.Item().Find(s.itemId);
        if (def == nullptr || def->tab != InvTab::EQUIP)
            return EquipReject::NOT_EQUIPPABLE;
    }

    if (player->_isDead)
        return EquipReject::DEAD;

    if (frame < player->_equipReadyFrame)
        return EquipReject::COOLDOWN;

    // 맞바꿀 상대 — 끼우기는 무기 칸(비었든 찼든), 빼기는 장비 탭의 첫 빈 칸
    if (!unequip)
    {
        outTo = WornIndexOf(EquipSlot::WEAPON);   // 부위가 무기 하나뿐 — 둘째 부위가 생기면 아이템 표의 부위 칸으로 고른다
        return EquipReject::COUNT;
    }
    outTo = inv.FirstEmptyInTab(InvTab::EQUIP);
    if (outTo < 0)
        return EquipReject::TAB_FULL;   // 아무것도 안 바꾼다 — 무기는 그대로 끼워져 있다
    return EquipReject::COUNT;
}

EquipReject CItemSystem::Equip(CPlayer* player, uint16_t slot, bool unequip, uint64_t frame)
{
    int from = -1, to = -1;
    const EquipReject reject = CheckEquip(player, slot, unequip, frame, from, to);
    if (reject != EquipReject::COUNT)
    {
        _gameMonitor._equip._rejected[static_cast<int>(reject)].Inc();
        return reject;
    }

    // ── 성공 — 칸 둘을 맞바꾸고 대기시간을 건다(성공했을 때만 — 거절은 안 건다) ──
    player->_inventory.SwapSlots(from, to);
    player->_equipReadyFrame = frame + EQUIP_COOLDOWN_FRAMES;
    if (unequip) _gameMonitor._equip._unequipped.Inc();
    else         _gameMonitor._equip._equipped.Inc();

    // 무기가 바뀌었으니 공격력을 다시 세운다 — 체력은 그대로다(refillHp=false, 무기는 공격력만 바꾼다).
    //   ★ 여기서 세우는 이유 — 「끼우면 공격력이 다시 선다」는 장착 규칙이라, 다른 길이 장착을 불러도 빠뜨릴 수 없게.
    //     보내지는 않으므로 와이어 순서(칸 패킷 → S2C_MY_STATS)는 부르는 쪽이 그대로 지킨다.
    _statSystem.RecalcStats(player, false);
    return EquipReject::COUNT;
}
#endif // USE_EQUIP

#endif // USE_INVENTORY
