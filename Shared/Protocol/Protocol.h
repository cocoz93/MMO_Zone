#pragma once

#include <cstdint>
// 와이어 헤더는 서버 코어가 갖는다 — "패킷 앞 2바이트가 길이다"는 전송 계층이 정하는 규약이라
//   게임이 바뀌어도 안 바뀐다. 이 파일이 include 하므로 클라·게임 사용처는 수정 불필요.
#include "../../ServerCore/Network/NetHeader.h"   // MsgHeader · EchoMsgHeader

// 패킷 타입 (혼용 방지를 위해 L7 Msg로 표기)
enum class MsgType : uint16_t
{
	// C2S: Client to Server
	// S2C: Server to Client

    //--------------------------------------------------
    // 에코 (NetWorkLib_EchoTest 모드 전용)
    //--------------------------------------------------
    ECHO = 0,

    //--------------------------------------------------
    // 이동
    //--------------------------------------------------
    C2S_MOVE_START = 1000,  // 클라이언트 → 서버: 이동 시작
    C2S_MOVE_STOP,          // 클라이언트 → 서버: 이동 정지

    S2C_MOVE_START,         // 서버 → 클라이언트: 다른 플레이어 이동 시작
    S2C_MOVE_STOP,          // 서버 → 클라이언트: 다른 플레이어 이동 정지

    //--------------------------------------------------
    // 스폰 / 삭제
    //--------------------------------------------------
    S2C_CREATE_MY_PLAYER,   // 서버 → 클라이언트: 내 캐릭터 생성
    S2C_CREATE_OTHER_PLAYER,// 서버 → 클라이언트: 다른 캐릭터 생성 (시야 진입)
    S2C_DELETE_PLAYER,      // 서버 → 클라이언트: 캐릭터 삭제 (시야 이탈/퇴장)

    //--------------------------------------------------
    // 채팅
    //--------------------------------------------------
    C2S_CHAT,               // 클라이언트 → 서버: 채팅 메시지 전송
    S2C_CHAT,               // 서버 → 클라이언트: 채팅 메시지 브로드캐스트

    //--------------------------------------------------
    // 좌표 보정
    //--------------------------------------------------
    S2C_SYNC_POSITION,  // 서버 → 클라이언트: 좌표 강제 보정

    //--------------------------------------------------
    // 존 정보 / 존 이동
    //--------------------------------------------------
    S2C_ZONE_INFO,        // 서버 → 클라이언트: 존 메타 정보 (맵 크기 등)
    C2S_ZONE_CHANGE,      // 클라이언트 → 서버: 맵 이동 요청
    S2C_ZONE_CHANGE_OK,   // 서버 → 클라이언트: 이동 성공
    S2C_ZONE_CHANGE_FAIL, // 서버 → 클라이언트: 이동 실패

    //--------------------------------------------------
    // 하트비트
    //--------------------------------------------------
    C2S_HEARTBEAT,          // 클라이언트 → 서버: 연결 유지 하트비트

    //--------------------------------------------------
    // 운영자
    //--------------------------------------------------
    C2S_ADMIN_LOGIN,        // 클라이언트 → 서버: 운영자 인증 요청
    S2C_ADMIN_LOGIN_OK,     // 서버 → 클라이언트: 인증 성공
    S2C_ADMIN_LOGIN_FAIL,   // 서버 → 클라이언트: 인증 실패

    //--------------------------------------------------
    // 에러
    //--------------------------------------------------
    S2C_ERROR,

    //--------------------------------------------------
    // 섹터 묶음 업데이트 (USE_SECTOR_AGGREGATION)
    //--------------------------------------------------
    S2C_SECTOR_UPDATES,     // 서버 → 클라이언트: 한 섹터의 이번 틱 최종 위치/상태 묶음

    //--------------------------------------------------
    // 멤버십 인바운드 묶음 (USE_MEMBERSHIP_INBOUND_BUNDLE)
    //--------------------------------------------------
    S2C_CREATE_PLAYER_BATCH,// 서버 → 클라이언트: 시야 진입 상대 묶음 (mover 전용, CREATE N개→1패킷)
    S2C_DELETE_PLAYER_BATCH,// 서버 → 클라이언트: 시야 이탈 상대 묶음 (mover 전용, DELETE N개→1패킷)

    //--------------------------------------------------
    // 전투 (USE_COMBAT)
    //--------------------------------------------------
    C2S_ATTACK,             // 클라이언트 → 서버: 공격 (대상은 서버가 "앞 한 칸"으로 고른다)
    S2C_ATTACK,             // 서버 → 클라이언트: 공격 모션 (대상이 없는 헛스윙도 보낸다)
    S2C_DAMAGE,             // 서버 → 클라이언트: 피해 적용 결과
    S2C_DEATH,              // 서버 → 클라이언트: 죽음
    S2C_RESPAWN,            // 서버 → 클라이언트: 부활
    S2C_ACTOR_STATS,        // 서버 → 클라이언트: 스탯 통보 (CREATE 직후 — 본인·시야 진입 상대)

