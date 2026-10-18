#include "MonsterSkillTable.h"

#if USE_MONSTER && USE_SKILL

#include <cmath>

#include <Common/ErrorLog.h>

#include "Combat.h"         // ATTACK_RANGE — FRONT 스킬이 닿는 거리(일반 공격과 같다)
#include "GameDataCsv.h"
#include "MapTable.h"       // 되살리기 줄 — 시전자가 깔리는 맵을 훑는다
#include "MonsterAI.h"      // SIGHT_RANGE — MAX_DIST 가 시야와 같은지 아래 단언이 지킨다
#include "MonsterTable.h"
#include "SkillTable.h"
#include "SpawnRegionTable.h"   // 되살리기 줄 — 되살릴 몬스터의 구역이 시전자와 같은 맵에 있나

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;
using GameDataCsv::ParseFloat;

const std::vector<MonsterSkillRule> CMonsterSkillTable::EMPTY;

// 규칙이 볼 수 있는 가장 먼 거리는 AI 가 대상을 보는 시야다 — 시야 밖의 대상은 AI 가 아예 안 보므로 그보다 먼 maxDist 는 뜻이 없다
static_assert(CMonsterSkillTable::MAX_DIST <= MonsterAI::SIGHT_RANGE, "MonsterSkill 의 maxDist 상한이 몬스터 시야를 넘으면 안 된다");

