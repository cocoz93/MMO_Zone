#include "LookTable.h"

#include <Common/ErrorLog.h>
#include <Data/GameDataCsv.h>
#include <vector>

namespace
{
    // gender,part,id,sprite,head — head 는 피부 줄에만 있다(SplitCsv 는 끝의 빈 칸을 안 돌려준다)
    constexpr int MIN_COLUMNS = 4;
    constexpr int MAX_COLUMNS = 5;

    bool ParsePart(const std::string& text, CLookTable::Part& out)
    {
        if (text == "skin")   { out = CLookTable::Part::Skin;   return true; }
        if (text == "hair")   { out = CLookTable::Part::Hair;   return true; }
        if (text == "top")    { out = CLookTable::Part::Top;    return true; }
        if (text == "bottom") { out = CLookTable::Part::Bottom; return true; }
        return false;
    }
}

bool CLookTable::Load(const std::string& path)
{
    std::vector<GameDataCsv::DataLine> lines;
    if (!GameDataCsv::ReadDataLines(path, lines))
    {
        SLOG_ERROR("[LookTable] 파일을 못 읽음: {}", path);
        return false;
    }

    std::vector<std::string> cells;
    for (const GameDataCsv::DataLine& line : lines)
    {
        GameDataCsv::SplitCsv(line.text, cells);
        if (static_cast<int>(cells.size()) < MIN_COLUMNS || static_cast<int>(cells.size()) > MAX_COLUMNS)
        {
            SLOG_ERROR("[LookTable] {}행: 칸이 {}~{}개여야 함(지금 {}개)", line.fileLine, MIN_COLUMNS, MAX_COLUMNS, cells.size());
            return false;
        }

        int64_t gender = 0;
        int64_t id = 0;
        Part part = Part::Skin;
        if (!GameDataCsv::ParseInt(cells[0], gender) || gender < CharacterLookBlob::GENDER_MALE || gender > CharacterLookBlob::GENDER_FEMALE)
        {
            SLOG_ERROR("[LookTable] {}행: 성별은 1 또는 2: '{}'", line.fileLine, cells[0]);
            return false;
        }
        if (!ParsePart(cells[1], part))
        {
            SLOG_ERROR("[LookTable] {}행: 모르는 부위 '{}'", line.fileLine, cells[1]);
            return false;
        }
        if (!GameDataCsv::ParseInt(cells[2], id) || id < 1 || id >= ID_SLOTS)
        {
            SLOG_ERROR("[LookTable] {}행: 번호는 1~{}: '{}'", line.fileLine, ID_SLOTS - 1, cells[2]);
            return false;
        }
        if (cells[3].empty())
        {
            SLOG_ERROR("[LookTable] {}행: 그림 이름이 비었음", line.fileLine);
            return false;
        }
        // 피부만 몸과 머리 그림 두 장이다
        const bool hasHead = static_cast<int>(cells.size()) == MAX_COLUMNS && !cells[4].empty();
        if ((part == Part::Skin) != hasHead)
        {
            SLOG_ERROR("[LookTable] {}행: 머리 그림 칸은 피부 줄에만 적음", line.fileLine);
            return false;
        }

        bool& slot = _allowed[gender][static_cast<int>(part)][id];
        if (slot)
        {
            SLOG_ERROR("[LookTable] {}행: 같은 번호가 두 번 나옴", line.fileLine);
            return false;
        }
        slot = true;
    }

    for (uint8_t gender = CharacterLookBlob::GENDER_MALE; gender <= CharacterLookBlob::GENDER_FEMALE; ++gender)
    {
        for (int part = 0; part < static_cast<int>(Part::COUNT); ++part)
        {
            if (Count(gender, static_cast<Part>(part)) == 0)
            {
                SLOG_ERROR("[LookTable] 성별 {} 부위 {} 줄이 없음", gender, part);
                return false;
            }
        }
    }
    return true;
}

bool CLookTable::Has(uint8_t gender, Part part, uint8_t id) const
{
    if (gender >= GENDER_SLOTS)
        return false;
    return _allowed[gender][static_cast<int>(part)][id];
}

bool CLookTable::IsAllowed(const CharacterLookBlob::Look& look) const
{
    return Has(look.gender, Part::Skin, look.skin)
        && Has(look.gender, Part::Hair, look.hair)
        && Has(look.gender, Part::Top, look.top)
        && Has(look.gender, Part::Bottom, look.bottom);
}

int CLookTable::Count(uint8_t gender, Part part) const
{
    if (gender >= GENDER_SLOTS)
        return 0;
    int count = 0;
    for (const bool allowed : _allowed[gender][static_cast<int>(part)])
        count += allowed ? 1 : 0;
    return count;
}
