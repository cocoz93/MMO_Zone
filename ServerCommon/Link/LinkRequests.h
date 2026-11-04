#pragma once
// ==========================================================================
// CLinkRequests — 서버 간 요청의 응답 대기: 요청 번호 발급 · 시한 · 짝 맞추기 · 응답 시간 지표
//
//   캐릭터 서버가 토큰 확인과 입장 예고에 쓴다. 로직 스레드 전용이라 락이 없고, 지표 값만 원자 값이다.
//   시각은 인자로 받는다 — 시험이 3초를 기다리지 않고 시한 갈래를 본다.
//   응답 내용은 모른다 — 받은 패킷을 void* 로 넘기고 해석은 등록한 함수가 한다.
//
// [지표] mmo_link_reply_seconds{request} · mmo_link_timeout_total{request}
// ==========================================================================

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <sstream>
#include <unordered_map>

#include <Monitoring/LatencyHistogram.h>

enum class LinkRequestKind : int
{
    TokenCheck = 0,
    EnterNotice = 1,
    COUNT
};

inline const char* LinkRequestKindName(LinkRequestKind kind)
{
    switch (kind)
    {
    case LinkRequestKind::TokenCheck:  return "token_check";
    case LinkRequestKind::EnterNotice: return "enter_notice";
    case LinkRequestKind::COUNT:       break;
    }
    return "unknown";
}

class CLinkRequests
{
public:
    // reply == nullptr 이면 응답을 못 받았다(시한 초과 또는 연결 끊김)
    using DoneFn = std::function<void(const void* reply)>;

    explicit CLinkRequests(int replyTimeoutSec) : _timeoutMs(static_cast<int64_t>(replyTimeoutSec) * 1000) {}

    // 요청 번호를 받는다 — 이 번호를 패킷에 실어 보낸다. 0 은 안 나온다
    uint64_t Begin(LinkRequestKind kind, int64_t nowMs, DoneFn done);

    // 응답이 왔다 — 짝이 있으면 done(reply) 후 true. 시한이 지난 뒤 늦게 온 응답은 짝이 없어 false
    bool Complete(uint64_t requestId, int64_t nowMs, const void* reply);

    // 보낼 수 없었다 — 응답 시간에 안 넣고 지운다. done 은 안 부른다(부르는 쪽이 바로 답한다)
    void Cancel(uint64_t requestId);

    // 시한이 지난 것을 done(nullptr) 로 끝낸다. 끝낸 수를 돌려준다
    int ExpireDue(int64_t nowMs);

    // 연결이 끊겼다 — 그 종류의 대기를 전부 done(nullptr) 로 끝낸다. 시한까지 기다리지 않는다. 시한 초과로 세지 않는다
    int FailAll(LinkRequestKind kind);

    int PendingCount() const { return static_cast<int>(_pending.size()); }

    int64_t TimeoutCount(LinkRequestKind kind) const { return _timeouts[static_cast<int>(kind)].load(std::memory_order_relaxed); }
    int64_t ReplyCount(LinkRequestKind kind) const { return _latency[static_cast<int>(kind)].Count(); }

    // 지표 스레드 — 모든 갈래를 시작부터 0 으로 내보낸다
    void WriteMetrics(std::ostringstream& ss) const;

private:
    struct Pending
    {
        LinkRequestKind kind = LinkRequestKind::TokenCheck;
        int64_t sentMs = 0;
        DoneFn done;
    };

    struct Deadline
    {
        int64_t atMs = 0;
        uint64_t requestId = 0;
    };

    int64_t _timeoutMs;
    uint64_t _nextId = 1;
    std::unordered_map<uint64_t, Pending> _pending;
    std::deque<Deadline> _deadlines;   // 시한이 일정하므로 넣은 차례가 곧 만료 차례다. 응답이 온 번호는 꺼낼 때 짝이 없어 건너뛴다

    CLatencyHistogram _latency[static_cast<int>(LinkRequestKind::COUNT)];
    std::atomic<int64_t> _timeouts[static_cast<int>(LinkRequestKind::COUNT)]{};
};
