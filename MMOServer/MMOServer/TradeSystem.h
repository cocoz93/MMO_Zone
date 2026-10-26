// ==========================================================================
// TradeSystem — 플레이어 간 거래 규칙 (USE_TRADE · MyDocs/claude-plans/trade-plan.md)
//
// [책임]
//  - 신청 표(시간 지남) · 거래창(TradeSession — 두 사람 · 올린 칸·개수·돈 · 닫힘 둘) · 요청마다 판정 · 칸·돈 잠그기/풀기(가방에 맡김)
//  - 맞바꾸기 — 두 가방의 복사본에 풀기 → 빼기 → 넣기 → 돈을 해 보고, 둘 다 되면 원본에 대입한다(하나라도 안 되면 원본 무변경 · 거래 끝)
//  - 그 과정에서 정한 것을 지표(CGameMonitor)로 센다 · 성사마다 로그 한 줄([TRADE] — 아이템 고유 번호가 없어 문의 추적은 이 줄뿐이다)
//  - 보내기·저장은 하지 않는다 — 거래 조각(CTradeHandler)이 알림(ITradeEvents)을 받아 패킷·가방 내보내기·짝 저장을 한다
//
// [규칙 — 10-26 사용자 결정, Protocol.h 거래 절 머리말]
//  어디서 — 마을(MapKind::TOWN)·사냥터(DUNGEON)에서만. 파티 방(INSTANCE)은 신청을 NOT_HERE 로 거절한다(TradeAllowedOn — 맵 종류마다 한 줄).
//    신청에서만 본다 — 수락·맞바꾸기는 같은 존이라 같은 맵이고, 맵을 옮기면 신청·거래가 LEFT 로 끝난다.
//  올릴 수 있는 것 = 가방 칸 최대 TRADE_MAX_ITEMS + 돈 · 「교환하기」 한 버튼 · 누른 사람 쪽만 닫힘(더 못 올림) · 상대는 닫기 전까지 계속 올린다 ·
//  둘 다 닫히면 맞바꾼다 · 올린 것은 못 뺀다(같은 칸 두 번 · 개수 줄이기 · 돈 깎기 없음) — 내가 누른 뒤 상대가 할 수 있는 건 더 올리기뿐이다.
//  칸은 통째로 잠근다 — 10개 중 3개만 올려도 그 칸의 나머지 7개도 거래가 끝날 때까지 못 쓴다(칸 잠금이 비트 하나라서 · 가방 [거래 잠금]).
//  신청·거래는 사람마다 하나씩 — 보낸 신청 · 받은 신청 · 거래창 중 하나라도 있으면 다른 신청을 못 보내고 못 받는다(SELF_BUSY · TARGET_BUSY).
//
// [맞바꾸기는 틱에서도 다시 본다] 둘 다 닫힌 순간 바로 해 보고, 저장 쪽이 받을 수 없으면(두 사람 중 떠 있는 저장 묶음이 있다 —
//  CCharacterStore::CanSaveTradeNow) 틱마다 다시 본다. TRADE_SAVE_WAIT_FRAMES 를 넘으면 SAVE_DELAY 로 끝낸다(아무것도 안 바뀜).
//  그동안은 둘 다 닫혀 있어 아무것도 못 올리고, 취소는 된다.
//
// [끝나는 계기] 취소 요청 · 존에서 빠짐(끊김 · 맵 이동 — CGameServer::OnLeavingZone) · 쓰러짐 · 맞바꾸기 성사·실패.
//  쓰러짐은 틱에서 본다 — 죽는 길(피해 · 스킬 · 운영 명령)이 여럿이라 자리마다 부르면 하나를 빠뜨려도 화면이 멀쩡하다(파티 상태 비교와 같은 까닭).
//  틱 사이에 쓰러진 사람의 요청은 요청마다 DEAD 로 거절하고(신청 · 수락 · 올리기 · 돈 · 교환하기), 둘 다 닫혀 그 자리에서 맞바꿀 때도
//  두 사람의 생사를 먼저 본다(TryExecute — 쓰러졌으면 DEAD 로 끝) — 틱의 생사 확인을 기다리지 않는다.
//
// [목록은 틱 너머로 주소를 든다] 신청·거래창의 CPlayer* — 존에서 빠질 때 지운다(OnLeavingZone). 놓기의 떠남(Forget)에서도 한 번 더(두 번 무해).
//  거래는 존을 안 넘는다 — 맵을 옮기면 끝난다(파티와 다르다).
//
// [계층 경계] CPlayer · CInventory · 아이템 표 · CGameMonitor 를 안다. 세션·패킷·섹터·저장소는 모른다 —
//  누른 사람을 주변에서 찾는 일은 거래 조각(CTradeHandler::FindPlayerAround)이 하고 찾은 사람(없으면 nullptr)을 넘긴다.
//  객체는 CGameServer 가 멤버로 든다 — _gameMonitor · _gameDataFiles · 알림 받는 _tradeHandler 뒤에 선언한다(그 참조를 받는다).
//
// [와이어 enum 을 그대로 쓴다] 거절(TradeFail)·끝(TradeEnd)은 정하는 쪽과 와이어가 1:1 이라 두 벌을 두지 않는다(파티 PartyNotice 와 같은 꼴).
// ==========================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "BuildConfig.h"          // USE_TRADE — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include <Protocol/Protocol.h>    // TradeFail · TradeEnd · TRADE_MAX_ITEMS
#include "MapTable.h"             // MapKind — 거래가 되는 맵 종류

