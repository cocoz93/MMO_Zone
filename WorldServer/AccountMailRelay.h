#pragma once
// ==========================================================================
// CAccountMailRelay — 계정 우편 중계: 존 서버의 요청을 요청 번호만 바꿔 계정 서버에 넘기고, 답을 되돌려 보낸다
//
//   우편 내용은 모른다 — 패킷을 열어 요청 번호만 바꾼다(mail-plan.md 9절 「월드 서버 CAccountMailRelay」).
//   번호 바꿈 표를 따로 두지 않는다. CLinkRequests 의 done 이 (존 세션, 존의 요청 번호)를 든다.
//
//   [존 서버가 끊김]   치울 것이 없다 — 답이 오면 보낼 세션이 없어 버려지고, 존 서버는 입장 때 다시 처리로 잇는다
//   [계정 서버가 끊김] 대기를 전부 끝내되 존 서버에 아무것도 안 보낸다 — 존 서버의 시한이 끝낸다.
//                      끊긴 뒤 온 요청은 바로 OFFLINE 으로 답한다(9절 「못 보내면 Cancel 후 존에 ACC_OFFLINE」)
//   [답 없는 둘]       받음 · 받는 중 풀기는 넘기기만 한다. 못 넘기면 행이 받는 중으로 남아 그 캐릭터의 입장 때 이어진다
//
//   로직 스레드 전용이라 락이 없다. 지표는 CLinkRequests 의 mmo_link_reply_seconds{request} · mmo_link_timeout_total{request}
// ==========================================================================

#include "IOCPServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서는 이 헤더가 맡는다
#include "MonitorManager.h"

#include <Link/LinkConnector.h>
#include <Link/LinkPeers.h>
#include <Link/LinkRequests.h>
#include <Lobby/PacketTable.h>
#include <sstream>

#include "WorldPresence.h"

class CAccountMailRelay
{
public:
    CAccountMailRelay(CLinkPeers& zones, CLinkConnector& account, CMonitorManager& monitor, const CWorldPresence& presence, int replyTimeoutSec);

    // 존 쪽 요청 여섯 + 답 없는 둘 · 계정 쪽 답 여섯 — false = 번호대 밖이거나 같은 번호를 두 번 등록했다
    bool RegisterRoutes(CPacketTable& zoneTable, CPacketTable& accountTable);

    void Tick(int64_t nowMs);   // 시한이 지난 대기를 지운다 — 존 서버에는 안 보낸다(존 서버도 같은 시한으로 끝낸다)
    void OnAccountDown();       // 대기를 전부 지운다 — 존 서버에는 안 보낸다

    void WriteMetrics(std::ostringstream& ss) const { _requests.WriteMetrics(ss); }   // 지표 스레드

private:
    template <typename TRequest, typename TAck>
    void Forward(SessionId zone, CSerialBuffer* msg, LinkRequestKind kind);
    template <typename TPacket>
    void ForwardOneWay(SessionId zone, CSerialBuffer* msg, const char* what);
    template <typename TAck>
    void Reply(CSerialBuffer* msg);

    CLinkPeers&           _zones;
    CLinkConnector&       _account;
    CMonitorManager&      _monitor;
    const CWorldPresence& _presence;
    CLinkRequests         _requests;
};
