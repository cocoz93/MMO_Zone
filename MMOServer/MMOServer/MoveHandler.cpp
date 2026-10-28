#include "MoveHandler.h"

#include "SightBroadcaster.h"
#include "MapManager.h"
#include "Zone.h"
#include "Player.h"           // CPlayer · 이동 상태 · 방향 검사(IsMoveDirection — Actor.h)
#include "GameMonitor.h"
#include "PacketRoutes.h"
#include "SerialBuffer.h"
#include <Protocol/Protocol.h>
#include <cmath>              // std::sqrt — 예산 소비 거리

// 받은 패킷 등록 — 게임 서버 등록의 맨 앞 두 줄 그대로(게임 서버 나누기 0-④ 의 등록에서 이 클래스 몫 둘 · 11 단계).
bool CMoveHandler::RegisterRoutes(CPacketRoutes& routes)
{
    bool ok = true;
    ok &= routes.OnPlayer(MsgType::C2S_MOVE_START, sizeof(MSG_C2S_MOVE_START), [this](CPlayer* p, CSerialBuffer* m) { RecvMoveStart(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_MOVE_STOP,  sizeof(MSG_C2S_MOVE_STOP),  [this](CPlayer* p, CSerialBuffer* m) { RecvMoveStop(p, m); });
    return ok;
}

// 틱 끝 — 게임 루프에 있던 두 줄 그대로(게임 서버 나누기 11 단계)
void CMoveHandler::ReportTickMetrics()
{
    _gameMonitor._sector._moveBudgetRejects.Add(_tickMoveBudgetRejects);  // 이동 예산 거부 건수
    _tickMoveBudgetRejects = 0;
    _gameMonitor._sector._moveWallRejects.Add(_tickMoveWallRejects);      // 막힌 칸 너머 좌표 거부 건수
    _tickMoveWallRejects = 0;
}

void CMoveHandler::RecvMoveStart(CPlayer* player, CSerialBuffer* pMsg)
{
#if USE_COMBAT
    if (player->_health.IsDead())
        return;   // 시체는 안 움직인다 — 부활 전까지 조작을 막는다
#endif

    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    MSG_C2S_MOVE_START recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // Direction 범위 검증 — 유효 값은 Actor.h 한 곳(IsMoveDirection)
    if (!IsMoveDirection(recvMsg.direction))
        return;

    Direction dir = static_cast<Direction>(recvMsg.direction);

    // 이동 중 방향 전환: 같은 방향이면 무시, 다른 방향이면 갱신 + 재브로드캐스트
    // ※ 좌표 수용보다 먼저 검사 — MOVING 중 반복 전송으로 서버 좌표를 밀어내는 치트 방지
    if (player->_moveState == MoveState::MOVING)
    {
        if (dir == player->_direction)
            return;

        // 벽 방향 검증: 벽(맵 끝 · 막힌 칸) 앞에서 벽 쪽으로 방향 전환 시 정지 처리
        if (zone->StepBlocked(player->_x, player->_y, dir))
        {
            player->_moveState = MoveState::IDLE;
            player->_direction = dir;

#if USE_DB_WORKER
            player->_dbDirty = true;   // MOVING → IDLE 전이(주기저장 MOVING 조건에서 빠지므로 최종위치 마킹)
#endif

            _sightBroadcaster.NotifyMoveStop(zone, player, false);  // 본인 포함
            return;
        }

        player->_direction = dir;
        player->_lastSyncX = player->_x;
        player->_lastSyncY = player->_y;

        _sightBroadcaster.NotifyMoveStart(zone, player);
        return;
    }

    // 클라이언트 예측 좌표 수용 (IDLE → MOVING 전환 시에만):
    // 서버 좌표와의 오차가 허용 범위 내이고, 초당 이동량 예산이 남아 있으면 채택.
    //   거리 임계값만으로는 1회 한도밖에 못 막는다 — STOP/START를 연타하면 전이마다 다시 8타일이
    //   수용되므로, 시간에 비례해 적립되는 예산을 함께 봐야 자기 속도 이상으로 못 움직인다.
    {
        float dx = recvMsg.x - player->_x;
        float dy = recvMsg.y - player->_y;
        const float distSq = dx * dx + dy * dy;
        // 맵 경계 클램핑 후 수용 — 서버 자리에서 그 자리까지 막힌 칸을 지나면 안 받는다(집이 8타일보다 얇아 벽 건너편 좌표로 뚫고 나가지 못하게).
        //   정상 클라는 같은 격자로 미리 걸으므로 벽을 넘은 좌표를 보낼 일이 없다. 예산보다 먼저 본다 — 거절한 좌표로 예산을 깎지 않게
        float acceptX = recvMsg.x;
        float acceptY = recvMsg.y;
        zone->ClampToMap(acceptX, acceptY);
        bool through = false;
        if (distSq <= MOVE_START_ACCEPT_DIST_SQ)
        {
            through = zone->IsPathBlocked(player->_x, player->_y, acceptX, acceptY);
            if (through)
                ++_tickMoveWallRejects;
        }
        if (distSq <= MOVE_START_ACCEPT_DIST_SQ && !through && ConsumeMoveBudget(player, distSq))
        {
            player->_x = acceptX;
            player->_y = acceptY;

#if USE_DB_WORKER
            player->_dbDirty = true;   // 클라 예측 좌표 채택 → 위치 변경, 저장 대상
#endif

            // 좌표 수용으로 섹터가 변경되었을 수 있으므로 재계산
            zone->UpdateSectorAndQueue(player);
        }
        // 범위 초과 · 막힌 칸 너머 · 예산 부족 시 서버 좌표 유지 (치트 방지)
    }

    // 벽 방향 검증: 벽(맵 끝 · 막힌 칸) 앞에서 벽 쪽으로 이동 시도 시 차단
    if (zone->StepBlocked(player->_x, player->_y, dir))
    {
        _sightBroadcaster.SendSyncPosition(player);        // 본인 — 서버 권위 좌표로 되돌림
        _sightBroadcaster.NotifyMoveSync(zone, player);    // 주변 — 위에서 채택한 좌표를 전파.
                                         //   이게 없으면 상태가 IDLE이라 주기 동기화(MOVING 대상)에도
                                         //   틱 끝 묶음(dirty 대상)에도 안 실려, 주변은 최대 8타일 떨어진
                                         //   옛 위치를 계속 렌더한다.
        return;
    }

    // 플레이어 상태 갱신
    player->_direction = dir;
    player->_moveState = MoveState::MOVING;

    // 델타 동기화 기준 좌표 갱신 (이동 시작 직후 즉시 동기화 방지)
    player->_lastSyncX = player->_x;
    player->_lastSyncY = player->_y;

    // 주변에 MOVE_START 브로드캐스트 (묶음 모드: dirty 마킹만)
    _sightBroadcaster.NotifyMoveStart(zone, player);
}

void CMoveHandler::RecvMoveStop(CPlayer* player, CSerialBuffer* pMsg)
{
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    MSG_C2S_MOVE_STOP recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // 이동 중이 아니면 무시 (서버 벽 클램핑으로 이미 IDLE 처리됨)
    if (player->_moveState != MoveState::MOVING)
        return;

    // Direction 범위 검증 — 유효 값은 Actor.h 한 곳(IsMoveDirection)
    if (!IsMoveDirection(recvMsg.direction))
        return;

    // 서버 권위 모델: 서버 좌표 유지, 상태만 변경
    player->_direction = static_cast<Direction>(recvMsg.direction);
    player->_moveState = MoveState::IDLE;

#if USE_DB_WORKER
    player->_dbDirty = true;   // 정지 → IDLE 전이(주기저장 MOVING 조건에서 빠지므로 최종위치 마킹)
#endif

    // 섹터 이동 판정 — 넘었으면 시야 진입/이탈을 대기열에 추가 (틱 끝에 배치 처리)
    zone->UpdateSectorAndQueue(player);

    // 주변에 MOVE_STOP 브로드캐스트 (묶음 모드: dirty 마킹만)
    _sightBroadcaster.NotifyMoveStop(zone, player, true);
}

// C2S_MOVE_START 좌표 수용의 이동량 예산 — 경과분을 적립하고, 통과하면 소비까지 수행.
//   적립: 마지막 검사 이후 흐른 프레임 × 자기 속도 × 여유계수. 상한이 1회 수용 한도(8타일)와 같아
//         정상 클라는 예산이 늘 만충이다(서버가 이동을 직접 계산하므로 예측 오차 소모가 거의 없음).
//   소비: 실제 수용한 거리만큼. 연타해도 장기 평균은 적립 속도(자기 속도 × 여유계수)로 수렴한다.
bool CMoveHandler::ConsumeMoveBudget(CPlayer* player, float distSq)
{
    const uint64_t elapsedFrames = _frameCount - player->_moveBudgetFrame;
    if (elapsedFrames > 0)
    {
        player->_moveBudgetFrame = _frameCount;
        const float elapsedSec = static_cast<float>(elapsedFrames) * (FRAME_INTERVAL_MS / 1000.0f);
        player->_moveBudget += static_cast<float>(player->_speed) * MOVE_BUDGET_SLACK * elapsedSec;
        if (player->_moveBudget > MOVE_BUDGET_CAP)
            player->_moveBudget = MOVE_BUDGET_CAP;
    }

    const float dist = std::sqrt(distSq);
    if (dist > player->_moveBudget)
    {
        ++_tickMoveBudgetRejects;
        return false;
    }

    player->_moveBudget -= dist;
    return true;
}
