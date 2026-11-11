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

class CWorldMetrics
{
public:
    void SetZones(int count) { _zones.store(count, std::memory_order_relaxed); }
    void SetOnline(int count) { _online.store(count, std::memory_order_relaxed); }

    void Write(std::ostringstream& ss) const;

private:
    std::atomic<int64_t> _zones{ 0 };
    std::atomic<int64_t> _online{ 0 };
};
