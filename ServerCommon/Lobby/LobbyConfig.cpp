#include "LobbyConfig.h"

#include <Common/ErrorLog.h>
#include <Config/IniFile.h>

bool LobbyConfig::Read(const CIniFile& ini, std::string& error)
{
    const auto required = [&](const char* section, const char* key, int& value)
    {
        value = ini.GetInt(section, key, 0);
        if (value > 0)
            return true;
        error = std::string("[") + section + "] " + key + " 가 없거나 틀림: " + std::to_string(value);
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
    const auto text = [&](const char* key, std::string& value)
    {
        value = ini.GetString("DB", key, "");
        if (!value.empty())
            return true;
        error = std::string("[DB] ") + key + " 가 없음";
        return false;
    };

    if (!required("Server", "Port", port) || !required("Server", "MonitorPort", monitorPort))
        return false;
    if (!positive("Server", "MaxClients", maxClients) || !positive("Server", "WorkerThreads", workerThreads)
        || !positive("Server", "LogicPeriodMs", logicPeriodMs))
        return false;

    db.connectTimeoutSec = 3;
    db.rwTimeoutSec = 5;
    if (!text("Host", db.host) || !required("DB", "Port", db.port) || !text("User", db.user)
        || !text("Password", db.password) || !text("Database", db.database))
        return false;
    return positive("DB", "Workers", dbWorkers) && positive("DB", "QueueMax", dbQueueMax)
        && positive("DB", "ConnectTimeoutSec", db.connectTimeoutSec) && positive("DB", "RwTimeoutSec", db.rwTimeoutSec);
}

bool LobbyConfig::CheckSessionSlots(int reserve, std::string& error) const
{
    if (maxClients + reserve <= MAX_SESSION_SLOTS)
        return true;
    error = "MaxClients(" + std::to_string(maxClients) + ") + SessionReserve(" + std::to_string(reserve)
        + ") 가 " + std::to_string(MAX_SESSION_SLOTS) + " 를 넘음";
    return false;
}

void LobbyConfig::Print() const
{
    SLOG_INFO("  Server : port {} · monitor {} · max {} · workers {} · logic {}ms",
              port, monitorPort, maxClients, workerThreads, logicPeriodMs);
    SLOG_INFO("  DB     : {}:{} {} {} · workers {} · queue {}",
              db.host, db.port, db.user, db.database, dbWorkers, dbQueueMax);
}
