#include "DropTable.h"

#if USE_DROP

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"
#include "ItemTable.h"
#include "Monster.h"   // MonsterKind — 파일의 몬스터 이름을 종류 번호로

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

const std::vector<DropRow> CDropTable::EMPTY;

namespace
{
    struct KindName { const char* file; MonsterKind kind; };

    // 파일 속 몬스터 이름 → 종류. 종류가 늘면 여기 한 줄(Monster.h MonsterKind 와 나란히 볼 것).
    //   글자 → enum 은 이 표를 돈다(종류마다 if 를 쓰지 않는다).
    constexpr KindName KIND_NAMES[] = {
        { "snake", MonsterKind::SNAKE },
    };

    bool ParseKind(const std::string& s, MonsterKind& out)
    {
        for (const KindName& k : KIND_NAMES)
        {
            if (s == k.file)
            {
                out = k.kind;
                return true;
            }
        }
        return false;
    }

    // "60" · "0.5" · "2.25" → 만분율(6000 · 50 · 225). 소수 셋째 자리 이상 · 음수 · 숫자 아님이면 false.
    //   0 과 100 초과는 여기서 막지 않는다 — 부르는 쪽이 까닭을 따로 말한다.
    bool ParseChanceBp(const std::string& s, int64_t& out)
    {
        const size_t dot = s.find('.');
        const std::string ip = s.substr(0, dot);
        const std::string fp = (dot == std::string::npos) ? std::string() : s.substr(dot + 1);
        if (ip.empty() || ip.size() > 3 || fp.size() > 2 || (dot != std::string::npos && fp.empty()))
            return false;

        int64_t v = 0;
        for (char c : ip)
        {
            if (c < '0' || c > '9')
                return false;
            v = v * 10 + (c - '0');
        }
        v *= 100;
        int64_t scale = 10;
        for (char c : fp)
        {
            if (c < '0' || c > '9')
                return false;
            v += (c - '0') * scale;
            scale /= 10;
        }
        out = v;
        return true;
    }
}

bool CDropTable::Load(const std::string& path, const CItemTable& items)
{
    _byKind.clear();
    _count = 0;

    size_t kinds = 0;
    for (const KindName& k : KIND_NAMES)
    {
        if (static_cast<size_t>(k.kind) + 1 > kinds)
            kinds = static_cast<size_t>(k.kind) + 1;
    }
    _byKind.resize(kinds);

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[DropTable] 파일을 못 엽니다: {}", path);
        return false;
    }
    if (lines.empty())
    {
        SLOG_ERROR("[DropTable] 값 줄이 하나도 없습니다: {}", path);
        return false;
    }

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 5)
        {
            SLOG_ERROR("[DropTable] {}줄: 칸이 {}개입니다 — itemId,monster,chance,min,max 다섯이 필요합니다",
                       dl.fileLine, cell.size());
            return false;
        }

        int64_t itemId = 0, minCount = 0, maxCount = 0, chanceBp = 0;
        if (!ParseInt(cell[0], itemId) || !ParseInt(cell[3], minCount) || !ParseInt(cell[4], maxCount))
        {
            SLOG_ERROR("[DropTable] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
            return false;
        }

        // ⑴ 몬스터 — 모르는 이름이면 어느 몬스터의 줄인지 모른다
        MonsterKind kind{};
        if (!ParseKind(cell[1], kind))
        {
            SLOG_ERROR("[DropTable] {}줄: 모르는 몬스터 \"{}\" — snake 여야 합니다", dl.fileLine, cell[1]);
            return false;
        }

        // ⑵ 아이템 — 아이템 표에 있어야 한다(표끼리 대조)
        const ItemDef* def = items.Find(static_cast<int32_t>(itemId));
        if (def == nullptr)
        {
            SLOG_ERROR("[DropTable] {}줄: 아이템 {} 은 아이템 표(ItemTable.csv)에 없습니다", dl.fileLine, itemId);
            return false;
        }

        // ⑶ 확률 — 백분율, 소수 둘째 자리까지. 0 이면 표에 둘 이유가 없고, 100 을 넘으면 뜻이 없다
        if (!ParseChanceBp(cell[2], chanceBp))
        {
            SLOG_ERROR("[DropTable] {}줄: chance \"{}\" — 백분율 숫자(소수 둘째 자리까지)여야 합니다", dl.fileLine, cell[2]);
            return false;
        }
        if (chanceBp < 1 || chanceBp > 10000)
        {
            SLOG_ERROR("[DropTable] {}줄: chance {} — 0 보다 크고 100 이하여야 합니다", dl.fileLine, cell[2]);
            return false;
        }

        // ⑷ 개수 — 1 ≤ min ≤ max ≤ 그 아이템의 쌓임 한도(한 줄이 한 칸에 들어가게)
        if (minCount < 1 || minCount > maxCount || maxCount > def->maxStack)
        {
            SLOG_ERROR("[DropTable] {}줄: 개수 {}~{} — 1 ≤ min ≤ max ≤ {}(아이템 {} 의 maxStack) 이어야 합니다",
                       dl.fileLine, minCount, maxCount, def->maxStack, itemId);
            return false;
        }

        // ⑸ 겹침 — 같은 몬스터·같은 아이템 줄이 둘이면 따로 굴려져 확률이 뜻과 달라진다
        std::vector<DropRow>& rows = _byKind[static_cast<size_t>(kind)];
        for (const DropRow& r : rows)
        {
            if (r.itemId == itemId)
            {
                SLOG_ERROR("[DropTable] {}줄: {} · 아이템 {} 줄이 겹칩니다", dl.fileLine, cell[1], itemId);
                return false;
            }
        }

        DropRow row;
        row.itemId   = static_cast<int32_t>(itemId);
        row.chanceBp = static_cast<int32_t>(chanceBp);
        row.minCount = static_cast<int32_t>(minCount);
        row.maxCount = static_cast<int32_t>(maxCount);
        rows.push_back(row);
        ++_count;
    }

    SLOG_INFO("[DropTable] {} — 줄 {}개", path, _count);
    return true;
}

#endif // USE_DROP
