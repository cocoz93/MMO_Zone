#pragma once
// ==========================================================================
// 캐릭터 서버 패킷 (번호대 2100~) — 웹 ↔ 캐릭터 서버
//
//   존 패킷 표(Protocol.h)에 넣지 않는다 — 콘솔 · 부하 더미가 다시 빌드될 일이 없다. 머리 규약은 같다.
//   계정 번호를 칸으로 받지 않는다. 받지 않으면 믿을 일도 없다 — 계정 번호는 토큰 확인의 답에서만 얻는다.
//   목록을 달라는 패킷은 없다 — 서버가 로그인 성공 뒤와 만들기 · 삭제 · 복구 성공 뒤에 보낸다.
// ==========================================================================

#include <cstdint>

#include "../../ServerCore/Network/NetHeader.h"   // MsgHeader
#include "../Identity/CharacterLookBlob.h"
#include "../Identity/CharacterName.h"

inline constexpr uint16_t CHAR_PACKET_FIRST = 2100;
inline constexpr uint16_t CHAR_PACKET_LAST  = 2199;

inline constexpr int CHAR_CODE_BYTES    = 64;   // 로그인 토큰 · 입장 번호 칸 — 앞 16바이트가 난수(SecureRandom)
inline constexpr int CHAR_WS_ADDR_BYTES = 64;   // 존 서버 웹 접속 주소 — LINK_WS_ADDR_BYTES 와 같아야 한다
inline constexpr int CHAR_LOOK_BYTES    = CharacterLookBlob::COLUMN_BYTES;

// 목록 패킷 하나에 드는 줄 수의 상한. 설정 CharacterSlots 가 이 값을 넘으면 기동을 거절한다.
//   16 은 근거 없이 고른 값이다 — 줄 65바이트 × 16 + 머리 6 = 1,046 이라 코어 패킷 상한(1458)에 넉넉하다
inline constexpr int CHAR_LIST_MAX = 16;

// ── 결과값 ── 값이 곧 와이어 값이다 — 뒤에만 더할 것. COUNT 는 지표가 갈래를 도는 데 쓴다
enum class CharLoginResult : uint8_t
{
    OK = 0,
    BAD_TOKEN = 1,    // 없는 토큰 · 크기 틀림 · 이미 쓴 토큰
    EXPIRED = 2,      // 2분이 지남 — 웹이 새 토큰을 받는다
    SERVER_DOWN = 3,  // 계정 서버와 끊김 · 응답 시한 초과
    BUSY = 4,         // DB 큐가 참
    DB_FAIL = 5,
    COUNT
};

enum class CharCreateResult : uint8_t
{
    OK = 0,
    TAKEN = 1,        // 이름이 이미 있음(유예가 끝난 행의 이름도 계속 막힌다)
    BAD_FORM = 2,     // 이름 글자 · 자리 번호가 틀림
    BAD_JOB = 3,
    BAD_LOOK = 4,
    SLOT_TAKEN = 5,
    BUSY = 6,         // 처리 중인 요청이 있음 · DB 큐가 참
    DB_FAIL = 7,
    COUNT
};

enum class CharDeleteResult : uint8_t
{
    OK = 0,
    NOT_MINE = 1,
    IN_GAME = 2,      // 접속 중이거나 입장 번호가 살아 있음
    BUSY = 3,
    DB_FAIL = 4,
    COUNT
};

enum class CharRestoreResult : uint8_t
{
    OK = 0,
    NOT_MINE = 1,
    EXPIRED = 2,      // 유예가 지남
    BUSY = 3,
    DB_FAIL = 4,
    COUNT
};

enum class CharSelectResult : uint8_t
{
    OK = 0,
    NOT_MINE = 1,
    DELETED = 2,      // 삭제 대기
    ZONE_DOWN = 3,    // 존 서버가 없음 · 응답 시한 초과
    ZONE_FULL = 4,
    BUSY = 5,
    DB_FAIL = 6,
    DUPLICATE_LOGIN = 7,    // 윈도 헤더에 DUPLICATE 매크로가 있어 이름을 늘렸다. 같은 계정이 게임 중이거나 입장 대기 중 — 앞 접속을 내보냈다. 다시 고르면 된다
    COUNT
};

#pragma pack(push, 1)

struct C2S_CHAR_LOGIN
{
    static constexpr uint16_t TYPE = 2100;
    MsgHeader header;
    char      authToken[CHAR_CODE_BYTES];
};

