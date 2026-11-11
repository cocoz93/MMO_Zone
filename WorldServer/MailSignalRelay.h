#pragma once
// ==========================================================================
// CMailSignalRelay — 우편 도착 신호 둘을 존 서버로 넘긴다(캐릭터 우편 = 받는 사람의 존 서버 · 계정 우편 = 전부) (world-server-plan.md 5.6 · 9절 7단계)
//
//   캐릭터 우편(LINK_MAIL_ARRIVED)  — 보낸 사람의 존 서버가 보낸다. 접속 장부로 받는 사람의 존 서버를 찾아 그 한 곳에만 넘긴다.
//                                     장부에 없으면(접속 중이 아님) 버린다 — 그 사람은 입장 때 수를 새로 받는다.
//   계정 우편(LINK_ACCMAIL_ARRIVED) — 계정 서버가 지급을 넣은 뒤 보낸다. 등록된 존 서버 전부에 넘긴다(5.6 「존 서버 전부」).
//                                     사람은 존 서버가 계정 번호로 찾는다(0 = 접속 중인 전원).
//   신호에는 답이 없다. 못 넘기면(그새 끊김) 그만이다 — 수는 입장 때와 목록을 받을 때 다시 맞춰진다(mail-plan.md 10절).
//
//   로직 스레드 전용이라 락이 없다. 지표는 지표 스레드가 원자 값만 읽는다.
// ==========================================================================

#include "IOCPServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서는 이 헤더가 맡는다
#include "MonitorManager.h"

#include <Link/LinkPeers.h>
#include <Lobby/PacketTable.h>
#include <atomic>
#include <cstdint>
#include <sstream>

#include "WorldPresence.h"

class CMailSignalRelay
{
public:
    CMailSignalRelay(CLinkPeers& zones, CMonitorManager& monitor, const CWorldPresence& presence);

    // 존 쪽 캐릭터 신호 · 계정 쪽 계정 신호 — false = 번호대 밖이거나 같은 번호를 두 번 등록했다
    bool RegisterRoutes(CPacketTable& zoneTable, CPacketTable& accountTable);

    void WriteMetrics(std::ostringstream& ss) const;   // 지표 스레드

private:
    // 지표 꼬리표 signal 의 값 — 칸 번호가 곧 배열 자리다
    enum Signal : int { CHARACTER = 0, ACCOUNT = 1, SIGNAL_COUNT = 2 };

    void OnMailArrived(SessionId zone, CSerialBuffer* msg);
    void OnAccMailArrived(CSerialBuffer* msg);

    CLinkPeers&           _zones;
    CMonitorManager&      _monitor;
    const CWorldPresence& _presence;

    // 대조(조용해진 뒤) — 캐릭터: received = forwarded + offline + sendFailed · 계정: forwarded + sendFailed = Σ(받은 신호마다 그때 등록된 존 서버 수)
    std::atomic<int64_t> _received[SIGNAL_COUNT]{};
    std::atomic<int64_t> _forwarded[SIGNAL_COUNT]{};    // 존 서버로 넘긴 패킷 수
    std::atomic<int64_t> _sendFailed[SIGNAL_COUNT]{};   // 넘기려 했으나 보내기가 실패(그새 끊김)
    std::atomic<int64_t> _offline{ 0 };                 // 캐릭터 신호 — 받는 사람이 장부에 없음(접속 중이 아님)
};
