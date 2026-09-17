// ==========================================================================
// CMonitorServer — Prometheus 메트릭 HTTP 엔드포인트
//
// [책임]
//  - 별도 스레드에서 경량 HTTP 서버 구동
//  - GET /metrics → CMonitorManager 지표를 Prometheus 텍스트 형식으로 노출
//  - IOCP 워커와 간섭 없이 읽기 전용으로 동작
//
// [사용법]
//  CMonitorServer monitorSvr(monitor, 9090);
//  monitorSvr.Start();   // HTTP 스레드 시작
//  monitorSvr.Stop();    // 종료 + join
// ==========================================================================
#pragma once

#include <CoreConfig.h>    // USE_RIO_TRANSPORT 분기 (include 순서 의존 제거 — 반드시 직접 include)

#ifdef _WIN32
#include <WinSock2.h>   // httplib(Windows 소켓) — winsock2를 windows.h보다 먼저
#include <Windows.h>
#endif
#include <thread>
#include <string>
#include <sstream>
#include <iomanip>
#include <memory>
#include <atomic>
#include <iostream>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <chrono>

#include "Platform/Platform.h"
#include "ThirdParty/httplib.h"
#include "MonitorManager.h"
#include <Common/ErrorLog.h>
#include "CoreAffinity.h"

class CMonitorServer
{
public:
    explicit CMonitorServer(CMonitorManager& monitor, int port = 9090)
        : _monitor(monitor), _port(port) {}

    ~CMonitorServer() { Stop(); }

    CMonitorServer(const CMonitorServer&) = delete;
    CMonitorServer& operator=(const CMonitorServer&) = delete;

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

private:
    void HttpThreadFunc()
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

    // ══════════════════════════════════════════════════════════════
    // Prometheus exposition format 생성
    // ══════════════════════════════════════════════════════════════

