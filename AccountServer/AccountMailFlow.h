#pragma once
// ==========================================================================
// CAccountMailFlow — 계정 우편: 서버 간 포트로 온 요청을 account_mail 문장으로 돌리고 답한다
//
//   요청은 월드 서버 연결로만 받는다(LINK_WORLD_REGISTER 로 가린 세션 · mail-plan.md 9절 「계정 서버 CAccountMailFlow」).
//   존 서버가 월드 서버를 거쳐 보낸다 — 월드 서버는 요청 번호만 바꿔 넘긴다.
//   DB 는 CDbPipeline 에 계정 번호를 열쇠로 넣는다(한 계정의 요청 차례가 안 뒤바뀐다). 전 계정 지급만 열쇠 0 이다.
//   넣기가 거절되면 BUSY 로 답한다. 답 없는 받음 · 풀기는 거절돼도 존 서버가 입장 때 다시 처리로 잇는다(6절).
//
//   이 클래스는 아이템 표 · 플레이어를 모른다. 첨부 바이트는 존 서버가 만든 것을 그대로 담는다.
//   로직 스레드 전용.
// ==========================================================================

#include <Identity/GameIds.h>
#include <Link/LinkPeers.h>

class CAccountMetrics;
class CDbPipeline;
class CMonitorManager;
class CSerialBuffer;

class CAccountMailFlow
{
public:
    // worldSession = 계정 서버가 들고 있는 월드 서버 연결 번호(등록 · 끊김 때 바뀐다) — 참조로 보고 따로 들지 않는다
    CAccountMailFlow(CLinkPeers& peers, CMonitorManager& monitor, CDbPipeline& db, CAccountMetrics& metrics,
                     const SessionId& worldSession, int expireDays)
        : _peers(peers), _monitor(monitor), _db(db), _metrics(metrics), _worldSession(worldSession), _expireDays(expireDays) {}

    void OnCount(SessionId peer, CSerialBuffer* msg);
    void OnList(SessionId peer, CSerialBuffer* msg);
    void OnRead(SessionId peer, CSerialBuffer* msg);
    void OnClaim(SessionId peer, CSerialBuffer* msg);
    void OnDone(SessionId peer, CSerialBuffer* msg);
    void OnUnclaim(SessionId peer, CSerialBuffer* msg);
    void OnErase(SessionId peer, CSerialBuffer* msg);
    void OnSend(SessionId peer, CSerialBuffer* msg);

private:
    // 패킷을 읽고, 월드 서버 연결에서 왔는지 본다. 아니면 패킷 오류로 세고 false
    template <typename TPacket>
    bool Accept(SessionId peer, CSerialBuffer* msg, TPacket& out, const char* what);

    // 형식이 틀린 요청 — 존 서버가 먼저 거르는 값이라 오면 버그다. 답 없이 버린다(존 서버의 시한이 끝낸다)
    void Malformed(SessionId peer, const char* what, int64_t account, int64_t mailId);

    CLinkPeers&       _peers;
    CMonitorManager&  _monitor;
    CDbPipeline&      _db;
    CAccountMetrics&  _metrics;
    const SessionId&  _worldSession;
    int               _expireDays;
};
