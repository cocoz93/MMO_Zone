#include "MetricsEndpoint.h"

#ifdef _WIN32
#include <WinSock2.h>   // httplib(Windows 소켓) — winsock2를 windows.h보다 먼저
#include <Windows.h>
#endif
#include <chrono>

#include "ThirdParty/httplib.h"
#include <Common/ErrorLog.h>
#include "CoreAffinity.h"

CMetricsEndpoint::CMetricsEndpoint(int port, TextBuilder buildText)
    : _port(port), _buildText(std::move(buildText))
{
}

CMetricsEndpoint::~CMetricsEndpoint()
{
    Stop();
}

bool CMetricsEndpoint::Start()
{
    _svr = std::make_unique<httplib::Server>();

    _svr->Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(_metricsMutex);
        res.set_content(_buildText(), "text/plain; version=0.0.4; charset=utf-8");
    });

    _httpThread = std::thread([this]() { HttpThreadFunc(); });
    return true;
}

void CMetricsEndpoint::Stop()
{
    _stopFlag = true;
    if (_svr)
    {
        // httplib의 stop()은 is_running_(=listen 진입 완료)일 때만 리슨 소켓을 닫는다.
        //   기동 직후라 아직 bind 중이면 stop()이 통째로 no-op이 되고, 그 직후 스레드가
        //   accept 루프로 들어가 아래 join()이 영구 대기한다.
        //   → running이 될 때까지 짧게 기다린다. 재시도 대기 중이라 영영 running이 안 되는
        //     경우도 있으므로(그때는 스레드가 _stopFlag를 보고 스스로 빠져나온다) 상한을 둔다.
        for (int i = 0; i < 100 && !_svr->is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        _svr->stop();
    }
    if (_httpThread.joinable()) _httpThread.join();
    _svr.reset();
}

void CMetricsEndpoint::HttpThreadFunc()
{
    CoreAffinity::PinIoThread();   // 모니터 HTTP 스레드 → 게임코어 밖으로 (격리 off면 no-op)

    static const int RETRY_INTERVAL_SEC = 5;

    while (!_stopFlag)
    {
        SLOG_INFO("[MonitorServer] Listening on port {}", _port);
        bool ok = _svr->listen("0.0.0.0", _port);

        if (_stopFlag) break;

        if (!ok)
        {
            SLOG_ERROR("[MonitorServer] listen failed on port {}. Retrying in {}s...", _port, RETRY_INTERVAL_SEC);

            // httplib은 bind 실패 시 is_decommisioned 래치를 세우고, 이후 모든 bind를 시도조차
            //   하지 않고 즉시 실패시킨다. 래치를 푸는 경로는 stop()뿐 — 이걸 부르지 않으면
            //   포트가 풀려도 이 재시도 루프가 영원히 무력해진다(로그만 5초마다 쌓임).
            _svr->stop();

            for (int i = 0; i < RETRY_INTERVAL_SEC * 10 && !_stopFlag; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    SLOG_INFO("[MonitorServer] Stopped");
}
