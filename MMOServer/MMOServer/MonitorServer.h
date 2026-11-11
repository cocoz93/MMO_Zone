// ==========================================================================
// CMonitorServer — 존 서버의 지표: 코어 지표에 게임 지표를 붙여 /metrics 로 낸다
//
// [책임]
//  - GET /metrics → 코어(CMonitorManager)와 게임(CGameMonitor) 지표를
//    Prometheus 텍스트 형식으로 한 벌에 합쳐 노출
//  - HTTP 스레드·재시도·직렬화는 CMetricsEndpoint, 코어 지표 글자는 CCoreMetrics 가 맡는다
//    (계정·캐릭터 서버도 같은 둘을 쓴다). 여기 남는 것은 게임 뜻이 있는 지표뿐이다
//
// [사용법]
//  CMonitorServer monitorSvr(monitor, gameMonitor, 9090);
//  monitorSvr.Start();   // HTTP 스레드 시작
//  monitorSvr.Stop();    // 종료 + join
// ==========================================================================
#pragma once

#include <string>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <functional>
#include <utility>

#include <Monitoring/CoreMetrics.h>
#include <Monitoring/MetricsEndpoint.h>
#include <Monitoring/MetricsText.h>
#include "MonitorManager.h"
#include "GameMonitor.h"   // 게임 컨텐츠 지표 — 저장소는 둘이고, 노출만 여기서 한 벌로 합친다

class CMonitorServer
{
public:
    CMonitorServer(CMonitorManager& monitor, CGameMonitor& gameMonitor, int port = 9090)
        : _monitor(monitor), _gameMonitor(gameMonitor), _coreMetrics(monitor),
          _endpoint(port, [this]() { return BuildMetricsText(); }) {}

    ~CMonitorServer() { Stop(); }

    CMonitorServer(const CMonitorServer&) = delete;
    CMonitorServer& operator=(const CMonitorServer&) = delete;

    // 서버 간 연결 지표 — Start 전에 건다. 안 걸면 줄이 없다
    void SetLinkMetrics(std::function<void(std::ostringstream&)> writer) { _linkMetrics = std::move(writer); }

    bool Start() { return _endpoint.Start(); }
    void Stop() { _endpoint.Stop(); }

private:
    static constexpr auto WriteCounter = &MetricsText::WriteCounter;   // 아래 호출 자리가 많아 짧은 이름을 남겨 둔다

    // ══════════════════════════════════════════════════════════════
    // Prometheus exposition format 생성
    // ══════════════════════════════════════════════════════════════

