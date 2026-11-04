#include "SelectFlow.h"

#include <Common/ErrorLog.h>
#include <Db/DbPipeline.h>
#include <Identity/SecureRandom.h>
#include <Link/LinkPeers.h>
#include <Link/LinkRequests.h>
#include <Lobby/LobbyLoop.h>
#include <Lobby/PacketSend.h>
#include <algorithm>
#include <cstring>

#include "CharSessionTable.h"
#include "CharacterDb.h"
#include "CharacterMetrics.h"
#include "ZoneDirectory.h"

static_assert(CHAR_WS_ADDR_BYTES == LINK_WS_ADDR_BYTES, "존 서버 주소 칸 크기가 어긋났다");
static_assert(CHAR_CODE_BYTES == SecureRandom::CODE_WIRE_BYTES, "입장 번호 칸 크기가 어긋났다");

CSelectFlow::CSelectFlow(CIOCPServer& network, CMonitorManager& monitor, CDbPipeline& db, CCharSessionTable& sessions, CZoneDirectory& zones,
                         CLinkPeers& peers, CLinkRequests& requests, CCharacterMetrics& metrics)
    : _network(network), _monitor(monitor), _db(db), _sessions(sessions), _zones(zones), _peers(peers), _requests(requests), _metrics(metrics)
{
}

void CSelectFlow::OnSelect(SessionId sessionId, CSerialBuffer* msg)
{
    C2S_CHAR_SELECT request;
    if (!PacketSend::Read(msg, request))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    if (!_sessions.BeginRequest(sessionId))
    {
        Reply(sessionId, CharSelectResult::BUSY, nullptr);
        return;
    }

    const AccountId accountId = _sessions.Find(sessionId)->account;
    const CharacterId characterId{ request.characterId };
    const bool queued = _db.Run<CharSelectResult>(ToInt(accountId), CCharacterDb::OP_SELECT,
        [accountId, characterId](CDbConnection& conn) { return CCharacterDb::CheckSelect(conn, accountId, characterId); },
        [this, sessionId, accountId, characterId](CharSelectResult& checked) { OnChecked(sessionId, accountId, characterId, checked); });
    if (!queued)
    {
        _sessions.EndRequest(sessionId);
        Reply(sessionId, CharSelectResult::BUSY, nullptr);
    }
}

void CSelectFlow::OnChecked(SessionId sessionId, AccountId accountId, CharacterId characterId, CharSelectResult checked)
{
    if (!CanReply(sessionId))
    {
        _sessions.EndRequest(sessionId);
        return;   // 웹이 먼저 나갔다 — 번호를 만들 일이 없다
    }
    if (checked != CharSelectResult::OK)
    {
        _sessions.EndRequest(sessionId);
        Reply(sessionId, checked, nullptr);
        return;
    }

    const CZoneDirectory::Zone* zone = _zones.PickZone();
    if (zone == nullptr)
    {
        _sessions.EndRequest(sessionId);
        Reply(sessionId, CharSelectResult::ZONE_DOWN, nullptr);
        return;
    }

    Pending pending;
    pending.sessionId = sessionId;
    pending.accountId = accountId;
    pending.characterId = characterId;
    pending.zoneSession = zone->session;
    pending.zoneWsAddr = zone->wsAddr;
    SecureRandom::MakeCode(pending.enterCode);

    LINK_ENTER_NOTICE_REQ notice{};
    notice.characterId = ToInt(characterId);
    notice.accountId = ToInt(accountId);
    std::memcpy(notice.enterCode, pending.enterCode, sizeof(notice.enterCode));

    const SessionId zoneSession = pending.zoneSession;
    notice.requestId = _requests.Begin(LinkRequestKind::EnterNotice, CLobbyLoop::NowMs(),
        [this, pending](const void* reply) { OnNoticeReply(pending, static_cast<const LINK_ENTER_NOTICE_ACK*>(reply)); });
    if (!_peers.Send(zoneSession, notice))
    {
        _requests.Cancel(notice.requestId);
        _sessions.EndRequest(sessionId);
        Reply(sessionId, CharSelectResult::ZONE_DOWN, nullptr);
    }
}

void CSelectFlow::OnNoticeReply(const Pending& pending, const LINK_ENTER_NOTICE_ACK* ack)
{
    _sessions.EndRequest(pending.sessionId);
    if (!CanReply(pending.sessionId))
        return;   // 받을 웹이 없다 — 번호를 아는 쪽이 없으므로 장부에 안 적는다. 존 서버 명단의 번호는 만료로 사라진다

    if (ack == nullptr)
    {
        Reply(pending.sessionId, CharSelectResult::ZONE_DOWN, nullptr);
        return;
    }

    switch (static_cast<EnterNoticeResult>(ack->result))
    {
    case EnterNoticeResult::Ok:
    {
        const int64_t expireMs = CLobbyLoop::NowMs() + int64_t{ ack->ttlSec } * 1000;
        _zones.AddEntering(pending.zoneSession, pending.characterId, pending.accountId, expireMs);
        Reply(pending.sessionId, CharSelectResult::OK, &pending);
        return;
    }
    case EnterNoticeResult::Full:
        Reply(pending.sessionId, CharSelectResult::ZONE_FULL, nullptr);
        return;
    }

    _monitor._packetErrors.Inc();   // 모르는 값 — 와이어에서 온 값이라 switch 밖으로 나올 수 있다
    Reply(pending.sessionId, CharSelectResult::ZONE_DOWN, nullptr);
}

bool CSelectFlow::CanReply(SessionId sessionId)
{
    const CCharSessionTable::Entry* entry = _sessions.Find(sessionId);
    return entry != nullptr && entry->stage == CCharSessionTable::Stage::LoggedIn;
}

void CSelectFlow::Reply(SessionId sessionId, CharSelectResult result, const Pending* granted)
{
    S2C_CHAR_SELECT reply{};
    reply.result = static_cast<uint8_t>(result);
    if (granted != nullptr)
    {
        std::memcpy(reply.enterCode, granted->enterCode, sizeof(reply.enterCode));
        const size_t length = (std::min)(granted->zoneWsAddr.size(), sizeof(reply.zoneWsAddr) - 1);   // 괄호 = 윈도 min 매크로 막기
        std::memcpy(reply.zoneWsAddr, granted->zoneWsAddr.data(), length);
    }
    PacketSend::Send(_network, _monitor, sessionId, reply);
    _metrics.Count(result);
}
