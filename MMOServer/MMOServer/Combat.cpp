#include "Combat.h"

#if USE_COMBAT

namespace Combat
{

AttackReject CanAttack(const CCharacter& attacker, uint64_t frame)
{
    if (attacker._isDead)
        return AttackReject::DEAD_SELF;

    // _lastAttackFrame == 0 은 "아직 한 번도 안 휘둘렀다"는 뜻이다.
    //   프레임 번호는 1부터 증가하므로(CGameServer가 틱 머리에서 ++) 실제 값으로 0이 될 일은 없다.
    //   이 검사를 빼면 첫 공격이 0과 비교돼 쿨타임에 걸린다.
    if (attacker._lastAttackFrame != 0 &&
        frame - attacker._lastAttackFrame < ATTACK_COOLDOWN_FRAMES)
        return AttackReject::COOLDOWN;

    return AttackReject::NONE;
}

bool IsInAttackRange(const CCharacter& attacker, const CCharacter& target, float& outDistSq)
{
    const float dx = target._x - attacker._x;
    const float dy = target._y - attacker._y;
    const float distSq = dx * dx + dy * dy;

    if (distSq > ATTACK_RANGE_SQ)
        return false;

    // 바라보는 쪽 단위벡터. 부호는 Zone.cpp 의 이동 적용부와 같아야 한다 — UP 이 -y 다.
    float fx = 0.0f;
    float fy = 0.0f;
    switch (attacker._direction)
    {
    case Direction::UP:    fy = -1.0f; break;
    case Direction::DOWN:  fy =  1.0f; break;
    case Direction::LEFT:  fx = -1.0f; break;
    case Direction::RIGHT: fx =  1.0f; break;
    default:               return false;   // NONE — 바라보는 쪽이 없으면 아무도 못 때린다
    }

    // 내적이 양수여야 정면이다. 뒤통수(<0)도, 정확히 옆(==0)도, 겹쳐 선 경우(distSq==0 → 내적 0)도 안 맞는다.
    if (dx * fx + dy * fy <= 0.0f)
        return false;

    outDistSq = distSq;
    return true;
}

int32_t CalcDamage(int32_t attack, int32_t defense, bool& outFloored)
{
    const int32_t raw = attack - defense;
    outFloored = (raw < MIN_DAMAGE);
    return outFloored ? MIN_DAMAGE : raw;
}

bool ApplyDamage(CCharacter& target, int32_t damage, int32_t& outRemainHp)
{
    target._hp -= damage;
    if (target._hp < 0)
        target._hp = 0;   // 와이어로 음수 HP가 나가면 클라 막대가 뒤집힌다

    outRemainHp = target._hp;
    return target._hp == 0;
}

bool IsRespawnDue(const CCharacter& actor, uint64_t frame)
{
    if (!actor._isDead)
        return false;

    return (frame - actor._deathFrame) >= RESPAWN_DELAY_FRAMES;
}

void Respawn(CCharacter& actor)
{
    actor._hp = actor._maxHp;
    actor._isDead = false;
    actor._deathFrame = 0;

    // 죽을 때 IDLE 로 세웠어도 여기서 한 번 더 못 박는다 —
    //   살아난 직후 MOVING 으로 남아 있으면 틱이 클라가 모르는 방향으로 끌고 간다.
    actor._moveState = MoveState::IDLE;
}

} // namespace Combat

#endif // USE_COMBAT
