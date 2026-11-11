#include "WorldLink.h"

#include <Common/ErrorLog.h>
#include <Link/SessionTag.h>
#include <Protocol/ServerLinkProtocol.h>
#include <cstring>

#include "EnterGate.h"
#include "LinkOnlineList.h"
#include "MonitorManager.h"
#include "SerialBuffer.h"
#include "SessionHub.h"

CWorldLink::CWorldLink(CMonitorManager& monitor, CSessionHub& sessionHub)
    : _monitor(monitor), _sessionHub(sessionHub), _table(LINK_PACKET_FIRST, LINK_PACKET_LAST)
{
    // 하트비트는 처리할 것이 없다 — 받는 것만으로 코어가 세션 시한을 늘린다
    _routesOk = _table.Register<LINK_HEARTBEAT>([](SessionId, CSerialBuffer*) {});
    _routesOk = _table.Register<LINK_CHANNEL_LIST_ACK>([this](SessionId, CSerialBuffer* msg) { RecvChannelListAck(msg); }) && _routesOk;
}

bool CWorldLink::Configure(const Config& config)
{
    if (!_routesOk)
    {
        SLOG_ERROR("[WorldLink] 처리표 등록 실패 — 번호대 밖이거나 같은 번호를 두 번 등록했다");
        return false;
    }
    if (config.publicWsAddr.size() >= LINK_WS_ADDR_BYTES)
    {
        SLOG_ERROR("[WorldLink] PublicWsAddr 가 패킷 칸({}바이트)에 안 들어간다: {}", LINK_WS_ADDR_BYTES, config.publicWsAddr);
        return false;
    }
    _config = config;
    _enabled = true;
    return true;
}

void CWorldLink::Start(NetIoModel& network)
{
    if (!_enabled)
        return;

    CLinkConnector::Config connector;
    connector.peerName = "world";
    connector.ip = _config.worldIp;
    connector.port = _config.worldPort;
    connector.retrySec = _config.retrySec;
    connector.heartbeatSec = _config.heartbeatSec;
    connector.tag = SessionTag::LinkUpWorld;

    _connector = std::make_unique<CLinkConnector>(network, _monitor, connector);
    _connector->SetHandlers(
        [this](SessionId)
        {
            SendRegister();
            if (_callbacks.onUp)
                _callbacks.onUp();
        },
        [this]()
        {
            SLOG_WARN("[WorldLink] 월드 서버 연결 끊김 — 게임은 그대로 돌고 다시 접속한다");
            if (_callbacks.onDown)
                _callbacks.onDown();
        });
    _connector->Start();
    SLOG_INFO("[WorldLink] 월드 서버에 접속 시작 ({}:{})", _config.worldIp, _config.worldPort);
}

void CWorldLink::Stop()
{
    if (_connector)
        _connector->Stop();
}

void CWorldLink::OnConnected(SessionId sessionId)
{
    if (_connector)
        _connector->OnConnected(sessionId);
}

void CWorldLink::OnDisconnected(SessionId sessionId)
{
    if (_connector)
        _connector->OnDisconnected(sessionId);
}

void CWorldLink::OnReceived(SessionId sessionId, CSerialBuffer* msg)
{
    const CPacketTable::Dispatch result = _table.Route(sessionId, msg);
    switch (result)
    {
    case CPacketTable::Dispatch::Handled:
        break;
    case CPacketTable::Dispatch::UnknownType:
    case CPacketTable::Dispatch::TooSmall:
    case CPacketTable::Dispatch::NoHeader:
        _monitor._packetErrors.Inc();
        break;
    }
    msg->SubRef();
}

void CWorldLink::Tick()
{
    if (_connector)
        _connector->Tick(CEnterGate::NowMs());
}

void CWorldLink::SendRegister()
{
    LINK_ZONE_REGISTER reg{};
    reg.zoneServerId = _config.zoneServerId;
    reg.maxClients = _config.maxClients;
    std::memcpy(reg.publicWsAddr, _config.publicWsAddr.data(), _config.publicWsAddr.size());   // 끝 NUL 자리는 Configure 가 봤다
    _connector->Send(reg);

    const int total = LinkOnlineList::Send(*_connector, _sessionHub);

    SLOG_INFO("[WorldLink] 등록 (zone server={}, max={}, web={}, online={})", _config.zoneServerId, _config.maxClients, _config.publicWsAddr, total);
}

void CWorldLink::NotifyEnter(const CPlayer& player)
{
    if (!_connector || !_connector->IsUp())
        return;   // 끊긴 동안의 입장은 다시 등록할 때 명단에 실린다
    LINK_ZONE_CHAR_ENTER packet{};
    LinkOnlineList::Fill(packet.entry, player);
    _connector->Send(packet);
}

bool CWorldLink::SendChannels(MapId mapId, const LinkZoneChannelRow* rows, int count)
{
    if (!IsUp() || count > LINK_ZONE_CHANNELS_MAX)
        return false;

    // pack 필수 — 없으면 줄이 경계로 밀려 머리와 첫 줄 사이에 빈 칸이 낀다
#pragma pack(push, 1)
    struct ChannelsPacket
    {
        LINK_ZONE_CHANNELS head;
        LinkZoneChannelRow rows[LINK_ZONE_CHANNELS_MAX];
    };
#pragma pack(pop)
    static_assert(sizeof(ChannelsPacket) == sizeof(LINK_ZONE_CHANNELS) + sizeof(LinkZoneChannelRow) * LINK_ZONE_CHANNELS_MAX, "채널 패킷에 빈 칸이 끼었다");

    ChannelsPacket packet{};
    packet.head.mapId = ToInt(mapId);
    packet.head.count = static_cast<uint16_t>(count);
    std::memcpy(packet.rows, rows, sizeof(LinkZoneChannelRow) * static_cast<size_t>(count));
    const int size = static_cast<int>(sizeof(packet.head) + sizeof(LinkZoneChannelRow) * static_cast<size_t>(count));
    packet.head.header.size = static_cast<uint16_t>(size);
    packet.head.header.type = static_cast<MsgType>(LINK_ZONE_CHANNELS::TYPE);
    return _connector->SendBytes(&packet, size);
}

bool CWorldLink::AskChannels(uint64_t requestId, MapId mapId)
{
    if (!IsUp())
        return false;
    LINK_CHANNEL_LIST_REQ packet{};
    packet.requestId = requestId;
    packet.mapId = ToInt(mapId);
    return _connector->Send(packet);
}

void CWorldLink::RecvChannelListAck(CSerialBuffer* msg)
{
    LINK_CHANNEL_LIST_ACK head;
    if (!PacketSend::Read(msg, head))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    if (head.count > LINK_CHANNEL_LIST_MAX
        || static_cast<size_t>(msg->GetDataSize()) < sizeof(head) + size_t{ head.count } * sizeof(LinkChannelListRow))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    if (_callbacks.onChannelListAck)
        _callbacks.onChannelListAck(head, msg->GetReadBufferPtr() + sizeof(head));
}

void CWorldLink::NotifyLeave(CharacterId characterId)
{
    if (!_connector || !_connector->IsUp())
        return;   // 끊긴 동안의 퇴장은 다시 등록할 때 명단에서 빠져 있다
    LINK_ZONE_CHAR_LEAVE packet{};
    packet.characterId = ToInt(characterId);
    _connector->Send(packet);
}
