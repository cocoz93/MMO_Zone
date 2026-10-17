#include "PartySystem.h"

#if USE_PARTY

#include <cassert>
#include "Player.h"        // _partyId · 상태 비교가 읽는 체력·레벨·존 번호
#include "MapManager.h"    // GetMapIdFromZoneId — 존 번호 → 맵 번호 풀기(정적 함수만)
#include "GameMonitor.h"   // 파티 계수기

CPartySystem::CPartySystem(CGameMonitor& gameMonitor, IPartyEvents& events)
    : _gameMonitor(gameMonitor)
    , _events(events)
{
}

// ── 조회 ──

Party* CPartySystem::PartyOf(const CPlayer* p)
{
    if (p->_partyId == 0)
        return nullptr;
    auto it = _parties.find(p->_partyId);
    // _partyId 를 쓰는 곳은 이 파일뿐이고 파티를 지울 때 파티원 전원을 0 으로 되돌린다 — 못 찾으면 그 약속이 깨진 것이다
    assert(it != _parties.end());
    return (it != _parties.end()) ? &it->second : nullptr;
}

bool CPartySystem::IsMember(const Party& party, const CPlayer* p)
{
    for (const Party::Member& m : party.members)
    {
        if (m.player == p)
            return true;
    }
    return false;
}

CPlayer* CPartySystem::FindMember(Party& party, int32_t actorId)
{
    for (Party::Member& m : party.members)
    {
        if (m.player->_actorId == actorId)
            return m.player;
    }
    return nullptr;
}

bool CPartySystem::HasReceived(const CPlayer* p) const
{
    for (const PendingRequest& r : _requests)
    {
        if (r.to == p)
            return true;
    }
    return false;
}

bool CPartySystem::HasSent(const CPlayer* p) const
{
    for (const PendingRequest& r : _requests)
    {
        if (r.origin == p)
            return true;
    }
    return false;
}

Party::Sent CPartySystem::SnapshotOf(const CPlayer* p)
{
    Party::Sent s;
    s.hp    = p->_hp;
    s.maxHp = p->_maxHp;
    s.level = p->_level;
    s.mapId = CMapManager::GetMapIdFromZoneId(p->_zoneId);
    return s;
}

// ── 보내기 대행 — 알림과 셈을 한 자리에서 ──

void CPartySystem::Notice(CPlayer* to, PartyNotice code, int32_t actorId)
{
    _gameMonitor._party._notices[static_cast<int>(code)].Inc();
    _events.OnPartyNotice(to, code, actorId);
}

void CPartySystem::Ask(CPlayer* to, PartyAskKind kind, CPlayer* origin, CPlayer* from, CPlayer* about, uint64_t frame)
{
    PendingRequest r;
    r.id          = _nextRequestId;
    r.kind        = kind;
    r.origin      = origin;
    r.from        = from;
    r.to          = to;
    r.about       = about;
    r.expireFrame = frame + ASK_TIMEOUT_FRAMES;
    _nextRequestId = (_nextRequestId == UINT32_MAX) ? 1 : _nextRequestId + 1;   // 0 은 안 쓴다
    _requests.push_back(r);

    _gameMonitor._party._requests[static_cast<int>(kind)].Inc();
    _gameMonitor._party._pending.Inc();
    _events.OnPartyAsk(to, r.id, kind, from, about, static_cast<uint32_t>(ASK_TIMEOUT_FRAMES * FRAME_INTERVAL_MS));
}

CPartySystem::PendingRequest CPartySystem::TakeRequest(size_t i, PartyRequestEnd why)
{
    const PendingRequest r = _requests[i];
    _requests.erase(_requests.begin() + static_cast<std::ptrdiff_t>(i));
    _gameMonitor._party._requestsEnded[static_cast<int>(why)].Inc();
    _gameMonitor._party._pending.Add(-1);
    return r;
}

