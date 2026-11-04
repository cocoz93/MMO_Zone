#include "RosterFlow.h"

#include <Common/ErrorLog.h>
#include <Data/JobTable.h>
#include <Db/DbPipeline.h>
#include <Lobby/LobbyLoop.h>
#include <Lobby/PacketSend.h>
#include <cstring>

#include "CharListSend.h"
#include "CharSessionTable.h"
#include "CharacterMetrics.h"
#include "CharacterRules.h"
#include "LookTable.h"
#include "ZoneDirectory.h"

CRosterFlow::CRosterFlow(CIOCPServer& network, CMonitorManager& monitor, CDbPipeline& db, CCharSessionTable& sessions, CZoneDirectory& zones,
                         const CJobTable& jobs, const CLookTable& looks, CCharacterMetrics& metrics, int slotCount, int graceDays)
    : _network(network), _monitor(monitor), _db(db), _sessions(sessions), _zones(zones)
    , _jobs(jobs), _looks(looks), _metrics(metrics), _slotCount(slotCount), _graceDays(graceDays)
{
}

CharCreateResult CRosterFlow::CheckForm(const C2S_CHAR_CREATE& request, CCharacterDb::CreateRequest& out) const
{
    if (!CCharacterRules::IsValidSlot(request.slot, _slotCount))
        return CharCreateResult::BAD_FORM;

    std::memcpy(out.name, request.name, sizeof(out.name));   // 패킷 칸은 줄이 안 맞아 참조로 못 넘긴다
    if (!CharacterName::IsValid(out.name))
        return CharCreateResult::BAD_FORM;

    if (!_jobs.Has(request.job))
        return CharCreateResult::BAD_JOB;

    CharacterLookBlob::Look look;
    if (!CharacterLookBlob::Read(request.look, CHAR_LOOK_BYTES, look) || !_looks.IsAllowed(look))
        return CharCreateResult::BAD_LOOK;

    out.slot = request.slot;
    out.job = request.job;
    CharacterLookBlob::Write(look, out.look);   // 받은 칸을 그대로 넣지 않는다 — 항상 지금 판으로 적는다
    return CharCreateResult::OK;
}

void CRosterFlow::OnCreate(SessionId sessionId, CSerialBuffer* msg)
{
    C2S_CHAR_CREATE request;
    if (!PacketSend::Read(msg, request))
    {
        _monitor._packetErrors.Inc();
        return;
    }

    CCharacterDb::CreateRequest job;
    const CharCreateResult form = CheckForm(request, job);
    if (form != CharCreateResult::OK)
    {
        ReplyCreate(sessionId, form, NO_CHARACTER);
        return;
    }

    if (!_sessions.BeginRequest(sessionId))
    {
        ReplyCreate(sessionId, CharCreateResult::BUSY, NO_CHARACTER);
        return;
    }

    job.account = _sessions.Find(sessionId)->account;
    const int graceDays = _graceDays;
    const bool queued = _db.Run<CCharacterDb::CreateOutcome>(ToInt(job.account), CCharacterDb::OP_CREATE,
        [job, graceDays](CDbConnection& conn) { return CCharacterDb::Create(conn, job, graceDays); },
        [this, sessionId](CCharacterDb::CreateOutcome& outcome)
        {
            const bool here = FinishRequest(sessionId);
            if (outcome.result == CharCreateResult::OK)
                SLOG_INFO("[Roster] created character={} (reply={})", ToInt(outcome.characterId), here);
            if (!here)
            {
                _metrics.Count(outcome.result);
                return;
            }
            ReplyCreate(sessionId, outcome.result, outcome.characterId);
            if (outcome.result == CharCreateResult::OK && outcome.list.ok)
                CharListSend::Send(_network, _monitor, sessionId, outcome.list.rows);
        });
    if (!queued)
    {
        _sessions.EndRequest(sessionId);
        ReplyCreate(sessionId, CharCreateResult::BUSY, NO_CHARACTER);
    }
}

