#include "MonsterTable.h"

#if USE_MONSTER

#include <Common/ErrorLog.h>
#include <Protocol/Protocol.h>   // DISPLAY_MONSTER_BASE — 겉모습 값의 구간

#include "Combat.h"          // MAX_BODY_RADIUS — 몸 반지름 상한 (USE_COMBAT 밖 상수)
#include "GameDataCsv.h"

using GameDataCsv::DataLine;
using GameDataCsv::ReadDataLines;
using GameDataCsv::SplitCsv;
using GameDataCsv::ParseInt;
using GameDataCsv::ParseFloat;

namespace
{
    // 몬스터 겉모습 구간의 끝 — Protocol.h 「0x01~0x0F = 몬스터 종류」(DISPLAY_MONSTER_BASE 주석).
    //   그 헤더는 클라들과 같이 쓰는 와이어 정의라 끝 값을 거기 새로 두지 않고 여기 적는다.
    constexpr int64_t DISPLAY_MONSTER_LAST = 0x0F;
}

bool CMonsterTable::Load(const std::string& path)
{
    _rows.clear();
    _count = 0;

    std::vector<DataLine> lines;
    if (!ReadDataLines(path, lines))
    {
        SLOG_ERROR("[MonsterTable] 파일을 못 엽니다: {}", path);
        return false;
    }
    if (lines.empty())
    {
        SLOG_ERROR("[MonsterTable] 값 줄이 하나도 없습니다: {}", path);
        return false;
    }

    std::vector<std::string> cell;
    for (const DataLine& dl : lines)
    {
        SplitCsv(dl.text, cell);
        if (cell.size() < 8)
        {
            SLOG_ERROR("[MonsterTable] {}줄: 칸이 {}개입니다 — monsterId,name,maxHp,attack,defense,speed,exp,display 여덟(+ bodyRadius, boss)이 필요합니다",
                       dl.fileLine, cell.size());
            return false;
        }

        // 아홉째 bodyRadius 칸은 옛 줄에 없어도 된다(없거나 비면 0 = 점). 끝의 빈 칸은 SplitCsv 가 안 담는다
        //   열째 boss 칸도 같다(없거나 비면 0) — 보스가 아닌 줄은 아홉 칸 그대로 둔다
        const std::string radiusText = (cell.size() > 8) ? cell[8] : std::string();
        const std::string bossText   = (cell.size() > 9) ? cell[9] : std::string();
        int64_t monsterId = 0, maxHp = 0, attack = 0, defense = 0, speed = 0, exp = 0, display = 0, boss = 0;
        double  bodyRadius = 0.0;
        if (!ParseInt(cell[0], monsterId) || !ParseInt(cell[2], maxHp) || !ParseInt(cell[3], attack) ||
            !ParseInt(cell[4], defense) || !ParseInt(cell[5], speed) || !ParseInt(cell[6], exp) || !ParseInt(cell[7], display) ||
            (!radiusText.empty() && !ParseFloat(radiusText, bodyRadius)) || (!bossText.empty() && !ParseInt(bossText, boss)))
        {
            SLOG_ERROR("[MonsterTable] {}줄: 숫자여야 하는 칸이 숫자가 아닙니다 — \"{}\"", dl.fileLine, dl.text);
            return false;
        }

        // ⑴ 번호 — 1 이상 · 상한 이하 · 중복 없음. 번호가 곧 색인이다
        if (monsterId <= 0 || monsterId > MAX_MONSTER_ID)
        {
            SLOG_ERROR("[MonsterTable] {}줄: 몬스터 번호 {} 는 1~{} 여야 합니다", dl.fileLine, monsterId, MAX_MONSTER_ID);
            return false;
        }
        if (monsterId < static_cast<int64_t>(_rows.size()) && _rows[static_cast<size_t>(monsterId)].monsterId != 0)
        {
            SLOG_ERROR("[MonsterTable] {}줄: 몬스터 번호 {} 가 겹칩니다", dl.fileLine, monsterId);
            return false;
        }

        // ⑵ 이름 — 로그가 가리킬 이름이 없다
        if (cell[1].empty())
        {
            SLOG_ERROR("[MonsterTable] {}줄: 이름이 비어 있습니다 (몬스터 {})", dl.fileLine, monsterId);
            return false;
        }

        // ⑶ 최대 체력 — 0 이면 태어나자마자 죽은 몬스터다
        if (maxHp < 1 || maxHp > MAX_VALUE)
        {
            SLOG_ERROR("[MonsterTable] {}줄: maxHp {} 는 1~{} 여야 합니다 (몬스터 {})", dl.fileLine, maxHp, MAX_VALUE, monsterId);
            return false;
        }

        // ⑷ 공격·방어·경험치 — 음수 금지(0 은 된다: 안 무는 몬스터 · 방어 없는 몬스터 · 경험치 없는 몬스터)
        if (attack < 0 || attack > MAX_VALUE || defense < 0 || defense > MAX_VALUE || exp < 0 || exp > MAX_VALUE)
        {
            SLOG_ERROR("[MonsterTable] {}줄: attack {} · defense {} · exp {} 는 0~{} 여야 합니다 (몬스터 {})",
                       dl.fileLine, attack, defense, exp, MAX_VALUE, monsterId);
            return false;
        }

        // ⑸ 속도 — 0 이면 쫓지도 배회하지도 못한다. 위쪽 한도는 AI 사정이라 게임 서버가 따로 본다(헤더 주석)
        if (speed < 1)
        {
            SLOG_ERROR("[MonsterTable] {}줄: speed {} 는 1 이상이어야 합니다 (몬스터 {})", dl.fileLine, speed, monsterId);
            return false;
        }

        // ⑹ 겉모습 — 몬스터 구간 밖이면 클라가 사람 글자로 그리거나 아무것도 못 그린다
        if (display < DISPLAY_MONSTER_BASE || display > DISPLAY_MONSTER_LAST)
        {
            SLOG_ERROR("[MonsterTable] {}줄: display {} 는 {}~{} 여야 합니다 (몬스터 {})",
                       dl.fileLine, display, DISPLAY_MONSTER_BASE, DISPLAY_MONSTER_LAST, monsterId);
            return false;
        }

        // ⑺ 몸 반지름 — 0 이상 상한 이하. 음수는 사거리를 줄이고, 상한을 넘으면 후보 반경(Combat::MAX_BODY_RADIUS)에서 놓치는 몬스터가 생긴다
        if (!(bodyRadius >= 0.0) || bodyRadius > static_cast<double>(Combat::MAX_BODY_RADIUS))
        {
            SLOG_ERROR("[MonsterTable] {}줄: bodyRadius {} 는 0~{} 여야 합니다 (몬스터 {})",
                       dl.fileLine, radiusText, Combat::MAX_BODY_RADIUS, monsterId);
            return false;
        }

        // ⑻ 보스 — 0 · 1 만. 그 밖의 값은 오타다(2 를 「보스 둘」로 읽을 사람이 있다). 어디에 깔리나는 스폰 표가 본다
        if (boss != 0 && boss != 1)
        {
            SLOG_ERROR("[MonsterTable] {}줄: boss {} 는 0 · 1 이거나 비워야 합니다 (몬스터 {})", dl.fileLine, boss, monsterId);
            return false;
        }

        if (static_cast<int64_t>(_rows.size()) <= monsterId)
            _rows.resize(static_cast<size_t>(monsterId) + 1);

        MonsterDef& d = _rows[static_cast<size_t>(monsterId)];
        d.monsterId = static_cast<int32_t>(monsterId);
        d.name      = cell[1];
        d.maxHp     = static_cast<int32_t>(maxHp);
        d.attack    = static_cast<int32_t>(attack);
        d.defense   = static_cast<int32_t>(defense);
        d.speed     = static_cast<int32_t>(speed);
        d.exp       = static_cast<int32_t>(exp);
        d.display   = static_cast<uint8_t>(display);
        d.bodyRadius = static_cast<float>(bodyRadius);
        d.boss      = (boss == 1);
        d.fileLine  = dl.fileLine;
        ++_count;
    }

    SLOG_INFO("[MonsterTable] {} — 몬스터 {}개", path, _count);
    return true;
}

bool CMonsterTable::CheckSpeed(int32_t maxSpeed) const
{
    bool ok = true;
    for (const MonsterDef& d : _rows)
    {
        if (d.monsterId == 0 || d.speed <= maxSpeed)
            continue;
        SLOG_ERROR("[MonsterTable] {}줄: speed {} 는 {} 이하여야 합니다 (몬스터 {}) — 판단 주기가 이 속도를 전제로 계산돼 있다(MonsterAI.h MAX_SPEED)",
                   d.fileLine, d.speed, maxSpeed, d.monsterId);
        ok = false;
    }
    return ok;
}

#endif // USE_MONSTER
