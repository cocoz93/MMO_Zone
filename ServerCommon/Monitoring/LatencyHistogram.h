#pragma once
// ==========================================================================
// CLatencyHistogram — 구간별 횟수 · 합계 (mmo_link_reply_seconds · mmo_db_query_seconds)
//
//   구간 = 1·5·10·50·100·500·1000·3000ms. 3000 은 ReplyTimeoutSec=3 이고 나머지는 근거 없이 고른 값이다.
//   기존 구간 표는 GameLoopCounters 안에 고정돼 있어(MonitorManager.h) 따로 둔다.
//   기록은 아무 스레드 · 읽기는 지표 스레드. 값마다 원자 값이다(서로 어긋나도 지표 한 번의 오차다).
// ==========================================================================

#include <atomic>
#include <cstdint>
#include <iomanip>
#include <sstream>

class CLatencyHistogram
{
public:
    static constexpr int BOUND_COUNT = 8;

    void Record(double ms)
    {
        _sumUs.fetch_add(static_cast<int64_t>(ms * 1000.0), std::memory_order_relaxed);
        _count.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < BOUND_COUNT; ++i)
        {
            if (ms <= BoundMs(i))
            {
                _buckets[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        _buckets[BOUND_COUNT].fetch_add(1, std::memory_order_relaxed);
    }

    int64_t Count() const { return _count.load(std::memory_order_relaxed); }

    // 값 줄만 쓴다 — 머리 두 줄(# HELP · # TYPE histogram)은 부르는 쪽 몫이다. 라벨이 여럿인 줄을 한 묶음으로 잇기 위함이다.
    //   labels = 「request="token_check"」 꼴, 라벨이 없으면 빈 문자열
    void WriteSeries(std::ostringstream& ss, const char* name, const char* labels) const
    {
        static const char* const LE[BOUND_COUNT] = { "0.001", "0.005", "0.01", "0.05", "0.1", "0.5", "1", "3" };

        const bool hasLabels = labels != nullptr && labels[0] != '\0';
        int64_t cumulative = 0;
        for (int i = 0; i < BOUND_COUNT; ++i)
        {
            cumulative += _buckets[i].load(std::memory_order_relaxed);
            ss << name << "_bucket{";
            if (hasLabels)
                ss << labels << ",";
            ss << "le=\"" << LE[i] << "\"} " << cumulative << "\n";
        }
        cumulative += _buckets[BOUND_COUNT].load(std::memory_order_relaxed);
        ss << name << "_bucket{";
        if (hasLabels)
            ss << labels << ",";
        ss << "le=\"+Inf\"} " << cumulative << "\n";

        ss << name << "_sum";
        if (hasLabels)
            ss << "{" << labels << "}";
        ss << " " << std::fixed << std::setprecision(6)
           << (static_cast<double>(_sumUs.load(std::memory_order_relaxed)) / 1000000.0) << std::defaultfloat << "\n";

        ss << name << "_count";
        if (hasLabels)
            ss << "{" << labels << "}";
        ss << " " << cumulative << "\n";
    }

private:
    static constexpr double BoundMs(int index)
    {
        constexpr double BOUNDS[BOUND_COUNT] = { 1, 5, 10, 50, 100, 500, 1000, 3000 };
        return BOUNDS[index];
    }

    std::atomic<int64_t> _buckets[BOUND_COUNT + 1]{};
    std::atomic<int64_t> _sumUs{ 0 };
    std::atomic<int64_t> _count{ 0 };
};
