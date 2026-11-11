#include "WorldServer.h"

#include <Common/ErrorLog.h>
#include <Link/SessionTag.h>
#include <Lobby/PacketSend.h>
#include <Protocol/ServerLinkProtocol.h>
#include <cstring>
#include <sstream>
#include <string>

namespace
{
    // 코어 값 중 설정에 안 내놓는 것 — 로비 서버와 같은 값이다. 재 본 적이 없다
    constexpr int SEND_WORKERS = 2;
    constexpr int RIO_WORKERS = 2;
    constexpr int COMPLETION_BATCH = 0;
    constexpr int SEND_DEPTH = 1;
    constexpr int64_t SWEEP_PERIOD_MS = 1000;   // 게이지 갱신 주기 — 로비 서버와 같은 값

    CLinkConnector::Config AccountLink(const ServerLinkConfig& link)
    {
        CLinkConnector::Config config;
        config.peerName = "account";
        config.ip = link.account.ip;
        config.port = link.account.port;
        config.retrySec = link.retrySec;
        config.heartbeatSec = link.heartbeatSec;
        return config;
    }
}

CWorldServer::CWorldServer(const WorldServerConfig& config, const ServerLinkConfig& link)
    : _config(config)
    , _link(link)
    , _network(config.port, config.maxClients + link.sessionReserve, ServerMode::GameServer, _monitor,
               config.workerThreads, SEND_WORKERS, RIO_WORKERS, COMPLETION_BATCH, SEND_DEPTH)
    , _loop(_network, _monitor, config.logicPeriodMs)
    , _account(_network, _monitor, AccountLink(link))
    , _peers(_network, _monitor, link.heartbeatSec)
    , _accountTable(LINK_PACKET_FIRST, LINK_PACKET_LAST)
    , _zoneTable(LINK_PACKET_FIRST, LINK_PACKET_LAST)
    , _coreMetrics(_monitor)
    , _endpoint(config.monitorPort, [this]() { return BuildMetricsText(); })
{
}

bool CWorldServer::RegisterRoutes()
{
    bool ok = _accountTable.Register<LINK_HEARTBEAT>([](SessionId, CSerialBuffer*) {});   // 받는 것만으로 코어가 세션 시한을 늘린다

    ok = _zoneTable.Register<LINK_HEARTBEAT>([](SessionId, CSerialBuffer*) {}) && ok;
    ok = _zoneTable.Register<LINK_ZONE_REGISTER>([this](SessionId id, CSerialBuffer* msg) { OnZoneRegister(id, msg); }) && ok;
    ok = _zoneTable.Register<LINK_ZONE_ONLINE_LIST>([this](SessionId id, CSerialBuffer* msg) { OnOnlineList(id, msg); }) && ok;
    ok = _zoneTable.Register<LINK_ZONE_CHAR_ENTER>([this](SessionId id, CSerialBuffer* msg) { OnCharEnter(id, msg); }) && ok;
    ok = _zoneTable.Register<LINK_ZONE_CHAR_LEAVE>([this](SessionId id, CSerialBuffer* msg) { OnCharLeave(id, msg); }) && ok;
    return ok;
}

