#include "PartyHandler.h"

#if USE_PARTY

#include "SessionHub.h"
#include "PlayerPlacement.h"
#include "MapManager.h"
#include "Zone.h"
#include "Player.h"
#include "PortalTable.h"     // PortalDef — 방 맵 포털(목적지 맵 · 도착 자리)
#include "GameMonitor.h"
#include "PacketRoutes.h"
#include "SerialBuffer.h"
#include <Protocol/Protocol.h>
#include <Common/ErrorLog.h>
#include <cassert>
#include "PacketBuild.h" // 머리 쓰기·닫기 · 파티 사람 조각·파티원 한 줄(WritePartyPerson · WritePartyMember) — 게임 서버 나누기 0-②

using namespace PacketBuild;   // 아래 조립 함수들이 머리 쓰기·닫기를 이름만으로 부른다

// ==========================================================================
// 파티 S2C 조립 — 이 조각만 부른다(GameServer.cpp 에서 옮김 · 몸·주석 그대로).
//   쓰는 법(스칼라는 << · 고정배열·문자열은 SetData(raw) · pack(1))은 GameServer.cpp 머리의 「S2C 직렬화 헬퍼」 주석과 같다.
// ==========================================================================
namespace
{
    // 파티 사람 조각·파티원 한 줄(WritePartyPerson · WritePartyMember)은 PacketBuild.h — 파티 패킷 여럿이 몸통에 끼워 쓴다

    CSerialBuffer* MakePartyAsk(uint32_t requestId, PartyAskKind kind, const CPlayer* from, const CPlayer* about, uint32_t remainMs)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_PARTY_ASK::TYPE);
        *buf << static_cast<uint32_t>(requestId);
        *buf << static_cast<uint8_t>(kind);
        WritePartyPerson(buf, from);
        WritePartyPerson(buf, about);
        *buf << static_cast<uint32_t>(remainMs);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_PARTY_ASK));
        return buf;
    }

    CSerialBuffer* MakePartyAskEnd(uint32_t requestId, PartyNotice why)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_PARTY_ASK_END::TYPE);
        *buf << static_cast<uint32_t>(requestId);
        *buf << static_cast<uint8_t>(why);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_PARTY_ASK_END));
        return buf;
    }

    CSerialBuffer* MakePartyNotice(PartyNotice code, int32_t actorId)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_PARTY_NOTICE::TYPE);
        *buf << static_cast<uint8_t>(code);
        *buf << static_cast<int>(actorId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_PARTY_NOTICE));
        return buf;
    }

    // 파티 전체는 한 장으로 간다 — 꽉 찬 파티(8명)도 한 패킷에 들어가야 한다(가방 전체와 같은 단언, SelfNotifier.cpp MakeInventory 앞).
    static_assert(sizeof(MSG_S2C_PARTY_INFO) <= MAX_PACKET_SIZE,
                  "꽉 찬 파티 전체가 한 패킷(MAX_PACKET_SIZE)에 안 들어간다 — 나눠 보내는 길을 만들 것");

    // 파티 전체 — 가변 길이(가입 순서 줄만). party 가 nullptr 이면 파티 없음(번호·파티장 0 · 인원 0 — 머리 13B 뿐).
    CSerialBuffer* MakePartyInfo(const Party* party)
    {
        const int count = (party != nullptr) ? static_cast<int>(party->members.size()) : 0;

        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_PARTY_INFO::TYPE);
        *buf << static_cast<int>(party != nullptr ? party->id : 0);
        *buf << static_cast<int>(party != nullptr ? party->leader->_actorId : 0);
        *buf << static_cast<uint8_t>(count);
        if (party != nullptr)
        {
            for (const Party::Member& m : party->members)
                WritePartyMember(buf, m.player);
        }
        FinalizePacket(buf);
        assert(buf->GetDataSize() ==
            offsetof(MSG_S2C_PARTY_INFO, entries) + static_cast<size_t>(count) * sizeof(PartyMemberEntry));
        return buf;
    }

    CSerialBuffer* MakePartyMember(const CPlayer* member)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_PARTY_MEMBER::TYPE);
        WritePartyMember(buf, member);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_PARTY_MEMBER));
        return buf;
    }
}