    std::string BuildMetricsText()
    {
        // /metrics 핸들러는 _httpThread가 아니라 httplib ThreadPool 워커(기본 ≥8개)에서 실행된다.
        //   스크레이프가 겹치면 아래 SampleThreadCpu가 CpuSample을 비원자 read-modify-write로
        //   갱신하면서 lastWall/lastCpu가 찢겨 cpu_ratio가 엉뚱한 값으로 노출된다.
        std::lock_guard<std::mutex> lk(_metricsMutex);

        std::ostringstream ss;

        // ── 카운터 ──
        WriteCounter(ss, "mmo_recv_packets_total",
                     "Total received packets", _monitor._recvPackets);
        WriteCounter(ss, "mmo_send_packets_total",
                     "Total sent packets", _monitor._sendPackets);
        WriteCounter(ss, "mmo_recv_bytes_total",
                     "Total received bytes", _monitor._recvBytes);
        WriteCounter(ss, "mmo_send_bytes_total",
                     "Total sent bytes", _monitor._sendBytes);
        WriteCounter(ss, "mmo_session_created_total",
                     "Total sessions created", _monitor._sessionCreated);
        WriteCounter(ss, "mmo_session_destroyed_total",
                     "Total sessions destroyed", _monitor._sessionDestroyed);
        WriteCounter(ss, "mmo_accept_failed_total",
                     "Total accept failures", _monitor._acceptFailed);
        WriteCounter(ss, "mmo_accept_rejected_queue_total",
                     "Total accepts rejected by game event queue overload", _monitor._acceptRejectedByQueue);
        WriteCounter(ss, "mmo_session_timed_out_total",
                     "Total session timeouts", _monitor._sessionTimedOut);
        WriteCounter(ss, "mmo_cheat_detected_total",
                     "Total cheat detections", _monitor._gameLoop._cheatDetected);
        WriteCounter(ss, "mmo_packet_errors_total",
                     "Total packet errors", _monitor._packetErrors);
        WriteCounter(ss, "mmo_send_queue_overflow_total",
                     "Total send queue overflows", _monitor._sendQueueOverflow);
        WriteCounter(ss, "mmo_partial_send_total",
                     "Total partial sends (success but fewer bytes than requested)", _monitor._partialSend);
        WriteCounter(ss, "mmo_recv_buffer_overflow_total",
                     "Total recv buffer overflows", _monitor._recvBufferOverflow);
        WriteCounter(ss, "mmo_zone_change_total",
                     "Total zone changes", _monitor._gameLoop._zoneChangeCount);
        WriteCounter(ss, "mmo_send_contention_total",
                     "Total PostSend contentions (backed off because another thread was submitting)", _monitor._sendContention);
        WriteCounter(ss, "mmo_recv_contention_total",
                     "Total recv-gate contentions (epoll only: another worker was already reading that session)",
                     _monitor._recvContention);
        WriteCounter(ss, "mmo_send_followup_total",
                     "Total follow-up submits after waiting for a completion (lower = depth absorbed more per round)",
                     _monitor._sendFollowUp);
        WriteCounter(ss, "mmo_send_wrap_splits_total",
                     "Total sends that carried only the straight run because the ring wrapped", _monitor._sendWrapSplits);
        WriteCounter(ss, "mmo_wsa_recv_calls_total",
                     "Total WSARecv system calls", _monitor._wsaRecvCalls);
        WriteCounter(ss, "mmo_wsa_send_calls_total",
                     "Total WSASend system calls", _monitor._wsaSendCalls);
        WriteCounter(ss, "mmo_wsa_send_completions_total",
                     "Total WSASend IOCP completions", _monitor._wsaSendCompletions);
        WriteCounter(ss, "mmo_send_enqueued_bytes_total",
                     "Total bytes enqueued to SendQ", _monitor._sendEnqueuedBytes);
        WriteCounter(ss, "mmo_send_discarded_bytes_total",
                     "Total bytes discarded from SendQ on disconnect", _monitor._sendDiscardedBytes);
        WriteCounter(ss, "mmo_broadcast_calls_total",
                     "Total broadcast invocations", _monitor._gameLoop._broadcastCalls);
        WriteCounter(ss, "mmo_broadcast_targets_total",
                     "Total broadcast target players", _monitor._gameLoop._broadcastTargets);
        WriteCounter(ss, "mmo_membership_sends_total",
                     "Uncounted membership-change copies (sector-change/enter/leave); compare vs broadcast_targets",
                     _monitor._gameLoop._membershipSends);
        WriteCounter(ss, "mmo_membership_pair_fix_total",
                     "Same-tick mover-pair CREATE/DELETE compensations (missed by tick-end batch notify)",
                     _monitor._gameLoop._membershipPairFixes);
        WriteCounter(ss, "mmo_move_budget_rejects_total",
                     "C2S_MOVE_START coordinate accepts rejected by move budget (0 expected for honest clients)",
                     _monitor._gameLoop._moveBudgetRejects);

        // ── DB 저장 파이프라인 (USE_DB_WORKER) — dirty flag 비동기 저장 ──
        WriteCounter(ss, "mmo_db_saved_total",
                     "Total player rows saved (UPSERT ok)", _monitor._dbSavedJobs);
        WriteCounter(ss, "mmo_db_failed_total",
                     "Total DB save failures", _monitor._dbFailedJobs);
        WriteCounter(ss, "mmo_db_dropped_total",
                     "Total jobs dropped by backpressure (slot queue full)", _monitor._dbDroppedJobs);
        if (_monitor._dbWorkerCount > 0)
        {
            ss << "# HELP mmo_db_queue_depth Jobs pulled in last drain, per DB worker (sustained growth = worker falling behind)\n";
            ss << "# TYPE mmo_db_queue_depth gauge\n";
            const int64_t dbCount = _monitor._dbWorkerCount;
            for (int i = 0; i < dbCount && i < CMonitorManager::MAX_DB_WORKERS; ++i)
                ss << "mmo_db_queue_depth{dbworker=\"" << i << "\"} " << _monitor._dbQueueDepth[i] << "\n";
            ss << "\n";
        }

        // ── 구간별 시간 (초 단위 counter) ──
        WritePhaseCounters(ss);

        // ── 게이지 ──
        ss << "# HELP mmo_session_count Current active sessions\n";
        ss << "# TYPE mmo_session_count gauge\n";
        ss << "mmo_session_count " << _monitor._sessionCount << "\n\n";

        // [transport A/B assert] active transport at build time — collect script cross-checks vs arm label (guards incremental-build mislabel)
        ss << "# HELP mmo_transport_rio Active transport at build time (1=RIO, 0=IOCP)\n";
        ss << "# TYPE mmo_transport_rio gauge\n";
        ss << "mmo_transport_rio " << (int)(USE_RIO_TRANSPORT) << "\n\n";

        // [완료 수거 A/B assert] 런타임 INI CompletionBatch의 실효값 — 수집 스크립트가 아암 라벨과
        //   대조해 "INI가 안 먹은 채로 돈 런"을 잡는다. 같은 바이너리로 팔을 바꾸므로 이 대조가 유일한 방어선.
        ss << "# HELP mmo_completion_batch Completion harvest mode (0=GQCS one-at-a-time, N=GQCSEx batch cap)\n";
        ss << "# TYPE mmo_completion_batch gauge\n";
        ss << "mmo_completion_batch " << _monitor._completionBatch << "\n\n";

        // [송신 깊이 A/B assert] 런타임 INI SendDepth의 실효값 — 위와 같은 용도.
        //   1=기존 1-pending. 2 이상이면 세션당 그 수만큼 동시 제출이 뜬다.
        ss << "# HELP mmo_send_depth Max in-flight sends per session (1=single-pending baseline)\n";
        ss << "# TYPE mmo_send_depth gauge\n";
        ss << "mmo_send_depth " << _monitor._sendDepth << "\n\n";

        ss << "# HELP mmo_event_queue_size Network event queue size before dispatch\n";
        ss << "# TYPE mmo_event_queue_size gauge\n";
        ss << "mmo_event_queue_size " << _monitor._gameLoop._eventQueueSize << "\n\n";

        // [USE_SEND_THREAD] 송신 핸드오프 백로그 — 워커별(토글 ON & 등록된 워커만). 어느 워커이 밀리는지 sendworker 라벨로.
        if (_monitor._sendWorkerCount > 0)
        {
            ss << "# HELP mmo_send_flush_backlog Sessions pulled per send-worker drain (sustained > 1-tick dirty count = that worker falling behind)\n";
            ss << "# TYPE mmo_send_flush_backlog gauge\n";
            const int64_t sendCount = _monitor._sendWorkerCount;
            for (int i = 0; i < sendCount && i < CMonitorManager::MAX_SEND_WORKERS; ++i)
            {
#if !USE_RIO_TRANSPORT
                // RIO: 워커 스레드 핸들은 workerCounters에만 등록(CPU 이중계상 방지) —
                // backlog는 카운트(_sendWorkerCount) 기반으로 노출하므로 핸들 가드를 건너뛴다.
                if (_monitor._sendCounters[i].threadHandle == Platform::kInvalidThreadCpuHandle)
                    continue;
#endif
                ss << "mmo_send_flush_backlog{sendworker=\"" << i << "\"} "
                   << _monitor._sendCounters[i].backlog << "\n";
            }
            ss << "\n";
        }

        // ── 히스토그램 (비누적 → 누적 변환) ──
        WriteTickHistogram(ss);
        WriteHandleLatencyHistogram(ss);

        // ── 워커 스레드 카운터 ──
        WriteWorkerCounters(ss);

        // ── 스레드별 CPU 점유율 (외부 관측: 게임루프 동결 사각지대 보강) ──
        WriteThreadCpu(ss);

        return ss.str();
    }

