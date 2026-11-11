#include "ZoneDirectory.h"

#include <algorithm>

SessionId CZoneDirectory::Register(SessionId session, ZoneServerId id, int maxClients, const std::string& wsAddr)
{
    SessionId previous = NO_SESSION;
    for (auto it = _zones.begin(); it != _zones.end(); ++it)
    {
        if (it->second.zone.id == id && it->second.zone.session != session)
        {
            previous = it->second.zone.session;
            break;
        }
    }
    if (previous != NO_SESSION)
        OnDisconnected(previous);

    ZoneState& state = _zones[ToInt(session)];
    state.zone = Zone{ session, id, maxClients, wsAddr };
    state.online.clear();   // 뒤따르는 명단으로 다시 채운다
    state.accounts.clear();
    return previous;
}

bool CZoneDirectory::OnDisconnected(SessionId session)
{
    const auto it = _zones.find(ToInt(session));
    if (it == _zones.end())
        return false;
    _zones.erase(it);

    for (auto entry = _entering.begin(); entry != _entering.end(); )
    {
        if (entry->second.zoneSession == ToInt(session))
        {
            _enteringByAccount.erase(entry->second.accountId);
            entry = _entering.erase(entry);
        }
        else
        {
            ++entry;
        }
    }
    return true;
}

bool CZoneDirectory::IsRegistered(SessionId session) const
{
    return _zones.find(ToInt(session)) != _zones.end();
}

void CZoneDirectory::Enter(SessionId session, CharacterId characterId, AccountId accountId)
{
    const auto it = _zones.find(ToInt(session));
    if (it == _zones.end())
        return;
    it->second.online[ToInt(characterId)] = ToInt(accountId);
    if (accountId != NO_ACCOUNT)
        it->second.accounts[ToInt(accountId)] = ToInt(characterId);
    EraseEntering(ToInt(characterId));
}

void CZoneDirectory::Leave(SessionId session, CharacterId characterId)
{
    const auto it = _zones.find(ToInt(session));
    if (it == _zones.end())
        return;
    ZoneState& state = it->second;
    const auto row = state.online.find(ToInt(characterId));
    if (row == state.online.end())
        return;

    // 계정 표의 줄이 이 캐릭터를 가리킬 때만 지운다 — 같은 계정의 다음 캐릭터가 이미 적혔을 수 있다
    const auto account = state.accounts.find(row->second);
    if (account != state.accounts.end() && account->second == ToInt(characterId))
        state.accounts.erase(account);
    state.online.erase(row);
}

const CZoneDirectory::Zone* CZoneDirectory::FindById(ZoneServerId id) const
{
    for (const auto& entry : _zones)
    {
        if (entry.second.zone.id == id)
            return &entry.second.zone;
    }
    return nullptr;
}

std::vector<CZoneDirectory::Zone> CZoneDirectory::Candidates(ZoneServerId owner) const
{
    std::vector<Zone> ordered;
    std::vector<Zone> rest;
    for (const auto& entry : _zones)
    {
        const Zone& zone = entry.second.zone;
        if (zone.id == owner)
            ordered.push_back(zone);
        else
            rest.push_back(zone);
    }
    std::sort(rest.begin(), rest.end(), [](const Zone& a, const Zone& b) { return ToInt(a.id) < ToInt(b.id); });

    const auto start = std::upper_bound(rest.begin(), rest.end(), _lastRotated, [](int id, const Zone& zone) { return id < ToInt(zone.id); });
    ordered.insert(ordered.end(), start, rest.end());
    ordered.insert(ordered.end(), rest.begin(), start);
    return ordered;
}

std::vector<SessionId> CZoneDirectory::FindAccountZones(AccountId accountId, int64_t nowMs) const
{
    std::vector<SessionId> found;
    for (const auto& entry : _zones)
    {
        if (entry.second.accounts.count(ToInt(accountId)) != 0)
            found.push_back(entry.second.zone.session);
    }

    const auto waiting = _enteringByAccount.find(ToInt(accountId));
    if (waiting != _enteringByAccount.end())
    {
        const auto it = _entering.find(waiting->second);
        if (it != _entering.end() && nowMs < it->second.expireMs)
        {
            const SessionId session{ it->second.zoneSession };
            if (std::find(found.begin(), found.end(), session) == found.end())
                found.push_back(session);
        }
    }
    return found;
}

void CZoneDirectory::EraseEnteringAccount(AccountId accountId)
{
    const auto waiting = _enteringByAccount.find(ToInt(accountId));
    if (waiting != _enteringByAccount.end())
        EraseEntering(waiting->second);
}

void CZoneDirectory::EraseEntering(int64_t characterId)
{
    const auto it = _entering.find(characterId);
    if (it == _entering.end())
        return;
    _enteringByAccount.erase(it->second.accountId);
    _entering.erase(it);
}

void CZoneDirectory::AddEntering(SessionId zoneSession, CharacterId characterId, AccountId accountId, int64_t expireMs)
{
    const auto previous = _enteringByAccount.find(ToInt(accountId));
    if (previous != _enteringByAccount.end())
        EraseEntering(previous->second);

    EraseEntering(ToInt(characterId));
    _entering[ToInt(characterId)] = Entering{ ToInt(zoneSession), ToInt(accountId), expireMs };
    _enteringByAccount[ToInt(accountId)] = ToInt(characterId);
}

bool CZoneDirectory::IsInGame(CharacterId characterId, int64_t nowMs) const
{
    for (const auto& entry : _zones)
    {
        if (entry.second.online.count(ToInt(characterId)) != 0)
            return true;
    }
    const auto it = _entering.find(ToInt(characterId));
    return it != _entering.end() && nowMs < it->second.expireMs;
}

void CZoneDirectory::Sweep(int64_t nowMs)
{
    for (auto entry = _entering.begin(); entry != _entering.end(); )
    {
        if (nowMs >= entry->second.expireMs)
        {
            _enteringByAccount.erase(entry->second.accountId);
            entry = _entering.erase(entry);
        }
        else
        {
            ++entry;
        }
    }
}

int CZoneDirectory::OnlineCount() const
{
    int count = 0;
    for (const auto& entry : _zones)
        count += static_cast<int>(entry.second.online.size());
    return count;
}