    //--------------------------------------------------
    // 입장 (USE_LOGIN)
    //--------------------------------------------------
    // ※ 새 타입은 반드시 맨 끝에 붙인다 — 중간에 넣으면 뒤의 값이 전부 한 칸씩 밀려
    //   콘솔·부하·웹·언리얼 네 클라가 조용히 엉뚱한 패킷을 읽는다.
    //   WebClient/proto.js 의 번호표도 같은 값으로 같이 고칠 것.
    C2S_ENTER_GAME,         // 클라이언트 → 서버: 입장 요청 (계정 번호 + 입장권 자리)

    //--------------------------------------------------
    // 경험치·레벨 (USE_EXP)
    //--------------------------------------------------
    S2C_EXP,                // 서버 → 클라이언트: 레벨·경험치 통보 (본인 전용)

    //--------------------------------------------------
    // 인벤토리 (USE_INVENTORY)
    //--------------------------------------------------
    S2C_INVENTORY,          // 서버 → 클라이언트: 가방 전체 (입장 때 한 번 — 빈 칸은 안 싣는다)
    S2C_INVENTORY_SLOT      // 서버 → 클라이언트: 가방 한 칸이 바뀜 (itemId 0 = 비었음)
};

// 패킷 헤더(MsgHeader · EchoMsgHeader)는 NetHeader.h 로 분리했다 — 전송 계층이
//   게임 패킷 정의를 보지 않게 하려는 것. 이 파일이 NetHeader.h 를 include하므로 사용처는 그대로다.
#pragma pack(push, 1)

// NetWorkLib_EchoTest: MsgHeader(4byte) + uint64_t 에코 값
constexpr uint16_t ECHO_BODY_SIZE  = static_cast<uint16_t>(sizeof(uint64_t));
constexpr uint16_t ECHO_TOTAL_SIZE = static_cast<uint16_t>(sizeof(MsgHeader) + sizeof(uint64_t));

//==================================================
// 이동
//==================================================

// C2S: 이동 시작 (방향키 누름)
struct MSG_C2S_MOVE_START
{
    MsgHeader header;
    uint8_t direction;    // Direction enum
    float x;              // 클라이언트 예측 좌표 (서버 허용 범위 내 수용)
    float y;
};

// C2S: 이동 정지 (방향키 뗌)
struct MSG_C2S_MOVE_STOP
{
    MsgHeader header;
    uint8_t direction;    // 마지막 방향 (캐릭터가 바라보는 방향)
    float x;              // 정지 시점 좌표
    float y;
};

// S2C: 다른 플레이어 이동 시작
struct MSG_S2C_MOVE_START
{
    static constexpr MsgType TYPE = MsgType::S2C_MOVE_START;
    MsgHeader header;
    int32_t playerId;     // 이동하는 플레이어
    uint8_t direction;
    float x;              // 이동 시작 시점 좌표
    float y;

    MSG_S2C_MOVE_START() : header{ sizeof(*this), TYPE }, playerId(0), direction(0), x(0), y(0) {}
};

// S2C: 다른 플레이어 이동 정지
struct MSG_S2C_MOVE_STOP
{
    static constexpr MsgType TYPE = MsgType::S2C_MOVE_STOP;
    MsgHeader header;
    int32_t playerId;
    uint8_t direction;
    float x;              // 정지 좌표
    float y;

    MSG_S2C_MOVE_STOP() : header{ sizeof(*this), TYPE }, playerId(0), direction(0), x(0), y(0) {}
};

//==================================================
// 스폰 / 삭제
//==================================================

// 표시 문자(displayChar)의 예약 구간 — 몬스터는 새 패킷을 파지 않고 이 값으로 종류를 알린다.
//   플레이어는 서버가 'A'~'Z','a'~'z','0'~'9' 62 종만 부여하므로(CalcDisplayChar)
//   0x20 미만이 통째로 비어 있다. 여기에 몬스터 종류를 싣는다.
//   ★ 이렇게 하면 CREATE 패킷의 구조·크기·필드 순서가 한 바이트도 안 바뀐다.
//     새 타입을 파면 네 클라(콘솔·부하더미·웹·언리얼)에 전부 case 를 넣어야 하고,
//     부하 더미는 모르는 타입에 크기 0 을 돌려줘 연결을 끊는다(DummyClient::GetPacketSize).
//   몬스터 이름·레벨처럼 실을 자리가 없는 정보가 필요해지면 그때 전용 패킷을 판다.
constexpr uint8_t DISPLAY_MONSTER_BASE = 0x01;   // 0x01~0x0F = 몬스터 종류
constexpr uint8_t DISPLAY_SNAKE        = 0x01;   // 뱀

