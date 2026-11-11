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

inline constexpr int LINK_NAME_CHARS = 6;        // 캐릭터 이름 칸 — CHAR_NAME_MAX_LEN 과 같아야 한다(존 서버가 static_assert 로 맞춘다)

// 채널 줄 상한 — 존 서버 한 대의 맵 하나는 ChannelNumber::BLOCK_MAX 까지 · 전 존 서버 합은 웹 패킷 상한(CHANNEL_LIST_MAX)까지
inline constexpr int LINK_ZONE_CHANNELS_MAX = 200;
inline constexpr int LINK_CHANNEL_LIST_MAX = 240;

// 명단 패킷 하나에 드는 줄 수 — (1458 − 머리 4 − 칸 수 2) ÷ 38 = 38. 5,600명이면 패킷 148개다
inline constexpr int LINK_ONLINE_LIST_MAX = 38;

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

// 접속 장부 한 줄 — 3004 · 3007 이 같은 꼴을 쓴다. 캐릭터 서버는 번호 둘만 쓰고(중복 판정 · 삭제 판정),
//   월드 서버는 전부 쓴다(파티 창 · 이름 찾기). 존 서버는 같은 패킷을 양쪽에 보낸다
struct LinkOnlineEntry
{
    int64_t  characterId;
    int64_t  accountId;     // 시험 입장 길의 사람은 0(NO_ACCOUNT) — 중복 판정 밖이다
    int32_t  level;
    int32_t  job;
    uint8_t  displayChar;
    uint8_t  colorIndex;
    char16_t name[LINK_NAME_CHARS];   // 끝 표시가 없을 수 있다 — 6글자를 꽉 채운 이름
};

// 지금 접속 중인 캐릭터 명단 — 머리 + 칸 수 + 줄 N개 (가변)
struct LINK_ZONE_ONLINE_LIST
{
    static constexpr uint16_t TYPE = 3004;
    MsgHeader header;
    uint16_t  count;
    // LinkOnlineEntry entries[count];
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
    int32_t   targetChannelIndex;   // 다른 존 서버로 채널을 바꿀 때 목적 채널 · -1 = 자동 배정(일반 입장)
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
    LinkOnlineEntry entry;
};

struct LINK_ZONE_CHAR_LEAVE
{
    static constexpr uint16_t TYPE = 3008;
    MsgHeader header;
    int64_t   characterId;
};

// ── 계정 우편 (존 서버 → 월드 서버 → 계정 서버 · mail-plan.md 6절 · 9절 「서버 간」) ──
//   월드 서버는 요청 번호만 바꿔 넘긴다. 계정 서버는 첨부 바이트의 뜻을 모른다 — 존 서버가 만든 것(MailAttach)을 그대로 담아 둔다.
//   글자 칸은 char16_t 이고 끝나면 0(TextUtf.h). 칸 크기는 존 서버의 MAIL_TITLE_MAX · MAIL_BODY_MAX 와 같아야 한다(존 서버가 static_assert 로 맞춘다).
//   3020~3038 은 월드 서버 플랜이 쓴다 — 11-10 에 더한 셋(풀기 · 지우기)은 그 뒤 3039 부터다
inline constexpr int LINK_MAIL_TITLE_CHARS  = 20;
inline constexpr int LINK_MAIL_BODY_CHARS   = 200;
inline constexpr int LINK_MAIL_ATTACH_BYTES = 66;   // 판 번호 1 + 칸 수 1 + (item_id, count) 8바이트 × 8칸 — MailAttach.h 의 최대
inline constexpr int LINK_ACCMAIL_LIST_ROWS = 10;   // 목록 한 장의 줄 수 — 더 있으면 존 서버가 beforeMailId 로 다음 장을 묻는다(9절 「여러 장 답을 안 쓰고 장마다 요청한다」)