    std::string BuildMetricsText()
    {
        std::ostringstream ss;

        // ── 코어 지표 (네트워크·세션·처리 지연·워커·스레드 CPU) — 세 서버가 같은 이름으로 낸다 ──
        _coreMetrics.Write(ss);
        if (_linkMetrics)
            _linkMetrics(ss);

        // ── 게임 지표 ──
        WriteCounter(ss, "mmo_zone_change_total",
                     "Zone transfers of any cause (counted in TransferPlayer)", _gameMonitor._sector._zoneChangeCount);   // 게임 그릇(10-06 코어에서 옮김) — 이름은 그대로
        // 존 이동 요청 결과 — result="ok" 가 성공. 칸의 합 = 목적지를 고른 요청 수(GameMonitor.h) — 표를 돌아 늘어도 안 빠진다
        ss << "# HELP mmo_zone_change_requests_total C2S_ZONE_CHANGE requests by result (ok or the reject reason)\n";
        ss << "# TYPE mmo_zone_change_requests_total counter\n";
        for (int i = 0; i < ZONE_CHANGE_RESULT_COUNT; ++i)
            ss << "mmo_zone_change_requests_total{result=\"" << ZoneChangeResultName(static_cast<ZoneChangeReject>(i)) << "\"} "
               << _gameMonitor._sector._zoneChangeResults[i] << "\n";
        ss << "\n";
        // 포털 — 칸의 합(used + Σ rejected) = 받은 C2S_PORTAL_USE 수(GameMonitor.h)
        WriteCounter(ss, "mmo_portal_used_total",
                     "Portal uses that moved the player", _gameMonitor._portal._used);
        ss << "# HELP mmo_portal_rejected_total Portal uses rejected, split by reason\n";
        ss << "# TYPE mmo_portal_rejected_total counter\n";
        for (int i = 0; i < PORTAL_REJECT_COUNT; ++i)
            ss << "mmo_portal_rejected_total{reason=\"" << PortalRejectName(static_cast<PortalReject>(i)) << "\"} "
               << _gameMonitor._portal._rejected[i] << "\n";
        ss << "\n";
        ss << "# HELP mmo_channel_move_total Channel moves to another zone server, split by result\n";
        ss << "# TYPE mmo_channel_move_total counter\n";
        for (int i = 0; i < CHANNEL_MOVE_RESULT_COUNT; ++i)
            ss << "mmo_channel_move_total{result=\"" << ChannelMoveResultName(static_cast<ChannelMoveResult>(i)) << "\"} "
               << _gameMonitor._channelMove._result[i] << "\n";
        ss << "\n";
        // 직업 고르기 — 토글 없음. 결과 표를 돈다(늘린 결과가 자동으로 나온다)
        ss << "# HELP mmo_job_choose_total C2S_CHOOSE_JOB received, split by result (one job per character, once)\n";
        ss << "# TYPE mmo_job_choose_total counter\n";
        for (int i = 0; i < static_cast<int>(JobChoose::COUNT); ++i)
            ss << "mmo_job_choose_total{result=\"" << JobChooseName(static_cast<JobChoose>(i)) << "\"} " << _gameMonitor._job._choose[i] << "\n";
        ss << "\n";
        // 채팅 명령 — 토글 없음(명령 틀은 늘 있고 명령 줄만 토글을 본다 — GameMonitor.h _chat). 사유 표를 돈다(늘어도 안 빠진다)
        ss << "# HELP mmo_chat_command_rejected_total Chat commands ('/'-prefixed) refused, split by reason\n";
        ss << "# TYPE mmo_chat_command_rejected_total counter\n";
        for (int i = 0; i < static_cast<int>(ChatCommandReject::COUNT); ++i)
        {
            ss << "mmo_chat_command_rejected_total{reason=\""
               << ChatCommandRejectName(static_cast<ChatCommandReject>(i))
               << "\"} " << _gameMonitor._chat._commandRejected[i] << "\n";
        }
        ss << "\n";
        ss << "# HELP mmo_chat_gm_toggles_total GM mode switched by /gm, split by the state after the switch\n";
        ss << "# TYPE mmo_chat_gm_toggles_total counter\n";
        ss << "mmo_chat_gm_toggles_total{state=\"off\"} " << _gameMonitor._chat._gmToggled[0] << "\n";
        ss << "mmo_chat_gm_toggles_total{state=\"on\"} "  << _gameMonitor._chat._gmToggled[1] << "\n";
        ss << "\n";
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
        WriteCounter(ss, "mmo_move_wall_rejects_total",
                     "C2S_MOVE_START coordinate accepts rejected because the path crosses a blocked cell (0 expected for honest clients)",
                     _gameMonitor._sector._moveWallRejects);

        // ── DB 저장 파이프라인 (USE_DB_WORKER) — 저장 묶음(트랜잭션 하나). 그릇은 게임 쪽(CGameMonitor::_db · _save, 10-06), 옛 이름은 그대로 ──
        //   saved·failed 는 이제 묶음 수(캐릭터 행은 바뀐 묶음만 — 행 수는 save_char_rows) · dropped 는 살아 있는 캐릭터가 비행 중 묶음 하나라 구조상 0 에 묶인다
        //   — DB 가 못 따라가는 것은 mmo_db_save_inflight(접속자 수에 붙어 안 내려감) · mmo_db_save_pending_leave · queue_depth 로 본다(저장 플랜 C).
        WriteCounter(ss, "mmo_db_saved_total",
                     "Total save bundles committed (one per bundle; character rows are counted by mmo_db_save_char_rows_total)", _gameMonitor._db._savedJobs);
        WriteCounter(ss, "mmo_db_failed_total",
                     "Total DB save failures", _gameMonitor._db._failedJobs);
        WriteCounter(ss, "mmo_db_save_fenced_total",
                     "Save bundles rejected because the character's owner moved to another zone server", _gameMonitor._db._fencedJobs);
        WriteCounter(ss, "mmo_db_dropped_total",
                     "Total jobs dropped by backpressure (slot queue full)", _gameMonitor._db._droppedJobs);
        WriteCounter(ss, "mmo_db_save_retried_total",
                     "Save bundles retried whole after a reconnect (success or not)", _gameMonitor._db._saveRetried);
        WriteCounter(ss, "mmo_db_save_slot_rows_total",
                     "Bag slot rows written inside committed save bundles", _gameMonitor._db._saveSlotRows);
        WriteCounter(ss, "mmo_db_save_char_rows_total",
                     "Character rows written inside committed save bundles (bundles whose row was unchanged skip it)", _gameMonitor._db._saveCharRows);
        {
            // 저장 묶음 — 게임 스레드 몫(저장소). 등식(조용해진 뒤): sum(txn) = saved + failed + fenced = sum(reply)
            const auto& sv = _gameMonitor._save;
            ss << "# HELP mmo_db_save_txn_total Save bundles accepted by the DB queue, split by trigger (one per bundle)\n";
            ss << "# TYPE mmo_db_save_txn_total counter\n";
            for (int i = 0; i < static_cast<int>(SaveTrigger::COUNT); ++i)
                ss << "mmo_db_save_txn_total{trigger=\"" << SaveTriggerName(static_cast<SaveTrigger>(i)) << "\"} " << sv._txn[i] << "\n";
            ss << "\n";
            ss << "# HELP mmo_db_save_request_total Tick-end save requests listed, split by reason (duplicates in one tick not counted)\n";
            ss << "# TYPE mmo_db_save_request_total counter\n";
            for (int i = 0; i < static_cast<int>(SaveReason::COUNT); ++i)
                ss << "mmo_db_save_request_total{reason=\"" << SaveReasonName(static_cast<SaveReason>(i)) << "\"} " << sv._request[i] << "\n";
            ss << "\n";
            ss << "# HELP mmo_db_save_reply_total Save replies taken by the game thread, split by result\n";
            ss << "# TYPE mmo_db_save_reply_total counter\n";
            for (int i = 0; i < static_cast<int>(SaveReplyResult::COUNT); ++i)
                ss << "mmo_db_save_reply_total{result=\"" << SaveReplyResultName(static_cast<SaveReplyResult>(i)) << "\"} " << sv._reply[i] << "\n";
            ss << "\n";
            ss << "# HELP mmo_db_save_inflight Live characters with a save bundle in flight (stuck near player count = DB falling behind)\n";
            ss << "# TYPE mmo_db_save_inflight gauge\n";
            ss << "mmo_db_save_inflight " << sv._inflight << "\n\n";
            ss << "# HELP mmo_db_save_pending_leave Leave records not yet confirmed by the DB (0 when quiet)\n";
            ss << "# TYPE mmo_db_save_pending_leave gauge\n";
            ss << "mmo_db_save_pending_leave " << sv._pendingLeave << "\n\n";
            WriteCounter(ss, "mmo_db_save_retry_total",
                         "Saves sent again by the retry timer (revived bundles, leave records, trade pairs)", sv._retry);
            WriteCounter(ss, "mmo_db_save_lost_total",
                         "Save bundles still unconfirmed after the shutdown drain (0 expected)", sv._lost);
        }
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

        // ── 히스토그램 (비누적 → 누적 변환) ──
        WriteTickHistogram(ss);

        // ── 게임 컨텐츠 지표 (저장소가 코어와 별개 — CGameMonitor) ──
        WriteCombatCounters(ss);
        WriteEnterCounters(ss);

        return ss.str();
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

    // 전투 지표 — _gameMonitor(게임 저장소)를 읽는 자리다. (아래 WriteEnterCounters 와 함께 둘뿐이다)
    //   USE_COMBAT=0 이어도 카운터는 남아 0으로 노출된다. 조건부로 감추지 않는 이유는
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
        // 부활 자리 — 둘의 합 = mmo_respawns_total(GameMonitor.h)
        WriteCounter(ss, "mmo_respawn_in_place_total",
                     "Respawns where the player died (same zone)", _gameMonitor._combat._respawnInPlace);
        WriteCounter(ss, "mmo_respawn_to_town_total",
                     "Respawns moved to the town (another map)", _gameMonitor._combat._respawnToTown);
        WriteCounter(ss, "mmo_damage_floored_total",
                     "Hits raised to the minimum damage (attack - defense < 1), any attacker",
                     _gameMonitor._combat._damageFloored);
        WriteCounter(ss, "mmo_my_stats_sent_total",
                     "S2C_MY_STATS packets sent (only SendMyStats sends them; one per enter, then one per tick in which attack or defense changed - level-up, equip, GM, buff)",
                     _gameMonitor._combat._myStatsSent);

#if USE_COMBAT
        // 지속 효과(버프). 등식(조용해진 뒤) = Σ applied + Σ restored = Σ ended + mmo_buff_active (GameMonitor.h) — 다 표를 돈다(늘어도 노출에서 안 빠진다).
        const auto& bf = _gameMonitor._buff;
        ss << "# HELP mmo_buff_applied_total Buffs newly applied, split by buff id\n";
        ss << "# TYPE mmo_buff_applied_total counter\n";
        for (int i = 0; i < bf._buffRows; ++i)
            ss << "mmo_buff_applied_total{buff=\"" << bf._buffIdOf[i] << "\"} " << bf._applied[i] << "\n";
        ss << "\n";
        ss << "# HELP mmo_buff_refreshed_total Buffs re-applied while still active (time rewound), split by buff id\n";
        ss << "# TYPE mmo_buff_refreshed_total counter\n";
        for (int i = 0; i < bf._buffRows; ++i)
            ss << "mmo_buff_refreshed_total{buff=\"" << bf._buffIdOf[i] << "\"} " << bf._refreshed[i] << "\n";
        ss << "\n";
        ss << "# HELP mmo_buff_restored_total Buffs restored on enter from the saved DB value (characters.buffs), split by buff id\n";
        ss << "# TYPE mmo_buff_restored_total counter\n";
        for (int i = 0; i < bf._buffRows; ++i)
            ss << "mmo_buff_restored_total{buff=\"" << bf._buffIdOf[i] << "\"} " << bf._restored[i] << "\n";
        ss << "\n";
        ss << "# HELP mmo_buff_ended_total Buffs ended, split by reason (expired, death, leave)\n";
        ss << "# TYPE mmo_buff_ended_total counter\n";
        for (int i = 0; i < static_cast<int>(BuffEnd::COUNT); ++i)
            ss << "mmo_buff_ended_total{reason=\"" << BuffEndName(static_cast<BuffEnd>(i)) << "\"} " << bf._ended[i] << "\n";
        ss << "\n";
        ss << "# HELP mmo_buff_active Buffs active right now across every character\n";
        ss << "# TYPE mmo_buff_active gauge\n";
        ss << "mmo_buff_active " << bf._active << "\n\n";
        WriteCounter(ss, "mmo_actor_buff_sent_total",
                     "S2C_ACTOR_BUFF packets sent (only SendActorBuff sends them, to the owner only; one per apply, refresh, expiry, death and zone change)",
                     bf._actorBuffSent);
#endif

#if USE_PARTY
        // 파티. 등식 셋(조용해진 뒤 — GameMonitor.h): created = ended + active · member_added = Σ member_removed + members ·
        //   Σ requests = Σ requests_ended + pending_requests. 사유 칸은 다 표를 돈다(늘어도 노출에서 안 빠진다).
        {
            const auto& pt = _gameMonitor._party;
            ss << "# HELP mmo_party_active Parties alive right now\n# TYPE mmo_party_active gauge\nmmo_party_active " << pt._active << "\n";
            ss << "# HELP mmo_party_members Party members right now across every party\n# TYPE mmo_party_members gauge\nmmo_party_members " << pt._members << "\n";
            ss << "# HELP mmo_party_pending_requests Party requests waiting for an answer right now\n# TYPE mmo_party_pending_requests gauge\nmmo_party_pending_requests " << pt._pending << "\n\n";
            WriteCounter(ss, "mmo_party_created_total", "Parties created (an invite accepted by someone without a party)", pt._created);
            WriteCounter(ss, "mmo_party_ended_total", "Parties gone (disbanded by the leader or down to one member)", pt._ended);
            WriteCounter(ss, "mmo_party_member_added_total", "Members added, counting both founders of a new party", pt._memberAdded);
            ss << "# HELP mmo_party_member_removed_total Members removed, split by why\n# TYPE mmo_party_member_removed_total counter\n";
            for (int i = 0; i < static_cast<int>(PartyLeave::COUNT); ++i)
                ss << "mmo_party_member_removed_total{why=\"" << PartyLeaveName(static_cast<PartyLeave>(i)) << "\"} " << pt._memberRemoved[i] << "\n";
            ss << "\n# HELP mmo_party_requests_total Party requests put in the table, split by kind (a suggestion approved by the leader adds one invite)\n# TYPE mmo_party_requests_total counter\n";
            for (int i = 0; i < static_cast<int>(PartyAskKind::COUNT); ++i)
                ss << "mmo_party_requests_total{kind=\"" << PartyAskKindName(static_cast<PartyAskKind>(i)) << "\"} " << pt._requests[i] << "\n";
            ss << "\n# HELP mmo_party_requests_ended_total Party requests that left the table, split by why\n# TYPE mmo_party_requests_ended_total counter\n";
            for (int i = 0; i < static_cast<int>(PartyRequestEnd::COUNT); ++i)
                ss << "mmo_party_requests_ended_total{why=\"" << PartyRequestEndName(static_cast<PartyRequestEnd>(i)) << "\"} " << pt._requestsEnded[i] << "\n";
            ss << "\n# HELP mmo_party_notices_total S2C_PARTY_NOTICE sent, split by code (rejection reasons included)\n# TYPE mmo_party_notices_total counter\n";
            for (int i = 0; i < static_cast<int>(PartyNotice::COUNT); ++i)
                ss << "mmo_party_notices_total{code=\"" << PartyNoticeName(static_cast<PartyNotice>(i)) << "\"} " << pt._notices[i] << "\n";
            ss << "\n";
            WriteCounter(ss, "mmo_party_info_sent_total", "S2C_PARTY_INFO packets sent", pt._infoSent);
            WriteCounter(ss, "mmo_party_member_sent_total", "S2C_PARTY_MEMBER packets sent (a member's hp, level or map changed)", pt._memberSent);
            WriteCounter(ss, "mmo_party_ask_sent_total", "S2C_PARTY_ASK packets sent (should equal the sum of mmo_party_requests_total)", pt._askSent);
            WriteCounter(ss, "mmo_party_ask_end_sent_total", "S2C_PARTY_ASK_END packets sent (expired or cancelled requests)", pt._askEndSent);
            ss << "# HELP mmo_party_mate_skipped_total Attack candidates skipped because they are in the attacker's party, split by the candidate function (front = one target in reach, area = scan radius before the shape test)\n";
            ss << "# TYPE mmo_party_mate_skipped_total counter\n";
            ss << "mmo_party_mate_skipped_total{where=\"front\"} " << pt._mateSkippedFront << "\n";
            ss << "mmo_party_mate_skipped_total{where=\"area\"} " << pt._mateSkippedArea << "\n\n";
        }
        // 파티 전용 방. 등식 둘(GameMonitor.h): entries{entered} = closed + active · Σ entries(entered 뺀 칸) = portal_rejected{room}
        {
            const auto& rm = _gameMonitor._room;
            ss << "# HELP mmo_room_active Party rooms open right now (one channel of an instance map each)\n# TYPE mmo_room_active gauge\nmmo_room_active " << rm._active << "\n\n";
            ss << "# HELP mmo_room_entries_total Room portal uses by a party, split by result (entered = a room was made and the party moved in)\n# TYPE mmo_room_entries_total counter\n";
            for (int i = 0; i < static_cast<int>(RoomEntryReject::COUNT); ++i)
                ss << "mmo_room_entries_total{result=\"" << RoomEntryResultName(static_cast<RoomEntryReject>(i)) << "\"} " << rm._entries[i] << "\n";
            ss << "\n";
            WriteCounter(ss, "mmo_room_moved_total", "Players moved into a room (leader included)", rm._moved);
            WriteCounter(ss, "mmo_room_closed_total", "Rooms removed by the empty channel cleanup", rm._closed);
            WriteCounter(ss, "mmo_room_rewards_total", "Players rewarded in a room (everyone in the room when its boss dies)", rm._rewards);
            WriteCounter(ss, "mmo_room_reward_items_bag_total", "Room reward items put straight into bags", rm._rewardItemsBag);
            WriteCounter(ss, "mmo_room_reward_items_ground_total", "Room reward items dropped at the player's feet because the bag was full", rm._rewardItemsGround);
            WriteCounter(ss, "mmo_room_cleared_total", "Rooms cleared = room bosses killed (MonsterTable.csv boss)", rm._cleared);
            WriteCounter(ss, "mmo_room_minions_cleared_total", "Monsters removed from a room because its boss died (no death, no reward)", rm._minionsCleared);
        }
#endif

#if USE_TRADE
        // 거래. 등식 둘(GameMonitor.h): requests = opened + Σ requests_ended + pending_requests · opened = Σ ended + active
        {
            const auto& tr = _gameMonitor._trade;
            ss << "# HELP mmo_trade_active Trade windows open right now\n# TYPE mmo_trade_active gauge\nmmo_trade_active " << tr._active << "\n";
            ss << "# HELP mmo_trade_pending_requests Trade requests waiting for an answer right now\n# TYPE mmo_trade_pending_requests gauge\nmmo_trade_pending_requests " << tr._pending << "\n\n";
            WriteCounter(ss, "mmo_trade_requests_total", "Trade requests put in the table", tr._requests);
            WriteCounter(ss, "mmo_trade_opened_total", "Trade windows opened (accepted requests)", tr._opened);
            ss << "# HELP mmo_trade_requests_ended_total Trade requests that ended without opening a window, split by why\n# TYPE mmo_trade_requests_ended_total counter\n";
            for (int i = 0; i < static_cast<int>(TradeEnd::COUNT); ++i)
                ss << "mmo_trade_requests_ended_total{why=\"" << TradeEndName(static_cast<TradeEnd>(i)) << "\"} " << tr._requestsEnded[i] << "\n";
            ss << "\n# HELP mmo_trade_ended_total Trade windows that ended, split by why (done = swapped)\n# TYPE mmo_trade_ended_total counter\n";
            for (int i = 0; i < static_cast<int>(TradeEnd::COUNT); ++i)
                ss << "mmo_trade_ended_total{why=\"" << TradeEndName(static_cast<TradeEnd>(i)) << "\"} " << tr._ended[i] << "\n";
            ss << "\n# HELP mmo_trade_fail_total Trade requests refused, split by reason (one S2C_TRADE_FAIL each)\n# TYPE mmo_trade_fail_total counter\n";
            for (int i = 0; i < static_cast<int>(TradeFail::COUNT); ++i)
                ss << "mmo_trade_fail_total{reason=\"" << TradeFailName(static_cast<TradeFail>(i)) << "\"} " << tr._fails[i] << "\n";
            ss << "\n";
            WriteCounter(ss, "mmo_trade_items_moved_total", "Items moved by finished trades, both directions", tr._itemsMoved);
            WriteCounter(ss, "mmo_trade_money_moved_total", "Money moved by finished trades, both directions", tr._moneyMoved);
            WriteCounter(ss, "mmo_trade_save_waits_total", "Ticks a closed trade waited because a save of either player was still in flight", tr._saveWaits);
            WriteCounter(ss, "mmo_trade_ask_sent_total", "S2C_TRADE_ASK packets sent (should equal mmo_trade_requests_total)", tr._askSent);
            WriteCounter(ss, "mmo_trade_ask_end_sent_total", "S2C_TRADE_ASK_END packets sent", tr._askEndSent);
            WriteCounter(ss, "mmo_trade_state_sent_total", "S2C_TRADE_STATE packets sent", tr._stateSent);
            WriteCounter(ss, "mmo_trade_end_sent_total", "S2C_TRADE_END packets sent", tr._endSent);
        }
#endif

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
                     "Monsters spawned (first fill, refills and revives; spawned - deaths = alive, except monsters freed with a cleaned-up channel or a cleared room)",
                     _gameMonitor._monster._spawnedTotal);
        WriteCounter(ss, "mmo_monster_revived_total",
                     "Monsters spawned by a revive skill (also counted in mmo_monster_spawned_total)",
                     _gameMonitor._monster._revivedTotal);
#endif

