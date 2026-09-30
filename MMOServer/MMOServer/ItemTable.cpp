#include "ItemTable.h"

#if USE_INVENTORY

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
    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 7)
        {
            SLOG_ERROR("[ItemTable] {}줄: 칸이 {}개입니다 — itemId,name,tab,maxStack,healHp,icon,attack 일곱이 필요합니다",
                       dl.fileLine, cell.size());
            return false;
        }

        int64_t itemId = 0, maxStack = 0, healHp = 0, icon = 0, attack = 0;
        if (!ParseInt(cell[0], itemId) || !ParseInt(cell[3], maxStack) ||
            !ParseInt(cell[4], healHp) || !ParseInt(cell[5], icon) || !ParseInt(cell[6], attack))
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

        // ⑶ 탭 — 모르는 글자면 칸 구간이 안 정해진다
        const InvTab tab = ParseTab(cell[2]);
        if (tab == InvTab::COUNT)
        {
            SLOG_ERROR("[ItemTable] {}줄: 모르는 탭 \"{}\" — equip · use · etc 중 하나여야 합니다",
                       dl.fileLine, cell[2]);
            return false;
        }

        // ⑷ 쌓임 한도 — 1 미만이면 넣을 수가 없다. 장비는 1 이어야 한다(장비는 안 쌓인다)
        if (maxStack < 1 || maxStack > 9999)
        {
            SLOG_ERROR("[ItemTable] {}줄: maxStack {} 는 1~9999 여야 합니다 (아이템 {})",
                       dl.fileLine, maxStack, itemId);
            return false;
        }
        if (tab == InvTab::EQUIP && maxStack != 1)
        {
            SLOG_ERROR("[ItemTable] {}줄: 장비는 maxStack 이 1 이어야 합니다 — 지금 {} (아이템 {})",
                       dl.fileLine, maxStack, itemId);
            return false;
        }

        // ⑸ 회복량 — 음수 금지. 소비 아이템은 써서 무슨 일이 일어나야 한다
        if (healHp < 0 || (tab == InvTab::USE && healHp <= 0))
        {
            SLOG_ERROR("[ItemTable] {}줄: healHp {} — 소비 아이템은 1 이상, 그 밖은 0 이상이어야 합니다 (아이템 {})",
                       dl.fileLine, healHp, itemId);
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

        if (static_cast<int64_t>(_rows.size()) <= itemId)
            _rows.resize(static_cast<size_t>(itemId) + 1);

        ItemDef& d = _rows[static_cast<size_t>(itemId)];
        d.itemId   = static_cast<int32_t>(itemId);
        d.name     = cell[1];
        d.tab      = tab;
        d.maxStack = static_cast<int32_t>(maxStack);
        d.healHp   = static_cast<int32_t>(healHp);
        d.attack   = static_cast<int32_t>(attack);
        ++_count;
        ++perTab[static_cast<int>(tab)];
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
    SLOG_INFO("[ItemTable] {} — 아이템 {}개 ({})", path, _count, byTab);
    return true;
}

#endif // USE_INVENTORY
