#include "WorldLink.h"

#include <Common/ErrorLog.h>
#include <Link/SessionTag.h>
#include <Protocol/ServerLinkProtocol.h>
#include <cstring>

#include "EnterGate.h"
#include "MonitorManager.h"
#include "SerialBuffer.h"

CWorldLink::CWorldLink(CMonitorManager& monitor)
    : _monitor(monitor), _table(LINK_PACKET_FIRST, LINK_PACKET_LAST)
{
    // 하트비트는 처리할 것이 없다 — 받는 것만으로 코어가 세션 시한을 늘린다
    _routesOk = _table.Register<LINK_HEARTBEAT>([](SessionId, CSerialBuffer*) {});
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
        [this](SessionId) { SendRegister(); },
        []() { SLOG_WARN("[WorldLink] 월드 서버 연결 끊김 — 게임은 그대로 돌고 다시 접속한다"); });
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

    SLOG_INFO("[WorldLink] 등록 (zone server={}, max={}, web={})", _config.zoneServerId, _config.maxClients, _config.publicWsAddr);
}
