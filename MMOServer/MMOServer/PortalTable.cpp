#include "PortalTable.h"

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"
#include "MapTable.h"   // 맵 번호·크기 대조

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

namespace
{
    // 좌표가 맵 안인가 — 오른쪽 끝은 폭 - 1 (헤더 [맵 대조])
    bool InsideMap(const MapDef& m, int64_t x, int64_t y)
    {
        return x >= 0 && y >= 0 && x <= m.width - 1 && y <= m.height - 1;
    }
}

bool CPortalTable::Load(const std::string& path, const CMapTable& maps)
{
    _rows.clear();

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[PortalTable] 파일을 못 엽니다: {}", path);
        return false;
    }
    // 값 줄 0 은 된다 — 포털 없는 세계(헤더 [토글 없음])

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 9)
        {
            SLOG_ERROR("[PortalTable] {}줄: 칸이 {}개입니다 — portalId,mapId,x,y,radius,targetMapId,targetX,targetY,name 아홉이 필요합니다",
                       dl.fileLine, cell.size());
            return false;
        }

        int64_t v[8] = {};
        for (int i = 0; i < 8; ++i)
        {
            if (!ParseInt(cell[static_cast<size_t>(i)], v[i]))
            {
                SLOG_ERROR("[PortalTable] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
                return false;
            }
        }
        const int64_t portalId = v[0], mapId = v[1], x = v[2], y = v[3], radius = v[4],
                      targetMapId = v[5], targetX = v[6], targetY = v[7];

        // ⑴ 번호 — 1 이상 · 상한 이하 · 중복 없음
        if (portalId < 1 || portalId > MAX_PORTAL_ID)
        {
            SLOG_ERROR("[PortalTable] {}줄: 포털 번호 {} 는 1~{} 여야 합니다", dl.fileLine, portalId, MAX_PORTAL_ID);
            return false;
        }
        if (Find(static_cast<int32_t>(portalId)) != nullptr)
        {
            SLOG_ERROR("[PortalTable] {}줄: 포털 번호 {} 가 겹칩니다", dl.fileLine, portalId);
            return false;
        }

        // ⑵ 맵 — 둘 다 맵 표에 있고 서로 다르다(헤더 [맵 대조]). 번호가 상한 밖이면 맵 표에 없는 것과 같다
        const MapDef* from = (mapId < 0 || mapId > CMapTable::MAX_MAP_ID) ? nullptr : maps.Find(static_cast<int32_t>(mapId));
        const MapDef* to   = (targetMapId < 0 || targetMapId > CMapTable::MAX_MAP_ID) ? nullptr : maps.Find(static_cast<int32_t>(targetMapId));
        if (from == nullptr || to == nullptr)
        {
            SLOG_ERROR("[PortalTable] {}줄: 맵 {} → {} — 둘 다 맵 표(MapTable.csv)에 있어야 합니다", dl.fileLine, mapId, targetMapId);
            return false;
        }
        if (mapId == targetMapId)
        {
            SLOG_ERROR("[PortalTable] {}줄: 맵 {} → {} — 같은 맵으로 가는 포털은 만들지 않습니다", dl.fileLine, mapId, targetMapId);
            return false;
        }

        // ⑶ 자리 — 포털 자리는 그 맵 안, 도착 자리는 가는 맵 안
        if (!InsideMap(*from, x, y))
        {
            SLOG_ERROR("[PortalTable] {}줄: 포털 자리 ({},{}) 가 맵 {} ({}×{}) 밖입니다 — 0~{}, 0~{}",
                       dl.fileLine, x, y, mapId, from->width, from->height, from->width - 1, from->height - 1);
            return false;
        }
        if (!InsideMap(*to, targetX, targetY))
        {
            SLOG_ERROR("[PortalTable] {}줄: 도착 자리 ({},{}) 가 맵 {} ({}×{}) 밖입니다 — 0~{}, 0~{}",
                       dl.fileLine, targetX, targetY, targetMapId, to->width, to->height, to->width - 1, to->height - 1);
            return false;
        }

        // ⑷ 반경 — 0 이면 아무도 못 쓴다
        if (radius < 1 || radius > MAX_RADIUS)
        {
            SLOG_ERROR("[PortalTable] {}줄: 반경 {} 는 1~{} 여야 합니다", dl.fileLine, radius, MAX_RADIUS);
            return false;
        }

        // ⑸ 이름 — 웹 안내에 뜬다
        if (cell[8].empty())
        {
            SLOG_ERROR("[PortalTable] {}줄: 이름이 비었습니다 — 웹이 안내에 띄운다", dl.fileLine);
            return false;
        }

        PortalDef d;
        d.portalId    = static_cast<int32_t>(portalId);
        d.mapId       = static_cast<int32_t>(mapId);
        d.x           = static_cast<int32_t>(x);
        d.y           = static_cast<int32_t>(y);
        d.radius      = static_cast<int32_t>(radius);
        d.targetMapId = static_cast<int32_t>(targetMapId);
        d.targetX     = static_cast<int32_t>(targetX);
        d.targetY     = static_cast<int32_t>(targetY);
        d.name        = cell[8];
        d.fileLine    = dl.fileLine;
        _rows.push_back(d);
    }

    SLOG_INFO("[PortalTable] {} — 포털 {}개", path, _rows.size());
    return true;
}

const PortalDef* CPortalTable::Find(int32_t portalId) const
{
    for (const PortalDef& d : _rows)
    {
        if (d.portalId == portalId)
            return &d;
    }
    return nullptr;
}
