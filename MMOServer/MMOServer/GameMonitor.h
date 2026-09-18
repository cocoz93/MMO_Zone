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
};
