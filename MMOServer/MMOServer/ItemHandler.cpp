#include "ItemHandler.h"

#if USE_INVENTORY

#include "SessionHub.h"
#include "SelfNotifier.h"
#include "SightBroadcaster.h"
#include "CharacterStore.h"   // USE_DB_WORKER — 가방이 바뀐 틱 끝 저장 요청
#include "ItemSystem.h"
#include "MapManager.h"
#include "Zone.h"
#include "Player.h"
#include "GameMonitor.h"
#include "GameDataFiles.h"
#include "PacketRoutes.h"
#include "SerialBuffer.h"
#include <Protocol/Protocol.h>
#include <Common/ErrorLog.h>
#include <cassert>
#include "PacketBuild.h" // 머리 쓰기·닫기 · 체력 바뀜(MakeActorStats — 체력 물약의 주변 방송) — 게임 서버 나누기 0-②

using namespace PacketBuild;   // 아래 조립 함수들이 머리 쓰기·닫기를 이름만으로 부른다

// ==========================================================================
// 가방·땅 S2C 조립과 거절 사유 → 와이어 값 — 이 클래스만 부른다(GameServer.cpp 에서 옮김 · 몸·주석 그대로).
//   쓰는 법(스칼라는 << · 고정배열·문자열은 SetData(raw) · pack(1))은 PacketBuild.h 머리의 [규칙] 주석과 같다.
// ==========================================================================
namespace
{
#if USE_DROP
    // 땅 아이템 쪽 사유 → 와이어 값. 사유를 정하는 건 존의 땅 아이템 관리자(GroundItem.h), 패킷 값으로 바꾸는 건 여기 —
    //   위 ToZoneChangeFailReason 과 같은 꼴(10-06, 아키텍처 전수검사 7번). 존 층은 Protocol.h 를 모른다.
    GroundItemGone ToGroundItemGone(GroundItemRemoval why)
    {
        switch (why)
        {
        case GroundItemRemoval::PICKED:  return GroundItemGone::PICKED;
        case GroundItemRemoval::EXPIRED: return GroundItemGone::EXPIRED;
        case GroundItemRemoval::CLEANED:   // 알릴 사람이 없는 사유 — 관리자가 알림을 안 보낸다
        case GroundItemRemoval::COUNT:   break;
        }
        assert(false && "와이어에 안 나가는 사유를 DELETE 로 바꾸려 했다");
        return GroundItemGone::EXPIRED;
    }

    PickupFail ToPickupFail(PickupReject reject)
    {
        switch (reject)
        {
        case PickupReject::DEAD:      return PickupFail::DEAD;
        case PickupReject::NO_ITEM:   return PickupFail::NO_ITEM;
        case PickupReject::NOT_OWNER: return PickupFail::NOT_OWNER;
        case PickupReject::NONE:      break;
        }
        assert(false && "거절이 아닌 결과를 실패 패킷으로 바꾸려 했다");
        return PickupFail::NO_ITEM;
    }
#endif

#if USE_INVENTORY
    // 쓰기·장착 거절 사유 → 와이어 값. 사유를 정하는 건 CItemSystem(ItemSystem.h), 패킷 값으로 바꾸는 건 여기 —
    //   위 ToPickupFail 과 같은 꼴(10-08, 아키텍처 2차 검사 2번). 통과(COUNT)는 실패 패킷이 아니다.
    UseItemFail ToUseItemFail(UseItemReject reject)
    {
        switch (reject)
        {
        case UseItemReject::BAD_SLOT:   return UseItemFail::BAD_SLOT;
        case UseItemReject::EMPTY:      return UseItemFail::EMPTY;
        case UseItemReject::IN_TRADE:   return UseItemFail::IN_TRADE;
        case UseItemReject::NOT_USABLE: return UseItemFail::NOT_USABLE;
        case UseItemReject::DEAD:       return UseItemFail::DEAD;
        case UseItemReject::COOLDOWN:   return UseItemFail::COOLDOWN;
        case UseItemReject::FULL_HP:    return UseItemFail::FULL_HP;
        case UseItemReject::FULL_MP:    return UseItemFail::FULL_MP;
        case UseItemReject::COUNT:      break;
        }
        assert(false && "거절이 아닌 결과를 실패 패킷으로 바꾸려 했다");
        return UseItemFail::BAD_SLOT;
    }

#if USE_EQUIP
    EquipFail ToEquipFail(EquipReject reject)
    {
        switch (reject)
        {
        case EquipReject::BAD_SLOT:       return EquipFail::BAD_SLOT;
        case EquipReject::EMPTY:          return EquipFail::EMPTY;
        case EquipReject::IN_TRADE:       return EquipFail::IN_TRADE;
        case EquipReject::NOT_EQUIPPABLE: return EquipFail::NOT_EQUIPPABLE;
        case EquipReject::DEAD:           return EquipFail::DEAD;
        case EquipReject::COOLDOWN:       return EquipFail::COOLDOWN;
        case EquipReject::TAB_FULL:       return EquipFail::TAB_FULL;
        case EquipReject::COUNT:          break;
        }
        assert(false && "거절이 아닌 결과를 실패 패킷으로 바꾸려 했다");
        return EquipFail::BAD_SLOT;
    }
#endif
#endif

