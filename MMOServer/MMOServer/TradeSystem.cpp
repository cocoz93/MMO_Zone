#include "TradeSystem.h"

#if USE_TRADE

#include <cassert>
#include <string>
#include "Player.h"          // 가방 · 생사 · 자리 · 존 번호
#include "GameDataFiles.h"   // 아이템 표(거래 가능 · 넣기의 정의)
#include "GameMonitor.h"     // 거래 계수기
#include "Reach.h"           // 손 닿는 범위 식 — 포털·상점과 같은 한 곳
#include "MapManager.h"      // GetMapIdFromZoneId — 존 번호 → 맵 번호 풀기(정적 함수만)
#include <Common/ErrorLog.h>

CTradeSystem::CTradeSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor, ITradeEvents& events)
    : _tables(tables)
    , _gameMonitor(gameMonitor)
    , _events(events)
{
}

bool TradeAllowedOn(MapKind kind)
{
    static_assert(static_cast<int>(MapKind::COUNT) == 3, "맵 종류를 늘렸으면 거래가 되는지 여기서 정할 것");
    switch (kind)
    {
    case MapKind::TOWN:     return true;    // 마을
    case MapKind::DUNGEON:  return true;    // 사냥터
    case MapKind::INSTANCE: return false;   // 파티 방 — 10-26 사용자 결정(마을·사냥터에서만)
    case MapKind::COUNT:    break;
    }
    return false;
}

namespace
{
    // 두 사람이 같은 존에서 손 닿는 거리인가 — 신청 · 수락 · 맞바꾸기가 같은 식으로 본다
    bool InReach(const CPlayer* a, const CPlayer* b)
    {
        return a->_zoneId == b->_zoneId && Reach::InRadius(a->_x, a->_y, b->_x, b->_y, CTradeSystem::TRADE_REACH);
    }
}

// ==========================================================================
// 조회
// ==========================================================================
bool CTradeSystem::Busy(const CPlayer* p) const
{
    for (const PendingTrade& r : _requests)
    {
        if (r.from == p || r.to == p)
            return true;
    }
    for (const TradeSession& t : _sessions)
    {
        if (t.side[0].player == p || t.side[1].player == p)
            return true;
    }
    return false;
}

TradeSession* CTradeSystem::SessionOf(const CPlayer* p, int* outSide)
{
    for (TradeSession& t : _sessions)
    {
        for (int s = 0; s < 2; ++s)
        {
            if (t.side[s].player == p)
            {
                if (outSide != nullptr)
                    *outSide = s;
                return &t;
            }
        }
    }
    return nullptr;
}

void CTradeSystem::SendState(const TradeSession& t)
{
    _events.OnTradeState(t.side[0].player, t, 0);
    _events.OnTradeState(t.side[1].player, t, 1);
}

void CTradeSystem::Fail(CPlayer* to, TradeFail why)
{
    _gameMonitor._trade._fails[static_cast<int>(why)].Inc();
    _events.OnTradeFail(to, why);
}

// ==========================================================================
// 신청 · 답
// ==========================================================================
void CTradeSystem::Request(CPlayer* me, CPlayer* target, uint64_t frame)
{
    // 확인 순서: NO_TARGET → SELF → NOT_HERE → DEAD → TARGET_DEAD → SELF_BUSY → TARGET_BUSY → NOT_IN_REACH. 먼저 걸린 하나로 답한다
    if (target == nullptr)                { Fail(me, TradeFail::NO_TARGET);    return; }
    if (target == me)                     { Fail(me, TradeFail::SELF);         return; }
    {
        // 어디서 — 내 맵의 종류(상대는 주변 9섹터에서 찾았으니 같은 존 · 같은 맵이다). 표에 없는 맵은 있을 수 없지만 「안 됨」으로
        const MapDef* map = _tables.Maps().Find(CMapManager::GetMapIdFromZoneId(me->_zoneId));
        if (map == nullptr || !TradeAllowedOn(map->kind)) { Fail(me, TradeFail::NOT_HERE); return; }
    }
    if (me->_health.IsDead())             { Fail(me, TradeFail::DEAD);         return; }
    if (target->_health.IsDead())         { Fail(me, TradeFail::TARGET_DEAD);  return; }
    if (Busy(me))                         { Fail(me, TradeFail::SELF_BUSY);    return; }
    if (Busy(target))                     { Fail(me, TradeFail::TARGET_BUSY);  return; }
    if (!InReach(me, target))             { Fail(me, TradeFail::NOT_IN_REACH); return; }

    PendingTrade r;
    r.id = _nextRequestId++;
    if (_nextRequestId == 0)
        _nextRequestId = 1;
    r.from        = me;
    r.to          = target;
    r.expireFrame = frame + ASK_TIMEOUT_FRAMES;
    _requests.push_back(r);
    _gameMonitor._trade._requests.Inc();
    _gameMonitor._trade._pending.Store(static_cast<int64_t>(_requests.size()));
    _events.OnTradeAsk(target, r.id, me, static_cast<uint32_t>(ASK_TIMEOUT_FRAMES * 1000 / FRAME_PER_SEC));
}

