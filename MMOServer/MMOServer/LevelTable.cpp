#include "LevelTable.h"

#if USE_COMBAT   // 헤더와 같은 토글 — 까닭은 LevelTable.h

#include <cassert>

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"   // 줄·칸 쪼개기 — 아이템 표가 생기며 이 파일에서 공용 자리로 옮겼다

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

bool CLevelTable::Load(const std::string& path)
{
    _rows.clear();

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[LevelTable] 파일을 못 엽니다: {}", path);
        return false;
    }
    if (lines.empty())
    {
        SLOG_ERROR("[LevelTable] 값 줄이 하나도 없습니다: {}", path);
        return false;
    }

    _rows.resize(1);   // [0] 은 안 쓰는 자리

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 6)
        {
            SLOG_ERROR("[LevelTable] {}줄: 칸이 {}개입니다 — level,requiredExp,maxHp,attackPower,defense,maxMp 여섯이 필요합니다",
                       dl.fileLine, cell.size());
            return false;
        }

        int64_t level = 0, requiredExp = 0, maxHp = 0, attackPower = 0, defense = 0, maxMp = 0;
        if (!ParseInt(cell[0], level) || !ParseInt(cell[1], requiredExp) ||
            !ParseInt(cell[2], maxHp) || !ParseInt(cell[3], attackPower) || !ParseInt(cell[4], defense) ||
            !ParseInt(cell[5], maxMp))
        {
            SLOG_ERROR("[LevelTable] {}줄: 숫자가 아닌 칸이 있습니다 — \"{}\"", dl.fileLine, dl.text);
            return false;
        }

        // ⑴ 레벨은 1부터 빈칸 없이 이어져야 한다 — 색인이 곧 레벨이라 구멍이 배열 구멍이 된다
        const int64_t expected = static_cast<int64_t>(_rows.size());
        if (level != expected)
        {
            SLOG_ERROR("[LevelTable] {}줄: 레벨이 {} 여야 하는데 {} 입니다 — 1부터 빠짐없이 이어져야 합니다",
                       dl.fileLine, expected, level);
            return false;
        }

        // ⑵ 음수 금지 — 최대 마력은 0 도 된다(마력 없는 레벨)
        if (requiredExp < 0 || maxHp <= 0 || attackPower < 0 || defense < 0 || maxMp < 0)
        {
            SLOG_ERROR("[LevelTable] {}줄: 값이 범위를 벗어납니다 (requiredExp={}, maxHp={}, attackPower={}, defense={}, maxMp={})",
                       dl.fileLine, requiredExp, maxHp, attackPower, defense, maxMp);
            return false;
        }

        // ⑶ 1레벨의 필요 경험치는 0 이어야 한다 — 아니면 시작하자마자 레벨이 오르거나 안 오른다
        if (level == 1 && requiredExp != 0)
        {
            SLOG_ERROR("[LevelTable] {}줄: 1레벨의 requiredExp 는 0 이어야 합니다 (지금 {})",
                       dl.fileLine, requiredExp);
            return false;
        }

        // ⑷ 필요 경험치는 단조 증가여야 한다 — 레벨업이 while 루프라, 줄어들면 한 칸에서 멈추거나
        //    여러 칸을 건너뛴다. 같은 값도 막는다(경험치 0 으로 레벨이 두 칸 오르는 꼴이 된다)
        if (level > 1 && requiredExp <= _rows.back().requiredExp)
        {
            SLOG_ERROR("[LevelTable] {}줄: requiredExp 가 안 늘어납니다 — 앞 레벨 {} · 이 레벨 {}",
                       dl.fileLine, _rows.back().requiredExp, requiredExp);
            return false;
        }

        Row row;
        row.requiredExp = requiredExp;
        row.maxHp       = static_cast<int32_t>(maxHp);
        row.attackPower = static_cast<int32_t>(attackPower);
        row.defense     = static_cast<int32_t>(defense);
        row.maxMp       = static_cast<int32_t>(maxMp);
        _rows.push_back(row);
    }

    SLOG_INFO("[LevelTable] {} — 레벨 1~{} · 만렙 필요 경험치 {} · 만렙 체력 {} · 만렙 마력 {}",
              path, MaxLevel(), _rows.back().requiredExp, _rows.back().maxHp, _rows.back().maxMp);
    return true;
}

// 아래 다섯은 clamp 하지 않는다 — 헤더 [범위] 참조. 범위를 벗어난 호출은 부르는 쪽의 결함이다.
//   ※ 릴리즈(NDEBUG)에서는 단언이 사라져 곧장 범위 밖 접근이 된다. 불러오는 자리에서 한 번
//     내려 두는 것이 그 불변식을 세우는 유일한 장치이므로 그 줄을 지우지 말 것.
int64_t CLevelTable::RequiredExp(int32_t level) const
{
    assert(InRange(level));
    return _rows[static_cast<size_t>(level)].requiredExp;
}

int32_t CLevelTable::MaxHp(int32_t level) const
{
    assert(InRange(level));
    return _rows[static_cast<size_t>(level)].maxHp;
}

int32_t CLevelTable::AttackPower(int32_t level) const
{
    assert(InRange(level));
    return _rows[static_cast<size_t>(level)].attackPower;
}

int32_t CLevelTable::Defense(int32_t level) const
{
    assert(InRange(level));
    return _rows[static_cast<size_t>(level)].defense;
}

int32_t CLevelTable::MaxMp(int32_t level) const
{
    assert(InRange(level));
    return _rows[static_cast<size_t>(level)].maxMp;
}

#endif // USE_COMBAT
