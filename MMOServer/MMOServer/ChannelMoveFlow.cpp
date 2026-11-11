#include "ChannelMoveFlow.h"

#if USE_DB_WORKER

#include <Common/ErrorLog.h>
#include <Protocol/Protocol.h>
#include <algorithm>

#include "CharacterStore.h"
#include "EnterGate.h"
#include "MapManager.h"
#include "PacketBuild.h"
#include "PacketRoutes.h"
#include "Player.h"
#include "PlayerPlacement.h"
#include "SerialBuffer.h"
#include "SessionHub.h"
#include "SightBroadcaster.h"
#if USE_TRADE
#include "TradeSystem.h"
#endif
#include "Zone.h"
#include "ZoneLink.h"

namespace
{
    // 내보낸 뒤 세션을 닫기까지 — 코어가 S2C_CHANNEL_MOVE 를 보낼 틈이다. 틱 하나(40ms)의 수십 배면 넉넉하다
    constexpr int64_t CLOSE_DELAY_MS = 1000;

    static_assert(CHANNEL_WS_ADDR_BYTES == LINK_WS_ADDR_BYTES, "웹 주소 칸 크기가 어긋났다");
    static_assert(sizeof(MSG_S2C_CHANNEL_MOVE::enterCode) == LINK_CODE_BYTES, "입장 번호 칸 크기가 어긋났다");

