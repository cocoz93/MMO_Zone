#include "Actor.h"

CActor::CActor()
    : _actorId(0)
    , _x(0.0f)
    , _y(0.0f)
    , _sectorX(0)
    , _sectorY(0)
    , _direction(Direction::NONE)
    , _moveState(MoveState::IDLE)
    , _speed(0)          // 실제 값은 파생 클래스가 정한다 (CPlayer는 랜덤, 몬스터는 종류별)
    , _zoneId(-1)
    , _lastSyncX(0.0f)
    , _lastSyncY(0.0f)
{
}

CActor::~CActor()
{
}
