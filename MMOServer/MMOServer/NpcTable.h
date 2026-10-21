#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BuildConfig.h"   // USE_SHOP — 상점 표 대조. 안 읽으면 아래 #if 가 조용히 0 이 된다

class CMapTable;
class CPortalTable;
#if USE_SHOP
class CShopTable;
#endif

// ==========================================================================
// CNpcTable — 마을 NPC (Run/bin/NpcTable.csv)
//
// [무엇] 한 줄 = 「이 맵의 이 자리에 · 이 이름·생김새의 NPC 가 서 있고 · 이 반경 안에서 말 걸면 이 인사와 이 상점」.
//        존이 처음 돌 때 그 맵의 줄마다 NPC 하나를 깐다(CNpcManager). 와이어엔 번호(npcId)만 실리고(등장 엔트리 defId)
//        이름·생김새·인사는 웹이 같은 파일을 읽어 번호로 찾는다(serve.js /data/NpcTable.csv). 서버는 이름을 로그에만 쓴다.
//
// [토글 없음] 맵·포털처럼 세계 데이터다(MapTable.h [토글 없음]). 값 줄 0 도 된다 — NPC 없는 세계.
//        상점 번호는 상점을 켠 빌드(USE_SHOP)만 상점 표와 대조한다(CheckShops) — 끈 빌드는 대조할 표가 없다.
//
// [맵 대조] 맵 표에 있는 맵 · 그 맵 안(오른쪽 끝은 폭 - 1 — 포털 표와 같은 선). 맵 표를 먼저 읽고 그 결과를 받는다.
//        같은 맵 포털과 손 닿는 범위가 겹치면 경고 한 줄(F 키가 둘에 걸린다 — 웹은 가까운 쪽) — 거절은 안 한다.
// ==========================================================================
struct NpcDef
{
    int32_t     npcId    = 0;
    int32_t     mapId    = 0;
    int32_t     x        = 0;   // 서 있는 자리(좌표)
    int32_t     y        = 0;
    int32_t     radius   = 0;   // 이 거리 안이면 말 걸고 사고판다(칸) — 식은 Reach.h
    std::string name;           // 이름표 — 웹이 그린다. 서버는 로그에만 쓴다
    std::string look;           // 생김새 — 웹만 읽는다(서버 무시)
    std::string greeting;       // 인사 한 줄 — 웹만 읽는다(말 걸기는 서버 왕복이 없다)
    int32_t     shopId   = 0;   // 상점 표(ShopTable.csv) 번호 · 0 = 상점 없음
    int32_t     fileLine = 0;   // 틀렸을 때 가리킬 줄
};

class CNpcTable
{
public:
    static constexpr int32_t MAX_NPC_ID = 10000;   // 와이어 defId(u16)에 들어가야 한다 — 오타 하나가 엉뚱한 큰 번호가 되지 않게
    static constexpr int32_t MAX_RADIUS = 10;      // 칸 — 포털과 같은 상한. 상점 판정이 NPC 를 찾는 섹터 범위도 이 값이다
    static constexpr int32_t MAX_SHOP_ID = 1000;   // 상점 번호 상한 — 상점 표의 상한과 같다(CShopTable::MAX_SHOP_ID 가 단언으로 맞춘다)

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    //   maps — 맵 번호·크기를 대조할 표(먼저 읽은 것)
    bool Load(const std::string& path, const CMapTable& maps);

#if USE_SHOP
    // 상점 번호 대조 — 0 이 아닌 번호는 상점 표에 있어야 한다. 틀린 줄마다 로그를 남기고 false.
    bool CheckShops(const CShopTable& shops) const;
#endif

    // 같은 맵 포털과 손 닿는 범위가 겹치는 줄마다 경고 한 줄(거절 아님) — 겹친 수를 돌려준다.
    int32_t WarnPortalOverlap(const CPortalTable& portals) const;

    // 없는 번호면 nullptr
    const NpcDef* Find(int32_t npcId) const;

    // 파일 순서 그대로 — 존이 깔 때 그 맵의 줄만 고른다(몇 줄뿐이라 맵별 색인을 따로 두지 않는다)
    const std::vector<NpcDef>& All() const { return _rows; }

private:
    std::vector<NpcDef> _rows;   // 파일 순서. 몇 줄뿐이라 찾기는 앞에서부터 훑는다(포털 표와 같은 꼴)
};
