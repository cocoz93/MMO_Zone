#pragma once
// ==========================================================================
// CHandoffFlow — 다른 존 서버로 채널 바꾸기: 존 서버의 요청을 받아 목적지에 입장 예고를 넣고 입장 번호를 돌려준다
//
//   고르기(CSelectFlow)와 같이 예고가 받아들여진 뒤에만 번호를 돌려준다 — 웹이 먼저 닿아도 명단에 번호가 있다.
//   후보를 돌지 않는다. 목적지는 플레이어가 고른 존 서버 하나다.
//   예고 응답을 기다리는 표는 고르기와 같은 종류(EnterNotice)라, 존 서버가 끊기면 CZoneLinkFlow 가 함께 끝낸다.
//   로직 스레드 전용.
// ==========================================================================

#include "IOCPServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서는 이 헤더가 맡는다
#include "MonitorManager.h"

#include <Identity/GameIds.h>
#include <Protocol/ServerLinkProtocol.h>
#include <string>

class CLinkPeers;
class CLinkRequests;
class CZoneDirectory;

class CHandoffFlow
{
public:
    CHandoffFlow(CMonitorManager& monitor, CLinkPeers& peers, CZoneDirectory& zones, CLinkRequests& requests);

    void OnRequest(SessionId sessionId, CSerialBuffer* msg);

private:
    struct Pending
    {
        SessionId   requester = NO_SESSION;   // 묻는 존 서버
        uint64_t    requestId = 0;            // 그 존 서버의 요청 번호
        CharacterId characterId = NO_CHARACTER;
        AccountId   accountId = NO_ACCOUNT;
        SessionId   destSession = NO_SESSION;
        std::string destWsAddr;
        char        enterCode[LINK_CODE_BYTES]{};
    };

    void OnNoticeReply(const Pending& pending, const LINK_ENTER_NOTICE_ACK* ack);
    void Reply(const Pending& pending, LinkHandoffResult result, int ttlSec);

    CMonitorManager& _monitor;
    CLinkPeers& _peers;
    CZoneDirectory& _zones;
    CLinkRequests& _requests;
};
