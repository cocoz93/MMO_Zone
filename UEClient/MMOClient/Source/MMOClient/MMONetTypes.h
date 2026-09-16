// MMONetTypes.h — 수신 스레드가 게임 스레드로 넘기는 이벤트
//   원본: MMO/GameClient/NetworkEventQueue.h 의 ClientNetworkEvent
//   언리얼에서는 게임 스레드 밖에서 액터를 건드릴 수 없다. 수신 스레드는
//   패킷을 이 구조체로만 바꿔 큐에 넣고, 액터 조작은 전부 게임 스레드에서 한다.
#pragma once

#include "CoreMinimal.h"

enum class EMMONetEventType : uint8
{
	ZoneInfo,
	CreateMyPlayer,
	CreateOtherPlayer,
	DeletePlayer,
	MoveStart,
	MoveStop,
	Chat,
	SyncPosition,
	ZoneChangeOk,
	ZoneChangeFail,
	ErrorMsg,
	Disconnected      // 원본에 없음 - 언리얼 쪽에서 연결 끊김을 게임 스레드에 알리기 위해 추가
};

// 서버의 4방향. 값이 MMOServer/Player.h 의 Direction 과 같아야 한다 - 와이어로 이 숫자가 그대로 오간다.
//   대각선은 없다. 좌표계는 서버 격자 기준이다.
enum class EMMODirection : uint8
{
	None = 0,
	Up,      // 서버 y 감소
	Down,    // 서버 y 증가
	Left,    // 서버 x 감소
	Right    // 서버 x 증가
};

struct FMMONetEvent
{
	EMMONetEventType Type = EMMONetEventType::ErrorMsg;

	// 공용 필드 (패킷 타입에 따라 쓰이는 것이 다르다 - 원본과 같은 방식)
	int32   PlayerId    = 0;
	uint8   DisplayChar = 0;
	uint8   ColorIndex  = 0;
	float   X           = 0.f;
	float   Y           = 0.f;
	uint8   Direction   = 0;
	uint8   MoveState   = 0;
	uint8   SpawnReason = 0;
	int32   Speed       = 0;

	// 존 정보
	int32   MapWidth    = 0;
	int32   MapHeight   = 0;
	int32   SectorSize  = 0;

	// 존 이동
	int32   MapId        = 0;
	int32   ChannelIndex = 0;
	uint8   Reason       = 0;

	// 채팅 / 에러
	//   와이어는 ChatChar(char16_t). FString 은 Windows 에서 같은 2바이트 UTF-16 이라
	//   변환 없이 그대로 담긴다.
	FString Message;
};