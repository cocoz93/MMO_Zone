#pragma once

#include <cstdint>

// 4방향
enum class Direction : uint8_t
{
    NONE = 0,
    UP,          // ↑
    DOWN,        // ↓
    LEFT,        // ←
    RIGHT        // →
};

// ── 방향의 규칙은 여기 한 곳 — 좌표계(UP 이 -y)·유효 값·개수 (10-08 아키텍처 2차 검사 4번) ──
//   쓰는 쪽(이동·벽 판정·공격 정면·몬스터가 바라볼 쪽·패킷 범위 검사·무작위 방향)은 숫자·부호를 다시 적지 않는다.
//   방향을 늘리면(대각선 등) 서버 안의 위 쓰임새는 enum · DIRECTION_COUNT · DirectionStep 의 줄만 고치면 따라온다
//   (클라의 방향 해석·벽에서 미끄러질지·몬스터가 대각선을 쓸지는 따로 정한다).
constexpr int DIRECTION_COUNT = 4;   // NONE 을 뺀 이동 방향 수 — 값은 1..DIRECTION_COUNT 로 붙어 있다
static_assert(static_cast<int>(Direction::RIGHT) == DIRECTION_COUNT, "방향을 늘렸으면 DIRECTION_COUNT·DirectionStep 도 고칠 것");

struct DirectionVec { float x; float y; };

// 방향 → 길이 1 의 단위 벡터. NONE 은 (0, 0). 대각선을 넣으면 0.7071 꼴 — 이동 거리·내적이 그대로 맞는다.
constexpr DirectionVec DirectionStep(Direction d)
{
    switch (d)
    {
    case Direction::UP:    return {  0.0f, -1.0f };
    case Direction::DOWN:  return {  0.0f,  1.0f };
    case Direction::LEFT:  return { -1.0f,  0.0f };
    case Direction::RIGHT: return {  1.0f,  0.0f };
    default:               return {  0.0f,  0.0f };
    }
}

// 와이어로 온 값이 이동 방향인가 (NONE·범위 밖이면 false)
constexpr bool IsMoveDirection(uint8_t v)
{
    return v >= 1 && v <= DIRECTION_COUNT;
}

// 벡터 → 방향 — 그 벡터와 내적이 가장 큰 방향(4방향이면 더 많이 벌어진 축). 부호는 DirectionStep 이 정한다.
//   동점이면 뒤 값이 이긴다(>=) — 「|dx| == |dy| 면 가로, 둘 다 0 이면 오른쪽」이 이 순서에서 나온다.
inline Direction DirectionFromDelta(float dx, float dy)
{
    Direction best    = Direction::NONE;
    float     bestDot = 0.0f;
    for (int i = 1; i <= DIRECTION_COUNT; ++i)
    {
        const Direction    d   = static_cast<Direction>(i);
        const DirectionVec s   = DirectionStep(d);
        const float        dot = dx * s.x + dy * s.y;
        if (best == Direction::NONE || dot >= bestDot)
        {
            best    = d;
            bestDot = dot;
        }
    }
    return best;
}

// 이동 상태
enum class MoveState : uint8_t
{
    IDLE = 0,
    MOVING
};

// ==========================================================================
// CActor — 월드에 존재하는 것의 공통 바탕
//
// [경계] 여기 두는 것 = 좌표를 갖고, 섹터에 속하고, 움직이는 것이면 다 갖는 것.
//        맞고 죽고 화면에 보이는 것(HP·표시·조종 세션)은 CCharacter 에,
//        사람이 붙어야 말이 되는 것(계정·이동 예산·운영자)은 CPlayer 에 있다.
//        가르는 물음 하나 — "나무나 포탈에도 말이 되나".
//
// [식별] _actorId는 원래부터 S2C 패킷에 싣기 위한 번호다. 서버 내부 조회는
//        세션 기준(CSessionHub::FindPlayer)뿐이고 actorId로 객체를 찾는 역인덱스는 없다.
//        그래서 이 필드를 CActor로 올려도 세션 계층과 섞이지 않는다.
// ==========================================================================

class CActor
{
public:
    CActor();
    virtual ~CActor();

    int32_t _actorId;     // S2C 패킷 전용 엔티티 식별자 (서버 내부는 포인터로 식별)

    // 월드 좌표
    float _x;
    float _y;

    // 섹터 좌표
    int32_t _sectorX;
    int32_t _sectorY;

    Direction _direction;
    MoveState _moveState;
    int32_t _speed;

    int32_t _zoneId;      // 소속 존 ID (-1: 미배정)

    // 델타 동기화: 마지막 위치 동기화 시점의 좌표 — 「클라가 마지막으로 받은 자리」라는 송신 쪽 기록.
    //   읽고 쓰는 곳은 게임 서버뿐(보정 훑기 · 입장 · 이동 수용 · 존 이동 · 몬스터 등장 알림). 존 층은 안 건드린다.
    float _lastSyncX;
    float _lastSyncY;

    int32_t _listIndex = -1;    // Zone::_characterList 내 인덱스 (O(1) 삭제용)
    int32_t _sectorIndex = -1;  // 현재 소속 섹터 벡터 내 인덱스 (O(1) 삭제용 — _listIndex와 동일 패턴)

    // USE_SECTOR_AGGREGATION: 이번 틱 섹터 묶음 dirty 등록 여부 (중복 방지, 틱 끝 리셋)
    bool _moveDirty = false;
};
