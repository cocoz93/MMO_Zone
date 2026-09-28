#include "SpawnRegionTable.h"

#if USE_MONSTER

#include <algorithm>

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

const std::vector<SpawnRegion> CSpawnRegionTable::EMPTY;

bool CSpawnRegionTable::Load(const std::string& path)
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
        if (cell.size() < 6)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: 칸이 {}개입니다 — mapId,x0,y0,x1,y1,weight 여섯이 필요합니다",
                       dl.fileLine, cell.size());
            return false;
        }

        int64_t v[6] = {};
        for (int i = 0; i < 6; ++i)
        {
            if (!ParseInt(cell[static_cast<size_t>(i)], v[i]))
            {
                SLOG_ERROR("[SpawnRegion] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
                return false;
            }
        }
        const int64_t mapId = v[0], x0 = v[1], y0 = v[2], x1 = v[3], y1 = v[4], weight = v[5];

        // ⑴ 맵 번호 — 번호가 곧 색인이다
        if (mapId < 0 || mapId > MAX_MAP_ID)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: 맵 번호 {} 는 0~{} 여야 합니다", dl.fileLine, mapId, MAX_MAP_ID);
            return false;
        }

        // ⑵ 좌표 — 음수 금지 · 넓이가 있어야 한다. 맵 안인지는 맵 크기를 알아야 해서 CheckMaps 가 본다
        if (x0 < 0 || y0 < 0 || x0 >= x1 || y0 >= y1)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: 구역 ({},{})-({},{}) — 0 ≤ x0 < x1, 0 ≤ y0 < y1 이어야 합니다",
                       dl.fileLine, x0, y0, x1, y1);
            return false;
        }

        // ⑶ 비율 — 0 이면 그 구역엔 한 마리도 안 간다(표에 둘 이유가 없다)
        if (weight < 1 || weight > MAX_WEIGHT)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: weight {} 는 1~{} 여야 합니다", dl.fileLine, weight, MAX_WEIGHT);
            return false;
        }

        if (static_cast<int64_t>(_byMap.size()) <= mapId)
            _byMap.resize(static_cast<size_t>(mapId) + 1);

        SpawnRegion r;
        r.mapId    = static_cast<int32_t>(mapId);
        r.x0       = static_cast<int32_t>(x0);
        r.y0       = static_cast<int32_t>(y0);
        r.x1       = static_cast<int32_t>(x1);
        r.y1       = static_cast<int32_t>(y1);
        r.weight   = static_cast<int32_t>(weight);
        r.fileLine = dl.fileLine;
        _byMap[static_cast<size_t>(mapId)].push_back(r);
        ++_count;
    }

    SLOG_INFO("[SpawnRegion] {} — 구역 {}개", path, _count);
    return true;
}

bool CSpawnRegionTable::CheckMaps(const std::vector<MapSize>& maps) const
{
    bool ok = true;

    // ⑴ 설정된 맵마다 — 구역이 하나 이상 · 전부 맵 안
    for (const MapSize& m : maps)
    {
        const std::vector<SpawnRegion>& regions = RegionsOf(m.mapId);
        if (regions.empty())
        {
            SLOG_ERROR("[SpawnRegion] 맵 {} 에 구역이 없습니다 — 뱀을 깔 자리가 없습니다", m.mapId);
            ok = false;
            continue;
        }
        for (const SpawnRegion& r : regions)
        {
            if (r.x1 > m.width - 1 || r.y1 > m.height - 1)
            {
                SLOG_ERROR("[SpawnRegion] {}줄: 구역 ({},{})-({},{}) 이 맵 {} ({}×{}) 밖입니다 — x1 ≤ {}, y1 ≤ {} 여야 합니다",
                           r.fileLine, r.x0, r.y0, r.x1, r.y1, m.mapId, m.width, m.height, m.width - 1, m.height - 1);
                ok = false;
            }
        }
    }

    // ⑵ 표에만 있는 맵 — 맵 번호 오타일 수 있다. 조용히 무시하면 그 구역이 영영 안 쓰인다
    for (size_t mapId = 0; mapId < _byMap.size(); ++mapId)
    {
        if (_byMap[mapId].empty())
            continue;
        const bool configured = std::any_of(maps.begin(), maps.end(),
            [mapId](const MapSize& m) { return m.mapId == static_cast<int32_t>(mapId); });
        if (!configured)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: 맵 {} 은 맵 설정(MapCount)에 없습니다",
                       _byMap[mapId].front().fileLine, mapId);
            ok = false;
        }
    }

    return ok;
}

void CSpawnRegionTable::SplitCount(int32_t mapId, int32_t total, std::vector<int32_t>& outCounts) const
{
    const std::vector<SpawnRegion>& regions = RegionsOf(mapId);
    outCounts.assign(regions.size(), 0);
    if (regions.empty() || total <= 0)
        return;

    int64_t weightSum = 0;
    for (const SpawnRegion& r : regions)
        weightSum += r.weight;

    // 몫을 먼저 주고, 나머지(몫에서 버려진 부분)가 큰 구역부터 하나씩 더 준다
    std::vector<int64_t> remainder(regions.size(), 0);
    int32_t given = 0;
    for (size_t i = 0; i < regions.size(); ++i)
    {
        const int64_t share = static_cast<int64_t>(total) * regions[i].weight;
        outCounts[i] = static_cast<int32_t>(share / weightSum);
        remainder[i] = share % weightSum;
        given += outCounts[i];
    }

    std::vector<size_t> order(regions.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(),
        [&remainder](size_t a, size_t b) { return remainder[a] > remainder[b]; });   // 같으면 앞 구역이 먼저

    for (size_t k = 0; given < total; ++k)
    {
        ++outCounts[order[k % order.size()]];
        ++given;
    }
}

#endif // USE_MONSTER
