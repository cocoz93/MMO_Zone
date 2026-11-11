#include "WorldServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서

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

    WorldServerConfig config;
    if (!config.Load(dir + "WorldServerConfig.ini", error))
    {
        SLOG_ERROR("[Error] WorldServerConfig.ini: {}", error);
        return 1;
    }

    ServerLinkConfig link;
    if (!link.Load(dir + "ServerLink.ini", error))
    {
        SLOG_ERROR("[Error] ServerLink.ini: {}", error);
        return 1;
    }
    if (!link.HasWorld())
    {
        SLOG_ERROR("[Error] ServerLink.ini: [World] InternalAddr 가 없음 — 월드 서버는 이 주소로 접속을 기다린다");
        return 1;
    }
    if (!config.CheckSessionSlots(link.sessionReserve, error))
    {
        SLOG_ERROR("[Error] {}", error);
        return 1;
    }

    SLOG_INFO("=== World Server ===");
    config.Print();

    CWorldServer server(config, link);
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
