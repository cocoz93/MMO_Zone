#pragma once
// ==========================================================================
// CLinkPeers — 위층 쪽: 서버 간 포트로 들어온 연결 목록 · 하트비트 · 끊김 알림
//
//   캐릭터 서버(존 서버들) · 계정 서버(캐릭터 서버들)가 쓴다. 로직 스레드 전용이라 락이 없다.
//   아래층이 몇 대인지 위층 설정에 없다 — 연결된 것이 곧 목록이다.
// ==========================================================================

#include <cstdint>
#include <functional>
#include <vector>

#include <Identity/GameIds.h>
#include <Lobby/PacketSend.h>
#include "IOCPServer.h"
#include "MonitorManager.h"

class CLinkPeers
{
public:
    using SessionFn = std::function<void(SessionId)>;

    CLinkPeers(CIOCPServer& network, CMonitorManager& monitor, int heartbeatSec);

    void SetHandlers(SessionFn onConnected, SessionFn onDisconnected)
    {
        _onConnected = std::move(onConnected);
        _onDisconnected = std::move(onDisconnected);
    }

    void OnConnected(SessionId sessionId);
    void OnDisconnected(SessionId sessionId);
    void Tick(int64_t nowMs);   // 하트비트

    int Count() const { return static_cast<int>(_peers.size()); }
    bool Has(SessionId sessionId) const;

    template <typename TPacket>
    bool Send(SessionId sessionId, const TPacket& packet)
    {
        return PacketSend::Send(_network, _monitor, sessionId, packet);
    }

    // 같은 번호로 다시 등록할 때 옛 연결을 끊는다(플랜 6절)
    void Disconnect(SessionId sessionId);

private:
    CIOCPServer& _network;
    CMonitorManager& _monitor;
    int _heartbeatSec;
    int64_t _lastHeartbeatMs = 0;
    std::vector<SessionId> _peers;
    SessionFn _onConnected;
    SessionFn _onDisconnected;
};
