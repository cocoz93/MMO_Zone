#include "ServerLinkConfig.h"

#include <cctype>

#include <Config/IniFile.h>

bool ServerLinkConfig::ParseAddress(const std::string& text, Address& out)
{
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size())
        return false;

    const std::string portText = text.substr(colon + 1);
    if (portText.size() > 5)
        return false;
    for (const char c : portText)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }

    const int port = std::stoi(portText);
    if (port < 1 || port > 65535)
        return false;

    out.ip = text.substr(0, colon);
    out.port = port;
    return true;
}

bool ServerLinkConfig::Load(const std::string& path, std::string& error)
{
    CIniFile ini;
    if (!ini.Load(path))
    {
        error = "파일을 못 읽음: " + path;
        return false;
    }

    const auto readAddress = [&](const char* section, Address& out)
    {
        const std::string text = ini.GetString(section, "InternalAddr", "");
        if (ParseAddress(text, out))
            return true;
        error = std::string("[") + section + "] InternalAddr 가 없거나 틀림: \"" + text + "\"";
        return false;
    };
    if (!readAddress("Account", account) || !readAddress("Character", character))
        return false;
    if (!ini.GetString("World", "InternalAddr", "").empty() && !readAddress("World", world))
        return false;

    const auto readPositive = [&](const char* key, int& value)
    {
        value = ini.GetInt("Link", key, value);
        if (value > 0)
            return true;
        error = std::string("[Link] ") + key + " 는 1 이상이어야 함: " + std::to_string(value);
        return false;
    };
    return readPositive("RetrySec", retrySec)
        && readPositive("HeartbeatSec", heartbeatSec)
        && readPositive("ReplyTimeoutSec", replyTimeoutSec)
        && readPositive("SessionReserve", sessionReserve);
}
