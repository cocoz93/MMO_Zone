// MMONetPlayerComponent.h — 내 캐릭터를 서버 좌표에 맞추는 부품
//   캐릭터(AMMOClientCharacter)에 붙어서 두 가지를 한다.
//     1) 서버가 보내온 좌표로 캐릭터를 옮긴다
//     2) 키 입력을 4방향으로 바꿔 서버에 알린다
//   ★ 이 클라는 좌표를 스스로 굴리지 않는다. 콘솔 클라(GameClient)와 같은 방식으로
//     좌표의 정본은 언제나 서버다. 키를 눌러도 서버가 답하기 전에는 제자리다.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MMONetTypes.h"
#include "MMONetPlayerComponent.generated.h"

class UMMONetSubsystem;

UCLASS(ClassGroup=(MMO), meta=(BlueprintSpawnableComponent))
class MMOCLIENT_API UMMONetPlayerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMMONetPlayerComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// 캐릭터가 받은 이동 입력(-1~1 두 축)을 4방향으로 바꿔 서버로 보낸다.
	void HandleMoveInput(float Right, float Forward);

	// 이동 키에서 손을 뗐을 때.
	void HandleMoveReleased();

private:
	void OnNetEvent(const FMMONetEvent& Ev);

	// 접속 중인 서브시스템을 꺼내 온다. 아직 없으면 nullptr.
	UMMONetSubsystem* GetNet() const;

	// 서버 격자 좌표를 언리얼 월드 좌표로 옮긴다. 높이(Z)는 지금 값을 유지한다.
	void ApplyServerPosition(float InGridX, float InGridY);

	// 두 축 입력을 4방향 하나로 줄인다. 서버에 대각선이 없으므로 더 기운 축을 고른다.
	static EMMODirection QuantizeToFourWay(float Right, float Forward);

	// 서버 방향을 언리얼 Yaw 각도로. 서버 x→언리얼 X, 서버 y→언리얼 Y 로 놓았을 때의 값이다.
	static float DirectionToYaw(EMMODirection Dir);

private:
	// 서버가 알려준 내 플레이어 번호. CREATE_MY_PLAYER 를 받기 전에는 아무것도 하지 않는다.
	int32 MyPlayerId   = 0;
	bool  bHasMyPlayer = false;

	// 서버에 마지막으로 알린 방향. 같은 방향을 반복해서 보내지 않으려고 들고 있다.
	//   원본 콘솔 클라도 방향이 바뀔 때만 MOVE_START 를 보낸다.
	EMMODirection SentDirection = EMMODirection::None;

	// 서버 기준 내 격자 좌표. 이동 패킷에 이 값을 실어 보낸다.
	float GridX = 0.f;
	float GridY = 0.f;

	FDelegateHandle NetEventHandle;

	// 서버 격자 한 칸 = 언리얼 100cm(1m).
	//   서버 맵이 120x120 이므로 언리얼에서는 120m x 120m 가 된다.
	static constexpr float GridToUU = 100.f;
};
