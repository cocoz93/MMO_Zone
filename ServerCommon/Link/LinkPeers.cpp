#include "LinkPeers.h"

#include <algorithm>

#include <Protocol/ServerLinkProtocol.h>

CLinkPeers::CLinkPeers(CIOCPServer& network, CMonitorManager& monitor, int heartbeatSec)
    : _network(network), _monitor(monitor), _heartbeatSec(heartbeatSec)
{
}

bool CLinkPeers::Has(SessionId sessionId) const
{
    return std::find(_peers.begin(), _peers.end(), sessionId) != _peers.end();
}

void CLinkPeers::OnConnected(SessionId sessionId)
{
    _peers.push_back(sessionId);
    if (_onConnected)
        _onConnected(sessionId);
}

void CLinkPeers::OnDisconnected(SessionId sessionId)
{
    auto found = std::find(_peers.begin(), _peers.end(), sessionId);
    if (found == _peers.end())
        return;
    _peers.erase(found);
    if (_onDisconnected)
        _onDisconnected(sessionId);
}

void CLinkPeers::Tick(int64_t nowMs)
{
    if (nowMs - _lastHeartbeatMs < static_cast<int64_t>(_heartbeatSec) * 1000)
        return;
    _lastHeartbeatMs = nowMs;
    for (SessionId sessionId : _peers)
        Send(sessionId, LINK_HEARTBEAT{});
}

void CLinkPeers::Disconnect(SessionId sessionId)
{
    _network.RequestDisconnectSession(ToInt(sessionId));
}
