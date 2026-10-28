#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BuildConfig.h"

#if USE_MONSTER

class CMapTable;
class CMonsterTable;

// ==========================================================================
// CSpawnRegionTable — 몬스터가 태어나는 구역 (Run/bin/SpawnRegion.csv)
//
// [무엇] 한 줄 = 「이 맵의 이 사각형 구역에 · 이 몬스터를 · 몇 마리 · 몇 초마다 다시 채우나」.
//        몬스터 관리자가 줄마다 count 마리를 유지한다 — 존이 생기면 다 깔고, 그 뒤로 regenSec 마다
//        빈 만큼 같은 구역에 한꺼번에 깐다(CMonsterManager — 구역 주기 리젠).
//        뱀마다 따로 재지 않는 이유 — 바람의나라 서술 「굴마다 젠 속도·양이 다르고 여러 마리가 한꺼번에 젠」
//        (MyDocs/claude-plans/baram-spawn-reference.md, 10-09 사용자 결정).
//
// [깔 자리 — 점] 읽을 때 구역을 count 칸 이상의 그리드로 나눠 점(칸) 하나마다 「칸 안쪽 범위」를 계산해 둔다(spots).
//        몬스터 한 마리가 점 하나를 맡고, 깔 때는 그 범위 안에서 난수만 뽑는다 — 한쪽에 몰리지 않고(10-09 사용자 결정)
//        깔 때마다 계산하지 않는다. 칸 나누기는 구역 크기·마릿수에서 나오는 배치 규칙이라 파일에는 없다.
//        기획이 점을 직접 찍고 싶어지면(맵 편집기) 그 점들로 spots 를 채우면 되고, 깔기 코드는 그대로다.
//
// [마릿수도 여기] 존당 마릿수는 이 맵 줄들의 count 합이다. 예전엔 총수를 INI 에 두고 표는 비율만 주었는데,
//        "이 구역에 몇 마리"를 보려면 두 파일을 같이 봐야 했다(10-08 표 한 곳으로). 두 곳에 두지 않는다는 것은 같다.
//
// [몬스터 번호 대조] 몬스터 번호는 몬스터 표에 있어야 한다 — 몬스터 표를 먼저 읽고 그 결과를 받는다(LoadAll 순서).
//
// [맵 대조] 맵 번호는 맵 표에 있어야 하고, 구역은 그 맵 안이어야 한다(0 ≤ x0 < x1 ≤ 폭 - 1).
//        구역 개수는 맵 종류로 가른다 — 던전·방은 하나 이상(뱀을 깔 자리가 없는 던전을 만들지 않게), 마을은 0(MapTable.h [종류·시작 구역]).
//        리젠 주기도 맵 종류로 가른다 — 방 맵(instance)은 0(한 번만 — 방이 생길 때 깔고 다시 안 채움), 나머지는 1~MAX_REGEN_SEC.
//        맵 표도 먼저 읽고 그 결과를 받는다. 10-09 전에는 맵 크기가 INI 에 있어 게임 서버가 표를 읽은 뒤 따로 대조했다(CheckMaps).
//        보스(몬스터 표 boss)도 맵 종류로 가른다 — 방 맵에만 깔고, 방 맵마다 보스 구역이 정확히 하나 · count 1 이어야 한다.
//        보스가 없는 방은 영영 클리어가 안 되고, 둘이면 「보스가 죽었다」가 두 번 온다(CCombatHandler::OnDeath 의 방 클리어).
//        ★ 오른쪽 끝을 "폭 - 1" 로 묶는 이유 — 이동 쪽 클램프(CZone::Tick)가 좌표를 폭 - 1 까지만 둔다.
//          그 밖에 태어나면 첫 걸음에 클램프로 멈춘다.
// ==========================================================================
// 점 하나의 깔 자리 범위 — [x0, x1) × [y0, y1) 안의 실수 좌표에 깐다
struct SpawnSpot
{
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
};

struct SpawnRegion
{
    int32_t mapId     = 0;
    int32_t x0        = 0;   // 구역 = [x0, x1) × [y0, y1) (칸)
    int32_t y0        = 0;
    int32_t x1        = 0;
    int32_t y1        = 0;
    int32_t monsterId = 0;   // 몬스터 표 번호 (CMonsterTable::Find)
    int32_t count     = 0;   // 이 구역에 유지할 마릿수
    int32_t regenFrames = 0;   // 리젠 주기 — 표의 regenSec × FRAME_PER_SEC(읽을 때 틱 수로 바꿔 둔다). 0 = 한 번만(방 맵 줄만)
    int32_t fileLine  = 0;   // 틀렸을 때 가리킬 줄
    std::vector<SpawnSpot> spots;   // 점 count 개 — 읽을 때 그리드로 계산해 둔다([깔 자리 — 점]). 색인 = 점 번호(CMonster::_spawnPoint)
};

class CSpawnRegionTable
{
public:
    static constexpr int32_t MAX_COUNT  = 10000;    // 한 구역 마릿수 상한 — 오타 하나(90 → 900000)가 존을 통째로 메우지 않게
    static constexpr int32_t MAX_REGEN_SEC = 86400;     // 리젠 주기 상한(하루) — 틱 수로 바꿔도 int32 안이다
    static constexpr float   SPOT_INNER    = 0.6f;      // 점의 범위 = 칸 안쪽 이 비율 — 이웃 칸의 몬스터와 칸 크기의 40% 이상 떨어지게

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    //   maps     — 맵 번호·크기를 대조할 표(먼저 읽은 것)
    //   monsters — 몬스터 번호를 대조할 표(먼저 읽은 것)
    bool Load(const std::string& path, const CMapTable& maps, const CMonsterTable& monsters);

    // 이 맵의 구역들 (없으면 빈 목록).
    const std::vector<SpawnRegion>& RegionsOf(int32_t mapId) const
    {
        if (mapId < 0 || mapId >= static_cast<int32_t>(_byMap.size()))
            return EMPTY;
        return _byMap[static_cast<size_t>(mapId)];
    }

private:
    // 구역을 count 칸 이상의 그리드로 나눠 점마다 칸 안쪽 범위를 채운다([깔 자리 — 점]).
    static void BuildSpots(SpawnRegion& r);

    std::vector<std::vector<SpawnRegion>> _byMap;   // 색인 = 맵 번호 (맵 표가 번호 상한을 본다 — CMapTable::MAX_MAP_ID)
    int32_t _count = 0;

    static const std::vector<SpawnRegion> EMPTY;
};

#endif // USE_MONSTER
