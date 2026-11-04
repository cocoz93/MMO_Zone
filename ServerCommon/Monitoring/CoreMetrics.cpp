#include "CoreMetrics.h"

#include <CoreConfig.h>    // USE_RIO_TRANSPORT 분기 (include 순서 의존 제거 — 반드시 직접 include)

#include <chrono>
#include <cstdio>
#include <iomanip>

#include "MetricsText.h"

using MetricsText::WriteCounter;

void CCoreMetrics::Write(std::ostringstream& ss)
{
    WriteCounters(ss);
    WriteGauges(ss);
    WriteSendWorkerFlush(ss);
    WriteHandleLatencyHistogram(ss);

    // ── 워커 스레드 카운터 ──
    WriteWorkerCounters(ss);

    // ── 스레드별 CPU 점유율 (외부 관측: 게임루프 동결 사각지대 보강) ──
    WriteThreadCpu(ss);
}

void CCoreMetrics::WriteCounters(std::ostringstream& ss)
{
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
    WriteCounter(ss, "mmo_packet_errors_total",
                 "Total packet errors", _monitor._packetErrors);
    WriteCounter(ss, "mmo_send_queue_overflow_total",
                 "Total send queue overflows", _monitor._sendQueueOverflow);
    WriteCounter(ss, "mmo_partial_send_total",
                 "Total partial sends (success but fewer bytes than requested)", _monitor._partialSend);
    WriteCounter(ss, "mmo_recv_buffer_overflow_total",
                 "Total recv buffer overflows", _monitor._recvBufferOverflow);
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
}

void CCoreMetrics::WriteGauges(std::ostringstream& ss)
{
    // ── 게이지 ──
    ss << "# HELP mmo_session_count Current active sessions\n";
    ss << "# TYPE mmo_session_count gauge\n";
    ss << "mmo_session_count " << _monitor._sessionCount << "\n\n";

    // 서버 간 연결 수 — 접속자 수(위)에 안 넣고 따로 센다. 서버 간 연결을 안 쓰면 0 이다
    ss << "# HELP mmo_link_session_count Current server-to-server sessions (not counted in mmo_session_count)\n";
    ss << "# TYPE mmo_link_session_count gauge\n";
    ss << "mmo_link_session_count " << _monitor._taggedSessionCount << "\n\n";

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
}

void CCoreMetrics::WriteSendWorkerFlush(std::ostringstream& ss)
{
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

void CCoreMetrics::WriteHandleLatencyHistogram(std::ostringstream& ss)
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

void CCoreMetrics::WriteWorkerCounters(std::ostringstream& ss)
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

void CCoreMetrics::SampleThreadCpu(std::ostringstream& ss, std::ostringstream& ssKernel, const char* label,
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

void CCoreMetrics::WriteThreadCpu(std::ostringstream& ss)
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
