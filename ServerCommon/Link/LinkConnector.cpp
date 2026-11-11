#include "LinkConnector.h"

#include <chrono>

#include <Common/ErrorLog.h>
#include <Link/SessionTag.h>
#include <Monitoring/MetricsText.h>
#include <Protocol/ServerLinkProtocol.h>
#include "CoreAffinity.h"

CLinkConnector::CLinkConnector(CIOCPServer& network, CMonitorManager& monitor, Config config)
    : _network(network), _monitor(monitor), _config(std::move(config))
{
}

CLinkConnector::~CLinkConnector()
{
    Stop();
}

void CLinkConnector::Start()
{
    _stop = false;
    _thread = std::thread(&CLinkConnector::ConnectThread, this);
}

void CLinkConnector::Stop()
{
    {
        std::lock_guard<std::mutex> lk(_mutex);
        _stop = true;
    }
    _cv.notify_all();
    if (_thread.joinable())
        _thread.join();
}

void CLinkConnector::ConnectThread()
{
    CoreAffinity::PinIoThread();   // 재접속 스레드 → 게임코어 밖으로 (격리 off면 no-op)

    while (!_stop)
    {
        if (_wantConnect)
        {
            // 끊김 이벤트를 받은 로직 스레드가 다시 세운다 — 그래서 ConnectTo 를 부르기 "전"에 내린다.
            //   ConnectTo 가 돌아오기 전에 그 연결이 끊겨 로직 스레드가 처리를 끝낼 수 있다. 돌아온 뒤에 내리면
            //   그 값을 덮어써 아래 wait 에서 영영 못 깬다(세우는 곳이 OnDisconnected 하나뿐이다).
            _wantConnect = false;
            const int64_t session = _network.ConnectTo(_config.ip.c_str(), _config.port, static_cast<int>(_config.tag));
            if (session != 0)
            {
                if (_everConnected)
                    _reconnects.fetch_add(1, std::memory_order_relaxed);
                _everConnected = true;
                SLOG_INFO("[Link] {} 연결 {}:{}", _config.peerName, _config.ip, _config.port);
                continue;
            }
            _wantConnect = true;   // 실패 — 세션이 없으니 끊김 이벤트도 안 온다. 아래에서 RetrySec 뒤 다시
        }

        // 연결이 있으면 끊길 때까지, 없으면 RetrySec 뒤 다시 — 어느 쪽이든 깨우는 쪽이 cv 로 알린다
        std::unique_lock<std::mutex> lk(_mutex);
        if (_wantConnect)
            _cv.wait_for(lk, std::chrono::seconds(_config.retrySec), [this] { return _stop.load(); });
        else
            _cv.wait(lk, [this] { return _stop.load() || _wantConnect.load(); });
    }
}

void CLinkConnector::OnConnected(SessionId sessionId)
{
    _sessionId = sessionId;
    _linkUp = true;
    _lastHeartbeatMs = 0;   // 다음 Tick 에 바로 보낸다 — 상대의 시한을 연결 직후부터 잰다
    if (_onUp)
        _onUp(sessionId);
}

void CLinkConnector::OnDisconnected(SessionId sessionId)
{
    if (sessionId != _sessionId)
        return;   // 이미 새 연결로 바뀐 뒤 늦게 온 옛 연결의 끊김

    _sessionId = NO_SESSION;
    _linkUp = false;
    SLOG_WARN("[Link] {} 끊김 — {}초마다 다시 접속", _config.peerName, _config.retrySec);
    if (_onDown)
        _onDown();

    {
        std::lock_guard<std::mutex> lk(_mutex);
        _wantConnect = true;
    }
    _cv.notify_all();
}

void CLinkConnector::Tick(int64_t nowMs)
{
    if (!IsUp())
        return;
    if (nowMs - _lastHeartbeatMs < static_cast<int64_t>(_config.heartbeatSec) * 1000)
        return;
    _lastHeartbeatMs = nowMs;
    Send(LINK_HEARTBEAT{});
}

void CLinkConnector::WriteMetrics(std::ostringstream& ss) const
{
    WriteMetrics(ss, { this });
}

void CLinkConnector::WriteMetrics(std::ostringstream& ss, std::initializer_list<const CLinkConnector*> connectors)
{
    bool any = false;
    for (const CLinkConnector* connector : connectors)
        any = any || connector != nullptr;
    if (!any)
        return;

    MetricsText::WriteHead(ss, "mmo_link_up", "Server-to-server link state toward the upper server (1 = connected)", "gauge");
    for (const CLinkConnector* connector : connectors)
    {
        if (connector != nullptr)
            ss << "mmo_link_up{peer=\"" << connector->_config.peerName << "\"} " << (connector->_linkUp.load() ? 1 : 0) << "\n";
    }
    ss << "\n";

    MetricsText::WriteHead(ss, "mmo_link_reconnect_total", "Reconnects after the link dropped (first connect not counted)", "counter");
    for (const CLinkConnector* connector : connectors)
    {
        if (connector != nullptr)
            ss << "mmo_link_reconnect_total{peer=\"" << connector->_config.peerName << "\"} " << connector->_reconnects.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";
}
