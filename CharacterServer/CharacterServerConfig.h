#pragma once
// ==========================================================================
// CharacterServerConfig — Run/bin/CharacterServerConfig.ini
//
//   [Server] · [DB] 는 LobbyConfig, 여기는 캐릭터 서버만의 값이다. 서버 간 주소는 ServerLink.ini.
//   런타임 INI — CP949 무BOM CRLF(CLAUDE.md).
// ==========================================================================

#include <string>

#include <Lobby/LobbyConfig.h>

struct CharacterServerConfig
{
    LobbyConfig lobby;
    int characterSlots = 4;    // 계정당 캐릭터 칸 — 사용자 결정(11-04). 코드에 박지 않아 나중에 늘릴 수 있다
    int deleteGraceDays = 7;   // 삭제 유예 — 사용자 결정(11-04)

    bool Load(const std::string& path, std::string& error);
    void Print() const;
};
