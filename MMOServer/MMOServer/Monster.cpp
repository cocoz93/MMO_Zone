#include "Monster.h"

#if USE_MONSTER

#include "MonsterTable.h"   // MonsterDef — 내 종류의 사양

CMonster::CMonster(const MonsterDef& def)
    : _def(def)
{
    // 자기 종류를 잡는다 — 파생이 정하는 게 규칙(Character.h [종류])
    _kind = CharacterKind::MONSTER;

    // 부품 붙이기 — 몸이 돌 때는 멤버 부품이 이미 만들어져 있다(Character.h AttachParts)
#if USE_COMBAT
    AttachParts(&_health, &_attack, &_buffs);
#else
    AttachParts(&_health, &_attack);
#endif

    // 표시 문자(_displayChar)는 여기서 안 넣는다 — 클라에게 「뭘로 그려라」를 알리는 패킷 값이라
    //   패킷을 아는 게임 서버의 몬스터 조각이 스폰 콜백(CMonsterHandler::OnMonsterSpawn)에서 부류 값과 정의 번호(_defId)를 넣는다.
    //   플레이어(CalcDisplayChar)와 같은 자리.
    _colorIndex  = 0;   // 뱀 한 종류라 색을 가를 일이 없다

    // 스탯 — 내 종류의 표 값을 부품에 넣는다(부품 칸의 0 = 아직 안 정해짐 — Character.h)
    _health.SetMaxHp(def.maxHp);
    _health.FillToMax();
    _attack._attackPower = def.attack;
    _health.SetDefense(def.defense);
    _bodyRadius  = def.bodyRadius;

    // 속도는 CActor 가 0 으로 두고 파생이 정한다(CPlayer 와 같은 규칙)
    _speed = def.speed;
}

CMonster::~CMonster()
{
}

#endif // USE_MONSTER
