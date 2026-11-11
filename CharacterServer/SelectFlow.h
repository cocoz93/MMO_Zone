#pragma once
// ==========================================================================
// CSelectFlow — 소유 · 삭제 확인 → 중복 판정 → 입장 번호 → 존 서버에 예고 → 응답 → 웹에 회신
//
//   중복 판정 — 같은 계정의 줄(접속 중 · 입장 대기)이 있으면 그 존 서버들에 내보내기(3042)를 보내고 DUPLICATE 로 답한다.
//   길 안내 — 주인 존 서버 → 나머지를 번호순으로 번갈아. Full 이면 같은 입장 번호로 다음 후보에 다시 예고한다.
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
#include <vector>

#include "CharacterDb.h"
#include "ZoneDirectory.h"

class CDbPipeline;
class CLinkPeers;
class CLinkRequests;
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
        ZoneServerId zoneId{ 0 };
        std::string zoneWsAddr;
        char        enterCode[CHAR_CODE_BYTES]{};

        ZoneServerId ownerZone{ 0 };
        std::vector<CZoneDirectory::Zone> candidates;
        size_t      next = 0;         // 다음에 물어볼 후보
        bool        sawFull = false;  // 후보 중 하나라도 찼다고 답했다 — 끝까지 못 들어가면 ZONE_FULL
    };

    void OnChecked(SessionId sessionId, AccountId accountId, CharacterId characterId, const CCharacterDb::SelectChecked& checked);
    void SendNextNotice(Pending pending);
    void OnNoticeReply(Pending pending, const LINK_ENTER_NOTICE_ACK* ack);
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