CTradeSystem::PendingTrade CTradeSystem::TakeRequest(size_t i, TradeEnd why, bool accepted)
{
    const PendingTrade r = _requests[i];
    _requests.erase(_requests.begin() + static_cast<std::ptrdiff_t>(i));
    if (!accepted)
        _gameMonitor._trade._requestsEnded[static_cast<int>(why)].Inc();
    _gameMonitor._trade._pending.Store(static_cast<int64_t>(_requests.size()));
    return r;
}

void CTradeSystem::Answer(CPlayer* me, uint32_t requestId, bool accept)
{
    size_t i = 0;
    for (; i < _requests.size(); ++i)
    {
        if (_requests[i].id == requestId && _requests[i].to == me)
            break;
    }
    if (i == _requests.size())
    {
        Fail(me, TradeFail::NO_REQUEST);
        return;
    }

    if (!accept)
    {
        const PendingTrade r = TakeRequest(i, TradeEnd::DECLINED, false);
        _events.OnTradeEnd(r.from, TradeEnd::DECLINED, me->_actorId);
        return;
    }

    // 수락 — 그새 처지가 바뀌었을 수 있다(쓰러짐 · 멀어짐). 안 되면 신청을 지우고 나에게 거절, 보낸 사람에게 CANCELLED
    CPlayer* from = _requests[i].from;
    TradeFail reject = TradeFail::COUNT;
    if (me->_health.IsDead())        reject = TradeFail::DEAD;
    else if (from->_health.IsDead()) reject = TradeFail::TARGET_DEAD;
    else if (!InReach(me, from))     reject = TradeFail::NOT_IN_REACH;
    if (reject != TradeFail::COUNT)
    {
        TakeRequest(i, TradeEnd::CANCELLED, false);
        Fail(me, reject);
        _events.OnTradeEnd(from, TradeEnd::CANCELLED, me->_actorId);
        return;
    }

    TakeRequest(i, TradeEnd::COUNT, true);
    TradeSession t;
    t.id = _nextTradeId++;
    if (_nextTradeId == 0)
        _nextTradeId = 1;
    t.side[0].player = from;
    t.side[1].player = me;
    t.side[0].items.reserve(TRADE_MAX_ITEMS);
    t.side[1].items.reserve(TRADE_MAX_ITEMS);
    _sessions.push_back(std::move(t));
    _gameMonitor._trade._opened.Inc();
    _gameMonitor._trade._active.Store(static_cast<int64_t>(_sessions.size()));
    SendState(_sessions.back());
}