struct S2C_CHAR_LOGIN
{
    static constexpr uint16_t TYPE = 2101;
    MsgHeader header;
    uint8_t   result;   // CharLoginResult
};

// 목록 한 줄
struct CharListEntry
{
    int64_t  characterId;
    int8_t   slot;
    char16_t name[CHAR_NAME_MAX_LEN];
    int32_t  job;
    int32_t  level;
    uint8_t  look[CHAR_LOOK_BYTES];
    int32_t  deleteLeftSec;   // 0 = 삭제 대기 아님. 남은 시간은 서버가 준 값만 쓴다
};

// 목록 — 머리 + 칸 수 + 줄 N개 (가변)
struct S2C_CHAR_LIST
{
    static constexpr uint16_t TYPE = 2102;
    MsgHeader header;
    uint16_t  count;
    // CharListEntry entries[count];
};

struct C2S_CHAR_CREATE
{
    static constexpr uint16_t TYPE = 2103;
    MsgHeader header;
    int8_t    slot;
    char16_t  name[CHAR_NAME_MAX_LEN];
    int32_t   job;
    uint8_t   look[CHAR_LOOK_BYTES];
};

struct S2C_CHAR_CREATE
{
    static constexpr uint16_t TYPE = 2104;
    MsgHeader header;
    uint8_t   result;        // CharCreateResult
    int64_t   characterId;   // OK 일 때만 의미가 있다
};

struct C2S_CHAR_DELETE
{
    static constexpr uint16_t TYPE = 2105;
    MsgHeader header;
    int64_t   characterId;
};

struct S2C_CHAR_DELETE
{
    static constexpr uint16_t TYPE = 2106;
    MsgHeader header;
    uint8_t   result;   // CharDeleteResult
};

struct C2S_CHAR_RESTORE
{
    static constexpr uint16_t TYPE = 2107;
    MsgHeader header;
    int64_t   characterId;
};

struct S2C_CHAR_RESTORE
{
    static constexpr uint16_t TYPE = 2108;
    MsgHeader header;
    uint8_t   result;   // CharRestoreResult
};

struct C2S_CHAR_SELECT
{
    static constexpr uint16_t TYPE = 2109;
    MsgHeader header;
    int64_t   characterId;
};

struct S2C_CHAR_SELECT
{
    static constexpr uint16_t TYPE = 2110;
    MsgHeader header;
    uint8_t   result;                         // CharSelectResult
    char      enterCode[CHAR_CODE_BYTES];     // OK 일 때만
    char      zoneWsAddr[CHAR_WS_ADDR_BYTES]; // OK 일 때만 · NUL 로 끝남
};

// 창을 띄워 두고 고민하는 동안 코어 세션 시한(60초)에 안 끊기게. 로그인 뒤에만 보낸다 — 웹은 15초마다
struct C2S_CHAR_HEARTBEAT
{
    static constexpr uint16_t TYPE = 2111;
    MsgHeader header;
};

#pragma pack(pop)

static_assert(sizeof(CharListEntry) == 8 + 1 + 12 + 4 + 4 + 32 + 4);
static_assert(sizeof(C2S_CHAR_LOGIN) == 4 + 64);
static_assert(sizeof(S2C_CHAR_LOGIN) == 4 + 1);
static_assert(sizeof(S2C_CHAR_LIST) == 4 + 2);
static_assert(sizeof(C2S_CHAR_CREATE) == 4 + 1 + 12 + 4 + 32);
static_assert(sizeof(S2C_CHAR_CREATE) == 4 + 1 + 8);
static_assert(sizeof(C2S_CHAR_DELETE) == 4 + 8);
static_assert(sizeof(S2C_CHAR_DELETE) == 4 + 1);
static_assert(sizeof(C2S_CHAR_RESTORE) == 4 + 8);
static_assert(sizeof(S2C_CHAR_RESTORE) == 4 + 1);
static_assert(sizeof(C2S_CHAR_SELECT) == 4 + 8);
static_assert(sizeof(S2C_CHAR_SELECT) == 4 + 1 + 64 + 64);
static_assert(sizeof(C2S_CHAR_HEARTBEAT) == 4);
static_assert(sizeof(S2C_CHAR_LIST) + CHAR_LIST_MAX * sizeof(CharListEntry) <= 1458, "코어 패킷 상한(IOCPServer.h)");
