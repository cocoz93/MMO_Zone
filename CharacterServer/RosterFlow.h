#pragma once
// ==========================================================================
// CRosterFlow — 만들기(유예 지난 자리 풀기 포함) · 삭제 · 복구
//
//   형식 판정은 로직 스레드에서 끝낸다 — 틀린 요청이 DB 큐를 쓰지 않는다.
//   이름 · 자리 겹침은 DB 유일 키가 판정한다 — 미리 읽어 보면 두 요청이 동시에 통과한다.
//   계정당 처리 중 요청은 하나다(CCharSessionTable). 로직 스레드 전용.
// ==========================================================================

#include "IOCPServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서는 이 헤더가 맡는다
#include "MonitorManager.h"

#include <Identity/GameIds.h>
#include <Protocol/CharacterProtocol.h>

#include "CharacterDb.h"

class CDbPipeline;
class CJobTable;
class CLookTable;
class CZoneDirectory;
class CCharSessionTable;
class CCharacterMetrics;

class CRosterFlow
{
public:
    CRosterFlow(CIOCPServer& network, CMonitorManager& monitor, CDbPipeline& db, CCharSessionTable& sessions, CZoneDirectory& zones,
                const CJobTable& jobs, const CLookTable& looks, CCharacterMetrics& metrics, int slotCount, int graceDays);

    void OnCreate(SessionId sessionId, CSerialBuffer* msg);
    void OnDelete(SessionId sessionId, CSerialBuffer* msg);
    void OnRestore(SessionId sessionId, CSerialBuffer* msg);

private:
    // 형식이 맞으면 OK 이고 out 을 채운다. 틀리면 어떤 칸이 틀렸는지를 돌려준다
    CharCreateResult CheckForm(const C2S_CHAR_CREATE& request, CCharacterDb::CreateRequest& out) const;

    void ReplyCreate(SessionId sessionId, CharCreateResult result, CharacterId characterId);
    void ReplyDelete(SessionId sessionId, CharDeleteResult result);
    void ReplyRestore(SessionId sessionId, CharRestoreResult result);
    bool FinishRequest(SessionId sessionId);   // 처리 중 표시를 풀고, 답할 세션이 있으면 true

    CIOCPServer& _network;
    CMonitorManager& _monitor;
    CDbPipeline& _db;
    CCharSessionTable& _sessions;
    CZoneDirectory& _zones;
    const CJobTable& _jobs;
    const CLookTable& _looks;
    CCharacterMetrics& _metrics;
    int _slotCount;
    int _graceDays;
};
