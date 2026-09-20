#include "Character.h"

CCharacter::CCharacter()
    : _kind(CharacterKind::PLAYER)
    , _displayChar('A')
    , _colorIndex(0)
    , _sessionId(-1)
{
}

CCharacter::~CCharacter()
{
}
