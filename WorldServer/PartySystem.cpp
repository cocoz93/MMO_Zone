#include "PartySystem.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "WorldPresence.h"

static_assert(PARTY_MAX_MEMBERS == LINK_PARTY_MEMBERS_MAX, "파티 정원과 서버 간 명단 칸이 어긋났다");

namespace
{
    bool Same(const LinkPartyPerson& person, CharacterId id) { return person.characterId == ToInt(id); }
    CharacterId IdOf(const LinkPartyPerson& person) { return CharacterId{ person.characterId }; }
}

CPartySystem::CPartySystem(const CWorldPresence& presence, CWorldMetrics& metrics, IPartyEvents& events, int64_t restoreSettleMs)
    : _presence(presence), _metrics(metrics), _events(events), _restoreSettleMs(restoreSettleMs)
{
}

// ── 조회 ──

Party* CPartySystem::PartyOf(CharacterId id)
{
    const auto member = _memberOf.find(ToInt(id));
    if (member == _memberOf.end())
        return nullptr;
    const auto it = _parties.find(member->second);
    // _memberOf 를 쓰는 곳은 이 파일뿐이고 파티를 지울 때 파티원 전원을 뺀다 — 못 찾으면 그 약속이 깨진 것이다
    assert(it != _parties.end());
    return it != _parties.end() ? &it->second : nullptr;
}

const Party* CPartySystem::FindParty(CharacterId id) const
{
    const auto member = _memberOf.find(ToInt(id));
    if (member == _memberOf.end())
        return nullptr;
    const auto it = _parties.find(member->second);
    return it != _parties.end() ? &it->second : nullptr;
}

LinkPartyMemberRow* CPartySystem::FindRow(Party& party, CharacterId id)
{
    for (LinkPartyMemberRow& row : party.members)
    {
        if (Same(row.who, id))
            return &row;
    }
    return nullptr;
}

bool CPartySystem::IsOnline(const Party& party, CharacterId id)
{
    for (const LinkPartyMemberRow& row : party.members)
    {
        if (Same(row.who, id))
            return row.online != 0;
    }
    return false;
}

LinkPartyPerson CPartySystem::PersonOf(CharacterId id) const
{
    LinkPartyPerson person{};
    person.characterId = ToInt(id);
    if (const CWorldPresence::Person* found = _presence.Find(id))
    {
        person.displayChar = found->displayChar;
        person.colorIndex = found->colorIndex;
        person.level = found->level;
        std::memcpy(person.name, found->name.data(), std::min(found->name.size(), size_t{ LINK_NAME_CHARS }) * sizeof(char16_t));
        return person;
    }
    const auto member = _memberOf.find(ToInt(id));
    if (member != _memberOf.end())
    {
        const auto party = _parties.find(member->second);
        if (party != _parties.end())
        {
            for (const LinkPartyMemberRow& row : party->second.members)
            {
                if (Same(row.who, id))
                    return row.who;
            }
        }
    }
    return person;
}

bool CPartySystem::HasReceived(CharacterId id) const
{
    return std::any_of(_requests.begin(), _requests.end(), [id](const PendingRequest& r) { return Same(r.to, id); });
}

bool CPartySystem::HasSent(CharacterId id) const
{
    return std::any_of(_requests.begin(), _requests.end(), [id](const PendingRequest& r) { return Same(r.origin, id); });
}

int CPartySystem::OnlineMembers() const
{
    int count = 0;
    for (const auto& [id, party] : _parties)
    {
        for (const LinkPartyMemberRow& row : party.members)
            count += row.online != 0 ? 1 : 0;
    }
    return count;
}

// ── 보내기 대행 — 알림과 계산을 한 자리에서 ──

void CPartySystem::Notice(CharacterId to, PartyNotice code, const LinkPartyPerson& who)
{
    _metrics._party.notices[static_cast<int>(code)].fetch_add(1, std::memory_order_relaxed);
    _events.OnPartyNotice(to, code, who);
}