    static void WriteCounter(std::ostringstream& ss,
                              const char* name, const char* help,
                              int64_t value)
    {
        ss << "# HELP " << name << " " << help << "\n";
        ss << "# TYPE " << name << " counter\n";
        ss << name << " " << value << "\n\n";
    }

    void WritePhaseCounters(std::ostringstream& ss)
    {
        // 마이크로초 → 초 변환하여 counter로 노출
        // rate(mmo_tick_phase_seconds_total) / rate(mmo_tick_duration_seconds_count) → 틱당 평균 구간 시간
        ss << "# HELP mmo_tick_phase_seconds_total Cumulative time spent in each game loop phase\n";
        ss << "# TYPE mmo_tick_phase_seconds_total counter\n";

        ss << std::fixed << std::setprecision(6);
        ss << "mmo_tick_phase_seconds_total{phase=\"network_dispatch\"} "
           << (static_cast<double>(_monitor._gameLoop._phaseNetworkUs) / 1000000.0) << "\n";
        ss << "mmo_tick_phase_seconds_total{phase=\"game_logic\"} "
           << (static_cast<double>(_monitor._gameLoop._phaseGameLogicUs) / 1000000.0) << "\n";
        ss << "mmo_tick_phase_seconds_total{phase=\"broadcast_sync\"} "
           << (static_cast<double>(_monitor._gameLoop._phaseBroadcastSyncUs) / 1000000.0) << "\n";
        ss << std::defaultfloat;
        ss << "\n";

        // 비용종류별 계측 (단계 경계와 무관한 축) — gather/enqueue(복사) vs flush_send(WSASend) 비교용.
        // 복사 vs 송신: rate(enqueue) vs rate(flush_send). USE_LOCKFREE_SENDQ A/B로 enqueue 변화 관찰.
        ss << "# HELP mmo_broadcast_cost_seconds_total Cumulative broadcast cost by type (1-stage: BroadcastAroundSector hot path)\n";
        ss << "# TYPE mmo_broadcast_cost_seconds_total counter\n";
        ss << std::fixed << std::setprecision(6);
        ss << "mmo_broadcast_cost_seconds_total{type=\"gather\"} "
           << (static_cast<double>(_monitor._gameLoop._broadcastGatherUs) / 1000000.0) << "\n";
        ss << "mmo_broadcast_cost_seconds_total{type=\"enqueue\"} "
           << (static_cast<double>(_monitor._gameLoop._broadcastEnqueueUs) / 1000000.0) << "\n";
        ss << "mmo_broadcast_cost_seconds_total{type=\"flush_send\"} "
           << (static_cast<double>(_monitor._gameLoop._flushSendUs) / 1000000.0) << "\n";
        ss << std::defaultfloat;
        ss << "\n";

        // 멤버십(섹터이동 CREATE/DELETE) 송신 시간 — broadcast_cost와 별도 메트릭(sum 합산 오염 방지).
        //   game_logic 페이즈에 섞여 있던 멤버십 복사를 분리 → "tick의 몇 ms가 멤버십이냐" 판정용.
        ss << "# HELP mmo_membership_cost_seconds_total Cumulative membership-change send time (ProcessSectorChange path; split from game_logic phase)\n";
        ss << "# TYPE mmo_membership_cost_seconds_total counter\n";
        ss << std::fixed << std::setprecision(6);
        ss << "mmo_membership_cost_seconds_total "
           << (static_cast<double>(_monitor._gameLoop._membershipCostUs) / 1000000.0) << "\n";
        ss << std::defaultfloat;
        ss << "\n";

        // [USE_SEND_THREAD] send 워커의 실제 WSASend 시간 — broadcast_cost와 "별도 메트릭"으로 노출.
        //   broadcast_cost 합산 쿼리(broadcast_total/share)에 섞이면 다른 스레드 비용이 게임루프 틱
        //   분해를 오염시키므로 분리한다. 토글 OFF면 0 (A/B에서 baseline=0으로 비교 가능).
        ss << "# HELP mmo_send_worker_flush_seconds_total Cumulative WSASend time per send-worker (flush_send offloaded here when USE_SEND_THREAD=1)\n";
        ss << "# TYPE mmo_send_worker_flush_seconds_total counter\n";
        ss << std::fixed << std::setprecision(6);
        {
            const int64_t sendCount = _monitor._sendWorkerCount;
            for (int i = 0; i < sendCount && i < CMonitorManager::MAX_SEND_WORKERS; ++i)
            {
                ss << "mmo_send_worker_flush_seconds_total{sendworker=\"" << i << "\"} "
                   << (static_cast<double>(_monitor._sendCounters[i].flushUs) / 1000000.0) << "\n";
            }
        }
        ss << std::defaultfloat;
        ss << "\n";
    }