bool CWorldServer::Start()
{
    if (!RegisterRoutes())
    {
        SLOG_ERROR("[WorldServer] 처리표 등록 실패 — 번호대 밖이거나 같은 번호를 두 번 등록했다");
        return false;
    }

    _network.SetUntaggedSessionLimit(_config.maxClients);   // 손님 포트가 서버 간 연결 자리를 못 먹게
    if (!_network.AddListener(_link.world.ip.c_str(), _link.world.port, static_cast<int>(SessionTag::LinkDown)))
    {
        SLOG_ERROR("[WorldServer] 서버 간 포트 주소가 틀림: {}:{}", _link.world.ip, _link.world.port);
        return false;
    }
    if (!_network.Start())
    {
        SLOG_ERROR("[WorldServer] 코어 시작 실패");
        return false;
    }
    _started = true;

    _loop.SetRoute(SessionTag::Web, nullptr,
        [this](SessionId id) { _network.RequestDisconnectSession(ToInt(id)); },   // 받을 손님이 없다
        nullptr);
    _loop.SetRoute(SessionTag::LinkUp, &_accountTable,
        [this](SessionId id) { _account.OnConnected(id); },
        [this](SessionId id) { _account.OnDisconnected(id); });
    _loop.SetRoute(SessionTag::LinkDown, &_zoneTable,
        [this](SessionId id) { _peers.OnConnected(id); },
        [this](SessionId id) { _peers.OnDisconnected(id); });
    _loop.SetPeriodic([this](int64_t nowMs) { Periodic(nowMs); });

    _account.SetHandlers(
        [this](SessionId id)
        {
            SLOG_INFO("[WorldServer] 계정 서버 연결됨 (session={})", ToInt(id));
            SendWorldRegister();
        },
        []() { SLOG_WARN("[WorldServer] 계정 서버 끊김"); });
    _peers.SetHandlers(
        [](SessionId id) { SLOG_INFO("[WorldServer] 존 서버 접속 (session={})", ToInt(id)); },
        [this](SessionId id)
        {
            SLOG_WARN("[WorldServer] 존 서버 끊김 (session={})", ToInt(id));
            _presence.OnDisconnected(id);
        });

    _loop.Start();
    _account.Start();

    if (!_endpoint.Start())
        SLOG_WARN("[WorldServer] 지표 포트 {} 를 못 열었다 — 서버는 그대로 돈다", _config.monitorPort);

    SLOG_INFO("[WorldServer] Started - link {}:{} · metrics {}", _link.world.ip, _link.world.port, _config.monitorPort);
    return true;
}

void CWorldServer::Stop()
{
    if (!_started)
        return;
    _started = false;

    _endpoint.Stop();
    _account.Stop();       // 코어를 내리기 전에 — 종료 중 세션 표를 건드리지 않게(코어 약속)
    _loop.Stop();          // 받을 쪽부터 — 남은 이벤트의 버퍼를 반납한다
    _network.ShutdownServer();
}

void CWorldServer::Periodic(int64_t nowMs)
{
    _account.Tick(nowMs);
    _peers.Tick(nowMs);

    if (nowMs >= _nextSweepMs)
    {
        _nextSweepMs = nowMs + SWEEP_PERIOD_MS;
        _metrics.SetZones(_presence.ZoneCount());
        _metrics.SetOnline(_presence.OnlineCount());
    }
}

void CWorldServer::OnZoneRegister(SessionId sessionId, CSerialBuffer* msg)
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

    const SessionId previous = _presence.Register(sessionId, ZoneServerId{ packet.zoneServerId }, packet.maxClients, wsAddr);
    if (previous != NO_SESSION)
    {
        SLOG_WARN("[WorldServer] 존 서버 {} 가 다시 등록 — 옛 연결을 끊음 (session={})", packet.zoneServerId, ToInt(previous));
        _peers.Disconnect(previous);
    }
    SLOG_INFO("[WorldServer] 존 서버 {} 등록 — {} · 정원 {}", packet.zoneServerId, wsAddr, packet.maxClients);
}

bool CWorldServer::CheckRegistered(SessionId sessionId)
{
    if (_presence.IsRegistered(sessionId))
        return true;
    _monitor._packetErrors.Inc();
    return false;
}

void CWorldServer::OnOnlineList(SessionId sessionId, CSerialBuffer* msg)
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
        _presence.Enter(sessionId, entry);
    }
}

void CWorldServer::OnCharEnter(SessionId sessionId, CSerialBuffer* msg)
{
    if (!CheckRegistered(sessionId))
        return;

    LINK_ZONE_CHAR_ENTER packet;
    if (!PacketSend::Read(msg, packet))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    _presence.Enter(sessionId, packet.entry);
}

void CWorldServer::OnCharLeave(SessionId sessionId, CSerialBuffer* msg)
{
    if (!CheckRegistered(sessionId))
        return;

    LINK_ZONE_CHAR_LEAVE packet;
    if (!PacketSend::Read(msg, packet))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    _presence.Leave(sessionId, CharacterId{ packet.characterId });
}

void CWorldServer::SendWorldRegister()
{
    LINK_WORLD_REGISTER packet{};
    packet.worldId = _config.worldId;
    _account.Send(packet);
}

std::string CWorldServer::BuildMetricsText()
{
    std::ostringstream ss;
    _coreMetrics.Write(ss);
    _metrics.Write(ss);
    _account.WriteMetrics(ss);
    return ss.str();
}