void CPartySystem::Ask(const LinkPartyPerson& to, PartyAskKind kind, const LinkPartyPerson& origin, const LinkPartyPerson& from, const LinkPartyPerson& about, int64_t nowMs)
{
    PendingRequest r;
    r.id       = _nextRequestId;
    r.kind     = kind;
    r.origin   = origin;
    r.from     = from;
    r.to       = to;
    r.about    = about;
    r.expireMs = nowMs + ASK_TIMEOUT_MS;
    _nextRequestId = (_nextRequestId == UINT32_MAX) ? 1 : _nextRequestId + 1;   // 0 은 안 쓴다
    _requests.push_back(r);

    _metrics._party.requests[static_cast<int>(kind)].fetch_add(1, std::memory_order_relaxed);
    _metrics._party.pending.fetch_add(1, std::memory_order_relaxed);
    _events.OnPartyAsk(IdOf(to), r.id, kind, from, about, static_cast<uint32_t>(ASK_TIMEOUT_MS));
}

CPartySystem::PendingRequest CPartySystem::TakeRequest(size_t i, PartyRequestEnd why)
{
    const PendingRequest r = _requests[i];
    _requests.erase(_requests.begin() + static_cast<std::ptrdiff_t>(i));
    _metrics._party.requestsEnded[static_cast<int>(why)].fetch_add(1, std::memory_order_relaxed);
    _metrics._party.pending.fetch_sub(1, std::memory_order_relaxed);
    return r;
}

void CPartySystem::CancelRequestsOf(const LinkPartyPerson& changed, CharacterId skip)
{
    const CharacterId id = IdOf(changed);
    for (size_t i = 0; i < _requests.size(); )
    {
        const PendingRequest& r = _requests[i];
        if (!Same(r.origin, id) && !Same(r.from, id) && !Same(r.to, id) && !Same(r.about, id))
        {
            ++i;
            continue;
        }
        const PendingRequest gone = TakeRequest(i, PartyRequestEnd::CANCELLED);   // i 는 그대로 — 뒤가 당겨진다

        // 받는 사람은 창을 닫는다 · 누른 사람과 만든 사람은 알림(같은 사람이면 한 번). 추천에서 대상(about)은 아직 모르는 사이라 안 알린다
        if (!Same(gone.to, skip))
            _events.OnPartyAskEnd(IdOf(gone.to), gone.id, PartyNotice::CANCELLED);
        if (!Same(gone.origin, skip) && gone.origin.characterId != gone.to.characterId)
            Notice(IdOf(gone.origin), PartyNotice::CANCELLED, changed);
        if (gone.from.characterId != gone.origin.characterId && !Same(gone.from, skip) && gone.from.characterId != gone.to.characterId)
            Notice(IdOf(gone.from), PartyNotice::CANCELLED, changed);
    }
}

bool CPartySystem::EnsureLeader(Party& party, CharacterId me)
{
    if (party.leader == me)
        return true;
    if (IsOnline(party, party.leader))
        return false;
    ChangeLeader(party, me);   // 파티장이 오프라인이면 파티장의 일을 시도한 파티원이 맡는다
    return true;
}

// ── 요청 ──