#if USE_TRADE

class CPlayer;
class CGameDataFiles;
class CGameMonitor;

// 거래창 한쪽 — 그 사람 · 올린 칸(올린 순서) · 올린 돈 · 닫힘. 와이어의 한쪽(Protocol.h TradeSide)은 거래 조각이 이것을 읽어 만든다.
struct TradeHalf
{
    struct Offer
    {
        int      index  = -1;   // 가방 배열 색인(CInventory::At)
        uint16_t slot   = 0;    // 와이어·DB 칸 번호(INV_TAB_RANGES)
        int32_t  itemId = 0;    // 올릴 때의 아이템 — 맞바꾸기 직전에 칸과 다시 견준다(잠근 칸이라 같아야 한다)
        int32_t  count  = 0;    // 올린 개수(1..그 칸 개수)
    };
    CPlayer*           player = nullptr;
    std::vector<Offer> items;            // 0 ~ TRADE_MAX_ITEMS
    int64_t            money  = 0;       // 올린 돈(합) — 가방의 잠근 돈과 같다
    bool               closed = false;   // 교환하기를 눌렀다
};

struct TradeSession
{
    uint32_t  id = 0;               // 거래창 번호(0 은 안 씀) — S2C_TRADE_STATE.tradeId
    TradeHalf side[2];              // [0] = 신청한 사람 · [1] = 받은 사람
    uint64_t  readyFrame = 0;       // 둘 다 닫힌 틱 — 0 = 아직(저장 기다림 TRADE_SAVE_WAIT_FRAMES 의 기준)
};

// 거래가 정한 것을 내보낸다 — 거래 조각 CTradeHandler 가 구현한다(IPartyEvents 와 같은 꼴). ★ 순수 가상이라 빠뜨리면 컴파일이 안 된다.
class ITradeEvents
{
public:
    virtual ~ITradeEvents() = default;

    virtual void OnTradeAsk(CPlayer* to, uint32_t requestId, const CPlayer* from, uint32_t remainMs) = 0;   // S2C_TRADE_ASK
    virtual void OnTradeAskEnd(CPlayer* to, uint32_t requestId, TradeEnd why) = 0;                       // S2C_TRADE_ASK_END
    virtual void OnTradeState(CPlayer* to, const TradeSession& trade, int mySide) = 0;                   // S2C_TRADE_STATE(mine = side[mySide])
    virtual void OnTradeEnd(CPlayer* to, TradeEnd why, int32_t actorId) = 0;                             // S2C_TRADE_END
    virtual void OnTradeFail(CPlayer* to, TradeFail why) = 0;                                            // S2C_TRADE_FAIL
    // 저장 쪽이 지금 짝 묶음을 받을 수 있나 — 맞바꾸기 직전(거짓이면 다음 틱에 다시)
    virtual bool CanSaveSwap(const CPlayer* a, const CPlayer* b) = 0;
    // 맞바꾼 직후(같은 틱) — 짝 저장 · 두 사람 가방 내보내기
    virtual void OnTradeSwapped(CPlayer* a, CPlayer* b) = 0;
};

// 이 맵 종류에서 거래가 되나 — 종류마다 한 줄(새 종류가 생기면 여기서 정한다 · 빠뜨리면 COUNT 갈래로 「안 됨」)
bool TradeAllowedOn(MapKind kind);