    void WriteTickHistogram(std::ostringstream& ss)
    {
        // 비누적 버킷 스냅샷
        using GLC = CMonitorManager::GameLoopCounters;

        int64_t raw[GLC::TICK_BUCKET_COUNT];
        for (int i = 0; i < GLC::TICK_BUCKET_COUNT; ++i)
            raw[i] = _monitor._gameLoop._tickBuckets[i];

        // 누적 변환
        int64_t cum[GLC::TICK_BUCKET_COUNT];
        cum[0] = raw[0];
        for (int i = 1; i < GLC::TICK_BUCKET_COUNT; ++i)
            cum[i] = cum[i - 1] + raw[i];

        int64_t tickCount = _monitor._gameLoop._tickCount;
        int64_t tickSumUs = _monitor._gameLoop._tickSumUs;

        // 버킷 경계 (밀리초 → 초)
        static const char* leBounds[] = {
            "0.001", "0.005", "0.01", "0.02", "0.04",
            "0.06", "0.08", "0.1", "0.2"
        };

        ss << "# HELP mmo_tick_duration_seconds Game loop tick duration\n";
        ss << "# TYPE mmo_tick_duration_seconds histogram\n";

        for (int i = 0; i < GLC::TICK_BUCKET_COUNT - 1; ++i)
        {
            ss << "mmo_tick_duration_seconds_bucket{le=\""
               << leBounds[i] << "\"} " << cum[i] << "\n";
        }
        ss << "mmo_tick_duration_seconds_bucket{le=\"+Inf\"} "
           << cum[GLC::TICK_BUCKET_COUNT - 1] << "\n";

        ss << std::fixed << std::setprecision(6);
        ss << "mmo_tick_duration_seconds_sum "
           << (static_cast<double>(tickSumUs) / 1000000.0) << "\n";
        ss << std::defaultfloat;

        ss << "mmo_tick_duration_seconds_count " << tickCount << "\n\n";
    }

