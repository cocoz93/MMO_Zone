#pragma once
// ==========================================================================
// CLinkConnector — 아래층 → 위층 접속 (존 서버 → 캐릭터 서버 · 캐릭터 서버 → 계정 서버)
//
//   ConnectTo 는 끝날 때까지 막히므로 로직 스레드에서 부르지 않는다. 재접속 스레드 하나가 맡는다.
//   연결이 되면 그 뒤는 로직 스레드 몫이다 — 코어가 CONNECTED(LinkUp) 이벤트를 올리면 OnConnected 가 등록 함수를 부른다.
//   스레드를 넘는 값은 원자 값(재접속 필요 여부 · 연결 상태) 둘이다.
//
//   시작 순서는 상관없다 — 위층이 없으면 RetrySec 마다 다시 접속한다.
//
// [수명] 코어 Start 뒤 Start · 코어 ShutdownServer 앞에 Stop (코어 약속 — 종료 중 세션 표를 건드리지 않게).
// [지표] mmo_link_up{peer} · mmo_link_reconnect_total{peer}
// ==========================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <Identity/GameIds.h>
#include <Lobby/PacketSend.h>
#include "IOCPServer.h"
#include "MonitorManager.h"

class CLinkConnector
{
public:
    struct Config
    {
        const char* peerName = "";   // 지표 라벨 peer — 미리 정한 목록뿐(account · character)
        std::string ip;
        int port = 0;
        int retrySec = 0;
        int heartbeatSec = 0;
    };

    // onUp — 연결이 됐다(로직 스레드). 위층에 등록 패킷을 보내는 자리다
    // onDown — 끊겼다(로직 스레드)
    using ConnectedFn = std::function<void(SessionId)>;
    using DisconnectedFn = std::function<void()>;

    CLinkConnector(CIOCPServer& network, CMonitorManager& monitor, Config config);
    ~CLinkConnector();

    CLinkConnector(const CLinkConnector&) = delete;
    CLinkConnector& operator=(const CLinkConnector&) = delete;

    void SetHandlers(ConnectedFn onUp, DisconnectedFn onDown)
    {
        _onUp = std::move(onUp);
        _onDown = std::move(onDown);
    }

    void Start();
    void Stop();

    // ── 로직 스레드 ──
    void OnConnected(SessionId sessionId);
    void OnDisconnected(SessionId sessionId);
    void Tick(int64_t nowMs);   // 하트비트

    bool IsUp() const { return _sessionId != NO_SESSION; }
    SessionId Session() const { return _sessionId; }

    template <typename TPacket>
    bool Send(const TPacket& packet)
    {
        if (!IsUp())
            return false;
        return PacketSend::Send(_network, _monitor, _sessionId, packet);
    }

    bool SendBytes(const void* data, int size)
    {
        if (!IsUp())
            return false;
        return PacketSend::SendBytes(_network, _monitor, _sessionId, data, size);
    }

    // ── 지표 스레드 ──
    void WriteMetrics(std::ostringstream& ss) const;

    int64_t ReconnectCount() const { return _reconnects.load(std::memory_order_relaxed); }

private:
    void ConnectThread();

    CIOCPServer& _network;
    CMonitorManager& _monitor;
    Config _config;
    ConnectedFn _onUp;
    DisconnectedFn _onDown;

    SessionId _sessionId = NO_SESSION;   // 로직 스레드만 쓴다
    int64_t _lastHeartbeatMs = 0;

    std::atomic<bool> _linkUp{ false };        // 지표용
    std::atomic<bool> _wantConnect{ true };    // 재접속 스레드가 ConnectTo 를 부를 차례인가
    std::atomic<int64_t> _reconnects{ 0 };     // 끊긴 뒤 다시 붙은 횟수(첫 연결은 안 센다)
    bool _everConnected = false;               // 재접속 스레드만 쓴다

    std::atomic<bool> _stop{ false };
    std::mutex _mutex;
    std::condition_variable _cv;
    std::thread _thread;
};