enum class LinkMailResult : uint8_t
{
    OK = 0,
    GONE = 1,            // 없음 · 받음 · 지움 · 기한 지남(「받는 중」은 기한과 무관하게 있다)
    BUSY = 2,            // 계정 서버 DB 큐가 찼다
    DB_ERROR = 3,
    NO_ACCOUNT = 4,      // 한 계정 지급의 대상 계정이 없다
    CANNOT_DELETE = 5,   // 첨부 · 돈이 남았거나 받는 중이거나 이미 없다
};
inline constexpr int LINK_MAIL_RESULT_COUNT = 6;

// account_mail.state 와 같은 값이다
enum class AccMailState : uint8_t
{
    NOT_TAKEN = 0,
    CLAIMING = 1,   // claimCharacterId 가 받는 중 — 그 캐릭터만 끝낸다(6절 「받는 중 + C 표시가 필요한 까닭」)
    TAKEN = 2,
    ERASED = 3,
};

// 붉은 수와 「이 캐릭터가 받는 중인 행」 하나(입장 때 다시 처리 · 6절)
struct LINK_ACCMAIL_COUNT_REQ
{
    static constexpr uint16_t TYPE = 3009;
    MsgHeader header;
    uint64_t  requestId;
    int64_t   accountId;
    int64_t   characterId;   // 「받는 중」 행은 이 캐릭터 것만 돌려준다
};

struct LINK_ACCMAIL_COUNT_ACK
{
    static constexpr uint16_t TYPE = 3010;
    MsgHeader header;
    uint64_t  requestId;
    uint8_t   result;        // LinkMailResult
    uint16_t  count;         // 안 받았고 (안 읽었거나 첨부 · 돈이 남은) 기한 안의 우편 수 — 65535 에서 멈춘다
    int64_t   claimMailId;   // 이 캐릭터가 받는 중인 가장 작은 번호 · 0 = 없음
};

struct LINK_ACCMAIL_LIST_REQ
{
    static constexpr uint16_t TYPE = 3011;
    MsgHeader header;
    uint64_t  requestId;
    int64_t   accountId;
    int64_t   beforeMailId;   // 0 = 처음부터 · 아니면 이 번호보다 작은 것부터(큰 번호 = 새 우편이 먼저)
};

// 목록 한 줄 — 본문 없음 · 보낸 사람은 늘 운영자(웹이 「운영자」로 그린다)
struct LinkAccMailLine
{
    int64_t  mailId;
    int64_t  money;
    char16_t title[LINK_MAIL_TITLE_CHARS];
    uint32_t remainSec;          // 기한까지 남은 초 · 지났으면 0(「받는 중」 줄은 기한이 지나도 보인다)
    uint8_t  state;              // AccMailState — 목록에는 NOT_TAKEN · CLAIMING 만 나온다
    uint8_t  read;               // 1 = 읽은 적 있음(read_at)
    int64_t  claimCharacterId;   // CLAIMING 일 때 받는 중인 캐릭터 · 아니면 0
    uint8_t  attachLen;          // 0 = 첨부 없음
    uint8_t  attach[LINK_MAIL_ATTACH_BYTES];
};

struct LINK_ACCMAIL_LIST_ACK
{
    static constexpr uint16_t TYPE = 3012;
    MsgHeader header;
    uint64_t  requestId;
    uint8_t   result;   // LinkMailResult
    uint8_t   more;     // 1 = 이 장 뒤에 더 있다
    uint8_t   count;    // lines 중 쓰는 줄 수(0 ~ LINK_ACCMAIL_LIST_ROWS)
    LinkAccMailLine lines[LINK_ACCMAIL_LIST_ROWS];
};

struct LINK_ACCMAIL_READ_REQ
{
    static constexpr uint16_t TYPE = 3013;
    MsgHeader header;
    uint64_t  requestId;
    int64_t   accountId;
    int64_t   mailId;
};

