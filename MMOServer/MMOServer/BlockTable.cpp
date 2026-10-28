#include "BlockTable.h"

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"
#include "MapTable.h"      // 맵 번호·크기 대조 · 시작 구역
#include "PortalTable.h"   // 자리 대조
#include "NpcTable.h"
#if USE_MONSTER
#include "SpawnRegionTable.h"
#endif

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

bool CBlockTable::Load(const std::string& path, const CMapTable& maps)
{
    _rows.clear();
    _grids.clear();

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[BlockTable] 파일을 못 엽니다: {}", path);
        return false;
    }
    // 값 줄 0 은 된다 — 막힌 칸 없는 세계(헤더 [토글 없음])

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 6)
        {
            SLOG_ERROR("[BlockTable] {}줄: 칸이 {}개입니다 — mapId,x0,y0,x1,y1,note 여섯이 필요합니다", dl.fileLine, cell.size());
            return false;
        }

        int64_t v[5] = {};
        for (int i = 0; i < 5; ++i)
        {
            if (!ParseInt(cell[static_cast<size_t>(i)], v[i]))
            {
                SLOG_ERROR("[BlockTable] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
                return false;
            }
        }
        const int64_t mapId = v[0], x0 = v[1], y0 = v[2], x1 = v[3], y1 = v[4];

        // ⑴ 맵 — 맵 표에 있어야 한다. 번호가 상한 밖이면 맵 표에 없는 것과 같다
        const MapDef* map = (mapId < 0 || mapId > CMapTable::MAX_MAP_ID) ? nullptr : maps.Find(static_cast<int32_t>(mapId));
        if (map == nullptr)
        {
            SLOG_ERROR("[BlockTable] {}줄: 맵 {} 가 맵 표(MapTable.csv)에 없습니다", dl.fileLine, mapId);
            return false;
        }

        // ⑵ 사각형 — 맵 안 · 시작 <= 끝(헤더 [맵 대조])
        if (x0 < 0 || y0 < 0 || x1 > map->width - 1 || y1 > map->height - 1 || x0 > x1 || y0 > y1)
        {
            SLOG_ERROR("[BlockTable] {}줄: 사각형 ({},{})~({},{}) 가 맵 {} ({}×{}) 안이 아니거나 시작이 끝보다 큽니다 — 0~{}, 0~{}",
                       dl.fileLine, x0, y0, x1, y1, mapId, map->width, map->height, map->width - 1, map->height - 1);
            return false;
        }

        BlockRect r;
        r.mapId    = static_cast<int32_t>(mapId);
        r.x0       = static_cast<int32_t>(x0);
        r.y0       = static_cast<int32_t>(y0);
        r.x1       = static_cast<int32_t>(x1);
        r.y1       = static_cast<int32_t>(y1);
        r.note     = cell[5];
        r.fileLine = dl.fileLine;
        _rows.push_back(r);
    }

    // 맵마다 CWalkGrid 하나로 펼친다 — 줄이 있는 맵만(없는 맵은 GridOf 가 nullptr)
    for (const BlockRect& r : _rows)
    {
        MapGrid* found = nullptr;
        for (MapGrid& g : _grids)
            if (g.mapId == r.mapId) { found = &g; break; }
        if (found == nullptr)
        {
            const MapDef* map = maps.Find(r.mapId);   // ⑴ 에서 있음을 봤다
            _grids.push_back({ r.mapId, CWalkGrid{} });
            found = &_grids.back();
            found->grid.Init(map->width, map->height);
        }
        found->grid.Block(r.x0, r.y0, r.x1, r.y1);
    }

    SLOG_INFO("[BlockTable] {} — 사각형 {}개 · 막힌 칸이 있는 맵 {}개", path, _rows.size(), _grids.size());
    for (const MapGrid& g : _grids)
        SLOG_INFO("[BlockTable] 맵 {} — 막힌 칸 {}", g.mapId, g.grid.BlockedCount());
    return true;
}

const CWalkGrid* CBlockTable::GridOf(int32_t mapId) const
{
    for (const MapGrid& g : _grids)
    {
        if (g.mapId == mapId)
            return &g.grid;
    }
    return nullptr;
}