// ==========================================================================
// 올리기 · 돈 · 교환하기 · 그만두기
// ==========================================================================
void CTradeSystem::Offer(CPlayer* me, uint16_t slot, int32_t count)
{
    // 확인 순서: NOT_IN_TRADE → DEAD → CLOSED → BAD_SLOT → ALREADY_OFFERED → NOT_TRADABLE → BAD_COUNT → ITEMS_FULL
    int s = 0;
    TradeSession* t = SessionOf(me, &s);
    if (t == nullptr)        { Fail(me, TradeFail::NOT_IN_TRADE); return; }
    if (me->_health.IsDead()) { Fail(me, TradeFail::DEAD);        return; }   // 틱의 생사 확인이 곧 거래를 끝낸다 — 그 전에 온 요청
    TradeHalf& half = t->side[s];
    if (half.closed)         { Fail(me, TradeFail::CLOSED);       return; }

    CInventory& inv = me->_inventory;
    InvTab tab;
    int index = -1;
    if (!FindInvSlot(slot, tab, index) || IsWornIndex(index) || inv.At(index).itemId == 0)
    {
        Fail(me, TradeFail::BAD_SLOT);
        return;
    }
    if (inv.IsLocked(index)) { Fail(me, TradeFail::ALREADY_OFFERED); return; }
    const Item& it = inv.At(index);
    const ItemDef* def = _tables.Item().Find(it.itemId);
    if (def == nullptr || !def->tradable) { Fail(me, TradeFail::NOT_TRADABLE); return; }   // 표에서 사라진 번호도 여기로
    if (count < 1 || count > it.count)    { Fail(me, TradeFail::BAD_COUNT);    return; }
    if (half.items.size() >= static_cast<size_t>(TRADE_MAX_ITEMS)) { Fail(me, TradeFail::ITEMS_FULL); return; }

    inv.LockSlot(index);
    half.items.push_back(TradeHalf::Offer{ index, slot, it.itemId, count });
    SendState(*t);
}

void CTradeSystem::AddMoney(CPlayer* me, int64_t amount)
{
    // 확인 순서: NOT_IN_TRADE → DEAD → CLOSED → BAD_COUNT → NOT_ENOUGH_MONEY
    int s = 0;
    TradeSession* t = SessionOf(me, &s);
    if (t == nullptr)                                 { Fail(me, TradeFail::NOT_IN_TRADE);     return; }
    if (me->_health.IsDead())                         { Fail(me, TradeFail::DEAD);             return; }
    TradeHalf& half = t->side[s];
    if (half.closed)                                  { Fail(me, TradeFail::CLOSED);           return; }
    if (amount < 1)                                   { Fail(me, TradeFail::BAD_COUNT);        return; }
    if (amount > me->_inventory.SpendableMoney())     { Fail(me, TradeFail::NOT_ENOUGH_MONEY); return; }

    me->_inventory.LockMoney(amount);
    half.money += amount;
    SendState(*t);
}

void CTradeSystem::Commit(CPlayer* me, uint64_t frame)
{
    // 확인 순서: NOT_IN_TRADE → DEAD → CLOSED
    int s = 0;
    TradeSession* t = SessionOf(me, &s);
    if (t == nullptr)          { Fail(me, TradeFail::NOT_IN_TRADE); return; }
    if (me->_health.IsDead())  { Fail(me, TradeFail::DEAD);         return; }
    if (t->side[s].closed)     { Fail(me, TradeFail::CLOSED);       return; }

    t->side[s].closed = true;
    SendState(*t);
    if (t->side[1 - s].closed)
    {
        t->readyFrame = frame;
        TryExecute(static_cast<size_t>(t - _sessions.data()), frame);
    }
}

void CTradeSystem::Cancel(CPlayer* me)
{
    int s = 0;
    if (TradeSession* t = SessionOf(me, &s))
    {
        EndSession(static_cast<size_t>(t - _sessions.data()), TradeEnd::CANCELLED, me->_actorId, nullptr);
        return;
    }
    for (size_t i = 0; i < _requests.size(); ++i)
    {
        if (_requests[i].from == me)
        {
            const PendingTrade r = TakeRequest(i, TradeEnd::CANCELLED, false);
            _events.OnTradeAskEnd(r.to, r.id, TradeEnd::CANCELLED);
            return;
        }
    }
    Fail(me, TradeFail::NOT_IN_TRADE);
}

// ==========================================================================
// 끝내기
// ==========================================================================
void CTradeSystem::EndSession(size_t i, TradeEnd why, int32_t actorId, const CPlayer* skip)
{
    TradeSession t = std::move(_sessions[i]);
    _sessions.erase(_sessions.begin() + static_cast<std::ptrdiff_t>(i));
    _gameMonitor._trade._ended[static_cast<int>(why)].Inc();
    _gameMonitor._trade._active.Store(static_cast<int64_t>(_sessions.size()));
    for (int s = 0; s < 2; ++s)
    {
        CPlayer* p = t.side[s].player;
        p->_inventory.UnlockAll();   // 성사면 이미 잠금 없는 복사본이 들어가 있다(무해)
        if (p == skip)
            continue;
        // DONE 이면 상대 번호 · 그 밖이면 끝낸 사람 번호
        const int32_t who = (why == TradeEnd::DONE) ? t.side[1 - s].player->_actorId : actorId;
        _events.OnTradeEnd(p, why, who);
    }
}

