#include "Monster.h"

#if USE_MONSTER

#include <Protocol/Protocol.h>   // DISPLAY_SNAKE — 표시 문자 예약값

CMonster::CMonster()
{
    // 자기 종류를 잡는다 — 파생이 정하는 게 규칙(Character.h [종류])
    _kind = CharacterKind::MONSTER;

    // 표시 — 와이어는 안 바꾼다. 기존 CREATE 패킷의 displayChar 에 예약값을 싣고,
    //   클라가 그 값을 보고 사람 대신 몬스터 스프라이트를 고른다(Protocol.h DISPLAY_MONSTER_BASE).
    _displayChar = DISPLAY_SNAKE;
    _colorIndex  = 0;   // 뱀 한 종류라 색을 가를 일이 없다

    // 스탯 — CCharacter 기본값(플레이어 기준 100/10)을 덮는다
    _hp          = SNAKE_MAX_HP;
    _maxHp       = SNAKE_MAX_HP;
    _attackPower = SNAKE_ATTACK_POWER;
    _defense     = SNAKE_DEFENSE;

    // 속도는 CActor 가 0 으로 두고 파생이 정한다(CPlayer 와 같은 규칙)
    _speed = SNAKE_SPEED;
}

CMonster::~CMonster()
{
}

#endif // USE_MONSTER
