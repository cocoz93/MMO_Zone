// MMONetSubsystem.cpp
#include "MMONetSubsystem.h"

#include "MMONetReceiver.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Protocol.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogMMONetSub, Log, All);

void UMMONetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UMMONetSubsystem::TickQueue));

	// 접속 대상을 정한다.
	//   -MMOConnect=IP:PORT 가 붙어 있으면 그것을, 없으면 기본값(로컬 서버)을 쓴다.
	//   기본값을 두는 이유는 에디터에서 플레이(PIE)를 눌렀을 때도 붙게 하려는 것이다.
	//   PIE 는 누를 때마다 게임 인스턴스를 새로 만들므로 이 Initialize 가 그때 불린다.
	//   서버가 떠 있지 않으면 Connect 가 실패 로그만 남기고 넘어간다.
	FString Ip   = TEXT("127.0.0.1");
	int32   Port = 6000;

	FString Param;
	if (FParse::Value(FCommandLine::Get(), TEXT("MMOConnect="), Param))
	{
		FString ParsedIp, PortStr;
		if (Param.Split(TEXT(":"), &ParsedIp, &PortStr))
		{
			Ip   = ParsedIp;
			Port = FCString::Atoi(*PortStr);
		}
		else
		{
			UE_LOG(LogMMONetSub, Warning, TEXT("-MMOConnect 형식은 IP:PORT 여야 한다(기본값으로 붙는다): %s"), *Param);
		}
	}

	Connect(Ip, Port);
}

void UMMONetSubsystem::Deinitialize()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	Disconnect();

	Super::Deinitialize();
}

bool UMMONetSubsystem::Connect(const FString& ServerIp, int32 Port)
{
	if (Socket != nullptr)
	{
		UE_LOG(LogMMONetSub, Warning, TEXT("이미 연결되어 있다."));
		return false;
	}

	FIPv4Address Addr;
	if (!FIPv4Address::Parse(ServerIp, Addr))
	{
		UE_LOG(LogMMONetSub, Error, TEXT("주소를 해석할 수 없다: %s"), *ServerIp);
		return false;
	}

	ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SocketSub == nullptr) { return false; }

	Socket = SocketSub->CreateSocket(NAME_Stream, TEXT("MMOClient"), false);
	if (Socket == nullptr)
	{
		UE_LOG(LogMMONetSub, Error, TEXT("소켓을 만들지 못했다."));
		return false;
	}

	// 원본 GameClient 는 setsockopt 를 하나도 호출하지 않는다(기본값 그대로).
	// 여기서도 옵션을 건드리지 않는다 — 이식 대조에서 동작 차이를 만들지 않기 위해서다.

	TSharedRef<FInternetAddr> RemoteAddr = SocketSub->CreateInternetAddr();
	RemoteAddr->SetIp(Addr.Value);
	RemoteAddr->SetPort(Port);

	if (!Socket->Connect(*RemoteAddr))
	{
		UE_LOG(LogMMONetSub, Error, TEXT("접속 실패: %s:%d"), *ServerIp, Port);
		SocketSub->DestroySocket(Socket);
		Socket = nullptr;
		return false;
	}

	Receiver = new FMMONetReceiver(Socket, &EventQueue);
	Thread   = FRunnableThread::Create(Receiver, TEXT("MMONetReceiver"));

	UE_LOG(LogMMONetSub, Log, TEXT("접속 성공: %s:%d"), *ServerIp, Port);
	return true;
}

void UMMONetSubsystem::Disconnect()
{
	if (Receiver) { Receiver->Stop(); }

	if (Socket)
	{
		Socket->Close();   // 수신 스레드의 blocking Recv 를 풀어 준다.
	}

	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	if (Receiver)
	{
		delete Receiver;
		Receiver = nullptr;
	}
	if (Socket)
	{
		if (ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			SocketSub->DestroySocket(Socket);
		}
		Socket = nullptr;
	}

	EventQueue.Empty();
}

bool UMMONetSubsystem::TickQueue(float DeltaTime)
{
	// 연결 유지 - 서버가 하트비트 없는 세션을 타임아웃으로 끊는다.
	if (Socket != nullptr)
	{
		HeartbeatTimer += DeltaTime;
		if (HeartbeatTimer >= HeartbeatIntervalSec)
		{
			HeartbeatTimer = 0.f;
			SendHeartbeat();
		}
	}

	int32 Processed = 0;
	FMMONetEvent Ev;
	while (Processed < MaxEventsPerFrame && EventQueue.Dequeue(Ev))
	{
		OnNetEvent.Broadcast(Ev);
		++Processed;
	}
	return true;   // 계속 틱
}

bool UMMONetSubsystem::SendRaw(const void* Data, int32 Length)
{
	if (Socket == nullptr) { return false; }

	int32 BytesSent = 0;
	if (!Socket->Send(static_cast<const uint8*>(Data), Length, BytesSent))
	{
		UE_LOG(LogMMONetSub, Error, TEXT("send 실패"));
		Disconnect();
		return false;
	}

	// 원본 ClientNetwork.cpp:175-180 과 같은 판정 — 부분 전송은 오류로 본다.
	if (BytesSent < Length)
	{
		UE_LOG(LogMMONetSub, Error, TEXT("Partial send: %d/%d"), BytesSent, Length);
		Disconnect();
		return false;
	}
	return true;
}

void UMMONetSubsystem::SendMoveStart(uint8 Direction, float X, float Y)
{
	// C2S 구조체에는 기본 생성자가 없다(S2C 쪽만 header 를 자동으로 채운다).
	// 원본과 같이 손으로 채운다.
	MSG_C2S_MOVE_START Msg;
	Msg.header.size = sizeof(MSG_C2S_MOVE_START);
	Msg.header.type = MsgType::C2S_MOVE_START;
	Msg.direction   = Direction;
	Msg.x           = X;
	Msg.y           = Y;
	SendRaw(&Msg, sizeof(Msg));
}

void UMMONetSubsystem::SendMoveStop(uint8 Direction, float X, float Y)
{
	MSG_C2S_MOVE_STOP Msg;
	Msg.header.size = sizeof(MSG_C2S_MOVE_STOP);
	Msg.header.type = MsgType::C2S_MOVE_STOP;
	Msg.direction   = Direction;
	Msg.x           = X;
	Msg.y           = Y;
	SendRaw(&Msg, sizeof(Msg));
}

void UMMONetSubsystem::SendHeartbeat()
{
	MSG_C2S_HEARTBEAT Msg;
	Msg.header.size = sizeof(MSG_C2S_HEARTBEAT);
	Msg.header.type = MsgType::C2S_HEARTBEAT;
	SendRaw(&Msg, sizeof(Msg));
}

void UMMONetSubsystem::SendChat(const FString& Message)
{
	// U4 에서 마무리한다. 확인할 것 둘:
	//   1) 원본 CClientNetwork::SendChat 이 구조체 전체를 보내는지, 쓴 길이만 보내는지
	//      (MSG_C2S_CHAT 은 ChatChar[512] 고정이라 통째로 보내면 1KB 가 넘는다)
	//   2) FString(TCHAR) -> ChatChar(char16_t) 복사 — Windows 에서는 둘 다 2바이트 UTF-16 이라
	//      재인코딩 없이 옮길 수 있다. 길이 절단은 CHAT_MSG_MAX_LEN 기준.
	(void)Message;
}