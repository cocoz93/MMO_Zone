#include "TradeHandler.h"

#if USE_TRADE

#include "SessionHub.h"
#include "ItemHandler.h"
#if USE_DB_WORKER
#include "CharacterStore.h"
#endif
#include "MapManager.h"
#include "Zone.h"
#include "Player.h"
#include "GameMonitor.h"
#include "PacketRoutes.h"
#include "SerialBuffer.h"
#include <Protocol/Protocol.h>
#include <cassert>
#include "PacketBuild.h"   // 머리 쓰기·닫기 · 사람 정보 쓰기(WritePartyPerson — 거래도 같은 10B 구조체를 싣는다)

using namespace PacketBuild;

// ==========================================================================
// 거래 S2C 조립 — 이 클래스만 부른다. 쓰는 법은 PacketBuild.h 머리의 [규칙] 주석과 같다(스칼라는 << · pack(1)).
// ==========================================================================
namespace
{
    CSerialBuffer* MakeTradeAsk(uint32_t requestId, const CPlayer* from, uint32_t remainMs)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_TRADE_ASK::TYPE);
        *buf << static_cast<uint32_t>(requestId);
        WritePartyPerson(buf, from);
        *buf << static_cast<uint32_t>(remainMs);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_TRADE_ASK));
        return buf;
    }

    CSerialBuffer* MakeTradeAskEnd(uint32_t requestId, TradeEnd why)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_TRADE_ASK_END::TYPE);
        *buf << static_cast<uint32_t>(requestId);
        *buf << static_cast<uint8_t>(why);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_TRADE_ASK_END));
        return buf;
    }

    // 거래창 한쪽(Protocol.h TradeSide 210B) — 쓰는 줄 뒤는 0 으로 채운다(고정 길이)
    void WriteTradeSide(CSerialBuffer* buf, const TradeHalf& half)
    {
        *buf << static_cast<int64_t>(half.money);
        *buf << static_cast<uint8_t>(half.closed ? 1 : 0);
        *buf << static_cast<uint8_t>(half.items.size());
        for (int i = 0; i < TRADE_MAX_ITEMS; ++i)
        {
            const bool used = i < static_cast<int>(half.items.size());
            *buf << static_cast<uint16_t>(used ? half.items[static_cast<size_t>(i)].slot : 0);
            *buf << static_cast<int>(used ? half.items[static_cast<size_t>(i)].itemId : 0);
            *buf << static_cast<int>(used ? half.items[static_cast<size_t>(i)].count : 0);
        }
    }

    CSerialBuffer* MakeTradeState(const TradeSession& t, int mySide)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_TRADE_STATE::TYPE);
        *buf << static_cast<uint32_t>(t.id);
        WritePartyPerson(buf, t.side[1 - mySide].player);
        WriteTradeSide(buf, t.side[mySide]);
        WriteTradeSide(buf, t.side[1 - mySide]);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_TRADE_STATE));
        return buf;
    }

    CSerialBuffer* MakeTradeEnd(TradeEnd why, int32_t actorId)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_TRADE_END::TYPE);
        *buf << static_cast<uint8_t>(why);
        *buf << static_cast<int>(actorId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_TRADE_END));
        return buf;
    }

    CSerialBuffer* MakeTradeFail(TradeFail why)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_TRADE_FAIL::TYPE);
        *buf << static_cast<uint8_t>(why);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_TRADE_FAIL));
        return buf;
    }
}

