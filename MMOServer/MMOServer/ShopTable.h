#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BuildConfig.h"   // USE_SHOP — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_SHOP

class CItemTable;

// ==========================================================================
// CShopTable — 상점마다 파는 것 · 사들이는 것 (Run/bin/ShopTable.csv · USE_SHOP)
//
// [무엇] 한 줄 = 「이 상점은 이 물건을 판다(sells) · 사들인다(buys)」. 값은 여기 없다 — 아이템 표의 price 하나가 정본이고
//        사들이는 값은 상점 규칙이 그 값에서 계산한다(CShopSystem::SellPriceOf). 어느 NPC 가 어느 상점인지는 NPC 표(shopId).
//        원본처럼 상점마다 목록이 다르고 「팔지 않고 사들이기만 하는 물건」이 있다(npc-shop-plan.md 따라 나온 것 2).
//
// [아이템 대조] 아이템 표에 있는 번호 · 돈 줄 금지(돈은 사고팔 물건이 아니다) · 값 0 금지(따라 나온 것 4) ·
//        파는 것도 사들이는 것도 아닌 줄 금지 · 같은 (상점, 아이템) 두 줄 금지. 아이템 표를 먼저 읽고 그 결과를 받는다.
// ==========================================================================
struct ShopLine
{
    int32_t itemId   = 0;
    bool    sells    = false;   // 이 상점에서 살 수 있다
    bool    buys     = false;   // 이 상점에 팔 수 있다
    int32_t fileLine = 0;
};

class CShopTable
{
public:
    static constexpr int32_t MAX_SHOP_ID = 1000;   // 번호가 곧 색인이다 — 오타 하나가 배열을 키우지 않게

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    //   items — 아이템 번호·담는 자리·값을 대조할 표(먼저 읽은 것)
    bool Load(const std::string& path, const CItemTable& items);

    bool Has(int32_t shopId) const;                      // 줄이 하나라도 있는 상점인가
    bool Sells(int32_t shopId, int32_t itemId) const;    // 그 상점이 이 물건을 파나
    bool Buys(int32_t shopId, int32_t itemId) const;     // 그 상점이 이 물건을 사들이나
    int32_t ShopCount() const { return _shopCount; }

private:
    const ShopLine* FindLine(int32_t shopId, int32_t itemId) const;

    std::vector<std::vector<ShopLine>> _shops;   // 색인 = 상점 번호 · 빈 목록 = 없는 상점. 상점당 몇 줄뿐이라 줄은 앞에서부터 훑는다
    int32_t _shopCount = 0;
    int32_t _lineCount = 0;
};

#endif // USE_SHOP
