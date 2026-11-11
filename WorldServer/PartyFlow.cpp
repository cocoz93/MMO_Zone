#include "PartyFlow.h"

#include <Lobby/PacketSend.h>
#include <cstring>

CPartyFlow::CPartyFlow(CLinkPeers& peers, CMonitorManager& monitor, CWorldPresence& presence, CWorldMetrics& metrics, int64_t restoreSettleMs)
    : _peers(peers), _monitor(monitor), _presence(presence), _metrics(metrics), _party(presence, metrics, *this, restoreSettleMs)
{
}

bool CPartyFlow::RegisterRoutes(CPacketTable& zoneTable)
{
    bool ok = zoneTable.Register<LINK_PARTY_OP>([this](SessionId id, CSerialBuffer* msg) { OnOp(id, msg); });
    ok = zoneTable.Register<LINK_PARTY_STATE>([this](SessionId id, CSerialBuffer* msg) { OnState(id, msg); }) && ok;
    ok = zoneTable.Register<LINK_PARTY_RESTORE>([this](SessionId id, CSerialBuffer* msg) { OnRestore(id, msg); }) && ok;
    return ok;
}

void CPartyFlow::Tick(int64_t nowMs)
{
    _nowMs = nowMs;
    _party.Tick(nowMs);
    _metrics.SetPartyMembersOnline(_party.OnlineMembers());
}

void CPartyFlow::OnOp(SessionId sessionId, CSerialBuffer* msg)
{
    LINK_PARTY_OP packet;
    if (!_presence.IsRegistered(sessionId) || !PacketSend::Read(msg, packet))
    {
        _monitor._packetErrors.Inc();
        return;
    }

    const CharacterId me{ packet.characterId };
    const CharacterId target{ packet.targetCharacterId };
    _presence.SetLevel(me, packet.level);   // 요청 창에 옛 레벨이 나가지 않게

    switch (static_cast<LinkPartyOpKind>(packet.op))
    {
    case LinkPartyOpKind::Request:
        _presence.SetLevel(target, packet.targetLevel);
        _party.Request(me, target, _nowMs);
        return;
    case LinkPartyOpKind::InviteByName:
    {
        const CWorldPresence::Person* found = _presence.FindByName(CWorldPresence::NameOf(packet.targetName));
        _party.Request(me, found != nullptr ? found->characterId : NO_CHARACTER, _nowMs);
        return;
    }
    case LinkPartyOpKind::Answer:     _party.Answer(me, packet.requestId, packet.accept != 0, _nowMs); return;
    case LinkPartyOpKind::Leave:      _party.Leave(me);             return;
    case LinkPartyOpKind::Kick:       _party.Kick(me, target);      return;
    case LinkPartyOpKind::SetLeader:  _party.SetLeader(me, target); return;
    case LinkPartyOpKind::Disband:    _party.Disband(me);           return;
    case LinkPartyOpKind::TakeLeader: _party.TakeLeader(me);        return;
    }
    _monitor._packetErrors.Inc();   // 모르는 종류
}

void CPartyFlow::OnState(SessionId sessionId, CSerialBuffer* msg)
{
    LINK_PARTY_STATE head;
    if (!_presence.IsRegistered(sessionId) || !PacketSend::Read(msg, head))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    if (head.count > LINK_PARTY_STATE_MAX
        || static_cast<size_t>(msg->GetDataSize()) < sizeof(head) + size_t{ head.count } * sizeof(LinkPartyStateRow))
    {
        _monitor._packetErrors.Inc();
        return;
    }

    const char* data = msg->GetReadBufferPtr() + sizeof(head);
    for (uint8_t i = 0; i < head.count; ++i)
    {
        LinkPartyStateRow row;
        std::memcpy(&row, data + size_t{ i } * sizeof(row), sizeof(row));
        // 옮겨 간 사람의 옛 존 서버가 늦게 보낸 줄은 버린다 — 장부가 말하는 존 서버의 줄만 믿는다
        if (SessionOf(CharacterId{ row.characterId }) == sessionId)
            _party.ApplyState(row);
    }
}

void CPartyFlow::OnRestore(SessionId sessionId, CSerialBuffer* msg)
{
    LINK_PARTY_RESTORE head;
    if (!_presence.IsRegistered(sessionId) || !PacketSend::Read(msg, head))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    if (head.count > LINK_PARTY_RESTORE_MAX
        || static_cast<size_t>(msg->GetDataSize()) < sizeof(head) + size_t{ head.count } * sizeof(LinkPartyRestoreRow))
    {
        _monitor._packetErrors.Inc();
        return;
    }

    const char* data = msg->GetReadBufferPtr() + sizeof(head);
    for (uint16_t i = 0; i < head.count; ++i)
    {
        LinkPartyRestoreRow row;
        std::memcpy(&row, data + size_t{ i } * sizeof(row), sizeof(row));
        _party.Restore(row, _nowMs);
    }
}

SessionId CPartyFlow::SessionOf(CharacterId id) const
{
    const CWorldPresence::Person* person = _presence.Find(id);
    return person != nullptr ? person->zoneSession : NO_SESSION;
}

int CPartyFlow::ZonesOf(const Party& party, SessionId (&out)[LINK_PARTY_MEMBERS_MAX], int& people) const
{
    int count = 0;
    people = 0;
    for (const LinkPartyMemberRow& row : party.members)
    {
        const SessionId session = SessionOf(CharacterId{ row.who.characterId });
        if (session == NO_SESSION)
            continue;
        ++people;
        bool seen = false;
        for (int i = 0; i < count; ++i)
            seen = seen || out[i] == session;
        if (!seen)
            out[count++] = session;
    }
    return count;
}