// S2C: 내 캐릭터 생성 (접속 직후)
struct MSG_S2C_CREATE_MY_PLAYER
{
    static constexpr MsgType TYPE = MsgType::S2C_CREATE_MY_PLAYER;
    MsgHeader header;
    int32_t playerId;
    uint8_t direction;
    uint8_t displayChar;  // 서버 권위 표시 문자 (ASCII: A-Z, a-z, 0-9)
    uint8_t colorIndex;   // 서버 권위 색상 인덱스 (0-6)
    float x;
    float y;
    int32_t speed;

    MSG_S2C_CREATE_MY_PLAYER() : header{ sizeof(*this), TYPE }, playerId(0), direction(0), displayChar('A'), colorIndex(0), x(0), y(0), speed(0) {}
};

// 스폰 사유 (S2C_CREATE_OTHER_PLAYER 전용)
enum class SpawnReason : uint8_t
{
    NORMAL = 0,         // 걸어서 시야 진입
    ZONE_TRANSFER = 1,  // 존/채널 이동으로 등장
    CONNECT = 2         // 최초 접속으로 등장
};

// S2C: 다른 캐릭터 생성 (시야 진입)
struct MSG_S2C_CREATE_OTHER_PLAYER
{
    static constexpr MsgType TYPE = MsgType::S2C_CREATE_OTHER_PLAYER;
    MsgHeader header;
    int32_t playerId;
    uint8_t direction;
    uint8_t moveState;    // MoveState enum (진입 시 이동 중일 수 있음)
    uint8_t displayChar;  // 서버 권위 표시 문자 (ASCII: A-Z, a-z, 0-9)
    uint8_t colorIndex;   // 서버 권위 색상 인덱스 (0-6)
    uint8_t spawnReason;  // SpawnReason enum (등장 사유)
    float x;
    float y;
    int32_t speed;

    MSG_S2C_CREATE_OTHER_PLAYER() : header{ sizeof(*this), TYPE }, playerId(0), direction(0), moveState(0), displayChar('A'), colorIndex(0), spawnReason(0), x(0), y(0), speed(0) {}
};

// S2C: 캐릭터 삭제 (시야 이탈 / 퇴장)
struct MSG_S2C_DELETE_PLAYER
{
    static constexpr MsgType TYPE = MsgType::S2C_DELETE_PLAYER;
    MsgHeader header;
    int32_t playerId;

    MSG_S2C_DELETE_PLAYER() : header{ sizeof(*this), TYPE }, playerId(0) {}
};

//==================================================
// 채팅
//==================================================

// 채팅 문자 타입 — 폭이 OS마다 달라지면 안 되므로 char16_t로 고정한다.
//   wchar_t는 Windows 2바이트 / 리눅스 4바이트라, 그대로 쓰면 같은 구조체가 OS마다
//   다른 크기가 되어 와이어 포맷이 어긋난다(Windows 클라 ↔ 리눅스 서버가 깨진다).
//   Windows에서는 원래 2바이트였으므로 이 교체로 와이어 포맷은 바뀌지 않는다.
using ChatChar = char16_t;
constexpr int32_t CHAT_MSG_MAX_LEN = 512; // ChatChar 기준 글자 수 (1024바이트, 양 OS 동일)

// C2S: 채팅 메시지
struct MSG_C2S_CHAT
{
    MsgHeader header;
    ChatChar message[CHAT_MSG_MAX_LEN];
};

// S2C: 채팅 메시지 (발신자 정보 포함)
struct MSG_S2C_CHAT
{
    static constexpr MsgType TYPE = MsgType::S2C_CHAT;
    MsgHeader header;
    int32_t playerId;     // 발신자
    uint8_t displayChar;  // 발신자 표시 문자
    uint8_t colorIndex;   // 발신자 색상 인덱스
    ChatChar message[CHAT_MSG_MAX_LEN];

    MSG_S2C_CHAT() : header{ sizeof(*this), TYPE }, playerId(0), displayChar('A'), colorIndex(0), message{} {}
};

//==================================================
// 좌표 보정
//==================================================

// S2C: 서버 권위 좌표 강제 동기화 (자기 자신 + 타인 주기적 동기화 겸용)
struct MSG_S2C_SYNC_POSITION
{
    static constexpr MsgType TYPE = MsgType::S2C_SYNC_POSITION;
    MsgHeader header;
    int32_t playerId;
    float x;
    float y;

