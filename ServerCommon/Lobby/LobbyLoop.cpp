#include "LobbyLoop.h"

#include <chrono>

#include <Common/ErrorLog.h>
#include <Db/DbPipeline.h>
#include "CoreAffinity.h"
#include "Platform/Platform.h"

CLobbyLoop::CLobbyLoop(CIOCPServer& network, CMonitorManager& monitor, int periodMs)
    : _network(network), _monitor(monitor), _periodMs(periodMs)
{
}

CLobbyLoop::~CLobbyLoop()
{
    Stop();
}

int64_t CLobbyLoop::NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void CLobbyLoop::SetRoute(SessionTag tag, const CPacketTable* table, SessionFn onConnected, SessionFn onDisconnected)
{
    Route& route = _routes[static_cast<int>(tag)];
    route.table = table;
    route.onConnected = std::move(onConnected);
    route.onDisconnected = std::move(onDisconnected);
}

void CLobbyLoop::Start()
{
    _running = true;
    _thread = std::thread(&CLobbyLoop::Run, this);
}

void CLobbyLoop::Stop()
{
    _running = false;
    if (_thread.joinable())
        _thread.join();
}

const CLobbyLoop::Route* CLobbyLoop::FindRoute(int tag) const
{
    if (tag < 0 || tag >= SESSION_TAG_COUNT)
        return nullptr;
    return &_routes[tag];
}

void CLobbyLoop::OnReceived(const Route* route, SessionId sessionId, CSerialBuffer* msg)
{
    if (route == nullptr || route->table == nullptr)
    {
        _monitor._packetErrors.Inc();
        return;
    }

    const CPacketTable::Dispatch result = route->table->Route(sessionId, msg);
    switch (result)
    {
    case CPacketTable::Dispatch::Handled:
        break;
    case CPacketTable::Dispatch::UnknownType:
    case CPacketTable::Dispatch::TooSmall:
    case CPacketTable::Dispatch::NoHeader:
        _monitor._packetErrors.Inc();
        break;
    }
}

void CLobbyLoop::ProcessEvent(NetworkEvent& event)
{
    const Route* route = FindRoute(event.tag);
    const SessionId sessionId{ event.sessionId };

    switch (event.type)
    {
    case NetworkEvent::Type::CONNECTED:
        if (route != nullptr && route->onConnected)
            route->onConnected(sessionId);
        break;

    case NetworkEvent::Type::DISCONNECTED:
        if (route != nullptr && route->onDisconnected)
            route->onDisconnected(sessionId);
        break;

    case NetworkEvent::Type::RECEIVED:
        OnReceived(route, sessionId, event.pMsg);
        // handle-latency: enqueue(recv) → 처리완료(응답 송신 포함) 시간. 존의 ProcessNetworkEvents 와 같은 자리에서 잰다.
        if (event.enqueueTimeNs != 0)
        {
            const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            _monitor._gameLoop.RecordHandleLatencyLocal(static_cast<double>(nowNs - event.enqueueTimeNs) / 1.0e6);
        }
        event.pMsg->SubRef();   // 처리 함수는 반납하지 않는다 — 여기 한 곳이다
        break;
    }
}

void CLobbyLoop::Run()
{
    CoreAffinity::PinGameThread();   // 로직 스레드 코어 고정 (격리 off면 no-op)
    _monitor._gameLoopThreadHandle = Platform::CaptureCurrentThreadCpu();

    while (_running)
    {
        _monitor._gameLoop._eventQueueSize.Store(static_cast<int64_t>(_network.GetEventQueueSize()));

        _network.SwapOutNetworkEvents(_localEvents);
        while (!_localEvents.empty())
        {
            ProcessEvent(_localEvents.front());
            _localEvents.pop();
        }

        // 네트워크 이벤트 "뒤" — 같은 틱에 그 세션의 끊김과 회신이 같이 오면 끊김을 먼저 처리해 회신이 주인 없는 것으로 버려지게 한다(존과 같은 규칙)
        if (_db != nullptr)
            _db->DrainReplies();

        if (_periodic)
            _periodic(NowMs());

        _monitor._gameLoop.FlushHandleLatency();

        std::this_thread::sleep_for(std::chrono::milliseconds(_periodMs));
    }

    // 끝나기 전에 남은 받은 패킷의 버퍼를 반납한다
    _network.SwapOutNetworkEvents(_localEvents);
    while (!_localEvents.empty())
    {
        NetworkEvent& event = _localEvents.front();
        if (event.type == NetworkEvent::Type::RECEIVED && event.pMsg != nullptr)
            event.pMsg->SubRef();
        _localEvents.pop();
    }
}