void CRosterFlow::OnDelete(SessionId sessionId, CSerialBuffer* msg)
{
    C2S_CHAR_DELETE request;
    if (!PacketSend::Read(msg, request))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    if (!_sessions.BeginRequest(sessionId))
    {
        ReplyDelete(sessionId, CharDeleteResult::BUSY);
        return;
    }

    const AccountId accountId = _sessions.Find(sessionId)->account;
    const CharacterId characterId{ request.characterId };
    const bool inGame = _zones.IsInGame(characterId, CLobbyLoop::NowMs());
    const int graceDays = _graceDays;
    const bool queued = _db.Run<CCharacterDb::DeleteOutcome>(ToInt(accountId), CCharacterDb::OP_DELETE,
        [accountId, characterId, inGame, graceDays](CDbConnection& conn) { return CCharacterDb::Delete(conn, accountId, characterId, inGame, graceDays); },
        [this, sessionId](CCharacterDb::DeleteOutcome& outcome)
        {
            if (!FinishRequest(sessionId))
            {
                _metrics.Count(outcome.result);
                return;
            }
            ReplyDelete(sessionId, outcome.result);
            if (outcome.result == CharDeleteResult::OK && outcome.list.ok)
                CharListSend::Send(_network, _monitor, sessionId, outcome.list.rows);
        });
    if (!queued)
    {
        _sessions.EndRequest(sessionId);
        ReplyDelete(sessionId, CharDeleteResult::BUSY);
    }
}

void CRosterFlow::OnRestore(SessionId sessionId, CSerialBuffer* msg)
{
    C2S_CHAR_RESTORE request;
    if (!PacketSend::Read(msg, request))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    if (!_sessions.BeginRequest(sessionId))
    {
        ReplyRestore(sessionId, CharRestoreResult::BUSY);
        return;
    }

    const AccountId accountId = _sessions.Find(sessionId)->account;
    const CharacterId characterId{ request.characterId };
    const int graceDays = _graceDays;
    const bool queued = _db.Run<CCharacterDb::RestoreOutcome>(ToInt(accountId), CCharacterDb::OP_RESTORE,
        [accountId, characterId, graceDays](CDbConnection& conn) { return CCharacterDb::Restore(conn, accountId, characterId, graceDays); },
        [this, sessionId](CCharacterDb::RestoreOutcome& outcome)
        {
            if (!FinishRequest(sessionId))
            {
                _metrics.Count(outcome.result);
                return;
            }
            ReplyRestore(sessionId, outcome.result);
            if (outcome.result == CharRestoreResult::OK && outcome.list.ok)
                CharListSend::Send(_network, _monitor, sessionId, outcome.list.rows);
        });
    if (!queued)
    {
        _sessions.EndRequest(sessionId);
        ReplyRestore(sessionId, CharRestoreResult::BUSY);
    }
}

bool CRosterFlow::FinishRequest(SessionId sessionId)
{
    _sessions.EndRequest(sessionId);
    const CCharSessionTable::Entry* entry = _sessions.Find(sessionId);
    return entry != nullptr && entry->stage == CCharSessionTable::Stage::LoggedIn;
}

void CRosterFlow::ReplyCreate(SessionId sessionId, CharCreateResult result, CharacterId characterId)
{
    S2C_CHAR_CREATE reply{};
    reply.result = static_cast<uint8_t>(result);
    reply.characterId = ToInt(characterId);
    PacketSend::Send(_network, _monitor, sessionId, reply);
    _metrics.Count(result);
}

void CRosterFlow::ReplyDelete(SessionId sessionId, CharDeleteResult result)
{
    S2C_CHAR_DELETE reply{};
    reply.result = static_cast<uint8_t>(result);
    PacketSend::Send(_network, _monitor, sessionId, reply);
    _metrics.Count(result);
}

void CRosterFlow::ReplyRestore(SessionId sessionId, CharRestoreResult result)
{
    S2C_CHAR_RESTORE reply{};
    reply.result = static_cast<uint8_t>(result);
    PacketSend::Send(_network, _monitor, sessionId, reply);
    _metrics.Count(result);
}
