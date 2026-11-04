// ==========================================================================
// CZoneLink — 존 서버 → 캐릭터 서버 연결
//
// [책임]
//  - 캐릭터 서버에 접속해 번호 · 웹 주소 · 정원 · 지금 접속 중인 캐릭터 명단을 등록한다
//  - 입장 예고를 입장 대기 명단(CEnterGate)에 적고 답한다
//  - 입장 · 퇴장을 캐릭터 번호로 알린다 — 캐릭터 서버가 게임 중인 캐릭터의 삭제를 막는 근거다(IN_GAME)
//
// [경계]
//  재접속 · 하트비트는 CLinkConnector, 번호 표는 CEnterGate, 입장 번호 확인은 CEntryHandler 다. 여기는 패킷만 오간다.
//  패킷을 받는 자리는 CGameServer::ProcessNetworkEvents 가 구분 값으로 가른다 — 웹 포트로 온 접속은 구분 값 0 이라
//  서버 간 패킷을 보내도 여기로 안 온다.
//
// [끄기] Configure 를 안 부르면(UseServerLink=0) 스레드도 연결도 지표 줄도 없고 알림은 바로 돌아간다 — 부하 측정 조건이 안 바뀐다.
// [캐릭터 서버가 없을 때] 3초마다 다시 접속할 뿐이다. 시험 입장 길과 게임은 그대로 돈다.
//
// [스레드] Start · Stop · Configure 는 메인 스레드. 나머지는 게임 스레드 단독. WriteMetrics 만 지표 스레드(원자 값만 읽는다).
// [수명] 코어 Start 뒤 Start · 코어 ShutdownServer 앞에 Stop(CLinkConnector 약속)
// ==========================================================================
#pragma once

#include "NetIoModel.h"   // 맨 앞 — 소켓 헤더가 Windows.h 보다 먼저 와야 한다

#include <Identity/GameIds.h>
#include <Link/LinkConnector.h>
#include <Lobby/PacketTable.h>
#include <memory>
#include <sstream>
#include <string>

class CEnterGate;
class CMonitorManager;
class CSerialBuffer;
class CSessionHub;

class CZoneLink
{
public:
    struct Config
    {
        std::string characterIp;     // ServerLink.ini [Character] InternalAddr
        int         characterPort = 0;
        int         retrySec = 0;
        int         heartbeatSec = 0;
        int         zoneServerId = 0;
        int         maxClients = 0;  // 웹 정원 — 세션 표 크기(정원 + 여유)가 아니다
        std::string publicWsAddr;
        int         enterCodeTtlSec = 0;
    };

    CZoneLink(CMonitorManager& monitor, CEnterGate& enterGate, CSessionHub& sessionHub);

    // 연결을 켠다 — 메인 스레드, Start 전. false = 처리표 등록 실패이거나 웹 주소가 패킷 칸에 안 들어간다(기동 거절)
    bool Configure(const Config& config);

    void Start(NetIoModel& network);
    void Stop();

    // ── 게임 스레드 ──
    void OnConnected(SessionId sessionId);
    void OnDisconnected(SessionId sessionId);
    void OnReceived(SessionId sessionId, CSerialBuffer* msg);   // 웹 처리기와 같이 버퍼를 여기서 반납한다
    void Tick();   // 하트비트 — 게임 틱마다

    void NotifyEnter(CharacterId characterId);
    void NotifyLeave(CharacterId characterId);

    // ── 지표 스레드 ── 꺼져 있으면 줄이 안 나온다
    void WriteMetrics(std::ostringstream& ss) const;

private:
    void SendRegister();
    void RecvEnterNotice(CSerialBuffer* msg);

    CMonitorManager& _monitor;
    CEnterGate&      _enterGate;
    CSessionHub&     _sessionHub;

    CPacketTable _table;
    bool         _routesOk = false;

    Config _config;
    bool   _enabled = false;
    std::unique_ptr<CLinkConnector> _connector;   // Start 가 만든다 — 끄면 없다
};
