#include "BuffTable.h"

#if USE_COMBAT

#include <utility>   // std::move — 줄을 표로 넘긴다

#include <Common/ErrorLog.h>

#include <Data/GameDataCsv.h>

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

namespace
{
    // 파일 글자 — BuffStat · BuffModKind 순서 그대로. 줄 수가 모자라면 아래 단언이 컴파일을 막는다.
    constexpr const char* STAT_NAMES[]     = { "attack", "defense", "max_hp", "max_mp", "move_speed", "root" };
    constexpr const char* MOD_KIND_NAMES[] = { "add", "pct", "mul" };
    static_assert(sizeof(STAT_NAMES) / sizeof(STAT_NAMES[0]) == static_cast<size_t>(BuffStat::COUNT),
                  "STAT_NAMES 는 BuffStat 마다 한 줄이어야 한다");
    static_assert(sizeof(MOD_KIND_NAMES) / sizeof(MOD_KIND_NAMES[0]) == static_cast<size_t>(BuffModKind::COUNT),
                  "MOD_KIND_NAMES 는 BuffModKind 마다 한 줄이어야 한다");

    // 글자 → 종류. 모르는 글자면 COUNT
    template <class E, size_t N>
    E ParseName(const std::string& s, const char* const (&names)[N])
    {
        for (size_t i = 0; i < N; ++i)
        {
            if (s == names[i])
                return static_cast<E>(i);
        }
        return E::COUNT;
    }

    // 방식마다 값의 범위가 다르다. 값이 셋이라 switch(하나가 나머지를 삼키지 않게)
    bool IsModValueInRange(BuffModKind kind, int64_t value)
    {
        switch (kind)
        {
        case BuffModKind::ADD: return value != 0 && value >= -CBuffTable::MAX_ADD && value <= CBuffTable::MAX_ADD;
        case BuffModKind::PCT: return value >= 1 && value <= CBuffTable::MAX_PCT;
        case BuffModKind::MUL: return value >= 2 && value <= CBuffTable::MAX_MUL;   // 1 은 아무 일도 안 한다
        case BuffModKind::COUNT: break;
        }
        return false;
    }