    MSG_S2C_SYNC_POSITION() : header{ sizeof(*this), TYPE }, playerId(0), x(0), y(0) {}
};

//==================================================
// 섹터 묶음 업데이트 (USE_SECTOR_AGGREGATION)
//==================================================

// 좌표 눈금 — 섹터 묶음 엔트리에서만 쓰는 전송 표현. 서버 내부 계산은 float 그대로다.
//   맵이 120타일인데 좌표를 float(4B)으로 실어 보내던 것을 1/512타일 눈금의 uint16(2B)으로 바꾼다.
//   오차는 최대 ±1/1024타일(0.00098) — 한 틱 이동량(속도30 × 40ms = 1.2타일)의 0.08%라 클라 보간에 묻힌다.
constexpr float POS_QUANT_SCALE     = 512.0f;
constexpr float POS_QUANT_INV_SCALE = 1.0f / 512.0f;   // 2의 거듭제곱이라 나눗셈 오차 없음

// 눈금이 uint16에 담기는 맵 크기 상한. 좌표 최댓값은 (맵크기 - 1)이므로 (128-1) × 512 = 65,024 ≤ 65,535.
// 129부터는 넘쳐서 좌표가 조용히 깨진다 — CZone::Init이 기동 시 막는다.
constexpr int32_t POS_QUANT_MAX_MAP_SIZE = 128;

// 좌표 → 눈금. 반올림(+0.5f)이어야 한다. 절삭이면 오차가 늘 음수 쪽으로만 생겨서,
// 클라가 이 좌표를 받아 자기 위치로 삼고 다시 MOVE_START로 보고하는 왕복마다
// 좌표가 한 방향으로 밀린다(수용 경로는 CGameServer::RecvMoveStart).
inline uint16_t QuantizePos(float v)
{
    if (v < 0.0f) v = 0.0f;   // 음수의 uint16 캐스팅은 정의되지 않음. 상한은 POS_QUANT_MAX_MAP_SIZE가 담당
    return static_cast<uint16_t>(v * POS_QUANT_SCALE + 0.5f);
}

// 눈금 → 좌표
inline float DequantizePos(uint16_t q)
{
    return static_cast<float>(q) * POS_QUANT_INV_SCALE;
}

// 방향·이동상태 한 바이트에 담기 — 하위 니블 Direction(0~4), 상위 니블 MoveState(0~1).
//   비트가 아니라 니블로 가른 건 패킷을 16진수로 덤프할 때 두 값이 자리로 구분돼서다.
inline uint8_t PackDirState(uint8_t direction, uint8_t moveState)
{
    return static_cast<uint8_t>((direction & 0x0F) | (moveState << 4));
}
inline uint8_t UnpackDir(uint8_t packed)   { return static_cast<uint8_t>(packed & 0x0F); }
inline uint8_t UnpackState(uint8_t packed) { return static_cast<uint8_t>(packed >> 4); }

// 묶음 1엔트리 — 한 플레이어의 이번 틱 최종 상태 (9B, pack(1))
//   필드명을 x/y가 아니라 qx/qy로 둔 건, 눈금 값을 좌표로 착각해 쓰는 코드를 컴파일 단계에서 걸러내려는 것이다.
struct SectorUpdateEntry
{
    int32_t  playerId;
    uint16_t qx;          // QuantizePos(x) — 복원은 DequantizePos
    uint16_t qy;          // QuantizePos(y)
    uint8_t  dirState;    // PackDirState(Direction, MoveState)
};

// 한 섹터 최대 엔트리 수. 패킷이 MAX_PACKET_SIZE(1458B)를 넘지 않게:
//   ※ 기준은 1460이 아니라 1458 — CSerialBuffer는 1460B를 잡지만 선두 2B가 길이 헤더 자리다.
//   엔트리가 9B로 줄어 (1458 - 헤더4 - count2) / 9 = 161개까지 들어가지만 100을 유지한다.
//   배달 단위까지 같이 바꾸면 엔트리 축소 효과와 청크 분할 변화가 A/B에 섞인다(100개 × 9B = 906B로 한 패킷에 넉넉하다).
// 균등 부하 평균 섹터 ~55명이라 평소엔 1패킷, 초과 시 청크 분할
constexpr int SECTOR_UPDATE_MAX_ENTRIES = 100;