    // ── 인벤토리 (USE_INVENTORY) ──
    //   필드 순서는 Protocol.h 의 InventoryEntry·MSG_S2C_INVENTORY_SLOT 와 글자 그대로 같아야 한다.

    CSerialBuffer* MakeInventorySlot(uint16_t slot, int32_t itemId, int32_t count)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_INVENTORY_SLOT::TYPE);
        *buf << static_cast<uint16_t>(slot);
        *buf << static_cast<int>(itemId);
        *buf << static_cast<int>(count);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_INVENTORY_SLOT));
        return buf;
    }

    CSerialBuffer* MakeUseItemFail(uint16_t slot, UseItemFail reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_USE_ITEM_FAIL::TYPE);
        *buf << static_cast<uint16_t>(slot);
        *buf << static_cast<uint8_t>(reason);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_USE_ITEM_FAIL));
        return buf;
    }

#if USE_EQUIP
    CSerialBuffer* MakeEquipFail(uint16_t slot, EquipFail reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_EQUIP_FAIL::TYPE);
        *buf << static_cast<uint16_t>(slot);
        *buf << static_cast<uint8_t>(reason);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_EQUIP_FAIL));
        return buf;
    }
#endif

#if USE_DROP
    CSerialBuffer* MakePickupFail(PickupFail reason, int32_t itemId)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_PICKUP_FAIL::TYPE);
        *buf << static_cast<uint8_t>(reason);
        *buf << static_cast<int32_t>(itemId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_PICKUP_FAIL));
        return buf;
    }
#endif // USE_DROP
}