    // mods 칸 글자 → 수정 항목. 「스탯:방식:값」 을 ';' 로 이은 것(예: max_hp:pct:30;max_mp:pct:30). 틀리면 false 와 까닭.
    //   같은 스탯·방식이 둘이면 오타로 본다 · 1~MAX_BUFF_MODS 개.
    bool ParseMods(const std::string& text, BuffDef& d, std::string& why)
    {
        d.modCount = 0;
        size_t begin = 0;
        while (begin <= text.size())
        {
            const size_t end = text.find(';', begin);
            const std::string item = text.substr(begin, (end == std::string::npos ? text.size() : end) - begin);

            const size_t colon1 = item.find(':');
            const size_t colon2 = (colon1 == std::string::npos) ? std::string::npos : item.find(':', colon1 + 1);
            int64_t value = 0;
            if (colon2 == std::string::npos || !ParseInt(item.substr(colon2 + 1), value))
            {
                why = "항목 \"" + item + "\" 은 「스탯:방식:정수」 여야 합니다";
                return false;
            }
            const BuffStat stat = ParseName<BuffStat>(item.substr(0, colon1), STAT_NAMES);
            if (stat == BuffStat::COUNT)
            {
                why = "항목 \"" + item + "\" — 모르는 스탯입니다(attack · defense · max_hp · max_mp · move_speed · root)";
                return false;
            }
            const BuffModKind kind = ParseName<BuffModKind>(item.substr(colon1 + 1, colon2 - colon1 - 1), MOD_KIND_NAMES);
            if (kind == BuffModKind::COUNT)
            {
                why = "항목 \"" + item + "\" — 모르는 방식입니다(add · pct · mul)";
                return false;
            }
            // 묶임은 켜고 끄는 값이라 root:add:1 하나뿐이다 — 다른 꼴은 뜻이 없다
            if (stat == BuffStat::ROOT && (kind != BuffModKind::ADD || value != 1))
            {
                why = "항목 \"" + item + "\" — root 는 root:add:1 만 됩니다";
                return false;
            }
            if (!IsModValueInRange(kind, value))
            {
                why = "항목 \"" + item + "\" — 값이 범위 밖입니다(add 0 아님·±" + std::to_string(CBuffTable::MAX_ADD) +
                      " · pct 1~" + std::to_string(CBuffTable::MAX_PCT) + " · mul 2~" + std::to_string(CBuffTable::MAX_MUL) + ")";
                return false;
            }
            for (int32_t i = 0; i < d.modCount; ++i)
            {
                if (d.mods[i].stat == stat && d.mods[i].kind == kind)
                {
                    why = "항목 \"" + item + "\" 의 스탯·방식이 겹칩니다";
                    return false;
                }
            }
            if (d.modCount >= MAX_BUFF_MODS)
            {
                why = "항목이 " + std::to_string(MAX_BUFF_MODS) + " 개를 넘습니다";
                return false;
            }
            d.mods[d.modCount++] = BuffMod{ stat, kind, static_cast<int32_t>(value) };

            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        return true;
    }
}

bool CBuffTable::Load(const std::string& path)
{
    _rows.clear();
    _rowById.clear();

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[BuffTable] 파일을 못 엽니다: {}", path);
        return false;
    }
    if (lines.empty())
    {
        SLOG_ERROR("[BuffTable] 값 줄이 하나도 없습니다: {}", path);
        return false;
    }

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() != 4)
        {
            SLOG_ERROR("[BuffTable] {}줄: 칸이 {}개입니다 — buffId,name,durationMs,mods 넷이어야 합니다", dl.fileLine, cell.size());
            return false;
        }

        int64_t buffId = 0, durationMs = 0;
        if (!ParseInt(cell[0], buffId) || !ParseInt(cell[2], durationMs))
        {
            SLOG_ERROR("[BuffTable] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
            return false;
        }

        // ⑴ 번호 — 1~MAX_BUFF_ID(와이어 u16) · 중복 없음 · 줄 수 상한. 번호는 이름표라 띄엄띄엄이어도 된다(색인이 아니다)
        if (buffId <= 0 || buffId > MAX_BUFF_ID)
        {
            SLOG_ERROR("[BuffTable] {}줄: 버프 번호 {} 는 1~{} 여야 합니다", dl.fileLine, buffId, MAX_BUFF_ID);
            return false;
        }
        if (_rowById.count(static_cast<int32_t>(buffId)) != 0)
        {
            SLOG_ERROR("[BuffTable] {}줄: 버프 번호 {} 가 겹칩니다", dl.fileLine, buffId);
            return false;
        }
        if (static_cast<int32_t>(_rows.size()) >= MAX_BUFF_ROWS)
        {
            SLOG_ERROR("[BuffTable] {}줄: 버프 줄이 {} 개를 넘습니다 — 지표 칸 수·DB 묶음 크기(MAX_BUFF_ROWS)를 올릴 것 (버프 {})", dl.fileLine, MAX_BUFF_ROWS, buffId);
            return false;
        }

        // ⑵ 이름 — 로그에 쓴다
        if (cell[1].empty())
        {
            SLOG_ERROR("[BuffTable] {}줄: 이름이 비어 있습니다 (버프 {})", dl.fileLine, buffId);
            return false;
        }

        // ⑶ 지속 — 1ms~상한. 0 이면 걸자마자 풀린다
        if (durationMs < 1 || durationMs > MAX_DURATION_MS)
        {
            SLOG_ERROR("[BuffTable] {}줄: durationMs {} 는 1~{} 여야 합니다 (버프 {})", dl.fileLine, durationMs, MAX_DURATION_MS, buffId);
            return false;
        }

        // ⑷ 수정 항목 — 하나 이상(아무것도 안 바꾸는 버프는 오타다)
        BuffDef d;
        std::string why;
        if (cell[3].empty())
            why = "항목이 비어 있습니다";
        if (!why.empty() || !ParseMods(cell[3], d, why))
        {
            SLOG_ERROR("[BuffTable] {}줄: mods {} (버프 {})", dl.fileLine, why, buffId);
            return false;
        }

        d.buffId         = static_cast<int32_t>(buffId);
        d.index          = static_cast<int32_t>(_rows.size());
        d.name           = cell[1];
        d.durationMs     = static_cast<int32_t>(durationMs);
        // 올림 — 내림이면 틱 주기를 바꿀 때 지속이 짧아질 수 있다(스킬 쿨타임과 같은 까닭). 30000 → 750
        d.durationFrames = static_cast<uint64_t>((durationMs + FRAME_INTERVAL_MS - 1) / FRAME_INTERVAL_MS);

        _rowById[d.buffId] = _rows.size();
        _rows.push_back(std::move(d));
    }

    SLOG_INFO("[BuffTable] {} — 버프 {}개", path, _rows.size());
    return true;
}

#endif // USE_COMBAT
