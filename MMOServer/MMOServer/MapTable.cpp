#include "MapTable.h"

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

bool CMapTable::Load(const std::string& path)
{
    _rows.clear();

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[MapTable] 파일을 못 엽니다: {}", path);
        return false;
    }
    if (lines.empty())
    {
        SLOG_ERROR("[MapTable] 값 줄이 하나도 없습니다 — 맵이 하나는 있어야 합니다: {}", path);
        return false;
    }

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 3)
        {
            SLOG_ERROR("[MapTable] {}줄: 칸이 {}개입니다 — mapId,width,height 셋이 필요합니다", dl.fileLine, cell.size());
            return false;
        }

        int64_t mapId = 0, width = 0, height = 0;
        if (!ParseInt(cell[0], mapId) || !ParseInt(cell[1], width) || !ParseInt(cell[2], height))
        {
            SLOG_ERROR("[MapTable] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
            return false;
        }

        // ⑴ 번호 — 0 이상 · 상한 이하 · 중복 없음
        if (mapId < 0 || mapId > MAX_MAP_ID)
        {
            SLOG_ERROR("[MapTable] {}줄: 맵 번호 {} 는 0~{} 여야 합니다", dl.fileLine, mapId, MAX_MAP_ID);
            return false;
        }
        if (Find(static_cast<int32_t>(mapId)) != nullptr)
        {
            SLOG_ERROR("[MapTable] {}줄: 맵 번호 {} 가 겹칩니다", dl.fileLine, mapId);
            return false;
        }

        // ⑵ 크기 — 0 이면 설 자리가 없다
        if (width < 1 || width > MAX_SIZE || height < 1 || height > MAX_SIZE)
        {
            SLOG_ERROR("[MapTable] {}줄: 크기 {}×{} — 폭·높이는 1~{} 여야 합니다 (맵 {})",
                       dl.fileLine, width, height, MAX_SIZE, mapId);
            return false;
        }

        MapDef d;
        d.mapId    = static_cast<int32_t>(mapId);
        d.width    = static_cast<int32_t>(width);
        d.height   = static_cast<int32_t>(height);
        d.fileLine = dl.fileLine;
        _rows.push_back(d);
    }

    SLOG_INFO("[MapTable] {} — 맵 {}개", path, _rows.size());
    return true;
}

const MapDef* CMapTable::Find(int32_t mapId) const
{
    for (const MapDef& d : _rows)
    {
        if (d.mapId == mapId)
            return &d;
    }
    return nullptr;
}
