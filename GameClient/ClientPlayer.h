#pragma once

#include <cstdint>

// 4방향 (서버 Player.h와 동일)
enum class Direction : uint8_t
{
    NONE = 0,
    UP,          // ↑
    DOWN,        // ↓
    LEFT,        // ←
    RIGHT        // →
};

// 이동 상태 (서버 Player.h와 동일)
enum class MoveState : uint8_t
{
    IDLE = 0,
    MOVING
};

// 클라이언트용 플레이어 데이터
struct ClientPlayer
{
    int32_t playerId = 0;
    uint8_t displayChar = 'A';  // 서버에서 할당받은 표시 문자
    uint8_t colorIndex = 0;     // 서버에서 할당받은 색상 인덱스
    float x = 0.0f;
    float y = 0.0f;
    Direction direction = Direction::NONE;
    MoveState moveState = MoveState::IDLE;
    int32_t speed = 0;

    // 존 이동 스폰 이펙트 (깜빡임 잔여 시간, 0이면 이펙트 없음)
    float spawnEffectTimer = 0.0f;

    // 레벨·경험치 (서버 USE_EXP) — 본인 것만 온다. 남의 것은 늘 기본값이다.
    //   nextLevelExp == 0 은 만렙이라는 뜻 (그때 비율을 계산하면 0 으로 나눈다)
    int32_t level = 1;
    int64_t exp = 0;
    int64_t levelBaseExp = 0;
    int64_t nextLevelExp = 0;
    // 첫 장을 받았나 — 레벨업 알림을 "기준선"과 가르는 데 쓴다.
    //   없으면 20 레벨로 접속하거나 존을 넘을 때마다 "Level up!" 이 뜬다
    //   (존 이동은 판을 비우고 다시 채우므로 직전 레벨이 늘 기본값 1 이다).
    bool hasExp = false;
};