// 확인 순서 = 대상 없음 → 자기 자신 → 내가 보낸 것이 아직 → 같은 파티 → 둘 다 파티 → 꽉 참 → 받을 사람이 바쁨. 먼저 걸린 하나로 답한다.
void CPartySystem::Request(CharacterId me, CharacterId target, int64_t nowMs)
{
    const LinkPartyPerson none{};
    if (IsRestoring(me))
    {
        Notice(me, PartyNotice::NO_WORLD, none);
        return;
    }
    if (target == NO_CHARACTER || _presence.Find(target) == nullptr)
    {
        Notice(me, PartyNotice::NO_TARGET, none);
        return;
    }
    const LinkPartyPerson mePerson = PersonOf(me);
    const LinkPartyPerson targetPerson = PersonOf(target);
    if (target == me)
    {
        Notice(me, PartyNotice::SELF, mePerson);
        return;
    }
    if (HasSent(me))
    {
        Notice(me, PartyNotice::SELF_BUSY, none);
        return;
    }
    if (IsRestoring(target))
    {
        Notice(me, PartyNotice::TARGET_BUSY, targetPerson);
        return;
    }

    Party* mine   = PartyOf(me);
    Party* theirs = PartyOf(target);
    if (mine != nullptr && mine == theirs)
    {
        Notice(me, PartyNotice::SAME_PARTY, targetPerson);
        return;
    }
    if (mine != nullptr && theirs != nullptr)
    {
        Notice(me, PartyNotice::BOTH_IN_PARTY, targetPerson);
        return;
    }

    // 두 사람의 처지로 셋 중 하나 — 남은 경우: (없음·없음) · (있음·없음) · (없음·있음)
    PartyAskKind    kind;
    PartyNotice     sent;
    LinkPartyPerson to = targetPerson;
    LinkPartyPerson about{};
    if (mine == nullptr && theirs == nullptr)
    {
        kind = PartyAskKind::INVITE;   // 수락되면 me 가 파티장인 새 파티
        sent = PartyNotice::SENT_INVITE;
    }
    else if (mine != nullptr)
    {
        if (mine->members.size() >= static_cast<size_t>(PARTY_MAX_MEMBERS))
        {
            Notice(me, PartyNotice::FULL, targetPerson);
            return;
        }
        if (EnsureLeader(*mine, me))
        {
            kind = PartyAskKind::INVITE;
            sent = PartyNotice::SENT_INVITE;
        }
        else
        {
            kind  = PartyAskKind::SUGGEST;   // 파티원이 부른 사람 — 파티장 승인이 먼저
            sent  = PartyNotice::SENT_SUGGEST;
            to    = PersonOf(mine->leader);
            about = targetPerson;
        }
    }
    else
    {
        if (theirs->members.size() >= static_cast<size_t>(PARTY_MAX_MEMBERS))
        {
            Notice(me, PartyNotice::FULL, targetPerson);
            return;
        }
        if (!IsOnline(*theirs, theirs->leader))
        {
            Notice(me, PartyNotice::TARGET_BUSY, PersonOf(theirs->leader));   // 답할 파티장이 오프라인이다
            return;
        }
        kind = PartyAskKind::APPLY;   // 파티 밖 사람의 가입 신청 — 받는 사람은 그 파티의 파티장
        sent = PartyNotice::SENT_APPLY;
        to   = PersonOf(theirs->leader);
    }

    if (HasReceived(IdOf(to)))
    {
        Notice(me, PartyNotice::TARGET_BUSY, to);
        return;
    }
    Ask(to, kind, mePerson, mePerson, about, nowMs);
    Notice(me, sent, to);
}

void CPartySystem::Answer(CharacterId me, uint32_t requestId, bool accept, int64_t nowMs)
{
    const LinkPartyPerson none{};
    size_t i = 0;
    while (i < _requests.size() && !(_requests[i].id == requestId && Same(_requests[i].to, me)))
        ++i;
    if (i == _requests.size())
    {
        Notice(me, PartyNotice::NO_REQUEST, none);
        return;
    }
    const PendingRequest r = TakeRequest(i, accept ? PartyRequestEnd::ACCEPTED : PartyRequestEnd::DECLINED);
    const LinkPartyPerson mePerson = PersonOf(me);
    const CharacterId from = IdOf(r.from);
    const CharacterId origin = IdOf(r.origin);

    if (!accept)
    {
        Notice(from, PartyNotice::DECLINED, mePerson);
        if (origin != from)
            Notice(origin, PartyNotice::DECLINED, mePerson);
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
        Party* party = PartyOf(from);
        if (party == nullptr)
        {
            Create(from, me);
            return;
        }
        assert(party->leader == from);
        if (party->members.size() >= static_cast<size_t>(PARTY_MAX_MEMBERS))
        {
            Notice(me, PartyNotice::FULL, r.from);
            Notice(from, PartyNotice::FULL, mePerson);
            if (origin != from)
                Notice(origin, PartyNotice::FULL, mePerson);
            return;
        }
        Join(*party, me);
        return;
    }

    case PartyAskKind::APPLY:
    {
        // me = 파티장 · from = 신청한 사람(파티 없음) — 신청이 곧 본인 동의라 바로 들어간다
        Party* party = PartyOf(me);
        assert(party != nullptr && party->leader == me && PartyOf(from) == nullptr);
        if (party->members.size() >= static_cast<size_t>(PARTY_MAX_MEMBERS))
        {
            Notice(me, PartyNotice::FULL, r.from);
            Notice(from, PartyNotice::FULL, mePerson);
            return;
        }
        Join(*party, from);
        return;
    }

    case PartyAskKind::SUGGEST:
    {
        // me = 파티장 · from = 추천한 파티원 · about = 초대하자는 사람(파티 없음) → about 에게 파티장 이름으로 초대가 간다(받는 사람 수락이 마지막)
        Party* party = PartyOf(me);
        const CharacterId about = IdOf(r.about);
        assert(party != nullptr && party->leader == me && PartyOf(about) == nullptr);
        if (party->members.size() >= static_cast<size_t>(PARTY_MAX_MEMBERS))
        {
            Notice(me, PartyNotice::FULL, r.about);
            Notice(from, PartyNotice::FULL, r.about);
            return;
        }
        if (HasReceived(about))
        {
            Notice(me, PartyNotice::TARGET_BUSY, r.about);
            Notice(from, PartyNotice::TARGET_BUSY, r.about);
            return;
        }
        Ask(r.about, PartyAskKind::INVITE, r.origin, mePerson, r.from, nowMs);   // 초대 — about 칸에 추천한 파티원
        Notice(me, PartyNotice::SENT_INVITE, r.about);
        Notice(from, PartyNotice::SENT_INVITE, r.about);                          // 추천한 사람에게 — Protocol.h SENT_INVITE 주석
        return;
    }

    case PartyAskKind::COUNT:
        break;   // 요청 표에 들어가지 않는다
    }
}

