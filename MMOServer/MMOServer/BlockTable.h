#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <World/WalkGrid.h>   // 맵마다 하나 세우는 칸 격자 — 콘솔 클라와 같은 식

#include "BuildConfig.h"

class CMapTable;
class CPortalTable;
class CNpcTable;
class CSpawnRegionTable;

// ==========================================================================
// CBlockTable — 막힌 칸 (Run/bin/BlockTable.csv)
//
// [무엇] 한 줄 = 그 맵의 막힌 칸 사각형 하나(칸 번호 · 양끝 포함). 읽은 뒤 맵마다 칸 격자(CWalkGrid) 하나로 펼친다.
//        같은 맵의 채널(존)은 그 격자 하나를 읽기만으로 같이 본다(CZone::Init 이 GridOf 로 받는다).
//
// [토글 없음] 맵 표와 같은 세계 데이터다(MapTable.h [토글 없음]). 값 줄이 0 이어도 된다 — 막힌 칸 없는 세계.
//
// [맵 대조] 맵이 맵 표에 있고 사각형이 그 맵 안이다(오른쪽 끝은 폭 - 1 — 포털 표와 같은 선).
//
// [자리 대조] 막힌 칸이 사람·몬스터가 「놓이는」 자리를 덮으면 안 된다 — 놓이는 순간 벽 안이라 못 움직인다.
//        시작 구역 · 포털(선 자리와 도착 자리) · NPC 자리 · 몬스터 스폰 구역을 본다(CheckPlaces · CheckSpawnRegions).
//        ★ 막힌 줄이 있는 맵은 시작 구역이 있어야 한다 — 없으면 들어오는 자리가 맵 전체 무작위(CZone::CalcSpawnPos)라
//          막힌 칸에 떨어질 수 있다. 이 대조가 있어 무작위 뽑기를 다시 뽑기 고리 없이 그대로 둔다.
//
// [마을 줄은 손으로 안 쓴다] 맵 0 줄은 WebClient/tools/town/blocks.js 가 집 배치(town0.js)에서 뽑아 쓴다.
// ==========================================================================
struct BlockRect
{
    int32_t     mapId    = 0;
    int32_t     x0       = 0;   // 칸 번호 — 양끝 포함
    int32_t     y0       = 0;
    int32_t     x1       = 0;
    int32_t     y1       = 0;
    std::string note;           // 사람이 읽을 설명(마을은 집 그림 이름). 서버는 로그에만 쓴다
    int32_t     fileLine = 0;   // 틀렸을 때 가리킬 줄
};

class CBlockTable
{
public:
    // 파일을 읽어 표를 세우고 맵마다 격자를 펼친다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    //   maps — 맵 번호·크기를 대조할 표(먼저 읽은 것)
    bool Load(const std::string& path, const CMapTable& maps);

    // 시작 구역 · 포털 · NPC 자리 대조(헤더 [자리 대조]) — 세 표를 다 읽은 뒤
    bool CheckPlaces(const CMapTable& maps, const CPortalTable& portals, const CNpcTable& npcs) const;

#if USE_MONSTER
    // 스폰 구역 대조(헤더 [자리 대조]) — 스폰 표를 읽은 뒤
    bool CheckSpawnRegions(const CMapTable& maps, const CSpawnRegionTable& regions) const;
#endif

    // 그 맵의 격자 — 막힌 줄이 없는 맵이면 nullptr(존은 판정을 통째로 건너뛴다)
    const CWalkGrid* GridOf(int32_t mapId) const;

    const std::vector<BlockRect>& All() const { return _rows; }

private:
    // 칸 사각형 [x0,x1]×[y0,y1](양끝 포함)이 막힌 칸을 하나라도 덮나
    bool TouchesBlocked(int32_t mapId, int32_t x0, int32_t y0, int32_t x1, int32_t y1) const;

    struct MapGrid { int32_t mapId; CWalkGrid grid; };

    std::vector<BlockRect> _rows;    // 파일 순서
    std::vector<MapGrid>   _grids;   // 막힌 줄이 있는 맵만. 맵이 몇 개뿐이라 찾기는 앞에서부터 훑는다(맵 표와 같은 꼴)
};
