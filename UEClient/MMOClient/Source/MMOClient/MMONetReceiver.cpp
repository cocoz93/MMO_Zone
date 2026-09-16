// MMONetReceiver.cpp
//   조립 버퍼 로직은 MMO/GameClient/ClientNetwork.cpp:96-153 (RecvThread) 를 그대로 옮긴 것이다.
//   자가검증 때 원본과 줄 단위로 대조할 수 있도록 순서와 판정 조건을 바꾸지 않았다.
#include "MMONetReceiver.h"

#include "Sockets.h"
#include "Protocol.h"

DEFINE_LOG_CATEGORY_STATIC(LogMMONet, Log, All);

FMMONetReceiver::FMMONetReceiver(FSocket* InSocket, TQueue<FMMONetEvent, EQueueMode::Spsc>* InQueue)
	: Socket(InSocket)
	, Queue(InQueue)
{
	FMemory::Memzero(RecvBuffer, sizeof(RecvBuffer));
}

FMMONetReceiver::~FMMONetReceiver()
{
}

bool FMMONetReceiver::Init()
{
	bRunning = (Socket != nullptr && Queue != nullptr);
	return bRunning;
}

uint32 FMMONetReceiver::Run()
{
	while (bRunning)
	{
		// [원본 100-107] 남은 공간이 없으면 조립이 불가능하다 - 끊는다.
		const int32 Space = RecvBufferSize - RecvBufferUsed;
		if (Space <= 0)
		{
			UE_LOG(LogMMONet, Error, TEXT("Recv buffer overflow"));
			break;
		}

		// [원본 109-120] 0 이하는 정상 종료(0)와 오류(-1)를 함께 뜻했다.
		//   언리얼은 성공 여부(bool)와 읽은 바이트를 나눠 주므로 둘을 같이 본다.
		int32 BytesRead = 0;
		if (!Socket->Recv(RecvBuffer + RecvBufferUsed, Space, BytesRead) || BytesRead <= 0)
		{
			UE_LOG(LogMMONet, Warning, TEXT("Connection lost."));
			break;
		}

		RecvBufferUsed += BytesRead;

		// [원본 124-151] 완성된 패킷을 반복 추출
		while (RecvBufferUsed >= static_cast<int32>(sizeof(MsgHeader)))
		{
			const MsgHeader* Header = reinterpret_cast<const MsgHeader*>(RecvBuffer);
			const uint16 PacketSize = Header->size;

			// [원본 130-137] 유효성 검사 - 원본은 여기서 return 으로 즉시 끝낸다.
			if (PacketSize < sizeof(MsgHeader) || PacketSize > RecvBufferSize)
			{
				UE_LOG(LogMMONet, Error, TEXT("Invalid packet size: %u"), PacketSize);
				bRunning = false;
				PushDisconnected();
				return 0;
			}

			// [원본 139-141] 아직 덜 도착했으면 다음 수신을 기다린다.
			if (RecvBufferUsed < static_cast<int32>(PacketSize))
			{
				break;
			}

			ParsePacket(RecvBuffer, PacketSize);

			// [원본 146-150] 처리한 만큼 앞으로 당긴다.
			const int32 Remaining = RecvBufferUsed - PacketSize;
			if (Remaining > 0)
			{
				FMemory::Memmove(RecvBuffer, RecvBuffer + PacketSize, Remaining);
			}
			RecvBufferUsed = Remaining;
		}
	}

	bRunning = false;
	PushDisconnected();
	return 0;
}

void FMMONetReceiver::Stop()
{
	bRunning = false;
}

void FMMONetReceiver::Exit()
{
}

void FMMONetReceiver::PushDisconnected()
{
	if (Queue)
	{
		FMMONetEvent Ev;
		Ev.Type = EMMONetEventType::Disconnected;
		Queue->Enqueue(MoveTemp(Ev));
	}
}

