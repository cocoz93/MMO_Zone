#pragma once
// ==========================================================================
// CWorldPresence — 월드 서버의 접속 장부: 등록된 존 서버 · 접속 중인 사람
//
//   존 서버 목록의 정본은 실제로 붙어 있는 연결이다. 연결이 끊기면 그 존 서버는 없는 것으로 친다.
//   사람 줄은 파티 · 이름 찾기 · 우편 신호에 쓴다. 고르기의 중복 판정과 삭제 판정은 캐릭터 서버의 장부가 맡는다.
//   로직 스레드 전용 · 네트워크 모름 — 끊기는 부르는 쪽이 한다.
// ==========================================================================

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <Identity/GameIds.h>
#include <Protocol/ServerLinkProtocol.h>

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

    struct Person
    {
        SessionId      zoneSession = NO_SESSION;
        ZoneServerId   zoneId{ 0 };
        CharacterId    characterId = NO_CHARACTER;
        AccountId      accountId = NO_ACCOUNT;
        int32_t        level = 0;
        int32_t        job = 0;
        uint8_t        displayChar = 0;
        uint8_t        colorIndex = 0;
        std::u16string name;
    };

    // 등록. 같은 번호로 다른 연결이 등록돼 있으면 옛 연결을 지우고 그 세션을 돌려준다(끊기는 부르는 쪽).
    //   거절하면 존 서버가 재시작한 뒤 옛 연결이 끊길 때까지(코어 세션 시한 60초) 등록하지 못한다.
    //   removed 가 있으면 옛 연결의 줄에서 지운 캐릭터 번호를 담는다
    SessionId Register(SessionId session, ZoneServerId id, int maxClients, const std::string& wsAddr, std::vector<CharacterId>* removed = nullptr);

    // 끊김 — 그 존 서버의 사람 줄을 지운다. 등록된 연결이 아니면 false.
    //   removed 가 있으면 지운 캐릭터 번호를 담는다(파티의 오프라인 표시)
    bool OnDisconnected(SessionId session, std::vector<CharacterId>* removed = nullptr);

    bool IsRegistered(SessionId session) const { return _zones.count(ToInt(session)) != 0; }

    // 그 연결의 존 서버 번호 — 등록된 연결이 아니면 0
    ZoneServerId ZoneOf(SessionId session) const
    {
        const auto it = _zones.find(ToInt(session));
        return it != _zones.end() ? it->second.id : ZoneServerId{ 0 };
    }

    // 입장 통지 · 재등록 명단. 다른 존 서버의 줄이 있으면 나중에 알린 쪽으로 덮어쓴다(채널 바꾸기의 3007 이 3008 보다 먼저 올 수 있다). 등록 안 된 연결이면 false
    bool Enter(SessionId session, const LinkOnlineEntry& entry);

    // 퇴장 통지 — 보낸 존 서버가 그 줄의 존 서버일 때만 지운다. 지웠으면 true
    bool Leave(SessionId session, CharacterId characterId);

    // 레벨은 입장 뒤에도 바뀐다 — 파티 요청이 지금 값을 실어 온다. 없는 사람이면 아무것도 안 한다
    void SetLevel(CharacterId characterId, int32_t level);

    const Person* Find(CharacterId characterId) const;
    const Person* FindByName(const std::u16string& name) const;

    // 등록된 존 서버마다 fn(const Zone&) — 계정 우편 도착 신호가 전부에 넘긴다(CMailSignalRelay). fn 안에서 등록 · 끊김을 부르면 안 된다
    template <typename Fn>
    void ForEachZone(Fn&& fn) const
    {
        for (const auto& [key, zone] : _zones)
            fn(zone);
    }

    int ZoneCount() const { return static_cast<int>(_zones.size()); }
    int OnlineCount() const { return static_cast<int>(_people.size()); }

    // 패킷의 이름 칸을 글자열로 — 6글자를 꽉 채운 이름은 끝 표시가 없다
    static std::u16string NameOf(const char16_t (&name)[LINK_NAME_CHARS]);

private:
    void ErasePerson(std::unordered_map<int64_t, Person>::iterator it);

    std::unordered_map<int64_t, Zone> _zones;     // 세션 번호로 찾는다 — 패킷이 세션으로 온다
    std::unordered_map<int64_t, Person> _people;  // 캐릭터 번호로 찾는다
    std::unordered_map<std::u16string, int64_t> _byName;
};