    // S2C_ZONE_CHANGE_FAIL 의 사유 번호(Protocol.h)
    uint8_t ReasonOf(ChannelMoveResult result)
    {
        static_assert(CHANNEL_MOVE_RESULT_COUNT == 11, "ChannelMoveResult 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
        switch (result)
        {
        case ChannelMoveResult::NOT_FOUND:   return 0;
        case ChannelMoveResult::FULL:        return 1;
        case ChannelMoveResult::SAME_ZONE:   return 2;
        case ChannelMoveResult::NOT_ALLOWED: return 3;
        case ChannelMoveResult::BUSY:        return 6;
        case ChannelMoveResult::DEAD:        return 7;
        case ChannelMoveResult::NOT_HERE:    return 8;
        case ChannelMoveResult::SAVE_FAILED: return 9;
        case ChannelMoveResult::TIMEOUT:     return 9;
        case ChannelMoveResult::ZONE_DOWN:   return 10;
        case ChannelMoveResult::OK:
        case ChannelMoveResult::COUNT:       break;
        }
        return 10;
    }
}

void CChannelMoveFlow::Configure(int zoneServerId, int channelBlock)
{
    _zoneServerId = zoneServerId;
    _channelBlock = channelBlock;
}

bool CChannelMoveFlow::RegisterRoutes(CPacketRoutes& routes)
{
    return routes.OnPlayer(MsgType::C2S_CHANNEL_MOVE_REQ, sizeof(MSG_C2S_CHANNEL_MOVE_REQ),
                           [this](CPlayer* p, CSerialBuffer* m) { RecvMoveReq(p, m); });
}

ChannelMoveResult CChannelMoveFlow::Check(const CPlayer* player, int32_t visibleNo, ChannelNumber::Place& out)
{
    if (_mapManager.FindFieldMap(CMapManager::GetMapIdFromZoneId(player->_zoneId)) == nullptr)
        return ChannelMoveResult::NOT_HERE;      // 방 맵 — 번호에 뜻이 없다
    if (player->_accountId == NO_ACCOUNT)
        return ChannelMoveResult::NOT_ALLOWED;   // 시험 입장 길 — 입장 번호를 받을 계정이 없다
#if USE_COMBAT
    if (player->_health.IsDead())
        return ChannelMoveResult::DEAD;
#endif
    if (player->_dbFlight.paired)
        return ChannelMoveResult::BUSY;          // 거래는 끝났어도 짝 묶음이 확정 전이다
#if USE_TRADE
    if (_tradeSystem.Busy(player))
        return ChannelMoveResult::BUSY;
#endif
    if (!ChannelNumber::FromVisible(visibleNo, _channelBlock, out))
        return ChannelMoveResult::NOT_FOUND;
    if (out.zoneServerId == _zoneServerId)
        return ChannelMoveResult::SAME_ZONE;     // 같은 존 서버 안은 C2S_ZONE_CHANGE 의 길이다
    return ChannelMoveResult::OK;
}

// 걷는 중이면 멈춘다 — 입력을 막으면 멈춤 패킷이 안 들어와, 저장한 자리와 달라진다
void CChannelMoveFlow::StopWalking(CPlayer* player)
{
    if (player->_moveState != MoveState::MOVING)
        return;
    player->_moveState = MoveState::IDLE;
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;
    zone->UpdateSectorAndQueue(player);
    _sight.NotifyMoveStop(zone, player, false);
}

void CChannelMoveFlow::RecvMoveReq(CPlayer* player, CSerialBuffer* msg)
{
    MSG_C2S_CHANNEL_MOVE_REQ req;
    if (msg->PeekData(reinterpret_cast<char*>(&req), sizeof(req)) != static_cast<int>(sizeof(req)))
        return;

    ChannelNumber::Place dest;
    const ChannelMoveResult reject = Check(player, req.visibleNo, dest);
    if (reject != ChannelMoveResult::OK)
    {
        Fail(player, reject);
        return;
    }

    StopWalking(player);
    player->_moveOp = CPlayer::ChannelMoveOp::SAVING;
    player->_dbDirty = true;   // 서 있어 바뀐 값이 없어도 묶음이 생기게 — 회신이 있어야 다음으로 간다
    player->_dbFlight.handoffWanted = true;
    _store.RequestSave(player, SaveReason::HANDOFF);
    _saving.push_back(Saving{ player->_sessionId, dest, CEnterGate::NowMs() + _timeoutMs });
}

void CChannelMoveFlow::OnSaved(CPlayer* player, bool ok)
{
    const SessionId sessionId = player->_sessionId;
    const auto it = std::find_if(_saving.begin(), _saving.end(), [&](const Saving& s) { return s.sessionId == sessionId; });
    if (it == _saving.end() || player->_moveOp != CPlayer::ChannelMoveOp::SAVING)
        return;
    const ChannelNumber::Place dest = it->dest;
    _saving.erase(it);

    if (!ok)
    {
        Fail(player, ChannelMoveResult::SAVE_FAILED);
        return;
    }

    player->_moveOp = CPlayer::ChannelMoveOp::ASKING;
    const CharacterId characterId = player->_characterId;
    const uint64_t requestId = _requests.Begin(LinkRequestKind::Handoff, CEnterGate::NowMs(),
        [this, sessionId, characterId, dest](const void* reply) { OnAskDone(sessionId, characterId, dest, static_cast<const LINK_HANDOFF_ACK*>(reply)); });
    if (!_zoneLink.SendHandoff(requestId, characterId, player->_accountId, dest.zoneServerId, dest.channelIndex))
    {
        _requests.Cancel(requestId);
        Fail(player, ChannelMoveResult::ZONE_DOWN);
    }
}

void CChannelMoveFlow::OnCharacterDown()
{
    _requests.FailAll(LinkRequestKind::Handoff);
}

void CChannelMoveFlow::OnHandoffAck(const LINK_HANDOFF_ACK& ack, int64_t nowMs)
{
    _requests.Complete(ack.requestId, nowMs, &ack);   // 시한이 지난 뒤 온 답은 짝이 없어 버려진다. 번호는 목적지에서 만료로 사라진다
}

void CChannelMoveFlow::OnAskDone(SessionId sessionId, CharacterId characterId, ChannelNumber::Place dest, const LINK_HANDOFF_ACK* ack)
{
    CPlayer* player = _sessionHub.FindPlayer(sessionId);
    if (player == nullptr || player->_characterId != characterId || player->_moveOp != CPlayer::ChannelMoveOp::ASKING)
        return;   // 답이 오기 전에 나갔다

    if (ack == nullptr)
    {
        Fail(player, ChannelMoveResult::ZONE_DOWN);
        return;
    }

    switch (static_cast<LinkHandoffResult>(ack->result))
    {
    case LinkHandoffResult::Ok:
        SendMove(player, *ack);
        _gameMonitor._channelMove._result[static_cast<int>(ChannelMoveResult::OK)].Inc();
        SLOG_INFO("[CHANNEL] handoff character={} zone={} channel={}", characterId, dest.zoneServerId, dest.channelIndex);
        _placement.HandOffGame(player);   // ★ 여기서 player 가 해제된다
        _closing.push_back(Closing{ sessionId, CEnterGate::NowMs() + CLOSE_DELAY_MS });
        return;
    case LinkHandoffResult::Full:
        Fail(player, ChannelMoveResult::FULL);
        return;
    case LinkHandoffResult::Down:
    case LinkHandoffResult::Unknown:
        Fail(player, ChannelMoveResult::ZONE_DOWN);
        return;
    }
    Fail(player, ChannelMoveResult::ZONE_DOWN);   // 모르는 값 — 와이어에서 온 값이라 switch 밖으로 나올 수 있다
}

void CChannelMoveFlow::Fail(CPlayer* player, ChannelMoveResult result)
{
    player->_moveOp = CPlayer::ChannelMoveOp::IDLE;
    player->_dbFlight.handoffWanted = false;
    player->_dbFlight.handoffBundleNo = 0;
    _gameMonitor._channelMove._result[static_cast<int>(result)].Inc();
    _placement.SendZoneChangeFail(player, ReasonOf(result));
}

void CChannelMoveFlow::SendMove(CPlayer* player, const LINK_HANDOFF_ACK& ack)
{
    LINK_HANDOFF_ACK copy = ack;   // SetData 가 char* 를 받는다
    CSerialBuffer* buf = CSerialBuffer::Alloc();
    PacketBuild::BeginPacket(buf, MSG_S2C_CHANNEL_MOVE::TYPE);
    buf->SetData(copy.wsAddr, sizeof(copy.wsAddr));
    buf->SetData(copy.enterCode, sizeof(copy.enterCode));
    PacketBuild::FinalizePacket(buf);
    _sessionHub.SendPacket(player, buf);
}

void CChannelMoveFlow::Tick(int64_t nowMs)
{
    for (size_t i = 0; i < _saving.size();)
    {
        if (nowMs < _saving[i].deadlineMs)
        {
            ++i;
            continue;
        }
        CPlayer* player = _sessionHub.FindPlayer(_saving[i].sessionId);
        if (player != nullptr && player->_moveOp == CPlayer::ChannelMoveOp::SAVING)
            Fail(player, ChannelMoveResult::TIMEOUT);
        _saving[i] = _saving.back();
        _saving.pop_back();
    }

    for (size_t i = 0; i < _closing.size();)
    {
        if (nowMs < _closing[i].dueMs)
        {
            ++i;
            continue;
        }
        _sessionHub.RequestDisconnect(_closing[i].sessionId);
        _closing[i] = _closing.back();
        _closing.pop_back();
    }
}

#endif // USE_DB_WORKER
