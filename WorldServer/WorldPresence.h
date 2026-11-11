#pragma once
// ==========================================================================
// CWorldPresence — 월드 서버의 접속 장부: 등록된 존 서버
//
//   존 서버 목록의 정본은 실제로 붙어 있는 연결이다. 연결이 끊기면 그 존 서버는 없는 것으로 친다.
//   로직 스레드 전용 · 네트워크 모름 — 끊기는 부르는 쪽이 한다.
// ==========================================================================

#include <string>
#include <unordered_map>

#include <Identity/GameIds.h>

class CWorldPresence
{
public:
    struct Zone
    {
        SessionId    session = NO_SESSION;
        ZoneServerId id{ 0 };
        int          maxClients = 0;
        std::string  wsAddr;
    };

    // 등록. 같은 번호로 다른 연결이 등록돼 있으면 옛 연결을 지우고 그 세션을 돌려준다(끊기는 부르는 쪽).
    //   거절하면 존 서버가 재시작한 뒤 옛 연결이 끊길 때까지(코어 세션 시한 60초) 등록하지 못한다
    SessionId Register(SessionId session, ZoneServerId id, int maxClients, const std::string& wsAddr);

    // 끊김 — 등록된 연결이 아니면 false
    bool OnDisconnected(SessionId session);

    bool IsRegistered(SessionId session) const { return _zones.count(ToInt(session)) != 0; }
    int  ZoneCount() const { return static_cast<int>(_zones.size()); }

private:
    std::unordered_map<int64_t, Zone> _zones;   // 세션 번호로 찾는다 — 패킷이 세션으로 온다
};
