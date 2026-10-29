#include "Character.h"

#include <cassert>

CCharacter::CCharacter()
    : _kind(CharacterKind::PLAYER)
    , _displayChar('A')
    , _colorIndex(0)
{
}

CCharacter::~CCharacter()
{
}

#if USE_COMBAT
void CCharacter::AttachComponents(CHealthComponent* health, CAttackComponent* attack, CBuffComponent* buffs)
#else
void CCharacter::AttachComponents(CHealthComponent* health, CAttackComponent* attack)
#endif
{
    assert(_healthComponent == nullptr && _attackComponent == nullptr && "컴포넌트는 생성자에서 한 번만 붙인다");
    _healthComponent = health;
    _attackComponent = attack;
#if USE_COMBAT
    assert(_buffComponent == nullptr && "컴포넌트는 생성자에서 한 번만 붙인다");
    _buffComponent = buffs;
#endif
}

void CCharacter::AttachTalk(const NpcDef* def)
{
    assert(_talk == nullptr && def != nullptr && "말 걸기 칸은 생성자에서 한 번만 붙인다");
    _talk = def;
}