// 한 통 전부 — 읽기(3014)와 받는 중 표시(3016)의 답이 같은 꼴이다
struct LinkAccMailFull
{
    uint8_t  state;              // AccMailState
    int64_t  mailId;
    int64_t  claimCharacterId;   // CLAIMING 이면 받는 중인 캐릭터 — 받는 중 표시의 답에서는 「적혀 있는 캐릭터」(내가 아닐 수 있다)
    int64_t  money;
    uint32_t remainSec;
    char16_t title[LINK_MAIL_TITLE_CHARS];
    char16_t body[LINK_MAIL_BODY_CHARS];
    uint8_t  attachLen;
    uint8_t  attach[LINK_MAIL_ATTACH_BYTES];
};

// 읽기 — 계정 서버가 read_at 을 처음 한 번만 적고 본문을 돌려준다. 받기 판정도 이 답으로 한다(6절 받기 1번)
struct LINK_ACCMAIL_READ_ACK
{
    static constexpr uint16_t TYPE = 3014;
    MsgHeader header;
    uint64_t  requestId;
    uint8_t   result;   // LinkMailResult — OK 가 아니면 mail 은 0
    LinkAccMailFull mail;
};

// 받는 중 표시 — 「안 받음」이면 이 캐릭터로 표시한다. 이미 받는 중이면 표시를 안 바꾸고 적혀 있는 캐릭터와 내용을 돌려준다(6절 받기 4번)
struct LINK_ACCMAIL_CLAIM_REQ
{
    static constexpr uint16_t TYPE = 3015;
    MsgHeader header;
    uint64_t  requestId;
    int64_t   accountId;
    int64_t   mailId;
    int64_t   characterId;
};

struct LINK_ACCMAIL_CLAIM_ACK
{
    static constexpr uint16_t TYPE = 3016;
    MsgHeader header;
    uint64_t  requestId;
    uint8_t   result;   // LinkMailResult — OK = 지금 받는 중(mail.claimCharacterId 가 누구인지는 존 서버가 본다) · GONE = 그 밖
    LinkAccMailFull mail;
};

// 받음 — 존 서버의 저장 묶음이 확정된 뒤. 답이 없다(안 닿으면 입장 때 다시 처리가 다시 보낸다)
struct LINK_ACCMAIL_DONE
{
    static constexpr uint16_t TYPE = 3017;
    MsgHeader header;
    int64_t   accountId;
    int64_t   mailId;
    int64_t   characterId;   // 받는 중으로 적힌 캐릭터만 끝낸다
};

// 운영자 지급 — targetAccount 0 = 전 계정(batchId 로 두 번 안 들어간다) · 아니면 그 계정 하나(batchId 0 = 없음)
struct LINK_ACCMAIL_SEND_REQ
{
    static constexpr uint16_t TYPE = 3018;
    MsgHeader header;
    uint64_t  requestId;
    int64_t   targetAccount;
    int64_t   batchId;
    int64_t   sentBy;   // 보낸 운영자의 캐릭터 번호
    int64_t   money;
    char16_t  title[LINK_MAIL_TITLE_CHARS];
    char16_t  body[LINK_MAIL_BODY_CHARS];
    uint8_t   attachLen;
    uint8_t   attach[LINK_MAIL_ATTACH_BYTES];
};

struct LINK_ACCMAIL_SEND_ACK
{
    static constexpr uint16_t TYPE = 3019;
    MsgHeader header;
    uint64_t  requestId;
    uint8_t   result;   // LinkMailResult
    int64_t   rows;     // 넣은 행 수 — 같은 batchId 를 다시 보내면 0
};

// 받는 중 풀기 — 존 서버의 다시 판정이 거절했을 때(가방이 모자람 등). 답이 없다(안 닿으면 입장 때 다시 처리가 다시 판정한다)
struct LINK_ACCMAIL_UNCLAIM
{
    static constexpr uint16_t TYPE = 3039;
    MsgHeader header;
    int64_t   accountId;
    int64_t   mailId;
    int64_t   characterId;   // 받는 중으로 적힌 캐릭터만 푼다
};

// 지우기 — 첨부도 돈도 없고 안 받은 우편만(6절 「지우기 — 계정 우편」)
struct LINK_ACCMAIL_ERASE_REQ
{
    static constexpr uint16_t TYPE = 3040;
    MsgHeader header;
    uint64_t  requestId;
    int64_t   accountId;
    int64_t   mailId;
};

