#include "SkillTable.h"

#if USE_SKILL

#include <utility>   // std::move — 칸 목록을 표 행으로 넘긴다

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"
#include "LevelTable.h"   // costMp 상한 — 닿을 수 있는 최대 마력(USE_SKILL 은 USE_COMBAT 을 요구하므로 레벨 표가 늘 있다)

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

namespace
{
    // 파일 글자 — SkillTarget · SkillEffect 순서 그대로. 줄 수가 모자라면 아래 단언이 컴파일을 막는다.
    constexpr const char* TARGET_NAMES[] = { "front", "shape", "self" };
    constexpr const char* EFFECT_NAMES[] = { "damage", "buff_attack_mul", "damage_attack" };   // buff_<종류> — 종류 글자는 GameMonitor.h BuffKindName 과 같다
    constexpr const char* USER_NAMES[]   = { "player", "monster" };
    static_assert(sizeof(TARGET_NAMES) / sizeof(TARGET_NAMES[0]) == static_cast<size_t>(SkillTarget::COUNT),
                  "TARGET_NAMES 는 SkillTarget 마다 한 줄이어야 한다");
    static_assert(sizeof(EFFECT_NAMES) / sizeof(EFFECT_NAMES[0]) == static_cast<size_t>(SkillEffect::COUNT),
                  "EFFECT_NAMES 는 SkillEffect 마다 한 줄이어야 한다");
    static_assert(sizeof(USER_NAMES) / sizeof(USER_NAMES[0]) == static_cast<size_t>(SkillUser::COUNT),
                  "USER_NAMES 는 SkillUser 마다 한 줄이어야 한다");

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

    // shape 칸 글자 → 칸 목록. 「앞:옆」 을 ';' 로 이은 것(예: 1:0;2:-1;2:0;2:1). 틀리면 false 와 까닭.
    //   앞 1~MAX_SHAPE_REACH(앞쪽만 — 시전자 자리·뒤는 칸이 아니다) · 옆 ±MAX_SHAPE_REACH · 겹침 금지 · 1~MAX_SHAPE_CELLS 칸.
    bool ParseShape(const std::string& text, std::vector<ShapeCell>& out, std::string& why)
    {
        out.clear();
        size_t begin = 0;
        while (begin <= text.size())
        {
            const size_t end = text.find(';', begin);
            const std::string item = text.substr(begin, (end == std::string::npos ? text.size() : end) - begin);
            const size_t colon = item.find(':');
            int64_t forward = 0, side = 0;
            if (colon == std::string::npos || !ParseInt(item.substr(0, colon), forward) || !ParseInt(item.substr(colon + 1), side))
            {
                why = "칸 \"" + item + "\" 은 「앞:옆」 정수 둘이어야 합니다";
                return false;
            }
            if (forward < 1 || forward > MAX_SHAPE_REACH || side < -MAX_SHAPE_REACH || side > MAX_SHAPE_REACH)
            {
                why = "칸 \"" + item + "\" — 앞은 1~" + std::to_string(MAX_SHAPE_REACH) + ", 옆은 ±" + std::to_string(MAX_SHAPE_REACH) + " 여야 합니다";
                return false;
            }
            for (const ShapeCell& c : out)
            {
                if (c.forward == forward && c.side == side)
                {
                    why = "칸 \"" + item + "\" 이 겹칩니다";
                    return false;
                }
            }
            if (static_cast<int32_t>(out.size()) >= MAX_SHAPE_CELLS)
            {
                why = "칸이 " + std::to_string(MAX_SHAPE_CELLS) + " 개를 넘습니다";
                return false;
            }
            out.push_back(ShapeCell{ static_cast<int8_t>(forward), static_cast<int8_t>(side) });

            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        return true;
    }
}

bool CSkillTable::Load(const std::string& path, const CLevelTable& levels)
{
    _rows.clear();
    _count = 0;

    // 사람이 닿을 수 있는 가장 큰 최대 마력 — costMp 가 이보다 크면 어느 레벨에서도 못 쓴다(아래 ⑺).
    //   만렙 값이 아니라 전 레벨의 최댓값이다 — 표가 레벨마다 늘지 않아도 맞게.
    int32_t highestMaxMp = 0;
    for (int32_t level = 1; level <= levels.MaxLevel(); ++level)
    {
        const int32_t mp = levels.MaxMp(level);
        highestMaxMp = (mp > highestMaxMp) ? mp : highestMaxMp;   // std::max 는 윈도 헤더의 max 매크로에 먹힌다
    }

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
        if (cell.size() < 8)
        {
            SLOG_ERROR("[SkillTable] {}줄: 칸이 {}개입니다 — skillId,name,target,effect,costHpPct,power,cooldownMs,costMp 여덟(+ shape, durationMs, user)이 필요합니다",
                       dl.fileLine, cell.size());
            return false;
        }
        // 아홉째 shape 칸은 shape 대상만, 열째 durationMs 칸은 지속 효과만, 열한째 user 칸은 몬스터 스킬만 쓴다. 끝의 빈 칸은 SplitCsv 가 안 담으므로
        //   front 줄은 여덟, 어검술 줄은 아홉, 이중공격 줄은 열, 몬스터 스킬 줄은 열한 칸으로 온다(여덟만 요구하고 나머지는 비면 빈 것으로 본다)
        const std::string shapeText    = (cell.size() > 8) ? cell[8] : std::string();
        const std::string durationText = (cell.size() > 9) ? cell[9] : std::string();
        const std::string userText     = (cell.size() > 10) ? cell[10] : std::string();

        int64_t skillId = 0, costHpPct = 0, power = 0, cooldownMs = 0, costMp = 0, durationMs = 0;
        if (!ParseInt(cell[0], skillId) || !ParseInt(cell[4], costHpPct) || !ParseInt(cell[5], power) || !ParseInt(cell[6], cooldownMs) ||
            !ParseInt(cell[7], costMp) || (!durationText.empty() && !ParseInt(durationText, durationMs)))
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
            SLOG_ERROR("[SkillTable] {}줄: 모르는 target \"{}\" — front · shape · self 중 하나여야 합니다 (스킬 {})", dl.fileLine, cell[2], skillId);
            return false;
        }
        const SkillEffect effect = ParseName<SkillEffect>(cell[3], EFFECT_NAMES);
        if (effect == SkillEffect::COUNT)
        {
            SLOG_ERROR("[SkillTable] {}줄: 모르는 effect \"{}\" — damage · buff_attack_mul · damage_attack 중 하나여야 합니다 (스킬 {})", dl.fileLine, cell[3], skillId);
            return false;
        }

