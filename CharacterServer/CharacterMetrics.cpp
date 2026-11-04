#include "CharacterMetrics.h"

#include <Monitoring/MetricsText.h>

#include "CharacterRules.h"

namespace
{
    template <typename TResult, size_t N>
    void WriteKind(std::ostringstream& ss, const char* kind, const std::atomic<int64_t> (&counts)[N])
    {
        for (size_t i = 0; i < N; ++i)
        {
            ss << "mmo_char_request_total{kind=\"" << kind << "\",result=\"" << CCharacterRules::ResultName(static_cast<TResult>(i)) << "\"} "
               << counts[i].load(std::memory_order_relaxed) << "\n";
        }
    }
}

void CCharacterMetrics::Write(std::ostringstream& ss) const
{
    MetricsText::WriteHead(ss, "mmo_char_login_total", "Character server login replies by result", "counter");
    for (int i = 0; i < static_cast<int>(CharLoginResult::COUNT); ++i)
    {
        ss << "mmo_char_login_total{result=\"" << CCharacterRules::ResultName(static_cast<CharLoginResult>(i)) << "\"} "
           << _login[i].load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    MetricsText::WriteHead(ss, "mmo_char_request_total", "Character requests by kind and result", "counter");
    WriteKind<CharCreateResult>(ss, "create", _create);
    WriteKind<CharDeleteResult>(ss, "delete", _delete);
    WriteKind<CharRestoreResult>(ss, "restore", _restore);
    WriteKind<CharSelectResult>(ss, "select", _select);
    ss << "\n";

    MetricsText::WriteGauge(ss, "mmo_char_online", "Characters in game on registered zone servers", _online.load(std::memory_order_relaxed));
    MetricsText::WriteGauge(ss, "mmo_zone_registered", "Registered zone servers", _zones.load(std::memory_order_relaxed));
}
