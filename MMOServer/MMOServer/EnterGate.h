// ==========================================================================
// CEnterGate — 입장 대기 명단: 입장 번호 적기(예고) · 확인하고 지우기(입장) · 만료
//
// [책임]
//  - 캐릭터 서버가 예고한 「입장 번호 → 캐릭터 번호 · 계정 번호 · 만료」를 메모리에 든다
//  - 계정당 번호 하나 · 정원(접속자 + 아직 안 지난 번호가 MaxClients 에 닿으면 거절) · 일회용(입장하면 지운다)
//  - 만료된 번호를 잠시 더 들고 있어 「늦게 왔다(Expired)」와 「그런 번호 없다(NotFound)」를 가른다
//
// [경계]
//  입장 요청 처리·끊기·지표는 CEntryHandler, 예고를 받고 답하는 일은 CZoneLink 다. 여기는 표만 든다.
//  존 서버를 다시 띄우면 명단이 사라진다 — 이전 번호는 NotFound 고, 웹은 캐릭터 창으로 돌아가 다시 고른다.
//
// [모르는 것]
//  패킷 · 세션 · 지표 · DB. 그래서 게임 없이 시험한다(Tests/EnterGateTest.cpp). 접근은 게임 스레드 단독.
// ==========================================================================
#pragma once

#include <Identity/GameIds.h>
#include <cstdint>
#include <string>
#include <unordered_map>

// 입장 번호 판정 — 이 갈래가 곧 지표 mmo_enter_code_total{result} 의 줄이다(시작부터 전부 0 으로 나간다)
enum class EnterCodeResult : uint8_t
{
    Ok,         // 명단에 있고 시간 안이다 — 지웠다
    BadSize,    // 칸 모양이 틀렸다(뒤쪽 칸에 값이 있거나 앞이 비었다) · 빈 번호인데 시험 입장 길이 꺼져 있다
    NotFound,   // 명단에 없다 — 위조 · 이미 씀 · 만료 뒤 보관도 끝남 · 다른 존 서버 번호 · 존 서버 재시작 전 번호
    Expired,    // 명단에 있지만 시간이 지났다 — 쌓이면 2분을 5분으로 올리는 근거다(사용자 결정 11-04)
    COUNT
};

inline const char* EnterCodeResultName(EnterCodeResult result)
{
    static_assert(static_cast<int>(EnterCodeResult::COUNT) == 4, "EnterCodeResult 를 늘렸으면 이름도 붙일 것");
    switch (result)
    {
    case EnterCodeResult::Ok:       return "ok";
    case EnterCodeResult::BadSize:  return "bad_size";
    case EnterCodeResult::NotFound: return "not_found";
    case EnterCodeResult::Expired:  return "expired";
    case EnterCodeResult::COUNT:    break;
    }
    return "?";
}

class CEnterGate
{
public:
    static constexpr int CODE_BYTES = 64;   // 패킷 칸 크기 — C2S_ENTER_GAME.ticket · LINK_ENTER_NOTICE_REQ.enterCode

    // 만료된 번호를 더 들고 있는 시간 — 바로 지우면 Expired 가 거의 안 잡혀 2분을 5분으로 올릴 근거가 사라진다.
    //   60 은 근거 없이 고른 값이다(플랜 4절).
    static constexpr int EXPIRED_KEEP_SEC = 60;

    static constexpr int TTL_SEC_DEFAULT = 120;   // 유효 시간 기본값 — INI [Zone] EnterCodeTtlSec 가 없을 때

    struct Ticket
    {
        CharacterId characterId = NO_CHARACTER;
        AccountId   accountId   = NO_ACCOUNT;
    };

    // 코드 이곳저곳이 같은 시계를 쓰게 한 자리에 둔다 — 벽시계가 아니라 단조 시계다(시간을 고쳐도 만료가 안 흔들린다)
    static int64_t NowMs();

    // 칸 전체가 0 인가 — 입장 번호를 안 낸 접속(시험 입장 길 후보)
    static bool IsEmptyCode(const char (&code)[CODE_BYTES]);

    void SetTtlSec(int ttlSec) { _ttlMs = int64_t{ ttlSec } * 1000; }

    // 예고를 적는다. false = 정원이 찼다 — 아무것도 안 바꾼다(그 계정의 앞 번호도 그대로).
    //   같은 계정의 앞 번호는 지운다 — 계정당 하나. 지워질 번호는 정원 계산에서 뺀다(다시 고른 사람이 자기 번호 때문에 거절당하면 안 된다).
    //   onlineCount = 지금 접속자 수(게임 중 + 불러오는 중 + 입장 대기) · maxClients = 웹 정원.
    //   칸 모양은 안 본다 — 캐릭터 서버가 SecureRandom::MakeCode 로 만든 값이다(서버 간 연결은 내부망 전제).
    bool AddNotice(const char (&code)[CODE_BYTES], CharacterId characterId, AccountId accountId,
                   int onlineCount, int maxClients, int64_t nowMs);

    // 입장 — Ok 면 out 을 채우고 그 번호를 지운다(일회용). 빈 칸은 BadSize 다: 빈 번호를 받아 줄지는 부르는 쪽이 먼저 정한다.
    EnterCodeResult Consume(const char (&code)[CODE_BYTES], int64_t nowMs, Ticket& out);

    // 시간이 지난 번호를 「아직 안 지난 수」에서 빼고, 보관까지 끝난 번호를 지운다.
    //   부르는 곳 = 동접 게이지 주기(약 0.5초) · 예고를 적을 때. 명단은 정원 안이라 훑는 비용이 작다.
    void Sweep(int64_t nowMs);

    int LiveCount() const { return _liveCount; }             // 아직 안 지난 번호 수 — 정원 · mmo_enter_code_pending
    int StoredCount() const { return static_cast<int>(_byCode.size()); }   // 지난 뒤 보관 중인 번호까지 — 시험용

private:
    struct Entry
    {
        CharacterId characterId;
        AccountId   accountId;
        int64_t     expireMs;
        bool        live;       // 아직 _liveCount 에 들어 있나 — Sweep 이 시간이 지난 것을 빼며 내린다
    };

    using EntryMap = std::unordered_map<std::string, Entry>;
    void Erase(EntryMap::iterator it);

    EntryMap _byCode;   // 난수 자리 16바이트 → 내용
    std::unordered_map<int64_t, std::string> _codeByAccount;   // 계정당 하나를 찾는 표
    int     _liveCount = 0;
    int64_t _ttlMs     = int64_t{ TTL_SEC_DEFAULT } * 1000;
};