bool CTradeHandler::RegisterRoutes(CPacketRoutes& routes)
{
    bool ok = true;
    // 이 여섯 줄이 없으면 거래 요청이 조용히 버려진다(등록 안 된 번호는 OnReceived 가 조용히 버린다 — 답도 로그도 없다)
    ok &= routes.OnPlayer(MsgType::C2S_TRADE_REQUEST, sizeof(MSG_C2S_TRADE_REQUEST), [this](CPlayer* p, CSerialBuffer* m) { RecvTradeRequest(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_TRADE_ANSWER,  sizeof(MSG_C2S_TRADE_ANSWER),  [this](CPlayer* p, CSerialBuffer* m) { RecvTradeAnswer(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_TRADE_OFFER,   sizeof(MSG_C2S_TRADE_OFFER),   [this](CPlayer* p, CSerialBuffer* m) { RecvTradeOffer(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_TRADE_MONEY,   sizeof(MSG_C2S_TRADE_MONEY),   [this](CPlayer* p, CSerialBuffer* m) { RecvTradeMoney(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_TRADE_COMMIT,  sizeof(MSG_C2S_TRADE_COMMIT),  // 몸 없음 — 헤더뿐
                          [this](CPlayer* p, CSerialBuffer*) { RecvTradeCommit(p); });
    ok &= routes.OnPlayer(MsgType::C2S_TRADE_CANCEL,  sizeof(MSG_C2S_TRADE_CANCEL),  // 몸 없음 — 헤더뿐
                          [this](CPlayer* p, CSerialBuffer*) { RecvTradeCancel(p); });
    return ok;
}

// ==========================================================================
// 받기 — 규칙에 그대로 넘긴다(신청만 누른 사람을 주변에서 찾아 넘긴다)
// ==========================================================================
void CTradeHandler::RecvTradeRequest(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_TRADE_REQUEST recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    _tradeSystem.Request(player, FindPlayerAround(player, recvMsg.targetId), _frameCount);
}

void CTradeHandler::RecvTradeAnswer(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_TRADE_ANSWER recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    _tradeSystem.Answer(player, recvMsg.requestId, recvMsg.accept != 0);
}

void CTradeHandler::RecvTradeOffer(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_TRADE_OFFER recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    _tradeSystem.Offer(player, recvMsg.slot, recvMsg.count);
}

void CTradeHandler::RecvTradeMoney(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_TRADE_MONEY recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    _tradeSystem.AddMoney(player, recvMsg.amount);
}

void CTradeHandler::RecvTradeCommit(CPlayer* player)
{
    _tradeSystem.Commit(player, _frameCount);
}

void CTradeHandler::RecvTradeCancel(CPlayer* player)
{
    _tradeSystem.Cancel(player);
}

CPlayer* CTradeHandler::FindPlayerAround(CPlayer* player, int32_t actorId)
{
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return nullptr;

    // 어느 9칸인가(시야 규칙)는 섹터 관리자가 정한다(GetAroundSectorList) — 사람 목록만 본다(몬스터·NPC 를 눌러도 NO_TARGET)
    const CSectorManager& sectors = zone->GetSectorManager();
    CSectorManager::SectorPos around[CSectorManager::MAX_AROUND_SECTORS];
    int32_t aroundCount = 0;
    sectors.GetAroundSectorList(player->_sectorX, player->_sectorY, around, aroundCount);
    for (int32_t i = 0; i < aroundCount; ++i)
    {
        for (CPlayer* other : sectors.GetSectorPlayers(around[i].x, around[i].y))
        {
            if (other->_actorId == actorId)
                return other;
        }
    }
    return nullptr;
}

// ==========================================================================
// 거래 알림 (ITradeEvents)
// ==========================================================================
void CTradeHandler::OnTradeAsk(CPlayer* to, uint32_t requestId, const CPlayer* from, uint32_t remainMs)
{
    _sessionHub.SendPacket(to, MakeTradeAsk(requestId, from, remainMs));
    _gameMonitor._trade._askSent.Inc();
}

void CTradeHandler::OnTradeAskEnd(CPlayer* to, uint32_t requestId, TradeEnd why)
{
    _sessionHub.SendPacket(to, MakeTradeAskEnd(requestId, why));
    _gameMonitor._trade._askEndSent.Inc();
}

void CTradeHandler::OnTradeState(CPlayer* to, const TradeSession& trade, int mySide)
{
    _sessionHub.SendPacket(to, MakeTradeState(trade, mySide));
    _gameMonitor._trade._stateSent.Inc();
}

void CTradeHandler::OnTradeEnd(CPlayer* to, TradeEnd why, int32_t actorId)
{
    _sessionHub.SendPacket(to, MakeTradeEnd(why, actorId));
    _gameMonitor._trade._endSent.Inc();
}

void CTradeHandler::OnTradeFail(CPlayer* to, TradeFail why)
{
    _sessionHub.SendPacket(to, MakeTradeFail(why));   // 세기는 규칙 쪽(CTradeSystem::Fail — 사유별)
}

bool CTradeHandler::CanSaveSwap(const CPlayer* a, const CPlayer* b)
{
#if USE_DB_WORKER
    return !_characterStore.IsRunning() || _characterStore.CanSaveTradeNow(a, b);
#else
    (void)a; (void)b;
    return true;
#endif
}

void CTradeHandler::OnTradeSwapped(CPlayer* a, CPlayer* b)
{
    // 짝 저장 먼저 — 고리를 건 뒤라 아래 내보내기가 부르는 틱 끝 저장 요청(RequestSave(BAG))은 저장소가 붙잡는다(혼자 묶음이 안 나간다)
#if USE_DB_WORKER
    if (_characterStore.IsRunning())
        _characterStore.SaveTrade(a, b);
#endif
    _itemHandler.FlushInventory(a);
    _itemHandler.FlushInventory(b);
}

#endif // USE_TRADE
