#pragma once
// 파티 알림 번호의 지표 이름 — 월드 서버(규칙이 낸 알림)와 존 서버(존 서버가 낸 거절)가 같은 어휘를 쓴다

#include <Protocol/Protocol.h>

inline const char* PartyNoticeName(PartyNotice r)
{
    static_assert(static_cast<int>(PartyNotice::COUNT) == 26, "PartyNotice 를 늘렸으면 이름도 붙일 것");
    switch (r)
    {
    case PartyNotice::SENT_INVITE:   return "sent_invite";
    case PartyNotice::SENT_APPLY:    return "sent_apply";
    case PartyNotice::SENT_SUGGEST:  return "sent_suggest";
    case PartyNotice::NO_TARGET:     return "no_target";
    case PartyNotice::SELF:          return "self";
    case PartyNotice::SAME_PARTY:    return "same_party";
    case PartyNotice::BOTH_IN_PARTY: return "both_in_party";
    case PartyNotice::FULL:          return "full";
    case PartyNotice::NOT_LEADER:    return "not_leader";
    case PartyNotice::NOT_IN_PARTY:  return "not_in_party";
    case PartyNotice::NOT_MEMBER:    return "not_member";
    case PartyNotice::TARGET_BUSY:   return "target_busy";
    case PartyNotice::SELF_BUSY:     return "self_busy";
    case PartyNotice::NO_REQUEST:    return "no_request";
    case PartyNotice::DECLINED:      return "declined";
    case PartyNotice::EXPIRED:       return "expired";
    case PartyNotice::CANCELLED:     return "cancelled";
    case PartyNotice::JOINED:        return "joined";
    case PartyNotice::LEFT:          return "left";
    case PartyNotice::KICKED:        return "kicked";
    case PartyNotice::LEADER:        return "leader";
    case PartyNotice::DISBANDED:     return "disbanded";
    case PartyNotice::ROOM_NOT_GATHERED: return "room_not_gathered";
    case PartyNotice::ROOM_DEAD:     return "room_dead";
    case PartyNotice::ROOM_LOCKED:   return "room_locked";
    case PartyNotice::NO_WORLD:      return "no_world";
    case PartyNotice::COUNT:         break;
    }
    return "?";
}