void FMMONetReceiver::ParsePacket(const uint8* Data, uint16 Size)
{
	const MsgHeader* Header = reinterpret_cast<const MsgHeader*>(Data);

	switch (Header->type)
	{
	case MsgType::S2C_ZONE_INFO:
	{
		if (Size < sizeof(MSG_S2C_ZONE_INFO)) { return; }
		const MSG_S2C_ZONE_INFO* Msg = reinterpret_cast<const MSG_S2C_ZONE_INFO*>(Data);

		FMMONetEvent Ev;
		Ev.Type       = EMMONetEventType::ZoneInfo;
		Ev.MapId        = Msg->mapId;
		Ev.ChannelIndex = Msg->channelIndex;
		Ev.MapWidth   = Msg->mapWidth;
		Ev.MapHeight  = Msg->mapHeight;
		Ev.SectorSize = Msg->sectorSize;
		UE_LOG(LogMMONet, Log, TEXT("[수신] ZONE_INFO map=%d ch=%d %dx%d sector=%d"),
			Msg->mapId, Msg->channelIndex, Msg->mapWidth, Msg->mapHeight, Msg->sectorSize);
		Queue->Enqueue(MoveTemp(Ev));
		break;
	}

	case MsgType::S2C_CREATE_MY_PLAYER:
	{
		if (Size < sizeof(MSG_S2C_CREATE_MY_PLAYER)) { return; }
		const MSG_S2C_CREATE_MY_PLAYER* Msg = reinterpret_cast<const MSG_S2C_CREATE_MY_PLAYER*>(Data);

		FMMONetEvent Ev;
		Ev.Type        = EMMONetEventType::CreateMyPlayer;
		Ev.PlayerId    = Msg->playerId;
		Ev.Direction   = Msg->direction;
		Ev.DisplayChar = Msg->displayChar;
		Ev.ColorIndex  = Msg->colorIndex;
		Ev.X           = Msg->x;
		Ev.Y           = Msg->y;
		Ev.Speed       = Msg->speed;
		UE_LOG(LogMMONet, Log, TEXT("[수신] CREATE_MY_PLAYER id=%d (%.2f, %.2f) dir=%u speed=%d"),
			Msg->playerId, Msg->x, Msg->y, (uint32)Msg->direction, Msg->speed);
		Queue->Enqueue(MoveTemp(Ev));
		break;
	}

	case MsgType::S2C_MOVE_START:
	{
		if (Size < sizeof(MSG_S2C_MOVE_START)) { return; }
		const MSG_S2C_MOVE_START* Msg = reinterpret_cast<const MSG_S2C_MOVE_START*>(Data);

		FMMONetEvent Ev;
		Ev.Type      = EMMONetEventType::MoveStart;
		Ev.PlayerId  = Msg->playerId;
		Ev.Direction = Msg->direction;
		Ev.X         = Msg->x;
		Ev.Y         = Msg->y;
		Queue->Enqueue(MoveTemp(Ev));
		break;
	}

	case MsgType::S2C_MOVE_STOP:
	{
		if (Size < sizeof(MSG_S2C_MOVE_STOP)) { return; }
		const MSG_S2C_MOVE_STOP* Msg = reinterpret_cast<const MSG_S2C_MOVE_STOP*>(Data);

		FMMONetEvent Ev;
		Ev.Type      = EMMONetEventType::MoveStop;
		Ev.PlayerId  = Msg->playerId;
		Ev.Direction = Msg->direction;
		Ev.X         = Msg->x;
		Ev.Y         = Msg->y;
		Queue->Enqueue(MoveTemp(Ev));
		break;
	}

	case MsgType::S2C_SYNC_POSITION:
	{
		if (Size < sizeof(MSG_S2C_SYNC_POSITION)) { return; }
		const MSG_S2C_SYNC_POSITION* Msg = reinterpret_cast<const MSG_S2C_SYNC_POSITION*>(Data);

		FMMONetEvent Ev;
		Ev.Type     = EMMONetEventType::SyncPosition;
		Ev.PlayerId = Msg->playerId;
		Ev.X        = Msg->x;
		Ev.Y        = Msg->y;
		// 이동 중에는 서버 틱마다 온다. 로그를 매번 남기면 파일이 금방 커지므로 남기지 않는다.
		Queue->Enqueue(MoveTemp(Ev));
		break;
	}

	// 다음 단계("다른 사람들")에서 채운다:
	//   S2C_CREATE_OTHER_PLAYER / S2C_DELETE_PLAYER / S2C_CHAT
	//   S2C_SECTOR_UPDATES / S2C_CREATE_PLAYER_BATCH / S2C_DELETE_PLAYER_BATCH
	//   ★ 묶음 셋은 엔트리별로 쪼개 낱개 이벤트로 넣는다.
	//     원본 GameInstance 의 OnSectorUpdates / OnCreatePlayerBatch / OnDeletePlayerBatch 가 그렇게 한다.

	default:
		// 아직 다루지 않는 패킷 - 조립은 정상이므로 버리기만 한다.
		//   U3/U4 에서 채우기 전까지, 서버가 무엇을 보내는지 보려고 타입만 남긴다.
		UE_LOG(LogMMONet, Log, TEXT("[수신] 미처리 type=%u size=%u"), (uint32)Header->type, Size);
		break;
	}
}