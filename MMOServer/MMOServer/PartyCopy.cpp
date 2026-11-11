#include "PartyCopy.h"

#if USE_PARTY

#include <algorithm>

#include "Player.h"   // _characterId · _partyId

const Party::Member* Party::Find(CharacterId characterId) const
{
    for (const Member& m : members)
    {
        if (m.row.who.characterId == ToInt(characterId))
            return &m;
    }
    return nullptr;
}

bool Party::LeaderOnline() const
{
    const Member* found = Find(leader);
    return found != nullptr && found->row.online != 0;
}

void CPartyCopy::AddPlayer(CPlayer* player)
{
    _players[ToInt(player->_characterId)] = player;
}

CPlayer* CPartyCopy::FindPlayer(CharacterId characterId) const
{
    const auto it = _players.find(ToInt(characterId));
    return it != _players.end() ? it->second : nullptr;
}

Party* CPartyCopy::FindParty(const CPlayer* player)
{
    return player->_partyId != 0 ? FindById(player->_partyId) : nullptr;
}

Party* CPartyCopy::FindById(int32_t partyId)
{
    const auto it = _parties.find(partyId);
    return it != _parties.end() ? &it->second : nullptr;
}

Party* CPartyCopy::ApplyInfo(int32_t partyId, CharacterId leader, const LinkPartyMemberRow* rows, int count)
{
    Party& party = _parties[partyId];

    // 명단에서 빠진 이 존 서버의 사람 — 보통은 「파티 없음」이 먼저 왔지만 끊겼다 붙은 뒤에는 이 길로 온다
    for (const Party::Member& old : party.members)
    {
        if (old.player == nullptr)
            continue;
        const bool kept = std::any_of(rows, rows + count, [&old](const LinkPartyMemberRow& row) { return row.who.characterId == old.row.who.characterId; });
        if (!kept)
            old.player->_partyId = 0;
    }

    party.id = partyId;
    party.leader = leader;
    party.members.clear();
    bool anyLocal = false;
    for (int i = 0; i < count; ++i)
    {
        Party::Member m;
        m.row = rows[i];
        m.player = FindPlayer(CharacterId{ rows[i].who.characterId });
        if (m.player != nullptr)
        {
            m.player->_partyId = partyId;
            anyLocal = true;
        }
        party.members.push_back(m);
    }

    if (!anyLocal)
    {
        _parties.erase(partyId);
        return nullptr;
    }
    return &party;
}

void CPartyCopy::ApplyNone(CPlayer* player)
{
    Party* party = FindParty(player);
    player->_partyId = 0;
    if (party == nullptr)
        return;
    RemoveRow(*party, player->_characterId);
    EraseIfNoLocal(party->id);
}

Party* CPartyCopy::Forget(CPlayer* player)
{
    const auto it = _players.find(ToInt(player->_characterId));
    if (it != _players.end() && it->second == player)
        _players.erase(it);

    Party* party = FindParty(player);
    player->_partyId = 0;
    if (party == nullptr)
        return nullptr;

    for (Party::Member& m : party->members)
    {
        if (m.player == player)
            m.player = nullptr;
    }
    const int32_t id = party->id;
    EraseIfNoLocal(id);
    return FindById(id);
}

void CPartyCopy::RemoveRow(Party& party, CharacterId characterId)
{
    party.members.erase(std::remove_if(party.members.begin(), party.members.end(),
                                       [characterId](const Party::Member& m) { return m.row.who.characterId == ToInt(characterId); }),
                        party.members.end());
}

void CPartyCopy::MarkOffline(Party& party, CharacterId characterId)
{
    for (Party::Member& m : party.members)
    {
        if (m.row.who.characterId != ToInt(characterId))
            continue;
        m.row.online = 0;
        m.row.visibleChannel = 0;   // 월드 서버의 Offline 과 같은 값
        return;
    }
}

Party* CPartyCopy::Relink(CPlayer* player)
{
    // 월드 서버가 없는 동안만 부른다 — 드문 길이라 파티를 다 훑는다
    for (auto& entry : _parties)
    {
        for (Party::Member& m : entry.second.members)
        {
            if (m.row.who.characterId != ToInt(player->_characterId) || m.player != nullptr)
                continue;
            m.player = player;
            player->_partyId = entry.first;
            return &entry.second;
        }
    }
    return nullptr;
}

void CPartyCopy::EraseIfNoLocal(int32_t partyId)
{
    const auto it = _parties.find(partyId);
    if (it == _parties.end())
        return;
    const std::vector<Party::Member>& members = it->second.members;
    if (std::none_of(members.begin(), members.end(), [](const Party::Member& m) { return m.player != nullptr; }))
        _parties.erase(it);
}

#endif // USE_PARTY
