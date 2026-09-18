#pragma once

#include <cstdint>
#include <string>
#include <random>

#include "Actor.h"   // CActor · Direction · MoveState

// CPlayer — 사람이 조종하는 액터.
//   좌표·섹터·방향·스탯처럼 "월드에 있는 것이면 다 갖는 것"은 CActor에 있다.
//   여기 남은 것은 세션·계정·표시 속성처럼 몬스터에게는 말이 안 되는 것뿐이다.
class CPlayer : public CActor
{
    friend class CGameServer;  // 경계 계층만 _sessionId 접근 허용

public:
    CPlayer();
    ~CPlayer() override;

    uint8_t _displayChar; // 표시 문자 (ASCII: A-Z, a-z, 0-9)
    uint8_t _colorIndex;  // 색상 인덱스 (0-6)

    bool _isAdmin;        // 운영자 여부

    // USE_DB_WORKER: 계정 식별자 (1단계는 스폰 시 서버가 임시 부여, 2단계는 접속 핸드셰이크로 수신)
    int64_t _accountId = 0;
    // USE_DB_WORKER: 마지막 저장 이후 위치가 바뀌었는지 (주기 저장 대상 선별, 저장 후 리셋)
    bool _dbDirty = false;

    // C2S_MOVE_START 좌표 수용의 초당 이동량 예산 (치트 방지).
    //   수용은 IDLE 전이 때마다 일어나므로 STOP/START 연타로 무제한 순간이동이 가능했다.
    //   경과 시간만큼 자기 속도에 비례해 적립하고, 수용한 거리만큼 차감한다.
    //   초기값 0 + 프레임 0 = 첫 수용 시 경과가 커서 상한까지 적립됨(스폰 직후 만충과 동일).
    float    _moveBudget = 0.0f;       // 남은 예산 (타일)
    uint64_t _moveBudgetFrame = 0;     // 마지막 적립 프레임 (CGameServer::_frameCount 기준)

private:
    int64_t _sessionId;   // 네트워크 세션 ID (CGameServer만 접근)
};
