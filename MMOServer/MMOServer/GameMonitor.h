// ==========================================================================
// CGameMonitor — 게임 컨텐츠 지표 저장소
//
// [책임]
//  - 게임 컨텐츠(전투 등)의 지표를 담는다. 담기만 하고 노출은 CMonitorServer가 한다
//
// [왜 코어와 나눴나]
//  CMonitorManager는 ServerCore 서브모듈에 있다. 게임 지표를 거기 넣으면
//  코어를 쓰는 다른 프로젝트까지 쓰지도 않는 필드를 함께 지게 된다.
//  그래서 그릇을 게임 쪽에 따로 두고 상속·포함 없이 독립시킨다 — 코어는 이 파일을 모른다.
//  (Counter만 코어에서 빌려 쓴다. 지표가 아니라 원자 카운터 타입이다)
//
// [소유권]
//  - main()이 값으로 소유, CGameServer/CMonitorServer가 레퍼런스로 참조
//    (CMonitorManager와 같은 방식)
// ==========================================================================
#pragma once

#include "BuildConfig.h"      // USE_MONSTER — 몬스터 카운터 묶음을 토글 안에 둔다
#include "MonitorManager.h"   // Counter — 원자 카운터 타입만 빌려 쓴다 (코어 지표는 참조하지 않음)

class CGameMonitor
{
public:
    CGameMonitor() = default;
    ~CGameMonitor() = default;

    CGameMonitor(const CGameMonitor&) = delete;
    CGameMonitor& operator=(const CGameMonitor&) = delete;

    // ══════════════════════════════════════════════════════════════
    // 전투 (USE_COMBAT)
    //
    // 갱신은 게임 루프 스레드 한 곳뿐이지만, 코어 카운터와 캐시 라인이 겹치지
    // 않도록 alignas(64)로 띄운다 (CMonitorManager의 덩어리 나눔과 같은 규칙)
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) CombatCounters
    {
        Counter _attacksTotal;              // 판정을 통과해 실제로 휘두른 공격

        // 거부는 사유별로 가른다 — 한 통에 담으면 제일 흔한 하나가 나머지를 삼켜서
        //   "헛스윙이 많은 건지 쿨타임에 막힌 건지"를 못 읽는다.
        Counter _attacksRejectedCooldown;   // 거부: 쿨타임이 안 찼다
        Counter _attacksRejectedDeadSelf;   // 거부: 공격자가 죽어 있다
        Counter _attacksRejectedNoTarget;   // 거부: 사거리·방향 안에 아무도 없다 (헛스윙)
        Counter _attacksRejectedDeadTarget; // 거부: 앞에 있는 것이 이미 시체다

        Counter _deathsTotal;               // HP가 0 이하로 떨어진 횟수
        Counter _respawnsTotal;             // 부활 횟수 (deaths와 같거나 1 적은 것이 정상)
    } _combat;

#if USE_MONSTER
    // ══════════════════════════════════════════════════════════════
    // 몬스터 (USE_MONSTER)
    //
    // ★ 위 _combat 과 나누는 이유 — 위 값들은 기존 A/B 실측의 기준선이다.
    //   몬스터를 같은 통에 섞으면 USE_MONSTER=0/1 사이에서 "같은 이름의 두 값이
    //   다른 것을 세는" 상태가 되어 비교가 성립하지 않는다.
    //
    // ★ 가르는 기준이 둘이다 — 공격은 때린 쪽, 죽음·부활은 당한 쪽으로 간다.
    //   죽음을 때린 쪽으로 가르면 "플레이어가 뱀을 죽인 것"이 플레이어 죽음으로 세어진다.
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) MonsterCounters
    {
        Counter _attacksTotal;      // 몬스터가 휘두른 공격
        Counter _attacksRejected;   // 몬스터 공격이 거부된 수 — 사유를 안 가른다.
                                    //   AI 가 쿨타임·사거리를 먼저 보고 시도하므로 거의 0 이어야 한다.
                                    //   0 이 아니게 되면 그 전제가 깨진 것이고, 그때 사유별로 가른다.
        Counter _deathsTotal;       // 몬스터가 죽은 횟수
        Counter _respawnsTotal;     // 몬스터가 되살아난 횟수

        Counter _stateTransitions;  // AI 상태 전이 총수 — 한 상태가 나머지를 삼키면 여기가 먼저 멈춘다
        Counter _thinkCalls;        // 판단을 실제로 돌린 횟수 (0 이면 AI 가 아예 안 도는 것)
    } _monster;
