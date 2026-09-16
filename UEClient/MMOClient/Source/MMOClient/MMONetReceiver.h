// MMONetReceiver.h — 수신 스레드 (원본: MMO/GameClient/ClientNetwork.cpp 의 RecvThread)
//   언리얼에서는 std::thread 대신 FRunnable 을 쓴다. 하는 일은 원본과 같다:
//   소켓에서 바이트를 받아 누적 버퍼에 쌓고, 완성된 패킷만 잘라내 이벤트로 바꾼다.
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "Containers/Queue.h"
#include "MMONetTypes.h"

class FSocket;

class FMMONetReceiver : public FRunnable
{
public:
	// 원본 ClientNetwork.h 의 RECV_BUFFER_SIZE 와 같은 값을 쓴다.
	static constexpr int32 RecvBufferSize = 8192;

	FMMONetReceiver(FSocket* InSocket, TQueue<FMMONetEvent, EQueueMode::Spsc>* InQueue);
	virtual ~FMMONetReceiver() override;

	// FRunnable
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

	bool IsRunning() const { return bRunning; }

private:
	// 완성된 패킷 하나를 이벤트로 바꿔 큐에 넣는다.
	//   원본의 DispatchPacket 자리. 다만 여기서는 게임 로직을 부르지 않는다 —
	//   수신 스레드에서 액터를 건드릴 수 없기 때문이다.
	void ParsePacket(const uint8* Data, uint16 Size);

	// 연결이 끊겼음을 게임 스레드에 알린다.
	void PushDisconnected();

private:
	FSocket* Socket = nullptr;
	TQueue<FMMONetEvent, EQueueMode::Spsc>* Queue = nullptr;

	FThreadSafeBool bRunning = false;

	// TCP 스트림 조립용 누적 버퍼 (원본과 같은 방식 — 앞에서 잘라내고 memmove 로 당긴다)
	uint8 RecvBuffer[RecvBufferSize];
	int32 RecvBufferUsed = 0;
};