#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "BuildConfig.h"

#if USE_DROP

#include "Item.h"

class CItemTable;
class CMonsterTable;

// ==========================================================================
// CDropTable — 몬스터가 무엇을 몇 % 로 떨어뜨리나 (Run/bin/DropTable.csv)
//
// [한 줄] 「이 몬스터가 · 이 아이템을 · 이 확률로 · 몇~몇 개」. 잡을 때마다 그 몬스터의 줄을 하나씩 따로 굴린다 —
//        그래서 한 마리가 여러 개를 한꺼번에 떨어뜨릴 수도, 아무것도 안 떨어뜨릴 수도 있다.
//
// [확률은 정수로] 파일엔 백분율(소수 둘째 자리까지, 0.5 가능)로 적고, 읽을 때 만분율 정수로 바꾼다.
//        굴림은 정수 난수 [0, 10000) 가 그보다 작은가 한 번 — 부동소수 비교를 안 한다.
//
// [표끼리 대조] 아이템 번호는 아이템 표에 있어야 하고, 개수는 그 아이템의 쌓임 한도 안이어야 한다.
//        몬스터 번호는 몬스터 표에 있어야 한다. 그래서 두 표를 먼저 읽고 그 결과를 받는다(CGameDataFiles::LoadAll 순서).
// ==========================================================================
struct DropRow
{
    int32_t itemId   = 0;
    int32_t chanceBp = 0;   // 만분율 — 60% = 6000, 0.5% = 50
    int32_t minCount = 0;
    int32_t maxCount = 0;
};

// 한 마리분 굴림 — 줄마다 따로. 로그도 서버도 모르는 순수 함수라 확률 시험이 이 헤더만 넣고 돌린다.
//   rng 는 부르는 쪽이 준다(서버는 게임 스레드 전용 mt19937 하나).
template <class Rng>
inline void RollDrops(const std::vector<DropRow>& rows, Rng& rng, std::vector<Item>& out)
{
    out.clear();
    std::uniform_int_distribution<int32_t> roll(0, 9999);
    for (const DropRow& r : rows)
    {
        if (roll(rng) >= r.chanceBp)
            continue;
        std::uniform_int_distribution<int32_t> count(r.minCount, r.maxCount);
        Item it;
        it.itemId = r.itemId;
        it.count  = count(rng);
        out.push_back(it);
    }
}

class CDropTable
{
public:
    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    bool Load(const std::string& path, const CItemTable& items, const CMonsterTable& monsters);

    // 이 몬스터의 줄들 (없으면 빈 목록 — 아무것도 안 떨어뜨리는 몬스터도 된다)
    const std::vector<DropRow>& RowsOf(int32_t monsterId) const
    {
        if (monsterId < 0 || monsterId >= static_cast<int32_t>(_byMonster.size()))
            return EMPTY;
        return _byMonster[static_cast<size_t>(monsterId)];
    }

private:
    std::vector<std::vector<DropRow>> _byMonster;   // 색인 = 몬스터 번호 (몬스터 표)
    int32_t _count = 0;

    static const std::vector<DropRow> EMPTY;
};

#endif // USE_DROP
