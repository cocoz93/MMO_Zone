#pragma once
// ==========================================================================
// CMetricsEndpoint — Prometheus 메트릭 HTTP 엔드포인트
//
// [책임]
//  - 별도 스레드에서 경량 HTTP 서버 구동
//  - GET /metrics → 부르는 쪽이 준 함수로 지표 글자를 받아 그대로 내보낸다
//  - IOCP 워커와 간섭 없이 읽기 전용으로 동작
//
// [사용법]
//  CMetricsEndpoint endpoint(9090, [&] { return BuildText(); });
//  endpoint.Start();   // HTTP 스레드 시작
//  endpoint.Stop();    // 종료 + join
// ==========================================================================

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httplib { class Server; }

class CMetricsEndpoint
{
public:
    using TextBuilder = std::function<std::string()>;

    CMetricsEndpoint(int port, TextBuilder buildText);
    ~CMetricsEndpoint();

    CMetricsEndpoint(const CMetricsEndpoint&) = delete;
    CMetricsEndpoint& operator=(const CMetricsEndpoint&) = delete;

    bool Start();
    void Stop();

private:
    void HttpThreadFunc();

    int _port;
    TextBuilder _buildText;
    std::atomic<bool> _stopFlag{ false };
    std::unique_ptr<httplib::Server> _svr;
    std::thread _httpThread;

    // /metrics 핸들러는 _httpThread가 아니라 httplib ThreadPool 워커(기본 ≥8개)에서 실행된다.
    //   스크레이프가 겹치면 CCoreMetrics::SampleThreadCpu가 CpuSample을 비원자 read-modify-write로
    //   갱신하면서 lastWall/lastCpu가 찢겨 cpu_ratio가 엉뚱한 값으로 노출된다.
    //   → 지표 글자 만들기 전체를 이 뮤텍스로 직렬화한다. 부르는 쪽 함수는 한 번에 하나만 돈다.
    std::mutex _metricsMutex;
};
