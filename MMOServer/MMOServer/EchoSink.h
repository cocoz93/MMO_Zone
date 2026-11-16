#pragma once

#include "IOCPServer.h"       // CIOCPServer · CRecvSink
#include "MonitorManager.h"
#include "SerialBuffer.h"

// 에코 모드의 받은 것 넘길 곳 — 받은 메시지를 워커 스레드에서 그대로 되돌려 보낸다(게임 루프 없음).
//   전에 코어 CIOCPServer::EchoTestSend 가 하던 일을 그대로 옮겼다 — 코어는 이제 에코를 모른다.
//   접속 · 끊김은 할 일이 없다(전에도 에코 모드는 이벤트를 안 냈다).
//   네트워크가 이 부품을 쥐므로(SetRecvSink) 네트워크보다 오래 살 일은 없다.
class CEchoSink final : public CRecvSink
{
public:
    CEchoSink(CIOCPServer& network, CMonitorManager& monitor)
        : _network(network), _monitor(monitor) {}

    void OnConnected(const SinkConnected&) override {}

    void OnReceived(const SinkReceived& info) override
    {
        // 송신 메트릭은 호출자(여기)가 집계 (RequestSendMsg는 enqueue 성공 여부만 반환)
        const int dataSize = info.msg->GetDataSize();
        if (_network.RequestSendMsg(info.sessionId, info.msg))   // ref 1 을 송신 쪽으로 넘긴다(실패해도 RequestSendMsg 가 돌려준다)
        {
            _monitor._sendPackets.Inc();
            _monitor._sendEnqueuedBytes.Add(static_cast<LONG64>(dataSize));
        }
    }

    void OnDisconnected(const SinkDisconnected&) override {}

private:
    CIOCPServer&     _network;
    CMonitorManager& _monitor;
};