// 받은 패킷 등록 — 옛 등록의 함정 주석을 그대로 옮겼다(게임 서버 나누기 0-④ 의 등록에서 이 조각 몫 여섯 · 7 단계).
bool CPartyHandler::RegisterRoutes(CPacketRoutes& routes)
{
    bool ok = true;
    // 이 여섯 줄이 없으면 파티 요청이 조용히 버려진다(등록 안 된 번호는 OnReceived 가 조용히 버린다 — 답도 로그도 없다)
    ok &= routes.OnPlayer(MsgType::C2S_PARTY_REQUEST, sizeof(MSG_C2S_PARTY_REQUEST), [this](CPlayer* p, CSerialBuffer* m) { RecvPartyRequest(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_PARTY_ANSWER,  sizeof(MSG_C2S_PARTY_ANSWER),  [this](CPlayer* p, CSerialBuffer* m) { RecvPartyAnswer(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_PARTY_LEAVE,   sizeof(MSG_C2S_PARTY_LEAVE),   // 몸 없음 — 헤더뿐
                           [this](CPlayer* p, CSerialBuffer*) { RecvPartyLeave(p); });
    ok &= routes.OnPlayer(MsgType::C2S_PARTY_KICK,    sizeof(MSG_C2S_PARTY_KICK),    [this](CPlayer* p, CSerialBuffer* m) { RecvPartyKick(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_PARTY_LEADER,  sizeof(MSG_C2S_PARTY_LEADER),  [this](CPlayer* p, CSerialBuffer* m) { RecvPartyLeader(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_PARTY_DISBAND, sizeof(MSG_C2S_PARTY_DISBAND), // 몸 없음 — 헤더뿐
                           [this](CPlayer* p, CSerialBuffer*) { RecvPartyDisband(p); });
    return ok;
}

// 방 맵 포털(PartyHandler.h 선언 주석 · Room.h). 판정은 순수 함수 하나, 와이어로 바꾸기는 SendRoomReject 한 곳.
void CPartyHandler::EnterRoom(CPlayer* leader, CZone* oldZone, const PortalDef& portal)
{
    auto& pc = _gameMonitor._portal;
    auto& rc = _gameMonitor._room;

    const Party* party = _partySystem.FindParty(leader);
    RoomEntryResult result = CheckRoomEntry(leader, party, _roomLocks, portal.targetMapId, _frameCount);

    CZone* room = nullptr;
    if (result.reject == RoomEntryReject::NONE)
    {
        room = _mapManager.CreateRoom(portal.targetMapId);
        if (room == nullptr)
            result.reject = RoomEntryReject::FULL;   // 번호가 다 찼다(CMapInstance::CreateRoom)
    }

    rc._entries[static_cast<int>(result.reject)].Inc();
    if (result.reject != RoomEntryReject::NONE)
    {
        pc._rejected[static_cast<int>(PortalReject::ROOM)].Inc();
        SendRoomReject(leader, result);
        return;
    }

    // 옮길 순서 = 파티장, 그다음 가입 순서. 목록을 먼저 떠 둔다 — 옮기는 동안 파티는 안 바뀌지만(TransferPlayer 는 파티를 모른다)
    //   돌고 있는 목록의 주인이 옮기는 쪽 밖에 있다. 판정이 「전원 같은 존」을 봤으니 옮겨지는 사람의 옛 존은 모두 oldZone 이다
    CPlayer* order[PARTY_MAX_MEMBERS];
    int32_t count = 0;
    order[count++] = leader;
    for (const Party::Member& m : party->members)
    {
        if (m.player != leader)
            order[count++] = m.player;
    }

    pc._used.Inc();
    rc._active.Inc();
    const CPlayerPlacement::SpawnAt at{ static_cast<float>(portal.targetX), static_cast<float>(portal.targetY) };
    for (int32_t i = 0; i < count; ++i)
    {
        assert(order[i]->_zoneId == oldZone->GetZoneId() && "판정이 같은 존을 봤다 — CheckRoomEntry NOT_GATHERED");
        _placement.TransferPlayer(order[i], oldZone, room, CPlayerPlacement::ArrivalKind::ZONE_CHANGE, &at);
        rc._moved.Inc();
    }
    SLOG_INFO("[Room] map {} zone {} — party {} · {} moved (leader actor {})",
              portal.targetMapId, room->GetZoneId(), party->id, count, leader->_actorId);
}

void CPartyHandler::SendRoomReject(CPlayer* leader, const RoomEntryResult& result)
{
    const int32_t blocker = (result.blocker != nullptr) ? result.blocker->_actorId : 0;
    switch (result.reject)
    {
    case RoomEntryReject::NOT_IN_PARTY: _partySystem.Notice(leader, PartyNotice::NOT_IN_PARTY, 0);            return;
    case RoomEntryReject::NOT_LEADER:   _partySystem.Notice(leader, PartyNotice::NOT_LEADER, 0);              return;
    case RoomEntryReject::NOT_GATHERED: _partySystem.Notice(leader, PartyNotice::ROOM_NOT_GATHERED, blocker); return;
    case RoomEntryReject::DEAD:         _partySystem.Notice(leader, PartyNotice::ROOM_DEAD, blocker);         return;
    case RoomEntryReject::LOCKED:       _partySystem.Notice(leader, PartyNotice::ROOM_LOCKED, blocker);       return;
    case RoomEntryReject::FULL:         _placement.SendZoneChangeFail(leader, 1);                                        return;   // 1: 모든 채널 가득 참
    case RoomEntryReject::NONE:
    case RoomEntryReject::COUNT:        break;
    }
    assert(false && "SendRoomReject 는 거절만 받는다");
}

// 방 다시 들어가기 잠금 넣기(PartyHandler.h 선언 주석) — 방 보상(RewardRoom)이 사람마다 부르던 Lock 과 그 뒤 게이지 한 줄을 여기 한 곳으로(게임 서버 나누기 7 단계)
void CPartyHandler::LockRoomReentry(int64_t accountId, int32_t mapId, uint64_t untilFrame)
{
    _roomLocks.Lock(accountId, mapId, untilFrame);
    _gameMonitor._room._locks.Store(static_cast<int64_t>(_roomLocks.Size()));
}

// 게임 루프 3-2c(파티 규칙의 틱 뒤)에 있던 몸 그대로(게임 서버 나누기 7 단계)
void CPartyHandler::TickRoomLocks()
{
    // 방 다시 들어가기 잠금 — 풀린 것 빼기(초마다). 판정은 풀리는 틱을 직접 보므로 빼기가 늦어도 결과는 같다 — 표 크기만 묶는다
    if (_frameCount % FRAME_PER_SEC == 0)
    {
        _roomLocks.Sweep(_frameCount);
        _gameMonitor._room._locks.Store(static_cast<int64_t>(_roomLocks.Size()));
    }
}

// 게임 루프 5(빈 채널 정리)의 콜백에 있던 몸 그대로(게임 서버 나누기 14 단계)
void CPartyHandler::OnErasingZone(CZone* zone)
{
    // 빈 방 — 열린 방 수를 내린다(등식: 들어감 = 닫힘 + 열림 — GameMonitor.h _room). 방 번호는 이 뒤 다시 쓸 수 있다(CMapInstance::CreateRoom)
    if (_mapManager.IsRoomMap(zone->GetMapId()))
    {
        _gameMonitor._room._active.Add(-1);
        _gameMonitor._room._closed.Inc();
    }
}

// ==========================================================================
// 파티 — 규칙은 CPartySystem(PartySystem.cpp), 여기는 받기와 알림을 패킷으로 바꿔 보내기만
// ==========================================================================

// 화면에서 누른 캐릭터 — 내 주변 9섹터에서 찾아 넘긴다(없으면 nullptr → 규칙이 NO_TARGET 으로 답한다)
void CPartyHandler::RecvPartyRequest(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_PARTY_REQUEST recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    _partySystem.Request(player, FindPlayerAround(player, recvMsg.targetId), _frameCount);
}

void CPartyHandler::RecvPartyAnswer(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_PARTY_ANSWER recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    _partySystem.Answer(player, recvMsg.requestId, recvMsg.accept != 0, _frameCount);
}

void CPartyHandler::RecvPartyLeave(CPlayer* player)
{
    _partySystem.Leave(player);
}

// 내보내기·넘기기 — 대상은 우리 파티원 중에서 찾는다(맵이 달라도 된다 — 화면에서 누르지 않고 파티 창에서 고른다)
void CPartyHandler::RecvPartyKick(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_PARTY_KICK recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    _partySystem.Kick(player, recvMsg.targetId);
}

void CPartyHandler::RecvPartyLeader(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_PARTY_LEADER recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    _partySystem.SetLeader(player, recvMsg.targetId);
}

void CPartyHandler::RecvPartyDisband(CPlayer* player)
{
    _partySystem.Disband(player);
}

CPlayer* CPartyHandler::FindPlayerAround(CPlayer* player, int32_t actorId)
{
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return nullptr;

    // 어느 9칸인가(시야 규칙)는 섹터 관리자가 정한다(GetAroundSectorList) — 사람 목록만 본다(몬스터를 눌러도 NO_TARGET)
    const CSectorManager& sectors = zone->GetSectorManager();
    CSectorManager::SectorPos around[CSectorManager::MAX_AROUND_SECTORS];
    int32_t aroundCount = 0;
    sectors.GetAroundSectorList(player->_sectorX, player->_sectorY, around, aroundCount);
    for (int32_t i = 0; i < aroundCount; ++i)
    {
        for (CPlayer* other : sectors.GetSectorPlayers(around[i].x, around[i].y))
        {
            if (other->_actorId == actorId)
                return other;
        }
    }
    return nullptr;
}

void CPartyHandler::OnPartyAsk(CPlayer* to, uint32_t requestId, PartyAskKind kind, const CPlayer* from, const CPlayer* about, uint32_t remainMs)
{
    _sessionHub.SendPacket(to, MakePartyAsk(requestId, kind, from, about, remainMs));
    _gameMonitor._party._askSent.Inc();
}

void CPartyHandler::OnPartyAskEnd(CPlayer* to, uint32_t requestId, PartyNotice why)
{
    _sessionHub.SendPacket(to, MakePartyAskEnd(requestId, why));
    _gameMonitor._party._askEndSent.Inc();
}

void CPartyHandler::OnPartyNotice(CPlayer* to, PartyNotice code, int32_t actorId)
{
    _sessionHub.SendPacket(to, MakePartyNotice(code, actorId));   // 세기는 규칙 쪽(CPartySystem::Notice — 번호별)
}

void CPartyHandler::OnPartyInfo(CPlayer* to, const Party* party)
{
    _sessionHub.SendPacket(to, MakePartyInfo(party));
    _gameMonitor._party._infoSent.Inc();
}

void CPartyHandler::OnPartyMember(CPlayer* to, const CPlayer* member)
{
    _sessionHub.SendPacket(to, MakePartyMember(member));
    _gameMonitor._party._memberSent.Inc();
}

#endif // USE_PARTY
