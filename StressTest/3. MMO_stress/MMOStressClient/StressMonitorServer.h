#pragma once

#include <WinSock2.h>
#include <Windows.h>
#include <thread>
#include <string>
#include <sstream>
#include <iomanip>
#include <memory>
#include <atomic>
#include <cstdio>
#include <climits>

#include "../../../ServerCore/Base/ThirdParty/httplib.h"
#include "MMOStats.h"

class StressMonitorServer
{
public:
    explicit StressMonitorServer(const MMOStats& stats, int port = 9101)
        : _stats(stats), _port(port) {}

    ~StressMonitorServer() { Stop(); }

    StressMonitorServer(const StressMonitorServer&) = delete;
    StressMonitorServer& operator=(const StressMonitorServer&) = delete;

    bool Start()
    {
        _svr = std::make_unique<httplib::Server>();

        _svr->Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(BuildMetricsText(),
                            "text/plain; version=0.0.4; charset=utf-8");
        });

        _httpThread = std::thread([this]() { HttpThreadFunc(); });
        return true;
    }

    void Stop()
    {
        _stopFlag = true;
        if (_svr) _svr->stop();
        if (_httpThread.joinable()) _httpThread.join();
        _svr.reset();
    }

private:
    void HttpThreadFunc()
    {
        static const int RETRY_INTERVAL_SEC = 5;

        while (!_stopFlag)
        {
            wprintf(L"[StressMonitor] Listening on port %d\n", _port);
            bool ok = _svr->listen("0.0.0.0", _port);

            if (_stopFlag) break;

            if (!ok)
            {
                wprintf(L"[StressMonitor] listen failed on port %d. Retrying in %ds...\n",
                        _port, RETRY_INTERVAL_SEC);

                for (int i = 0; i < RETRY_INTERVAL_SEC * 10 && !_stopFlag; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        wprintf(L"[StressMonitor] Stopped\n");
    }

    // ══════════════════════════════════════════════════════════════
    // Prometheus exposition format 생성
    // ══════════════════════════════════════════════════════════════

    std::string BuildMetricsText()
    {
        std::ostringstream ss;

        // ── 게이지 ──
        ss << "# HELP mmo_dummy_connected_clients Current connected clients\n";
        ss << "# TYPE mmo_dummy_connected_clients gauge\n";
        ss << "mmo_dummy_connected_clients " << _stats.connectedCount.load(std::memory_order_relaxed) << "\n\n";

        ss << "# HELP mmo_dummy_ready_clients Clients that received CREATE_MY_PLAYER\n";
        ss << "# TYPE mmo_dummy_ready_clients gauge\n";
        ss << "mmo_dummy_ready_clients " << _stats.readyCount.load(std::memory_order_relaxed) << "\n\n";

        // ── 카운터 ──
        WriteCounter(ss, "mmo_dummy_send_packets_total",
                     "Total sent packets", _stats.sendPackets);
        WriteCounter(ss, "mmo_dummy_recv_packets_total",
                     "Total received packets", _stats.recvPackets);
        WriteCounter(ss, "mmo_dummy_send_bytes_total",
                     "Total sent bytes", _stats.sendBytes);
        WriteCounter(ss, "mmo_dummy_recv_bytes_total",
                     "Total received bytes", _stats.recvBytes);
        WriteCounter(ss, "mmo_dummy_connect_total",
                     "Total successful connections", _stats.connectTotal);
        WriteCounter(ss, "mmo_dummy_connect_fail_total",
                     "Total connection failures", _stats.connectFail);
        WriteCounter(ss, "mmo_dummy_disconnect_from_server_total",
                     "Total server-initiated disconnects", _stats.disconnectFromServer);
        WriteCounter(ss, "mmo_dummy_move_start_total",
                     "Total MoveStart sent", _stats.moveStartSent);
        WriteCounter(ss, "mmo_dummy_move_stop_total",
                     "Total MoveStop sent", _stats.moveStopSent);
        WriteCounter(ss, "mmo_dummy_heartbeat_total",
                     "Total Heartbeat sent", _stats.heartbeatSent);
        WriteCounter(ss, "mmo_dummy_chat_total",
                     "Total Chat sent", _stats.chatSent);
        WriteCounter(ss, "mmo_dummy_zone_change_total",
                     "Total ZoneChange sent", _stats.zoneChangeSent);
        WriteCounter(ss, "mmo_dummy_zone_change_fail_total",
                     "Total ZoneChange failures", _stats.zoneChangeFail);
        WriteCounter(ss, "mmo_dummy_send_buffer_full_total",
                     "Total send buffer full events", _stats.sendBufferFull);
        WriteCounter(ss, "mmo_dummy_recv_buffer_overflow_total",
                     "Total recv buffer overflow disconnects", _stats.recvBufferOverflow);
        WriteCounter(ss, "mmo_dummy_send_error_total",
                     "Total send error disconnects", _stats.sendError);
        WriteCounter(ss, "mmo_dummy_packet_parse_fail_total",
                     "Total packet parse failure disconnects", _stats.packetParseFail);

        // ── 수신 오버플로 진단 ──
        // 아래 3개를 recv_buffer_overflow_total로 나누면 오버플로 사건만의 평균이 된다.
        WriteCounter(ss, "mmo_dummy_ovf_gap_ms_sum",
                     "Sum of gap since previous recv, at overflow moments (ms)", _stats.ovfGapMsSum);
        WriteCounter(ss, "mmo_dummy_ovf_bytes_sum",
                     "Sum of bytes drained from kernel in that OnRecv, at overflow moments", _stats.ovfBytesSum);
        WriteCounter(ss, "mmo_dummy_ovf_recv_calls_sum",
                     "Sum of recv() call counts in that OnRecv, at overflow moments", _stats.ovfRecvCalls);

        // ── RTT 게이지 ──
        ss << "# HELP mmo_dummy_rtt_max_seconds Worst RTT observed\n";
        ss << "# TYPE mmo_dummy_rtt_max_seconds gauge\n";
        ss << std::fixed << std::setprecision(6);
        ss << "mmo_dummy_rtt_max_seconds "
           << (static_cast<double>(_stats.rttMaxMs.load(std::memory_order_relaxed)) / 1000.0) << "\n\n";

        {
            int64_t minVal = _stats.rttMinMs.load(std::memory_order_relaxed);
            ss << "# HELP mmo_dummy_rtt_min_seconds Best RTT observed\n";
            ss << "# TYPE mmo_dummy_rtt_min_seconds gauge\n";
            if (minVal < LLONG_MAX)
                ss << "mmo_dummy_rtt_min_seconds "
                   << (static_cast<double>(minVal) / 1000.0) << "\n\n";
            else
                ss << "mmo_dummy_rtt_min_seconds 0\n\n";
        }
        ss << std::defaultfloat;

        // ── RTT 히스토그램 ──
        WriteRttHistogram(ss);

        // ── 더미 루프 비용 (계측기 포화 진단) ──
        ss << "# HELP mmo_dummy_loop_max_seconds Worst NetworkLoop iteration work time\n";
        ss << "# TYPE mmo_dummy_loop_max_seconds gauge\n";
        ss << std::fixed << std::setprecision(6);
        ss << "mmo_dummy_loop_max_seconds "
           << (static_cast<double>(_stats.loopMaxMs.load(std::memory_order_relaxed)) / 1000.0) << "\n\n";
        ss << std::defaultfloat;

        WriteLoopHistogram(ss);

        // ── 수신 대조군 게이지 (정상 수신까지 전량 포함) ──
        ss << "# HELP mmo_dummy_recv_gap_max_seconds Worst gap between consecutive OnRecv calls\n";
        ss << "# TYPE mmo_dummy_recv_gap_max_seconds gauge\n";
        ss << std::fixed << std::setprecision(6);
        ss << "mmo_dummy_recv_gap_max_seconds "
           << (static_cast<double>(_stats.recvGapMaxMs.load(std::memory_order_relaxed)) / 1000.0) << "\n\n";
        ss << std::defaultfloat;

        ss << "# HELP mmo_dummy_recv_chunk_max_bytes Most bytes drained from kernel in one OnRecv\n";
        ss << "# TYPE mmo_dummy_recv_chunk_max_bytes gauge\n";
        ss << "mmo_dummy_recv_chunk_max_bytes "
           << _stats.recvChunkMaxBytes.load(std::memory_order_relaxed) << "\n\n";

        return ss.str();
    }

    static void WriteCounter(std::ostringstream& ss,
                              const char* name, const char* help,
                              const std::atomic<int64_t>& value)
    {
        ss << "# HELP " << name << " " << help << "\n";
        ss << "# TYPE " << name << " counter\n";
        ss << name << " " << value.load(std::memory_order_relaxed) << "\n\n";
    }

    void WriteRttHistogram(std::ostringstream& ss)
    {
        // 비누적 버킷 스냅샷
        int64_t raw[MMOStats::RTT_BUCKET_COUNT];
        for (int i = 0; i < MMOStats::RTT_BUCKET_COUNT; ++i)
            raw[i] = _stats.rttBuckets[i].load(std::memory_order_relaxed);

        // 누적 변환
        int64_t cum[MMOStats::RTT_BUCKET_COUNT];
        cum[0] = raw[0];
        for (int i = 1; i < MMOStats::RTT_BUCKET_COUNT; ++i)
            cum[i] = cum[i - 1] + raw[i];

        int64_t rttCount = _stats.rttSamples.load(std::memory_order_relaxed);
        int64_t rttSumMs = _stats.rttSumMs.load(std::memory_order_relaxed);

        // 버킷 경계 (밀리초 → 초)
        static const char* leBounds[] = {
            "0.001", "0.005", "0.01", "0.02", "0.05",
            "0.1", "0.2", "0.5", "1.0"
        };

        ss << "# HELP mmo_dummy_rtt_seconds Chat round-trip time\n";
        ss << "# TYPE mmo_dummy_rtt_seconds histogram\n";

        for (int i = 0; i < MMOStats::RTT_BUCKET_COUNT - 1; ++i)
        {
            ss << "mmo_dummy_rtt_seconds_bucket{le=\""
               << leBounds[i] << "\"} " << cum[i] << "\n";
        }
        ss << "mmo_dummy_rtt_seconds_bucket{le=\"+Inf\"} "
           << cum[MMOStats::RTT_BUCKET_COUNT - 1] << "\n";

        ss << std::fixed << std::setprecision(6);
        ss << "mmo_dummy_rtt_seconds_sum "
           << (static_cast<double>(rttSumMs) / 1000.0) << "\n";
        ss << std::defaultfloat;

        ss << "mmo_dummy_rtt_seconds_count " << rttCount << "\n\n";
    }

    void WriteLoopHistogram(std::ostringstream& ss)
    {
        // 더미 NetworkLoop 1바퀴 work 시간(Sleep 제외). 서버 mmo_tick_duration의 더미판.
        // 부하 따라 치솟으면 select 단일스레드 포화 → RTT 신뢰 불가. 비누적 → 누적 변환.
        int64_t raw[MMOStats::LOOP_BUCKET_COUNT];
        for (int i = 0; i < MMOStats::LOOP_BUCKET_COUNT; ++i)
            raw[i] = _stats.loopBuckets[i].load(std::memory_order_relaxed);

        int64_t cum[MMOStats::LOOP_BUCKET_COUNT];
        cum[0] = raw[0];
        for (int i = 1; i < MMOStats::LOOP_BUCKET_COUNT; ++i)
            cum[i] = cum[i - 1] + raw[i];

        int64_t loopCount = _stats.loopSamples.load(std::memory_order_relaxed);
        int64_t loopSumMs = _stats.loopSumMs.load(std::memory_order_relaxed);

        // 버킷 경계 (밀리초 → 초), LOOP_BUCKET_BOUNDS와 일치
        static const char* leBounds[] = {
            "0.001", "0.002", "0.005", "0.01", "0.02",
            "0.05", "0.1", "0.2", "0.5"
        };

        ss << "# HELP mmo_dummy_loop_duration_seconds NetworkLoop iteration work time (excl. sleep)\n";
        ss << "# TYPE mmo_dummy_loop_duration_seconds histogram\n";

        for (int i = 0; i < MMOStats::LOOP_BUCKET_COUNT - 1; ++i)
        {
            ss << "mmo_dummy_loop_duration_seconds_bucket{le=\""
               << leBounds[i] << "\"} " << cum[i] << "\n";
        }
        ss << "mmo_dummy_loop_duration_seconds_bucket{le=\"+Inf\"} "
           << cum[MMOStats::LOOP_BUCKET_COUNT - 1] << "\n";

        ss << std::fixed << std::setprecision(6);
        ss << "mmo_dummy_loop_duration_seconds_sum "
           << (static_cast<double>(loopSumMs) / 1000.0) << "\n";
        ss << std::defaultfloat;

        ss << "mmo_dummy_loop_duration_seconds_count " << loopCount << "\n\n";
    }

private:
    const MMOStats& _stats;
    int _port;
    std::atomic<bool> _stopFlag{false};
    std::unique_ptr<httplib::Server> _svr;
    std::thread _httpThread;
};