        // ⑶-2 쓰는 쪽 — 빈 칸은 사람(옛 줄이 그대로 사람 스킬). 모르는 글자면 누가 쓰는지가 안 정해진다
        SkillUser user = SkillUser::PLAYER;
        if (!userText.empty())
        {
            user = ParseName<SkillUser>(userText, USER_NAMES);
            if (user == SkillUser::COUNT)
            {
                SLOG_ERROR("[SkillTable] {}줄: 모르는 user \"{}\" — player · monster 중 하나이거나 비워야 합니다 (스킬 {})", dl.fileLine, userText, skillId);
                return false;
            }
        }

        // ⑷ 체력 비용 — 0~99. 100 이면 떼고 나서 늘 0 이라 영영 못 쓰는 스킬이 된다(LOW_HP — 자기 스킬로는 안 죽는다)
        if (costHpPct < 0 || costHpPct > 99)
        {
            SLOG_ERROR("[SkillTable] {}줄: costHpPct {} 는 0~99 여야 합니다 (스킬 {})", dl.fileLine, costHpPct, skillId);
            return false;
        }

        // ⑸ 위력·지속·대상 짝 — 효과마다 뜻이 다르다. 안 쓰이는 칸(피해의 durationMs)은 0 이어야 한다(front 의 shape 와 같은 규칙 — 뜻을 가진 척하지 않게)
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
            if (durationMs != 0)
            {
                SLOG_ERROR("[SkillTable] {}줄: damage 는 durationMs 를 비워야 합니다 — {} (스킬 {})", dl.fileLine, durationMs, skillId);
                return false;
            }
            // 자기를 때리는 스킬은 없다 — 「자기 스킬로는 안 죽는다」(LOW_HP) 규칙의 연장
            if (target == SkillTarget::SELF)
            {
                SLOG_ERROR("[SkillTable] {}줄: damage 는 target self 와 짝지을 수 없습니다 (스킬 {})", dl.fileLine, skillId);
                return false;
            }
            break;

        case SkillEffect::DAMAGE_ATK:
            // 위력 = 시전자 공격력의 % — 1~MAX_POWER(100 = 일반 공격과 같은 공격값). DAMAGE 와 달리 체력 비용과 위력을 견주지 않는다
            //   (공격값이 체력에서 안 나오므로 「소모보다 약하게 때림」이 생기지 않는다 — 비용이 있는 DAMAGE_ATK 는 사람 스킬이 쓰는 날을 위해 막지 않는다)
            if (power < 1 || power > MAX_POWER)
            {
                SLOG_ERROR("[SkillTable] {}줄: damage_attack 의 power {} 는 1~{} 여야 합니다 (스킬 {})", dl.fileLine, power, MAX_POWER, skillId);
                return false;
            }
            if (durationMs != 0)
            {
                SLOG_ERROR("[SkillTable] {}줄: damage_attack 은 durationMs 를 비워야 합니다 — {} (스킬 {})", dl.fileLine, durationMs, skillId);
                return false;
            }
            if (target == SkillTarget::SELF)
            {
                SLOG_ERROR("[SkillTable] {}줄: damage_attack 은 target self 와 짝지을 수 없습니다 (스킬 {})", dl.fileLine, skillId);
                return false;
            }
            break;

