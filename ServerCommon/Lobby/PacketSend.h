#pragma once
// ==========================================================================
// 구조체 패킷 보내기 · 받기 — 로비 서버와 서버 간 연결이 쓴다
//
//   새 패킷은 전부 머리(MsgHeader)로 시작하는 고정 구조체거나 「머리 + 줄 N개」다.
//   보낸 수·바이트는 CSessionHub::SendPacket 과 같은 코어 지표로 센다.
// ==========================================================================

#include <cstring>
#include <type_traits>

#include <Identity/GameIds.h>
#include "IOCPServer.h"
#include "MonitorManager.h"
#include "NetHeader.h"
#include "SerialBuffer.h"

namespace PacketSend
{
    // 머리까지 채운 바이트 덩어리 하나를 보낸다. 세션이 없거나 큐가 넘치면 false
    inline bool SendBytes(CIOCPServer& network, CMonitorManager& monitor, SessionId sessionId, const void* data, int size)
    {
        CSerialBuffer* msg = CSerialBuffer::Alloc();
        std::memcpy(msg->GetWriteBufferPtr(), data, static_cast<size_t>(size));
        msg->MoveWritePos(size);
        msg->Seal();
        // Alloc 이 잡아 준 소유권 1을 RequestSendMsg 가 소비한다(실패해도 회수한다)
        if (!network.RequestSendMsg(ToInt(sessionId), msg))
            return false;
        monitor._sendPackets.Inc();
        monitor._sendEnqueuedBytes.Add(static_cast<int64_t>(size));
        return true;
    }

    // 고정 크기 패킷 — 머리를 여기서 채운다
    template <typename TPacket>
    bool Send(CIOCPServer& network, CMonitorManager& monitor, SessionId sessionId, TPacket packet)
    {
        static_assert(std::is_trivially_copyable_v<TPacket>);
        packet.header.size = static_cast<uint16_t>(sizeof(TPacket));
        packet.header.type = static_cast<MsgType>(TPacket::TYPE);
        return SendBytes(network, monitor, sessionId, &packet, static_cast<int>(sizeof(TPacket)));
    }

    // 받은 버퍼에서 고정 크기 패킷을 읽는다(최소 크기는 표가 이미 봤다). 모자라면 false
    template <typename TPacket>
    bool Read(CSerialBuffer* msg, TPacket& out)
    {
        static_assert(std::is_trivially_copyable_v<TPacket>);
        return msg->PeekData(reinterpret_cast<char*>(&out), static_cast<int>(sizeof(TPacket))) == static_cast<int>(sizeof(TPacket));
    }
}