void CPartySystem::CancelRequestsOf(CPlayer* changed, const CPlayer* skip)
{
    for (size_t i = 0; i < _requests.size(); )
    {
        const PendingRequest& r = _requests[i];
        if (r.origin != changed && r.from != changed && r.to != changed && r.about != changed)
        {
            ++i;
            continue;
        }
        const PendingRequest gone = TakeRequest(i, PartyRequestEnd::CANCELLED);   // i 는 그대로 — 뒤가 당겨진다

        // 받는 사람은 창을 닫는다 · 누른 사람과 만든 사람은 알림(같은 사람이면 한 번). 추천에서 대상(about)은 아직 모르는 사이라 안 알린다
        if (gone.to != skip)
            _events.OnPartyAskEnd(gone.to, gone.id, PartyNotice::CANCELLED);
        if (gone.origin != skip && gone.origin != gone.to)
            Notice(gone.origin, PartyNotice::CANCELLED, changed->_actorId);
        if (gone.from != gone.origin && gone.from != skip && gone.from != gone.to)
            Notice(gone.from, PartyNotice::CANCELLED, changed->_actorId);
    }
}

// ── 요청 ──

// 확인 순서 = 대상 없음 → 자기 자신 → 내가 보낸 것이 아직 → 같은 파티 → 둘 다 파티 → 꽉 참 → 받을 사람이 바쁨. 먼저 걸린 하나로 답한다.
void CPartySystem::Request(CPlayer* me, CPlayer* target, uint64_t frame)
{
    if (target == nullptr)
    {
        Notice(me, PartyNotice::NO_TARGET, 0);
        return;
    }
    if (target == me)
    {
        Notice(me, PartyNotice::SELF, me->_actorId);
        return;
    }
    if (HasSent(me))
    {
        Notice(me, PartyNotice::SELF_BUSY, 0);
        return;
    }

    Party* mine   = PartyOf(me);
    Party* theirs = PartyOf(target);
    if (mine != nullptr && mine == theirs)
    {
        Notice(me, PartyNotice::SAME_PARTY, target->_actorId);
        return;
    }
    if (mine != nullptr && theirs != nullptr)
    {
        Notice(me, PartyNotice::BOTH_IN_PARTY, target->_actorId);
        return;
    }

    // 두 사람의 처지로 셋 중 하나 — 남은 갈래: (없음·없음) · (있음·없음) · (없음·있음)
    PartyAskKind kind;
    PartyNotice  sent;
    CPlayer*     to    = target;
    CPlayer*     about = nullptr;
    if (mine == nullptr && theirs == nullptr)
    {
        kind = PartyAskKind::INVITE;   // 수락되면 me 가 파티장인 새 파티
        sent = PartyNotice::SENT_INVITE;
    }
    else if (mine != nullptr)
    {
        if (mine->members.size() >= static_cast<size_t>(PARTY_MAX_MEMBERS))
        {
            Notice(me, PartyNotice::FULL, target->_actorId);
            return;
        }
        if (mine->leader == me)
        {
            kind = PartyAskKind::INVITE;
            sent = PartyNotice::SENT_INVITE;
        }
        else
        {
            kind  = PartyAskKind::SUGGEST;   // 파티원이 부른 사람 — 파티장 승인이 먼저
            sent  = PartyNotice::SENT_SUGGEST;
            to    = mine->leader;
            about = target;
        }
    }
    else
    {
        if (theirs->members.size() >= static_cast<size_t>(PARTY_MAX_MEMBERS))
        {
            Notice(me, PartyNotice::FULL, target->_actorId);
            return;
        }
        kind = PartyAskKind::APPLY;   // 파티 밖 사람의 가입 신청 — 받는 사람은 그 파티의 파티장
        sent = PartyNotice::SENT_APPLY;
        to   = theirs->leader;
    }

    if (HasReceived(to))
    {
        Notice(me, PartyNotice::TARGET_BUSY, to->_actorId);
        return;
    }
    Ask(to, kind, me, me, about, frame);
    Notice(me, sent, to->_actorId);
}

