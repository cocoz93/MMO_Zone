#include "ItemTable.h"

#if USE_INVENTORY

#include <utility>   // move — 다 본 행을 표에 넣을 때

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

namespace
{
    struct TabNames { const char* file; const char* display; };

    // InvTab 순서 그대로 — 줄 수가 모자라면 아래 단언이 컴파일을 막는다.
    constexpr TabNames TAB_NAMES[] = {
        { "equip", "장비" },
        { "use",   "소비" },
        { "etc",   "기타" },
    };
    static_assert(sizeof(TAB_NAMES) / sizeof(TAB_NAMES[0]) == static_cast<unsigned>(InvTab::COUNT),
                  "TAB_NAMES 는 InvTab 마다 한 줄이어야 한다");

    // 글자 → 탭. 모르는 글자면 InvTab::COUNT
    InvTab ParseTab(const std::string& s)
    {
        for (unsigned i = 0; i < static_cast<unsigned>(InvTab::COUNT); ++i)
        {
            if (s == TAB_NAMES[i].file)
                return static_cast<InvTab>(i);
        }
        return InvTab::COUNT;
    }
}

const char* InvTabFileName(InvTab tab)
{
    const unsigned i = static_cast<unsigned>(tab);
    return i < static_cast<unsigned>(InvTab::COUNT) ? TAB_NAMES[i].file : "?";
}

const char* InvTabDisplayName(InvTab tab)
{
    const unsigned i = static_cast<unsigned>(tab);
    return i < static_cast<unsigned>(InvTab::COUNT) ? TAB_NAMES[i].display : "?";
}