    void WriteHandleLatencyHistogram(std::ostringstream& ss)
    {
        // 서버 handle-latency: recv enqueue → 처리완료(응답 송신) 시간.
        // 클라 mmo_dummy_rtt(왕복)의 분해용 대조군. 비누적 버킷 스냅샷 → 누적 변환.
        using GLC = CMonitorManager::GameLoopCounters;

        int64_t raw[GLC::HANDLE_BUCKET_COUNT];
        for (int i = 0; i < GLC::HANDLE_BUCKET_COUNT; ++i)
            raw[i] = _monitor._gameLoop._handleBuckets[i];

        int64_t cum[GLC::HANDLE_BUCKET_COUNT];
        cum[0] = raw[0];
        for (int i = 1; i < GLC::HANDLE_BUCKET_COUNT; ++i)
            cum[i] = cum[i - 1] + raw[i];

        int64_t handleCount = _monitor._gameLoop._handleCount;
        int64_t handleSumUs = _monitor._gameLoop._handleSumUs;

        // 버킷 경계 (밀리초 → 초), HANDLE_BUCKET_BOUNDS와 일치
        static const char* leBounds[] = {
            "0.001", "0.005", "0.01", "0.02", "0.04",
            "0.06", "0.08", "0.1", "0.2"
        };

        ss << "# HELP mmo_handle_latency_seconds Server handle latency: recv enqueue to processed (response sent)\n";
        ss << "# TYPE mmo_handle_latency_seconds histogram\n";

        for (int i = 0; i < GLC::HANDLE_BUCKET_COUNT - 1; ++i)
        {
            ss << "mmo_handle_latency_seconds_bucket{le=\""
               << leBounds[i] << "\"} " << cum[i] << "\n";
        }
        ss << "mmo_handle_latency_seconds_bucket{le=\"+Inf\"} "
           << cum[GLC::HANDLE_BUCKET_COUNT - 1] << "\n";

        ss << std::fixed << std::setprecision(6);
        ss << "mmo_handle_latency_seconds_sum "
           << (static_cast<double>(handleSumUs) / 1000000.0) << "\n";
        ss << std::defaultfloat;

        ss << "mmo_handle_latency_seconds_count " << handleCount << "\n\n";
    }

