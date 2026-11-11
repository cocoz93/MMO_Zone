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
#include "WorldLink.h"
#include <Identity/ChannelNumber.h>
#include <Lobby/PacketSend.h>
#include <Protocol/Protocol.h>
#include <Common/ErrorLog.h>
#include <cassert>
#include "PacketBuild.h" // 머리 쓰기·닫기 — 게임 서버 나누기 0-②

using namespace PacketBuild;   // 아래 조립 함수들이 머리 쓰기·닫기를 이름만으로 부른다

// ==========================================================================
// 파티 S2C 조립 — 이 클래스만 부른다(GameServer.cpp 에서 옮김).
//   쓰는 법(스칼라는 << · 고정배열·문자열은 SetData(raw) · pack(1))은 PacketBuild.h 머리의 [규칙] 주석과 같다.
// ==========================================================================
namespace
{
    // 파티원 정보(Protocol.h PartyPerson 30B) — 액터 번호는 받는 사람과 같은 존 서버일 때만 채운다
    void WritePerson(CSerialBuffer* buf, const LinkPartyPerson& p, ActorId actorId)
    {
        *buf << static_cast<int64_t>(p.characterId);
        *buf << static_cast<int>(actorId);
        *buf << static_cast<uint8_t>(p.displayChar);
        *buf << static_cast<uint8_t>(p.colorIndex);
        *buf << static_cast<int>(p.level);
        for (int i = 0; i < CHAR_NAME_MAX_LEN; ++i)
            *buf << static_cast<uint16_t>(p.name[i]);
    }

    // 파티원 한 줄(Protocol.h PartyMemberEntry 45B) — INFO 의 줄이자 MEMBER 의 몸통. 줄을 늘리면 여기 한 곳 · proto.js readPartyMember.
    void WriteRow(CSerialBuffer* buf, const LinkPartyMemberRow& row, ActorId actorId)
    {
        WritePerson(buf, row.who, actorId);
        *buf << static_cast<int>(row.hp);
        *buf << static_cast<int>(row.maxHp);
        *buf << static_cast<int>(row.mapId);
        *buf << static_cast<uint16_t>(row.visibleChannel);
        *buf << static_cast<uint8_t>(row.online);
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

    CSerialBuffer* MakePartyNotice(PartyNotice code, const LinkPartyPerson& who, ActorId actorId)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_PARTY_NOTICE::TYPE);
        *buf << static_cast<uint8_t>(code);
        WritePerson(buf, who, actorId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_PARTY_NOTICE));
        return buf;
    }

    // 파티 전체는 한 장으로 간다 — 꽉 찬 파티(8명)도 한 패킷에 들어가야 한다(가방 전체와 같은 단언, SelfNotifier.cpp MakeInventory 앞).
    static_assert(sizeof(MSG_S2C_PARTY_INFO) <= MAX_PACKET_SIZE,
                  "꽉 찬 파티 전체가 한 패킷(MAX_PACKET_SIZE)에 안 들어간다 — 나눠 보내는 길을 만들 것");
}

ActorId CPartyHandler::ActorOf(const LinkPartyPerson& person) const
{
    const CPlayer* player = _copy.FindPlayer(CharacterId{ person.characterId });
    return player != nullptr ? player->_actorId : NO_ACTOR;
}

int CPartyHandler::VisibleChannelOf(const CPlayer* player) const
{
    if (_mapManager.IsRoomMap(CMapManager::GetMapIdFromZoneId(player->_zoneId)))
        return 0;
    return ChannelNumber::ToVisible(_zoneServerId, _channelBlock, CMapManager::GetChannelIndexFromZoneId(player->_zoneId));
}

LinkPartyMemberRow CPartyHandler::LiveRow(const Party::Member& m) const
{
    LinkPartyMemberRow row = m.row;
    const CPlayer* p = m.player;
    if (p == nullptr)
        return row;
    // 체력은 등장 상태 쓰기(WriteSnapshot)와 같은 규칙 — 전투가 꺼진 빌드는 0
#if USE_COMBAT
    row.hp    = p->_health.Hp();
    row.maxHp = p->_health.MaxHp();
#endif
    row.who.level = p->_level;
    row.mapId = ToInt(CMapManager::GetMapIdFromZoneId(p->_zoneId));
    row.visibleChannel = static_cast<uint16_t>(VisibleChannelOf(p));
    row.online = 1;
    return row;
}

