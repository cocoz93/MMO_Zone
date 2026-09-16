// ProtocolPilot.cpp — U0b 파일럿
//
//   목적 둘.
//     1) Shared/Protocol/Protocol.h 가 언리얼 빌드에서 그대로 컴파일되는가
//        (Build.cs 의 경로 탐색이 먹는지 확인하는 것도 겸한다)
//     2) #pragma pack(1) 이 언리얼에서도 그대로 먹는가
//
//   2번이 중요하다. 정렬이 어긋나면 컴파일은 통과하고 패킷만 조용히 깨진다.
//   그때는 "서버가 이상한 값을 준다"로 보여서 원인을 찾기 어렵다.
//   아래 크기는 Protocol.h 의 필드를 pack(1) 기준으로 합산한 값이다.

#include "Protocol.h"

static_assert(sizeof(MsgHeader) == 4,
	"MsgHeader 는 4바이트여야 한다 (uint16 size + uint16 type). pack(1) 이 안 먹었을 수 있다.");

static_assert(sizeof(MSG_C2S_MOVE_START) == 13,
	"MSG_C2S_MOVE_START = header4 + uint8 1 + float 4 + float 4 = 13");

static_assert(sizeof(MSG_C2S_MOVE_STOP) == 13,
	"MSG_C2S_MOVE_STOP = header4 + uint8 1 + float 4 + float 4 = 13");

static_assert(sizeof(MSG_C2S_HEARTBEAT) == 4,
	"MSG_C2S_HEARTBEAT = header 만 = 4");

static_assert(sizeof(MSG_S2C_ZONE_INFO) == 24,
	"MSG_S2C_ZONE_INFO = header4 + int32 x5 = 24");

static_assert(sizeof(MSG_S2C_CREATE_MY_PLAYER) == 23,
	"MSG_S2C_CREATE_MY_PLAYER = header4 + int32 4 + uint8 x3 + float x2 + int32 4 = 23");

// 채팅은 와이어 문자 타입이 char16_t 여야 한다.
//   wchar_t 로 두면 Windows 2바이트 / 리눅스 4바이트가 되어 같은 구조체가 OS 마다 달라진다.
static_assert(sizeof(ChatChar) == 2,
	"ChatChar 는 2바이트(char16_t) 여야 한다.");

// 이 파일은 검증만 한다. 모듈에 심볼 하나는 남겨 둔다.
namespace MMOClientProtocolPilot { void Verified() {} }