struct LINK_ACCMAIL_ERASE_ACK
{
    static constexpr uint16_t TYPE = 3041;
    MsgHeader header;
    uint64_t  requestId;
    uint8_t   result;   // LinkMailResult
};

// ── 월드 서버 등록 (월드 서버 → 계정 서버) ──
//   계정 서버의 서버 간 포트에는 캐릭터 서버도 붙는다. 이 패킷을 보낸 연결이 월드 서버다
struct LINK_WORLD_REGISTER
{
    static constexpr uint16_t TYPE = 3031;
    MsgHeader header;
    int32_t   worldId;
};

// ── 채널 목록 ──
//   존 서버가 1초마다 바뀐 맵을 통째로 알린다(맵 하나씩). 월드 서버는 그 맵 · 그 존 서버 줄을 갈아 끼운다
struct LinkZoneChannelRow
{
    int32_t  channel;
    uint16_t players;
};

struct LINK_ZONE_CHANNELS
{
    static constexpr uint16_t TYPE = 3034;
    MsgHeader header;
    int32_t   mapId;
    uint16_t  count;
    // LinkZoneChannelRow rows[count];
};

struct LINK_CHANNEL_LIST_REQ
{
    static constexpr uint16_t TYPE = 3035;
    MsgHeader header;
    uint64_t  requestId;
    int32_t   mapId;
};

struct LinkChannelListRow
{
    uint16_t zoneServerId;
    uint16_t channel;
    uint16_t players;
};

struct LINK_CHANNEL_LIST_ACK
{
    static constexpr uint16_t TYPE = 3036;
    MsgHeader header;
    uint64_t  requestId;
    uint16_t  count;
    // LinkChannelListRow rows[count];
};

// ── 다른 존 서버로 채널 바꾸기 (존 서버 ↔ 캐릭터 서버) ──
//   존 서버는 저장이 확정된 뒤에 묻는다. 캐릭터 서버가 목적지에 입장 예고를 넣고 입장 번호를 돌려준다
enum class LinkHandoffResult : uint8_t
{
    Ok = 0,
    Full = 1,      // 목적지 존 서버의 정원이 찼다
    Down = 2,      // 목적지가 답하지 않았다(끊김 · 시한)
    Unknown = 3,   // 그 번호로 등록된 존 서버가 없다
};

struct LINK_HANDOFF_REQ
{
    static constexpr uint16_t TYPE = 3037;
    MsgHeader header;
    uint64_t  requestId;
    int64_t   characterId;
    int64_t   accountId;
    int32_t   destZone;
    int32_t   destChannel;
};

struct LINK_HANDOFF_ACK
{
    static constexpr uint16_t TYPE = 3038;
    MsgHeader header;
    uint64_t  requestId;
    uint8_t   result;     // LinkHandoffResult
    char      enterCode[LINK_CODE_BYTES];
    char      wsAddr[LINK_WS_ADDR_BYTES];
    int32_t   ttlSec;
};

// ── 내보내기 (캐릭터 서버 → 존 서버) ──
//   같은 계정이 다시 고르면 줄이 있는 존 서버마다 보낸다. 회신이 없다
struct LINK_ZONE_KICK_ACCOUNT
{
    static constexpr uint16_t TYPE = 3042;
    MsgHeader header;
    int64_t   accountId;
};

// ── 파티 (존 서버 ↔ 월드 서버) ──
//   규칙은 월드 서버 CPartySystem 이 맡는다. 존 서버는 요청을 넘기고, 받은 알림을 웹 패킷으로 옮기고, 사본을 든다.
//   사람은 캐릭터 번호로 가리킨다 — 액터 번호는 존 서버마다 따로 세서 겹친다. 번호가 PartyNotice · PartyAskKind 인 칸은 Protocol.h 의 값 그대로다

