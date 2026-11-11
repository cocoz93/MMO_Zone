#include "WorldServerConfig.h"

#include <Common/ErrorLog.h>
#include <Config/IniFile.h>

bool WorldServerConfig::Load(const std::string& path, std::string& error)
{
    CIniFile ini;
    if (!ini.Load(path))
    {
        error = "파일을 못 읽음: " + path;
        return false;
    }

    const auto required = [&](const char* key, int& value)
    {
        value = ini.GetInt("Server", key, 0);
        if (value > 0)
            return true;
        error = std::string("[Server] ") + key + " 가 없거나 틀림: " + std::to_string(value);
        return false;
    };
    const auto positive = [&](const char* section, const char* key, int& value)
    {
        value = ini.GetInt(section, key, value);
        if (value > 0)
            return true;
        error = std::string("[") + section + "] " + key + " 는 1 이상이어야 함: " + std::to_string(value);
        return false;
    };

    return required("Port", port) && required("MonitorPort", monitorPort)
        && positive("Server", "MaxClients", maxClients)
        && positive("Server", "WorkerThreads", workerThreads)
        && positive("Server", "LogicPeriodMs", logicPeriodMs)
        && positive("World", "WorldId", worldId);
}

bool WorldServerConfig::CheckSessionSlots(int reserve, std::string& error) const
{
    if (maxClients + reserve <= MAX_SESSION_SLOTS)
        return true;
    error = "MaxClients(" + std::to_string(maxClients) + ") + SessionReserve(" + std::to_string(reserve)
        + ") 가 " + std::to_string(MAX_SESSION_SLOTS) + " 를 넘음";
    return false;
}

void WorldServerConfig::Print() const
{
    SLOG_INFO("  Server : port {} · monitor {} · max {} · workers {} · logic {}ms",
              port, monitorPort, maxClients, workerThreads, logicPeriodMs);
    SLOG_INFO("  World  : id {}", worldId);
}
