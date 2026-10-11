#include "SkillTable.h"

#if USE_SKILL

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

namespace
{
    // 파일 글자 — SkillTarget · SkillEffect 순서 그대로. 줄 수가 모자라면 아래 단언이 컴파일을 막는다.
    constexpr const char* TARGET_NAMES[] = { "front" };
    constexpr const char* EFFECT_NAMES[] = { "damage" };
    static_assert(sizeof(TARGET_NAMES) / sizeof(TARGET_NAMES[0]) == static_cast<size_t>(SkillTarget::COUNT),
                  "TARGET_NAMES 는 SkillTarget 마다 한 줄이어야 한다");
    static_assert(sizeof(EFFECT_NAMES) / sizeof(EFFECT_NAMES[0]) == static_cast<size_t>(SkillEffect::COUNT),
                  "EFFECT_NAMES 는 SkillEffect 마다 한 줄이어야 한다");

    // 글자 → 종류. 모르는 글자면 COUNT
    template <class E, size_t N>
    E ParseName(const std::string& s, const char* const (&names)[N])
    {
        for (size_t i = 0; i < N; ++i)
        {
            if (s == names[i])
                return static_cast<E>(i);
        }
        return E::COUNT;
    }
}

bool CSkillTable::Load(const std::string& path)
{
    _rows.clear();
    _count = 0;

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[SkillTable] 파일을 못 엽니다: {}", path);
        return false;
    }
    if (lines.empty())
    {
        SLOG_ERROR("[SkillTable] 값 줄이 하나도 없습니다: {}", path);
        return false;
    }

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 7)
        {
            SLOG_ERROR("[SkillTable] {}줄: 칸이 {}개입니다 — skillId,name,target,effect,costHpPct,power,cooldownMs 일곱이 필요합니다",
                       dl.fileLine, cell.size());
            return false;
        }

        int64_t skillId = 0, costHpPct = 0, power = 0, cooldownMs = 0;
        if (!ParseInt(cell[0], skillId) || !ParseInt(cell[4], costHpPct) || !ParseInt(cell[5], power) || !ParseInt(cell[6], cooldownMs))
        {
            SLOG_ERROR("[SkillTable] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
            return false;
        }

        // ⑴ 번호 — 1~MAX_SKILL_ID · 중복 없음. 번호가 곧 색인이고 캐릭터마다의 쿨타임 배열 칸이다
        if (skillId <= 0 || skillId > MAX_SKILL_ID)
        {
            SLOG_ERROR("[SkillTable] {}줄: 스킬 번호 {} 는 1~{} 여야 합니다", dl.fileLine, skillId, MAX_SKILL_ID);
            return false;
        }
        if (skillId < static_cast<int64_t>(_rows.size()) && _rows[static_cast<size_t>(skillId)].skillId != 0)
        {
            SLOG_ERROR("[SkillTable] {}줄: 스킬 번호 {} 가 겹칩니다", dl.fileLine, skillId);
            return false;
        }

        // ⑵ 이름 — 웹이 그릴 이름이 없다
        if (cell[1].empty())
        {
            SLOG_ERROR("[SkillTable] {}줄: 이름이 비어 있습니다 (스킬 {})", dl.fileLine, skillId);
            return false;
        }

        // ⑶ 대상 종류 · 효과 종류 — 모르는 글자면 무엇을 할지가 안 정해진다
        const SkillTarget target = ParseName<SkillTarget>(cell[2], TARGET_NAMES);
        if (target == SkillTarget::COUNT)
        {
            SLOG_ERROR("[SkillTable] {}줄: 모르는 target \"{}\" — front 여야 합니다 (스킬 {})", dl.fileLine, cell[2], skillId);
            return false;
        }
        const SkillEffect effect = ParseName<SkillEffect>(cell[3], EFFECT_NAMES);
        if (effect == SkillEffect::COUNT)
        {
            SLOG_ERROR("[SkillTable] {}줄: 모르는 effect \"{}\" — damage 여야 합니다 (스킬 {})", dl.fileLine, cell[3], skillId);
            return false;
        }

        // ⑷ 체력 비용 — 0~99. 100 이면 떼고 나서 늘 0 이라 영영 못 쓰는 스킬이 된다(LOW_HP — 자기 스킬로는 안 죽는다)
        if (costHpPct < 0 || costHpPct > 99)
        {
            SLOG_ERROR("[SkillTable] {}줄: costHpPct {} 는 0~99 여야 합니다 (스킬 {})", dl.fileLine, costHpPct, skillId);
            return false;
        }

        // ⑸ 위력 — 효과마다 뜻이 다르다
        switch (effect)
        {
        case SkillEffect::DAMAGE:
            // 피해가 소모보다 작으면 안 된다 — 체력을 떼고 그보다 약하게 때리는 스킬은 기획상 이상하다(10-01)
            if (power < 1 || power > MAX_POWER || power < costHpPct)
            {
                SLOG_ERROR("[SkillTable] {}줄: damage 의 power {} 는 1~{} 이고 costHpPct({}) 이상이어야 합니다 (스킬 {})",
                           dl.fileLine, power, MAX_POWER, costHpPct, skillId);
                return false;
            }
            break;

        case SkillEffect::COUNT:
            return false;   // 위 ⑶ 이 이미 걸렀다 — 여기 오지 않는다
        }

        // ⑹ 쿨타임 — 음수 금지 · 상한(오타)
        if (cooldownMs < 0 || cooldownMs > MAX_COOLDOWN_MS)
        {
            SLOG_ERROR("[SkillTable] {}줄: cooldownMs {} 는 0~{} 여야 합니다 (스킬 {})", dl.fileLine, cooldownMs, MAX_COOLDOWN_MS, skillId);
            return false;
        }

        if (static_cast<int64_t>(_rows.size()) <= skillId)
            _rows.resize(static_cast<size_t>(skillId) + 1);

        SkillDef& d = _rows[static_cast<size_t>(skillId)];
        d.skillId        = static_cast<int32_t>(skillId);
        d.name           = cell[1];
        d.target         = target;
        d.effect         = effect;
        d.costHpPct      = static_cast<int32_t>(costHpPct);
        d.power          = static_cast<int32_t>(power);
        d.cooldownMs     = static_cast<int32_t>(cooldownMs);
        // 올림 — 480ms 면 12틱(일반 공격 쿨타임과 같다). 내림이면 틱 주기를 바꿀 때 쿨타임이 짧아질 수 있다
        d.cooldownFrames = static_cast<uint64_t>((cooldownMs + FRAME_INTERVAL_MS - 1) / FRAME_INTERVAL_MS);
        ++_count;
    }

    SLOG_INFO("[SkillTable] {} — 스킬 {}개", path, _count);
    return true;
}

#endif // USE_SKILL
