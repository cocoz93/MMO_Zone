#pragma once
// ==========================================================================
// CTokenCheckFlow — 서버 간 포트로 온 토큰 확인 요청에 장부 결과로 답한다
//
//   계정 번호를 웹 패킷으로 받지 않는다 — 캐릭터 서버는 이 답에서만 계정 번호를 얻는다.
//   로직 스레드 전용.
// ==========================================================================

#include <Identity/GameIds.h>
#include <Link/LinkPeers.h>

class CAccountMetrics;
class CAuthTokenLedger;
class CMonitorManager;
class CSerialBuffer;

class CTokenCheckFlow
{
public:
    CTokenCheckFlow(CLinkPeers& peers, CMonitorManager& monitor, CAuthTokenLedger& ledger, CAccountMetrics& metrics)
        : _peers(peers), _monitor(monitor), _ledger(ledger), _metrics(metrics) {}

    void OnCheck(SessionId peer, CSerialBuffer* msg);

private:
    CLinkPeers&       _peers;
    CMonitorManager&  _monitor;
    CAuthTokenLedger& _ledger;
    CAccountMetrics&  _metrics;
};
