#pragma once
// 시험 공통 — 외부 라이브러리 없음, 실패하면 종료 값 1
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "Platform/Platform.h"

namespace TestHelp
{
    inline int g_failed = 0;
    inline int g_checks = 0;

    inline void Check(bool ok, const std::string& what)
    {
        ++g_checks;
        if (!ok)
            ++g_failed;
        std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
        std::fflush(stdout);
    }

    inline int Finish(const char* name)
    {
        std::printf("== %s %d/%d\n", name, g_checks - g_failed, g_checks);
        std::fflush(stdout);
        return g_failed == 0 ? 0 : 1;
    }

    // 조건이 참이 될 때까지 기다린다. 시한까지 안 되면 false
    template <typename Pred>
    bool WaitUntil(Pred pred, int timeoutMs = 5000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!pred())
        {
            if (std::chrono::steady_clock::now() >= until)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

    inline SOCKET ConnectRaw(const char* ip, int port)
    {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            return INVALID_SOCKET;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, ip, &addr.sin_addr);
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            Platform::CloseSocket(s);
            return INVALID_SOCKET;
        }
        return s;
    }

    // 메트릭 글자 — /metrics 를 소켓으로 직접 읽는다
    inline std::string HttpGet(const char* ip, int port, const char* path)
    {
        SOCKET s = ConnectRaw(ip, port);
        if (s == INVALID_SOCKET)
            return {};
        const std::string request = std::string("GET ") + path + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
        ::send(s, request.c_str(), static_cast<int>(request.size()), 0);
        std::string response;
        char buf[4096];
        while (true)
        {
            const int got = static_cast<int>(::recv(s, buf, sizeof(buf), 0));
            if (got <= 0)
                break;
            response.append(buf, static_cast<size_t>(got));
        }
        Platform::CloseSocket(s);
        return response;
    }
}