void CPartySystem::Answer(CPlayer* me, uint32_t requestId, bool accept, uint64_t frame)
{
    size_t i = 0;
    while (i < _requests.size() && !(_requests[i].id == requestId && _requests[i].to == me))
        ++i;
    if (i == _requests.size())
    {
        Notice(me, PartyNotice::NO_REQUEST, 0);
        return;
    }
    const PendingRequest r = TakeRequest(i, accept ? PartyRequestEnd::ACCEPTED : PartyRequestEnd::DECLINED);

    if (!accept)
    {
        Notice(r.from, PartyNotice::DECLINED, me->_actorId);
        if (r.origin != r.from)
            Notice(r.origin, PartyNotice::DECLINED, me->_actorId);
        return;
    }

    // 처지가 바뀐 사람이 낀 요청은 이미 무효가 됐다(CancelRequestsOf) — 여기 온 요청은 만든 때의 처지 그대로다.
    //   인원만 다른 사람의 가입으로 바뀌므로 다시 본다. 아래 assert 는 그 약속(처지 그대로)을 지킨다.
    switch (r.kind)
    {
    case PartyAskKind::INVITE:
    {
        // me = 초대받은 사람(파티 없음) · from = 파티장 또는 파티 없는 사람
        assert(PartyOf(me) == nullptr);
        Party* party = PartyOf(r.from);
        if (party == nullptr)
        {
            Create(r.from, me);
            return;
        }
        assert(party->leader == r.from);
        if (party->members.size() >= static_cast<size_t>(PARTY_MAX_MEMBERS))
        {
            Notice(me, PartyNotice::FULL, r.from->_actorId);
            Notice(r.from, PartyNotice::FULL, me->_actorId);
            if (r.origin != r.from)
                Notice(r.origin, PartyNotice::FULL, me->_actorId);
            return;
        }
        Join(*party, me);
        return;
    }

    case PartyAskKind::APPLY:
    {
        // me = 파티장 · from = 신청한 사람(파티 없음) — 신청이 곧 본인 동의라 바로 들어간다
        Party* party = PartyOf(me);
        assert(party != nullptr && party->leader == me && PartyOf(r.from) == nullptr);
        if (party->members.size() >= static_cast<size_t>(PARTY_MAX_MEMBERS))
        {
            Notice(me, PartyNotice::FULL, r.from->_actorId);
            Notice(r.from, PartyNotice::FULL, me->_actorId);
            return;
        }
        Join(*party, r.from);
        return;
    }

    case PartyAskKind::SUGGEST:
    {
        // me = 파티장 · from = 추천한 파티원 · about = 초대하자는 사람(파티 없음) → about 에게 파티장 이름으로 초대가 간다(받는 사람 수락이 마지막)
        Party* party = PartyOf(me);
        assert(party != nullptr && party->leader == me && IsMember(*party, r.from) && PartyOf(r.about) == nullptr);
        if (party->members.size() >= static_cast<size_t>(PARTY_MAX_MEMBERS))
        {
            Notice(me, PartyNotice::FULL, r.about->_actorId);
            Notice(r.from, PartyNotice::FULL, r.about->_actorId);
            return;
        }
        if (HasReceived(r.about))
        {
            Notice(me, PartyNotice::TARGET_BUSY, r.about->_actorId);
            Notice(r.from, PartyNotice::TARGET_BUSY, r.about->_actorId);
            return;
        }
        Ask(r.about, PartyAskKind::INVITE, r.origin, me, r.from, frame);   // 초대 — about 칸에 추천한 파티원
        Notice(me, PartyNotice::SENT_INVITE, r.about->_actorId);
        Notice(r.from, PartyNotice::SENT_INVITE, r.about->_actorId);       // 추천한 사람에게 — Protocol.h SENT_INVITE 주석
        return;
    }

    case PartyAskKind::COUNT:
        break;   // 요청 표에 들어가지 않는다
    }
}

// ── 파티원 관리 ──

void CPartySystem::Leave(CPlayer* me)
{
    Party* party = PartyOf(me);
    if (party == nullptr)
    {
        Notice(me, PartyNotice::NOT_IN_PARTY, 0);
        return;
    }
    RemoveMember(*party, me, PartyLeave::LEAVE, true);
}