// 파티 요청의 종류 — 초대 · 신청 · 추천은 두 사람의 처지로 월드 서버가 가르므로 Request 하나다
enum class LinkPartyOpKind : uint8_t
{
    Request = 0,      // 화면에서 누른 사람과 파티
    InviteByName = 1, // /party 이름 — 월드 서버가 장부에서 이름을 찾는다
    Answer = 2,       // 받은 요청에 답
    Leave = 3,
    Kick = 4,
    SetLeader = 5,
    Disband = 6,
    TakeLeader = 7,   // 파티장이 오프라인일 때 보스방 포털을 누른 파티원이 파티장을 맡는다
};

// 사람 하나 — 26바이트. 액터 번호는 싣지 않는다(존 서버가 웹 패킷으로 옮길 때 자기 세션을 찾아 채운다)
struct LinkPartyPerson
{
    int64_t  characterId;   // 0 = 없음
    uint8_t  displayChar;
    uint8_t  colorIndex;
    int32_t  level;
    char16_t name[LINK_NAME_CHARS];
};

// 파티원 상태 — 존 서버가 0.5초마다 지난 값과 견줘 바뀐 사람만 보낸다
struct LinkPartyStateRow
{
    int64_t  characterId;
    int32_t  hp;
    int32_t  maxHp;
    int32_t  level;
    int32_t  mapId;
    uint16_t visibleChannel;   // 방 맵은 0
};

// 파티원 한 줄 — 명단(3027)과 바뀐 줄(3029)이 같이 쓴다
struct LinkPartyMemberRow
{
    LinkPartyPerson who;
    int32_t  hp;
    int32_t  maxHp;
    int32_t  mapId;
    uint16_t visibleChannel;   // 방 맵과 오프라인은 0
    uint8_t  online;
};

inline constexpr int LINK_PARTY_MEMBERS_MAX = 8;   // PARTY_MAX_MEMBERS 와 같아야 한다(WorldServer/PartySystem.cpp 가 static_assert 로 맞춘다)

// 상태 패킷 하나에 드는 줄 수 — (1458 − 머리 4 − 칸 수 1) ÷ 26 = 55
inline constexpr int LINK_PARTY_STATE_MAX = 55;
// 복원 패킷 하나에 드는 줄 수 — (1458 − 머리 4 − 칸 수 2) ÷ 32 = 45
inline constexpr int LINK_PARTY_RESTORE_MAX = 45;

// 존 → 월드: 플레이어의 파티 요청. 레벨 둘은 월드 서버 장부의 값이 입장 때 그대로라 요청 창에 옛 레벨이 나가지 않게 실어 보낸다
struct LINK_PARTY_OP
{
    static constexpr uint16_t TYPE = 3023;
    MsgHeader header;
    uint8_t   op;                  // LinkPartyOpKind
    int64_t   characterId;         // 보낸 사람
    int32_t   level;               // 보낸 사람의 지금 레벨
    int64_t   targetCharacterId;   // Request · Kick · SetLeader
    int32_t   targetLevel;         // Request 만
    char16_t  targetName[LINK_NAME_CHARS];   // InviteByName 만
    uint32_t  requestId;           // Answer 만
    uint8_t   accept;              // Answer 만
};

// 월드 → 존: 답할 요청이 왔다
struct LINK_PARTY_ASK
{
    static constexpr uint16_t TYPE = 3024;
    MsgHeader       header;
    int64_t         to;
    uint32_t        requestId;
    uint8_t         kind;       // PartyAskKind
    LinkPartyPerson from;
    LinkPartyPerson about;      // 없으면 characterId 0
    uint32_t        remainMs;
};

// 월드 → 존: 그 요청이 무효가 됐다
struct LINK_PARTY_ASK_END
{
    static constexpr uint16_t TYPE = 3025;
    MsgHeader header;
    int64_t   to;
    uint32_t  requestId;
    uint8_t   why;   // PartyNotice — EXPIRED · CANCELLED
};

// 월드 → 존: 알림 한 줄
struct LINK_PARTY_NOTICE
{
    static constexpr uint16_t TYPE = 3026;
    MsgHeader       header;
    int64_t         to;
    uint8_t         code;   // PartyNotice
    LinkPartyPerson who;    // 없으면 characterId 0
};