bool CItemTable::Load(const std::string& path)
{
    _rows.clear();
    _count = 0;

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[ItemTable] 파일을 못 엽니다: {}", path);
        return false;
    }
    if (lines.empty())
    {
        SLOG_ERROR("[ItemTable] 값 줄이 하나도 없습니다: {}", path);
        return false;
    }

    int32_t perTab[static_cast<int>(InvTab::COUNT)] = {};
    int32_t moneyRows = 0;   // 돈 줄 수 — 하나만(헤더 [담는 자리])
    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 10)
        {
            SLOG_ERROR("[ItemTable] {}줄: 칸이 {}개입니다 — itemId,name,tab,maxStack,healHp,icon,attack,healMp,price,trade 열 개가 필요합니다",
                       dl.fileLine, cell.size());
            return false;
        }

        int64_t itemId = 0, maxStack = 0, healHp = 0, icon = 0, attack = 0, healMp = 0, price = 0, trade = 0;
        if (!ParseInt(cell[0], itemId) || !ParseInt(cell[3], maxStack) ||
            !ParseInt(cell[4], healHp) || !ParseInt(cell[5], icon) || !ParseInt(cell[6], attack) ||
            !ParseInt(cell[7], healMp) || !ParseInt(cell[8], price) || !ParseInt(cell[9], trade))
        {
            SLOG_ERROR("[ItemTable] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
            return false;
        }

        // ⑴ 번호 — 1 이상 · 상한 이하 · 중복 없음. 번호가 곧 색인이다
        if (itemId <= 0 || itemId > MAX_ITEM_ID)
        {
            SLOG_ERROR("[ItemTable] {}줄: 아이템 번호 {} 는 1~{} 여야 합니다", dl.fileLine, itemId, MAX_ITEM_ID);
            return false;
        }
        if (itemId < static_cast<int64_t>(_rows.size()) && _rows[static_cast<size_t>(itemId)].itemId != 0)
        {
            SLOG_ERROR("[ItemTable] {}줄: 아이템 번호 {} 가 겹칩니다", dl.fileLine, itemId);
            return false;
        }

        // ⑵ 이름 — 클라가 그릴 게 없다
        if (cell[1].empty())
        {
            SLOG_ERROR("[ItemTable] {}줄: 이름이 비어 있습니다 (아이템 {})", dl.fileLine, itemId);
            return false;
        }

        // ⑶ 탭 — 모르는 글자면 칸 구간이 안 정해진다. money 는 탭이 아니라 담는 자리다(헤더 [담는 자리] — 돈 줄의 tab 은 COUNT)
        const bool   isMoney = (cell[2] == "money");
        const InvTab tab     = isMoney ? InvTab::COUNT : ParseTab(cell[2]);
        if (!isMoney && tab == InvTab::COUNT)
        {
            SLOG_ERROR("[ItemTable] {}줄: 모르는 탭 \"{}\" — equip · use · etc · money 중 하나여야 합니다",
                       dl.fileLine, cell[2]);
            return false;
        }
        if (isMoney && ++moneyRows > 1)
        {
            SLOG_ERROR("[ItemTable] {}줄: 돈(money) 줄이 둘째입니다 — 돈 줄은 하나만 (아이템 {})", dl.fileLine, itemId);
            return false;
        }

        // ⑷ 쌓임 한도 — 1 미만이면 넣을 수가 없다. 장비는 1 이어야 한다(장비는 안 쌓인다). 돈은 땅 한 무더기 최대까지
        const int64_t stackMax = isMoney ? MAX_MONEY_STACK : MAX_STACK;
        if (maxStack < 1 || maxStack > stackMax)
        {
            SLOG_ERROR("[ItemTable] {}줄: maxStack {} 는 1~{} 여야 합니다 (아이템 {})",
                       dl.fileLine, maxStack, stackMax, itemId);
            return false;
        }
        if (tab == InvTab::EQUIP && maxStack != 1)
        {
            SLOG_ERROR("[ItemTable] {}줄: 장비는 maxStack 이 1 이어야 합니다 — 지금 {} (아이템 {})",
                       dl.fileLine, maxStack, itemId);
            return false;
        }

        // ⑸ 회복량 — 음수 금지. 「소비 아이템은 써서 무슨 일이 일어나야 한다」는 아래 ⑻ 이 ItemDef::HasEffect 로 본다
        if (healHp < 0 || healMp < 0)
        {
            SLOG_ERROR("[ItemTable] {}줄: healHp {} · healMp {} 는 0 이상이어야 합니다 (아이템 {})", dl.fileLine, healHp, healMp, itemId);
            return false;
        }
        //    한 물약 한 효과 — 둘 다 채우는 줄은 거부한다(10-10). 성공 알림이 효과마다 받는 사람이 달라서
        //    (체력은 주변 방송, 마력은 본인만) 한 줄에 둘이면 알림이 「둘 중 하나」가 아니라 조합이 된다.
        if (healHp > 0 && healMp > 0)
        {
            SLOG_ERROR("[ItemTable] {}줄: healHp {} 와 healMp {} 가 둘 다 있습니다 — 한 물약은 한 효과만 (아이템 {})",
                       dl.fileLine, healHp, healMp, itemId);
            return false;
        }

        // ⑹ 그림 번호 — 서버는 안 쓰지만 음수면 웹 클라가 그릴 칸을 못 찾는다
        if (icon < 0)
        {
            SLOG_ERROR("[ItemTable] {}줄: icon {} 는 0 이상이어야 합니다 (아이템 {})", dl.fileLine, icon, itemId);
            return false;
        }

        // ⑺ 착용 공격력 — 음수 금지. 장비가 아닌데 값이 있으면 끼울 수 없는 아이템의 값이라 아무 일도 안 한다(⑸ 와 같은 결)
        if (attack < 0 || attack > 9999 || (tab != InvTab::EQUIP && attack != 0))
        {
            SLOG_ERROR("[ItemTable] {}줄: attack {} — 장비는 0~9999, 그 밖은 0 이어야 합니다 (아이템 {})",
                       dl.fileLine, attack, itemId);
            return false;
        }

        // ⑻ 값 — 0~돈 상한. 돈 줄은 효과·값이 없다(쓰거나 사고팔 물건이 아니라 돈 그 자체)
        if (price < 0 || price > MONEY_MAX)
        {
            SLOG_ERROR("[ItemTable] {}줄: price {} 는 0~{} 여야 합니다 (아이템 {})", dl.fileLine, price, MONEY_MAX, itemId);
            return false;
        }
        if (isMoney && (healHp != 0 || healMp != 0 || price != 0))
        {
            SLOG_ERROR("[ItemTable] {}줄: 돈(money) 줄은 healHp · healMp · price 가 0 이어야 합니다 — 지금 {} · {} · {} (아이템 {})",
                       dl.fileLine, healHp, healMp, price, itemId);
            return false;
        }

        // ⑻-2 거래 — 1(올릴 수 있다) 또는 0(못 올린다). 그 밖의 값은 오타다(2 를 「더 잘 된다」로 읽게 두지 않는다)
        if (trade != 0 && trade != 1)
        {
            SLOG_ERROR("[ItemTable] {}줄: trade {} 는 0 또는 1 이어야 합니다 (아이템 {})", dl.fileLine, trade, itemId);
            return false;
        }

        ItemDef d;
        d.itemId   = static_cast<int32_t>(itemId);
        d.name     = cell[1];
        d.store    = isMoney ? ItemStore::MONEY : ItemStore::SLOT;
        d.tab      = tab;
        d.maxStack = static_cast<int32_t>(maxStack);
        d.healHp   = static_cast<int32_t>(healHp);
        d.healMp   = static_cast<int32_t>(healMp);
        d.attack   = static_cast<int32_t>(attack);
        d.price    = static_cast<int32_t>(price);
        d.tradable = (trade == 1);

        // ⑼ 소비 아이템은 써서 무슨 일이 일어나야 한다 — 「효과가 있나」는 ItemDef::HasEffect 하나만 답한다.
        //    「이 빌드에서 쓸 수 있나」(IsUsable)를 묻지 않는 까닭 — 스킬을 끈 빌드도 마력 물약 줄을 받아야 서버가 뜬다(ItemTable.h)
        if (d.tab == InvTab::USE && !d.HasEffect())
        {
            SLOG_ERROR("[ItemTable] {}줄: 소비 아이템인데 쓸 효과가 없습니다 — healHp 나 healMp 가 1 이상이어야 합니다 (아이템 {})",
                       dl.fileLine, itemId);
            return false;
        }

        if (static_cast<int64_t>(_rows.size()) <= itemId)
            _rows.resize(static_cast<size_t>(itemId) + 1);
        _rows[static_cast<size_t>(itemId)] = std::move(d);
        ++_count;
        if (!isMoney)
            ++perTab[static_cast<int>(tab)];   // 돈 줄의 탭은 COUNT — 색인으로 쓰면 배열 밖이다
    }

    // 탭별 개수 — 탭 이름을 박아 쓰지 않고 표를 돈다(넷째 탭이 생겨도 로그에서 안 빠진다)
    std::string byTab;
    for (unsigned i = 0; i < static_cast<unsigned>(InvTab::COUNT); ++i)
    {
        if (i > 0)
            byTab += " · ";
        byTab += TAB_NAMES[i].display;
        byTab += ' ';
        byTab += std::to_string(perTab[i]);
    }
    SLOG_INFO("[ItemTable] {} — 아이템 {}개 ({} · 돈 {})", path, _count, byTab, moneyRows);
    return true;
}

#endif // USE_INVENTORY
