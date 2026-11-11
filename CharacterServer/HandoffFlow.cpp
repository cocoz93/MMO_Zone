#include "HandoffFlow.h"

#include <Common/ErrorLog.h>
#include <Identity/SecureRandom.h>
#include <Link/LinkPeers.h>
#include <Link/LinkRequests.h>
#include <Lobby/LobbyLoop.h>
#include <Lobby/PacketSend.h>
#include <algorithm>
#include <cstring>

#include "ZoneDirectory.h"

static_assert(LINK_CODE_BYTES == SecureRandom::CODE_WIRE_BYTES, "입장 번호 칸 크기가 어긋났다");

CHandoffFlow::CHandoffFlow(CMonitorManager& monitor, CLinkPeers& peers, CZoneDirectory& zones, CLinkRequests& requests)
    : _monitor(monitor), _peers(peers), _zones(zones), _requests(requests)
{
}

void CHandoffFlow::OnRequest(SessionId sessionId, CSerialBuffer* msg)
{
    if (!_zones.IsRegistered(sessionId))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    LINK_HANDOFF_REQ request;
    if (!PacketSend::Read(msg, request))
    {
        _monitor._packetErrors.Inc();
        return;
    }

    Pending pending;
    pending.requester = sessionId;
    pending.requestId = request.requestId;
    pending.characterId = CharacterId{ request.characterId };
    pending.accountId = AccountId{ request.accountId };

    const CZoneDirectory::Zone* zone = _zones.FindById(ZoneServerId{ request.destZone });
    if (zone == nullptr)
    {
        Reply(pending, LinkHandoffResult::Unknown, 0);
        return;
    }
    pending.destSession = zone->session;
    pending.destWsAddr = zone->wsAddr;
    if (!SecureRandom::MakeCode(pending.enterCode))
    {
        SLOG_ERROR("[Handoff] 입장 번호를 만들지 못했다 (character={})", request.characterId);
        Reply(pending, LinkHandoffResult::Down, 0);
        return;
    }

    LINK_ENTER_NOTICE_REQ notice{};
    notice.characterId = request.characterId;
    notice.accountId = request.accountId;
    std::memcpy(notice.enterCode, pending.enterCode, sizeof(notice.enterCode));
    notice.targetChannelIndex = request.destChannel;

    notice.requestId = _requests.Begin(LinkRequestKind::EnterNotice, CLobbyLoop::NowMs(),
        [this, pending](const void* reply) { OnNoticeReply(pending, static_cast<const LINK_ENTER_NOTICE_ACK*>(reply)); });
    if (!_peers.Send(pending.destSession, notice))
    {
        _requests.Cancel(notice.requestId);
        Reply(pending, LinkHandoffResult::Down, 0);
    }
}

void CHandoffFlow::OnNoticeReply(const Pending& pending, const LINK_ENTER_NOTICE_ACK* ack)
{
    if (ack == nullptr)
    {
        Reply(pending, LinkHandoffResult::Down, 0);
        return;
    }

    switch (static_cast<EnterNoticeResult>(ack->result))
    {
    case EnterNoticeResult::Ok:
        // 입장 대기로 적는다 — 넘어가는 사이 캐릭터 삭제를 막는다
        _zones.AddEntering(pending.destSession, pending.characterId, pending.accountId,
                           CLobbyLoop::NowMs() + int64_t{ ack->ttlSec } * 1000);
        Reply(pending, LinkHandoffResult::Ok, ack->ttlSec);
        return;
    case EnterNoticeResult::Full:
        Reply(pending, LinkHandoffResult::Full, 0);
        return;
    }
    _monitor._packetErrors.Inc();   // 모르는 값 — 와이어에서 온 값이라 switch 밖으로 나올 수 있다
    Reply(pending, LinkHandoffResult::Down, 0);
}

void CHandoffFlow::Reply(const Pending& pending, LinkHandoffResult result, int ttlSec)
{
    LINK_HANDOFF_ACK ack{};
    ack.requestId = pending.requestId;
    ack.result = static_cast<uint8_t>(result);
    ack.ttlSec = ttlSec;
    if (result == LinkHandoffResult::Ok)
    {
        std::memcpy(ack.enterCode, pending.enterCode, sizeof(ack.enterCode));
        const size_t length = (std::min)(pending.destWsAddr.size(), sizeof(ack.wsAddr) - 1);   // 괄호 = 윈도 min 매크로 막기
        std::memcpy(ack.wsAddr, pending.destWsAddr.data(), length);
    }
    _peers.Send(pending.requester, ack);
}