void CTradeSystem::Drop(CPlayer* player, TradeEnd why, bool skipSelf)
{
    for (size_t i = 0; i < _requests.size();)
    {
        const PendingTrade& r = _requests[i];
        if (r.from != player && r.to != player)
        {
            ++i;
            continue;
        }
        const PendingTrade taken = TakeRequest(i, why, false);
        if (taken.from == player)
        {
            _events.OnTradeAskEnd(taken.to, taken.id, why);              // 받은 사람 창을 닫는다
            if (!skipSelf)
                _events.OnTradeEnd(taken.from, why, player->_actorId);
        }
        else
        {
            _events.OnTradeEnd(taken.from, why, player->_actorId);       // 보낸 사람에게 — 받을 사람이 빠졌다
            if (!skipSelf)
                _events.OnTradeAskEnd(taken.to, taken.id, why);
        }
    }
    if (TradeSession* t = SessionOf(player))
        EndSession(static_cast<size_t>(t - _sessions.data()), why, player->_actorId, skipSelf ? player : nullptr);
}

void CTradeSystem::OnLeavingZone(CPlayer* player)
{
    Drop(player, TradeEnd::LEFT, false);
}

void CTradeSystem::Forget(CPlayer* player)
{
    Drop(player, TradeEnd::LEFT, true);
}

// ==========================================================================
// 틱 — 시간 지남 · 쓰러짐 · 저장을 기다리던 맞바꾸기
// ==========================================================================
void CTradeSystem::Tick(uint64_t frame)
{
    if (_requests.empty() && _sessions.empty())
        return;

    for (size_t i = 0; i < _requests.size();)
    {
        const PendingTrade& r = _requests[i];
        TradeEnd why = TradeEnd::COUNT;
        int32_t who = 0;
        if (r.from->_health.IsDead())      { why = TradeEnd::DEAD; who = r.from->_actorId; }
        else if (r.to->_health.IsDead())   { why = TradeEnd::DEAD; who = r.to->_actorId; }
        else if (frame >= r.expireFrame)   { why = TradeEnd::EXPIRED; }
        if (why == TradeEnd::COUNT)
        {
            ++i;
            continue;
        }
        const PendingTrade taken = TakeRequest(i, why, false);
        _events.OnTradeAskEnd(taken.to, taken.id, why);
        _events.OnTradeEnd(taken.from, why, who);
    }

    for (size_t i = 0; i < _sessions.size();)
    {
        TradeSession& t = _sessions[i];
        const CPlayer* dead = t.side[0].player->_health.IsDead() ? t.side[0].player
                            : t.side[1].player->_health.IsDead() ? t.side[1].player : nullptr;
        if (dead != nullptr)
        {
            EndSession(i, TradeEnd::DEAD, dead->_actorId, nullptr);
            continue;
        }
        if (t.side[0].closed && t.side[1].closed && TryExecute(i, frame))
            continue;   // 끝났다(지워졌다) — 같은 i 가 다음 거래창이다
        ++i;
    }
}

