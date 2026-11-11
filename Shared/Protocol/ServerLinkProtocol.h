#pragma once
// ==========================================================================
// 서버 간 패킷 (번호대 3000~) — 존 서버 → 캐릭터 서버 → 계정 서버
//
//   웹이 쓰는 패킷 표와 섞지 않는다. 머리 규약은 같다(NetHeader.h 만 포함 · Protocol.h 안 포함).
//   서버끼리 묻고 답한다 — 서명 없음. 내부망 분리가 전제다(플랜 4절).
//   코어의 패킷 상한이 1458바이트다(IOCPServer.h) — 긴 명단은 패킷 여러 개로 나눈다.
// ==========================================================================

#include <cstdint>

#include "../../ServerCore/Network/NetHeader.h"   // MsgHeader

inline constexpr uint16_t LINK_PACKET_FIRST = 3000;
inline constexpr uint16_t LINK_PACKET_LAST  = 3099;

inline constexpr int LINK_CODE_BYTES    = 64;   // 로그인 토큰 · 입장 번호 칸 — 앞 16바이트가 난수(SecureRandom.h)
inline constexpr int LINK_WS_ADDR_BYTES = 64;   // 존 서버 웹 접속 주소(ws://…) — NUL 로 끝난다. 64 는 사설·공인 주소와 포트가 넉넉히 드는 크기로 고른 값이다

// 명단 패킷 하나에 드는 줄 수 — (1458 − 머리 4 − 칸 수 2) ÷ 8 = 181. 5,600명이면 패킷 31개다
inline constexpr int LINK_ONLINE_LIST_MAX = 181;

#pragma pack(push, 1)

// 하트비트 — 양쪽이 각각 보낸다. 코어 세션 시한(60초)은 데이터를 받아야 갱신되기 때문이다. 답이 없다
struct LINK_HEARTBEAT
{
    static constexpr uint16_t TYPE = 3000;
    MsgHeader header;
};

// ── 토큰 확인 (캐릭터 서버 → 계정 서버) ──
enum class TokenCheckResult : uint8_t
{
    Ok = 0,
    BadSize = 1,
    NotFound = 2,
    Expired = 3,
};
inline constexpr int TOKEN_CHECK_RESULT_COUNT = 4;

struct LINK_TOKEN_CHECK_REQ
{
    static constexpr uint16_t TYPE = 3001;
    MsgHeader header;
    uint64_t  requestId;
    char      token[LINK_CODE_BYTES];
};

struct LINK_TOKEN_CHECK_ACK
{
    static constexpr uint16_t TYPE = 3002;
    MsgHeader header;
    uint64_t  requestId;
    uint8_t   result;      // TokenCheckResult
    int64_t   accountId;   // Ok 일 때만 의미가 있다
};

// ── 존 서버 등록 (존 서버 → 캐릭터 서버) ──
//   접속하면 등록 한 장 → 명단 여러 장. 연결이 끊기면 그 존 서버는 없는 것으로 친다
struct LINK_ZONE_REGISTER
{
    static constexpr uint16_t TYPE = 3003;
    MsgHeader header;
    int32_t   zoneServerId;
    int32_t   maxClients;
    char      publicWsAddr[LINK_WS_ADDR_BYTES];
};

// 지금 접속 중인 캐릭터 명단 — 머리 + 칸 수 + 캐릭터 번호 N개 (가변)
struct LINK_ZONE_ONLINE_LIST
{
    static constexpr uint16_t TYPE = 3004;
    MsgHeader header;
    uint16_t  count;
    // int64_t characterId[count];
};

// ── 입장 예고 (캐릭터 서버 → 존 서버) ──
enum class EnterNoticeResult : uint8_t
{
    Ok = 0,
    Full = 1,    // 접속자 + 대기 명단이 정원에 닿았다 → 웹에는 ZONE_FULL
};

struct LINK_ENTER_NOTICE_REQ
{
    static constexpr uint16_t TYPE = 3005;
    MsgHeader header;
    uint64_t  requestId;
    int64_t   characterId;
    int64_t   accountId;
    char      enterCode[LINK_CODE_BYTES];
};

struct LINK_ENTER_NOTICE_ACK
{
    static constexpr uint16_t TYPE = 3006;
    MsgHeader header;
    uint64_t  requestId;
    uint8_t   result;     // EnterNoticeResult
    int32_t   ttlSec;     // 입장 번호 유효 초 — 정본은 존 서버 설정(값이 두 곳에 생기지 않게 여기로 받는다)
};

// ── 접속 통지 (존 서버 → 캐릭터 서버) ──
//   캐릭터 서버가 게임 중인 캐릭터의 삭제를 막는 근거다(IN_GAME)
struct LINK_ZONE_CHAR_ENTER
{
    static constexpr uint16_t TYPE = 3007;
    MsgHeader header;
    int64_t   characterId;
};

struct LINK_ZONE_CHAR_LEAVE
{
    static constexpr uint16_t TYPE = 3008;
    MsgHeader header;
    int64_t   characterId;
};

// ── 월드 서버 등록 (월드 서버 → 계정 서버) ──
//   계정 서버의 서버 간 포트에는 캐릭터 서버도 붙는다. 이 패킷을 보낸 연결이 월드 서버다
struct LINK_WORLD_REGISTER
{
    static constexpr uint16_t TYPE = 3031;
    MsgHeader header;
    int32_t   worldId;
};

#pragma pack(pop)

static_assert(sizeof(LINK_HEARTBEAT) == 4);
static_assert(sizeof(LINK_TOKEN_CHECK_REQ) == 4 + 8 + 64);
static_assert(sizeof(LINK_TOKEN_CHECK_ACK) == 4 + 8 + 1 + 8);
static_assert(sizeof(LINK_ZONE_REGISTER) == 4 + 4 + 4 + 64);
static_assert(sizeof(LINK_ZONE_ONLINE_LIST) == 4 + 2);
static_assert(sizeof(LINK_ENTER_NOTICE_REQ) == 4 + 8 + 8 + 8 + 64);
static_assert(sizeof(LINK_ENTER_NOTICE_ACK) == 4 + 8 + 1 + 4);
static_assert(sizeof(LINK_WORLD_REGISTER) == 4 + 4);
static_assert(sizeof(LINK_ZONE_ONLINE_LIST) + LINK_ONLINE_LIST_MAX * sizeof(int64_t) <= 1458, "코어 패킷 상한(IOCPServer.h)");
