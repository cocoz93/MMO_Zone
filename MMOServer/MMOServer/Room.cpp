#include "Room.h"

#if USE_PARTY

#include "Player.h"   // _zoneId · 체력 컴포넌트(생사) · _roomLockouts

#include <algorithm>

void LockRoom(CPlayer* player, MapId mapId, int64_t untilMs, int64_t nowMs)
{
    std::vector<RoomLockoutBlob::Lockout>& book = player->_roomLockouts;
    book.erase(std::remove_if(book.begin(), book.end(),
                              [nowMs](const RoomLockoutBlob::Lockout& l) { return l.untilMs <= nowMs; }), book.end());
    for (RoomLockoutBlob::Lockout& l : book)
    {
        if (l.mapId == ToInt(mapId))
        {
            l.untilMs = untilMs;
            return;
        }
    }
    // 방 맵 수는 상한 안이다(CMapTable::Load). 맵 표에서 빠진 맵의 옛 잠금이 남아 꽉 찼다면 가장 먼저 끝나는 칸을 비운다
    if (book.size() >= static_cast<size_t>(RoomLockoutBlob::MAX_SLOTS))
        book.erase(std::min_element(book.begin(), book.end(),
                                    [](const RoomLockoutBlob::Lockout& a, const RoomLockoutBlob::Lockout& b) { return a.untilMs < b.untilMs; }));
    book.push_back(RoomLockoutBlob::Lockout{ ToInt(mapId), untilMs });
}

bool IsRoomLocked(const CPlayer* player, MapId mapId, int64_t nowMs)
{
    for (const RoomLockoutBlob::Lockout& l : player->_roomLockouts)
    {
        if (l.mapId == ToInt(mapId))
            return nowMs < l.untilMs;
    }
    return false;
}

RoomEntryResult CheckRoomEntry(const CPlayer* me, const Party* party, MapId roomMapId, int64_t nowMs)
{
    if (party == nullptr)
        return { RoomEntryReject::NOT_IN_PARTY, nullptr };
    if (party->leader != me->_characterId)
        return { RoomEntryReject::NOT_LEADER, nullptr };

    // 모였나 — 파티장과 같은 존(같은 맵 · 같은 채널). 파티장 자신은 늘 통과한다.
    //   다른 존 서버의 파티원과 오프라인 파티원은 주소가 없다 — 파티장이 내보내면 들어갈 수 있다
    for (const Party::Member& m : party->members)
    {
        if (m.player == nullptr || m.player->_zoneId != me->_zoneId)
            return { RoomEntryReject::NOT_GATHERED, &m };
    }
    // 살았나 — 쓰러진 채 옮기면 부활 대기가 옛 존을 가리킨다(포털이 죽은 사람을 막는 까닭과 같다 — CEntryHandler::RecvPortalUse)
    for (const Party::Member& m : party->members)
    {
        if (m.player->_health.IsDead())
            return { RoomEntryReject::DEAD, &m };
    }
    // 잠금 — 캐릭터마다(다시 접속해도 이어진다)
    for (const Party::Member& m : party->members)
    {
        if (IsRoomLocked(m.player, roomMapId, nowMs))
            return { RoomEntryReject::LOCKED, &m };
    }
    return { RoomEntryReject::NONE, nullptr };
}

#endif // USE_PARTY