// ── 파티원 관리 ──

void CPartySystem::Leave(CharacterId me)
{
    const LinkPartyPerson none{};
    if (IsRestoring(me))
    {
        Notice(me, PartyNotice::NO_WORLD, none);
        return;
    }
    Party* party = PartyOf(me);
    if (party == nullptr)
    {
        Notice(me, PartyNotice::NOT_IN_PARTY, none);
        return;
    }
    RemoveMember(*party, me, PartyLeave::LEAVE);
}

void CPartySystem::Kick(CharacterId me, CharacterId target)
{
    const LinkPartyPerson none{};
    if (IsRestoring(me))
    {
        Notice(me, PartyNotice::NO_WORLD, none);
        return;
    }
    Party* party = PartyOf(me);
    if (party == nullptr)
    {
        Notice(me, PartyNotice::NOT_IN_PARTY, none);
        return;
    }
    if (!EnsureLeader(*party, me))
    {
        Notice(me, PartyNotice::NOT_LEADER, none);
        return;
    }
    if (FindRow(*party, target) == nullptr)
    {
        Notice(me, PartyNotice::NOT_MEMBER, PersonOf(target));
        return;
    }
    if (target == me)
    {
        Notice(me, PartyNotice::SELF, PersonOf(me));   // 자기 자신은 내보내기가 아니라 나가기
        return;
    }
    RemoveMember(*party, target, PartyLeave::KICKED);
}

void CPartySystem::SetLeader(CharacterId me, CharacterId target)
{
    const LinkPartyPerson none{};
    if (IsRestoring(me))
    {
        Notice(me, PartyNotice::NO_WORLD, none);
        return;
    }
    Party* party = PartyOf(me);
    if (party == nullptr)
    {
        Notice(me, PartyNotice::NOT_IN_PARTY, none);
        return;
    }
    if (!EnsureLeader(*party, me))
    {
        Notice(me, PartyNotice::NOT_LEADER, none);
        return;
    }
    if (FindRow(*party, target) == nullptr)
    {
        Notice(me, PartyNotice::NOT_MEMBER, PersonOf(target));
        return;
    }
    if (target == me)
    {
        Notice(me, PartyNotice::SELF, PersonOf(me));
        return;
    }
    if (!IsOnline(*party, target))
    {
        Notice(me, PartyNotice::TARGET_BUSY, PersonOf(target));   // 오프라인인 파티원에게는 못 넘긴다
        return;
    }
    ChangeLeader(*party, target);
}

void CPartySystem::Disband(CharacterId me)
{
    const LinkPartyPerson none{};
    if (IsRestoring(me))
    {
        Notice(me, PartyNotice::NO_WORLD, none);
        return;
    }
    Party* party = PartyOf(me);
    if (party == nullptr)
    {
        Notice(me, PartyNotice::NOT_IN_PARTY, none);
        return;
    }
    if (!EnsureLeader(*party, me))
    {
        Notice(me, PartyNotice::NOT_LEADER, none);
        return;
    }
    DisbandParty(*party);
}

