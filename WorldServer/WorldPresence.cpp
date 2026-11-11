#include "WorldPresence.h"

SessionId CWorldPresence::Register(SessionId session, ZoneServerId id, int maxClients, const std::string& wsAddr)
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
        OnDisconnected(previous);

    _zones[ToInt(session)] = Zone{ session, id, maxClients, wsAddr };
    return previous;
}

bool CWorldPresence::OnDisconnected(SessionId session)
{
    return _zones.erase(ToInt(session)) != 0;
}