// 파티 전체 — 가변 길이(가입 순서 줄만). party 가 nullptr 이면 파티 없음(번호·파티장 0 · 인원 0 — 머리 17B 뿐).
CSerialBuffer* CPartyHandler::MakePartyInfo(const Party* party) const
{
    const int count = (party != nullptr) ? static_cast<int>(party->members.size()) : 0;

    CSerialBuffer* buf = CSerialBuffer::Alloc();
    BeginPacket(buf, MSG_S2C_PARTY_INFO::TYPE);
    *buf << static_cast<int>(party != nullptr ? party->id : 0);
    *buf << static_cast<int64_t>(party != nullptr ? ToInt(party->leader) : 0);
    *buf << static_cast<uint8_t>(count);
    if (party != nullptr)
    {
        for (const Party::Member& m : party->members)
            WriteRow(buf, LiveRow(m), m.player != nullptr ? m.player->_actorId : NO_ACTOR);
    }
    FinalizePacket(buf);
    assert(buf->GetDataSize() ==
        offsetof(MSG_S2C_PARTY_INFO, entries) + static_cast<size_t>(count) * sizeof(PartyMemberEntry));
    return buf;
}

CSerialBuffer* CPartyHandler::MakePartyMember(const Party::Member& m) const
{
    CSerialBuffer* buf = CSerialBuffer::Alloc();
    BeginPacket(buf, MSG_S2C_PARTY_MEMBER::TYPE);
    WriteRow(buf, LiveRow(m), m.player != nullptr ? m.player->_actorId : NO_ACTOR);
    FinalizePacket(buf);
    assert(buf->GetDataSize() == sizeof(MSG_S2C_PARTY_MEMBER));
    return buf;
}

// 받은 패킷 등록 — 옛 등록의 함정 주석을 그대로 옮겼다(게임 서버 나누기 0-④ 의 등록에서 이 클래스 몫 여섯 · 7 단계).
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

    CPacketTable& world = _worldLink.Routes();
    ok &= world.Register<LINK_PARTY_ASK>([this](SessionId, CSerialBuffer* m) { RecvWorldAsk(m); });
    ok &= world.Register<LINK_PARTY_ASK_END>([this](SessionId, CSerialBuffer* m) { RecvWorldAskEnd(m); });
    ok &= world.Register<LINK_PARTY_NOTICE>([this](SessionId, CSerialBuffer* m) { RecvWorldNotice(m); });
    ok &= world.Register<LINK_PARTY_INFO>([this](SessionId, CSerialBuffer* m) { RecvWorldInfo(m); });
    ok &= world.Register<LINK_PARTY_MEMBER>([this](SessionId, CSerialBuffer* m) { RecvWorldMember(m); });
    return ok;
}

