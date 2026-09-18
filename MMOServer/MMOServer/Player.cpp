#include "Player.h"

CPlayer::CPlayer()
    : _sessionId(-1)
    , _displayChar('A')
    , _colorIndex(0)
    , _isAdmin(false)
{
    // 25 ~ 50 (5 단위) 랜덤 속도 부여 — CActor는 0으로 두고 파생이 정한다
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(5, 10);
    _speed = dist(rng) * 5;
}

CPlayer::~CPlayer()
{
}
