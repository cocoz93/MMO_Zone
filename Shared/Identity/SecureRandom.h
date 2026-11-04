#pragma once
// ==========================================================================
// SecureRandom — 암호 수준 난수와 64바이트 칸의 일회용 번호(로그인 토큰 · 입장 번호)
//
//   칸은 64바이트이고 앞 16바이트가 난수 · 나머지는 0 이다. 16바이트는 무작위로 맞힐 수 없는 크기로 고른 값이다.
//   윈도는 BCrypt, 리눅스는 getrandom. 코어·MySQL·HTTP 에 의존하지 않는다.
// ==========================================================================

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include <WinSock2.h>   // winsock2를 windows.h보다 먼저 (MonitorServer.h 와 같은 차례)
#include <Windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <sys/random.h>
#endif

namespace SecureRandom
{
    inline constexpr int CODE_WIRE_BYTES = 64;     // 패킷 칸 크기 — C2S_ENTER_GAME.ticket(ENTER_TICKET_MAX_LEN)과 같다
    inline constexpr int CODE_RANDOM_BYTES = 16;   // 그중 난수 자리

    // out 을 난수로 채운다. 실패하면 false — 그 내용을 쓰면 안 된다
    inline bool Fill(void* out, size_t size)
    {
#ifdef _WIN32
        return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#else
        size_t done = 0;
        char* p = static_cast<char*>(out);
        while (done < size)
        {
            const ssize_t got = getrandom(p + done, size - done, 0);
            if (got < 0)
                return false;   // EINTR 포함 — 칸 크기가 256바이트 이하면 끼어들지 않는다(getrandom 규약)
            done += static_cast<size_t>(got);
        }
        return true;
#endif
    }

    // 64바이트 칸에 새 번호를 쓴다
    inline bool MakeCode(char (&out)[CODE_WIRE_BYTES])
    {
        std::memset(out, 0, CODE_WIRE_BYTES);
        return Fill(out, CODE_RANDOM_BYTES);
    }

    enum class CodeShape
    {
        Empty,    // 전부 0 — 번호를 안 냈다
        Valid,    // 앞 16바이트에 값이 있고 나머지는 0
        BadSize,  // 16바이트 뒤에 값이 있거나 앞이 비었다
    };

    // 칸의 모양만 본다 — 장부를 찾기 전에 거른다
    inline CodeShape ClassifyCode(const char (&code)[CODE_WIRE_BYTES])
    {
        bool headHasValue = false;
        for (int i = 0; i < CODE_RANDOM_BYTES; ++i)
            headHasValue = headHasValue || code[i] != 0;

        bool tailHasValue = false;
        for (int i = CODE_RANDOM_BYTES; i < CODE_WIRE_BYTES; ++i)
            tailHasValue = tailHasValue || code[i] != 0;

        if (!headHasValue && !tailHasValue)
            return CodeShape::Empty;
        if (!headHasValue || tailHasValue)
            return CodeShape::BadSize;
        return CodeShape::Valid;
    }
}
