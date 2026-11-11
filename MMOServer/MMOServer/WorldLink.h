// ==========================================================================
// CWorldLink — 존 서버 → 월드 서버 연결
//
// [책임]
//  - 월드 서버에 접속해 번호 · 웹 주소 · 정원 · 지금 접속 중인 사람 명단을 등록한다
//  - 입장 · 퇴장을 알린다 — 월드 서버의 접속 장부(파티 · 이름 찾기 · 우편 신호)
//
// [경계]
//  재접속 · 하트비트는 CLinkConnector 다. 여기는 패킷만 오간다.
//  패킷을 받는 자리는 CGameServer::ProcessNetworkEvents 가 구분 값으로 가른다(SessionTag::LinkUpWorld).
//
// [끄기] Configure 를 안 부르면(UseServerLink=0 · ServerLink.ini 에 [World] 없음) 스레드도 연결도 지표 줄도 없다 — 부하 측정 조건이 안 바뀐다.
// [월드 서버가 없을 때] 3초마다 다시 접속할 뿐이다. 게임은 그대로 돈다.
//
// [스레드] Start · Stop · Configure 는 메인 스레드. 나머지는 게임 스레드 단독. 지표는 지표 스레드(원자 값만 읽는다).
// [수명] 코어 Start 뒤 Start · 코어 ShutdownServer 앞에 Stop(CLinkConnector 약속)
// ==========================================================================
#pragma once

#include "NetIoModel.h"   // 맨 앞 — 소켓 헤더가 Windows.h 보다 먼저 와야 한다

#include <Identity/GameIds.h>
#include <Link/LinkConnector.h>
#include <Lobby/PacketTable.h>
#include <functional>
#include <memory>
#include <string>

class CMonitorManager;
class CPlayer;
class CSerialBuffer;
class CSessionHub;
struct LINK_CHANNEL_LIST_ACK;
struct LINK_PARTY_OP;
struct LinkPartyRestoreRow;
struct LinkPartyStateRow;
struct LinkZoneChannelRow;

class CWorldLink
{
public:
    struct Config
    {
        std::string worldIp;         // ServerLink.ini [World] InternalAddr
        int         worldPort = 0;
        int         retrySec = 0;
        int         heartbeatSec = 0;
        int         zoneServerId = 0;
        int         maxClients = 0;  // 웹 정원 — 세션 표 크기(정원 + 여유)가 아니다
        std::string publicWsAddr;
    };

    CWorldLink(CMonitorManager& monitor, CSessionHub& sessionHub);

    // 연결을 켠다 — 메인 스레드, Start 전. false = 처리표 등록 실패이거나 웹 주소가 패킷 칸에 안 들어간다(기동 거절)
    bool Configure(const Config& config);

    // 위쪽에 알릴 것 — Start 전에 넣는다
    struct Callbacks
    {
        std::function<void()> onUp;     // 연결됐고 등록을 보냈다
        std::function<void()> onDown;   // 끊겼다
        std::function<void(const LINK_CHANNEL_LIST_ACK& head, const char* rows)> onChannelListAck;   // 줄 수는 검사를 마쳤다
    };
    void SetCallbacks(Callbacks callbacks) { _callbacks = std::move(callbacks); }

    void Start(NetIoModel& network);
    void Stop();

    // ── 게임 스레드 ──
    void OnConnected(SessionId sessionId);
    void OnDisconnected(SessionId sessionId);
    void OnReceived(SessionId sessionId, CSerialBuffer* msg);   // 웹 처리기와 같이 버퍼를 여기서 반납한다
    void Tick();   // 하트비트 — 게임 틱마다

    void NotifyEnter(const CPlayer& player);
    void NotifyLeave(CharacterId characterId);

    bool IsUp() const { return _connector && _connector->IsUp(); }
    bool SendChannels(MapId mapId, const LinkZoneChannelRow* rows, int count);   // 끊겨 있으면 false
    bool AskChannels(uint64_t requestId, MapId mapId);                           // 끊겨 있으면 false
    bool SendPartyOp(const LINK_PARTY_OP& packet);                               // 끊겨 있으면 false
    bool SendPartyState(const LinkPartyStateRow* rows, int count);               // 끊겨 있으면 false
    bool SendPartyRestore(const LinkPartyRestoreRow* rows, int count);           // 끊겨 있으면 false

    // 몸을 만든 쪽이 그대로 보내는 패킷 — 계정 우편 요청(CAccountMailClient). 끊겨 있으면 false
    template <typename TPacket>
    bool SendLink(const TPacket& packet) { return IsUp() && _connector->Send(packet); }

    // 컨텐츠가 자기 받는 패킷을 등록하는 자리 — 등록은 Start 전
    CPacketTable& Routes() { return _table; }
    void CountPacketError();

    // 꺼져 있으면 nullptr — 지표는 CLinkConnector::WriteMetrics 가 연결 여럿을 한 번에 쓴다
    const CLinkConnector* Connector() const { return _connector.get(); }

private:
    void SendRegister();
    void RecvChannelListAck(CSerialBuffer* msg);

    CMonitorManager& _monitor;
    CSessionHub&     _sessionHub;

    CPacketTable _table;
    bool         _routesOk = false;

    Config    _config;
    Callbacks _callbacks;
    bool      _enabled = false;
    std::unique_ptr<CLinkConnector> _connector;   // Start 가 만든다 — 끄면 없다
};
