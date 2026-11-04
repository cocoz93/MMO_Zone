#pragma once
// ==========================================================================
// 계정 서버 패킷 (번호대 2000~) — 웹 ↔ 계정 서버
//
//   웹이 이름을 내면 토큰을 받고 연결이 끊긴다 — 접속 상태가 없다.
//   이름만으로 로그인하는 길은 시험용이다(AllowDevLogin). 로그인 서버가 붙으면 끈다.
//   머리 규약은 같다(NetHeader.h 만 포함 · Protocol.h 를 안 들인다).
// ==========================================================================

#include <cstdint>

#include "../../ServerCore/Network/NetHeader.h"   // MsgHeader

inline constexpr uint16_t ACCOUNT_PACKET_FIRST = 2000;
inline constexpr uint16_t ACCOUNT_PACKET_LAST  = 2099;

inline constexpr int ACCOUNT_LOGIN_NAME_MIN   = 4;    // 영문·숫자 4~16자(사용자 결정 11-04)
inline constexpr int ACCOUNT_LOGIN_NAME_MAX   = 16;
inline constexpr int ACCOUNT_LOGIN_NAME_BYTES = ACCOUNT_LOGIN_NAME_MAX + 1;   // NUL 로 끝난다
inline constexpr int ACCOUNT_TOKEN_BYTES      = 64;   // 앞 16바이트가 난수, 나머지는 0(SecureRandom)

// 값이 곧 와이어 값이다 — 뒤에만 더할 것
enum class AccountLoginResult : uint8_t
{
    OK = 0,
    BAD_FORM = 1,        // 이름 형식이 틀림
    LOGIN_DISABLED = 2,  // 시험 로그인 꺼짐
    BUSY = 3,            // DB 큐가 참
    DB_FAIL = 4,         // DB 실패 — 다시 내면 된다
    COUNT
};

#pragma pack(push, 1)

struct C2S_ACCOUNT_LOGIN
{
    static constexpr uint16_t TYPE = 2000;
    MsgHeader header;
    char      loginName[ACCOUNT_LOGIN_NAME_BYTES];
};

struct S2C_ACCOUNT_LOGIN
{
    static constexpr uint16_t TYPE = 2001;
    MsgHeader header;
    uint8_t   result;                       // AccountLoginResult
    char      token[ACCOUNT_TOKEN_BYTES];   // OK 일 때만 의미가 있다
};

#pragma pack(pop)

static_assert(sizeof(C2S_ACCOUNT_LOGIN) == 4 + 17);
static_assert(sizeof(S2C_ACCOUNT_LOGIN) == 4 + 1 + 64);
