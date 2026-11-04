#include "ZoneDirectory.h"

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

void CZoneDirectory::Enter(SessionId session, CharacterId characterId)
{
    const auto it = _zones.find(ToInt(session));
    if (it == _zones.end())
        return;
    it->second.online.insert(ToInt(characterId));
    EraseEntering(ToInt(characterId));
}

void CZoneDirectory::Leave(SessionId session, CharacterId characterId)
{
    const auto it = _zones.find(ToInt(session));
    if (it != _zones.end())
        it->second.online.erase(ToInt(characterId));
}

const CZoneDirectory::Zone* CZoneDirectory::PickZone() const
{
    const Zone* best = nullptr;
    for (const auto& entry : _zones)
    {
        const Zone& zone = entry.second.zone;
        if (best == nullptr || ToInt(zone.id) < ToInt(best->id))
            best = &zone;
    }
    return best;
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