void CPartySystem::TakeLeader(CharacterId me)
{
    Party* party = PartyOf(me);
    if (party == nullptr || party->leader == me || IsOnline(*party, party->leader))
        return;   // 판정과 이 요청 사이에 파티장이 돌아왔다 — 다음 포털 판정이 맞게 본다
    ChangeLeader(*party, me);
}

// ── 접속 ──

void CPartySystem::Online(CharacterId id)
{
    Party* party = PartyOf(id);
    if (party == nullptr)
        return;
    LinkPartyMemberRow* row = FindRow(*party, id);
    assert(row != nullptr);
    row->who = PersonOf(id);
    row->online = 1;
    _events.OnPartyInfo(*party);
}

void CPartySystem::Offline(CharacterId id)
{
    CancelRequestsOf(PersonOf(id), id);

    Party* party = PartyOf(id);
    if (party == nullptr)
        return;
    LinkPartyMemberRow* row = FindRow(*party, id);
    assert(row != nullptr);
    row->online = 0;
    row->visibleChannel = 0;

    const bool anyoneOnline = std::any_of(party->members.begin(), party->members.end(),
                                          [](const LinkPartyMemberRow& r) { return r.online != 0; });
    if (!anyoneOnline)
    {
        EraseQuietly(*party);
        return;
    }
    _events.OnPartyInfo(*party);
}

void CPartySystem::ApplyState(const LinkPartyStateRow& state)
{
    const CharacterId id{ state.characterId };
    Party* party = PartyOf(id);
    if (party == nullptr)
        return;
    LinkPartyMemberRow* row = FindRow(*party, id);
    assert(row != nullptr);
    row->hp = state.hp;
    row->maxHp = state.maxHp;
    row->who.level = state.level;
    row->mapId = state.mapId;
    row->visibleChannel = state.visibleChannel;
    _events.OnPartyMember(*party, *row);
}

void CPartySystem::Restore(const LinkPartyRestoreRow& row, int64_t nowMs)
{
    const CharacterId id{ row.state.characterId };
    if (_memberOf.count(ToInt(id)) != 0)
    {
        ApplyState(row.state);   // 월드 서버가 이미 안다 — 온라인으로 돌리는 일은 3004 가 했다
        return;
    }
    if (_parties.count(row.partyId) != 0)
    {
        _events.OnPartyNone(id);   // 파티는 있는데 이 사람이 없다 — 끊긴 사이 내보내졌다. 정하는 쪽은 월드 서버다
        return;
    }

    Restoring& restoring = _restoring[row.partyId];
    const auto same = std::find_if(restoring.rows.begin(), restoring.rows.end(),
                                   [&row](const LinkPartyRestoreRow& r) { return r.state.characterId == row.state.characterId; });
    if (same != restoring.rows.end())
        *same = row;
    else
        restoring.rows.push_back(row);
    restoring.settleAtMs = nowMs + _restoreSettleMs;
    _restoringPeople.insert(ToInt(id));
    _nextPartyId = std::max(_nextPartyId, row.partyId + 1);   // 새 파티 번호는 모은 것 중 가장 큰 번호 다음부터
}

void CPartySystem::SettleRestores(int64_t nowMs)
{
    for (auto it = _restoring.begin(); it != _restoring.end(); )
    {
        if (nowMs < it->second.settleAtMs)
        {
            ++it;
            continue;
        }
        const int32_t partyId = it->first;
        for (const LinkPartyRestoreRow& row : it->second.rows)
            _restoringPeople.erase(row.state.characterId);

        const std::vector<LinkPartyRestoreRow> all = std::move(it->second.rows);
        it = _restoring.erase(it);

        const RestoreDecision decision = DecideRestore(all, [this](CharacterId id) { return _presence.Find(id) != nullptr; });
        if (decision.disband)
        {
            const LinkPartyPerson none{};
            for (const LinkPartyRestoreRow& row : decision.rows)
            {
                const CharacterId id{ row.state.characterId };
                _events.OnPartyNone(id);
                Notice(id, PartyNotice::DISBANDED, none);
            }
            continue;
        }

        Party& party = _parties[partyId];
        party.id = partyId;
        party.leader = CharacterId{ decision.rows[static_cast<size_t>(decision.leaderIndex)].state.characterId };
        for (const LinkPartyRestoreRow& restored : decision.rows)
        {
            const CharacterId id{ restored.state.characterId };
            LinkPartyMemberRow row = NewRow(id);
            row.hp = restored.state.hp;
            row.maxHp = restored.state.maxHp;
            row.who.level = restored.state.level;
            row.mapId = restored.state.mapId;
            row.visibleChannel = restored.state.visibleChannel;
            party.members.push_back(row);
            _memberOf[ToInt(id)] = partyId;
        }
        _metrics._party.created.fetch_add(1, std::memory_order_relaxed);
        _metrics._party.active.fetch_add(1, std::memory_order_relaxed);
        _metrics._party.memberAdded.fetch_add(static_cast<int64_t>(party.members.size()), std::memory_order_relaxed);
        _metrics._party.members.fetch_add(static_cast<int64_t>(party.members.size()), std::memory_order_relaxed);
        _events.OnPartyInfo(party);
    }
}

