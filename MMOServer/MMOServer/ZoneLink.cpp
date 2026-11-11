#include "ZoneLink.h"

#include <Common/ErrorLog.h>
#include <Protocol/ServerLinkProtocol.h>
#include <cstring>

#include "EnterGate.h"
#include "LinkOnlineList.h"
#include "MonitorManager.h"
#include "Player.h"
#include "SerialBuffer.h"
#include "SessionHub.h"

static_assert(LINK_CODE_BYTES == CEnterGate::CODE_BYTES, "입장 번호 칸 크기가 어긋났다");

CZoneLink::CZoneLink(CMonitorManager& monitor, CEnterGate& enterGate, CSessionHub& sessionHub)
    : _monitor(monitor), _enterGate(enterGate), _sessionHub(sessionHub), _table(LINK_PACKET_FIRST, LINK_PACKET_LAST)
{
    // 하트비트는 처리할 것이 없다 — 받는 것만으로 코어가 세션 시한을 늘린다
    _routesOk = _table.Register<LINK_HEARTBEAT>([](SessionId, CSerialBuffer*) {});
    _routesOk = _table.Register<LINK_ENTER_NOTICE_REQ>([this](SessionId, CSerialBuffer* msg) { RecvEnterNotice(msg); }) && _routesOk;
    _routesOk = _table.Register<LINK_ZONE_KICK_ACCOUNT>([this](SessionId, CSerialBuffer* msg) { RecvKickAccount(msg); }) && _routesOk;
}

bool CZoneLink::Configure(const Config& config)
{
    if (!_routesOk)
    {
        SLOG_ERROR("[ZoneLink] 처리표 등록 실패 — 번호대 밖이거나 같은 번호를 두 번 등록했다");
        return false;
    }
    if (config.publicWsAddr.size() >= LINK_WS_ADDR_BYTES)
    {
        SLOG_ERROR("[ZoneLink] PublicWsAddr 가 패킷 칸({}바이트)에 안 들어간다: {}", LINK_WS_ADDR_BYTES, config.publicWsAddr);
        return false;
    }
    _config = config;
    _enabled = true;
    return true;
}

void CZoneLink::Start(NetIoModel& network)
{
    if (!_enabled)
        return;

    CLinkConnector::Config connector;
    connector.peerName = "character";
    connector.ip = _config.characterIp;
    connector.port = _config.characterPort;
    connector.retrySec = _config.retrySec;
    connector.heartbeatSec = _config.heartbeatSec;

    _connector = std::make_unique<CLinkConnector>(network, _monitor, connector);
    _connector->SetHandlers(
        [this](SessionId) { SendRegister(); },
        []() { SLOG_WARN("[ZoneLink] 캐릭터 서버 연결 끊김 — 게임은 그대로 돌고 다시 접속한다"); });
    _connector->Start();
    SLOG_INFO("[ZoneLink] 캐릭터 서버에 접속 시작 ({}:{})", _config.characterIp, _config.characterPort);
}

void CZoneLink::Stop()
{
    if (_connector)
        _connector->Stop();
}

void CZoneLink::OnConnected(SessionId sessionId)
{
    if (_connector)
        _connector->OnConnected(sessionId);
}

void CZoneLink::OnDisconnected(SessionId sessionId)
{
    if (_connector)
        _connector->OnDisconnected(sessionId);
}

void CZoneLink::OnReceived(SessionId sessionId, CSerialBuffer* msg)
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

void CZoneLink::Tick()
{
    if (_connector)
        _connector->Tick(CEnterGate::NowMs());
}

// 등록 한 장 → 명단 여러 장. 명단이 비었으면 등록만 간다 — 받는 쪽은 연결이 끊길 때 이 존의 장부를 비우므로 빈 명단을 알릴 일이 없다.
void CZoneLink::SendRegister()
{
    LINK_ZONE_REGISTER reg{};
    reg.zoneServerId = _config.zoneServerId;
    reg.maxClients = _config.maxClients;
    std::memcpy(reg.publicWsAddr, _config.publicWsAddr.data(), _config.publicWsAddr.size());   // 끝 NUL 자리는 Configure 가 봤다
    _connector->Send(reg);

    const int total = LinkOnlineList::Send(*_connector, _sessionHub);

    SLOG_INFO("[ZoneLink] 등록 (zone server={}, max={}, web={}, online={})",
              _config.zoneServerId, _config.maxClients, _config.publicWsAddr, total);
}

void CZoneLink::RecvEnterNotice(CSerialBuffer* msg)
{
    LINK_ENTER_NOTICE_REQ request;
    if (!PacketSend::Read(msg, request))
    {
        _monitor._packetErrors.Inc();
        return;
    }

    // 정원의 접속자 수 = 웹 세션 전부(게임 중 + 불러오는 중 + 입장 대기). 구분 값이 있는 서버 간 연결은 코어가 따로 센다
    const int online = static_cast<int>(_monitor._sessionCount.Load());
    const bool accepted = _enterGate.AddNotice(request.enterCode, CharacterId{ request.characterId }, AccountId{ request.accountId },
                                               online, _config.maxClients, CEnterGate::NowMs());
    if (!accepted)
        SLOG_WARN("[ZoneLink] 입장 예고 거절: 정원 (character={}, online={}, codes={}, max={})",
                  request.characterId, online, _enterGate.LiveCount(), _config.maxClients);

    LINK_ENTER_NOTICE_ACK ack{};
    ack.requestId = request.requestId;
    ack.result = static_cast<uint8_t>(accepted ? EnterNoticeResult::Ok : EnterNoticeResult::Full);
    ack.ttlSec = _config.enterCodeTtlSec;
    _connector->Send(ack);
}

void CZoneLink::RecvKickAccount(CSerialBuffer* msg)
{
    LINK_ZONE_KICK_ACCOUNT packet;
    if (!PacketSend::Read(msg, packet) || packet.accountId == ToInt(NO_ACCOUNT))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    if (_kickAccount)
        _kickAccount(AccountId{ packet.accountId });
}

void CZoneLink::NotifyEnter(const CPlayer& player)
{
    if (!_connector || !_connector->IsUp())
        return;   // 끊긴 동안의 입장은 다시 등록할 때 명단에 실린다
    LINK_ZONE_CHAR_ENTER packet{};
    LinkOnlineList::Fill(packet.entry, player);
    _connector->Send(packet);
}

void CZoneLink::NotifyLeave(CharacterId characterId)
{
    if (!_connector || !_connector->IsUp())
        return;   // 끊긴 동안의 퇴장은 다시 등록할 때 명단에서 빠져 있다
    LINK_ZONE_CHAR_LEAVE packet{};
    packet.characterId = ToInt(characterId);
    _connector->Send(packet);
}
