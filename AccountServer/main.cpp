#include "AccountServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서

#include <Common/ErrorLog.h>
#include <Common/Logger.h>
#include <Link/ServerLinkConfig.h>
#include <Process/ShutdownWait.h>
#include <string>

#include "Platform/Platform.h"

int main()
{
    shared::LoggerGuard loggerGuard;

    const std::string dir = Platform::GetExecutableDir();
    std::string error;

    AccountServerConfig config;
    if (!config.Load(dir + "AccountServerConfig.ini", error))
    {
        SLOG_ERROR("[Error] AccountServerConfig.ini: {}", error);
        return 1;
    }

    ServerLinkConfig link;
    if (!link.Load(dir + "ServerLink.ini", error))
    {
        SLOG_ERROR("[Error] ServerLink.ini: {}", error);
        return 1;
    }
    if (!config.lobby.CheckSessionSlots(link.sessionReserve, error))
    {
        SLOG_ERROR("[Error] {}", error);
        return 1;
    }

    SLOG_INFO("=== Account Server ===");
    config.Print();

    CAccountServer server(config, link);
    CShutdownWait::Install();

    if (!server.Start())
    {
        SLOG_ERROR("[Error] Server Start failed");
        return 1;
    }

    CShutdownWait::Wait();
    server.Stop();

    SLOG_INFO("Server shutdown complete");
    CShutdownWait::MarkCleanedUp();
    return 0;
}