        case SkillEffect::BUFF_ATTACK_MUL:
            // 배수 2~MAX_BUFF_MUL(1 은 아무 일도 안 한다) · 지속 1ms~상한(0 이면 비어 있는 칸과 구별이 안 된다 — BuffSlot) · 대상은 자기 자신만
            if (power < 2 || power > MAX_BUFF_MUL)
            {
                SLOG_ERROR("[SkillTable] {}줄: buff_attack_mul 의 power(배수) {} 는 2~{} 여야 합니다 (스킬 {})", dl.fileLine, power, MAX_BUFF_MUL, skillId);
                return false;
            }
            if (durationMs < 1 || durationMs > MAX_COOLDOWN_MS)
            {
                SLOG_ERROR("[SkillTable] {}줄: buff_attack_mul 의 durationMs {} 는 1~{} 여야 합니다 (스킬 {})", dl.fileLine, durationMs, MAX_COOLDOWN_MS, skillId);
                return false;
            }
            if (target != SkillTarget::SELF)
            {
                SLOG_ERROR("[SkillTable] {}줄: buff_attack_mul 은 target self 여야 합니다 — \"{}\" (스킬 {})", dl.fileLine, cell[2], skillId);
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

        // ⑺ 마력 비용 — 0~닿을 수 있는 최대 마력. 넘으면 어느 레벨에서도 못 쓰는 스킬이 된다(LOW_MP — ⑷ 의 99 와 같은 까닭)
        if (costMp < 0 || costMp > highestMaxMp)
        {
            SLOG_ERROR("[SkillTable] {}줄: costMp {} 는 0~{}(레벨 표의 가장 큰 maxMp) 여야 합니다 (스킬 {})",
                       dl.fileLine, costMp, highestMaxMp, skillId);
            return false;
        }

        // ⑻ 칸 목록 — shape 대상은 반드시 있고, 다른 대상은 비어 있어야 한다(안 쓰이는 값이 뜻을 가진 척하지 않게)
        std::vector<ShapeCell> shape;
        switch (target)
        {
        case SkillTarget::FRONT:
        case SkillTarget::SELF:
            if (!shapeText.empty())
            {
                SLOG_ERROR("[SkillTable] {}줄: target {} 는 shape 칸을 비워야 합니다 — \"{}\" (스킬 {})", dl.fileLine, cell[2], shapeText, skillId);
                return false;
            }
            break;

        case SkillTarget::SHAPE:
        {
            std::string why;
            if (shapeText.empty())
                why = "칸이 비어 있습니다";
            if (!why.empty() || !ParseShape(shapeText, shape, why))
            {
                SLOG_ERROR("[SkillTable] {}줄: shape {} (스킬 {})", dl.fileLine, why, skillId);
                return false;
            }
            break;
        }

        case SkillTarget::COUNT:
            return false;   // 위 ⑶ 이 이미 걸렀다 — 여기 오지 않는다
        }

        if (static_cast<int64_t>(_rows.size()) <= skillId)
            _rows.resize(static_cast<size_t>(skillId) + 1);

        SkillDef& d = _rows[static_cast<size_t>(skillId)];
        d.skillId        = static_cast<int32_t>(skillId);
        d.name           = cell[1];
        d.target         = target;
        d.effect         = effect;
        d.user           = user;
        d.costHpPct      = static_cast<int32_t>(costHpPct);
        d.costMp         = static_cast<int32_t>(costMp);
        d.power          = static_cast<int32_t>(power);
        d.cooldownMs     = static_cast<int32_t>(cooldownMs);
        // 올림 — 480ms 면 12틱(일반 공격 쿨타임과 같다). 내림이면 틱 주기를 바꿀 때 쿨타임이 짧아질 수 있다
        d.cooldownFrames = static_cast<uint64_t>((cooldownMs + FRAME_INTERVAL_MS - 1) / FRAME_INTERVAL_MS);
        d.durationMs     = static_cast<int32_t>(durationMs);
        d.durationFrames = static_cast<uint64_t>((durationMs + FRAME_INTERVAL_MS - 1) / FRAME_INTERVAL_MS);   // 같은 올림 — 30000 → 750
        d.shapeReach     = ShapeReach(shape);
        d.shape          = std::move(shape);
        ++_count;
    }

    SLOG_INFO("[SkillTable] {} — 스킬 {}개", path, _count);
    return true;
}

#endif // USE_SKILL
