#include "SkillSystem.h"

#if USE_SKILL

#include <algorithm>         // remove_if — 모양 밖 후보 버리기

#include "SkillTable.h"      // SkillDef · 대상·효과 종류 · IsInShape
#include "GameDataFiles.h"   // 스킬 표
#include "GameMonitor.h"     // 시전·거절 지표
#include "CombatSystem.h"    // FindFrontTarget · CollectCandidates · ApplyHit — 일반 공격과 같이 쓴다
#include "BuffSystem.h"      // Apply — 지속 효과 걸기(효과 축 BUFF_ATTACK_MUL)
#include "Character.h"       // 종류·생사·체력·쿨타임 필드 — 시전자는 CCharacter 다

CSkillSystem::CSkillSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor, CCombatSystem& combat, CBuffSystem& buff, ISkillEvents& events)
    : _tables(tables)
    , _gameMonitor(gameMonitor)
    , _combat(combat)
    , _buff(buff)
    , _events(events)
{
}

SkillReject CSkillSystem::UseSkill(CZone* zone, CCharacter* caster, int32_t skillId, uint64_t frame)
{
    // 지표(_skill)는 지금 사람 몫뿐이다 — 몬스터가 스킬을 부르는 곳이 아직 없다.
    //   보스 단계에서 몬스터가 부르게 되면 일반 공격(PerformAttack)처럼 종류로 갈라 센다 — 안 가르면 사람 지표에 섞인다.

    // ── 1) 시전자 검사 — 번호·생사·쿨타임·체력·마력. 뗄 체력은 여기서 세고 4) 가 그대로 뗀다 ──
    const SkillDef* def = nullptr;
    int32_t hpCost = 0;
    SkillReject reject = CheckCaster(caster, skillId, frame, def, hpCost);

    // ── 2) 대상 고르기 — 없으면 NO_TARGET. 아직 아무것도 안 뗐다 ──
    if (reject == SkillReject::COUNT)
        reject = SelectTargets(zone, caster, *def);

    if (reject != SkillReject::COUNT)
    {
        _gameMonitor._skill._rejected[static_cast<int>(reject)].Inc();
        return reject;
    }

    // ── 3) 시전 전 체력 — 피해 식의 바탕이라 떼기 전에 잡아 둔다 ──
    const int32_t hpBefore = caster->_hp;

    // ── 4) 떼기 — 여기까지 왔으면 효과가 반드시 난다(모든 검사를 지났다) ──
    caster->_hp -= hpCost;
    caster->_mp -= def->costMp;   // 모자라지 않음은 1) 의 LOW_MP 검사가 이미 봤다
    caster->_skillReadyFrame[def->skillId] = frame + def->cooldownFrames;   // 성공했을 때만 — 거절은 쿨타임을 안 건다
    _gameMonitor._skill._cast[def->skillId].Inc();

    // ── 5) 시전 알림 — 방송과 시전자 체력·마력 알림은 받는 쪽(게임 서버) ──
    _events.OnCast(zone, caster, *def);

    // ── 6) 효과 ──
    ApplyEffect(zone, caster, *def, hpBefore, frame);
    return SkillReject::COUNT;
}

SkillReject CSkillSystem::CheckCaster(const CCharacter* caster, int32_t skillId, uint64_t frame, const SkillDef*& outDef, int32_t& outHpCost) const
{
    // SkillReject 의 순서 그대로 본다 — 여럿이 겹치면 먼저 걸린 하나로 답한다(열거 순서가 곧 확인 순서).
    //   번호 0 · 상한 밖 · 표에 없는 번호는 Find 가 다 nullptr 이다 — 그래서 아래 배열 색인은 늘 1~MAX_SKILL_ID 다.
    outDef = _tables.Skill().Find(skillId);
    if (outDef == nullptr)
        return SkillReject::BAD_SKILL;

    if (caster->_isDead)
        return SkillReject::DEAD;

    if (frame < caster->_skillReadyFrame[outDef->skillId])
        return SkillReject::COOLDOWN;

    // 체력 비용 — 현재 체력 × % 를 내림, 소모가 있는 스킬이면 최소 1
    outHpCost = 0;
    if (outDef->costHpPct > 0)
    {
        const int64_t cost = static_cast<int64_t>(caster->_hp) * outDef->costHpPct / 100;
        outHpCost = (cost < 1) ? 1 : static_cast<int32_t>(cost);
    }

    // 자기 스킬로는 안 죽는다 — 떼고 나서 1 미만이면 거절(아무것도 안 뗀다)
    if (caster->_hp - outHpCost < 1)
        return SkillReject::LOW_HP;

    // 마력 비용 — 표 값 그대로(고정). 딱 맞으면 쓴다(0 까지 쓸 수 있다 — 체력과 달리 0 이 죽음이 아니다)
    if (caster->_mp < outDef->costMp)
        return SkillReject::LOW_MP;

    return SkillReject::COUNT;   // 통과
}

