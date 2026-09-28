#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BuildConfig.h"

#if USE_MONSTER

// ==========================================================================
// CSpawnRegionTable — 몬스터가 태어나는 구역 (Run/bin/SpawnRegion.csv)
//
// [무엇] 맵마다 사각형 구역 몇 개와 비율. 뱀은 처음 깔릴 때 구역 하나를 배정받아 들고(CMonster::_spawnRegion),
//        죽었다 되살아날 때도 그 구역 안의 무작위 자리로 간다. 그래서 구역별 마릿수가 객체 수로 지켜진다.
//
// [마릿수는 여기 없다] 총수는 INI [Game] MonsterPerZone 하나다. 표는 비율만 준다 —
//        마릿수를 두 곳에 두면 부하 시험·시험 도구가 INI 로 바꾼 값이 표와 어긋난다.
//
// [맵 크기 대조는 따로] 표만 읽어서는 맵 크기를 모른다. CGameServer 가 표를 읽은 직후 맵 설정과 대조한다
//        (CheckMaps) — 설정된 맵마다 구역이 하나 이상이어야 하고, 구역은 맵 안이어야 한다.
// ==========================================================================
struct SpawnRegion
{
    int32_t mapId    = 0;
    int32_t x0       = 0;   // 뽑는 좌표 = [x0, x1) × [y0, y1) 안의 실수
    int32_t y0       = 0;
    int32_t x1       = 0;
    int32_t y1       = 0;
    int32_t weight   = 0;   // 비율 — 맵 안 구역끼리만 견준다
    int32_t fileLine = 0;   // 맵 대조에서 틀렸을 때 가리킬 줄
};

class CSpawnRegionTable
{
public:
    static constexpr int32_t MAX_MAP_ID = 1000;     // 맵 번호가 곧 색인이다 — 오타 하나가 배열을 키우지 않게
    static constexpr int32_t MAX_WEIGHT = 10000;

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    bool Load(const std::string& path);

    struct MapSize { int32_t mapId; int32_t width; int32_t height; };

    // 맵 설정과 대조 — 설정된 맵마다 구역 ≥ 1 · 구역이 맵 안(0 ≤ x0 < x1 ≤ 폭 - 1) · 표에만 있는 맵 없음.
    //   ★ 오른쪽 끝을 "폭 - 1" 로 묶는 이유 — 이동 쪽 클램프(CZone::Tick)가 좌표를 폭 - 1 까지만 둔다.
    //     그 밖에 태어나면 첫 걸음에 클램프로 멈춘다.
    bool CheckMaps(const std::vector<MapSize>& maps) const;

    // 이 맵의 구역들 (없으면 빈 목록). 색인이 곧 CMonster::_spawnRegion 이다.
    const std::vector<SpawnRegion>& RegionsOf(int32_t mapId) const
    {
        if (mapId < 0 || mapId >= static_cast<int32_t>(_byMap.size()))
            return EMPTY;
        return _byMap[static_cast<size_t>(mapId)];
    }

    // 총수를 비율대로 정수로 나눈다 — 먼저 몫을 주고, 모자란 만큼은 나머지가 큰 구역부터 하나씩(같으면 앞 구역).
    //   ★ 무작위로 구역을 고르지 않는다 — 구역별 마릿수가 런마다 흔들리면 시험이 개수를 못 본다.
    //   outCounts 의 크기 = RegionsOf(mapId) 의 크기. 구역이 없으면 빈 목록.
    void SplitCount(int32_t mapId, int32_t total, std::vector<int32_t>& outCounts) const;

private:
    std::vector<std::vector<SpawnRegion>> _byMap;   // 색인 = 맵 번호
    int32_t _count = 0;

    static const std::vector<SpawnRegion> EMPTY;
};

#endif // USE_MONSTER
