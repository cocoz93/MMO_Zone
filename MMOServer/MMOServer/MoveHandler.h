// ==========================================================================
// CMoveHandler — 이동: 이동 시작·멈춤 요청과 좌표 수용 예산 (게임 서버 나누기 11 단계 · C2 · 토글 없음)
//
// [책임]
//  - 받은 패킷 둘의 처리기와 그 등록(RegisterRoutes — 이동 시작 · 멈춤) — 컨텐츠 조각이 자기 줄을 등록한다
//  - 클라 예측 좌표 수용(IDLE → MOVING 때만 · 거리 한도 + 시간 비례 예산) · 벽 방향 차단 · 방향 전환
//  - 이동 예산 거부 건수를 틱 안에서 모아 틱 끝에 지표로 한 번(ReportTickMetrics)
//
// [경계]
//  좌표 이동·섹터 바뀜 판정은 존(CZone — 맵 경계 · 벽 · 섹터 대기열), 주변에 알리기는 시야 조각(이동 묶음 · 본인 되돌림).
//  매 틱 서버 주도 이동(speed × dt)은 존 틱이 한다 — 여기는 요청이 들어온 순간의 상태 바꾸기만.
//
// [모르는 것]
//  전투·보상·파티·가방·입장. 저장은 표시(_dbDirty)만 켜고, 언제 쓰는지는 캐릭터 저장소가 정한다.
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"     // USE_COMBAT · USE_DB_WORKER · FRAME_INTERVAL_MS — 안 읽으면 아래 #if 가 조용히 0 이 된다

class CPlayer;
class CSerialBuffer;
class CPacketRoutes;
class CSightBroadcaster;
class CMapManager;
class CGameMonitor;

class CMoveHandler
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 넷(시야 조각 · 맵 관리자 · 게임 지표 · 틱 번호)보다 뒤에 이것을 선언해야 한다
    //   (멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    CMoveHandler(CSightBroadcaster& sightBroadcaster, CMapManager& mapManager, CGameMonitor& gameMonitor, const uint64_t& frameCount)
        : _sightBroadcaster(sightBroadcaster), _mapManager(mapManager), _gameMonitor(gameMonitor), _frameCount(frameCount) {}

    // 받은 패킷 등록 — 이동 시작 · 멈춤. 게임 서버의 등록(RegisterRoutes)이 부른다. false = 등록 실패(기동 거절).
    bool RegisterRoutes(CPacketRoutes& routes);

    // 틱 끝) 이동 예산 거부 건수를 지표에 한 번 반영하고 0 으로 — 게임 루프가 시야 조각의 틱 끝 지표 옆에서 부른다.
    void ReportTickMetrics();

private:
    // 이동 시작 · 멈춤 요청 — 서버 권위 좌표(시작은 예산 안에서 클라 예측 좌표를 받아들일 수 있다 · 멈춤은 상태만 바꾼다)
    void RecvMoveStart(CPlayer* player, CSerialBuffer* pMsg);
    void RecvMoveStop(CPlayer* player, CSerialBuffer* pMsg);

    // C2S_MOVE_START 좌표 수용의 이동량 예산 검사 — 경과분 적립 후 통과하면 소비까지 수행.
    // 반환 false면 수용하지 않고 서버 좌표를 유지한다(기존 범위초과 경로와 동일한 처리).
    bool ConsumeMoveBudget(CPlayer* player, float distSq);

    static constexpr float MOVE_START_ACCEPT_DIST_SQ = 64.0f; // C2S_MOVE_START 좌표 수용 임계값 제곱 (8타일)

    // 좌표 수용 예산 — 위 임계값은 1회 한도일 뿐 빈도 제한이 없어, STOP/START를 연타하면
    //   매 전이마다 8타일씩 수용돼 사실상 무제한 순간이동이 됐다(수신 빈도 제한은 서버 어디에도 없음).
    //   상한은 위 임계값과 같은 8타일 → 정상 클라의 단발 보정은 지금과 동일하게 통과.
    //   적립은 자기 속도 × 여유계수로 시간 비례 → 연타해도 장기 평균은 적립 속도로 수렴한다.
    //   단 서버 주도 이동(매 틱 speed×dt)은 예산 검사 대상이 아니라 그 위에 얹히므로,
    //   치트 이동량 상한은 speed×(1+SLACK)이다. SLACK 1.25일 때 실측 2.09배 → 0.25로 조여 총합 1.25배.
    static constexpr float MOVE_BUDGET_CAP = 8.0f;     // 예산 상한 (타일) — MOVE_START_ACCEPT_DIST_SQ의 제곱근
    // 적립 여유계수 (네트워크 지터·프레임 흔들림 흡수). 낮출수록 조이지만 정상 클라 오탐 위험이 커진다:
    //   정상 소모 ≈ RTT/2 × speed(예측-서버 좌표 오차), 적립은 speed×SLACK/초.
    //   speed 30·RTT 200ms(오차 3타일/회) 기준 견디는 MOVE_START 빈도 = 초당 2.5회.
    //   루프백 실측은 RTT≈0이라 이 오탐을 못 잡는다 → mmo_move_budget_rejects_total 실네트워크 관측 필요.
    static constexpr float MOVE_BUDGET_SLACK = 0.25f;

    CSightBroadcaster& _sightBroadcaster;   // 이동 묶음(시작 · 멈춤 · 보정) · 본인 되돌림
    CMapManager&       _mapManager;         // 존 찾기
    CGameMonitor&      _gameMonitor;        // 이동 예산 거부 지표(틱 끝)
    const uint64_t&    _frameCount;         // 게임 루프의 틱 번호 — 빌린 것, 읽기만(예산 적립)

    // 비용종류별 계측 — 틱 내 누적 후 틱 끝 1회 모니터 반영 (단일 게임루프 스레드 전용 → 누적 자체엔 원자연산 불필요)
    int64_t _tickMoveBudgetRejects = 0;   // 이동 예산 초과로 거부한 좌표 수용 수 — 틱 끝 _moveBudgetRejects로 반영
    int64_t _tickMoveWallRejects   = 0;   // 서버 자리 → 보낸 자리 사이에 막힌 칸이 있어 거부한 좌표 수용 수 — 틱 끝 _moveWallRejects로 반영
};