        // NPC — 토글 없음. 존의 첫 틱마다 그 맵의 NPC 표 줄 수만큼(안 죽고 다시 안 깔린다 — 새 채널마다 그만큼 는다)
        WriteCounter(ss, "mmo_npc_spawned_total",
                     "NPCs placed (each zone places its map's NPC table rows on its first tick; never die, never respawn)",
                     _gameMonitor._npc._spawned);

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

        // 채팅 명령 거절 · /gm 켜고 끈 수는 토글 밖으로 갔다 — 포털 뒤 채팅 묶음(_chat · 게임 서버 나누기 14 단계)

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

        // 돈 — 금액(전) 합. 가방 토글 안 · DB 토글 밖(돈은 DB 를 끈 빌드에도 메모리에 있다). 길은 표를 돈다(늘어도 노출에서 안 빠진다)
        ss << "# HELP mmo_money_in_total Jeon that came into bags, split by source\n";
        ss << "# TYPE mmo_money_in_total counter\n";
        for (int i = 0; i < static_cast<int>(MoneySource::COUNT); ++i)
            ss << "mmo_money_in_total{source=\"" << MoneySourceName(static_cast<MoneySource>(i)) << "\"} " << inv._moneyIn[i] << "\n";
        ss << "\n";
        ss << "# HELP mmo_money_out_total Jeon that left bags, split by sink\n";
        ss << "# TYPE mmo_money_out_total counter\n";
        for (int i = 0; i < static_cast<int>(MoneySink::COUNT); ++i)
            ss << "mmo_money_out_total{sink=\"" << MoneySinkName(static_cast<MoneySink>(i)) << "\"} " << inv._moneyOut[i] << "\n";
        ss << "\n";

#if USE_DB_WORKER
        // 가방 불러오기. 가방 칸 저장은 저장 묶음 지표(mmo_db_save_* — 칸 줄 수 mmo_db_save_slot_rows_total)가 센다.
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

#if USE_SHOP
        // 상점 — 성공은 요청마다, 거절은 사유마다 한 줄. 사유는 표를 돈다(늘어도 노출에서 안 빠진다). 돈은 mmo_money_in/out_total
        const auto& sh = _gameMonitor._shop;
        WriteCounter(ss, "mmo_shop_bought_total", "Shop buy requests that succeeded (requests, not item units)", sh._bought);
        WriteCounter(ss, "mmo_shop_sold_total",   "Shop sell requests that succeeded (requests, not item units)", sh._sold);
        ss << "# HELP mmo_shop_rejected_total Shop buy / sell requests refused, split by reason (nothing changed)\n";
        ss << "# TYPE mmo_shop_rejected_total counter\n";
        for (int i = 0; i < static_cast<int>(ShopReject::COUNT); ++i)
        {
            ss << "mmo_shop_rejected_total{reason=\"" << ShopRejectName(static_cast<ShopReject>(i))
               << "\"} " << sh._rejected[i] << "\n";
        }
        ss << "\n";
#endif
#endif

#if USE_SKILL
        // 스킬 — 시전·맞힌 수는 스킬마다, 거절은 사유마다 한 줄. 다 표를 돈다(늘어도 노출에서 안 빠진다).
        //   스킬 줄은 표의 줄마다 하나(칸 = 안쪽 순번, 라벨 = 스킬 번호 — GameMonitor.h _skillIdOf). 표에 없는 번호는 줄이 없다.
        //   시전 = 비용을 뗀 수, 거절 = 아무것도 안 뗀 수, 맞힌 수 = 피해를 준 대상 수. 죽인 수는 일반 공격과 같은 mmo_deaths_total 등으로 센다.
        const auto& sk = _gameMonitor._skill;
        ss << "# HELP mmo_skill_cast_total Skill casts that passed every check (cost paid), split by skill id\n";
        ss << "# TYPE mmo_skill_cast_total counter\n";
        for (int i = 0; i < sk._skillRows; ++i)
            ss << "mmo_skill_cast_total{skill=\"" << sk._skillIdOf[i] << "\"} " << sk._cast[i] << "\n";
        ss << "\n";
        ss << "# HELP mmo_skill_hits_total Targets hit by damage skills (one per target per cast - an area skill can hit several; buffs count in mmo_buff_*), split by skill id\n";
        ss << "# TYPE mmo_skill_hits_total counter\n";
        for (int i = 0; i < sk._skillRows; ++i)
            ss << "mmo_skill_hits_total{skill=\"" << sk._skillIdOf[i] << "\"} " << sk._hits[i] << "\n";
        ss << "\n";
        ss << "# HELP mmo_skill_rejected_total Skill casts refused (nothing paid), split by reason\n";
        ss << "# TYPE mmo_skill_rejected_total counter\n";
        for (int i = 0; i < static_cast<int>(SkillReject::COUNT); ++i)
        {
            ss << "mmo_skill_rejected_total{reason=\"" << SkillRejectName(static_cast<SkillReject>(i))
               << "\"} " << sk._rejected[i] << "\n";
        }
        ss << "\n";
        // 위 셋의 몬스터(보스) 몫 — 사람 지표와 이름이 겹치지 않게 따로 낸다(합치면 sum(mmo_skill_cast_total) 에 보스가 섞인다)
        ss << "# HELP mmo_monster_skill_cast_total Skill casts by monsters that passed every check, split by skill id (players' casts are mmo_skill_cast_total)\n";
        ss << "# TYPE mmo_monster_skill_cast_total counter\n";
        for (int i = 0; i < sk._skillRows; ++i)
            ss << "mmo_monster_skill_cast_total{skill=\"" << sk._skillIdOf[i] << "\"} " << sk._monsterCast[i] << "\n";
        ss << "\n";
        ss << "# HELP mmo_monster_skill_hits_total Targets hit by monsters' damage skills, split by skill id\n";
        ss << "# TYPE mmo_monster_skill_hits_total counter\n";
        for (int i = 0; i < sk._skillRows; ++i)
            ss << "mmo_monster_skill_hits_total{skill=\"" << sk._skillIdOf[i] << "\"} " << sk._monsterHits[i] << "\n";
        ss << "\n";
        ss << "# HELP mmo_monster_skill_rejected_total Skill casts by monsters that were refused (nothing paid), split by reason\n";
        ss << "# TYPE mmo_monster_skill_rejected_total counter\n";
        for (int i = 0; i < static_cast<int>(SkillReject::COUNT); ++i)
        {
            ss << "mmo_monster_skill_rejected_total{reason=\"" << SkillRejectName(static_cast<SkillReject>(i))
               << "\"} " << sk._monsterRejected[i] << "\n";
        }
        ss << "\n";
        WriteCounter(ss, "mmo_actor_mp_sent_total",
                     "S2C_ACTOR_MP packets sent (only SendActorMp sends them, to the owner only; one per enter and zone change, then one per tick with a level-up, mana cast or mana potion)",
                     sk._actorMpSent);
        WriteCounter(ss, "mmo_skill_cooldown_remain_sent_total",
                     "S2C_SKILL_COOLDOWN_REMAIN packets sent (only SendActiveSkillCooldowns sends them, to the owner only; one per running cooldown on enter and zone change)",
                     sk._cooldownRemainSent);
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
                     "Found rows whose map or position was unusable - spawned at the map entry spot, start area or random (subset of found)",
                     e._fallback);
        WriteCounter(ss, "mmo_enter_dead_respawn_total",
                     "Found rows saved while dead (hp 0) - entered alive by the respawn rule, town start area if outside the town (subset of found)",
                     e._deadRespawn);
        WriteCounter(ss, "mmo_enter_duplicate_total",
                     "Old sessions kicked because the same account entered again",
                     e._duplicate);

        // 입장 번호 — 갈래마다 한 줄. 시작부터 0 으로 나간다(줄이 처음 생기는 순간의 증가는 rate() 가 못 본다).
        //   게이지 이름을 mmo_enter_pending 으로 못 쓴다 — 그 이름은 이미 「불러오는 중」이다(아래). 그래서 _code_ 를 넣었다.
        ss << "# HELP mmo_enter_code_total Enter codes presented at C2S_ENTER_GAME, split by result\n";
        ss << "# TYPE mmo_enter_code_total counter\n";
        for (int i = 0; i < static_cast<int>(EnterCodeResult::COUNT); ++i)
        {
            ss << "mmo_enter_code_total{result=\"" << EnterCodeResultName(static_cast<EnterCodeResult>(i))
               << "\"} " << e._codeResult[i] << "\n";
        }
        ss << "\n";
        ss << "# HELP mmo_enter_code_pending Enter codes noticed but not used yet (not expired)\n";
        ss << "# TYPE mmo_enter_code_pending gauge\n";
        ss << "mmo_enter_code_pending " << e._codePending << "\n\n";

        // 쿨타임·버프 되살리기 — 칸 단위(위 등식 밖). 버프를 되살린 수는 mmo_buff_restored_total(버프 등식 쪽)
        WriteCounter(ss, "mmo_enter_cooldown_restored_total",
                     "Skill cooldown slots restored on enter from characters.skill_cooldowns",
                     e._cooldownRestored);
        WriteCounter(ss, "mmo_enter_timer_expired_total",
                     "Saved cooldown or buff entries already over on enter (ran out while offline) - dropped",
                     e._timerExpired);
        WriteCounter(ss, "mmo_enter_timer_skipped_total",
                     "Saved cooldown or buff entries skipped for bad content (skill or buff id not in the table, skill not in the book, duplicate id)",
                     e._timerSkipped);
        WriteCounter(ss, "mmo_enter_timer_bad_total",
                     "Saved cooldown or buff columns dropped whole for a bad format (unknown version, length mismatch, longer than the buffer)",
                     e._timerBad);
        WriteCounter(ss, "mmo_enter_cooldown_old_format_total",
                     "Saved cooldown columns read in the old format (version 1, one-byte skill id) - kept and rewritten as version 2 on the next save",
                     e._cooldownOldFormat);
        WriteCounter(ss, "mmo_enter_buff_old_format_total",
                     "Saved buff columns read in the old format (version 1, kind + value) - kept and rewritten as version 2 on the next save",
                     e._buffOldFormat);

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

        // 게이지 — 존 수와 그중 쉬는 존(사람 0 — 걷기·몬스터 판단을 건너뜀). 같은 순회라 같은 주기다
        ss << "# HELP mmo_zones Zones (channels and rooms) that exist right now (refreshed with mmo_players_ingame)\n";
        ss << "# TYPE mmo_zones gauge\n";
        ss << "mmo_zones " << e._zones << "\n\n";
        ss << "# HELP mmo_zones_idle Zones with no player right now - their movement and monster thinking are skipped\n";
        ss << "# TYPE mmo_zones_idle gauge\n";
        ss << "mmo_zones_idle " << e._zonesIdle << "\n\n";

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

private:
    CMonitorManager& _monitor;       // 코어 지표 저장소 (ServerCore)
    CGameMonitor&    _gameMonitor;   // 게임 컨텐츠 지표 저장소 — 코어와 별개 객체
    CCoreMetrics     _coreMetrics;   // 코어 지표 글자 — CPU 직전 샘플도 여기 있다
    std::function<void(std::ostringstream&)> _linkMetrics;   // 서버 간 연결 줄 — 꺼져 있으면 비어 있다
    CMetricsEndpoint _endpoint;      // 맨 뒤에 둔다 — 먼저 내려가야 위 멤버를 쓰는 핸들러가 남지 않는다
};
