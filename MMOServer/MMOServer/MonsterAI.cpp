#include "MonsterAI.h"

#if USE_MONSTER

#include <cassert>
#include <random>

#include "Combat.h"   // CanAttack · AttackReach · 쿨타임 — 공격 판정은 플레이어와 같은 값을 쓴다

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
    //   닿는 거리는 두 몸 반지름을 더한 Combat::AttackReach — 몸이 큰 보스는 그만큼 먼 데서 멈춘다.
    bool InReach(const CMonster& monster, const CCharacter& target)
    {
        const float dx = target._x - monster._x;
        const float dy = target._y - monster._y;
        const float distSq = dx * dx + dy * dy;
        const float reach = Combat::AttackReach(monster, target);
        return distSq > 0.0f && distSq <= reach * reach;
    }

#if USE_SKILL
    // 쓸 스킬 하나 — 규칙을 위에서부터 보며 **첫 통과**의 번호(없으면 0). 규칙이 없는 몬스터는 곧바로 0 이다.
    //   ⑴ 쿨타임이 찼나 — 안 보면 쿨타임 중인 스킬이 매번 뽑혀 관리자가 거절(COOLDOWN)만 받고, 그 판단의 평타도 스킬에 먹혀 몬스터가 아예 안 때린다
    //   ⑵ 내 체력이 조건 이하인가 — 정수 곱으로 견준다(hp × 100 ≤ maxHp × %). 나눗셈의 반올림이 경계를 흔들지 않게
    //   ⑶ 대상이 거리 안인가 — 제곱끼리 견준다(루트 안 씀). 규칙의 maxDist 는 두 몸의 가장자리 사이 거리라 중심 사이로는 두 반지름을 더해 본다
    //      (표 읽기의 FRONT·SHAPE 상한이 가장자리 사이 거리로 대조되므로 닿지 않는 거리를 뽑지 않는다 — MonsterSkillTable.h maxDist)
    //   ⑷ 되살리기 줄이면 되살릴 빈 점이 있나 — 없으면 건너뛴다(Think 선언 주석의 vacancy)
    //   쿨타임 칸은 캐릭터 것이다(CCharacter::_skillReadyFrame — 사람과 같은 칸). 비용·대상·효과는 여기서 안 본다 — 그건 CSkillSystem 의 일이다.
    int32_t PickSkill(const CMonster& monster, const CCharacter& target, uint64_t frame, const SkillRules& rules, const RuleVacancy& vacancy)
    {
        if (rules.empty())
            return 0;

        const float dx = target._x - monster._x;
        const float dy = target._y - monster._y;
        const float distSq = dx * dx + dy * dy;

        // [불변식] 규칙 수는 표 읽기가 상한으로 묶었다(CMonsterSkillTable::MAX_RULES_PER_MONSTER = vacancy 의 칸 수)
        assert(rules.size() <= vacancy.size());
        for (size_t i = 0; i < rules.size(); ++i)
        {
            const MonsterSkillRule& r = rules[i];
            if (frame < monster._skillReadyFrame[r.skillId])
                continue;
            if (static_cast<int64_t>(monster._hp) * 100 > static_cast<int64_t>(monster._maxHp) * r.hpBelowPct)
                continue;
            const float reach = r.maxDist + monster._bodyRadius + target._bodyRadius;
            if (distSq > reach * reach)
                continue;
            if (r.reviveMonsterId != 0 && vacancy[i] == 0)
                continue;
            return r.skillId;
        }
        return 0;
    }
#endif
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

Decision Think(const CMonster& monster, const CCharacter* target, uint64_t frame
#if USE_SKILL
             , const SkillRules& rules, const RuleVacancy& vacancy
#endif
)
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
    //   10-09 부터 죽은 몬스터는 죽는 순간 지워져(CGameServer::OnDeath 끝) 여기까지 오지 않는다 — 지우는 길이
    //   바뀌어 시체가 하나라도 남는 날 걸어 다니지 않게 남겨 둔다. 예전엔 시체가 남아 이 검사가 실제로 일했다
    //   (공격 본체가 죽는 순간 IDLE 로 세워도, 이 검사가 없으면 0.4초 뒤 판단이 다시 MOVING 으로 돌렸다).
    //   대상도 비운다 — 시체가 누군가를 쫓는 중으로 남지 않게.
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
            //   걸으면 한 틱(0.6타일)을 가서 대상 위에 올라탄다. 쉬던 뱀 바로 앞까지 사람이 걸어온 경우다.
            //   실측: 고치기 전(몬스터가 제자리에서 되살아나던 때) 되살아난 뱀 12/12 가 0.6타일을 걸어와 물었다.
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

#if USE_SKILL
        // 쫓는 중에도 스킬이 닿는 거리면 쓴다 — 멈추지 않는다(이동은 아래가 정한다). 방향은 위에서 대상 쪽으로 세웠고 관리자가 그 방향을 적용한 뒤 시전한다(모양이 앞쪽이다)
        d.skillId = PickSkill(monster, *target, frame, rules, vacancy);
#endif

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

#if USE_SKILL
        // 스킬이 닿으면 이번 판단은 스킬이다 — 평타(d.attack)를 대신한다(한 번의 판단에 행동 하나). 다음 틱에 다시 본다:
        //   스킬은 쿨타임이 차면 다시 쓸 수 있고 평타는 따로 센다(10-10 — 스킬이 평타를 안 막는다)라 다음 판단은 평타를 칠 수 있다
        d.skillId = PickSkill(monster, *target, frame, rules, vacancy);
        if (d.skillId != 0)
        {
            d.nextThinkFrame = frame + 1;
            break;
        }
#endif

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