class CTradeSystem
{
public:
    // 신청을 기다리는 시간 — 10초. ★ 임시값(trade-plan.md 흐름 1 — 수치는 사람 몫)
    static constexpr uint64_t ASK_TIMEOUT_FRAMES = 10ull * FRAME_PER_SEC;
    // 둘 다 닫힌 뒤 저장 쪽을 기다리는 상한 — 30초. ★ 임시값(trade-plan.md 「위험」)
    static constexpr uint64_t TRADE_SAVE_WAIT_FRAMES = 30ull * FRAME_PER_SEC;
    // 손 닿는 거리 — 3칸(신청 · 수락 · 맞바꾸기 때만 본다 — trade-plan.md 정해진 것 2)
    static constexpr float TRADE_REACH = 3.0f;

    CTradeSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor, ITradeEvents& events);

    // 화면에서 누른 사람에게 신청 — target 은 거래 조각이 주변에서 찾은 사람(없으면 nullptr → NO_TARGET)
    void Request(CPlayer* me, CPlayer* target, uint64_t frame);
    // 받은 신청에 답한다 — 수락이면 거래창이 열린다(두 사람에게 상태 한 장)
    void Answer(CPlayer* me, uint32_t requestId, bool accept);
    // 가방 칸 하나를 count 개 올린다 · 돈을 amount 더 올린다 — 둘 다 「늘리기만」
    void Offer(CPlayer* me, uint16_t slot, int32_t count);
    void AddMoney(CPlayer* me, int64_t amount);
    // 교환하기 — 내 쪽을 닫는다. 둘 다 닫히면 그 자리에서 맞바꾸기를 해 본다
    void Commit(CPlayer* me, uint64_t frame);
    // 그만두기 — 거래창이면 거래를, 아니면 내가 보낸 신청을 거둔다
    void Cancel(CPlayer* me);

    // 틱마다 1회 — 신청 시간 지남 · 쓰러짐 · 저장을 기다리던 맞바꾸기. 둘 다 비면 바로 끝난다(부하 시험엔 거래가 없다).
    void Tick(uint64_t frame);

    // 존에서 빠짐(끊김 · 맵 이동) — 낀 신청·거래를 끝낸다(LEFT). 이 사람에게도 보낸다(맵 이동이면 아직 접속 중이다)
    void OnLeavingZone(CPlayer* player);
    // 떠남 — 위와 같되 이 사람에게는 안 보낸다(세션이 없다). 두 번 불러도 무해
    void Forget(CPlayer* player);

private:
    struct PendingTrade
    {
        uint32_t id          = 0;
        CPlayer* from        = nullptr;
        CPlayer* to          = nullptr;
        uint64_t expireFrame = 0;
    };

    bool Busy(const CPlayer* p) const;               // 보낸·받은 신청 · 거래창 중 하나라도 있나
    TradeSession* SessionOf(const CPlayer* p, int* outSide = nullptr);
    void SendState(const TradeSession& t);           // 두 사람에게 상태 한 장씩
    void Fail(CPlayer* to, TradeFail why);           // 거절 — 사유별로 세고 보낸다

    // 신청 표에서 i 번째를 뺀다 — 끝난 까닭을 센다(받아들여진 것은 _opened 가 센다). 알림은 부르는 쪽 몫
    PendingTrade TakeRequest(size_t i, TradeEnd why, bool accepted);
    // 거래창을 끝낸다 — 두 가방 잠금을 풀고, 두 사람(skip 빼고)에게 END(why, actorId)
    void EndSession(size_t i, TradeEnd why, int32_t actorId, const CPlayer* skip);
    // 둘 다 닫힌 거래창 — 저장 쪽 기다림 · 다시 판정 · 맞바꾸기. 끝났으면 true(거래창이 지워졌다)
    bool TryExecute(size_t i, uint64_t frame);
    // 맞바꾸기 판정·적용 — TradeEnd::DONE 이면 원본에 대입했다 · 그 밖이면 원본 무변경. outWho = 걸린 사람
    TradeEnd Swap(TradeSession& t, int32_t& outWho);
    // 이 사람이 낀 신청·거래를 다 끝낸다(OnLeavingZone · Forget 공용) — skip 이면 이 사람에게는 안 보낸다
    void Drop(CPlayer* player, TradeEnd why, bool skipSelf);

    const CGameDataFiles& _tables;
    CGameMonitor&         _gameMonitor;
    ITradeEvents&         _events;

    std::vector<PendingTrade> _requests;   // 신청 표 — 사람마다 하나라 접속자 수로 묶인다
    std::vector<TradeSession> _sessions;   // 열린 거래창
    uint32_t _nextRequestId = 1;           // 0 은 안 쓴다(돌아 감기면 1 부터)
    uint32_t _nextTradeId   = 1;
};

#endif // USE_TRADE
