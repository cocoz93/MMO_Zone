#include "Room.h"

#if USE_PARTY

#include "Player.h"   // _zoneId · 체력 컴포넌트(생사) · _accountId

void CRoomLocks::Lock(int64_t accountId, int32_t mapId, uint64_t untilFrame)
{
    _untilFrame[Key{ accountId, mapId }] = untilFrame;
}

bool CRoomLocks::IsLocked(int64_t accountId, int32_t mapId, uint64_t frame) const
{
    auto it = _untilFrame.find(Key{ accountId, mapId });
    return it != _untilFrame.end() && frame < it->second;
}

void CRoomLocks::Sweep(uint64_t frame)
{
    for (auto it = _untilFrame.begin(); it != _untilFrame.end(); )
    {
        if (frame >= it->second)
            it = _untilFrame.erase(it);
        else
            ++it;
    }
}

RoomEntryResult CheckRoomEntry(const CPlayer* me, const Party* party, const CRoomLocks& locks, int32_t roomMapId, uint64_t frame)
{
    if (party == nullptr)
        return { RoomEntryReject::NOT_IN_PARTY, nullptr };
    if (party->leader != me)
        return { RoomEntryReject::NOT_LEADER, nullptr };

    // 모였나 — 파티장과 같은 존(같은 맵 · 같은 채널). 파티장 자신은 늘 통과한다
    for (const Party::Member& m : party->members)
    {
        if (m.player->_zoneId != me->_zoneId)
            return { RoomEntryReject::NOT_GATHERED, m.player };
    }
    // 살았나 — 쓰러진 채 옮기면 부활 대기가 옛 존을 가리킨다(포털이 죽은 사람을 막는 까닭과 같다 — CEntryHandler::RecvPortalUse)
    for (const Party::Member& m : party->members)
    {
        if (m.player->_health.IsDead())
            return { RoomEntryReject::DEAD, m.player };
    }
    // 잠금 — 계정마다(다시 접속해도 이어진다)
    for (const Party::Member& m : party->members)
    {
        if (locks.IsLocked(m.player->_accountId, roomMapId, frame))
            return { RoomEntryReject::LOCKED, m.player };
    }
    return { RoomEntryReject::NONE, nullptr };
}

#endif // USE_PARTY
