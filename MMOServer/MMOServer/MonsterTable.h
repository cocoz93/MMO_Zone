#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BuildConfig.h"

#if USE_MONSTER

// ==========================================================================
// CMonsterTable — 몬스터 종류별 사양 (Run/bin/MonsterTable.csv)
//
// [어디 것이냐] 기획 데이터다. 「뱀의 최대 체력은 얼마」는 전원 공통이라 여기,
//        「이 뱀이 지금 몇 남았나」는 몬스터 객체(CMonster — CCharacter 의 _hp)다.
//        몬스터는 생길 때 자기 행을 가리키고 능력치를 복사해 간다(CMonster 생성자).
//
// [색인] 번호가 곧 색인이다(아이템 표와 같은 꼴). 0 = 없음, 구멍은 monsterId 0 인 빈 행.
//        스폰 표·드랍 표가 이 번호로 몬스터를 가리키고, 읽을 때 이 표와 대조한다(CGameDataFiles::LoadAll 순서).
//
// [겉모습] display 는 와이어의 표시 문자(displayChar) 값이다 — 클라가 그 값으로 몬스터 그림을 고른다.
//        값은 여기 두고, 와이어에 싣는 손은 게임 서버다(CGameServer::OnMonsterSpawn). 존 층은 이 값을 안 읽는다.
//
// [속도 상한은 여기서 안 본다] 판단 주기가 전제하는 상한(MonsterAI::MAX_SPEED)은 AI 사정이라
//        게임 서버가 표를 읽은 직후 대조한다(CheckSpeed).
// ==========================================================================
struct MonsterDef
{
    int32_t     monsterId = 0;   // 0 = 이 번호의 몬스터는 없다(배열 구멍)
    std::string name;            // UTF-8. 서버는 로그에만 쓴다
    int32_t     maxHp     = 0;
    int32_t     attack    = 0;   // 물 때 공격력
    int32_t     defense   = 0;   // 맞을 때 빼는 값
    int32_t     speed     = 0;   // 칸/초
    int32_t     exp       = 0;   // 잡은 사람에게 주는 경험치
    uint8_t     display   = 0;   // 와이어 표시 문자 — Protocol.h DISPLAY_MONSTER_BASE 구간
    int32_t     fileLine  = 0;   // 대조(CheckSpeed)에서 틀렸을 때 가리킬 줄
};

class CMonsterTable
{
public:
    static constexpr int32_t MAX_MONSTER_ID = 10000;     // 오타 하나(100000)가 배열을 통째로 키우지 않게
    static constexpr int32_t MAX_VALUE      = 1000000;   // 능력치·경험치 상한 — 피해·경험치 계산이 int32 를 넘지 않게

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    bool Load(const std::string& path);

    // 없는 번호면 nullptr — 스폰 표·드랍 표가 번호를 대조하는 길이다.
    const MonsterDef* Find(int32_t monsterId) const
    {
        if (monsterId <= 0 || monsterId >= static_cast<int32_t>(_rows.size()))
            return nullptr;
        const MonsterDef& d = _rows[static_cast<size_t>(monsterId)];
        return d.monsterId == 0 ? nullptr : &d;
    }

    // 속도 대조 — 모든 몬스터가 maxSpeed 이하인가. 넘는 줄마다 로그를 남기고 false.
    bool CheckSpeed(int32_t maxSpeed) const;

    int32_t Count() const { return _count; }

private:
    std::vector<MonsterDef> _rows;   // 색인 = 몬스터 번호. 구멍은 monsterId 0
    int32_t _count = 0;              // 실제로 정의된 몬스터 수
};

#endif // USE_MONSTER
