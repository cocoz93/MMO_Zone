#include "Combat.h"

#include <cassert>

#if USE_COMBAT

namespace Combat
{

AttackReject CanAttack(const CHealthPart& attackerHealth, const CAttackPart& attackerAttack, uint64_t frame)
{
    if (attackerHealth.IsDead())
        return AttackReject::DEAD_SELF;

    // _lastAttackFrame == 0 은 "아직 한 번도 안 휘둘렀다"는 뜻이다.
    //   프레임 번호는 1부터 증가하므로(CGameServer가 틱 머리에서 ++) 실제 값으로 0이 될 일은 없다.
    //   이 검사를 빼면 첫 공격이 0과 비교돼 쿨타임에 걸린다.
    if (attackerAttack._lastAttackFrame != 0 &&
        frame - attackerAttack._lastAttackFrame < ATTACK_COOLDOWN_FRAMES)
        return AttackReject::COOLDOWN;

    return AttackReject::NONE;
}

bool IsInAttackRange(const CCharacter& attacker, const CCharacter& target, float& outDistSq)
{
    const float dx = target._x - attacker._x;
    const float dy = target._y - attacker._y;
    const float distSq = dx * dx + dy * dy;

    const float reach = AttackReach(attacker, target);
    if (distSq > reach * reach)
        return false;

    // 바라보는 쪽 단위벡터 — 부호는 DirectionStep(Actor.h) 한 곳. NONE 은 (0, 0) 이라 아래 내적이 0 → 아무도 못 때린다.
    const DirectionVec f = DirectionStep(attacker._direction);

    // 내적이 양수여야 정면이다. 뒤통수(<0)도, 정확히 옆(==0)도, 겹쳐 선 경우(distSq==0 → 내적 0)도 안 맞는다.
    if (dx * f.x + dy * f.y <= 0.0f)
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

bool IsRespawnDue(const CHealthPart& health, uint64_t frame)
{
    if (!health.IsDead())
        return false;

    return (frame - health.DeathFrame()) >= RESPAWN_DELAY_FRAMES;
}

void Respawn(CCharacter& actor)
{
    assert(actor.Health() != nullptr && "체력 부품 없는 것을 되살림 — 죽을 수 없는 것이다");
    actor.Health()->Revive();

    // 죽을 때 IDLE 로 세웠어도 여기서 한 번 더 못 박는다 —
    //   살아난 직후 MOVING 으로 남아 있으면 틱이 클라가 모르는 방향으로 끌고 간다.
    actor._moveState = MoveState::IDLE;
}

} // namespace Combat

#endif // USE_COMBAT
