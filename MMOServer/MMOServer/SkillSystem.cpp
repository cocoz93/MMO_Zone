#include "SkillSystem.h"

#if USE_SKILL

#include "SkillTable.h"      // SkillDef · 대상·효과 종류
#include "GameDataFiles.h"   // 스킬 표
#include "GameMonitor.h"     // 시전·거절 지표
#include "CombatSystem.h"    // FindFrontTarget · ApplyHit — 일반 공격과 같이 쓴다
#include "Player.h"          // 생사·체력·쿨타임 필드

CSkillSystem::CSkillSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor, CCombatSystem& combat, ISkillEvents& events)
    : _tables(tables)
    , _gameMonitor(gameMonitor)
    , _combat(combat)
    , _events(events)
{
}

SkillReject CSkillSystem::UseSkill(CZone* zone, CPlayer* player, int32_t skillId, uint64_t frame)
{
    // ── 1) 시전자 검사 — 번호·생사·쿨타임·체력. 뗄 체력은 여기서 세고 4) 가 그대로 뗀다 ──
    const SkillDef* def = nullptr;
    int32_t hpCost = 0;
    SkillReject reject = CheckCaster(player, skillId, frame, def, hpCost);

    // ── 2) 대상 고르기 — 없으면 NO_TARGET. 아직 아무것도 안 뗐다 ──
    if (reject == SkillReject::COUNT)
        reject = SelectTargets(zone, player, *def);

    if (reject != SkillReject::COUNT)
    {
        _gameMonitor._skill._rejected[static_cast<int>(reject)].Inc();
        return reject;
    }

    // ── 3) 시전 전 체력 — 피해 식의 바탕이라 떼기 전에 잡아 둔다 ──
    const int32_t hpBefore = player->_hp;

    // ── 4) 떼기 — 여기까지 왔으면 효과가 반드시 난다(모든 검사를 지났다) ──
    player->_hp -= hpCost;
    player->_skillReadyFrame[def->skillId] = frame + def->cooldownFrames;   // 성공했을 때만 — 거절은 쿨타임을 안 건다
    _gameMonitor._skill._cast[def->skillId].Inc();

    // ── 5) 시전 알림 — 방송과 시전자 체력 알림은 받는 쪽(게임 서버) ──
    _events.OnCast(zone, player, *def);

    // ── 6) 효과 ──
    ApplyEffect(zone, player, *def, hpBefore, frame);
    return SkillReject::COUNT;
}

SkillReject CSkillSystem::CheckCaster(const CPlayer* player, int32_t skillId, uint64_t frame, const SkillDef*& outDef, int32_t& outHpCost) const
{
    // SkillReject 의 순서 그대로 본다 — 여럿이 겹치면 먼저 걸린 하나로 답한다(열거 순서가 곧 확인 순서).
    //   번호 0 · 상한 밖 · 표에 없는 번호는 Find 가 다 nullptr 이다 — 그래서 아래 배열 색인은 늘 1~MAX_SKILL_ID 다.
    outDef = _tables.Skill().Find(skillId);
    if (outDef == nullptr)
        return SkillReject::BAD_SKILL;

    if (player->_isDead)
        return SkillReject::DEAD;

    if (frame < player->_skillReadyFrame[outDef->skillId])
        return SkillReject::COOLDOWN;

    // 체력 비용 — 현재 체력 × % 를 내림, 소모가 있는 스킬이면 최소 1
    outHpCost = 0;
    if (outDef->costHpPct > 0)
    {
        const int64_t cost = static_cast<int64_t>(player->_hp) * outDef->costHpPct / 100;
        outHpCost = (cost < 1) ? 1 : static_cast<int32_t>(cost);
    }

    // 자기 스킬로는 안 죽는다 — 떼고 나서 1 미만이면 거절(아무것도 안 뗀다)
    if (player->_hp - outHpCost < 1)
        return SkillReject::LOW_HP;

    return SkillReject::COUNT;   // 통과
}

SkillReject CSkillSystem::SelectTargets(CZone* zone, CPlayer* player, const SkillDef& def)
{
    _targets.clear();

    switch (def.target)
    {
    case SkillTarget::FRONT:
    {
        // 일반 공격과 같은 함수 — 후보만 나머지 목록(지금은 몬스터뿐). 사람은 스킬에 안 맞는다.
        //   시체만 있었는지(sawDead)는 지금 안 가른다 — 없으면 NO_TARGET 하나. 사유를 가를 날 쓸 값이다.
        bool sawDead = false;
        if (CCharacter* target = _combat.FindFrontTarget(zone, player, CCombatSystem::CandidateList::OTHERS, sawDead))
            _targets.push_back(target);
        break;
    }

    case SkillTarget::COUNT:
        break;   // 표 읽기가 막는다(SkillTable.cpp ⑶) — 여기 오지 않는다
    }

    return _targets.empty() ? SkillReject::NO_TARGET : SkillReject::COUNT;
}

void CSkillSystem::ApplyEffect(CZone* zone, CPlayer* player, const SkillDef& def, int32_t hpBefore, uint64_t frame)
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
            _combat.ApplyHit(zone, player, target, attackValue, frame);
        break;
    }

    case SkillEffect::COUNT:
        break;   // 표 읽기가 막는다 — 여기 오지 않는다
    }

    _targets.clear();   // 해제된 몬스터를 가리키는 칸을 남기지 않는다
}

#endif // USE_SKILL
