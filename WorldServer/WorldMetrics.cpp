#include "WorldMetrics.h"

#include <Monitoring/MetricsText.h>

void CWorldMetrics::Write(std::ostringstream& ss) const
{
    MetricsText::WriteGauge(ss, "mmo_world_zones", "Zone servers registered with the world server", _zones.load(std::memory_order_relaxed));
    MetricsText::WriteGauge(ss, "mmo_world_online", "Characters in the world server presence ledger", _online.load(std::memory_order_relaxed));
}
