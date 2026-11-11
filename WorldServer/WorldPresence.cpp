#include "WorldPresence.h"

SessionId CWorldPresence::Register(SessionId session, ZoneServerId id, int maxClients, const std::string& wsAddr, std::vector<CharacterId>* removed)
{
    SessionId previous = NO_SESSION;
    for (const auto& [key, zone] : _zones)
    {
        if (zone.id == id && zone.session != session)
        {
            previous = zone.session;
            break;
        }
    }
    if (previous != NO_SESSION)
        OnDisconnected(previous, removed);

    _zones[ToInt(session)] = Zone{ session, id, maxClients, wsAddr };
    return previous;
}

bool CWorldPresence::OnDisconnected(SessionId session, std::vector<CharacterId>* removed)
{
    if (_zones.erase(ToInt(session)) == 0)
        return false;

    for (auto it = _people.begin(); it != _people.end(); )
    {
        const auto current = it++;
        if (current->second.zoneSession != session)
            continue;
        if (removed != nullptr)
            removed->push_back(current->second.characterId);
        ErasePerson(current);
    }
    return true;
}

bool CWorldPresence::Enter(SessionId session, const LinkOnlineEntry& entry)
{
    const auto zone = _zones.find(ToInt(session));
    if (zone == _zones.end())
        return false;

    const auto previous = _people.find(entry.characterId);
    if (previous != _people.end())
        ErasePerson(previous);

    Person person;
    person.zoneSession = session;
    person.zoneId = zone->second.id;
    person.characterId = CharacterId{ entry.characterId };
    person.accountId = AccountId{ entry.accountId };
    person.level = entry.level;
    person.job = entry.job;
    person.displayChar = entry.displayChar;
    person.colorIndex = entry.colorIndex;
    person.name = NameOf(entry.name);

    if (!person.name.empty())
        _byName[person.name] = entry.characterId;
    _people[entry.characterId] = std::move(person);
    return true;
}

bool CWorldPresence::Leave(SessionId session, CharacterId characterId)
{
    const auto it = _people.find(ToInt(characterId));
    if (it == _people.end() || it->second.zoneSession != session)
        return false;
    ErasePerson(it);
    return true;
}

void CWorldPresence::SetLevel(CharacterId characterId, int32_t level)
{
    const auto it = _people.find(ToInt(characterId));
    if (it != _people.end())
        it->second.level = level;
}

const CWorldPresence::Person* CWorldPresence::Find(CharacterId characterId) const
{
    const auto it = _people.find(ToInt(characterId));
    return it == _people.end() ? nullptr : &it->second;
}

const CWorldPresence::Person* CWorldPresence::FindByName(const std::u16string& name) const
{
    const auto it = _byName.find(name);
    return it == _byName.end() ? nullptr : Find(CharacterId{ it->second });
}

std::u16string CWorldPresence::NameOf(const char16_t (&name)[LINK_NAME_CHARS])
{
    size_t length = 0;
    while (length < LINK_NAME_CHARS && name[length] != 0)
        ++length;
    return std::u16string(name, length);
}

void CWorldPresence::ErasePerson(std::unordered_map<int64_t, Person>::iterator it)
{
    // 이름 표의 줄이 이 사람을 가리킬 때만 지운다 — 같은 이름을 다른 번호가 덮어썼으면 그쪽 줄이다
    const auto name = _byName.find(it->second.name);
    if (name != _byName.end() && name->second == it->first)
        _byName.erase(name);
    _people.erase(it);
}