// S2C: 한 섹터의 이번 틱 변경분 묶음 (이동 1건당 즉시 전달을 대체)
// 가변 길이 — 와이어에는 header + count + SectorUpdateEntry × count 만 실린다.
// entries는 최대치 배열로 선언하되 빌더가 count개만 직렬화하고 header.size를 백패치한다.
struct MSG_S2C_SECTOR_UPDATES
{
    static constexpr MsgType TYPE = MsgType::S2C_SECTOR_UPDATES;
    MsgHeader header;
    uint16_t  count;
    SectorUpdateEntry entries[SECTOR_UPDATE_MAX_ENTRIES];

    MSG_S2C_SECTOR_UPDATES() : header{ sizeof(*this), TYPE }, count(0), entries{} {}
};

//==================================================
// 멤버십 인바운드 묶음 (USE_MEMBERSHIP_INBOUND_BUNDLE)
//==================================================

// 묶음 1엔트리 — 시야에 들어온 상대 1명의 생성 정보 (21B, pack(1))
// 필드 구성은 MSG_S2C_CREATE_OTHER_PLAYER 바디와 동일 — 클라가 엔트리를 기존 CREATE 이벤트로 분해한다.
struct CreatePlayerBatchEntry
{
    int32_t playerId;
    uint8_t direction;    // Direction enum
    uint8_t moveState;    // MoveState enum (진입 시 이동 중일 수 있음)
    uint8_t displayChar;  // 서버 권위 표시 문자 (ASCII: A-Z, a-z, 0-9)
    uint8_t colorIndex;   // 서버 권위 색상 인덱스 (0-6)
    uint8_t spawnReason;  // SpawnReason enum (등장 사유)
    float   x;
    float   y;
    int32_t speed;
};

// 묶음 1엔트리 — 시야에서 나간 상대 1명 (4B, MSG_S2C_DELETE_PLAYER 바디와 동일)
struct DeletePlayerBatchEntry
{
    int32_t playerId;
};

// 배치당 최대 엔트리 수. 패킷이 MAX_PACKET_SIZE(1458B)를 넘지 않게 (SECTOR_UPDATE_MAX_ENTRIES와 동일 정책):
//   CREATE: (1458 - 헤더4 - count2) / 엔트리21 = 최대 69개 → 여유 두고 64.
//   DELETE: (1458 - 6) / 엔트리4 = 최대 363개 → 여유 두고 350.
// 초과분은 서버가 청크 분할 송신
constexpr int CREATE_PLAYER_BATCH_MAX_ENTRIES = 64;
constexpr int DELETE_PLAYER_BATCH_MAX_ENTRIES = 350;

// S2C: 시야 진입 상대 묶음 — 인바운드(상대→나) CREATE N개를 mover 1명에게 1패킷으로.
// 가변 길이 — 와이어에는 header + count + CreatePlayerBatchEntry × count 만 실린다.
// entries는 최대치 배열로 선언하되 빌더가 count개만 직렬화하고 header.size를 백패치한다.
struct MSG_S2C_CREATE_PLAYER_BATCH
{
    static constexpr MsgType TYPE = MsgType::S2C_CREATE_PLAYER_BATCH;
    MsgHeader header;
    uint16_t  count;
    CreatePlayerBatchEntry entries[CREATE_PLAYER_BATCH_MAX_ENTRIES];

    MSG_S2C_CREATE_PLAYER_BATCH() : header{ sizeof(*this), TYPE }, count(0), entries{} {}
};

// S2C: 시야 이탈 상대 묶음 — 인바운드(상대→나) DELETE N개를 mover 1명에게 1패킷으로.
struct MSG_S2C_DELETE_PLAYER_BATCH
{
    static constexpr MsgType TYPE = MsgType::S2C_DELETE_PLAYER_BATCH;
    MsgHeader header;
    uint16_t  count;
    DeletePlayerBatchEntry entries[DELETE_PLAYER_BATCH_MAX_ENTRIES];

    MSG_S2C_DELETE_PLAYER_BATCH() : header{ sizeof(*this), TYPE }, count(0), entries{} {}
};

//==================================================
// 존 정보 / 존 이동
//==================================================

// S2C: 존 메타 정보 (존 입장/이동 시 CREATE_MY_PLAYER 앞에 전송)
struct MSG_S2C_ZONE_INFO
{
    static constexpr MsgType TYPE = MsgType::S2C_ZONE_INFO;
    MsgHeader header;
    int32_t mapId;
    int32_t channelIndex;
    int32_t mapWidth;
    int32_t mapHeight;
    int32_t sectorSize;

    MSG_S2C_ZONE_INFO() : header{ sizeof(*this), TYPE }, mapId(0), channelIndex(0), mapWidth(0), mapHeight(0), sectorSize(0) {}
};

