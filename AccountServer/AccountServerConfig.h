#pragma once
// ==========================================================================
// AccountServerConfig — Run/bin/AccountServerConfig.ini
//
//   [Server] · [DB] 는 LobbyConfig, 여기는 계정 서버만의 값이다. 서버 간 주소는 ServerLink.ini.
//   런타임 INI — CP949 무BOM CRLF(CLAUDE.md).
// ==========================================================================

#include <string>

#include <Lobby/LobbyConfig.h>

struct AccountServerConfig
{
    LobbyConfig lobby;
    int  authTokenTtlSec = 120;    // 2분 — 사용자 결정(11-04). mmo_auth_token_total{result="expired"} 가 쌓이면 300 으로 올린다
    bool allowDevLogin = false;    // 이름만으로 로그인 — 코드 기본은 닫힘. 로그인 서버가 붙으면 끈다
    int  mailExpireDays = 30;      // [Mail] MailExpireDays — 계정 우편 기한(일) · 1 ~ 365. 존 서버 MMOServerConfig.ini 의 같은 키와 따로 둔다(mail-plan.md 3절)

    bool Load(const std::string& path, std::string& error);
    void Print() const;
};