bool CMonsterSkillTable::Load(const std::string& path, const CMonsterTable& monsters, const CSkillTable& skills,
                              const CMapTable& maps, const CSpawnRegionTable& spawns)
{
    _byMonster.clear();
    _count = 0;

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[MonsterSkillTable] 파일을 못 엽니다: {}", path);
        return false;
    }
    // 값 줄이 0 이어도 된다 — 스킬을 쓰는 몬스터가 없는 세계(클래스 머리 [값 줄 0 도 된다])

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 4)
        {
            SLOG_ERROR("[MonsterSkillTable] {}줄: 칸이 {}개입니다 — monsterId,skillId,maxDist,hpBelowPct 넷이 필요합니다", dl.fileLine, cell.size());
            return false;
        }

        int64_t monsterId = 0, skillId = 0, hpBelowPct = 0;
        double  maxDist = 0.0;
        if (!ParseInt(cell[0], monsterId) || !ParseInt(cell[1], skillId) || !ParseFloat(cell[2], maxDist) || !ParseInt(cell[3], hpBelowPct))
        {
            SLOG_ERROR("[MonsterSkillTable] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
            return false;
        }

        // ⑴ 몬스터 — 몬스터 표에 있어야 한다(표끼리 대조)
        if (monsterId < 1 || monsterId > CMonsterTable::MAX_MONSTER_ID || monsters.Find(static_cast<int32_t>(monsterId)) == nullptr)
        {
            SLOG_ERROR("[MonsterSkillTable] {}줄: 몬스터 {} 는 몬스터 표(MonsterTable.csv)에 없습니다", dl.fileLine, monsterId);
            return false;
        }

        // ⑵ 스킬 — 스킬 표에 있어야 한다(표끼리 대조)
        const SkillDef* def = skills.Find(static_cast<int32_t>(skillId));
        if (def == nullptr)
        {
            SLOG_ERROR("[MonsterSkillTable] {}줄: 스킬 {} 은 스킬 표(SkillTable.csv)에 없습니다", dl.fileLine, skillId);
            return false;
        }

        // ⑶ 몬스터 몫 — 사람 스킬(user=player)을 몬스터가 쓰면 CheckCaster 가 NOT_FOR_CASTER 로 막는다. 기동 때 먼저 걸리게 한다
        if (def->user != SkillUser::MONSTER)
        {
            SLOG_ERROR("[MonsterSkillTable] {}줄: 스킬 {}({}) 은 user=monster 가 아닙니다 — 몬스터 표에 둘 수 없습니다", dl.fileLine, skillId, def->name);
            return false;
        }

        // ⑷ 비용 — 몬스터는 마력이 0 이라 마력이 드는 스킬은 늘 LOW_MP 이고, 체력을 떼는 스킬은 몬스터가 스스로 깎는다
        if (def->costMp != 0 || def->costHpPct != 0)
        {
            SLOG_ERROR("[MonsterSkillTable] {}줄: 스킬 {}({}) 은 비용이 있습니다(costMp {} · costHpPct {}) — 몬스터 스킬은 둘 다 0 이어야 합니다",
                       dl.fileLine, skillId, def->name, def->costMp, def->costHpPct);
            return false;
        }

        // ⑸ 거리 — 0 초과 MAX_DIST 이하, 그리고 스킬이 닿는 거리 안. 안 닿는 거리에서 시도하면 거절(NO_TARGET)만 쌓인다
        if (!(maxDist > 0.0) || maxDist > static_cast<double>(MAX_DIST))
        {
            SLOG_ERROR("[MonsterSkillTable] {}줄: maxDist {} 는 0 보다 크고 {} 이하여야 합니다(몬스터 시야)", dl.fileLine, cell[2], MAX_DIST);
            return false;
        }
        switch (def->target)
        {
        case SkillTarget::FRONT:
            if (maxDist > static_cast<double>(Combat::ATTACK_RANGE))
            {
                SLOG_ERROR("[MonsterSkillTable] {}줄: 스킬 {} 은 front 라 maxDist 가 일반 공격 사거리 {} 이하여야 합니다 — {}",
                           dl.fileLine, skillId, Combat::ATTACK_RANGE, cell[2]);
                return false;
            }
            break;
        case SkillTarget::SHAPE:
            if (maxDist > static_cast<double>(def->shapeReach))
            {
                // 알려 주는 값은 소수 둘째 자리 **내림** — 반올림하면 닿는 거리 2.9155 가 「2.92 이하」 로 보여 2.92 를 적은 사람이 왜 막혔는지 모른다
                const double shownReach = std::floor(static_cast<double>(def->shapeReach) * 100.0) / 100.0;
                SLOG_ERROR("[MonsterSkillTable] {}줄: 스킬 {} 은 shape 라 maxDist 가 칸이 닿는 거리 {:.2f} 이하여야 합니다 — {}",
                           dl.fileLine, skillId, shownReach, cell[2]);
                return false;
            }
            break;
        case SkillTarget::SELF:   // 대상을 안 찾는 스킬 — 「대상이 시야 안에 있을 때만 쓴다」 뜻이라 위 시야 상한만 본다
        case SkillTarget::COUNT:
            break;
        }

        // ⑹ 체력 조건 — 1~100(%). 0 이면 영영 못 쓰고 100 을 넘으면 뜻이 없다
        if (hpBelowPct < 1 || hpBelowPct > 100)
        {
            SLOG_ERROR("[MonsterSkillTable] {}줄: hpBelowPct {} 는 1~100 이어야 합니다 (100 = 늘)", dl.fileLine, hpBelowPct);
            return false;
        }

        // ⑹-2 되살리기 — 되살릴 몬스터(스킬 표 power)가 있고 · 보스가 아니고 · 시전자가 깔리는 맵마다 그 몬스터 구역도 있어야 한다(헤더 [읽을 때 대조])
        int32_t reviveMonsterId = 0;
        if (def->effect == SkillEffect::REVIVE)
        {
            const MonsterDef* revived = monsters.Find(def->power);
            if (revived == nullptr)
            {
                SLOG_ERROR("[MonsterSkillTable] {}줄: 스킬 {}({}) 이 되살릴 몬스터 {} 는 몬스터 표(MonsterTable.csv)에 없습니다",
                           dl.fileLine, skillId, def->name, def->power);
                return false;
            }
            if (revived->boss)
            {
                SLOG_ERROR("[MonsterSkillTable] {}줄: 스킬 {}({}) 이 되살릴 몬스터 {}({}) 는 보스입니다 — 보스를 되살리면 방 클리어가 두 번 옵니다",
                           dl.fileLine, skillId, def->name, def->power, revived->name);
                return false;
            }
            for (const MapDef& m : maps.All())
            {
                bool casterHere = false;
                bool revivedHere = false;
                for (const SpawnRegion& r : spawns.RegionsOf(m.mapId))
                {
                    casterHere  = casterHere  || (r.monsterId == monsterId);
                    revivedHere = revivedHere || (r.monsterId == def->power);
                }
                if (casterHere && !revivedHere)
                {
                    SLOG_ERROR("[MonsterSkillTable] {}줄: 맵 {} 에 몬스터 {} 는 깔리는데 되살릴 몬스터 {}({}) 의 구역이 없습니다 — 되살릴 빈 점이 없어 영영 안 쓰입니다",
                               dl.fileLine, m.mapId, monsterId, def->power, revived->name);
                    return false;
                }
            }
            reviveMonsterId = def->power;
        }

        // ⑺ 겹침 · 개수 — 같은 (몬스터, 스킬) 줄이 둘이면 뒤 줄이 영영 안 쓰이고, 규칙이 많으면 한 번의 판단이 길어진다
        if (static_cast<int64_t>(_byMonster.size()) <= monsterId)
            _byMonster.resize(static_cast<size_t>(monsterId) + 1);
        std::vector<MonsterSkillRule>& rules = _byMonster[static_cast<size_t>(monsterId)];
        for (const MonsterSkillRule& r : rules)
        {
            if (r.skillId == skillId)
            {
                SLOG_ERROR("[MonsterSkillTable] {}줄: 몬스터 {} · 스킬 {} 줄이 겹칩니다({}줄과)", dl.fileLine, monsterId, skillId, r.fileLine);
                return false;
            }
        }
        if (static_cast<int32_t>(rules.size()) >= MAX_RULES_PER_MONSTER)
        {
            SLOG_ERROR("[MonsterSkillTable] {}줄: 몬스터 {} 의 규칙이 {} 개를 넘습니다", dl.fileLine, monsterId, MAX_RULES_PER_MONSTER);
            return false;
        }

        MonsterSkillRule rule;
        rule.skillId    = static_cast<int32_t>(skillId);
        rule.maxDist    = static_cast<float>(maxDist);
        rule.hpBelowPct = static_cast<int32_t>(hpBelowPct);
        rule.reviveMonsterId = reviveMonsterId;
        rule.fileLine   = dl.fileLine;
        rules.push_back(rule);
        ++_count;
    }

    SLOG_INFO("[MonsterSkillTable] {} — 규칙 {}개", path, _count);
    return true;
}

#endif // USE_MONSTER && USE_SKILL