// 월드 → 존: 파티 전체 명단(사본 갱신 겸 화면) — 머리 + 줄 N개(가변).
//   파티에 접속 중인 사람이 있는 존 서버마다 한 장씩 간다. to != 0 이면 파티가 없어진 한 사람에게(partyId 0 · 줄 없음)
struct LINK_PARTY_INFO
{
    static constexpr uint16_t TYPE = 3027;
    MsgHeader header;
    int64_t   to;
    int32_t   partyId;
    int64_t   leader;
    uint8_t   count;
};

// 존 → 월드: 자기 쪽 파티원의 바뀐 줄 — 머리 + 줄 N개(가변)
struct LINK_PARTY_STATE
{
    static constexpr uint16_t TYPE = 3028;
    MsgHeader header;
    uint8_t   count;
};

// 월드 → 존: 파티원 한 명의 바뀐 줄 — 그 파티에 접속 중인 사람이 있는 존 서버마다
struct LINK_PARTY_MEMBER
{
    static constexpr uint16_t TYPE = 3029;
    MsgHeader          header;
    int32_t            partyId;
    LinkPartyMemberRow row;
};

// 복원 한 줄 — 존 서버는 자기 사람만 증언하므로 두 곳이 같은 사람을 다르게 말하는 일이 없다
struct LinkPartyRestoreRow
{
    int32_t           partyId;
    uint8_t           isLeader;
    uint8_t           joinOrder;
    LinkPartyStateRow state;
};

// 존 → 월드: 다시 붙을 때 자기 쪽 사람의 소속 — 머리 + 줄 N개(가변)
struct LINK_PARTY_RESTORE
{
    static constexpr uint16_t TYPE = 3030;
    MsgHeader header;
    uint16_t  count;
};

#pragma pack(pop)

