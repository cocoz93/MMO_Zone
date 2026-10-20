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
void CCharacter::AttachParts(CHealthPart* health, CAttackPart* attack, CBuffPart* buffs)
#else
void CCharacter::AttachParts(CHealthPart* health, CAttackPart* attack)
#endif
{
    assert(_healthPart == nullptr && _attackPart == nullptr && "부품은 생성자에서 한 번만 붙인다");
    _healthPart = health;
    _attackPart = attack;
#if USE_COMBAT
    assert(_buffPart == nullptr && "부품은 생성자에서 한 번만 붙인다");
    _buffPart = buffs;
#endif
}
