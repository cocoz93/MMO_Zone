#pragma once
// ==========================================================================
// MetricsText — Prometheus 텍스트 형식 쓰기 도우미 (세 서버가 같은 꼴로 쓴다)
// ==========================================================================

#include <cstdint>
#include <sstream>

namespace MetricsText
{
    inline void WriteCounter(std::ostringstream& ss, const char* name, const char* help, int64_t value)
    {
        ss << "# HELP " << name << " " << help << "\n";
        ss << "# TYPE " << name << " counter\n";
        ss << name << " " << value << "\n\n";
    }

    inline void WriteGauge(std::ostringstream& ss, const char* name, const char* help, int64_t value)
    {
        ss << "# HELP " << name << " " << help << "\n";
        ss << "# TYPE " << name << " gauge\n";
        ss << name << " " << value << "\n\n";
    }

    // 라벨이 있는 지표의 머리 두 줄 — 값 줄은 부르는 쪽이 이어 쓰고 빈 줄로 닫는다
    inline void WriteHead(std::ostringstream& ss, const char* name, const char* help, const char* type)
    {
        ss << "# HELP " << name << " " << help << "\n";
        ss << "# TYPE " << name << " " << type << "\n";
    }
}
