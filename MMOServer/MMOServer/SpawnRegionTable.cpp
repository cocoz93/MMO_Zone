#include "SpawnRegionTable.h"

#if USE_MONSTER

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"
#include "MapTable.h"       // 맵 번호·크기 대조
#include "MonsterTable.h"   // 몬스터 번호 대조

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

const std::vector<SpawnRegion> CSpawnRegionTable::EMPTY;

bool CSpawnRegionTable::Load(const std::string& path, const CMapTable& maps, const CMonsterTable& monsters)
{
    _byMap.clear();
    _count = 0;

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[SpawnRegion] 파일을 못 엽니다: {}", path);
        return false;
    }
    if (lines.empty())
    {
        SLOG_ERROR("[SpawnRegion] 값 줄이 하나도 없습니다: {}", path);
        return false;
    }

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 7)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: 칸이 {}개입니다 — mapId,x0,y0,x1,y1,monsterId,count 일곱이 필요합니다",
                       dl.fileLine, cell.size());
            return false;
        }

        int64_t v[7] = {};
        for (int i = 0; i < 7; ++i)
        {
            if (!ParseInt(cell[static_cast<size_t>(i)], v[i]))
            {
                SLOG_ERROR("[SpawnRegion] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
                return false;
            }
        }
        const int64_t mapId = v[0], x0 = v[1], y0 = v[2], x1 = v[3], y1 = v[4], monsterId = v[5], count = v[6];

        // ⑴ 맵 — 맵 표에 있어야 한다(표끼리 대조). 번호가 곧 색인이라 상한은 맵 표가 본다
        const MapDef* map = (mapId < 0 || mapId > CMapTable::MAX_MAP_ID) ? nullptr : maps.Find(static_cast<int32_t>(mapId));
        if (map == nullptr)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: 맵 {} 은 맵 표(MapTable.csv)에 없습니다", dl.fileLine, mapId);
            return false;
        }

        // ⑵ 좌표 — 음수 금지 · 넓이가 있어야 한다 · 맵 안(오른쪽 끝은 폭 - 1 — 헤더 [맵 대조] ★)
        if (x0 < 0 || y0 < 0 || x0 >= x1 || y0 >= y1)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: 구역 ({},{})-({},{}) — 0 ≤ x0 < x1, 0 ≤ y0 < y1 이어야 합니다",
                       dl.fileLine, x0, y0, x1, y1);
            return false;
        }
        if (x1 > map->width - 1 || y1 > map->height - 1)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: 구역 ({},{})-({},{}) 이 맵 {} ({}×{}) 밖입니다 — x1 ≤ {}, y1 ≤ {} 여야 합니다",
                       dl.fileLine, x0, y0, x1, y1, mapId, map->width, map->height, map->width - 1, map->height - 1);
            return false;
        }

        // ⑶ 몬스터 — 몬스터 표에 있어야 한다(표끼리 대조)
        if (monsterId < 1 || monsterId > CMonsterTable::MAX_MONSTER_ID || monsters.Find(static_cast<int32_t>(monsterId)) == nullptr)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: 몬스터 {} 는 몬스터 표(MonsterTable.csv)에 없습니다", dl.fileLine, monsterId);
            return false;
        }

        // ⑷ 마릿수 — 0 이면 그 구역엔 한 마리도 안 간다(표에 둘 이유가 없다)
        if (count < 1 || count > MAX_COUNT)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: count {} 는 1~{} 여야 합니다", dl.fileLine, count, MAX_COUNT);
            return false;
        }

        if (static_cast<int64_t>(_byMap.size()) <= mapId)
            _byMap.resize(static_cast<size_t>(mapId) + 1);

        SpawnRegion r;
        r.mapId    = static_cast<int32_t>(mapId);
        r.x0       = static_cast<int32_t>(x0);
        r.y0       = static_cast<int32_t>(y0);
        r.x1       = static_cast<int32_t>(x1);
        r.y1        = static_cast<int32_t>(y1);
        r.monsterId = static_cast<int32_t>(monsterId);
        r.count     = static_cast<int32_t>(count);
        r.fileLine  = dl.fileLine;
        _byMap[static_cast<size_t>(mapId)].push_back(r);
        ++_count;
    }

    // ⑸ 맵 표의 맵마다 구역이 하나 이상 — 뱀을 깔 자리가 없는 맵을 만들지 않는다. 빠진 맵을 다 적고 끝낸다
    bool ok = true;
    for (const MapDef& m : maps.All())
    {
        if (RegionsOf(m.mapId).empty())
        {
            SLOG_ERROR("[SpawnRegion] 맵 {} (MapTable.csv {}줄) 에 구역이 없습니다 — 뱀을 깔 자리가 없습니다", m.mapId, m.fileLine);
            ok = false;
        }
    }
    if (!ok)
        return false;

    SLOG_INFO("[SpawnRegion] {} — 구역 {}개", path, _count);
    return true;
}

#endif // USE_MONSTER
