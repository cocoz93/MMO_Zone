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
    S2C_DELETE_PLAYER_BATCH // 서버 → 클라이언트: 시야 이탈 상대 묶음 (mover 전용, DELETE N개→1패킷)
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

#pragma pack(pop)