// 방 맵 포털(PartyHandler.h 선언 주석 · Room.h). 판정은 순수 함수 하나, 와이어로 바꾸기는 SendRoomReject 한 곳.
void CPartyHandler::EnterRoom(CPlayer* leader, CZone* oldZone, const PortalDef& portal)
{
    auto& pc = _gameMonitor._portal;
    auto& rc = _gameMonitor._room;

    const Party* party = _copy.FindParty(leader);
    if (party != nullptr && party->leader != leader->_characterId && !party->LeaderOnline())
    {
        // 파티장이 오프라인이면 누른 파티원이 맡는다 — 판정에는 맡은 뒤의 명단이 필요해 이번 입장은 여기서 끝낸다
        rc._entries[static_cast<int>(RoomEntryReject::NOT_LEADER)].Inc();
        pc._rejected[static_cast<int>(PortalReject::ROOM)].Inc();
        SendOp(leader, LinkPartyOpKind::TakeLeader, LINK_PARTY_OP{});
        return;
    }

    RoomEntryResult result = CheckRoomEntry(leader, party, portal.targetMapId, RoomLockoutBlob::NowMs());

    CZone* room = nullptr;
    if (result.reject == RoomEntryReject::NONE)
    {
        room = _mapManager.CreateRoom(portal.targetMapId);
        if (room == nullptr)
            result.reject = RoomEntryReject::FULL;   // 번호가 다 찼다(CRoomMap::CreateRoom)
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
    const LinkPartyPerson none{};
    const LinkPartyPerson& who = (result.blocker != nullptr) ? result.blocker->row.who : none;
    const ActorId actor = (result.blocker != nullptr && result.blocker->player != nullptr) ? result.blocker->player->_actorId : NO_ACTOR;
    switch (result.reject)
    {
    case RoomEntryReject::NOT_IN_PARTY: NoticeLocal(leader, PartyNotice::NOT_IN_PARTY, none, NO_ACTOR);       return;
    case RoomEntryReject::NOT_LEADER:   NoticeLocal(leader, PartyNotice::NOT_LEADER, none, NO_ACTOR);         return;
    case RoomEntryReject::NOT_GATHERED: NoticeLocal(leader, PartyNotice::ROOM_NOT_GATHERED, who, actor);      return;
    case RoomEntryReject::DEAD:         NoticeLocal(leader, PartyNotice::ROOM_DEAD, who, actor);              return;
    case RoomEntryReject::LOCKED:       NoticeLocal(leader, PartyNotice::ROOM_LOCKED, who, actor);            return;
    case RoomEntryReject::FULL:         _placement.SendZoneChangeFail(leader, 1);                             return;   // 1: 모든 채널 가득 참
    case RoomEntryReject::NONE:
    case RoomEntryReject::COUNT:        break;
    }
    assert(false && "SendRoomReject 는 거절만 받는다");
}

// 게임 루프 5(빈 채널 정리)의 콜백에 있던 몸 그대로(게임 서버 나누기 14 단계)
void CPartyHandler::OnErasingZone(CZone* zone)
{
    // 빈 방 — 열린 방 수를 내린다(등식: 들어감 = 닫힘 + 열림 — GameMonitor.h _room). 방 번호는 이 뒤 다시 쓸 수 있다(CRoomMap::CreateRoom)
    //   지워지는 존은 늘 방이다(빈 방 정리 CRoomMap::CloseEmptyRooms — 일반 맵 채널은 안 지운다). 10-27 전엔 빈 동적 채널도 와서 방 맵인지 갈랐다
    assert(_mapManager.IsRoomMap(zone->GetMapId()) && "지우는 존은 방뿐이다 — CMapManager::CloseEmptyRooms");
    (void)zone;   // 릴리즈(NDEBUG)에선 위 단언이 사라진다
    _gameMonitor._room._active.Add(-1);
    _gameMonitor._room._closed.Inc();
}

// ==========================================================================
// 플레이어 요청 — 규칙은 월드 서버 CPartySystem, 여기는 상대를 찾아 넘기기만
// ==========================================================================

void CPartyHandler::SendOp(CPlayer* me, LinkPartyOpKind kind, LINK_PARTY_OP op)
{
    op.op = static_cast<uint8_t>(kind);
    op.characterId = ToInt(me->_characterId);
    op.level = me->_level;
    if (!_worldLink.SendPartyOp(op))
        NoticeLocal(me, PartyNotice::NO_WORLD, LinkPartyPerson{}, NO_ACTOR);
}

// 화면에서 누른 캐릭터 — 내 주변 9섹터에서 찾아 넘긴다(없으면 NO_TARGET)
void CPartyHandler::RecvPartyRequest(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_PARTY_REQUEST recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    const CPlayer* target = FindPlayerAround(player, ActorId{ recvMsg.targetId });
    if (target == nullptr)
    {
        NoticeLocal(player, PartyNotice::NO_TARGET, LinkPartyPerson{}, NO_ACTOR);
        return;
    }
    LINK_PARTY_OP op{};
    op.targetCharacterId = ToInt(target->_characterId);
    op.targetLevel = target->_level;
    SendOp(player, LinkPartyOpKind::Request, op);
}

void CPartyHandler::InviteByName(CPlayer* me, const char16_t (&name)[LINK_NAME_CHARS])
{
    LINK_PARTY_OP op{};
    std::memcpy(op.targetName, name, sizeof(op.targetName));
    SendOp(me, LinkPartyOpKind::InviteByName, op);
}

void CPartyHandler::RecvPartyAnswer(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_PARTY_ANSWER recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    const auto open = _openAsks.find(ToInt(player->_characterId));
    if (open != _openAsks.end() && open->second == recvMsg.requestId)
        _openAsks.erase(open);

    LINK_PARTY_OP op{};
    op.requestId = recvMsg.requestId;
    op.accept = recvMsg.accept != 0 ? 1 : 0;
    SendOp(player, LinkPartyOpKind::Answer, op);
}

void CPartyHandler::RecvPartyLeave(CPlayer* player)
{
    SendOp(player, LinkPartyOpKind::Leave, LINK_PARTY_OP{});
}

// 내보내기·넘기기 — 대상은 캐릭터 번호다(다른 존 서버의 파티원과 오프라인 파티원도 지정한다)
void CPartyHandler::RecvPartyKick(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_PARTY_KICK recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    LINK_PARTY_OP op{};
    op.targetCharacterId = recvMsg.targetCharacterId;
    SendOp(player, LinkPartyOpKind::Kick, op);
}

void CPartyHandler::RecvPartyLeader(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_PARTY_LEADER recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    LINK_PARTY_OP op{};
    op.targetCharacterId = recvMsg.targetCharacterId;
    SendOp(player, LinkPartyOpKind::SetLeader, op);
}

void CPartyHandler::RecvPartyDisband(CPlayer* player)
{
    SendOp(player, LinkPartyOpKind::Disband, LINK_PARTY_OP{});
}

CPlayer* CPartyHandler::FindPlayerAround(CPlayer* player, ActorId actorId)
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

void CPartyHandler::NoticeLocal(CPlayer* to, PartyNotice code, const LinkPartyPerson& who, ActorId actorId)
{
    _gameMonitor._party._notices[static_cast<int>(code)].Inc();
    _sessionHub.SendPacket(to, MakePartyNotice(code, who, actorId));
}

// ==========================================================================
// 월드 서버 패킷 — 사본을 고치고 웹 패킷으로 옮긴다
// ==========================================================================

void CPartyHandler::RecvWorldAsk(CSerialBuffer* msg)
{
    LINK_PARTY_ASK packet;
    if (!PacketSend::Read(msg, packet))
    {
        _worldLink.CountPacketError();
        return;
    }
    CPlayer* to = _copy.FindPlayer(CharacterId{ packet.to });
    if (to == nullptr)
        return;   // 패킷이 오는 사이 나갔다 — 월드 서버가 곧 그 요청을 취소한다
    _openAsks[packet.to] = packet.requestId;

    CSerialBuffer* buf = CSerialBuffer::Alloc();
    BeginPacket(buf, MSG_S2C_PARTY_ASK::TYPE);
    *buf << static_cast<uint32_t>(packet.requestId);
    *buf << static_cast<uint8_t>(packet.kind);
    WritePerson(buf, packet.from, ActorOf(packet.from));
    WritePerson(buf, packet.about, ActorOf(packet.about));
    *buf << static_cast<uint32_t>(packet.remainMs);
    FinalizePacket(buf);
    assert(buf->GetDataSize() == sizeof(MSG_S2C_PARTY_ASK));
    _sessionHub.SendPacket(to, buf);
}

void CPartyHandler::RecvWorldAskEnd(CSerialBuffer* msg)
{
    LINK_PARTY_ASK_END packet;
    if (!PacketSend::Read(msg, packet))
    {
        _worldLink.CountPacketError();
        return;
    }
    CPlayer* to = _copy.FindPlayer(CharacterId{ packet.to });
    if (to == nullptr)
        return;
    const auto open = _openAsks.find(packet.to);
    if (open != _openAsks.end() && open->second == packet.requestId)
        _openAsks.erase(open);
    _sessionHub.SendPacket(to, MakePartyAskEnd(packet.requestId, static_cast<PartyNotice>(packet.why)));
}

void CPartyHandler::RecvWorldNotice(CSerialBuffer* msg)
{
    LINK_PARTY_NOTICE packet;
    if (!PacketSend::Read(msg, packet))
    {
        _worldLink.CountPacketError();
        return;
    }
    CPlayer* to = _copy.FindPlayer(CharacterId{ packet.to });
    if (to == nullptr)
        return;
    _sessionHub.SendPacket(to, MakePartyNotice(static_cast<PartyNotice>(packet.code), packet.who, ActorOf(packet.who)));
}

void CPartyHandler::RecvWorldInfo(CSerialBuffer* msg)
{
    LINK_PARTY_INFO head;
    if (!PacketSend::Read(msg, head) || head.count > LINK_PARTY_MEMBERS_MAX
        || static_cast<size_t>(msg->GetDataSize()) < sizeof(head) + size_t{ head.count } * sizeof(LinkPartyMemberRow))
    {
        _worldLink.CountPacketError();
        return;
    }

    if (head.to != 0)
    {
        CPlayer* to = _copy.FindPlayer(CharacterId{ head.to });
        if (to == nullptr)
            return;
        _copy.ApplyNone(to);
        _sessionHub.SendPacket(to, MakePartyInfo(nullptr));
        return;
    }

    // 줄은 pack(1) 이라 정렬 요구가 없다 — 받은 버퍼를 그대로 읽는다
    const auto* rows = reinterpret_cast<const LinkPartyMemberRow*>(msg->GetReadBufferPtr() + sizeof(head));
    Party* party = _copy.ApplyInfo(head.partyId, CharacterId{ head.leader }, rows, head.count);
    if (party == nullptr)
        return;
    SendInfoToLocals(*party);

    // 월드 서버의 줄은 입장 전 값이거나 비어 있다 — 바로 견줘서 다른 존 서버의 파티원이 0.5초를 기다리지 않게 한다
    LinkPartyStateRow batch[LINK_PARTY_STATE_MAX];
    int count = 0;
    ScanParty(*party, batch, count);
    if (count > 0)
        _worldLink.SendPartyState(batch, count);
}

void CPartyHandler::RecvWorldMember(CSerialBuffer* msg)
{
    LINK_PARTY_MEMBER packet;
    if (!PacketSend::Read(msg, packet))
    {
        _worldLink.CountPacketError();
        return;
    }
    Party* party = _copy.FindById(packet.partyId);
    if (party == nullptr)
        return;
    for (Party::Member& m : party->members)
    {
        if (m.row.who.characterId != packet.row.who.characterId)
            continue;
        if (m.player == nullptr)
            m.row = packet.row;   // 이 존 서버 사람의 값은 이 존 서버가 정본이다 — 되돌아온 옛 값으로 덮으면 다시 보내게 된다
        SendMemberToLocals(*party, m);
        return;
    }
}

void CPartyHandler::SendInfoToLocals(const Party& party)
{
    for (const Party::Member& m : party.members)
    {
        if (m.player != nullptr)
            _sessionHub.SendPacket(m.player, MakePartyInfo(&party));
    }
}

void CPartyHandler::SendMemberToLocals(const Party& party, const Party::Member& changed)
{
    for (const Party::Member& m : party.members)
    {
        if (m.player != nullptr)
            _sessionHub.SendPacket(m.player, MakePartyMember(changed));
    }
}

// ==========================================================================
// 틱 · 입퇴장 · 연결
// ==========================================================================

void CPartyHandler::ScanParty(Party& party, LinkPartyStateRow* batch, int& count)
{
    const bool up = _worldLink.IsUp();
    for (Party::Member& m : party.members)
    {
        if (m.player == nullptr)
            continue;
        const LinkPartyMemberRow live = LiveRow(m);
        if (live.hp == m.row.hp && live.maxHp == m.row.maxHp && live.who.level == m.row.who.level
            && live.mapId == m.row.mapId && live.visibleChannel == m.row.visibleChannel)
            continue;
        m.row = live;

        if (!up)
        {
            SendMemberToLocals(party, m);
            continue;
        }
        LinkPartyStateRow& state = batch[count++];
        state.characterId = live.who.characterId;
        state.hp = live.hp;
        state.maxHp = live.maxHp;
        state.level = live.who.level;
        state.mapId = live.mapId;
        state.visibleChannel = live.visibleChannel;
        if (count == LINK_PARTY_STATE_MAX)
        {
            _worldLink.SendPartyState(batch, count);
            count = 0;
        }
    }
}

void CPartyHandler::ScanAll()
{
    LinkPartyStateRow batch[LINK_PARTY_STATE_MAX];
    int count = 0;
    for (auto& entry : _copy.Parties())
        ScanParty(entry.second, batch, count);
    if (count > 0)
        _worldLink.SendPartyState(batch, count);
}

void CPartyHandler::Tick()
{
    if (_copy.Parties().empty() || _frameCount - _lastScanFrame < MEMBER_SCAN_FRAMES)
        return;
    _lastScanFrame = _frameCount;
    ScanAll();
}

void CPartyHandler::AddPlayer(CPlayer* player)
{
    _copy.AddPlayer(player);
    if (_worldLink.IsUp())
        return;   // 월드 서버가 붙어 있으면 입장 알림의 답으로 명단이 온다
    // 월드 서버가 없는 동안 다시 들어왔다 — 사본에 줄이 남아 있으면 다시 이어야 복원 때 이 사람도 소속을 보낸다
    Party* party = _copy.Relink(player);
    if (party != nullptr)
        SendInfoToLocals(*party);
}

void CPartyHandler::Forget(CPlayer* player)
{
    const CharacterId id = player->_characterId;
    // 창은 이 사람 것이다 — 같은 캐릭터가 새로 들어와 있으면 그 창을 지우면 안 되므로 지금 주소가 이 사람일 때만
    if (_copy.FindPlayer(id) == player)
        _openAsks.erase(ToInt(id));

    Party* party = _copy.Forget(player);
    if (party == nullptr || _worldLink.IsUp())
        return;   // 월드 서버가 붙어 있으면 퇴장 알림의 답으로 명단이 온다
    CPartyCopy::MarkOffline(*party, id);
    SendInfoToLocals(*party);
}

void CPartyHandler::OnWorldUp()
{
    LinkPartyRestoreRow batch[LINK_PARTY_RESTORE_MAX];
    int count = 0;
    for (const auto& entry : _copy.Parties())
    {
        const Party& party = entry.second;
        for (size_t i = 0; i < party.members.size(); ++i)
        {
            const Party::Member& m = party.members[i];
            if (m.player == nullptr)
                continue;
            const LinkPartyMemberRow live = LiveRow(m);
            LinkPartyRestoreRow& row = batch[count++];
            row.partyId = party.id;
            row.isLeader = party.leader == m.player->_characterId ? 1 : 0;
            row.joinOrder = static_cast<uint8_t>(i);
            row.state.characterId = live.who.characterId;
            row.state.hp = live.hp;
            row.state.maxHp = live.maxHp;
            row.state.level = live.who.level;
            row.state.mapId = live.mapId;
            row.state.visibleChannel = live.visibleChannel;
            if (count == LINK_PARTY_RESTORE_MAX)
            {
                _worldLink.SendPartyRestore(batch, count);
                count = 0;
            }
        }
    }
    if (count > 0)
        _worldLink.SendPartyRestore(batch, count);
}

void CPartyHandler::OnWorldDown()
{
    for (const auto& [characterId, requestId] : _openAsks)
    {
        CPlayer* to = _copy.FindPlayer(CharacterId{ characterId });
        if (to != nullptr)
            _sessionHub.SendPacket(to, MakePartyAskEnd(requestId, PartyNotice::EXPIRED));
    }
    _openAsks.clear();
}

#endif // USE_PARTY