#endif

    // ══════════════════════════════════════════════════════════════
    // 입장 — 캐릭터 불러오기 (USE_DB_WORKER)
    //
    // ★ 전부 게임 루프 스레드가 센다. DB 워커는 결과에 코드만 실어 보내고, 세는 일은
    //   게임 루프가 결과를 꺼낼 때 한다 → 쓰는 스레드가 하나고, 코어(ServerCore)의
    //   mmo_db_saved_total·mmo_db_failed_total 은 계속 "저장"만 센다(같은 이름이 두 가지를
    //   세면 기존 게이트가 흐려진다).
    //
    // ★ 맞아야 하는 등식 — 조용해진 뒤 requested = found + newCharacter + failed + orphaned.
    //   왼쪽은 요청을 넣는 자리, 오른쪽은 결과를 꺼내는 자리에서 따로 센다. 그래서 워커가
    //   어느 갈래에서 결과를 빠뜨리면 등식이 영영 안 맞는다. 갈래마다 계수기를 둔 이유다 —
    //   한 통에 담으면 제일 흔한 하나(찾음)가 나머지를 삼킨다.
    //
    // USE_DB_WORKER=0 이어도 계수기는 남아 0 으로 노출된다(전투 계수기와 같은 이유 —
    //   "꺼진 것"과 "노출을 빠뜨린 것"이 화면에서 똑같아 보이지 않게).
    // ══════════════════════════════════════════════════════════════
    struct alignas(64) EnterCounters
    {
        Counter _requested;      // 조회를 워커 큐에 넣은 수
        Counter _rejected;       // 불러오기 상한에 걸려 큐에 못 넣은 수 (requested 에 안 들어간다)

        Counter _found;          // 결과: 행이 있었다
        Counter _newCharacter;   // 결과: 행이 없었다 (새 캐릭터 — 정상)
        Counter _failed;         // 결과: 조회 실패 ("없음"과 섞이면 안 되는 갈래)
        Counter _orphaned;       // 결과가 왔는데 주인(세션)이 이미 나가고 없다 — 코드는 안 본다

        // 요청 → 결과 수령 지연. 평균은 튄 값을 못 보므로 구간별 개수로 담는다(비누적, 노출 때 누적 변환).
        //   개수 합 = 꺼낸 결과 전부 = found + newCharacter + failed + orphaned (등식의 둘째 대조군).
        //   첫 경계가 40ms 가 아니라 50ms 인 이유: 결과는 "다음 틱"에 꺼내므로 정상값이 틱 주기(40ms)
        //   바로 위에 몰린다(실측 평균 40.5ms). 경계를 40 에 두면 같은 정상값이 두 칸으로 갈려 읽을 수 없다.
        //   첫 칸 = "다음 틱에 돌아왔다"로 읽는다.
        static constexpr int     LATENCY_BUCKET_COUNT = 5;   // 마지막 칸 = +Inf
        static constexpr int64_t LATENCY_BOUNDS_US[LATENCY_BUCKET_COUNT - 1] = { 50000, 100000, 500000, 2000000 };
        static constexpr const char* LATENCY_LE[LATENCY_BUCKET_COUNT - 1]    = { "0.05", "0.1", "0.5", "2" };
        Counter _latencyBuckets[LATENCY_BUCKET_COUNT];
        Counter _latencySumUs;

        void RecordLatencyUs(int64_t us)
        {
            if (us < 0)
                us = 0;
            int i = 0;
            while (i < LATENCY_BUCKET_COUNT - 1 && us > LATENCY_BOUNDS_US[i])
                ++i;
            _latencyBuckets[i].Inc();
            _latencySumUs.Add(us);
        }
    } _enter;
};