// ==========================================================================
// 맞바꾸기
// ==========================================================================
bool CTradeSystem::TryExecute(size_t i, uint64_t frame)
{
    TradeSession& t = _sessions[i];
    CPlayer* a = t.side[0].player;
    CPlayer* b = t.side[1].player;

    // 생사 — 교환하기 요청에서 바로 오면 틱의 생사 확인을 아직 안 지났다(그 틱에 앞서 쓰러졌을 수 있다)
    if (a->_health.IsDead() || b->_health.IsDead())
    {
        EndSession(i, TradeEnd::DEAD, (a->_health.IsDead() ? a : b)->_actorId, nullptr);
        return true;
    }

    // 저장 쪽 — 두 사람 다 떠 있는 묶음이 없어야 짝 묶음을 낼 수 있다(CCharacterStore::CanSaveTradeNow)
    if (!_events.CanSaveSwap(a, b))
    {
        if (frame - t.readyFrame >= TRADE_SAVE_WAIT_FRAMES)
        {
            SLOG_WARN("[TRADE] save wait over (trade={}, a={}, b={}) — 아무것도 안 바꾸고 끝낸다", t.id, a->_accountId, b->_accountId);
            EndSession(i, TradeEnd::SAVE_DELAY, 0, nullptr);
            return true;
        }
        _gameMonitor._trade._saveWaits.Inc();
        return false;
    }

    int32_t who = 0;
    const TradeEnd r = Swap(t, who);
    if (r != TradeEnd::DONE)
    {
        EndSession(i, r, who, nullptr);
        return true;
    }

    // 성사 — 로그 한 줄(누가 · 누구에게 · 무엇을) → 저장·내보내기(알림) → 끝
    std::string gives[2];
    int64_t moved = 0;
    for (int s = 0; s < 2; ++s)
    {
        for (const TradeHalf::Offer& o : t.side[s].items)
        {
            gives[s] += std::to_string(o.itemId) + "x" + std::to_string(o.count) + " ";
            moved += o.count;
        }
    }
    SLOG_INFO("[TRADE] done trade={} a={} b={} a_gives=[{}] money {} b_gives=[{}] money {}",
              t.id, a->_accountId, b->_accountId, gives[0], t.side[0].money, gives[1], t.side[1].money);
    _gameMonitor._trade._itemsMoved.Add(moved);
    _gameMonitor._trade._moneyMoved.Add(t.side[0].money + t.side[1].money);

    _events.OnTradeSwapped(a, b);
    EndSession(i, TradeEnd::DONE, 0, nullptr);
    return true;
}

TradeEnd CTradeSystem::Swap(TradeSession& t, int32_t& outWho)
{
    CPlayer* p[2] = { t.side[0].player, t.side[1].player };

    // 다시 판정 — 거리 · 올린 칸이 그대로인가(잠근 칸이라 같아야 한다 — 다르면 결함이라 로그 ERROR)
    if (!InReach(p[0], p[1]))
        return TradeEnd::NOT_IN_REACH;
    for (int s = 0; s < 2; ++s)
    {
        const CInventory& inv = p[s]->_inventory;
        bool same = inv.LockedMoney() == t.side[s].money && inv.Money() >= t.side[s].money;
        for (const TradeHalf::Offer& o : t.side[s].items)
        {
            const Item& it = inv.At(o.index);
            same = same && inv.IsLocked(o.index) && it.itemId == o.itemId && it.count >= o.count;
        }
        if (!same)
        {
            SLOG_ERROR("[TRADE] offer changed under lock (trade={}, account={}) — 잠근 칸·돈이 바뀌었다", t.id, p[s]->_accountId);
            outWho = p[s]->_actorId;
            return TradeEnd::CHANGED;
        }
    }

    // 복사본에 해 본다 — 풀기 → 내줄 것 빼기(칸 · 돈) → 받을 것 넣기(칸 · 돈). 원본은 둘 다 될 때만 바뀐다
    CInventory copy[2] = { p[0]->_inventory, p[1]->_inventory };
    for (int s = 0; s < 2; ++s)
    {
        copy[s].UnlockAll();
        for (const TradeHalf::Offer& o : t.side[s].items)
            copy[s].Remove(o.index, o.count);
        if (t.side[s].money > 0)
            copy[s].SpendMoney(t.side[s].money);
    }
    for (int s = 0; s < 2; ++s)
    {
        const TradeHalf& give = t.side[1 - s];   // s 가 받는 쪽
        for (const TradeHalf::Offer& o : give.items)
        {
            const ItemDef* def = _tables.Item().Find(o.itemId);
            if (def == nullptr)
            {
                outWho = give.player->_actorId;
                return TradeEnd::CHANGED;        // 그새 표에서 사라졌다(재시작 없이는 안 생긴다)
            }
            const InvAddResult added = copy[s].TryAdd(*def, o.count);
            if (added != InvAddResult::OK)
            {
                outWho = p[s]->_actorId;
                return TradeEnd::BAG_FULL;
            }
        }
        if (give.money > 0 && !copy[s].TryAddMoney(give.money))
        {
            outWho = p[s]->_actorId;
            return TradeEnd::MONEY_FULL;
        }
    }

    p[0]->_inventory = copy[0];   // 바뀐 칸·저장 표시·돈 바뀜이 복사본에 이미 켜져 있다(가방 [바뀐 칸])
    p[1]->_inventory = copy[1];
    return TradeEnd::DONE;
}

#endif // USE_TRADE
