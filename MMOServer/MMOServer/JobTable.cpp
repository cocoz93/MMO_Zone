#include "JobTable.h"

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

bool CJobTable::Load(const std::string& path)
{
    _rows.clear();

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[JobTable] 파일을 못 엽니다: {}", path);
        return false;
    }

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 2)
        {
            SLOG_ERROR("[JobTable] {}줄: 칸이 {}개입니다 — jobId,name 둘이 필요합니다", dl.fileLine, cell.size());
            return false;
        }
        int64_t jobId = 0;
        if (!ParseInt(cell[0], jobId) || jobId < 1 || jobId > MAX_JOB_ID)
        {
            SLOG_ERROR("[JobTable] {}줄: 직업 번호 \"{}\" 는 1~{} 여야 합니다 (0 = 아직 안 고름이라 줄이 될 수 없다)", dl.fileLine, cell[0], MAX_JOB_ID);
            return false;
        }
        if (Has(static_cast<int32_t>(jobId)))
        {
            SLOG_ERROR("[JobTable] {}줄: 직업 번호 {} 가 겹칩니다", dl.fileLine, jobId);
            return false;
        }
        if (cell[1].empty())
        {
            SLOG_ERROR("[JobTable] {}줄: 이름이 비어 있습니다 (직업 {})", dl.fileLine, jobId);
            return false;
        }
        _rows.push_back(JobDef{ static_cast<int32_t>(jobId), cell[1] });
    }

    if (!Has(BASE_JOB))
    {
        SLOG_ERROR("[JobTable] 직업 {} 줄이 없습니다 — 고르기 전 캐릭터의 스탯이 이 직업 줄을 쓴다: {}", BASE_JOB, path);
        return false;
    }
    SLOG_INFO("[JobTable] {} — 직업 {}개", path, _rows.size());
    return true;
}

bool CJobTable::Has(int32_t jobId) const
{
    for (const JobDef& d : _rows)
    {
        if (d.jobId == jobId)
            return true;
    }
    return false;
}