    void WriteWorkerCounters(std::ostringstream& ss)
    {
        int64_t workerCount = _monitor._workerThreadCount;
        if (workerCount <= 0) return;

        ss << "# HELP mmo_worker_completions_total IOCP completions per worker\n";
        ss << "# TYPE mmo_worker_completions_total counter\n";

        for (int i = 0; i < workerCount && i < CMonitorManager::MAX_WORKER_THREADS; ++i)
        {
            ss << "mmo_worker_completions_total{worker=\"" << i << "\"} "
               << _monitor._workerCounters[i].completionCount << "\n";
        }
        ss << "\n";

        // 완료 수거 호출 횟수 — completions / dequeue_calls = 한 호출에 몇 건을 걷었나(배치 효율).
        //   GQCS는 1콜=1완료라 비율 1.0이 정상, GQCSEx는 1보다 커야 이득이 생긴다.
        //   [한계] 우리가 센 건 의도한 호출분이지 커널 진입 총량이 아니다 → 절대값 인용 금지, 양팔 Δ 전용.
        ss << "# HELP mmo_worker_dequeue_calls_total Completion-harvest calls per worker (GQCS/GQCSEx/RIODequeue)\n";
        ss << "# TYPE mmo_worker_dequeue_calls_total counter\n";

        for (int i = 0; i < workerCount && i < CMonitorManager::MAX_WORKER_THREADS; ++i)
        {
            ss << "mmo_worker_dequeue_calls_total{worker=\"" << i << "\"} "
               << _monitor._workerCounters[i].dequeueCalls << "\n";
        }
        ss << "\n";
    }

    // ══════════════════════════════════════════════════════════════
    // 스레드별 CPU 점유율 (gauge, 1.0 = 코어 1개 풀)
    //
    // HTTP 스레드(외부 관측자)가 각 스레드 핸들에 GetThreadTimes를 호출.
    // 두 스크레이프 사이의 ΔCPU시간 / Δ벽시계시간 = 그 구간 평균 점유율.
    // 게임루프가 드워커 루프에 갇혀도 외부에서 읽으니 동결되지 않음(진단정리 6 보강).
    // CPU 시간·벽시계 모두 ns 단위 → 무차원 비율. (측정은 Platform 뒤로 격리, 벽시계는 steady_clock)
    // ══════════════════════════════════════════════════════════════
    struct CpuSample
    {
        uint64_t lastCpuNs = 0;
        uint64_t lastKernelNs = 0;      // 직전 커널모드 CPU 누적 (syscall 실행분 격리용)
        uint64_t lastWallNs = 0;
        bool primed = false;
    };

    void SampleThreadCpu(std::ostringstream& ss, std::ostringstream& ssKernel, const char* label,
                         Platform::ThreadCpuHandle h, CpuSample& s, uint64_t wallNow)
    {
        if (h == Platform::kInvalidThreadCpuHandle) return;   // 아직 미등록(예: 에코 모드엔 게임루프 없음)

        uint64_t cpuNow = 0, kernelNow = 0;
        if (!Platform::GetThreadCpuTimeNs(h, cpuNow, kernelNow))
            return;

        double ratio = 0.0;
        double kernelRatio = 0.0;
        if (s.primed && wallNow > s.lastWallNs)
        {
            uint64_t dWall = wallNow - s.lastWallNs;
            ratio       = static_cast<double>(cpuNow - s.lastCpuNs)       / static_cast<double>(dWall);
            kernelRatio = static_cast<double>(kernelNow - s.lastKernelNs) / static_cast<double>(dWall);
        }
        s.lastCpuNs = cpuNow;
        s.lastKernelNs = kernelNow;
        s.lastWallNs = wallNow;
        s.primed = true;

        // 커널 라인은 ssKernel에 따로 모은다 — Prometheus 텍스트 포맷은 한 메트릭 패밀리의
        // 라인들이 연속이어야 하므로(교차 금지), cpu_ratio 블록과 kernel_ratio 블록을 분리해 뒤에 붙인다.
        ss       << "mmo_thread_cpu_ratio{thread=\""    << label << "\"} " << ratio       << "\n";
        ssKernel << "mmo_thread_kernel_ratio{thread=\"" << label << "\"} " << kernelRatio << "\n";
    }

