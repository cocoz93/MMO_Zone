#include "SpawnRegionTable.h"

#if USE_MONSTER

#include <algorithm>   // min·max — 격자
#include <cmath>       // ceil·sqrt — 격자

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
        if (cell.size() < 8)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: 칸이 {}개입니다 — mapId,x0,y0,x1,y1,monsterId,count,regenSec 여덟이 필요합니다",
                       dl.fileLine, cell.size());
            return false;
        }

        int64_t v[8] = {};
        for (int i = 0; i < 8; ++i)
        {
            if (!ParseInt(cell[static_cast<size_t>(i)], v[i]))
            {
                SLOG_ERROR("[SpawnRegion] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
                return false;
            }
        }
        const int64_t mapId = v[0], x0 = v[1], y0 = v[2], x1 = v[3], y1 = v[4], monsterId = v[5], count = v[6], regenSec = v[7];

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
        const MonsterDef* monster = (monsterId < 1 || monsterId > CMonsterTable::MAX_MONSTER_ID) ? nullptr : monsters.Find(static_cast<int32_t>(monsterId));
        if (monster == nullptr)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: 몬스터 {} 는 몬스터 표(MonsterTable.csv)에 없습니다", dl.fileLine, monsterId);
            return false;
        }

        // ⑶-2 보스는 방 맵에만 — 필드에서 죽으면 클리어할 방이 없다(방 클리어는 방 맵 분기 — CCombatHandler::OnDeath).
        //   방 맵마다 하나인지는 줄을 다 읽은 뒤 아래 ⑹ 이 센다
        if (monster->boss && map->kind != MapKind::INSTANCE)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: 몬스터 {}({}) 는 보스라 방 맵(instance)에만 깔 수 있습니다 — 맵 {} 은 방 맵이 아닙니다",
                       dl.fileLine, monsterId, monster->name, mapId);
            return false;
        }

        // ⑷ 마릿수 — 0 이면 그 구역엔 한 마리도 안 간다(표에 둘 이유가 없다)
        if (count < 1 || count > MAX_COUNT)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: count {} 는 1~{} 여야 합니다", dl.fileLine, count, MAX_COUNT);
            return false;
        }

        // ⑸ 리젠 주기 — 마을·던전은 1~상한(0 이면 매 틱 채워 잡은 티가 안 난다).
        //    방 맵(instance)은 0 만 — 「한 번만」: 방이 생길 때 다 깔고 다시 안 채운다(CMonsterManager::RegenDue).
        //    방 줄에 주기를 주면 잡은 보스가 다시 깔리고, 던전 줄에 0 을 주면 그 구역이 한 번 비면 영영 빈다 — 둘 다 막는다
        if (map->kind == MapKind::INSTANCE)
        {
            if (regenSec != 0)
            {
                SLOG_ERROR("[SpawnRegion] {}줄: 방 맵 {} 의 구역은 regenSec 0(한 번만)이어야 합니다 — 지금 {}", dl.fileLine, mapId, regenSec);
                return false;
            }
        }
        else if (regenSec < 1 || regenSec > MAX_REGEN_SEC)
        {
            SLOG_ERROR("[SpawnRegion] {}줄: regenSec {} 는 1~{} 여야 합니다(0 = 한 번만은 방 맵 전용)", dl.fileLine, regenSec, MAX_REGEN_SEC);
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
        r.regenFrames = static_cast<int32_t>(regenSec) * FRAME_PER_SEC;
        r.fileLine  = dl.fileLine;
        BuildSpots(r);
        _byMap[static_cast<size_t>(mapId)].push_back(r);
        ++_count;
    }

    // ⑹ 맵 종류마다 구역 개수 — 던전·방은 하나 이상(뱀을 깔 자리가 없는 던전을 만들지 않는다), 마을은 0(마을에 몬스터를 깔지 않는다).
    //    틀린 맵을 다 적고 끝낸다. 종류가 늘면 case 가 하나 는다(빠지면 컴파일러가 아니라 COUNT case 가 막는다)
    bool ok = true;
    for (const MapDef& m : maps.All())
    {
        const size_t regions = RegionsOf(m.mapId).size();
        switch (m.kind)
        {
        case MapKind::DUNGEON:
            if (regions == 0)
            {
                SLOG_ERROR("[SpawnRegion] 던전 맵 {} (MapTable.csv {}줄) 에 구역이 없습니다 — 뱀을 깔 자리가 없습니다", m.mapId, m.fileLine);
                ok = false;
            }
            break;
        case MapKind::INSTANCE:
        {
            if (regions == 0)
            {
                SLOG_ERROR("[SpawnRegion] 방 맵 {} (MapTable.csv {}줄) 에 구역이 없습니다 — 보스를 깔 자리가 없습니다", m.mapId, m.fileLine);
                ok = false;
                break;
            }
            // 보스 구역 — 정확히 하나 · count 1. 없으면 그 방은 영영 클리어가 안 되고(보상·잠금이 안 걸린다),
            //   둘 이상이면 보스가 죽을 때마다 클리어가 다시 온다(헤더 [맵 대조])
            int32_t bossRegions = 0;
            for (const SpawnRegion& r : RegionsOf(m.mapId))
            {
                const MonsterDef* def = monsters.Find(r.monsterId);   // ⑶ 이 이미 대조했다
                if (def == nullptr || !def->boss)
                    continue;
                ++bossRegions;
                if (r.count != 1)
                {
                    SLOG_ERROR("[SpawnRegion] {}줄: 방 맵 {} 의 보스 {}({}) 구역은 count 1 이어야 합니다 — 지금 {}",
                               r.fileLine, m.mapId, r.monsterId, def->name, r.count);
                    ok = false;
                }
            }
            if (bossRegions != 1)
            {
                SLOG_ERROR("[SpawnRegion] 방 맵 {} (MapTable.csv {}줄) 의 보스 구역이 {}개입니다 — 정확히 하나여야 합니다(몬스터 표 boss = 1 인 몬스터)",
                           m.mapId, m.fileLine, bossRegions);
                ok = false;
            }
            break;
        }
        case MapKind::TOWN:
            if (regions != 0)
            {
                SLOG_ERROR("[SpawnRegion] 마을 맵 {} (MapTable.csv {}줄) 을 가리키는 구역이 {}개 있습니다 — 마을엔 몬스터를 깔지 않습니다",
                           m.mapId, m.fileLine, regions);
                ok = false;
            }
            break;
        case MapKind::COUNT:
            SLOG_ERROR("[SpawnRegion] 맵 {} 의 종류 값이 개수 표시(COUNT)입니다 — 맵 표가 걸렀어야 합니다", m.mapId);
            ok = false;
            break;
        }
    }
    if (!ok)
        return false;

    SLOG_INFO("[SpawnRegion] {} — 구역 {}개", path, _count);
    return true;
}

