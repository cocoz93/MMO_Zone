#pragma once
// ==========================================================================
// CPartyFlow — 파티의 서버 간 패킷: 존 서버의 요청을 규칙(CPartySystem)에 넘기고, 규칙의 알림을 받는 사람의 존 서버로 보낸다
//
//   규칙은 CPartySystem 이다 — 여기는 패킷을 풀고 싸기만 한다.
//   받는 사람의 존 서버는 장부(CWorldPresence)로 찾는다. 장부에 없으면 오프라인이라 보낼 곳이 없다.
//   로직 스레드 전용이라 락이 없다.
// ==========================================================================

#include "IOCPServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서는 이 헤더가 맡는다
#include "MonitorManager.h"

#include <Link/LinkPeers.h>
#include <Lobby/PacketTable.h>

#include "PartySystem.h"
#include "WorldMetrics.h"
#include "WorldPresence.h"

class CPartyFlow : public IPartyEvents
{
public:
    CPartyFlow(CLinkPeers& peers, CMonitorManager& monitor, CWorldPresence& presence, CWorldMetrics& metrics, int64_t restoreSettleMs);

    bool RegisterRoutes(CPacketTable& zoneTable);

    void Online(CharacterId id) { _party.Online(id); }
    void Offline(CharacterId id) { _party.Offline(id); }
    // 옮겨 간 사람의 퇴장 알림이 새 존 서버의 입장 알림보다 늦게 왔다 — 옛 존 서버에 남은 파티원에게 명단을 다시 보낸다
    void ResendToOldZone(SessionId oldZone, CharacterId id);
    void Tick(int64_t nowMs);

    int OnlineMembers() const { return _party.OnlineMembers(); }

private:
    void OnOp(SessionId sessionId, CSerialBuffer* msg);
    void OnState(SessionId sessionId, CSerialBuffer* msg);
    void OnRestore(SessionId sessionId, CSerialBuffer* msg);

    // 파티 알림 — 뜻은 PartySystem.h
    void OnPartyAsk(CharacterId to, uint32_t requestId, PartyAskKind kind, const LinkPartyPerson& from, const LinkPartyPerson& about, uint32_t remainMs) override;
    void OnPartyAskEnd(CharacterId to, uint32_t requestId, PartyNotice why) override;
    void OnPartyNotice(CharacterId to, PartyNotice code, const LinkPartyPerson& who) override;
    void OnPartyInfo(const Party& party) override;
    void OnPartyNone(CharacterId to) override;
    void OnPartyMember(const Party& party, const LinkPartyMemberRow& row) override;

    // 받는 사람의 존 서버 — 접속 중이 아니면 NO_SESSION
    SessionId SessionOf(CharacterId id) const;
    // 이 파티에 접속 중인 사람이 있는 존 서버들 · 그 사람 수
    int ZonesOf(const Party& party, SessionId (&out)[LINK_PARTY_MEMBERS_MAX], int& people) const;
    // 명단 패킷을 싸서 zones 에 보낸다
    void SendInfo(const Party& party, const SessionId* zones, int count);

    CLinkPeers&      _peers;
    CMonitorManager& _monitor;
    CWorldPresence&  _presence;
    CWorldMetrics&   _metrics;
    CPartySystem     _party;
    int64_t          _nowMs = 0;
};
