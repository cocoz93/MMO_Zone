#pragma once
// ==========================================================================
// CZoneLinkFlow — 존 서버가 보낸 패킷을 장부로 옮긴다: 등록 · 명단 · 입장/퇴장 · 예고 응답
//
//   등록하지 않은 연결의 패킷은 오류다 — 서버 간 포트가 내부 주소에서만 열려 있어도 믿지 않는다.
//   같은 번호로 다시 등록하면 옛 연결을 끊는다. 거절하면 존 서버가 재시작한 뒤 옛 연결이 끊길 때까지 등록하지 못한다.
//   로직 스레드 전용.
// ==========================================================================

#include "IOCPServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서는 이 헤더가 맡는다
#include "MonitorManager.h"

#include <Identity/GameIds.h>

class CLinkPeers;
class CLinkRequests;
class CZoneDirectory;

class CZoneLinkFlow
{
public:
    CZoneLinkFlow(CMonitorManager& monitor, CLinkPeers& peers, CZoneDirectory& zones, CLinkRequests& requests);

    void OnRegister(SessionId sessionId, CSerialBuffer* msg);
    void OnOnlineList(SessionId sessionId, CSerialBuffer* msg);
    void OnNoticeAck(SessionId sessionId, CSerialBuffer* msg);
    void OnCharEnter(SessionId sessionId, CSerialBuffer* msg);
    void OnCharLeave(SessionId sessionId, CSerialBuffer* msg);
    void OnDisconnected(SessionId sessionId);

private:
    bool CheckRegistered(SessionId sessionId);

    CMonitorManager& _monitor;
    CLinkPeers& _peers;
    CZoneDirectory& _zones;
    CLinkRequests& _requests;
};