    void WriteThreadCpu(std::ostringstream& ss)
    {
        // 벽시계는 monotonic steady_clock (ns) — CPU 시간과 동일 단위로 비율 계산
        uint64_t wallNow = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        std::ostringstream ssKernel;   // 커널 비율 라인은 여기 모아 cpu 블록 뒤에 append (패밀리 연속성 유지)

        ss << "# HELP mmo_thread_cpu_ratio Per-thread CPU utilization (1.0 = one full core)\n";
        ss << "# TYPE mmo_thread_cpu_ratio gauge\n";
        ssKernel << "# HELP mmo_thread_kernel_ratio Per-thread kernel-mode CPU (1.0 = one full core; syscall 실행분 격리)\n";
        ssKernel << "# TYPE mmo_thread_kernel_ratio gauge\n";
        ss << std::fixed << std::setprecision(4);
        ssKernel << std::fixed << std::setprecision(4);

        // 게임루프 (진단정리 4-🔴 capacity-bound 판정 핵심 지표)
        SampleThreadCpu(ss, ssKernel, "gameloop",
                        _monitor._gameLoopThreadHandle, _cpuGameLoop, wallNow);

        // IOCP 워커 ("게임루프만 타고 워커는 노나" 대조용). RIO 워커도 같은 라벨(worker-i)로 등록됨.
        int64_t workerCount = _monitor._workerThreadCount;
        for (int i = 0; i < workerCount && i < CMonitorManager::MAX_WORKER_THREADS; ++i)
        {
            char label[24];
            std::snprintf(label, sizeof(label), "worker-%d", i);
            SampleThreadCpu(ss, ssKernel, label,
                            _monitor._workerCounters[i].threadHandle, _cpuWorker[i], wallNow);
        }

        // [USE_SEND_THREAD] 전용 송신 워커 (비용 이전 판정 — 게임루프 flush가 여기로 샜는지, 워커별 분산 확인).
        //   토글 OFF면 _sendWorkerCount=0이라 루프가 돌지 않음(라인 생략). 워커 노출과 동일 패턴.
        //   RIO 빌드는 송신 워커가 통합워커로 흡수돼 이 루프가 비어(sendworker-* 라인 0) 있음.
        const int64_t sendCount = _monitor._sendWorkerCount;
        for (int i = 0; i < sendCount && i < CMonitorManager::MAX_SEND_WORKERS; ++i)
        {
            char label[24];
            std::snprintf(label, sizeof(label), "sendworker-%d", i);
            SampleThreadCpu(ss, ssKernel, label,
                            _monitor._sendCounters[i].threadHandle, _cpuSendWorker[i], wallNow);
        }

        ss << std::defaultfloat << "\n";
        ss << ssKernel.str() << "\n";   // 커널 블록을 cpu 블록 뒤에 이어붙임
    }

private:
    CMonitorManager& _monitor;
    int _port;
    std::atomic<bool> _stopFlag{false};
    std::unique_ptr<httplib::Server> _svr;
    std::thread _httpThread;

    // CPU 점유율 직전 샘플 상태.
    //   핸들러가 httplib ThreadPool 워커에서 도는 탓에 "HTTP 스레드 단독 접근"이 성립하지 않는다.
    //   → BuildMetricsText 전체를 _metricsMutex로 직렬화해 보호한다.
    std::mutex _metricsMutex;
    CpuSample _cpuGameLoop;
    CpuSample _cpuWorker[CMonitorManager::MAX_WORKER_THREADS];
    CpuSample _cpuSendWorker[CMonitorManager::MAX_SEND_WORKERS];   // [USE_SEND_THREAD] 송신 워커별 CPU 직전 샘플
};
