#include "Player.h"

CPlayer::CPlayer()
    : _isAdmin(false)
{
    // 자기 종류를 잡는다 — CCharacter 기본값과 같지만 명시한다(파생이 정하는 게 규칙, Character.h [종류])
    _kind = CharacterKind::PLAYER;

    // 부품 붙이기 — 몸이 돌 때는 멤버 부품이 이미 만들어져 있다(Character.h AttachParts)
#if USE_COMBAT
    AttachParts(&_health, &_attack, &_buffs);
#else
    AttachParts(&_health, &_attack);
#endif

    // 25 ~ 50 (5 단위) 랜덤 속도 부여 — CActor는 0으로 두고 파생이 정한다
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(5, 10);
    _speed = dist(rng) * 5;
}

CPlayer::~CPlayer()
{
}
