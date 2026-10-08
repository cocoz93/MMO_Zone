#include "MonsterAI.h"

#if USE_MONSTER

#include <random>

#include "Combat.h"   // CanAttack · ATTACK_RANGE_SQ · 쿨타임 — 공격 판정은 플레이어와 같은 값을 쓴다

namespace MonsterAI
{
namespace
{
    // 배회 방향·쉬는 시간용. CZone::CalcSpawnPos 와 같은 방식(thread_local)이다 —
    //   게임 스레드 단독에서만 불리지만, 전역 상태를 공유하지 않는 편이 나중에 안전하다.
    std::mt19937& Rng()
    {
        static thread_local std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    uint64_t RandFrames(uint64_t lo, uint64_t hi)
    {
        std::uniform_int_distribution<uint64_t> d(lo, hi);
        return d(Rng());
    }

    Direction RandDirection()
    {
        std::uniform_int_distribution<int> d(1, DIRECTION_COUNT);   // 이동 방향 전부 (Actor.h — 값이 1 부터 붙어 있다)
        return static_cast<Direction>(d(Rng()));
    }

    // 지금 방향을 뺀 나머지 셋 중 하나 — 벽에 막혀 멈춘 뱀이 같은 벽으로 다시 걷지 않게
    Direction RandDirectionExcept(Direction except)
    {
        std::uniform_int_distribution<int> d(1, DIRECTION_COUNT - 1);
        int v = d(Rng());                                   // 1 ~ 개수-1
        if (v >= static_cast<int>(except)) ++v;             // except 를 건너뛰어 나머지 중 하나
        return static_cast<Direction>(v);
    }

    // 배회를 새로 시작한다 — 방향·기간을 정하고, 주변은 0.4초마다 보게 한다
    void BeginWander(Decision& d, uint64_t frame)
    {
        d.nextState      = AIState::WANDER;
        d.targetActorId  = -1;
        d.direction      = RandDirection();
        d.moveState      = MoveState::MOVING;
        d.stateEndFrame  = frame + RandFrames(WANDER_MIN_FRAMES, WANDER_MAX_FRAMES);   // 얼마나 오래
        d.nextThinkFrame = frame + THINK_INTERVAL_FRAMES;                              // 얼마나 자주 보나
    }

    // 사거리 안인가 — "이번 판단에서 대상 쪽을 보게 한 뒤" 기준이다.
    //   Combat::IsInAttackRange 는 몬스터의 "지금" 방향을 쓰는데, 판단 도중의 몬스터는 아직
    //   옛 방향을 들고 있다(새 방향은 Decision 에만 있다). 옛 방향으로 물으면 대상이 옆으로
    //   비켜선 순간 "사거리 밖"으로 읽혀 한 걸음(0.6타일) 더 걸어가 대상 위에 올라탄다.
    //   새 방향은 FaceTo 가 대상 쪽 축으로 고르므로 거리만 보면 정면 조건(내적>0)도 저절로 선다.
    //   완전히 겹친 경우(거리 0)만 빼는데, 그땐 내적이 0 이라 공격 본체도 못 때리기 때문이다.
    bool InReach(const CMonster& monster, const CCharacter& target)
    {
        const float dx = target._x - monster._x;
        const float dy = target._y - monster._y;
        const float distSq = dx * dx + dy * dy;
        return distSq > 0.0f && distSq <= Combat::ATTACK_RANGE_SQ;
    }
}

bool IsInSight(const CMonster& monster, const CCharacter& target, float& outDistSq)
{
    const float dx = target._x - monster._x;
    const float dy = target._y - monster._y;
    const float distSq = dx * dx + dy * dy;

    outDistSq = distSq;
    return distSq <= SIGHT_RANGE_SQ;
}

bool IsTooFar(const CMonster& monster, const CCharacter& target)
{
    const float dx = target._x - monster._x;
    const float dy = target._y - monster._y;
    return (dx * dx + dy * dy) > LEASH_RANGE_SQ;
}

Direction FaceTo(const CActor& from, const CActor& to)
{
    const float dx = to._x - from._x;
    const float dy = to._y - from._y;

    // 더 많이 벌어진 축을 고른다 — 부호·동점 규칙은 DirectionFromDelta(Actor.h) 한 곳
    return DirectionFromDelta(dx, dy);
}

Decision Think(const CMonster& monster, const CCharacter* target, uint64_t frame)
{
    // 기본값 = 지금 상태를 그대로 두고 다음 판단만 예약
    Decision d;
    d.nextState      = monster._aiState;
    d.direction      = monster._direction;
    d.moveState      = monster._moveState;
    d.targetActorId  = monster._targetActorId;
    d.attack         = false;
    d.nextThinkFrame = frame + THINK_INTERVAL_FRAMES;
    d.stateEndFrame  = monster._stateEndFrame;

    // ★ 죽어 있으면 아무것도 안 한다.
    //   공격 본체가 죽는 순간 IDLE 로 세워 두는데(시체가 걷지 않게), 여기서 이 검사를 빼면
    //   0.4초 뒤 다음 판단이 다시 MOVING 으로 돌려 시체가 걸어간다.
    //   되살아났을 때 죽기 전 대상을 물고 시작하지 않도록 대상도 비운다.
    if (monster._isDead)
    {
        d.nextState     = AIState::IDLE;
        d.moveState     = MoveState::IDLE;
        d.targetActorId = -1;
        return d;
    }

    // 갈래가 넷이라 switch 로 쓴다 — if/else 면 else 하나가 나머지를 삼킨다.
    switch (monster._aiState)
    {
    case AIState::IDLE:
    {
        if (target != nullptr)
        {
            d.targetActorId = target->_actorId;
            d.direction     = FaceTo(monster, *target);

            // ★ 이미 사거리 안이면 걷지 않는다 — 돌아서서 바로 문다.
            //   걸으면 한 틱(0.6타일)을 가서 대상 위에 올라탄다. 되살아난 직후가 대표적이다
            //   (물던 자리에서 죽고 제자리에서 되살아나므로 이미 사거리 안이다).
            //   실측: 고치기 전 되살아난 뱀 12/12 가 0.6타일을 걸어와 물었다.
            if (InReach(monster, *target))
            {
                d.nextState      = AIState::ATTACK;
                d.moveState      = MoveState::IDLE;
                d.nextThinkFrame = frame + 1;   // 다음 틱에 새 방향으로 쿨타임을 보고 문다
            }
            else
            {
                d.nextState      = AIState::CHASE;
                d.moveState      = MoveState::MOVING;
                d.nextThinkFrame = frame + CHASE_THINK_INTERVAL_FRAMES;
            }
            break;
        }

        // 쉴 만큼 쉬었으면 배회로.
        //   가만히 있는 동안은 쉬는 시간 끝에만 본다(IDLE 로 들어올 때 다음 판단을 멀리 잡았다) —
        //   움직이지 않으니 누구 위로 걸어갈 일이 없다. 배회부터는 0.4초마다 본다.
        BeginWander(d, frame);
        break;
    }

    case AIState::WANDER:
    {
        if (target != nullptr)
        {
            d.targetActorId = target->_actorId;
            d.direction     = FaceTo(monster, *target);

            // IDLE 과 같은 이유 — 이미 사거리 안이면 걷지 않고 바로 문다
            if (InReach(monster, *target))
            {
                d.nextState      = AIState::ATTACK;
                d.moveState      = MoveState::IDLE;
                d.nextThinkFrame = frame + 1;
            }
            else
            {
                d.nextState      = AIState::CHASE;
                d.moveState      = MoveState::MOVING;
                d.nextThinkFrame = frame + CHASE_THINK_INTERVAL_FRAMES;
            }
            break;
        }

        // 배회 기간이 끝났으면 잠깐 선다 — 계속 걷기만 하면 맵 구석에 몰려 붙는다.
        if (frame >= monster._stateEndFrame)
        {
            d.nextState      = AIState::IDLE;
            d.moveState      = MoveState::IDLE;
            d.nextThinkFrame = frame + RandFrames(IDLE_MIN_FRAMES, IDLE_MAX_FRAMES);   // 졸다가 끝에만 본다
            break;
        }

        // 아직 배회 중 — 0.4초 뒤에 다시 주변을 본다.
        //   ★ 벽에 막혀 멈춰 있으면(CZone::Tick 이 맵 경계에서 IDLE 로 세운다) 다른 방향을 고른다.
        //     예전에는 배회 판단이 곧 배회 끝이라 IDLE 을 거치며 저절로 새 방향을 골랐는데,
        //     이제는 배회가 기간 끝까지 이어지므로 이걸 안 하면 벽을 향해 제자리걸음을 한다.
        if (monster._moveState != MoveState::MOVING)
        {
            d.direction = RandDirectionExcept(monster._direction);
            d.moveState = MoveState::MOVING;
        }
        d.nextThinkFrame = frame + THINK_INTERVAL_FRAMES;
        break;
    }

    case AIState::CHASE:
    {
        // 놓쳤거나(시야 밖으로 나가 CMonsterManager 가 nullptr 을 준다) 너무 멀어지면 포기
        if (target == nullptr || IsTooFar(monster, *target))
        {
            BeginWander(d, frame);   // 배회 기간과 주변 보는 주기를 따로 잡는다
            break;
        }

        d.targetActorId  = target->_actorId;
        d.direction      = FaceTo(monster, *target);   // ★ 먼저 보게 한다 — 정면이어야 때릴 수 있다
        d.nextThinkFrame = frame + CHASE_THINK_INTERVAL_FRAMES;   // 쫓는 동안은 자주 본다

        if (InReach(monster, *target))   // 새 방향 기준 (옛 방향으로 물으면 한 걸음 더 간다 — InReach 주석)
        {
            // 사거리 안 — 멈춰서 때린다
            d.nextState = AIState::ATTACK;
            d.moveState = MoveState::IDLE;
            break;
        }

        d.moveState = MoveState::MOVING;   // 아직 멀다 — 계속 쫓는다
        break;
    }

    case AIState::ATTACK:
    {
        if (target == nullptr || IsTooFar(monster, *target))
        {
            BeginWander(d, frame);   // 배회 기간과 주변 보는 주기를 따로 잡는다
            break;
        }

        d.targetActorId = target->_actorId;
        d.direction     = FaceTo(monster, *target);
        d.moveState     = MoveState::IDLE;

        if (!InReach(monster, *target))
        {
            d.nextState      = AIState::CHASE;     // 대상이 걸어 나갔다 — 다시 쫓는다
            d.moveState      = MoveState::MOVING;
            // ★ 쫓는 주기로 되돌린다. 빠뜨리면 기본값(10틱)이 남아, 쫓는 동안 0.4초를 눈 감고
            //   6타일을 걸어가 대상 위에 올라탄다(CHASE_THINK_INTERVAL_FRAMES 주석과 같은 이유).
            d.nextThinkFrame = frame + CHASE_THINK_INTERVAL_FRAMES;
            break;
        }

#if USE_COMBAT
        // ★ 쿨타임을 여기서 먼저 묻는다.
        //   판단 주기(10틱)가 쿨타임(12틱)보다 짧다고 공격이 안 밀리는 게 아니다 —
        //   그냥 때리면 0틱 성공 → 10틱 거부 → 20틱 성공이라 실효 주기가 20틱(0.8초)이 된다.
        //   쿨타임이 차는 시점으로 다음 판단을 당겨 잡으면 12틱마다 정확히 문다.
        if (Combat::CanAttack(monster, frame) == Combat::AttackReject::NONE)
        {
            d.attack         = true;
            d.nextThinkFrame = frame + Combat::ATTACK_COOLDOWN_FRAMES;
        }
        else
        {
            // 아직 쿨타임 — 찰 때까지 기다렸다가 다시 본다 (거부당할 시도를 아예 안 한다)
            d.nextThinkFrame = monster._lastAttackFrame + Combat::ATTACK_COOLDOWN_FRAMES;
        }
#else
        // 전투를 끄면 물지 않는다 — 곁에 서서 기본 주기로 다시 본다(대상이 걸어 나가면 위 InReach 로 다시 쫓는다)
        d.nextThinkFrame = frame + THINK_INTERVAL_FRAMES;
#endif
        break;
    }
    }

    return d;
}

} // namespace MonsterAI

#endif // USE_MONSTER