void CPartyFlow::OnPartyAsk(CharacterId to, uint32_t requestId, PartyAskKind kind, const LinkPartyPerson& from, const LinkPartyPerson& about, uint32_t remainMs)
{
    const SessionId session = SessionOf(to);
    if (session == NO_SESSION)
        return;
    LINK_PARTY_ASK packet{};
    packet.to = ToInt(to);
    packet.requestId = requestId;
    packet.kind = static_cast<uint8_t>(kind);
    packet.from = from;
    packet.about = about;
    packet.remainMs = remainMs;
    _peers.Send(session, packet);
    _metrics._party.askSent.fetch_add(1, std::memory_order_relaxed);
}

void CPartyFlow::OnPartyAskEnd(CharacterId to, uint32_t requestId, PartyNotice why)
{
    const SessionId session = SessionOf(to);
    if (session == NO_SESSION)
        return;
    LINK_PARTY_ASK_END packet{};
    packet.to = ToInt(to);
    packet.requestId = requestId;
    packet.why = static_cast<uint8_t>(why);
    _peers.Send(session, packet);
    _metrics._party.askEndSent.fetch_add(1, std::memory_order_relaxed);
}

void CPartyFlow::OnPartyNotice(CharacterId to, PartyNotice code, const LinkPartyPerson& who)
{
    const SessionId session = SessionOf(to);
    if (session == NO_SESSION)
        return;
    LINK_PARTY_NOTICE packet{};
    packet.to = ToInt(to);
    packet.code = static_cast<uint8_t>(code);
    packet.who = who;
    _peers.Send(session, packet);
}

void CPartyFlow::OnPartyInfo(const Party& party)
{
    SessionId zones[LINK_PARTY_MEMBERS_MAX];
    int people = 0;
    const int count = ZonesOf(party, zones, people);
    SendInfo(party, zones, count);
    _metrics._party.infoSent.fetch_add(people, std::memory_order_relaxed);
}

// 옛 존 서버는 이 사람을 아직 제 사람으로 여기는 동안 명단을 받아 옛 액터 번호 · 옛 채널로 그렸다.
// 그 사람이 빠진 지금 월드 서버의 줄(새 존 서버 값)로 다시 그리게 한다 — 안 보내면 그 사람 값이 다음에 바뀔 때까지 남는다
void CPartyFlow::ResendToOldZone(SessionId oldZone, CharacterId id)
{
    const Party* party = _party.FindParty(id);
    if (party == nullptr)
        return;
    int people = 0;
    for (const LinkPartyMemberRow& row : party->members)
        people += SessionOf(CharacterId{ row.who.characterId }) == oldZone ? 1 : 0;
    if (people == 0)
        return;   // 그 존 서버에 남은 파티원이 없다
    SendInfo(*party, &oldZone, 1);
    _metrics._party.infoSent.fetch_add(people, std::memory_order_relaxed);
}

void CPartyFlow::SendInfo(const Party& party, const SessionId* zones, int count)
{
    // pack 필수 — 없으면 줄이 경계로 밀려 머리와 첫 줄 사이에 빈 칸이 낀다
#pragma pack(push, 1)
    struct InfoPacket
    {
        LINK_PARTY_INFO    head;
        LinkPartyMemberRow rows[LINK_PARTY_MEMBERS_MAX];
    };
#pragma pack(pop)
    static_assert(sizeof(InfoPacket) == sizeof(LINK_PARTY_INFO) + sizeof(LinkPartyMemberRow) * LINK_PARTY_MEMBERS_MAX, "명단 패킷에 빈 칸이 끼었다");

    InfoPacket packet{};
    packet.head.partyId = party.id;
    packet.head.leader = ToInt(party.leader);
    packet.head.count = static_cast<uint8_t>(party.members.size());
    std::memcpy(packet.rows, party.members.data(), sizeof(LinkPartyMemberRow) * party.members.size());
    const int size = static_cast<int>(sizeof(packet.head) + sizeof(LinkPartyMemberRow) * party.members.size());
    packet.head.header.size = static_cast<uint16_t>(size);
    packet.head.header.type = static_cast<MsgType>(LINK_PARTY_INFO::TYPE);

    for (int i = 0; i < count; ++i)
        _peers.SendBytes(zones[i], &packet, size);
}

void CPartyFlow::OnPartyNone(CharacterId to)
{
    const SessionId session = SessionOf(to);
    if (session == NO_SESSION)
        return;
    LINK_PARTY_INFO packet{};
    packet.to = ToInt(to);
    _peers.Send(session, packet);
    _metrics._party.infoSent.fetch_add(1, std::memory_order_relaxed);
}

void CPartyFlow::OnPartyMember(const Party& party, const LinkPartyMemberRow& row)
{
    LINK_PARTY_MEMBER packet{};
    packet.partyId = party.id;
    packet.row = row;

    SessionId zones[LINK_PARTY_MEMBERS_MAX];
    int people = 0;
    const int count = ZonesOf(party, zones, people);
    for (int i = 0; i < count; ++i)
        _peers.Send(zones[i], packet);
    _metrics._party.memberSent.fetch_add(people, std::memory_order_relaxed);
}