static_assert(sizeof(LINK_HEARTBEAT) == 4);
static_assert(sizeof(LINK_TOKEN_CHECK_REQ) == 4 + 8 + 64);
static_assert(sizeof(LINK_TOKEN_CHECK_ACK) == 4 + 8 + 1 + 8);
static_assert(sizeof(LINK_ZONE_REGISTER) == 4 + 4 + 4 + 64);
static_assert(sizeof(LINK_ZONE_ONLINE_LIST) == 4 + 2);
static_assert(sizeof(LINK_ENTER_NOTICE_REQ) == 4 + 8 + 8 + 8 + 64 + 4);
static_assert(sizeof(LINK_ENTER_NOTICE_ACK) == 4 + 8 + 1 + 4);
// 계정 우편 — 크기는 mail-plan.md 9절 「서버 간」 표와 같다
static_assert(sizeof(LINK_ACCMAIL_COUNT_REQ) == 28);
static_assert(sizeof(LINK_ACCMAIL_COUNT_ACK) == 23);
static_assert(sizeof(LINK_ACCMAIL_LIST_REQ) == 28);
static_assert(sizeof(LinkAccMailLine) == 137);
static_assert(sizeof(LINK_ACCMAIL_LIST_ACK) == 1385);
static_assert(sizeof(LINK_ACCMAIL_READ_REQ) == 28);
static_assert(sizeof(LinkAccMailFull) == 1 + 8 + 8 + 8 + 4 + 40 + 400 + 1 + 66);
static_assert(sizeof(LINK_ACCMAIL_READ_ACK) == 549);
static_assert(sizeof(LINK_ACCMAIL_CLAIM_REQ) == 36);
static_assert(sizeof(LINK_ACCMAIL_CLAIM_ACK) == 549);
static_assert(sizeof(LINK_ACCMAIL_DONE) == 28);
static_assert(sizeof(LINK_ACCMAIL_SEND_REQ) == 551);
static_assert(sizeof(LINK_ACCMAIL_SEND_ACK) == 21);
static_assert(sizeof(LINK_ACCMAIL_UNCLAIM) == 28);
static_assert(sizeof(LINK_ACCMAIL_ERASE_REQ) == 28);
static_assert(sizeof(LINK_ACCMAIL_ERASE_ACK) == 13);
static_assert(sizeof(LINK_ACCMAIL_LIST_ACK) <= 1458, "코어 패킷 상한(IOCPServer.h)");
static_assert(sizeof(LINK_WORLD_REGISTER) == 4 + 4);
static_assert(sizeof(LINK_ZONE_KICK_ACCOUNT) == 4 + 8);
static_assert(sizeof(LinkPartyPerson) == 8 + 1 + 1 + 4 + 12);
static_assert(sizeof(LinkPartyStateRow) == 8 + 4 + 4 + 4 + 4 + 2);
static_assert(sizeof(LinkPartyMemberRow) == 26 + 4 + 4 + 4 + 2 + 1);
static_assert(sizeof(LinkPartyRestoreRow) == 4 + 1 + 1 + 26);
static_assert(sizeof(LINK_PARTY_OP) == 4 + 1 + 8 + 4 + 8 + 4 + 12 + 4 + 1);
static_assert(sizeof(LINK_PARTY_ASK) == 4 + 8 + 4 + 1 + 26 + 26 + 4);
static_assert(sizeof(LINK_PARTY_ASK_END) == 4 + 8 + 4 + 1);
static_assert(sizeof(LINK_PARTY_NOTICE) == 4 + 8 + 1 + 26);
static_assert(sizeof(LINK_PARTY_INFO) == 4 + 8 + 4 + 8 + 1);
static_assert(sizeof(LINK_PARTY_STATE) == 4 + 1);
static_assert(sizeof(LINK_PARTY_MEMBER) == 4 + 4 + 41);
static_assert(sizeof(LINK_PARTY_RESTORE) == 4 + 2);
static_assert(sizeof(LINK_PARTY_INFO) + LINK_PARTY_MEMBERS_MAX * sizeof(LinkPartyMemberRow) <= 1458, "코어 패킷 상한을 넘는다");
static_assert(sizeof(LINK_PARTY_STATE) + LINK_PARTY_STATE_MAX * sizeof(LinkPartyStateRow) <= 1458, "코어 패킷 상한을 넘는다");
static_assert(sizeof(LINK_PARTY_RESTORE) + LINK_PARTY_RESTORE_MAX * sizeof(LinkPartyRestoreRow) <= 1458, "코어 패킷 상한을 넘는다");
static_assert(sizeof(LinkOnlineEntry) == 8 + 8 + 4 + 4 + 1 + 1 + 12);
static_assert(sizeof(LINK_ZONE_CHAR_ENTER) == 4 + sizeof(LinkOnlineEntry));
static_assert(sizeof(LINK_ZONE_ONLINE_LIST) + LINK_ONLINE_LIST_MAX * sizeof(LinkOnlineEntry) <= 1458, "코어 패킷 상한(IOCPServer.h)");
static_assert(sizeof(LinkZoneChannelRow) == 6);
static_assert(sizeof(LINK_ZONE_CHANNELS) == 4 + 4 + 2);
static_assert(sizeof(LINK_CHANNEL_LIST_REQ) == 4 + 8 + 4);
static_assert(sizeof(LinkChannelListRow) == 6);
static_assert(sizeof(LINK_CHANNEL_LIST_ACK) == 4 + 8 + 2);
static_assert(sizeof(LINK_HANDOFF_REQ) == 4 + 8 + 8 + 8 + 4 + 4);
static_assert(sizeof(LINK_HANDOFF_ACK) == 4 + 8 + 1 + 64 + 64 + 4);
static_assert(sizeof(LINK_ZONE_CHANNELS) + LINK_ZONE_CHANNELS_MAX * sizeof(LinkZoneChannelRow) <= 1458, "코어 패킷 상한");
static_assert(sizeof(LINK_CHANNEL_LIST_ACK) + LINK_CHANNEL_LIST_MAX * sizeof(LinkChannelListRow) <= 1458, "코어 패킷 상한");