// 받은 패킷 등록 — 줄마다 옛 크기 표의 함정 주석을 그대로 옮겼다(게임 서버 나누기 0-④ 의 등록에서 이 클래스 몫 넷 · 6 단계).
bool CItemHandler::RegisterRoutes(CPacketRoutes& routes)
{
    bool ok = true;
#if USE_INVENTORY
    // 이 줄이 없으면 물약 쓰기가 조용히 버려진다(등록 안 된 번호는 OnReceived 가 조용히 버린다 — 답도 로그도 없다).
    ok &= routes.OnPlayer(MsgType::C2S_USE_ITEM, sizeof(MSG_C2S_USE_ITEM), [this](CPlayer* p, CSerialBuffer* m) { RecvUseItem(p, m); });
#endif
#if USE_EQUIP
    // 이 두 줄이 없으면 끼우기·빼기가 조용히 버려진다(위 물약과 같은 함정 — 거절 패킷도 안 간다).
    ok &= routes.OnPlayer(MsgType::C2S_EQUIP,   sizeof(MSG_C2S_EQUIP),   [this](CPlayer* p, CSerialBuffer* m) { RecvEquip(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_UNEQUIP, sizeof(MSG_C2S_UNEQUIP), [this](CPlayer* p, CSerialBuffer* m) { RecvUnequip(p, m); });
#endif
#if USE_DROP
    ok &= routes.OnPlayer(MsgType::C2S_PICKUP, sizeof(MSG_C2S_PICKUP),   // 몸 없음 — 헤더뿐
                           [this](CPlayer* p, CSerialBuffer* m) { RecvPickup(p, m); });
#endif
    return ok;
}

void CItemHandler::FlushInventory(CPlayer* player)
{
    CInventory& inv = player->_inventory;
    inv.TakeChanges([&](int index)
    {
        // 칸의 "최종값"을 보낸다 — 증감이 아니라서 같은 칸이 두 번 가도 결과가 같다
        const Item& s = inv.At(index);
        const uint16_t slotNo = CInventory::SlotNumberOf(index);
        _sessionHub.SendPacket(player, MakeInventorySlot(slotNo, s.itemId, s.count));
        _gameMonitor._inventory._slotUpdatesSent.Inc();
    });

    // 돈 — 바뀌었으면 최종값 한 장(만드는 곳은 본인 알림 한 곳 — SelfNotifier::SendMoney). 칸과 같은 요청에서 바뀌어도 패킷은 따로, 저장 묶음은 하나다
    const bool moneyChanged = inv.TakeMoneyChange();
    if (moneyChanged)
        _selfNotifier.SendMoney(player);

#if USE_DB_WORKER
    // DB 는 이 틱 끝에 — 가방이 스스로 켠 저장 표시(CInventory::HasUnsaved)를 저장소가 캐릭터 행과 한 묶음으로 싣는다.
    //   같은 요청에서 바뀐 체력·경험치도 같은 묶음이라 강제 종료 뒤에도 같은 순간으로 돌아온다(물약 · 방 보상 — 저장 플랜 C).
    //   빈 칸은 행을 지우지 않고 itemId 0 으로 덮는다(DB 계정에 DELETE 권한이 없다).
    //   돈은 캐릭터 행의 열이라 칸 표시가 없다 — 바뀌었으면 같은 사유(가방 — 돈은 가방의 일부)로 요청하고, 묶음에 싣는 판정은
    //   저장소가 마지막으로 실은 값과 견준다(ShouldSave). 사고팔기처럼 칸·돈이 같이 바뀌어도 요청은 하나 · 묶음은 하나 = 트랜잭션 하나.
    if (_characterStore.IsRunning() && (inv.HasUnsaved() || moneyChanged))
        _characterStore.RequestSave(player, SaveReason::BAG);
#endif
}

void CItemHandler::RecvUseItem(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_USE_ITEM recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // 판정·적용(체력·마력·개수·대기시간)·세기는 CItemSystem — 여기는 결과를 패킷으로 바꿔 내보내기만 한다
    const ItemDef* used = nullptr;
    const UseItemReject reject = _itemSystem.UseItem(player, recvMsg.slot, _frameCount, used);
    if (reject != UseItemReject::COUNT)
    {
        _sessionHub.SendPacket(player, MakeUseItemFail(recvMsg.slot, ToUseItemFail(reject)));
        return;
    }

    // 칸 갱신(본인) → 채운 쪽 통보. 효과마다 받는 사람이 다르다(10-11) — 한 물약 한 효과라 둘 중 하나만 간다.
    //   체력이면 본인 + 주변(남의 머리 위 막대도 차야 한다 — 레벨업 때 체력 방송과 같은 꼴),
    //   마력이면 본인에게만(S2C_ACTOR_MP — 남의 마력은 화면에 안 그린다).
    //   ★ 순서가 곧 와이어 순서다 — 칸 패킷이 통보보다 앞선다(시험이 이 순서로 "이 값이 물약 몫"을 가린다).
    FlushInventory(player);
    if (used->healHp > 0)
    {
        CZone* zone = _mapManager.GetZone(player->_zoneId);
        if (zone != nullptr)
            _sightBroadcaster.BroadcastAroundSector(zone, player, MakeActorStats(player->_actorId, player->_health.Hp(), player->_health.MaxHp()), false);
    }
#if USE_SKILL
    if (used->healMp > 0)
        _selfNotifier.SendActorMp(player);
#endif
}

#if USE_EQUIP
// ==========================================================================
// 장착 — 판정·맞바꾸기·대기시간·스탯은 CItemSystem::Equip(ItemSystem.cpp), 여기는 받기와 내보내기만
// ==========================================================================
void CItemHandler::RecvEquip(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_EQUIP recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    ApplyEquip(player, recvMsg.slot, false);
}

void CItemHandler::RecvUnequip(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_UNEQUIP recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    ApplyEquip(player, recvMsg.slot, true);
}

void CItemHandler::ApplyEquip(CPlayer* player, uint16_t slot, bool unequip)
{
    const EquipReject reject = _itemSystem.Equip(player, slot, unequip, _frameCount);
    if (reject != EquipReject::COUNT)
    {
        _sessionHub.SendPacket(player, MakeEquipFail(slot, ToEquipFail(reject)));
        return;
    }

    // 칸 패킷 둘(DB 는 틱 끝 묶음 하나) → 본인에게 새 공격력(공격력은 CItemSystem::Equip 이 이미 다시 세웠다).
    //   ★ 순서가 곧 와이어 순서다 — 칸 패킷이 MY_STATS 보다 앞선다(클라가 무기 칸을 먼저 알고 숫자를 채운다).
    FlushInventory(player);
    _selfNotifier.SendMyStats(player);
}
#endif // USE_EQUIP

#if USE_DROP
GroundItemContext CItemHandler::MakeGroundItemContext()
{
    return GroundItemContext{ *this, _gameMonitor };
}

// 게임 루프 5(빈 채널 정리)의 콜백에 있던 몸 그대로(게임 서버 나누기 14 단계) — 묶음은 정리 한 번에 하나 만들던 것을 존마다 만든다(참조 둘뿐이다)
void CItemHandler::OnErasingZone(CZone* zone)
{
    zone->GetGroundItemManager().ForgetAll(MakeGroundItemContext());   // 땅의 아이템은 세기만 — 목록은 존과 함께 사라진다(알릴 사람이 없다)
}

// ── 땅 아이템 알림과 번호 부탁 (IGroundItemEvents) ── 패킷 조립·송신은 이 파일 것을 그대로 쓴다.

int32_t CItemHandler::AllocGroundId()
{
    return _nextGroundId++;
}

void CItemHandler::OnGroundItemDrop(CZone* zone, const CGroundItem& item, int32_t sectorX, int32_t sectorY)
{
    _sightBroadcaster.BroadcastGroundItemCreate(zone, item, sectorX, sectorY);   // 주변에 CREATE(DROP) — 몸은 CSightBroadcaster 로 옮겼다(3 단계)
}

void CItemHandler::OnGroundItemGone(CZone* zone, int32_t sectorX, int32_t sectorY,
                                   int32_t groundId, GroundItemRemoval why, int32_t pickerId)
{
    const GroundItemGone wire = ToGroundItemGone(why);   // 사유 → 와이어 값은 여기서 — 관리자는 패킷을 모른다

    // 주변 + 같은 틱 이동자에게 DELETE — 몸은 CSightBroadcaster 로 옮겼다(3 단계)
    _sightBroadcaster.BroadcastGroundItemDelete(zone, sectorX, sectorY, groundId, wire, pickerId);
}

void CItemHandler::TickGroundItems()
{
    if (++_groundItemScanCounter < GROUND_ITEM_SCAN_FRAMES)
        return;
    _groundItemScanCounter = 0;

    // 훑기는 존마다 관리자가 — 땅에 아무것도 없는 존은 거기서 바로 끝난다
    const GroundItemContext ctx = MakeGroundItemContext();
    _mapManager.ForEachZone([&](CZone* zone)
    {
        zone->GetGroundItemManager().TickExpire(ctx, _frameCount);
    });
}

void CItemHandler::RecvPickup(CPlayer* player, CSerialBuffer* /*pMsg*/)   // 몸이 없다 — 헤더뿐
{
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    CGroundItemManager& groundItems = zone->GetGroundItemManager();
    CGroundItemManager::PickupTarget at;
    const PickupReject reject = groundItems.FindPickup(player, _frameCount, at);   // 존의 판정(그 층의 어휘)
    PickupFail fail = (reject == PickupReject::NONE) ? PickupFail::COUNT : ToPickupFail(reject);   // 와이어 값으로는 여기서
    int32_t failItemId = 0;

    if (reject == PickupReject::NONE)
    {
        const CGroundItem& g = groundItems.GetSectorItems(at.sectorX, at.sectorY)[at.index];
        const ItemDef* def = _gameDataFiles.Item().Find(g.item.itemId);
        if (def == nullptr)
        {
            assert(false && "드랍 표 로드가 아이템 표에 없는 번호를 막는다");
            return;
        }

        // ★ 가방 넣기가 먼저, 땅에서 빼기가 나중 — 반대면 가득 찬 가방에서 아이템이 사라진다
        const InvAddResult r = player->_inventory.TryAdd(*def, g.item.count);
        if (r == InvAddResult::OK)
        {
            if (def->store == ItemStore::MONEY)
                _gameMonitor._inventory._moneyIn[static_cast<int>(MoneySource::PICKUP)].Add(g.item.count);   // 돈 무더기 — 개수 = 금액(전)
            else
                _gameMonitor._inventory._itemsAdded.Add(g.item.count);   // /item 과 같은 계수기 — 가방에 들어간 개수(돈은 단위가 달라 위에만)
            groundItems.Remove(MakeGroundItemContext(), at.sectorX, at.sectorY, at.index,
                               GroundItemRemoval::PICKED, player->_actorId);
            FlushInventory(player);   // 칸 패킷 + 그 틱 끝 저장 요청
            return;
        }

        // 드랍 아이템은 표에 있고 개수 1 이상이라 TryAdd 가 돌려줄 거절은 둘 — 그 탭이 가득(칸 아이템) · 돈 상한(돈 아이템).
        //   둘 다 아이템은 땅에 그대로. 그 밖의 값은 올 수 없다(위 두 조건이 막는다) — 갈래를 하나로 박지 않고 다 적는다
        _gameMonitor._inventory._addRejected[static_cast<int>(r)].Inc();
        switch (r)
        {
        case InvAddResult::TAB_FULL:   fail = PickupFail::TAB_FULL;   break;
        case InvAddResult::MONEY_FULL: fail = PickupFail::MONEY_FULL; break;
        case InvAddResult::OK:
        case InvAddResult::UNKNOWN_ITEM:
        case InvAddResult::BAD_COUNT:
        case InvAddResult::COUNT:
            assert(false && "드랍 아이템 넣기는 가득 · 돈 상한 말고 거절할 수 없다");
            fail = PickupFail::TAB_FULL;
            break;
        }
        failItemId = g.item.itemId;   // 클라가 어느 탭이 찼는지 · 돈이 넘쳤는지 안다
    }

    _gameMonitor._drop._pickupRejected[static_cast<int>(fail)].Inc();
    _sessionHub.SendPacket(player, MakePickupFail(fail, failItemId));
}
#endif // USE_DROP

#endif // USE_INVENTORY