// C2S: 맵 이동 / 채널 이동 요청
struct MSG_C2S_ZONE_CHANGE
{
    MsgHeader header;
    int32_t targetMapId;
    int32_t targetChannelIndex = -1; // -1: 자동배정(기존 동작), 0 이상: 지정 채널
};

// S2C: 맵 이동 성공
struct MSG_S2C_ZONE_CHANGE_OK
{
    static constexpr MsgType TYPE = MsgType::S2C_ZONE_CHANGE_OK;
    MsgHeader header;
    int32_t mapId;
    int32_t channelIndex;
    int32_t playerId;
    uint8_t displayChar;  // 서버 권위 표시 문자
    uint8_t colorIndex;   // 서버 권위 색상 인덱스
    uint8_t direction;    // 서버 권위 방향 (Direction enum)
    float x;
    float y;

    MSG_S2C_ZONE_CHANGE_OK() : header{ sizeof(*this), TYPE }, mapId(0), channelIndex(0), playerId(0), displayChar('A'), colorIndex(0), direction(0), x(0), y(0) {}
};

// S2C: 맵 이동 실패
struct MSG_S2C_ZONE_CHANGE_FAIL
{
    static constexpr MsgType TYPE = MsgType::S2C_ZONE_CHANGE_FAIL;
    MsgHeader header;
    uint8_t reason;  // 0: 존재하지 않는 맵/채널, 1: 모든 채널 가득 참, 2: 이미 해당 채널

    MSG_S2C_ZONE_CHANGE_FAIL() : header{ sizeof(*this), TYPE }, reason(0) {}
};

//==================================================
// 운영자
//==================================================

constexpr int32_t ADMIN_KEY_MAX_LEN = 64;

// C2S: 운영자 인증 요청
struct MSG_C2S_ADMIN_LOGIN
{
    MsgHeader header;
    char key[ADMIN_KEY_MAX_LEN];
};

// S2C: 운영자 인증 성공
struct MSG_S2C_ADMIN_LOGIN_OK
{
    static constexpr MsgType TYPE = MsgType::S2C_ADMIN_LOGIN_OK;
    MsgHeader header;

    MSG_S2C_ADMIN_LOGIN_OK() : header{ sizeof(*this), TYPE } {}
};

// S2C: 운영자 인증 실패
struct MSG_S2C_ADMIN_LOGIN_FAIL
{
    static constexpr MsgType TYPE = MsgType::S2C_ADMIN_LOGIN_FAIL;
    MsgHeader header;

    MSG_S2C_ADMIN_LOGIN_FAIL() : header{ sizeof(*this), TYPE } {}
};

//==================================================
// 하트비트
//==================================================

// C2S: 연결 유지 하트비트 (페이로드 없음)
struct MSG_C2S_HEARTBEAT
{
    MsgHeader header;
};

//==================================================
// 에러
//==================================================

// S2C: 에러 응답
struct MSG_S2C_ERROR
{
    static constexpr MsgType TYPE = MsgType::S2C_ERROR;
    MsgHeader header;
    char message[256];

    MSG_S2C_ERROR() : header{ sizeof(*this), TYPE }, message{} {}
};

//==================================================
// 전투 (USE_COMBAT)
//==================================================
// 기존 구조체에는 한 필드도 넣지 않는다 — Protocol.h 한 벌을 콘솔·부하·웹·언리얼 네 클라가
//   같이 쓰므로, 구조가 바뀌면 넷을 같이 고쳐야 하고 옛 실행파일은 조용히 깨진다.
//   그래서 HP 통보도 CREATE에 얹지 않고 별도 타입(S2C_ACTOR_STATS)으로 뒤따라 보낸다.

// C2S: 공격 (페이로드 없음 — 방향은 서버가 알고 있고, 대상은 서버가 "앞 한 칸"으로 고른다)
struct MSG_C2S_ATTACK
{
    MsgHeader header;
};

// S2C: 공격 모션 — 대상이 없어도(헛스윙) 보낸다. 클라가 휘두르는 그림을 그리는 용도.
struct MSG_S2C_ATTACK
{
    static constexpr MsgType TYPE = MsgType::S2C_ATTACK;
    MsgHeader header;
    int32_t attackerId;
    uint8_t direction;    // Direction enum (휘두른 방향)

    MSG_S2C_ATTACK() : header{ sizeof(*this), TYPE }, attackerId(0), direction(0) {}
};

// S2C: 피해 적용 결과
struct MSG_S2C_DAMAGE
{
    static constexpr MsgType TYPE = MsgType::S2C_DAMAGE;
    MsgHeader header;
    int32_t targetId;
    int32_t attackerId;
    int32_t damage;
    int32_t remainHp;     // 적용 후 남은 체력. 0이면 S2C_DEATH가 뒤따른다

