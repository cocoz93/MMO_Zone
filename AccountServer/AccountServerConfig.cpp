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
    if (!lobby.Read(ini, path, error))
        return false;

    authTokenTtlSec = ini.GetInt("Account", "AuthTokenTtlSec", authTokenTtlSec);
    if (authTokenTtlSec <= 0)
    {
        error = "[Account] AuthTokenTtlSec 는 1 이상이어야 함: " + std::to_string(authTokenTtlSec);
        return false;
    }
    allowDevLogin = (ini.GetInt("Account", "AllowDevLogin", allowDevLogin ? 1 : 0) != 0);

    mailExpireDays = ini.GetInt("Mail", "MailExpireDays", mailExpireDays);
    if (mailExpireDays < 1 || mailExpireDays > 365)   // 존 서버와 같은 범위(ServerConfig.h mailExpireDays)
    {
        error = "[Mail] MailExpireDays 는 1 ~ 365 여야 함: " + std::to_string(mailExpireDays);
        return false;
    }
    return true;
}

void AccountServerConfig::Print() const
{
    lobby.Print();
    SLOG_INFO("  Account: token ttl {}s · dev login {}", authTokenTtlSec, allowDevLogin ? "on" : "off");
    SLOG_INFO("  Mail: account mail expires in {} days", mailExpireDays);
}