void CPartySystem::Kick(CPlayer* me, int32_t targetId)
{
    Party* party = PartyOf(me);
    if (party == nullptr)
    {
        Notice(me, PartyNotice::NOT_IN_PARTY, 0);
        return;
    }
    if (party->leader != me)
    {
        Notice(me, PartyNotice::NOT_LEADER, 0);
        return;
    }
    CPlayer* target = FindMember(*party, targetId);
    if (target == nullptr)
    {
        Notice(me, PartyNotice::NOT_MEMBER, targetId);
        return;
    }
    if (target == me)
    {
        Notice(me, PartyNotice::SELF, me->_actorId);   // 자기 자신은 내보내기가 아니라 나가기
        return;
    }
    RemoveMember(*party, target, PartyLeave::KICKED, true);
}

void CPartySystem::SetLeader(CPlayer* me, int32_t targetId)
{
    Party* party = PartyOf(me);
    if (party == nullptr)
    {
        Notice(me, PartyNotice::NOT_IN_PARTY, 0);
        return;
    }
    if (party->leader != me)
    {
        Notice(me, PartyNotice::NOT_LEADER, 0);
        return;
    }
    CPlayer* target = FindMember(*party, targetId);
    if (target == nullptr)
    {
        Notice(me, PartyNotice::NOT_MEMBER, targetId);
        return;
    }
    if (target == me)
    {
        Notice(me, PartyNotice::SELF, me->_actorId);
        return;
    }
    ChangeLeader(*party, target);
}

void CPartySystem::Disband(CPlayer* me)
{
    Party* party = PartyOf(me);
    if (party == nullptr)
    {
        Notice(me, PartyNotice::NOT_IN_PARTY, 0);
        return;
    }
    if (party->leader != me)
    {
        Notice(me, PartyNotice::NOT_LEADER, 0);
        return;
    }
    DisbandParty(*party);
}

void CPartySystem::Forget(CPlayer* player)
{
    CancelRequestsOf(player, player);
    if (Party* party = PartyOf(player))
        RemoveMember(*party, player, PartyLeave::DISCONNECTED, false);
}

// ── 바꾸기 한 곳들 ──

void CPartySystem::Create(CPlayer* leader, CPlayer* first)
{
    // 두 사람 다 처지가 바뀐다 — 낀 요청부터 무효
    CancelRequestsOf(leader, nullptr);
    CancelRequestsOf(first, nullptr);

    const int32_t id = _nextPartyId++;
    Party& party = _parties[id];
    party.id     = id;
    party.leader = leader;
    party.members.push_back({ leader, {} });
    party.members.push_back({ first, {} });
    leader->_partyId = id;
    first->_partyId  = id;

    _gameMonitor._party._created.Inc();
    _gameMonitor._party._active.Inc();
    _gameMonitor._party._memberAdded.Add(2);
    _gameMonitor._party._members.Add(2);

    BroadcastInfo(party);
    BroadcastNotice(party, PartyNotice::JOINED, first->_actorId);
}

void CPartySystem::Join(Party& party, CPlayer* p)
{
    CancelRequestsOf(p, nullptr);   // 들어오는 사람의 처지가 바뀐다

    party.members.push_back({ p, {} });
    p->_partyId = party.id;
    _gameMonitor._party._memberAdded.Inc();
    _gameMonitor._party._members.Inc();

    BroadcastInfo(party);
    BroadcastNotice(party, PartyNotice::JOINED, p->_actorId);
}

