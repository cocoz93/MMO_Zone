#include "MapTable.h"

#include <Common/ErrorLog.h>

#include "GameDataCsv.h"

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;

namespace
{
    // 맵 종류 이름표 — MapKind 순서 그대로. 표의 kind 칸 글자가 이 중 하나여야 한다
    constexpr const char* KIND_NAMES[] = { "town", "dungeon", "instance" };
    static_assert(sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0]) == static_cast<size_t>(MapKind::COUNT),
                  "KIND_NAMES 는 MapKind 마다 한 줄이어야 한다");

    bool ParseKind(const std::string& s, MapKind& out)
    {
        for (size_t i = 0; i < static_cast<size_t>(MapKind::COUNT); ++i)
        {
            if (s == KIND_NAMES[i])
            {
                out = static_cast<MapKind>(i);
                return true;
            }
        }
        return false;
    }
}

bool CMapTable::Load(const std::string& path)
{
    _rows.clear();

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[MapTable] 파일을 못 엽니다: {}", path);
        return false;
    }
    if (lines.empty())
    {
        SLOG_ERROR("[MapTable] 값 줄이 하나도 없습니다 — 맵이 하나는 있어야 합니다: {}", path);
        return false;
    }

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 4)
        {
            SLOG_ERROR("[MapTable] {}줄: 칸이 {}개입니다 — mapId,width,height,kind 넷이 필요합니다", dl.fileLine, cell.size());
            return false;
        }

        int64_t mapId = 0, width = 0, height = 0;
        if (!ParseInt(cell[0], mapId) || !ParseInt(cell[1], width) || !ParseInt(cell[2], height))
        {
            SLOG_ERROR("[MapTable] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
            return false;
        }

        // ⑴ 번호 — 0 이상 · 상한 이하 · 중복 없음
        if (mapId < 0 || mapId > MAX_MAP_ID)
        {
            SLOG_ERROR("[MapTable] {}줄: 맵 번호 {} 는 0~{} 여야 합니다", dl.fileLine, mapId, MAX_MAP_ID);
            return false;
        }
        if (Find(static_cast<int32_t>(mapId)) != nullptr)
        {
            SLOG_ERROR("[MapTable] {}줄: 맵 번호 {} 가 겹칩니다", dl.fileLine, mapId);
            return false;
        }

        // ⑵ 크기 — 0 이면 설 자리가 없다
        if (width < 1 || width > MAX_SIZE || height < 1 || height > MAX_SIZE)
        {
            SLOG_ERROR("[MapTable] {}줄: 크기 {}×{} — 폭·높이는 1~{} 여야 합니다 (맵 {})",
                       dl.fileLine, width, height, MAX_SIZE, mapId);
            return false;
        }

        // ⑶ 종류 — 이름표 글자 중 하나
        MapKind kind = MapKind::DUNGEON;
        if (!ParseKind(cell[3], kind))
        {
            SLOG_ERROR("[MapTable] {}줄: 종류 \"{}\" 를 모릅니다 — town · dungeon · instance 중 하나 (맵 {})", dl.fileLine, cell[3], mapId);
            return false;
        }

        // ⑷ 시작 구역 — 넷 다 쓰거나 다 비운다. 비우는 꼴은 칸을 아예 안 쓰거나(칸 4개) 「,,,」 처럼 빈 칸으로 둔다
        //    (SplitCsv 는 맨 끝 빈 칸 하나만 안 담으므로 빈 칸으로 비우면 칸 수가 5~7 이 된다 — 그래서 수가 아니라 내용으로 가른다).
        //    맵 안(0 ~ 폭-1)이고 시작 ≤ 끝. 마을은 반드시 있어야 한다 — 새 캐릭터·부활이 설 자리다.
        //    뒤에 아홉째 칸(lockoutSec — ⑸)이 붙을 수 있어 시작 구역은 다섯째~여덟째 칸(4~7)만 센다.
        if (cell.size() > 9)
        {
            SLOG_ERROR("[MapTable] {}줄: 칸이 {}개입니다 — 많아야 아홉(mapId,width,height,kind,startX0,startY0,startX1,startY1,lockoutSec) (맵 {})",
                       dl.fileLine, cell.size(), mapId);
            return false;
        }
        StartArea start;
        size_t filled = 0;
        for (size_t i = 4; i < cell.size() && i < 8; ++i)
        {
            if (!cell[i].empty())
                ++filled;
        }
        if (filled != 0 && filled != 4)
        {
            SLOG_ERROR("[MapTable] {}줄: 시작 구역(startX0,startY0,startX1,startY1)은 넷 다 쓰거나 다 비웁니다 — 칸 {}개 중 값 {}개 (맵 {})",
                       dl.fileLine, cell.size(), filled, mapId);
            return false;
        }
        if (filled == 4)
        {
            int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            if (!ParseInt(cell[4], x0) || !ParseInt(cell[5], y0) || !ParseInt(cell[6], x1) || !ParseInt(cell[7], y1))
            {
                SLOG_ERROR("[MapTable] {}줄: 시작 구역 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
                return false;
            }
            if (x0 < 0 || y0 < 0 || x0 > x1 || y0 > y1 || x1 > width - 1 || y1 > height - 1)
            {
                SLOG_ERROR("[MapTable] {}줄: 시작 구역 ({},{})~({},{}) — 맵 안(0~{}, 0~{})이고 시작 ≤ 끝이어야 합니다 (맵 {})",
                           dl.fileLine, x0, y0, x1, y1, width - 1, height - 1, mapId);
                return false;
            }
            start.has = true;
            start.x0  = static_cast<int32_t>(x0);
            start.y0  = static_cast<int32_t>(y0);
            start.x1  = static_cast<int32_t>(x1);
            start.y1  = static_cast<int32_t>(y1);
        }
        if (kind == MapKind::TOWN && !start.has)
        {
            SLOG_ERROR("[MapTable] {}줄: 마을(town)은 시작 구역이 있어야 합니다 — 새 캐릭터와 부활이 설 자리 (맵 {})", dl.fileLine, mapId);
            return false;
        }

        // ⑸ 다시 들어가기 잠금(lockoutSec) — 방 맵(instance)만 쓰고 반드시 1~상한, 다른 종류는 비운다.
        //    방은 보스가 한 번만 깔려서, 잠금이 없으면 잡고 나와 바로 다시 들어가 새 보스를 깐다(room-plan.md 「사람이 정할 것」 2).
        const bool hasLockout = cell.size() == 9 && !cell[8].empty();
        int64_t lockoutSec = 0;
        if (hasLockout && !ParseInt(cell[8], lockoutSec))
        {
            SLOG_ERROR("[MapTable] {}줄: lockoutSec 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
            return false;
        }
        if (kind == MapKind::INSTANCE)
        {
            if (!hasLockout || lockoutSec < 1 || lockoutSec > MAX_LOCKOUT_SEC)
            {
                SLOG_ERROR("[MapTable] {}줄: 방 맵(instance)은 lockoutSec 이 1~{} 여야 합니다 — 지금 {} (맵 {})",
                           dl.fileLine, MAX_LOCKOUT_SEC, hasLockout ? cell[8] : std::string("빈 칸"), mapId);
                return false;
            }
        }
        else if (hasLockout)
        {
            SLOG_ERROR("[MapTable] {}줄: lockoutSec 은 방 맵(instance)만 씁니다 — {} 맵은 비웁니다 (맵 {})",
                       dl.fileLine, cell[3], mapId);
            return false;
        }

        MapDef d;
        d.mapId    = static_cast<int32_t>(mapId);
        d.width    = static_cast<int32_t>(width);
        d.height   = static_cast<int32_t>(height);
        d.kind     = kind;
        d.start    = start;
        d.lockoutSec = static_cast<int32_t>(lockoutSec);
        d.fileLine = dl.fileLine;
        _rows.push_back(d);
    }

    // ⑹ 첫 줄은 마을 — 첫 줄이 기본 맵이고(새 캐릭터가 들어가는 곳), 죽으면 그 맵의 시작 구역에서 되살아난다(CGameServer::TickRespawn).
    //    던전이 첫 줄이면 새 캐릭터가 뱀 한가운데 무작위로 떨어지고 부활도 갈 곳이 없다
    if (_rows.front().kind != MapKind::TOWN)
    {
        SLOG_ERROR("[MapTable] {}줄: 첫 줄(맵 {})은 마을(town)이어야 합니다 — 새 캐릭터가 들어가고 죽으면 돌아오는 맵이다",
                   _rows.front().fileLine, _rows.front().mapId);
        return false;
    }

    SLOG_INFO("[MapTable] {} — 맵 {}개", path, _rows.size());
    return true;
}

const MapDef* CMapTable::Find(int32_t mapId) const
{
    for (const MapDef& d : _rows)
    {
        if (d.mapId == mapId)
            return &d;
    }
    return nullptr;
}
