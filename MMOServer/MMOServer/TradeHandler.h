// ==========================================================================
// CTradeHandler — 거래: 거래 요청과 거래 알림 (trade-plan.md · USE_TRADE)
//
// [책임]
//  - 받은 패킷 여섯의 처리기와 그 등록(RegisterRoutes — 신청 · 답 · 올리기 · 돈 · 교환하기 · 그만두기)
//  - 신청이 가리킨 사람을 주변에서 찾기(캐릭터 번호 → 그 사람 · 주변 9섹터의 사람 목록)
//  - 거래 알림(ITradeEvents)을 패킷으로 바꿔 보내기 · 보낸 수 세기
//  - 맞바꾼 직후 — 짝 저장(CCharacterStore::SaveTrade) · 두 사람 가방 내보내기(CItemHandler::FlushInventory)
//
// [경계]
//  판정·거래창·맞바꾸기는 CTradeSystem. 칸·돈 내보내기와 그 틱 끝 저장 요청은 CItemHandler 한 곳(FlushInventory) —
//  두 사람의 짝 묶음이 확정될 때까지 그 저장 요청은 저장소가 붙잡는다(CCharacterStore 「짝 저장」).
//  거래 규칙 객체와 그 틱(CTradeSystem::Tick)·퇴장 정리(OnLeavingZone · Forget)는 게임 서버가 든다 — 이 클래스는 그 참조를 빌려 부르고, 그 알림을 받는다.
//
// [모르는 것]
//  전투 · 파티 · 상점 · 입장. 누가 무엇을 할 수 있나는 규칙 몫이다.
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"     // USE_TRADE · USE_DB_WORKER — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_TRADE

#include "TradeSystem.h"     // ITradeEvents · TradeSession

class CPlayer;
class CSerialBuffer;
class CPacketRoutes;
class CSessionHub;
class CItemHandler;
class CMapManager;
class CGameMonitor;
#if USE_DB_WORKER
class CCharacterStore;
#endif

// 거래 알림(ITradeEvents)은 공개 상속 — 게임 서버가 거래 규칙(_tradeSystem)에 이것을 알림 받는 쪽으로 꽂는다(CPartyHandler 와 같은 꼴).
class CTradeHandler : public ITradeEvents
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(세션 허브 · CItemHandler · 저장소 · 맵 관리자 · 게임 지표 · 틱 번호)보다 뒤에 이것을 선언해야 한다.
    //   거래 규칙(tradeSystem)만 거꾸로 이것보다 뒤에 세워진다 — 그 알림 받는 쪽이 이 클래스라서. 그래서 생성 중엔 아무것도 쓰지 않는다.
    CTradeHandler(CSessionHub& sessionHub, CItemHandler& itemHandler,
#if USE_DB_WORKER
                  CCharacterStore& characterStore,
#endif
                  CTradeSystem& tradeSystem, CMapManager& mapManager, CGameMonitor& gameMonitor, const uint64_t& frameCount)
        : _sessionHub(sessionHub), _itemHandler(itemHandler)
#if USE_DB_WORKER
        , _characterStore(characterStore)
#endif
        , _tradeSystem(tradeSystem), _mapManager(mapManager), _gameMonitor(gameMonitor), _frameCount(frameCount) {}

    // 받은 패킷 등록 — 거래 요청 여섯. 게임 서버의 등록(RegisterRoutes)이 부른다. false = 등록 실패(기동 거절).
    bool RegisterRoutes(CPacketRoutes& routes);

private:
    void RecvTradeRequest(CPlayer* player, CSerialBuffer* pMsg);
    void RecvTradeAnswer(CPlayer* player, CSerialBuffer* pMsg);
    void RecvTradeOffer(CPlayer* player, CSerialBuffer* pMsg);
    void RecvTradeMoney(CPlayer* player, CSerialBuffer* pMsg);
    void RecvTradeCommit(CPlayer* player);
    void RecvTradeCancel(CPlayer* player);

    // 내 주변 9섹터의 사람 중 이 번호 — 없으면 nullptr. 몬스터·NPC 는 안 본다(사람 목록만).
    //   ★ CPartyHandler::FindPlayerAround 와 같은 식이다 — 같은 일을 하는 자리가 둘이 됐다(섹터 쪽으로 모을지는 미결 · trade-progress.md).
    CPlayer* FindPlayerAround(CPlayer* player, int32_t actorId);

    // 거래 알림 (ITradeEvents) — 뜻은 TradeSystem.h. 보내는 수는 여기서 센다(_gameMonitor._trade 의 …Sent).
    void OnTradeAsk(CPlayer* to, uint32_t requestId, const CPlayer* from, uint32_t remainMs) override;
    void OnTradeAskEnd(CPlayer* to, uint32_t requestId, TradeEnd why) override;
    void OnTradeState(CPlayer* to, const TradeSession& trade, int mySide) override;
    void OnTradeEnd(CPlayer* to, TradeEnd why, int32_t actorId) override;
    void OnTradeFail(CPlayer* to, TradeFail why) override;
    bool CanSaveSwap(const CPlayer* a, const CPlayer* b) override;
    void OnTradeSwapped(CPlayer* a, CPlayer* b) override;

    CSessionHub&     _sessionHub;      // 거래 패킷(한 사람에게)
    CItemHandler&    _itemHandler;     // 맞바꾼 두 가방 내보내기(칸 패킷 · S2C_MONEY · 틱 끝 저장 요청)
#if USE_DB_WORKER
    CCharacterStore& _characterStore;  // 짝 저장
#endif
    CTradeSystem&    _tradeSystem;     // 거래 규칙 — 빌린 것(게임 서버가 들고, 이 클래스보다 뒤에 세워진다)
    CMapManager&     _mapManager;      // 존 찾기(주변 9섹터)
    CGameMonitor&    _gameMonitor;     // 보낸 수
    const uint64_t&  _frameCount;      // 게임 루프의 틱 번호 — 빌린 것, 읽기만(신청 시간 · 교환하기)
};

#endif // USE_TRADE
