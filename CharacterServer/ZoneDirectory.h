#pragma once
// ==========================================================================
// CZoneDirectory — 등록된 존 서버 · 정원 · 접속 중인 캐릭터 · 입장 대기 장부
//
//   존 서버 목록의 정본은 실제로 떠 있는 서버다. 연결이 끊기면 그 존 서버는 없는 것으로 친다.
//   끊긴 동안에는 장부에 그 존 서버 캐릭터가 없으므로 삭제가 통과할 수 있다 — 지움은 표시뿐이라
//   저장이 깨지지 않으므로 허용하고, 다시 등록할 때 명단으로 복원한다(플랜 4절).
//
//   입장 대기 — 입장 예고가 성공한 캐릭터. 만료까지 삭제를 막는다. 안 막으면 입장 번호를 받은 뒤
//   다른 탭에서 지운 캐릭터가 게임에 들어간다. mmo_char_online 에는 안 센다.
//
//   응답 대기는 CLinkRequests 가 맡는다. 로직 스레드 전용 · 네트워크 모름 · 시각은 인자로.
// ==========================================================================

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <Identity/GameIds.h>

class CZoneDirectory
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

    // 끊김 — 그 존 서버의 접속 · 입장 대기 장부를 지운다. 등록된 연결이 아니면 false
    bool OnDisconnected(SessionId session);

    bool IsRegistered(SessionId session) const;

    // 접속 통지 · 재등록 명단. 입장 대기였다면 접속 중으로 바뀐다
    void Enter(SessionId session, CharacterId characterId);
    void Leave(SessionId session, CharacterId characterId);

    // 이번에는 한 대뿐이다 — 번호가 가장 작은 것. 맵에 따라 대를 고르는 규칙은 여러 대를 띄우는 플랜에서 정한다
    const Zone* PickZone() const;

    // 입장 예고 성공. 같은 계정의 앞 항목을 지운다(존 서버의 대기 명단과 같은 규칙)
    void AddEntering(SessionId zoneSession, CharacterId characterId, AccountId accountId, int64_t expireMs);

    // 접속 중이거나 입장 번호가 살아 있으면 true — 삭제 판정
    bool IsInGame(CharacterId characterId, int64_t nowMs) const;

    void Sweep(int64_t nowMs);   // 만료된 입장 대기 지우기

    int ZoneCount() const { return static_cast<int>(_zones.size()); }
    int OnlineCount() const;
    int EnteringCount() const { return static_cast<int>(_entering.size()); }

private:
    struct Entering
    {
        int64_t zoneSession = 0;
        int64_t accountId = 0;
        int64_t expireMs = 0;
    };

    struct ZoneState
    {
        Zone zone;
        std::unordered_set<int64_t> online;
    };

    void EraseEntering(int64_t characterId);

    std::unordered_map<int64_t, ZoneState> _zones;   // 세션 번호로 찾는다 — 패킷이 세션으로 온다
    std::unordered_map<int64_t, Entering> _entering;
    std::unordered_map<int64_t, int64_t> _enteringByAccount;
};
