#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BuildConfig.h"

#if USE_MONSTER

class CMonsterTable;

// ==========================================================================
// CSpawnRegionTable — 몬스터가 태어나는 구역 (Run/bin/SpawnRegion.csv)
//
// [무엇] 한 줄 = 「이 맵의 이 사각형 구역에 · 이 몬스터를 · 몇 마리」. 깔 때 줄마다 그 구역 안의 무작위 자리에 count 마리.
//
// [마릿수도 여기] 존당 마릿수는 이 맵 줄들의 count 합이다. 예전엔 총수를 INI 에 두고 표는 비율만 주었는데,
//        "이 구역에 몇 마리"를 보려면 두 파일을 같이 봐야 했다(10-08 표 한 곳으로). 두 곳에 두지 않는다는 것은 같다.
//
// [몬스터 번호 대조] 몬스터 번호는 몬스터 표에 있어야 한다 — 몬스터 표를 먼저 읽고 그 결과를 받는다(LoadAll 순서).
//
// [맵 크기 대조는 따로] 표만 읽어서는 맵 크기를 모른다. CGameServer 가 표를 읽은 직후 맵 설정과 대조한다
//        (CheckMaps) — 설정된 맵마다 구역이 하나 이상이어야 하고, 구역은 맵 안이어야 한다.
// ==========================================================================
struct SpawnRegion
{
    int32_t mapId     = 0;
    int32_t x0        = 0;   // 뽑는 좌표 = [x0, x1) × [y0, y1) 안의 실수
    int32_t y0        = 0;
    int32_t x1        = 0;
    int32_t y1        = 0;
    int32_t monsterId = 0;   // 몬스터 표 번호 (CMonsterTable::Find)
    int32_t count     = 0;   // 이 구역에 깔 마릿수
    int32_t fileLine  = 0;   // 맵 대조에서 틀렸을 때 가리킬 줄
};

class CSpawnRegionTable
{
public:
    static constexpr int32_t MAX_MAP_ID = 1000;     // 맵 번호가 곧 색인이다 — 오타 하나가 배열을 키우지 않게
    static constexpr int32_t MAX_COUNT  = 10000;    // 한 구역 마릿수 상한 — 오타 하나(90 → 900000)가 존을 통째로 메우지 않게

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    //   monsters — 몬스터 번호를 대조할 표(먼저 읽은 것)
    bool Load(const std::string& path, const CMonsterTable& monsters);

    struct MapSize { int32_t mapId; int32_t width; int32_t height; };

    // 맵 설정과 대조 — 설정된 맵마다 구역 ≥ 1 · 구역이 맵 안(0 ≤ x0 < x1 ≤ 폭 - 1) · 표에만 있는 맵 없음.
    //   ★ 오른쪽 끝을 "폭 - 1" 로 묶는 이유 — 이동 쪽 클램프(CZone::Tick)가 좌표를 폭 - 1 까지만 둔다.
    //     그 밖에 태어나면 첫 걸음에 클램프로 멈춘다.
    bool CheckMaps(const std::vector<MapSize>& maps) const;

    // 이 맵의 구역들 (없으면 빈 목록).
    const std::vector<SpawnRegion>& RegionsOf(int32_t mapId) const
    {
        if (mapId < 0 || mapId >= static_cast<int32_t>(_byMap.size()))
            return EMPTY;
        return _byMap[static_cast<size_t>(mapId)];
    }

private:
    std::vector<std::vector<SpawnRegion>> _byMap;   // 색인 = 맵 번호
    int32_t _count = 0;

    static const std::vector<SpawnRegion> EMPTY;
};

#endif // USE_MONSTER
