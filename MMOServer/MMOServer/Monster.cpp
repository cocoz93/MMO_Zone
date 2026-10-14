#include "Monster.h"

#if USE_MONSTER

#include "MonsterTable.h"   // MonsterDef — 내 종류의 사양

CMonster::CMonster(const MonsterDef& def)
    : _def(def)
{
    // 자기 종류를 잡는다 — 파생이 정하는 게 규칙(Character.h [종류])
    _kind = CharacterKind::MONSTER;

    // 표시 문자(_displayChar)는 여기서 안 넣는다 — 클라에게 「뭘로 그려라」를 알리는 패킷 값이라
    //   패킷을 아는 게임 서버가 스폰 콜백(CGameServer::OnMonsterSpawn)에서 표의 값(_def.display)을 넣는다.
    //   플레이어(CalcDisplayChar)와 같은 자리.
    _colorIndex  = 0;   // 뱀 한 종류라 색을 가를 일이 없다

    // 스탯 — 내 종류의 표 값으로 CCharacter 기본값(플레이어 기준 100/10)을 덮는다
    _hp          = def.maxHp;
    _maxHp       = def.maxHp;
    _attackPower = def.attack;
    _defense     = def.defense;
    _bodyRadius  = def.bodyRadius;

    // 속도는 CActor 가 0 으로 두고 파생이 정한다(CPlayer 와 같은 규칙)
    _speed = def.speed;
}

CMonster::~CMonster()
{
}

#endif // USE_MONSTER
