#pragma once
// ==========================================================================
// CWorldMetrics — 월드 서버 지표 값과 지표 글자
//
//   값을 시작부터 0 으로 내보낸다 — 줄이 없으면 「0」과 「못 읽음」을 가르지 못한다.
//   세기는 로직 스레드 · 읽기는 지표 스레드라 값마다 원자 값이다.
// ==========================================================================

#include <atomic>
#include <cstdint>
#include <sstream>

#include <Protocol/Protocol.h>   // PartyNotice · PartyAskKind

// 파티원이 빠진 까닭 — 지표 칸(mmo_party_member_removed_total{why}). 끊김은 빠짐이 아니다(오프라인으로 남는다)
enum class PartyLeave : uint8_t
{
    LEAVE,       // 스스로 나감(C2S_PARTY_LEAVE)
    KICKED,      // 파티장이 내보냄
    DISBANDED,   // 해산(파티장이 해산 · 한 명만 남음 · 전원 오프라인)
    COUNT
};

// 요청이 끝난 까닭 — 지표 칸(mmo_party_requests_ended_total{why}). 등식: Σ 요청 = Σ 끝남 + 지금 요청 표 크기
enum class PartyRequestEnd : uint8_t
{
    ACCEPTED,    // 받은 사람이 수락(가입이 됐든 인원 상한에 막혔든 — 답이 온 것)
    DECLINED,    // 받은 사람이 거절
    EXPIRED,     // 시간 지남(CPartySystem::ASK_TIMEOUT_MS)
    CANCELLED,   // 무효 — 낀 사람의 처지가 바뀜
    COUNT
};

class CWorldMetrics
{
public:
    // 파티 — 등식 셋(조용해진 뒤): created = ended + active · memberAdded = Σ memberRemoved + members · Σ requests = Σ requestsEnded + pending
    struct PartyCounters
    {
        std::atomic<int64_t> active{ 0 };          // [게이지] 지금 파티 수
        std::atomic<int64_t> members{ 0 };         // [게이지] 지금 파티원 수(오프라인 포함)
        std::atomic<int64_t> membersOnline{ 0 };   // [게이지] 지금 접속 중인 파티원 수
        std::atomic<int64_t> pending{ 0 };         // [게이지] 지금 요청 표 크기
        std::atomic<int64_t> created{ 0 };
        std::atomic<int64_t> ended{ 0 };
        std::atomic<int64_t> memberAdded{ 0 };
        std::atomic<int64_t> memberRemoved[static_cast<int>(PartyLeave::COUNT)]{};
        std::atomic<int64_t> requests[static_cast<int>(PartyAskKind::COUNT)]{};
        std::atomic<int64_t> requestsEnded[static_cast<int>(PartyRequestEnd::COUNT)]{};
        std::atomic<int64_t> notices[static_cast<int>(PartyNotice::COUNT)]{};
        std::atomic<int64_t> infoSent{ 0 };
        std::atomic<int64_t> memberSent{ 0 };
        std::atomic<int64_t> askSent{ 0 };
        std::atomic<int64_t> askEndSent{ 0 };
    } _party;

    void SetPartyMembersOnline(int count) { _party.membersOnline.store(count, std::memory_order_relaxed); }

    void SetZones(int count) { _zones.store(count, std::memory_order_relaxed); }
    void SetOnline(int count) { _online.store(count, std::memory_order_relaxed); }
    void SetChannelRows(int count) { _channelRows.store(count, std::memory_order_relaxed); }

    void Write(std::ostringstream& ss) const;

private:
    std::atomic<int64_t> _zones{ 0 };
    std::atomic<int64_t> _online{ 0 };
    std::atomic<int64_t> _channelRows{ 0 };
};
