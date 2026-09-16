// MMONetPlayerComponent.cpp
#include "MMONetPlayerComponent.h"

#include "MMONetSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY_STATIC(LogMMOPlayer, Log, All);

UMMONetPlayerComponent::UMMONetPlayerComponent()
{
	// 서버가 보내줄 때만 움직이므로 매 프레임 돌 일이 없다.
	PrimaryComponentTick.bCanEverTick = false;
}

void UMMONetPlayerComponent::BeginPlay()
{
	Super::BeginPlay();

	if (UMMONetSubsystem* Net = GetNet())
	{
		NetEventHandle = Net->OnNetEvent.AddUObject(this, &UMMONetPlayerComponent::OnNetEvent);
	}
	else
	{
		UE_LOG(LogMMOPlayer, Warning, TEXT("네트워크 서브시스템을 찾지 못했다. 캐릭터가 서버와 이어지지 않는다."));
	}
}

void UMMONetPlayerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 구독을 남겨 두면 다음 판(PIE 재실행)에서 죽은 객체를 부른다.
	if (NetEventHandle.IsValid())
	{
		if (UMMONetSubsystem* Net = GetNet())
		{
			Net->OnNetEvent.Remove(NetEventHandle);
		}
		NetEventHandle.Reset();
	}

	Super::EndPlay(EndPlayReason);
}

UMMONetSubsystem* UMMONetPlayerComponent::GetNet() const
{
	const UWorld* World = GetWorld();
	if (World == nullptr) { return nullptr; }

	UGameInstance* GI = World->GetGameInstance();
	if (GI == nullptr) { return nullptr; }

	return GI->GetSubsystem<UMMONetSubsystem>();
}

void UMMONetPlayerComponent::OnNetEvent(const FMMONetEvent& Ev)
{
	switch (Ev.Type)
	{
	case EMMONetEventType::CreateMyPlayer:
	{
		MyPlayerId    = Ev.PlayerId;
		bHasMyPlayer  = true;
		SentDirection = static_cast<EMMODirection>(Ev.Direction);
		ApplyServerPosition(Ev.X, Ev.Y);

		if (AActor* Owner = GetOwner())
		{
			Owner->SetActorRotation(FRotator(0.f, DirectionToYaw(SentDirection), 0.f));
		}
		UE_LOG(LogMMOPlayer, Log, TEXT("내 캐릭터 배치: id=%d 격자(%.2f, %.2f) → 월드(%.0f, %.0f)"),
			MyPlayerId, Ev.X, Ev.Y, Ev.X * GridToUU, Ev.Y * GridToUU);
		break;
	}

	case EMMONetEventType::MoveStart:
	case EMMONetEventType::MoveStop:
	{
		// 내 것만 본다. 남의 캐릭터는 다음 단계에서 다룬다.
		if (!bHasMyPlayer || Ev.PlayerId != MyPlayerId) { break; }

		ApplyServerPosition(Ev.X, Ev.Y);

		// 서버가 확정한 방향으로 캐릭터를 돌린다. 4방향뿐이라 옆걸음이 안 나온다.
		if (AActor* Owner = GetOwner())
		{
			const EMMODirection Dir = static_cast<EMMODirection>(Ev.Direction);
			if (Dir != EMMODirection::None)
			{
				Owner->SetActorRotation(FRotator(0.f, DirectionToYaw(Dir), 0.f));
			}
		}
		break;
	}

	case EMMONetEventType::SyncPosition:
	{
		if (bHasMyPlayer && Ev.PlayerId == MyPlayerId)
		{
			ApplyServerPosition(Ev.X, Ev.Y);
		}
		break;
	}

	case EMMONetEventType::Disconnected:
	{
		bHasMyPlayer  = false;
		SentDirection = EMMODirection::None;
		UE_LOG(LogMMOPlayer, Warning, TEXT("연결이 끊겼다."));
		break;
	}

	default:
		break;
	}
}

void UMMONetPlayerComponent::ApplyServerPosition(float InGridX, float InGridY)
{
	GridX = InGridX;
	GridY = InGridY;

	AActor* Owner = GetOwner();
	if (Owner == nullptr) { return; }

	// 높이는 건드리지 않는다. 캐릭터가 처음 선 바닥 높이를 그대로 쓴다.
	//   서버는 2D 격자라 Z 를 모른다.
	const FVector Current = Owner->GetActorLocation();
	Owner->SetActorLocation(FVector(InGridX * GridToUU, InGridY * GridToUU, Current.Z));
}

EMMODirection UMMONetPlayerComponent::QuantizeToFourWay(float Right, float Forward)
{
	const float AbsRight   = FMath::Abs(Right);
	const float AbsForward = FMath::Abs(Forward);

	// 아주 작은 입력은 무시한다(게임패드 중립 위치의 떨림).
	constexpr float DeadZone = 0.2f;
	if (AbsRight < DeadZone && AbsForward < DeadZone) { return EMMODirection::None; }

	// 두 축이 같이 들어오면(대각선) 더 기운 쪽만 남긴다. 서버에 대각선이 없다.
	if (AbsForward >= AbsRight)
	{
		return (Forward > 0.f) ? EMMODirection::Up : EMMODirection::Down;
	}
	return (Right > 0.f) ? EMMODirection::Right : EMMODirection::Left;
}

float UMMONetPlayerComponent::DirectionToYaw(EMMODirection Dir)
{
	// 서버 x 를 언리얼 X, 서버 y 를 언리얼 Y 로 놓았다.
	//   언리얼 Yaw 는 0=+X, 90=+Y, 180=-X, 270=-Y 이다.
	switch (Dir)
	{
	case EMMODirection::Right: return 0.f;     // 서버 x 증가
	case EMMODirection::Down:  return 90.f;    // 서버 y 증가
	case EMMODirection::Left:  return 180.f;   // 서버 x 감소
	case EMMODirection::Up:    return 270.f;   // 서버 y 감소
	default:                   return 0.f;
	}
}

void UMMONetPlayerComponent::HandleMoveInput(float Right, float Forward)
{
	if (!bHasMyPlayer) { return; }

	const EMMODirection Dir = QuantizeToFourWay(Right, Forward);
	if (Dir == EMMODirection::None) { return; }

	// 같은 방향을 계속 누르고 있는 동안에는 다시 보내지 않는다.
	if (Dir == SentDirection) { return; }

	if (UMMONetSubsystem* Net = GetNet())
	{
		Net->SendMoveStart(static_cast<uint8>(Dir), GridX, GridY);
		SentDirection = Dir;
	}
}

void UMMONetPlayerComponent::HandleMoveReleased()
{
	if (!bHasMyPlayer || SentDirection == EMMODirection::None) { return; }

	if (UMMONetSubsystem* Net = GetNet())
	{
		Net->SendMoveStop(static_cast<uint8>(SentDirection), GridX, GridY);
		SentDirection = EMMODirection::None;
	}
}