// ── 바꾸기 한 곳들 ──

LinkPartyMemberRow CPartySystem::NewRow(CharacterId id) const
{
    LinkPartyMemberRow row{};
    row.who = PersonOf(id);
    row.online = 1;   // 파티에 드는 사람은 요청에 답했거나 요청을 낸 사람이라 접속 중이다
    return row;
}

void CPartySystem::Create(CharacterId leader, CharacterId first)
{
    // 두 사람 다 처지가 바뀐다 — 낀 요청부터 무효
    const LinkPartyPerson leaderPerson = PersonOf(leader);
    const LinkPartyPerson firstPerson = PersonOf(first);
    CancelRequestsOf(leaderPerson, NO_CHARACTER);
    CancelRequestsOf(firstPerson, NO_CHARACTER);

    const int32_t id = _nextPartyId++;
    Party& party = _parties[id];
    party.id     = id;
    party.leader = leader;
    party.members.push_back(NewRow(leader));
    party.members.push_back(NewRow(first));
    _memberOf[ToInt(leader)] = id;
    _memberOf[ToInt(first)]  = id;

    _metrics._party.created.fetch_add(1, std::memory_order_relaxed);
    _metrics._party.active.fetch_add(1, std::memory_order_relaxed);
    _metrics._party.memberAdded.fetch_add(2, std::memory_order_relaxed);
    _metrics._party.members.fetch_add(2, std::memory_order_relaxed);

    _events.OnPartyInfo(party);
    BroadcastNotice(party, PartyNotice::JOINED, firstPerson);
}

void CPartySystem::Join(Party& party, CharacterId id)
{
    const LinkPartyPerson person = PersonOf(id);
    CancelRequestsOf(person, NO_CHARACTER);   // 들어오는 사람의 처지가 바뀐다

    party.members.push_back(NewRow(id));
    _memberOf[ToInt(id)] = party.id;
    _metrics._party.memberAdded.fetch_add(1, std::memory_order_relaxed);
    _metrics._party.members.fetch_add(1, std::memory_order_relaxed);

    _events.OnPartyInfo(party);
    BroadcastNotice(party, PartyNotice::JOINED, person);
}