    MSG_S2C_DAMAGE() : header{ sizeof(*this), TYPE }, targetId(0), attackerId(0), damage(0), remainHp(0) {}
};

// S2C: 죽음
struct MSG_S2C_DEATH
{
    static constexpr MsgType TYPE = MsgType::S2C_DEATH;
    MsgHeader header;
    int32_t targetId;

    MSG_S2C_DEATH() : header{ sizeof(*this), TYPE }, targetId(0) {}
};

// S2C: 부활 (제자리·만피). 좌표를 싣는 건 나중에 부활 지점이 바뀌어도 와이어가 그대로이게 하려는 것.
struct MSG_S2C_RESPAWN
{
    static constexpr MsgType TYPE = MsgType::S2C_RESPAWN;
    MsgHeader header;
    int32_t targetId;
    float x;
    float y;
    int32_t hp;

    MSG_S2C_RESPAWN() : header{ sizeof(*this), TYPE }, targetId(0), x(0), y(0), hp(0) {}
};

// S2C: 스탯 통보 — CREATE 계열에 HP 자리가 없어서 뒤따라 보낸다.
//   시야에 들어온 상대뿐 아니라 본인에게도 보낸다(MSG_S2C_CREATE_MY_PLAYER에도 HP가 없다).
struct MSG_S2C_ACTOR_STATS
{
    static constexpr MsgType TYPE = MsgType::S2C_ACTOR_STATS;
    MsgHeader header;
    int32_t actorId;
    int32_t hp;
    int32_t maxHp;

    MSG_S2C_ACTOR_STATS() : header{ sizeof(*this), TYPE }, actorId(0), hp(0), maxHp(0) {}
};

//==================================================
// 입장 (USE_LOGIN)
//==================================================

// 입장권 자리 — 지금은 늘 빈 값이다. 인증 서버가 붙으면 그 토큰이 여기 들어가고, 서버는
//   토큰으로 계정 번호를 꺼낸다. 쓰지도 않을 자리를 지금 비워 두는 이유는, 나중에 필드를
//   더하면 Protocol.h 한 벌을 쓰는 클라 넷을 같이 고쳐야 하기 때문이다(접속당 한 번이라 값이 싸다).
//   ※ 자리만 둔다. 토큰을 검사하는 코드는 붙일 상대가 생길 때 쓴다.
constexpr int32_t ENTER_TICKET_MAX_LEN = 64;

// C2S: 입장 요청 — 접속 직후 한 번. 이 패킷이 와야 서버가 캐릭터를 불러온다.
struct MSG_C2S_ENTER_GAME
{
    MsgHeader header;
    int64_t   accountId;                      // 0 = "번호를 지정하지 않음"(서버가 붙인다). "새 캐릭터"라는 뜻은 아니다
    char      ticket[ENTER_TICKET_MAX_LEN];   // 지금은 안 쓴다 — 전부 0
};

//==================================================
// 경험치·레벨 (USE_EXP)
//==================================================

// S2C: 레벨·경험치 통보 — 본인에게만 보낸다.
//   남의 레벨은 화면에 안 띄운다. 띄우려면 CREATE 계열에 자리가 필요한데 그건 구조체 변경이라 안 한다.
//   ★ 값 넷을 다 실어 보내는 이유 — 클라가 레벨 표를 안 갖게 한다. 표는 서버 것이고,
//     클라는 막대를 그릴 분자·분모만 있으면 된다.
//       분자 = exp - levelBaseExp, 분모 = nextLevelExp - levelBaseExp
//   ★ 만렙에서는 nextLevelExp 가 0 이다. 클라는 0 을 보면 나누지 말고 "MAX" 로 그린다
//     (마지막 레벨의 값을 그대로 넣어 가득 찬 막대처럼 보이게 하는 것보다 덜 헷갈린다).
//   레벨업에 별도 타입을 안 판다 — level 이 달라진 것으로 알 수 있고, 늘어난 체력은
//     기존 S2C_ACTOR_STATS 를 다시 보내면 된다.
struct MSG_S2C_EXP
{
    static constexpr MsgType TYPE = MsgType::S2C_EXP;
    MsgHeader header;
    int32_t level;
    int64_t exp;             // 지금까지 얻은 총 누적 (레벨 안에서 다시 0부터 세지 않는다)
    int64_t levelBaseExp;    // 지금 레벨에 도달할 때의 누적 = 막대의 0 점
    int64_t nextLevelExp;    // 다음 레벨에 필요한 누적. 만렙이면 0

    MSG_S2C_EXP() : header{ sizeof(*this), TYPE }, level(1), exp(0), levelBaseExp(0), nextLevelExp(0) {}
};