void CPartySystem::RemoveMember(Party& party, CPlayer* p, PartyLeave why, bool notifyRemoved)
{
    const bool wasLeader = (party.leader == p);
    CancelRequestsOf(p, notifyRemoved ? nullptr : p);

    for (size_t i = 0; i < party.members.size(); ++i)
    {
        if (party.members[i].player == p)
        {
            party.members.erase(party.members.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    p->_partyId = 0;
    _gameMonitor._party._memberRemoved[static_cast<int>(why)].Inc();
    _gameMonitor._party._members.Add(-1);

    const PartyNotice code = (why == PartyLeave::KICKED) ? PartyNotice::KICKED : PartyNotice::LEFT;   // 끊김도 LEFT
    if (notifyRemoved)
    {
        _events.OnPartyInfo(p, nullptr);
        Notice(p, code, p->_actorId);
    }

    // 순서 = 빼기 → 한 명 남으면 해산 → 아니면 파티장이었으면 넘기기 → INFO (MyDocs/claude-plans/party-plan.md 「함정」)
    if (party.members.size() <= 1)
    {
        DisbandParty(party);
        return;
    }
    if (wasLeader)
    {
        party.leader = party.members.front().player;   // 가입 순서 다음 사람
        CancelRequestsOf(party.leader, nullptr);       // 새 파티장의 처지가 바뀐다
    }
    BroadcastInfo(party);
    BroadcastNotice(party, code, p->_actorId);
    if (wasLeader)
        BroadcastNotice(party, PartyNotice::LEADER, party.leader->_actorId);
}

void CPartySystem::DisbandParty(Party& party)
{
    const int32_t id = party.id;
    // 전원 처지가 바뀐다 — 목록을 떠 두고(아래에서 파티를 지운다) 낀 요청부터 무효
    std::vector<CPlayer*> members;
    members.reserve(party.members.size());
    for (const Party::Member& m : party.members)
        members.push_back(m.player);

    for (CPlayer* m : members)
        CancelRequestsOf(m, nullptr);
    for (CPlayer* m : members)
    {
        m->_partyId = 0;
        _gameMonitor._party._memberRemoved[static_cast<int>(PartyLeave::DISBANDED)].Inc();
        _gameMonitor._party._members.Add(-1);
        _events.OnPartyInfo(m, nullptr);
        Notice(m, PartyNotice::DISBANDED, 0);
    }
    _gameMonitor._party._ended.Inc();
    _gameMonitor._party._active.Add(-1);
    _parties.erase(id);   // party 는 이 뒤 못 쓴다
}

void CPartySystem::ChangeLeader(Party& party, CPlayer* newLeader)
{
    CancelRequestsOf(party.leader, nullptr);   // 그만두는 파티장 · 새 파티장 둘 다 처지가 바뀐다
    CancelRequestsOf(newLeader, nullptr);
    party.leader = newLeader;
    BroadcastInfo(party);
    BroadcastNotice(party, PartyNotice::LEADER, newLeader->_actorId);
}

void CPartySystem::BroadcastInfo(Party& party)
{
    for (Party::Member& m : party.members)
        m.sent = SnapshotOf(m.player);
    for (const Party::Member& m : party.members)
        _events.OnPartyInfo(m.player, &party);
}

void CPartySystem::BroadcastNotice(const Party& party, PartyNotice code, int32_t actorId)
{
    for (const Party::Member& m : party.members)
        Notice(m.player, code, actorId);
}

// ── 틱 ──

void CPartySystem::Tick(uint64_t frame)
{
    if (!_requests.empty())
        Expire(frame);
    if (!_parties.empty() && frame - _lastScanFrame >= MEMBER_SCAN_FRAMES)
    {
        _lastScanFrame = frame;
        ScanMembers();
    }
}

void CPartySystem::Expire(uint64_t frame)
{
    for (size_t i = 0; i < _requests.size(); )
    {
        if (frame < _requests[i].expireFrame)
        {
            ++i;
            continue;
        }
        const PendingRequest r = TakeRequest(i, PartyRequestEnd::EXPIRED);   // i 는 그대로 — 뒤가 당겨진다
        _events.OnPartyAskEnd(r.to, r.id, PartyNotice::EXPIRED);
        Notice(r.origin, PartyNotice::EXPIRED, r.to->_actorId);
        if (r.from != r.origin)
            Notice(r.from, PartyNotice::EXPIRED, r.to->_actorId);
    }
}

void CPartySystem::ScanMembers()
{
    for (auto& entry : _parties)
    {
        Party& party = entry.second;
        for (Party::Member& m : party.members)
        {
            const Party::Sent now = SnapshotOf(m.player);
            if (now.hp == m.sent.hp && now.maxHp == m.sent.maxHp && now.level == m.sent.level && now.mapId == m.sent.mapId)
                continue;
            m.sent = now;
            for (const Party::Member& to : party.members)
                _events.OnPartyMember(to.player, m.player);
        }
    }
}

#endif // USE_PARTY