bool CBlockTable::TouchesBlocked(int32_t mapId, int32_t x0, int32_t y0, int32_t x1, int32_t y1) const
{
    const CWalkGrid* grid = GridOf(mapId);
    if (grid == nullptr)
        return false;
    for (int32_t cy = y0; cy <= y1; ++cy)
        for (int32_t cx = x0; cx <= x1; ++cx)
            if (grid->IsBlockedCell(cx, cy))
                return true;
    return false;
}

bool CBlockTable::CheckPlaces(const CMapTable& maps, const CPortalTable& portals, const CNpcTable& npcs) const
{
    bool ok = true;   // 하나에서 멈추지 않고 다 적는다 — 집 하나를 옮기면 여럿이 같이 걸리는 일이 흔하다

    // 시작 구역 — 막힌 줄이 있는 맵은 반드시 있고(헤더 ★), 있으면 통째로 걸을 수 있어야 한다. 좌표 양끝 포함 → 칸도 양끝 포함
    for (const MapDef& m : maps.All())
    {
        if (GridOf(m.mapId) == nullptr)
            continue;
        if (!m.start.has)
        {
            SLOG_ERROR("[BlockTable] 맵 {}: 막힌 칸이 있는데 시작 구역이 없습니다 — 들어오는 자리가 맵 전체 무작위라 벽 안에 떨어질 수 있습니다(MapTable.csv {}줄)",
                       m.mapId, m.fileLine);
            ok = false;
        }
        else if (TouchesBlocked(m.mapId, m.start.x0, m.start.y0, m.start.x1, m.start.y1))
        {
            SLOG_ERROR("[BlockTable] 맵 {}: 시작 구역 ({},{})~({},{}) 이 막힌 칸에 닿습니다(MapTable.csv {}줄)",
                       m.mapId, m.start.x0, m.start.y0, m.start.x1, m.start.y1, m.fileLine);
            ok = false;
        }
    }

    // 포털 — 선 자리와 도착 자리
    for (const PortalDef& p : portals.All())
    {
        if (TouchesBlocked(p.mapId, p.x, p.y, p.x, p.y))
        {
            SLOG_ERROR("[BlockTable] 포털 {}: 자리 (맵 {} · {},{}) 가 막힌 칸입니다(PortalTable.csv {}줄)", p.portalId, p.mapId, p.x, p.y, p.fileLine);
            ok = false;
        }
        if (TouchesBlocked(p.targetMapId, p.targetX, p.targetY, p.targetX, p.targetY))
        {
            SLOG_ERROR("[BlockTable] 포털 {}: 도착 자리 (맵 {} · {},{}) 가 막힌 칸입니다(PortalTable.csv {}줄)",
                       p.portalId, p.targetMapId, p.targetX, p.targetY, p.fileLine);
            ok = false;
        }
    }

    // NPC — 서 있는 자리
    for (const NpcDef& n : npcs.All())
    {
        if (TouchesBlocked(n.mapId, n.x, n.y, n.x, n.y))
        {
            SLOG_ERROR("[BlockTable] NPC {}: 자리 (맵 {} · {},{}) 가 막힌 칸입니다(NpcTable.csv {}줄)", n.npcId, n.mapId, n.x, n.y, n.fileLine);
            ok = false;
        }
    }
    return ok;
}

#if USE_MONSTER
bool CBlockTable::CheckSpawnRegions(const CMapTable& maps, const CSpawnRegionTable& regions) const
{
    bool ok = true;
    for (const MapDef& m : maps.All())
    {
        if (GridOf(m.mapId) == nullptr)
            continue;
        // 스폰 구역 = [x0, x1) × [y0, y1) (SpawnRegionTable.h) → 칸은 x0 ~ x1-1
        for (const SpawnRegion& r : regions.RegionsOf(m.mapId))
        {
            if (TouchesBlocked(m.mapId, r.x0, r.y0, r.x1 - 1, r.y1 - 1))
            {
                SLOG_ERROR("[BlockTable] 스폰 구역 (맵 {} · [{},{})×[{},{})) 이 막힌 칸에 닿습니다(SpawnRegion.csv {}줄)",
                           m.mapId, r.x0, r.x1, r.y0, r.y1, r.fileLine);
                ok = false;
            }
        }
    }
    return ok;
}
#endif
