#include "ShopHandler.h"

#if USE_SHOP

#include "ShopSystem.h"
#include "ItemHandler.h"     // FlushInventory — 칸 패킷 + S2C_MONEY + 틱 끝 저장 요청
#include "SessionHub.h"
#include "MapManager.h"
#include "Zone.h"
#include "SectorManager.h"
#include "Player.h"
#include "NpcTable.h"        // MAX_RADIUS — NPC 를 찾는 섹터 범위
#include "PacketRoutes.h"
#include "SerialBuffer.h"
#include <Protocol/Protocol.h>
#include <cassert>
#include "PacketBuild.h"     // 머리 쓰기·닫기

using namespace PacketBuild;   // 아래 조립 함수가 머리 쓰기·닫기를 이름만으로 부른다

namespace
{
    // 사고팔기 거절 사유 → 와이어 값. 사유를 정하는 건 CShopSystem(ShopSystem.h), 패킷 값으로 바꾸는 건 여기 —
    //   ItemHandler.cpp 의 ToUseItemFail 과 같은 꼴. 통과(COUNT)는 실패 패킷이 아니다.
    ShopFail ToShopFail(ShopReject reject)
    {
        static_assert(static_cast<int>(ShopReject::COUNT) == static_cast<int>(ShopFail::COUNT), "사유를 늘렸으면 와이어(ShopFail)와 이 표에도");
        switch (reject)
        {
        case ShopReject::DEAD:             return ShopFail::DEAD;
        case ShopReject::NO_NPC:           return ShopFail::NO_NPC;
        case ShopReject::NOT_IN_REACH:     return ShopFail::NOT_IN_REACH;
        case ShopReject::NOT_SOLD_HERE:    return ShopFail::NOT_SOLD_HERE;
        case ShopReject::NOT_BOUGHT_HERE:  return ShopFail::NOT_BOUGHT_HERE;
        case ShopReject::BAD_SLOT:         return ShopFail::BAD_SLOT;
        case ShopReject::IN_TRADE:         return ShopFail::IN_TRADE;
        case ShopReject::BAD_COUNT:        return ShopFail::BAD_COUNT;
        case ShopReject::NOT_ENOUGH_MONEY: return ShopFail::NOT_ENOUGH_MONEY;
        case ShopReject::BAG_FULL:         return ShopFail::BAG_FULL;
        case ShopReject::MONEY_FULL:       return ShopFail::MONEY_FULL;
        case ShopReject::COUNT:            break;
        }
        assert(false && "거절이 아닌 결과를 실패 패킷으로 바꾸려 했다");
        return ShopFail::NO_NPC;
    }

    // 필드 순서는 Protocol.h MSG_S2C_SHOP_FAIL 과 글자 그대로 같아야 한다.
    CSerialBuffer* MakeShopFail(ShopFail reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_SHOP_FAIL::TYPE);
        *buf << static_cast<uint8_t>(reason);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_SHOP_FAIL));
        return buf;
    }
}

bool CShopHandler::RegisterRoutes(CPacketRoutes& routes)
{
    bool ok = true;
    ok &= routes.OnPlayer(MsgType::C2S_SHOP_BUY,  sizeof(MSG_C2S_SHOP_BUY),  [this](CPlayer* p, CSerialBuffer* m) { RecvShopBuy(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_SHOP_SELL, sizeof(MSG_C2S_SHOP_SELL), [this](CPlayer* p, CSerialBuffer* m) { RecvShopSell(p, m); });
    return ok;
}

const CCharacter* CShopHandler::FindAround(const CPlayer* player, int32_t actorId) const
{
    const CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return nullptr;

    // 반경 상한만큼의 섹터만 참조로 훑는다(줍기 · 공격 대상 찾기와 같은 꼴 — 복사 0). 상한 밖의 같은 번호는 「주변에 없음」이다
    const CSectorManager& sectors = zone->GetSectorManager();
    const CCharacter* found = nullptr;
    sectors.ForEachSectorInRange(player->_x, player->_y, static_cast<float>(CNpcTable::MAX_RADIUS), [&](int32_t sx, int32_t sy)
    {
        if (found != nullptr)
            return;
        sectors.ForEachInSector(sx, sy, [&](const CCharacter* c)
        {
            if (found == nullptr && c->_actorId == actorId)
                found = c;
        });
    });
    return found;
}

void CShopHandler::Finish(CPlayer* player, ShopReject reject)
{
    if (reject != ShopReject::COUNT)
    {
        _sessionHub.SendPacket(player, MakeShopFail(ToShopFail(reject)));
        return;
    }
    // 성공 알림은 따로 없다 — 칸 패킷 + 돈 패킷이 결과다(물약 쓰기 · 줍기와 같은 꼴). 칸·돈이 같이 바뀌어도 저장 요청은 하나 · 묶음 하나
    _itemHandler.FlushInventory(player);
}

void CShopHandler::RecvShopBuy(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_SHOP_BUY recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    const CCharacter* npc = FindAround(player, recvMsg.npcActorId);
    Finish(player, _shopSystem.Buy(player, npc, recvMsg.itemId, recvMsg.count));
}

void CShopHandler::RecvShopSell(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_SHOP_SELL recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    const CCharacter* npc = FindAround(player, recvMsg.npcActorId);
    Finish(player, _shopSystem.Sell(player, npc, recvMsg.slot, recvMsg.count));
}

#endif // USE_SHOP
