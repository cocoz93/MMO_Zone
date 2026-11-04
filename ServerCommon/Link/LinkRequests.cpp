#include "LinkRequests.h"

#include <string>
#include <utility>
#include <vector>

#include <Monitoring/MetricsText.h>

uint64_t CLinkRequests::Begin(LinkRequestKind kind, int64_t nowMs, DoneFn done)
{
    const uint64_t requestId = _nextId++;
    Pending pending;
    pending.kind = kind;
    pending.sentMs = nowMs;
    pending.done = std::move(done);
    _pending.emplace(requestId, std::move(pending));
    _deadlines.push_back(Deadline{ nowMs + _timeoutMs, requestId });
    return requestId;
}

bool CLinkRequests::Complete(uint64_t requestId, int64_t nowMs, const void* reply)
{
    auto found = _pending.find(requestId);
    if (found == _pending.end())
        return false;

    Pending pending = std::move(found->second);
    _pending.erase(found);

    _latency[static_cast<int>(pending.kind)].Record(static_cast<double>(nowMs - pending.sentMs));
    pending.done(reply);
    return true;
}

void CLinkRequests::Cancel(uint64_t requestId)
{
    _pending.erase(requestId);
}

int CLinkRequests::ExpireDue(int64_t nowMs)
{
    int expired = 0;
    while (!_deadlines.empty() && _deadlines.front().atMs <= nowMs)
    {
        const uint64_t requestId = _deadlines.front().requestId;
        _deadlines.pop_front();

        auto found = _pending.find(requestId);
        if (found == _pending.end())
            continue;   // 이미 응답이 왔다

        Pending pending = std::move(found->second);
        _pending.erase(found);
        _timeouts[static_cast<int>(pending.kind)].fetch_add(1, std::memory_order_relaxed);
        pending.done(nullptr);
        ++expired;
    }
    return expired;
}

int CLinkRequests::FailAll(LinkRequestKind kind)
{
    // 함수가 또 요청을 시작해도 흔들리지 않게 먼저 뺀 뒤에 부른다
    std::vector<DoneFn> dones;
    for (auto it = _pending.begin(); it != _pending.end();)
    {
        if (it->second.kind == kind)
        {
            dones.push_back(std::move(it->second.done));
            it = _pending.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (DoneFn& done : dones)
        done(nullptr);
    return static_cast<int>(dones.size());
}

void CLinkRequests::WriteMetrics(std::ostringstream& ss) const
{
    constexpr int KIND_COUNT = static_cast<int>(LinkRequestKind::COUNT);

    MetricsText::WriteHead(ss, "mmo_link_reply_seconds", "Server-to-server request reply time", "histogram");
    for (int i = 0; i < KIND_COUNT; ++i)
    {
        const std::string labels = std::string("request=\"") + LinkRequestKindName(static_cast<LinkRequestKind>(i)) + "\"";
        _latency[i].WriteSeries(ss, "mmo_link_reply_seconds", labels.c_str());
    }
    ss << "\n";

    MetricsText::WriteHead(ss, "mmo_link_timeout_total", "Server-to-server requests that got no reply in time", "counter");
    for (int i = 0; i < KIND_COUNT; ++i)
    {
        ss << "mmo_link_timeout_total{request=\"" << LinkRequestKindName(static_cast<LinkRequestKind>(i)) << "\"} "
           << _timeouts[i].load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";
}
