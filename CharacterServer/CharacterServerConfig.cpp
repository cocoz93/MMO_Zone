#include "CharacterServerConfig.h"

#include <Common/ErrorLog.h>
#include <Config/IniFile.h>
#include <Protocol/CharacterProtocol.h>

bool CharacterServerConfig::Load(const std::string& path, std::string& error)
{
    CIniFile ini;
    if (!ini.Load(path))
    {
        error = "파일을 못 읽음: " + path;
        return false;
    }
    if (!lobby.Read(ini, error))
        return false;

    characterSlots = ini.GetInt("Character", "CharacterSlots", characterSlots);
    if (characterSlots < 1 || characterSlots > CHAR_LIST_MAX)
    {
        error = "[Character] CharacterSlots 는 1~" + std::to_string(CHAR_LIST_MAX) + " 이어야 함: " + std::to_string(characterSlots);
        return false;
    }
    deleteGraceDays = ini.GetInt("Character", "DeleteGraceDays", deleteGraceDays);
    if (deleteGraceDays < 1)
    {
        error = "[Character] DeleteGraceDays 는 1 이상이어야 함: " + std::to_string(deleteGraceDays);
        return false;
    }
    return true;
}

void CharacterServerConfig::Print() const
{
    lobby.Print();
    SLOG_INFO("  Character: slots {} · delete grace {} days", characterSlots, deleteGraceDays);
}
