#pragma once
// ==========================================================================
// CCharLoginFlow — 토큰 → 계정 서버에 확인 → 유예 지난 자리 풀기 → 목록
//
//   계정 번호는 토큰 확인의 답에서만 얻는다 — 웹이 보낸 번호를 믿을 칸이 없다.
//   실패하면 세션을 Fresh 로 되돌린다 — 같은 연결로 다시 내도 받는다(지금 웹은 연결을 닫고 새 토큰으로 새 연결을 연다).
//   한 계정 한 창 — 성공하면 같은 계정의 옛 세션을 끊는다.
//   로직 스레드 전용.
// ==========================================================================

#include "IOCPServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서는 이 헤더가 맡는다
#include "MonitorManager.h"

#include <Identity/GameIds.h>
#include <Protocol/CharacterProtocol.h>
#include <Protocol/ServerLinkProtocol.h>

#include "CharacterDb.h"

class CDbPipeline;
class CLinkConnector;
class CLinkRequests;
class CCharSessionTable;
class CCharacterMetrics;

class CCharLoginFlow
{
public:
    CCharLoginFlow(CIOCPServer& network, CMonitorManager& monitor, CDbPipeline& db, CCharSessionTable& sessions,
                   CLinkConnector& account, CLinkRequests& requests, CCharacterMetrics& metrics, int graceDays);

    void OnLogin(SessionId sessionId, CSerialBuffer* msg);   // 웹 — C2S_CHAR_LOGIN
    void OnTokenAck(CSerialBuffer* msg);                     // 계정 서버 — LINK_TOKEN_CHECK_ACK

private:
    void OnTokenReply(SessionId sessionId, const LINK_TOKEN_CHECK_ACK* ack);
    void OnListLoaded(SessionId sessionId, AccountId accountId, const CCharacterDb::ListResult& list);
    void Fail(SessionId sessionId, CharLoginResult result);

    CIOCPServer& _network;
    CMonitorManager& _monitor;
    CDbPipeline& _db;
    CCharSessionTable& _sessions;
    CLinkConnector& _account;
    CLinkRequests& _requests;
    CCharacterMetrics& _metrics;
    int _graceDays;
};
