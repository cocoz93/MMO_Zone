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

#include "AccountRules.h"

class CAccountMetrics
{
public:
    void CountLogin(AccountLoginResult result) { _login[static_cast<int>(result)].fetch_add(1, std::memory_order_relaxed); }
    void CountToken(TokenCheckResult result) { _token[static_cast<int>(result)].fetch_add(1, std::memory_order_relaxed); }
    void CountCreated() { _created.fetch_add(1, std::memory_order_relaxed); }
    void CountIssued() { _issued.fetch_add(1, std::memory_order_relaxed); }
    void SetPending(int count) { _pending.store(count, std::memory_order_relaxed); }

    // 계정 우편 — 답이 없는 받음 · 풀기도 DB 결과로 센다(0행 = gone)
    void CountMail(AccMailOp op, LinkMailResult result)
    {
        const int index = static_cast<int>(result);
        if (index >= LINK_MAIL_RESULT_ACCOUNT_COUNT)
            return;   // OFFLINE 은 월드 서버만 만든다 — 계정 서버가 부를 길이 없다(칸 밖을 안 쓰게만 막는다)
        _mail[static_cast<int>(op)][index].fetch_add(1, std::memory_order_relaxed);
    }
    void AddMailSentRows(int64_t rows) { _mailSentRows.fetch_add(rows, std::memory_order_relaxed); }
    // 도착 신호(LINK_ACCMAIL_ARRIVED) — 보냄 · 못 보냄(월드 서버 끊김)
    void CountMailSignal(bool sent) { (sent ? _mailSignalSent : _mailSignalNotSent).fetch_add(1, std::memory_order_relaxed); }

    void Write(std::ostringstream& ss) const;

private:
    static constexpr int LOGIN_RESULTS = static_cast<int>(AccountLoginResult::COUNT);

    std::atomic<int64_t> _login[LOGIN_RESULTS]{};
    std::atomic<int64_t> _token[TOKEN_CHECK_RESULT_COUNT]{};
    std::atomic<int64_t> _created{ 0 };
    std::atomic<int64_t> _issued{ 0 };
    std::atomic<int64_t> _pending{ 0 };
    std::atomic<int64_t> _mail[ACC_MAIL_OP_COUNT][LINK_MAIL_RESULT_ACCOUNT_COUNT]{};
    std::atomic<int64_t> _mailSentRows{ 0 };
    std::atomic<int64_t> _mailSignalSent{ 0 };
    std::atomic<int64_t> _mailSignalNotSent{ 0 };
};