// 점 계산 — 기동 때 줄마다 한 번. 깔 때는 이 범위에서 난수만 뽑는다(CMonsterManager::PickSpawnPos).
void CSpawnRegionTable::BuildSpots(SpawnRegion& r)
{
    // 칸이 정사각형에 가깝게 — 열 = ⌈√(count × 폭 ÷ 높이)⌉(마릿수를 넘지 않게), 행 = ⌈count ÷ 열⌉ → 칸 수 ≥ count.
    //   한 마리면 1×1(구역 전체), 23마리·정사각형이면 5×5.
    const float w = static_cast<float>(r.x1 - r.x0);
    const float h = static_cast<float>(r.y1 - r.y0);
    const int32_t cols  = (std::min)(r.count, (std::max)(1, static_cast<int32_t>(std::ceil(std::sqrt(static_cast<float>(r.count) * w / h)))));
    const int32_t rows  = (r.count + cols - 1) / cols;
    const int32_t cells = cols * rows;
    const float cw = w / static_cast<float>(cols);
    const float ch = h / static_cast<float>(rows);
    const float mx = cw * (1.0f - SPOT_INNER) * 0.5f;   // 칸 가장자리 띠 — 여기엔 안 깐다
    const float my = ch * (1.0f - SPOT_INNER) * 0.5f;

    r.spots.clear();
    r.spots.reserve(static_cast<size_t>(r.count));
    for (int32_t p = 0; p < r.count; ++p)
    {
        // 점 → 칸 — 칸 수가 마릿수보다 많으면 남는 칸을 고르게 건너뛴다(23마리·25칸이면 12·24번 칸이 빈다)
        const int32_t cell = static_cast<int32_t>(static_cast<int64_t>(p) * cells / r.count);
        const float cx = static_cast<float>(r.x0) + static_cast<float>(cell % cols) * cw;   // 칸 왼쪽 위
        const float cy = static_cast<float>(r.y0) + static_cast<float>(cell / cols) * ch;
        r.spots.push_back({ cx + mx, cy + my, cx + cw - mx, cy + ch - my });
    }
}

#endif // USE_MONSTER
