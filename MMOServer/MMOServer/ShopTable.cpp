#include "ShopTable.h"

#if USE_SHOP

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"
#include "ItemTable.h"   // 아이템 번호·담는 자리·값 대조
#include "NpcTable.h"    // NPC 표의 상점 번호 상한과 맞추는 단언

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

static_assert(CShopTable::MAX_SHOP_ID == CNpcTable::MAX_SHOP_ID, "상점 번호 상한이 상점 표와 NPC 표에서 갈렸다");

bool CShopTable::Load(const std::string& path, const CItemTable& items)
{
    _shops.clear();
    _shopCount = 0;
    _lineCount = 0;

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[ShopTable] 파일을 못 엽니다: {}", path);
        return false;
    }
    // 값 줄 0 은 된다 — 상점 없는 세계(그때 NPC 표의 상점 번호는 전부 0 이어야 한다 — CNpcTable::CheckShops)

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 4)
        {
            SLOG_ERROR("[ShopTable] {}줄: 칸이 {}개입니다 — shopId,itemId,sells,buys 넷이 필요합니다", dl.fileLine, cell.size());
            return false;
        }

        int64_t shopId = 0, itemId = 0, sells = 0, buys = 0;
        if (!ParseInt(cell[0], shopId) || !ParseInt(cell[1], itemId) || !ParseInt(cell[2], sells) || !ParseInt(cell[3], buys))
        {
            SLOG_ERROR("[ShopTable] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
            return false;
        }

        // ⑴ 상점 번호 — 1 이상 · 상한 이하. 번호가 곧 색인이다
        if (shopId < 1 || shopId > MAX_SHOP_ID)
        {
            SLOG_ERROR("[ShopTable] {}줄: 상점 번호 {} 는 1~{} 여야 합니다", dl.fileLine, shopId, MAX_SHOP_ID);
            return false;
        }

        // ⑵ 아이템 — 표에 있다 · 돈 줄이 아니다 · 값이 있다(헤더 [아이템 대조])
        const ItemDef* def = (itemId > 0 && itemId <= CItemTable::MAX_ITEM_ID) ? items.Find(static_cast<int32_t>(itemId)) : nullptr;
        if (def == nullptr)
        {
            SLOG_ERROR("[ShopTable] {}줄: 아이템 {} 가 아이템 표(ItemTable.csv)에 없습니다 (상점 {})", dl.fileLine, itemId, shopId);
            return false;
        }
        if (def->store == ItemStore::MONEY)
        {
            SLOG_ERROR("[ShopTable] {}줄: 아이템 {} ({}) 은 돈 줄입니다 — 돈은 사고팔 물건이 아닙니다 (상점 {})", dl.fileLine, itemId, def->name, shopId);
            return false;
        }
        if (def->price <= 0)
        {
            SLOG_ERROR("[ShopTable] {}줄: 아이템 {} ({}) 의 값이 0 입니다 — 값 0 인 물건은 사고팔 수 없습니다 (상점 {})", dl.fileLine, itemId, def->name, shopId);
            return false;
        }

        // ⑶ 판다·사들인다 — 0 · 1 만, 둘 다 0 이면 쓸모없는 줄
        if ((sells != 0 && sells != 1) || (buys != 0 && buys != 1) || (sells == 0 && buys == 0))
        {
            SLOG_ERROR("[ShopTable] {}줄: sells {} · buys {} — 각각 0 · 1 이고 둘 다 0 이면 안 됩니다 (상점 {} 아이템 {})",
                       dl.fileLine, sells, buys, shopId, itemId);
            return false;
        }

        // ⑷ 같은 (상점, 아이템) 두 줄 금지 — 어느 줄을 따를지 모른다
        if (FindLine(static_cast<int32_t>(shopId), static_cast<int32_t>(itemId)) != nullptr)
        {
            SLOG_ERROR("[ShopTable] {}줄: 상점 {} 의 아이템 {} 줄이 둘입니다", dl.fileLine, shopId, itemId);
            return false;
        }

        if (static_cast<int64_t>(_shops.size()) <= shopId)
            _shops.resize(static_cast<size_t>(shopId) + 1);
        std::vector<ShopLine>& shop = _shops[static_cast<size_t>(shopId)];
        if (shop.empty())
            ++_shopCount;
        shop.push_back(ShopLine{ static_cast<int32_t>(itemId), sells == 1, buys == 1, dl.fileLine });
        ++_lineCount;
    }

    SLOG_INFO("[ShopTable] {} — 상점 {}개 · 줄 {}개", path, _shopCount, _lineCount);
    return true;
}

const ShopLine* CShopTable::FindLine(int32_t shopId, int32_t itemId) const
{
    if (shopId <= 0 || shopId >= static_cast<int32_t>(_shops.size()))
        return nullptr;
    for (const ShopLine& l : _shops[static_cast<size_t>(shopId)])
    {
        if (l.itemId == itemId)
            return &l;
    }
    return nullptr;
}

bool CShopTable::Has(int32_t shopId) const
{
    return shopId > 0 && shopId < static_cast<int32_t>(_shops.size()) && !_shops[static_cast<size_t>(shopId)].empty();
}

bool CShopTable::Sells(int32_t shopId, int32_t itemId) const
{
    const ShopLine* l = FindLine(shopId, itemId);
    return l != nullptr && l->sells;
}

bool CShopTable::Buys(int32_t shopId, int32_t itemId) const
{
    const ShopLine* l = FindLine(shopId, itemId);
    return l != nullptr && l->buys;
}

#endif // USE_SHOP
