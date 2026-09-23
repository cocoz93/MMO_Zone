#include "LevelTable.h"

#if USE_EXP

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <Common/ErrorLog.h>

namespace
{
    // 한 줄을 쉼표로 쪼갠다. 앞뒤 공백과 CR 을 턴다(CRLF 파일을 텍스트 모드로 읽어도 안전하게).
    //   ※ 표가 둘째로 늘어나는 날 이 둘을 공용 자리로 옮긴다. 지금은 쓰는 곳이 하나뿐이라 여기 둔다.
    void SplitCsv(const std::string& line, std::vector<std::string>& out)
    {
        out.clear();
        std::string cell;
        std::istringstream ss(line);
        while (std::getline(ss, cell, ','))
        {
            size_t b = cell.find_first_not_of(" \t\r\n");
            size_t e = cell.find_last_not_of(" \t\r\n");
            out.push_back(b == std::string::npos ? std::string() : cell.substr(b, e - b + 1));
        }
    }

    // 값으로 읽을 줄만 남긴다 — 빈 줄 · '#' 주석 · 헤더(첫 칸이 숫자가 아닌 줄) 를 버린다.
    //   반환하는 줄 번호는 파일 기준이라 오류 메시지가 사람이 볼 줄을 가리킨다.
    struct DataLine { int fileLine; std::string text; };

    bool ReadDataLines(const std::string& path, std::vector<DataLine>& out)
    {
        std::ifstream f(path);
        if (!f.is_open())
            return false;

        out.clear();
        std::string line;
        int no = 0;
        while (std::getline(f, line))
        {
            ++no;
            // UTF-8 BOM 이 붙어 있으면 첫 줄 앞 세 바이트를 턴다 — 편집기가 붙여 놓는 일이 있다
            if (no == 1 && line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3);

            size_t b = line.find_first_not_of(" \t\r\n");
            if (b == std::string::npos)      continue;   // 빈 줄
            if (line[b] == '#')              continue;   // 주석
            if (!std::isdigit(static_cast<unsigned char>(line[b])))
                continue;                                // 헤더 — 첫 칸이 숫자가 아니면 값 줄이 아니다

            out.push_back({ no, line });
        }
        return true;
    }

    // 문자열이 통째로 정수인가 (뒤에 쓰레기가 붙은 "12abc" 를 12 로 받지 않게)
    bool ParseInt(const std::string& s, int64_t& out)
    {
        if (s.empty())
            return false;
        char* end = nullptr;
        const long long v = std::strtoll(s.c_str(), &end, 10);
        if (end != s.c_str() + s.size())
            return false;
        out = static_cast<int64_t>(v);
        return true;
    }
}

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
        if (cell.size() < 4)
        {
            SLOG_ERROR("[LevelTable] {}줄: 칸이 {}개입니다 — level,requiredExp,maxHp,attackPower 넷이 필요합니다",
                       dl.fileLine, cell.size());
            return false;
        }

        int64_t level = 0, requiredExp = 0, maxHp = 0, attackPower = 0;
        if (!ParseInt(cell[0], level) || !ParseInt(cell[1], requiredExp) ||
            !ParseInt(cell[2], maxHp) || !ParseInt(cell[3], attackPower))
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

        // ⑵ 음수 금지
        if (requiredExp < 0 || maxHp <= 0 || attackPower < 0)
        {
            SLOG_ERROR("[LevelTable] {}줄: 값이 범위를 벗어납니다 (requiredExp={}, maxHp={}, attackPower={})",
                       dl.fileLine, requiredExp, maxHp, attackPower);
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
        _rows.push_back(row);
    }

    SLOG_INFO("[LevelTable] {} — 레벨 1~{} · 만렙 필요 경험치 {} · 만렙 체력 {}",
              path, MaxLevel(), _rows.back().requiredExp, _rows.back().maxHp);
    return true;
}

// 아래 셋은 clamp 하지 않는다 — 헤더 [범위] 참조. 범위를 벗어난 호출은 부르는 쪽의 결함이다.
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

#endif // USE_EXP
