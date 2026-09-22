#pragma once

// ==========================================================================
// ClientConfig — INI 파일 기반 클라이언트 설정 로더
//
// [사용법]
//  ClientConfig config;
//  config.Load();  // 실행 파일 옆의 ClientConfig.ini 로드
//
// [INI 포맷]
//  [Server]
//  IP=127.0.0.1
//  Port=6000
//  [Account]
//  Id=10000000000
// ==========================================================================

#include <string>
#include <iostream>
#include <cstdint>
#include <cstdlib>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

struct ClientConfig
{
    std::string serverIp  = "127.0.0.1";
    int         serverPort = 6000;

    // [Account] 입장 요청에 실어 보낼 계정 번호. 0 = "번호를 지정하지 않음"(서버가 접속마다 새로 붙인다).
    //   지정 번호는 2^31 이상에서 고른다 — 서버의 자동 번호는 int32 카운터라 그 위로 못 올라오기 때문에
    //   구간이 타입으로 갈린다("100만 이상" 같은 약속은 부하 더미가 몇 시간이면 넘어선다).
    int64_t     accountId = 0;

    // 실행 파일 경로 기준으로 ClientConfig.ini 로드
    bool Load()
    {
        // 실행 파일 디렉토리에서 INI 경로 구성
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);

        std::wstring iniPath(exePath);
        size_t pos = iniPath.find_last_of(L"\\/");
        iniPath = iniPath.substr(0, pos + 1) + L"ClientConfig.ini";

        // 파일 존재 확인
        DWORD attr = GetFileAttributesW(iniPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES)
        {
            std::cout << "[ClientConfig] ClientConfig.ini not found. Using defaults." << std::endl;
            return false;
        }

        const wchar_t* path = iniPath.c_str();

        // [Server] 섹션
        wchar_t buf[256];
        GetPrivateProfileStringW(L"Server", L"IP", L"127.0.0.1", buf, 256, path);
        serverIp = WtoA(buf);

        serverPort = GetPrivateProfileIntW(L"Server", L"Port", 6000, path);

        // [Account] 섹션 — GetPrivateProfileIntW 는 32비트라 100억을 못 담는다. 문자열로 읽어 64비트로 바꾼다
        GetPrivateProfileStringW(L"Account", L"Id", L"0", buf, 256, path);
        accountId = _wtoi64(buf);

        PrintConfig();
        return true;
    }

private:
    static std::string WtoA(const wchar_t* wstr)
    {
        int len = WideCharToMultiByte(CP_ACP, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
        std::string result(len - 1, '\0');
        WideCharToMultiByte(CP_ACP, 0, wstr, -1, &result[0], len, nullptr, nullptr);
        return result;
    }

    void PrintConfig() const
    {
        std::cout << "[ClientConfig] Loaded from INI" << std::endl;
        std::cout << "  ServerIP   : " << serverIp << std::endl;
        std::cout << "  ServerPort : " << serverPort << std::endl;
        std::cout << "  AccountId  : " << accountId << (accountId == 0 ? " (미지정 - 매번 새 캐릭터)" : "") << std::endl;
    }
};
