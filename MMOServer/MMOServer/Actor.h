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

// 이동 상태
enum class MoveState : uint8_t
{
    IDLE = 0,
    MOVING
};

// ==========================================================================
// CActor — 월드에 존재하는 것의 공통 바탕
//
// [경계] 여기 두는 것 = 좌표를 갖고, 섹터에 속하고, 맞고 죽을 수 있는 것이면 다 갖는 것.
//        플레이어만의 것(세션·계정·표시 문자)은 CPlayer에 남긴다.
//        가르는 물음 하나 — "몬스터에게도 말이 되나".
//
// [주의] 섹터·존 컨테이너는 여전히 vector<CPlayer*>다. 타입을 CActor*로 바꾸면
//        게임 루프(단일 코어 천장)에 종류 분기가 들어가 실측을 다시 재야 한다.
//        이 클래스는 "계층을 세운 것"까지이고, 컨테이너 교체는 범위 밖이다.
//
// [식별] _actorId는 원래부터 S2C 패킷에 싣기 위한 번호다. 서버 내부 조회는
//        세션 기준(CGameServer::FindPlayer)뿐이고 actorId로 객체를 찾는 역인덱스는 없다.
//        그래서 이 필드를 CActor로 올려도 세션 계층과 섞이지 않는다.
// ==========================================================================

// 스탯 기본값 — 전투가 붙기 전까지는 아무도 읽지 않는다(선언만 하는 단계).
constexpr int32_t ACTOR_DEFAULT_MAX_HP        = 100;
constexpr int32_t ACTOR_DEFAULT_ATTACK_POWER  = 10;

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

    // 델타 동기화: 마지막 위치 동기화 시점의 좌표
    float _lastSyncX;
    float _lastSyncY;

    int32_t _listIndex = -1;    // Zone::_playerList 내 인덱스 (O(1) 삭제용)
    int32_t _sectorIndex = -1;  // 현재 소속 섹터 벡터 내 인덱스 (O(1) 삭제용 — _listIndex와 동일 패턴)

    // USE_SECTOR_AGGREGATION: 이번 틱 섹터 묶음 dirty 등록 여부 (중복 방지, 틱 끝 리셋)
    bool _moveDirty = false;

    // ── 전투 스탯 ── 선언만. 읽는 쪽은 USE_COMBAT 경로에서 붙는다.
    int32_t _hp        = ACTOR_DEFAULT_MAX_HP;       // 현재 체력 (0 이하 = 죽음)
    int32_t _maxHp     = ACTOR_DEFAULT_MAX_HP;       // 최대 체력
    int32_t _attackPower = ACTOR_DEFAULT_ATTACK_POWER; // 한 대당 피해량

    bool _isDead = false;            // 죽은 상태 (이동·공격 거부, 공격 대상에서 제외)
    uint64_t _lastAttackFrame = 0;   // 마지막 공격 프레임 (쿨타임 판정 — CGameServer::_frameCount 기준)
    uint64_t _deathFrame = 0;        // 죽은 프레임 (부활 시점 판정 기준)
};
