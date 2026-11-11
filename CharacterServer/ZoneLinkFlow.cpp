#include "ZoneLinkFlow.h"

#include <Common/ErrorLog.h>
#include <Link/LinkPeers.h>
#include <Link/LinkRequests.h>
#include <Lobby/LobbyLoop.h>
#include <Lobby/PacketSend.h>
#include <Protocol/ServerLinkProtocol.h>
#include <cstring>
#include <string>

#include "ZoneDirectory.h"

CZoneLinkFlow::CZoneLinkFlow(CMonitorManager& monitor, CLinkPeers& peers, CZoneDirectory& zones, CLinkRequests& requests)
    : _monitor(monitor), _peers(peers), _zones(zones), _requests(requests)
{
}

bool CZoneLinkFlow::CheckRegistered(SessionId sessionId)
{
    if (_zones.IsRegistered(sessionId))
        return true;
    _monitor._packetErrors.Inc();
    return false;
}

void CZoneLinkFlow::OnRegister(SessionId sessionId, CSerialBuffer* msg)
{
    LINK_ZONE_REGISTER packet;
    if (!PacketSend::Read(msg, packet))
    {
        _monitor._packetErrors.Inc();
        return;
    }

    size_t length = 0;
    while (length < sizeof(packet.publicWsAddr) && packet.publicWsAddr[length] != 0)
        ++length;
    const std::string wsAddr(packet.publicWsAddr, length);

    const SessionId previous = _zones.Register(sessionId, ZoneServerId{ packet.zoneServerId }, packet.maxClients, wsAddr);
    if (previous != NO_SESSION)
    {
        SLOG_WARN("[ZoneLink] 존 서버 {} 가 다시 등록 — 옛 연결을 끊음 (session={})", packet.zoneServerId, ToInt(previous));
        _requests.FailAll(LinkRequestKind::EnterNotice);   // 옛 연결의 끊김 이벤트는 등록을 안 건드리므로 여기서 끝낸다
        _peers.Disconnect(previous);
    }
    SLOG_INFO("[ZoneLink] 존 서버 {} 등록 — {} · 정원 {}", packet.zoneServerId, wsAddr, packet.maxClients);
}

void CZoneLinkFlow::OnOnlineList(SessionId sessionId, CSerialBuffer* msg)
{
    if (!CheckRegistered(sessionId))
        return;

    LINK_ZONE_ONLINE_LIST head;
    if (!PacketSend::Read(msg, head))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    if (head.count > LINK_ONLINE_LIST_MAX
        || static_cast<size_t>(msg->GetDataSize()) < sizeof(head) + size_t{ head.count } * sizeof(LinkOnlineEntry))
    {
        _monitor._packetErrors.Inc();
        return;
    }

    const char* data = msg->GetReadBufferPtr() + sizeof(head);
    for (uint16_t i = 0; i < head.count; ++i)
    {
        LinkOnlineEntry entry;
        std::memcpy(&entry, data + size_t{ i } * sizeof(LinkOnlineEntry), sizeof(entry));
        _zones.Enter(sessionId, CharacterId{ entry.characterId }, AccountId{ entry.accountId });
    }
}

void CZoneLinkFlow::OnNoticeAck(SessionId sessionId, CSerialBuffer* msg)
{
    if (!CheckRegistered(sessionId))
        return;

    LINK_ENTER_NOTICE_ACK ack;
    if (!PacketSend::Read(msg, ack))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    _requests.Complete(ack.requestId, CLobbyLoop::NowMs(), &ack);   // 시한이 지난 뒤 온 답은 짝이 없어 버려진다
}

void CZoneLinkFlow::OnCharEnter(SessionId sessionId, CSerialBuffer* msg)
{
    if (!CheckRegistered(sessionId))
        return;

    LINK_ZONE_CHAR_ENTER packet;
    if (!PacketSend::Read(msg, packet))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    _zones.Enter(sessionId, CharacterId{ packet.entry.characterId }, AccountId{ packet.entry.accountId });
}

void CZoneLinkFlow::OnCharLeave(SessionId sessionId, CSerialBuffer* msg)
{
    if (!CheckRegistered(sessionId))
        return;

    LINK_ZONE_CHAR_LEAVE packet;
    if (!PacketSend::Read(msg, packet))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    _zones.Leave(sessionId, CharacterId{ packet.characterId });
}

void CZoneLinkFlow::OnDisconnected(SessionId sessionId)
{
    if (!_zones.OnDisconnected(sessionId))
        return;   // 등록 전이거나 이미 새 연결로 바뀐 연결이다 — 새 연결의 대기를 건드리면 안 된다
    _requests.FailAll(LinkRequestKind::EnterNotice);
}