void CPartySystem::RemoveMember(Party& party, CharacterId id, PartyLeave why)
{
    const bool wasLeader = (party.leader == id);
    const LinkPartyPerson person = PersonOf(id);
    CancelRequestsOf(person, NO_CHARACTER);

    for (size_t i = 0; i < party.members.size(); ++i)
    {
        if (Same(party.members[i].who, id))
        {
            party.members.erase(party.members.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    _memberOf.erase(ToInt(id));
    _metrics._party.memberRemoved[static_cast<int>(why)].fetch_add(1, std::memory_order_relaxed);
    _metrics._party.members.fetch_sub(1, std::memory_order_relaxed);

    const PartyNotice code = (why == PartyLeave::KICKED) ? PartyNotice::KICKED : PartyNotice::LEFT;
    _events.OnPartyNone(id);   // 빠진 사람이 오프라인이면 받을 존 서버가 없어 그냥 사라진다
    Notice(id, code, person);

    // 순서 = 빼기 → 한 명 남으면 해산 → 아무도 접속 중이 아니면 지우기 → 아니면 파티장이었으면 넘기기 → INFO
    if (party.members.size() <= 1)
    {
        DisbandParty(party);
        return;
    }
    if (std::none_of(party.members.begin(), party.members.end(), [](const LinkPartyMemberRow& r) { return r.online != 0; }))
    {
        EraseQuietly(party);
        return;
    }
    if (wasLeader)
    {
        party.leader = IdOf(party.members.front().who);   // 가입 순서 다음 사람
        CancelRequestsOf(party.members.front().who, NO_CHARACTER);   // 새 파티장의 처지가 바뀐다
    }
    _events.OnPartyInfo(party);
    BroadcastNotice(party, code, person);
    if (wasLeader)
        BroadcastNotice(party, PartyNotice::LEADER, party.members.front().who);
}

void CPartySystem::DisbandParty(Party& party)
{
    const int32_t id = party.id;
    // 전원 처지가 바뀐다 — 목록을 떠 두고(아래에서 파티를 지운다) 낀 요청부터 무효
    const std::vector<LinkPartyMemberRow> members = party.members;
    const LinkPartyPerson none{};

    for (const LinkPartyMemberRow& m : members)
        CancelRequestsOf(m.who, NO_CHARACTER);
    for (const LinkPartyMemberRow& m : members)
    {
        const CharacterId member = IdOf(m.who);
        _memberOf.erase(ToInt(member));
        _metrics._party.memberRemoved[static_cast<int>(PartyLeave::DISBANDED)].fetch_add(1, std::memory_order_relaxed);
        _metrics._party.members.fetch_sub(1, std::memory_order_relaxed);
        _events.OnPartyNone(member);
        Notice(member, PartyNotice::DISBANDED, none);
    }
    _metrics._party.ended.fetch_add(1, std::memory_order_relaxed);
    _metrics._party.active.fetch_sub(1, std::memory_order_relaxed);
    _parties.erase(id);   // party 는 이 뒤 못 쓴다
}

void CPartySystem::EraseQuietly(Party& party)
{
    const int32_t id = party.id;
    for (const LinkPartyMemberRow& m : party.members)
    {
        _memberOf.erase(m.who.characterId);
        _metrics._party.memberRemoved[static_cast<int>(PartyLeave::DISBANDED)].fetch_add(1, std::memory_order_relaxed);
        _metrics._party.members.fetch_sub(1, std::memory_order_relaxed);
    }
    _metrics._party.ended.fetch_add(1, std::memory_order_relaxed);
    _metrics._party.active.fetch_sub(1, std::memory_order_relaxed);
    _parties.erase(id);   // party 는 이 뒤 못 쓴다
}

void CPartySystem::ChangeLeader(Party& party, CharacterId newLeader)
{
    const LinkPartyPerson leaderPerson = PersonOf(newLeader);
    CancelRequestsOf(PersonOf(party.leader), NO_CHARACTER);   // 그만두는 파티장 · 새 파티장 둘 다 처지가 바뀐다
    CancelRequestsOf(leaderPerson, NO_CHARACTER);
    party.leader = newLeader;
    _events.OnPartyInfo(party);
    BroadcastNotice(party, PartyNotice::LEADER, leaderPerson);
}

void CPartySystem::BroadcastNotice(const Party& party, PartyNotice code, const LinkPartyPerson& who)
{
    for (const LinkPartyMemberRow& m : party.members)
    {
        if (m.online != 0)
            Notice(IdOf(m.who), code, who);
    }
}

// ── 틱 ──

void CPartySystem::Tick(int64_t nowMs)
{
    if (!_requests.empty())
        Expire(nowMs);
    if (!_restoring.empty())
        SettleRestores(nowMs);
}

void CPartySystem::Expire(int64_t nowMs)
{
    for (size_t i = 0; i < _requests.size(); )
    {
        if (nowMs < _requests[i].expireMs)
        {
            ++i;
            continue;
        }
        const PendingRequest r = TakeRequest(i, PartyRequestEnd::EXPIRED);   // i 는 그대로 — 뒤가 당겨진다
        _events.OnPartyAskEnd(IdOf(r.to), r.id, PartyNotice::EXPIRED);
        Notice(IdOf(r.origin), PartyNotice::EXPIRED, r.to);
        if (r.from.characterId != r.origin.characterId)
            Notice(IdOf(r.from), PartyNotice::EXPIRED, r.to);
    }
}
