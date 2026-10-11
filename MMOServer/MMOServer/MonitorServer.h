// ==========================================================================
// CMonitorServer — Prometheus 메트릭 HTTP 엔드포인트
//
// [책임]
//  - 별도 스레드에서 경량 HTTP 서버 구동
//  - GET /metrics → 코어(CMonitorManager)와 게임(CGameMonitor) 지표를
//    Prometheus 텍스트 형식으로 한 벌에 합쳐 노출
//  - IOCP 워커와 간섭 없이 읽기 전용으로 동작
//
// [사용법]
//  CMonitorServer monitorSvr(monitor, gameMonitor, 9090);
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
#include "GameMonitor.h"   // 게임 컨텐츠 지표 — 저장소는 둘이고, 노출만 여기서 한 벌로 합친다
#include <Common/ErrorLog.h>
#include "CoreAffinity.h"

class CMonitorServer
{
public:
    CMonitorServer(CMonitorManager& monitor, CGameMonitor& gameMonitor, int port = 9090)
        : _monitor(monitor), _gameMonitor(gameMonitor), _port(port) {}

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
        WriteCounter(ss, "mmo_packet_errors_total",
                     "Total packet errors", _monitor._packetErrors);
        WriteCounter(ss, "mmo_send_queue_overflow_total",
                     "Total send queue overflows", _monitor._sendQueueOverflow);
        WriteCounter(ss, "mmo_partial_send_total",
                     "Total partial sends (success but fewer bytes than requested)", _monitor._partialSend);
        WriteCounter(ss, "mmo_recv_buffer_overflow_total",
                     "Total recv buffer overflows", _monitor._recvBufferOverflow);
        WriteCounter(ss, "mmo_zone_change_total",
                     "Total zone changes", _gameMonitor._sector._zoneChangeCount);   // 게임 그릇(10-06 코어에서 옮김) — 이름은 그대로
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
                     _gameMonitor._sector._membershipSends);
        WriteCounter(ss, "mmo_membership_pair_fix_total",
                     "Same-tick mover-pair CREATE/DELETE compensations (missed by tick-end batch notify)",
                     _gameMonitor._sector._membershipPairFixes);
        WriteCounter(ss, "mmo_move_budget_rejects_total",
                     "C2S_MOVE_START coordinate accepts rejected by move budget (0 expected for honest clients)",
                     _gameMonitor._sector._moveBudgetRejects);

        // ── DB 저장 파이프라인 (USE_DB_WORKER) — dirty flag 비동기 저장. 그릇은 게임 쪽(CGameMonitor::_db, 10-06), 이름은 그대로 ──
        WriteCounter(ss, "mmo_db_saved_total",
                     "Total player rows saved (UPSERT ok)", _gameMonitor._db._savedJobs);
        WriteCounter(ss, "mmo_db_failed_total",
                     "Total DB save failures", _gameMonitor._db._failedJobs);
        WriteCounter(ss, "mmo_db_dropped_total",
                     "Total jobs dropped by backpressure (slot queue full)", _gameMonitor._db._droppedJobs);
        if (_gameMonitor._db._workerCount > 0)
        {
            ss << "# HELP mmo_db_queue_depth Jobs pulled in last drain, per DB worker (sustained growth = worker falling behind)\n";
            ss << "# TYPE mmo_db_queue_depth gauge\n";
            const int64_t dbCount = _gameMonitor._db._workerCount;
            for (int i = 0; i < dbCount && i < DBSaveCounters::MAX_WORKERS; ++i)
                ss << "mmo_db_queue_depth{dbworker=\"" << i << "\"} " << _gameMonitor._db._queueDepth[i] << "\n";
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

        // ── 게임 컨텐츠 지표 (저장소가 코어와 별개 — CGameMonitor) ──
        WriteCombatCounters(ss);
        WriteEnterCounters(ss);

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
           << (static_cast<double>(_gameMonitor._sector._membershipCostUs) / 1000000.0) << "\n";
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

    // 전투 지표 — _gameMonitor(게임 저장소)를 읽는 자리다. (아래 WriteEnterCounters 와 함께 둘뿐이다)
    //   USE_COMBAT=0 이어도 계수기는 남아 0으로 노출된다. 조건부로 감추지 않는 이유는
    //   USE_COMBAT 이 BuildConfig.h 에 있어 이 헤더가 그걸 안 읽으면 #if 가 조용히 0이 되고,
    //   그러면 "전투가 꺼진 것"과 "노출을 빠뜨린 것"이 화면에서 똑같아 보이기 때문이다.
    void WriteCombatCounters(std::ostringstream& ss)
    {
        WriteCounter(ss, "mmo_attacks_total",
                     "Attacks that passed cooldown/range checks and actually swung",
                     _gameMonitor._combat._attacksTotal);

        // 거부는 사유별로 가른다 — 한 통에 담으면 제일 흔한 하나가 나머지를 삼켜서
        //   "헛스윙이 많은 건지 쿨타임에 막힌 건지"를 못 읽는다. 라벨 방식은 mmo_send_flush_backlog 와 같다.
        ss << "# HELP mmo_attacks_rejected_total Attacks rejected, split by reason\n";
        ss << "# TYPE mmo_attacks_rejected_total counter\n";
        ss << "mmo_attacks_rejected_total{reason=\"cooldown\"} "    << _gameMonitor._combat._attacksRejectedCooldown   << "\n";
        ss << "mmo_attacks_rejected_total{reason=\"dead_self\"} "   << _gameMonitor._combat._attacksRejectedDeadSelf   << "\n";
        ss << "mmo_attacks_rejected_total{reason=\"no_target\"} "   << _gameMonitor._combat._attacksRejectedNoTarget   << "\n";
        ss << "mmo_attacks_rejected_total{reason=\"dead_target\"} " << _gameMonitor._combat._attacksRejectedDeadTarget << "\n\n";

        WriteCounter(ss, "mmo_deaths_total",
                     "Actors whose HP dropped to zero",
                     _gameMonitor._combat._deathsTotal);
        WriteCounter(ss, "mmo_respawns_total",
                     "Respawns after death (trails deaths by at most one)",
                     _gameMonitor._combat._respawnsTotal);
        WriteCounter(ss, "mmo_damage_floored_total",
                     "Hits raised to the minimum damage (attack - defense < 1), any attacker",
                     _gameMonitor._combat._damageFloored);
        WriteCounter(ss, "mmo_my_stats_sent_total",
                     "S2C_MY_STATS packets sent (only SendMyStats sends them; one per enter, level-up and equip change)",
                     _gameMonitor._combat._myStatsSent);

#if USE_MONSTER
        // 위 값들은 "플레이어만" 센다 — 몬스터를 섞으면 기존 A/B 실측의 기준선이 오염된다.
        //   공격은 때린 쪽, 죽음·부활은 당한 쪽 기준으로 갈린다(GameMonitor.h 참조).
        WriteCounter(ss, "mmo_monster_attacks_total",
                     "Attacks swung by monsters",
                     _gameMonitor._monster._attacksTotal);
        WriteCounter(ss, "mmo_monster_attacks_rejected_total",
                     "Monster attacks rejected (should stay near zero: AI checks cooldown/range first)",
                     _gameMonitor._monster._attacksRejected);
        WriteCounter(ss, "mmo_monster_deaths_total",
                     "Monsters whose HP dropped to zero",
                     _gameMonitor._monster._deathsTotal);
        WriteCounter(ss, "mmo_monster_think_total",
                     "Monster AI decisions actually run (zero means AI is not ticking)",
                     _gameMonitor._monster._thinkCalls);
        WriteCounter(ss, "mmo_monster_state_transitions_total",
                     "Monster AI state changes (one state swallowing others shows up as a stall here)",
                     _gameMonitor._monster._stateTransitions);
        WriteCounter(ss, "mmo_monster_spawned_total",
                     "Monsters spawned (first fill and refills; spawned - deaths = alive, except monsters freed with a cleaned-up channel)",
                     _gameMonitor._monster._spawnedTotal);
#endif

#if USE_EXP
        // 경험치·레벨. 둘을 나눠 세는 이유 — 횟수만으로는 "몇 마리 잡았나"만 보이고
        //   총량과 갈라 두면 나중에 종류별 보상이 생겨도 평균이 바로 나온다.
        WriteCounter(ss, "mmo_exp_granted_total",
                     "Kills that actually awarded exp (max-level kills award nothing and are not counted)",
                     _gameMonitor._exp._granted);
        WriteCounter(ss, "mmo_exp_granted_amount_total",
                     "Total exp awarded",
                     _gameMonitor._exp._grantedAmount);
        WriteCounter(ss, "mmo_exp_level_ups_total",
                     "Level-up events (one per call even if several levels are gained at once)",
                     _gameMonitor._exp._levelUps);
        WriteCounter(ss, "mmo_exp_level_clamped_total",
                     "Loaded characters whose level fell outside the table and was clamped (0 expected)",
                     _gameMonitor._exp._levelClamped);
#endif

#if USE_INVENTORY
        // 인벤토리. 거절 둘은 사유 표를 돌며 내보낸다 — 사유를 늘려도 여기서 빠지는 칸이 없게.
        const auto& inv = _gameMonitor._inventory;
        WriteCounter(ss, "mmo_inv_items_added_total",
                     "Item units put into bags (count of items, not slots)",
                     inv._itemsAdded);

        ss << "# HELP mmo_inv_add_rejected_total Item adds refused, split by reason (nothing was added)\n";
        ss << "# TYPE mmo_inv_add_rejected_total counter\n";
        for (int i = 1; i < static_cast<int>(InvAddResult::COUNT); ++i)   // [0] = OK — 거절이 아니다
        {
            ss << "mmo_inv_add_rejected_total{reason=\"" << InvAddResultName(static_cast<InvAddResult>(i))
               << "\"} " << inv._addRejected[i] << "\n";
        }
        ss << "\n";

        ss << "# HELP mmo_chat_command_rejected_total Chat commands ('/'-prefixed) refused, split by reason\n";
        ss << "# TYPE mmo_chat_command_rejected_total counter\n";
        for (int i = 0; i < static_cast<int>(ChatCommandReject::COUNT); ++i)
        {
            ss << "mmo_chat_command_rejected_total{reason=\""
               << ChatCommandRejectName(static_cast<ChatCommandReject>(i))
               << "\"} " << inv._commandRejected[i] << "\n";
        }
        ss << "\n";

        WriteCounter(ss, "mmo_inv_slot_updates_sent_total",
                     "S2C_INVENTORY_SLOT packets sent (only FlushInventory sends them)",
                     inv._slotUpdatesSent);
        WriteCounter(ss, "mmo_inv_snapshots_sent_total",
                     "S2C_INVENTORY packets sent (one per spawn, empty bags included)",
                     inv._snapshotsSent);
        WriteCounter(ss, "mmo_inv_items_used_total",
                     "Items used successfully (potions consumed)",
                     inv._itemsUsed);

        ss << "# HELP mmo_inv_use_rejected_total Item uses refused, split by reason (nothing was consumed)\n";
        ss << "# TYPE mmo_inv_use_rejected_total counter\n";
        for (int i = 0; i < static_cast<int>(UseItemReject::COUNT); ++i)
        {
            ss << "mmo_inv_use_rejected_total{reason=\"" << UseItemRejectName(static_cast<UseItemReject>(i))
               << "\"} " << inv._useRejected[i] << "\n";
        }
        ss << "\n";

#if USE_DB_WORKER
        // 가방 저장·불러오기. mmo_db_* 로 안 붙인다 — 그쪽은 위치 저장만 세는 기존 게이트다.
        //   등식(조용해진 뒤): saves_queued = saved + save_failed
        const auto& dbItem = _gameMonitor._dbItem;
        WriteCounter(ss, "mmo_inv_saves_queued_total",
                     "Bag slot saves enqueued to the DB worker (one per changed slot, FlushInventory)",
                     inv._itemSavesQueued);
        WriteCounter(ss, "mmo_inv_save_rejected_total",
                     "Bag slot saves refused at enqueue (item save ceiling hit; 0 expected - nonzero means the DB is stalled)",
                     inv._itemSaveRejected);
        WriteCounter(ss, "mmo_inv_saved_total",
                     "Bag slot saves written by the DB worker",
                     dbItem._saved);
        WriteCounter(ss, "mmo_inv_save_failed_total",
                     "Bag slot saves lost after one reconnect and retry (0 expected - that slot differs from the DB)",
                     dbItem._failed);
        WriteCounter(ss, "mmo_inv_save_retried_total",
                     "Bag slot saves retried after a reconnect (success or not)",
                     dbItem._retried);
        WriteCounter(ss, "mmo_inv_loaded_slots_total",
                     "Bag slots filled from the DB at enter",
                     inv._loadedSlots);

        ss << "# HELP mmo_inv_load_skipped_total Bag rows skipped at enter, split by reason (0 expected)\n";
        ss << "# TYPE mmo_inv_load_skipped_total counter\n";
        for (int i = 0; i < static_cast<int>(InvLoadSkip::COUNT); ++i)
        {
            ss << "mmo_inv_load_skipped_total{reason=\"" << InvLoadSkipName(static_cast<InvLoadSkip>(i))
               << "\"} " << inv._loadSkipped[i] << "\n";
        }
        ss << "\n";
#endif

#if USE_EQUIP
        // 장착 — 성공은 방향(op)마다, 거절은 사유마다 한 줄. 사유는 표를 돈다(늘어도 노출에서 안 빠진다).
        const auto& eq = _gameMonitor._equip;
        ss << "# HELP mmo_equip_total Equip / unequip requests that succeeded\n";
        ss << "# TYPE mmo_equip_total counter\n";
        ss << "mmo_equip_total{op=\"equip\"} "   << eq._equipped   << "\n";
        ss << "mmo_equip_total{op=\"unequip\"} " << eq._unequipped << "\n\n";
        ss << "# HELP mmo_equip_rejected_total Equip / unequip requests refused, split by reason\n";
        ss << "# TYPE mmo_equip_rejected_total counter\n";
        for (int i = 0; i < static_cast<int>(EquipReject::COUNT); ++i)
        {
            ss << "mmo_equip_rejected_total{reason=\"" << EquipRejectName(static_cast<EquipReject>(i))
               << "\"} " << eq._rejected[i] << "\n";
        }
        ss << "\n";
#endif
#endif

#if USE_SKILL
        // 스킬 — 시전·맞힌 수는 스킬 번호마다, 거절은 사유마다 한 줄. 다 표를 돈다(늘어도 노출에서 안 빠진다).
        //   시전 = 비용을 뗀 수, 거절 = 아무것도 안 뗀 수, 맞힌 수 = 피해를 준 대상 수. 죽인 수는 일반 공격과 같은 mmo_deaths_total 등으로 센다.
        const auto& sk = _gameMonitor._skill;
        ss << "# HELP mmo_skill_cast_total Skill casts that passed every check (cost paid), split by skill id\n";
        ss << "# TYPE mmo_skill_cast_total counter\n";
        for (int i = 1; i <= MAX_SKILL_ID; ++i)
            ss << "mmo_skill_cast_total{skill=\"" << i << "\"} " << sk._cast[i] << "\n";
        ss << "\n";
        ss << "# HELP mmo_skill_hits_total Targets hit by skills (one per target per cast - an area skill can hit several), split by skill id\n";
        ss << "# TYPE mmo_skill_hits_total counter\n";
        for (int i = 1; i <= MAX_SKILL_ID; ++i)
            ss << "mmo_skill_hits_total{skill=\"" << i << "\"} " << sk._hits[i] << "\n";
        ss << "\n";
        ss << "# HELP mmo_skill_rejected_total Skill casts refused (nothing paid), split by reason\n";
        ss << "# TYPE mmo_skill_rejected_total counter\n";
        for (int i = 0; i < static_cast<int>(SkillReject::COUNT); ++i)
        {
            ss << "mmo_skill_rejected_total{reason=\"" << SkillRejectName(static_cast<SkillReject>(i))
               << "\"} " << sk._rejected[i] << "\n";
        }
        ss << "\n";
        WriteCounter(ss, "mmo_actor_mp_sent_total",
                     "S2C_ACTOR_MP packets sent (only SendActorMp sends them, to the owner only; one per enter, zone change, level-up, mana cast and mana potion)",
                     sk._actorMpSent);
#endif

#if USE_DROP
        // 드랍. 등식(조용해진 뒤) = dropped = 빼는 몫의 합 + mmo_ground_items (GameMonitor.h)
        const auto& drop = _gameMonitor._drop;
        WriteCounter(ss, "mmo_drop_rolls_total",
                     "Drop-table rolls = monsters killed by players (counted even when nothing drops)",
                     drop._rolls);
        WriteCounter(ss, "mmo_ground_items_dropped_total",
                     "Ground items created by drops",
                     drop._dropped);
        ss << "# HELP mmo_ground_items_removed_total Ground items taken off the ground, split by reason\n";
        ss << "# TYPE mmo_ground_items_removed_total counter\n";
        for (int i = 0; i < static_cast<int>(GroundItemRemoval::COUNT); ++i)
        {
            ss << "mmo_ground_items_removed_total{reason=\"" << GroundItemRemovalName(static_cast<GroundItemRemoval>(i))
               << "\"} " << drop._removed[i] << "\n";
        }
        ss << "\n";
        ss << "# HELP mmo_pickup_rejected_total Pickups refused, split by reason (the item stays on the ground)\n";
        ss << "# TYPE mmo_pickup_rejected_total counter\n";
        for (int i = 0; i < static_cast<int>(PickupFail::COUNT); ++i)
        {
            ss << "mmo_pickup_rejected_total{reason=\"" << PickupFailName(static_cast<PickupFail>(i))
               << "\"} " << drop._pickupRejected[i] << "\n";
        }
        ss << "\n";
        WriteCounter(ss, "mmo_ground_item_late_mover_deletes_total",
                     "DELETEs sent to players who crossed a sector in the same tick as the removal (they would keep a ghost otherwise)",
                     drop._lateMoverDeletes);
        ss << "# HELP mmo_ground_items Ground items lying in all zones right now\n";
        ss << "# TYPE mmo_ground_items gauge\n";
        ss << "mmo_ground_items " << drop._groundItems << "\n\n";
#endif
    }

    // 입장(캐릭터 불러오기) 지표 — 전부 게임 루프가 센 값이다(GameMonitor.h 참조).
    //   mmo_db_* 와 이름을 섞지 않는다: 그쪽은 "저장"만 세는 기존 게이트라 뜻이 바뀌면 안 된다.
    //   등식: requested = found + new + failed + orphaned (조용해진 뒤). 지연 분포의 count 도 오른쪽과 같아야 한다.
    void WriteEnterCounters(std::ostringstream& ss)
    {
        using EC = CGameMonitor::EnterCounters;
        const EC& e = _gameMonitor._enter;

        WriteCounter(ss, "mmo_enter_requested_total",
                     "Character loads enqueued to the DB worker",
                     e._requested);
        WriteCounter(ss, "mmo_enter_rejected_total",
                     "Character loads refused at enqueue (load ceiling hit; 0 expected - nonzero means the DB is stalled)",
                     e._rejected);
        WriteCounter(ss, "mmo_enter_found_total",
                     "Load results: row existed",
                     e._found);
        WriteCounter(ss, "mmo_enter_new_total",
                     "Load results: no row (new character)",
                     e._newCharacter);
        WriteCounter(ss, "mmo_enter_failed_total",
                     "Load results: query failed (never treated as 'no row')",
                     e._failed);
        WriteCounter(ss, "mmo_enter_orphaned_total",
                     "Load results whose session was already gone",
                     e._orphaned);
        WriteCounter(ss, "mmo_enter_fallback_total",
                     "Found rows whose map or position was unusable - spawned at a random spot (subset of found)",
                     e._fallback);
        WriteCounter(ss, "mmo_enter_duplicate_total",
                     "Old sessions kicked because the same account entered again",
                     e._duplicate);

        // 게이지 — 조용해지면 0 이어야 한다(안 내려가면 대기표가 샌 것)
        ss << "# HELP mmo_enter_pending Sessions currently waiting for their character load\n";
        ss << "# TYPE mmo_enter_pending gauge\n";
        ss << "mmo_enter_pending " << e._pending << "\n\n";

        // 게이지 — 접속은 했는데 입장 요청을 아직 안 보낸 세션. 올라가서 안 내려오면 클라가 안 보내는 것이다
        //   (네트워크 계층의 유휴 타임아웃은 하트비트만 보내도 갱신돼서 이런 세션을 안 끊는다).
        ss << "# HELP mmo_enter_waiting Sessions connected but not yet sent C2S_ENTER_GAME\n";
        ss << "# TYPE mmo_enter_waiting gauge\n";
        ss << "mmo_enter_waiting " << e._waiting << "\n\n";

        // 게이지 — 조용해지면 mmo_session_count = mmo_players_ingame + mmo_enter_pending + mmo_enter_waiting
        ss << "# HELP mmo_players_ingame Players currently spawned in zones (sum of zone player counts, refreshed about every 0.5s)\n";
        ss << "# TYPE mmo_players_ingame gauge\n";
        ss << "mmo_players_ingame " << e._inGame << "\n\n";

        // 비누적 버킷 스냅샷 → 누적 변환 (mmo_tick_duration_seconds 와 같은 방식)
        int64_t cum[EC::LATENCY_BUCKET_COUNT];
        int64_t running = 0;
        for (int i = 0; i < EC::LATENCY_BUCKET_COUNT; ++i)
        {
            running += e._latencyBuckets[i];
            cum[i] = running;
        }
        const int64_t sumUs = e._latencySumUs;

        ss << "# HELP mmo_enter_latency_seconds Character load latency: enqueue to result drained by game loop\n";
        ss << "# TYPE mmo_enter_latency_seconds histogram\n";
        for (int i = 0; i < EC::LATENCY_BUCKET_COUNT - 1; ++i)
        {
            ss << "mmo_enter_latency_seconds_bucket{le=\""
               << EC::LATENCY_LE[i] << "\"} " << cum[i] << "\n";
        }
        ss << "mmo_enter_latency_seconds_bucket{le=\"+Inf\"} "
           << cum[EC::LATENCY_BUCKET_COUNT - 1] << "\n";

        ss << std::fixed << std::setprecision(6);
        ss << "mmo_enter_latency_seconds_sum "
           << (static_cast<double>(sumUs) / 1000000.0) << "\n";
        ss << std::defaultfloat;

        ss << "mmo_enter_latency_seconds_count " << cum[EC::LATENCY_BUCKET_COUNT - 1] << "\n\n";
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
    CMonitorManager& _monitor;       // 코어 지표 저장소 (ServerCore)
    CGameMonitor&    _gameMonitor;   // 게임 컨텐츠 지표 저장소 — 코어와 별개 객체
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
