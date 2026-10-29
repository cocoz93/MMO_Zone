#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Reach.h"   // 손이 닿나 — 포털·NPC 가 같은 식

class CMapTable;

// ==========================================================================
// CPortalTable — 맵과 맵을 잇는 포털 (Run/bin/PortalTable.csv)
//
// [무엇] 한 줄 = 「이 맵의 이 자리에서 · 이 반경 안에서 상호작용하면 · 저 맵의 저 자리로」.
//        클라는 포털 번호만 보내고(C2S_PORTAL_USE) 어디로 가는지는 이 표가 정한다 — 목적지를 클라가 고르지 못한다.
//
// [토글 없음] 맵 표와 같은 세계 데이터다(MapTable.h [토글 없음]). 값 줄이 0 이어도 된다 — 포털 없는 세계.
//        맵 표가 0 줄이면 오류인 것과 다르다(맵이 없으면 설 곳이 없지만, 포털이 없으면 못 옮겨 다닐 뿐이다).
//
// [맵 대조] 두 맵 다 맵 표에 있어야 하고 서로 달라야 한다 — 같은 맵 안 순간이동은 이 표의 일이 아니다
//        (같은 존으로 옮기면 자리만 다시 뽑히는 문제 — PickZoneChangeTarget 의 SAME_ZONE 과 같다).
//        포털 자리·도착 자리는 각 맵 안이다(오른쪽 끝은 폭 - 1 — 이동 클램프와 같은 선, SpawnRegionTable.h [맵 대조] ★).
//        맵 표를 먼저 읽고 그 결과를 받는다(CGameDataFiles::LoadAll 순서).
//
// [판정은 표 옆] 「손이 닿나」(PortalDef::InReach)는 포털 한 줄과 좌표만 보는 계산이라 여기 둔다(식 자체는 NPC 와 같은 Reach.h).
//        같은 맵인지는 부르는 쪽이 본다 — 지금 어느 존에 있는지를 아는 쪽이다.
// ==========================================================================
struct PortalDef
{
    int32_t     portalId    = 0;
    int32_t     mapId       = 0;   // 포털이 있는 맵
    int32_t     x           = 0;   // 포털 자리(좌표)
    int32_t     y           = 0;
    int32_t     radius      = 0;   // 이 거리 안이면 쓸 수 있다(칸)
    int32_t     targetMapId = 0;   // 가는 맵
    int32_t     targetX     = 0;   // 도착 자리(좌표)
    int32_t     targetY     = 0;
    std::string name;              // 안내 글자 — 웹이 띄운다(같은 파일을 serve.js 로 받는다). 서버는 로그에만 쓴다
    int32_t     fileLine    = 0;   // 틀렸을 때 가리킬 줄

    // 손이 닿나 — 포털 자리와의 거리가 반경 안(경계 포함). 식은 NPC 와 같은 한 곳(Reach.h)
    bool InReach(float px, float py) const
    {
        return Reach::InRadius(px, py, static_cast<float>(x), static_cast<float>(y), static_cast<float>(radius));
    }
};

class CPortalTable
{
public:
    static constexpr int32_t MAX_PORTAL_ID = 10000;   // 오타 하나가 엉뚱한 큰 번호가 되지 않게
    static constexpr int32_t MAX_RADIUS    = 10;      // 칸 — 화면 반 너머에서 들어가지 않게

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    //   maps — 맵 번호·크기를 대조할 표(먼저 읽은 것)
    bool Load(const std::string& path, const CMapTable& maps);

    // 없는 번호면 nullptr
    const PortalDef* Find(int32_t portalId) const;

    // 파일 순서 그대로
    const std::vector<PortalDef>& All() const { return _rows; }

private:
    std::vector<PortalDef> _rows;   // 파일 순서. 몇 줄뿐이라 찾기는 앞에서부터 훑는다(맵 표와 같은 꼴)
};