//==================================================
// 인벤토리 (USE_INVENTORY)
//==================================================

// 탭과 칸 번호 — 칸 번호가 곧 DB 열쇠(계정, 칸)다.
//   탭마다 번호 구간을 띄워 둔다(장비 0~ · 소비 100~ · 기타 200~). 그래서 탭을 늘려도, 한 탭의
//   칸 수를 늘려도 기존 칸 번호가 안 밀리고 DB 를 고칠 일이 없다.
//   탭 하나 더 = 아래 enum 한 줄 + 구간 한 줄 + 클라 탭 하나.
//   ★ 칸 번호 → 탭을 가를 때 탭마다 if 를 쓰지 말고 이 표를 돈다 — 넷째 탭이 오면 if 하나가 나머지를 삼킨다.
//   ★ WebClient/proto.js 에 같은 표를 옮겨 둔다 — 한쪽만 고치면 웹이 엉뚱한 탭에 그린다.
enum class InvTab : uint8_t
{
    EQUIP,   // 장비 — 쌓이지 않는다(아이템 표의 maxStack 이 1)
    USE,     // 소비 — 물약
    ETC,     // 기타 — 재료
    COUNT
};

struct InvTabRange
{
    uint16_t firstSlot;   // 이 탭 첫 칸의 번호
    uint16_t slotCount;   // 이 탭의 칸 수
};

// InvTab 순서 그대로. 구간은 겹치지 않고 오름차순이어야 한다(아래 단언이 막는다).
constexpr InvTabRange INV_TAB_RANGES[] = {
    {   0, 20 },   // EQUIP
    { 100, 20 },   // USE
    { 200, 20 },   // ETC
};
constexpr int INVENTORY_TOTAL_SLOTS = 60;   // 위 칸 수의 합 — 가방 배열·전체 목록 패킷의 크기

static_assert(sizeof(INV_TAB_RANGES) / sizeof(INV_TAB_RANGES[0]) == static_cast<unsigned>(InvTab::COUNT),
              "INV_TAB_RANGES 는 InvTab 마다 한 줄이어야 한다");

constexpr bool InvTabRangesAreValid()
{
    int total = 0;
    for (unsigned i = 0; i < sizeof(INV_TAB_RANGES) / sizeof(INV_TAB_RANGES[0]); ++i)
    {
        if (INV_TAB_RANGES[i].slotCount == 0)
            return false;
        if (i > 0 && INV_TAB_RANGES[i].firstSlot <
                     INV_TAB_RANGES[i - 1].firstSlot + INV_TAB_RANGES[i - 1].slotCount)
            return false;   // 앞 탭 구간과 겹친다
        total += INV_TAB_RANGES[i].slotCount;
    }
    return total == INVENTORY_TOTAL_SLOTS;
}
static_assert(InvTabRangesAreValid(), "탭 구간이 겹치거나 칸 수의 합이 INVENTORY_TOTAL_SLOTS 와 다르다");

// 가방 한 칸 (10B, pack(1))
struct InventoryEntry
{
    uint16_t slot;     // 칸 번호 (위 구간 안)
    int32_t  itemId;   // 아이템 번호 (Run/bin/ItemTable.csv). 0 = 빈 칸 — 전체 목록에는 안 실린다
    int32_t  count;    // 개수 (1 ~ 그 아이템의 maxStack)
};

// S2C: 가방 전체 — 입장 때 한 번.
//   가변 길이 — 와이어에는 header + count + InventoryEntry × count 만 실린다(빈 칸은 안 싣는다).
//   ★ 빈 가방이어도 한 장 간다(count 0). 부하 더미도 받으므로 더미가 이 타입을 몰라서는 안 된다.
struct MSG_S2C_INVENTORY
{
    static constexpr MsgType TYPE = MsgType::S2C_INVENTORY;
    MsgHeader      header;
    uint16_t       count;
    InventoryEntry entries[INVENTORY_TOTAL_SLOTS];

    MSG_S2C_INVENTORY() : header{ sizeof(*this), TYPE }, count(0), entries{} {}
};

// S2C: 가방 한 칸이 바뀜 — 넣기·쓰기 모두 이것 하나로 알린다. 칸의 "최종값"이다(증감 아님).
struct MSG_S2C_INVENTORY_SLOT
{
    static constexpr MsgType TYPE = MsgType::S2C_INVENTORY_SLOT;
    MsgHeader header;
    uint16_t  slot;
    int32_t   itemId;   // 0 = 이 칸이 비었다
    int32_t   count;

    MSG_S2C_INVENTORY_SLOT() : header{ sizeof(*this), TYPE }, slot(0), itemId(0), count(0) {}
};

#pragma pack(pop)
