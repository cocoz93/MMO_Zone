#include "NpcTable.h"

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"
#include "MapTable.h"      // 맵 번호·크기 대조
#include "PortalTable.h"   // 포털과 반경 겹침 경고
#if USE_SHOP
#include "ShopTable.h"     // 상점 번호 대조
#endif

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

namespace
{
    // 좌표가 맵 안인가 — 오른쪽 끝은 폭 - 1 (포털 표와 같은 선)
    bool InsideMap(const MapDef& m, int64_t x, int64_t y)
    {
        return x >= 0 && y >= 0 && x <= m.width - 1 && y <= m.height - 1;
    }
}

bool CNpcTable::Load(const std::string& path, const CMapTable& maps)
{
    _rows.clear();

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[NpcTable] 파일을 못 엽니다: {}", path);
        return false;
    }
    // 값 줄 0 은 된다 — NPC 없는 세계(헤더 [토글 없음])

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 9)
        {
            SLOG_ERROR("[NpcTable] {}줄: 칸이 {}개입니다 — npcId,mapId,x,y,radius,name,look,greeting,shopId 아홉이 필요합니다",
                       dl.fileLine, cell.size());
            return false;
        }

        int64_t npcId = 0, mapId = 0, x = 0, y = 0, radius = 0, shopId = 0;
        if (!ParseInt(cell[0], npcId) || !ParseInt(cell[1], mapId) || !ParseInt(cell[2], x) || !ParseInt(cell[3], y) ||
            !ParseInt(cell[4], radius) || !ParseInt(cell[8], shopId))
        {
            SLOG_ERROR("[NpcTable] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
            return false;
        }

        // ⑴ 번호 — 1 이상 · 상한 이하 · 중복 없음
        if (npcId < 1 || npcId > MAX_NPC_ID)
        {
            SLOG_ERROR("[NpcTable] {}줄: NPC 번호 {} 는 1~{} 여야 합니다", dl.fileLine, npcId, MAX_NPC_ID);
            return false;
        }
        if (Find(static_cast<int32_t>(npcId)) != nullptr)
        {
            SLOG_ERROR("[NpcTable] {}줄: NPC 번호 {} 가 겹칩니다", dl.fileLine, npcId);
            return false;
        }

        // ⑵ 맵 — 맵 표에 있다(헤더 [맵 대조]). 번호가 상한 밖이면 맵 표에 없는 것과 같다
        const MapDef* map = (mapId < 0 || mapId > CMapTable::MAX_MAP_ID) ? nullptr : maps.Find(static_cast<int32_t>(mapId));
        if (map == nullptr)
        {
            SLOG_ERROR("[NpcTable] {}줄: 맵 {} 가 맵 표(MapTable.csv)에 없습니다 (NPC {})", dl.fileLine, mapId, npcId);
            return false;
        }

        // ⑶ 자리 — 그 맵 안
        if (!InsideMap(*map, x, y))
        {
            SLOG_ERROR("[NpcTable] {}줄: 자리 ({},{}) 가 맵 {} ({}×{}) 밖입니다 — 0~{}, 0~{} (NPC {})",
                       dl.fileLine, x, y, mapId, map->width, map->height, map->width - 1, map->height - 1, npcId);
            return false;
        }

        // ⑷ 반경 — 0 이면 아무도 말을 못 건다
        if (radius < 1 || radius > MAX_RADIUS)
        {
            SLOG_ERROR("[NpcTable] {}줄: 반경 {} 는 1~{} 여야 합니다 (NPC {})", dl.fileLine, radius, MAX_RADIUS, npcId);
            return false;
        }

        // ⑸ 이름·생김새·인사 — 웹이 그리고 띄운다. 비면 이름표 없는 사람 · 아무 모습 · 빈 말풍선이 된다
        if (cell[5].empty() || cell[6].empty() || cell[7].empty())
        {
            SLOG_ERROR("[NpcTable] {}줄: name · look · greeting 은 비울 수 없습니다 — 웹이 그리고 띄운다 (NPC {})", dl.fileLine, npcId);
            return false;
        }

        // ⑹ 상점 번호 — 0(없음) 또는 1~상한. 상점 표에 있는지는 CheckShops(상점을 켠 빌드)
        if (shopId < 0 || shopId > MAX_SHOP_ID)
        {
            SLOG_ERROR("[NpcTable] {}줄: shopId {} 는 0~{} 여야 합니다 (NPC {})", dl.fileLine, shopId, MAX_SHOP_ID, npcId);
            return false;
        }

        NpcDef d;
        d.npcId    = static_cast<int32_t>(npcId);
        d.mapId    = static_cast<int32_t>(mapId);
        d.x        = static_cast<int32_t>(x);
        d.y        = static_cast<int32_t>(y);
        d.radius   = static_cast<int32_t>(radius);
        d.name     = cell[5];
        d.look     = cell[6];
        d.greeting = cell[7];
        d.shopId   = static_cast<int32_t>(shopId);
        d.fileLine = dl.fileLine;
        _rows.push_back(d);
    }

    SLOG_INFO("[NpcTable] {} — NPC {}개", path, _rows.size());
    return true;
}

#if USE_SHOP
bool CNpcTable::CheckShops(const CShopTable& shops) const
{
    bool ok = true;
    for (const NpcDef& d : _rows)
    {
        if (d.shopId == 0 || shops.Has(d.shopId))
            continue;
        SLOG_ERROR("[NpcTable] {}줄: 상점 {} 가 상점 표(ShopTable.csv)에 없습니다 (NPC {} {})", d.fileLine, d.shopId, d.npcId, d.name);
        ok = false;
    }
    return ok;
}
#endif

int32_t CNpcTable::WarnPortalOverlap(const CPortalTable& portals) const
{
    int32_t overlaps = 0;
    for (const NpcDef& n : _rows)
    {
        for (const PortalDef& p : portals.All())
        {
            if (p.mapId != n.mapId)
                continue;
            // 두 원(중심 거리 < 반경 합)이 겹치면 그 사이에 선 사람의 F 키가 둘에 걸린다
            const int64_t dx = n.x - p.x, dy = n.y - p.y, rr = n.radius + p.radius;
            if (dx * dx + dy * dy >= rr * rr)
                continue;
            SLOG_WARN("[NpcTable] {}줄: NPC {} ({}) 와 포털 {} ({}) 의 손 닿는 범위가 겹칩니다 — F 키는 가까운 쪽(웹)",
                      n.fileLine, n.npcId, n.name, p.portalId, p.name);
            ++overlaps;
        }
    }
    return overlaps;
}

const NpcDef* CNpcTable::Find(int32_t npcId) const
{
    for (const NpcDef& d : _rows)
    {
        if (d.npcId == npcId)
            return &d;
    }
    return nullptr;
}
