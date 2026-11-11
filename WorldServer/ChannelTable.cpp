#include "ChannelTable.h"

#include <algorithm>

void CChannelTable::Update(ZoneServerId zone, int32_t mapId, const LinkZoneChannelRow* rows, int count)
{
    std::vector<LinkZoneChannelRow>& slot = _maps[mapId][ToInt(zone)];
    _rowCount += count - static_cast<int>(slot.size());
    slot.assign(rows, rows + count);
    std::sort(slot.begin(), slot.end(),
              [](const LinkZoneChannelRow& a, const LinkZoneChannelRow& b) { return a.channel < b.channel; });
}

void CChannelTable::RemoveZone(ZoneServerId zone)
{
    for (auto map = _maps.begin(); map != _maps.end(); )
    {
        const auto found = map->second.find(ToInt(zone));
        if (found != map->second.end())
        {
            _rowCount -= static_cast<int>(found->second.size());
            map->second.erase(found);
        }
        if (map->second.empty())
            map = _maps.erase(map);
        else
            ++map;
    }
}

int CChannelTable::ListOf(int32_t mapId, LinkChannelListRow* out, int maxRows) const
{
    const auto map = _maps.find(mapId);
    if (map == _maps.end())
        return 0;

    int filled = 0;
    for (const auto& [zone, rows] : map->second)
    {
        for (const LinkZoneChannelRow& row : rows)
        {
            if (filled == maxRows)
                return filled;
            out[filled].zoneServerId = static_cast<uint16_t>(zone);
            out[filled].channel = static_cast<uint16_t>(row.channel);
            out[filled].players = row.players;
            ++filled;
        }
    }
    return filled;
}
