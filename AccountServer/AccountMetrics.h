#pragma once
// ==========================================================================
// CAccountMetrics — 계정 서버 지표 값과 지표 글자
//
//   모든 갈래를 시작부터 0 으로 내보낸다 — 줄이 처음 생기는 순간의 증가는 rate() 가 못 보고,
//   줄이 없으면 「0번」과 「못 읽음」을 가르지 못한다(플랜 6절).
//   세기는 로직 스레드 · 읽기는 지표 스레드라 값마다 원자 값이다.
// ==========================================================================

#include <atomic>
#include <cstdint>
#include <sstream>

#include <Protocol/AccountProtocol.h>
#include <Protocol/ServerLinkProtocol.h>

class CAccountMetrics
{
public:
    void CountLogin(AccountLoginResult result) { _login[static_cast<int>(result)].fetch_add(1, std::memory_order_relaxed); }
    void CountToken(TokenCheckResult result) { _token[static_cast<int>(result)].fetch_add(1, std::memory_order_relaxed); }
    void CountCreated() { _created.fetch_add(1, std::memory_order_relaxed); }
    void CountIssued() { _issued.fetch_add(1, std::memory_order_relaxed); }
    void SetPending(int count) { _pending.store(count, std::memory_order_relaxed); }

    void Write(std::ostringstream& ss) const;

private:
    static constexpr int LOGIN_RESULTS = static_cast<int>(AccountLoginResult::COUNT);

    std::atomic<int64_t> _login[LOGIN_RESULTS]{};
    std::atomic<int64_t> _token[TOKEN_CHECK_RESULT_COUNT]{};
    std::atomic<int64_t> _created{ 0 };
    std::atomic<int64_t> _issued{ 0 };
    std::atomic<int64_t> _pending{ 0 };
};
