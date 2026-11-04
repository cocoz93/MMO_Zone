#include "AccountServerConfig.h"

#include <Common/ErrorLog.h>
#include <Config/IniFile.h>

bool AccountServerConfig::Load(const std::string& path, std::string& error)
{
    CIniFile ini;
    if (!ini.Load(path))
    {
        error = "파일을 못 읽음: " + path;
        return false;
    }
    if (!lobby.Read(ini, error))
        return false;

    authTokenTtlSec = ini.GetInt("Account", "AuthTokenTtlSec", authTokenTtlSec);
    if (authTokenTtlSec <= 0)
    {
        error = "[Account] AuthTokenTtlSec 는 1 이상이어야 함: " + std::to_string(authTokenTtlSec);
        return false;
    }
    allowDevLogin = (ini.GetInt("Account", "AllowDevLogin", allowDevLogin ? 1 : 0) != 0);
    return true;
}

void AccountServerConfig::Print() const
{
    lobby.Print();
    SLOG_INFO("  Account: token ttl {}s · dev login {}", authTokenTtlSec, allowDevLogin ? "on" : "off");
}
