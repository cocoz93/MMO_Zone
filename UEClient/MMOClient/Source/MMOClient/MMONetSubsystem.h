// MMONetSubsystem.h — 게임 스레드 쪽 창구
//   원본 대응: MMO/GameClient 의 CClientNetwork(접속·송신) + CGameInstance(이벤트 소비)
//   수신 스레드가 큐에 넣은 것을 매 프레임 여기서 꺼내 게임 스레드에서만 처리한다.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "MMONetTypes.h"
#include "MMONetSubsystem.generated.h"

class FSocket;
class FRunnableThread;
class FMMONetReceiver;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnMMONetEvent, const FMMONetEvent&);

UCLASS()
class MMOCLIENT_API UMMONetSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool Connect(const FString& ServerIp, int32 Port);
	void Disconnect();
	bool IsConnected() const { return Socket != nullptr; }

	// 게임 스레드에서만 브로드캐스트된다. 액터 스폰·이동은 여기 붙는다.
	FOnMMONetEvent OnNetEvent;

	// --- C2S 송신 (원본 CClientNetwork 의 Send* 대응) ---
	void SendMoveStart(uint8 Direction, float X, float Y);
	void SendMoveStop(uint8 Direction, float X, float Y);
	void SendChat(const FString& Message);
	void SendHeartbeat();

private:
	// 원본 SendPacket 과 같다 - 부분 전송이 나오면 오류로 보고 끊는다.
	bool SendRaw(const void* Data, int32 Length);

	// 매 프레임 큐를 비운다 (게임 스레드).
	bool TickQueue(float DeltaTime);

private:
	FSocket*           Socket   = nullptr;
	FRunnableThread*   Thread   = nullptr;
	FMMONetReceiver*   Receiver = nullptr;

	TQueue<FMMONetEvent, EQueueMode::Spsc> EventQueue;
	FTSTicker::FDelegateHandle TickHandle;

	// 한 프레임에 처리할 이벤트 상한.
	//   섹터 묶음이 낱개로 쪼개져 들어오면 한 번에 수백 개가 쌓일 수 있다.
	//   프레임이 통째로 멈추지 않도록 끊어서 소비한다.
	static constexpr int32 MaxEventsPerFrame = 512;

	// 하트비트 - 안 보내면 서버가 세션을 끊는다(mmo_session_timed_out_total 로 확인됨).
	float HeartbeatTimer = 0.f;
	static constexpr float HeartbeatIntervalSec = 5.0f;
};