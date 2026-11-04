#pragma once
// ==========================================================================
// CSelectFlow — 소유 · 삭제 확인 → 입장 번호 → 존 서버에 예고 → 응답 → 웹에 회신
//
//   웹에 입장 번호를 주기 전에 존 서버가 받았는지 확인한다 — 웹이 먼저 닿아도 명단에 번호가 있다.
//   입장 번호의 만료 시각은 존 서버가 답에 실은 유효 초로 계산한다(값이 두 곳에 생기지 않게).
//   로직 스레드 전용.
// ==========================================================================

#include "IOCPServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서는 이 헤더가 맡는다
#include "MonitorManager.h"

#include <Identity/GameIds.h>
#include <Protocol/CharacterProtocol.h>
#include <Protocol/ServerLinkProtocol.h>
#include <string>

class CDbPipeline;
class CLinkPeers;
class CLinkRequests;
class CZoneDirectory;
class CCharSessionTable;
class CCharacterMetrics;

class CSelectFlow
{
public:
    CSelectFlow(CIOCPServer& network, CMonitorManager& monitor, CDbPipeline& db, CCharSessionTable& sessions, CZoneDirectory& zones,
                CLinkPeers& peers, CLinkRequests& requests, CCharacterMetrics& metrics);

    void OnSelect(SessionId sessionId, CSerialBuffer* msg);

private:
    struct Pending
    {
        SessionId   sessionId = NO_SESSION;
        AccountId   accountId = NO_ACCOUNT;
        CharacterId characterId = NO_CHARACTER;
        SessionId   zoneSession = NO_SESSION;
        std::string zoneWsAddr;
        char        enterCode[CHAR_CODE_BYTES]{};
    };

    void OnChecked(SessionId sessionId, AccountId accountId, CharacterId characterId, CharSelectResult checked);
    void OnNoticeReply(const Pending& pending, const LINK_ENTER_NOTICE_ACK* ack);
    void Reply(SessionId sessionId, CharSelectResult result, const Pending* granted);
    bool CanReply(SessionId sessionId);

    CIOCPServer& _network;
    CMonitorManager& _monitor;
    CDbPipeline& _db;
    CCharSessionTable& _sessions;
    CZoneDirectory& _zones;
    CLinkPeers& _peers;
    CLinkRequests& _requests;
    CCharacterMetrics& _metrics;
};