SkillReject CSkillSystem::SelectTargets(CZone* zone, CCharacter* caster, const SkillDef& def)
{
    _targets.clear();

    // 후보는 시전자 종류로 고른다 — 일반 공격(CombatSystem.cpp PerformAttack 의 3) 대상 고르기)과 같은 식.
    //   사람이 쓰면 나머지 목록(지금은 몬스터뿐 — 사람은 사람의 스킬에 안 맞는다),
    //   몬스터가 쓰면 플레이어 목록(몬스터는 몬스터를 못 때린다 — 빠뜨리면 일반 공격에서 실제로 났던 뱀→뱀 결함이 스킬로 다시 난다).
    //   ★ 몬스터 쪽(PLAYERS)은 아직 아무도 안 탄다 — 몬스터가 스킬을 부르는 곳이 보스 단계에서 생긴다.
#if USE_MONSTER
    const bool byMonster = (caster->_kind == CharacterKind::MONSTER);
#else
    constexpr bool byMonster = false;
#endif
    const CCombatSystem::CandidateList which = byMonster ? CCombatSystem::CandidateList::PLAYERS : CCombatSystem::CandidateList::OTHERS;

    switch (def.target)
    {
    case SkillTarget::FRONT:
    {
        // 일반 공격과 같은 함수 — 후보 목록만 위에서 고른 것.
        //   시체만 있었는지(sawDead)는 지금 안 가른다 — 없으면 NO_TARGET 하나. 사유를 가를 날 쓸 값이다.
        bool sawDead = false;
        if (CCharacter* target = _combat.FindFrontTarget(zone, caster, which, sawDead))
            _targets.push_back(target);
        break;
    }

    case SkillTarget::SHAPE:
    {
        // 칸 목록이 닿는 반경(shapeReach)의 후보를 다 모은 뒤 모양 밖을 버린다 — 맞히기는 ApplyEffect(다 고른 뒤)다.
        //   기준은 시전자의 실제 좌표와 바라보는 쪽(IsInShape). 한 칸에 여럿이면 다 남는다(맞는 수 상한 없음, 10-11).
        _combat.CollectCandidates(zone, caster->_x, caster->_y, def.shapeReach, which, caster, _targets);
        const float     casterX = caster->_x;
        const float     casterY = caster->_y;
        const Direction facing  = caster->_direction;
        _targets.erase(std::remove_if(_targets.begin(), _targets.end(), [&](const CCharacter* t)
        {
            return !IsInShape(facing, t->_x - casterX, t->_y - casterY, def.shape);
        }), _targets.end());
        break;
    }

    case SkillTarget::SELF:
        // 자기 자신 — 후보 목록(which)을 안 본다. 살아 있음은 1) 의 DEAD 검사가 봤다. 늘 하나라 NO_TARGET 이 안 난다
        _targets.push_back(caster);
        break;

    case SkillTarget::COUNT:
        break;   // 표 읽기가 막는다(SkillTable.cpp ⑶) — 여기 오지 않는다
    }

    return _targets.empty() ? SkillReject::NO_TARGET : SkillReject::COUNT;
}

void CSkillSystem::ApplyEffect(CZone* zone, CCharacter* caster, const SkillDef& def, int32_t hpBefore, uint64_t frame)
{
    switch (def.effect)
    {
    case SkillEffect::DAMAGE:
    {
        // 공격값 = 시전 전 체력 × power% (내림). 방어력은 ApplyHit 가 뺀다(일반 공격과 같은 식)
        const int32_t attackValue = static_cast<int32_t>(static_cast<int64_t>(hpBefore) * def.power / 100);

        // ★ 다 고른 뒤 하나씩 — 몬스터가 죽으면 ApplyHit 안(OnDeath)에서 해제되고 섹터 목록에서 빠진다.
        //   버퍼의 그 칸은 이 뒤로 안 읽는다(한 대상은 한 번만 맞는다).
        for (CCharacter* target : _targets)
        {
            _gameMonitor._skill._hits[def.skillId].Inc();   // 맞힌 대상마다 1 — ApplyHit 앞에서 센다(몬스터면 그 안에서 해제된다)
            _combat.ApplyHit(zone, caster, target, attackValue, frame);
        }
        break;
    }

    case SkillEffect::BUFF_ATTACK_MUL:
    {
        // 대상(지금은 self 뿐 — 표 읽기가 막는다)마다 공격력 배수를 걸거나 되감는다(같은 종류는 나중 것이 값·시간을 덮는다).
        //   스탯 다시 세우기와 알림(ACTOR_BUFF → MY_STATS)은 CBuffSystem 이 한다. 와이어 = SKILL_CAST → ACTOR_MP → ACTOR_BUFF → MY_STATS.
        //   _hits 는 안 센다 — 「맞힌 수」는 피해 스킬 것이다. 걸린 수는 mmo_buff_applied_total·refreshed 가 센다.
        for (CCharacter* target : _targets)
            _buff.Apply(target, BuffKind::ATTACK_MUL, def.power, def.durationFrames, frame);
        break;
    }

    case SkillEffect::COUNT:
        break;   // 표 읽기가 막는다 — 여기 오지 않는다
    }

    _targets.clear();   // 해제된 몬스터를 가리키는 칸을 남기지 않는다
}

#endif // USE_SKILL
