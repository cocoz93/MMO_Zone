#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BuildConfig.h"

#if USE_EXP

// ==========================================================================
// CLevelTable — 레벨별 기획 값 (필요 경험치 · 최대 체력 · 공격력 · 방어력)
//
// [어디 것이냐] 기획 데이터다. 유저 데이터(내 레벨·내 경험치)는 DB 의 characters 행에 있고,
//        "레벨 10 이 되려면 경험치 얼마"는 전원 공통이라 파일에 둔다. 파일인 이유는
//        밸런스 값이 코드와 같은 커밋에 들어가 같이 롤백되고 diff 로 보이기 때문.
//
// [누적 기준] requiredExp 는 "그 레벨에 도달하는 데 필요한 총 누적"이다. 레벨 안에서 다시
//        0 부터 세지 않는다. 그래서 레벨업 판정이 한 줄이고, 한 번에 여러 레벨이 올라도
//        넘친 경험치가 안 사라진다 —
//            while (level < MaxLevel() && exp >= RequiredExp(level + 1)) ++level;
//        ★ 앞의 level < MaxLevel() 이 없으면 만렙에서 배열 밖을 읽는다.
//
// [색인] _rows 의 색인이 곧 레벨이다. [0] 한 칸은 안 쓴다 — level-1 로 밀면 읽는 자리마다
//        -1 이 붙어 틀리기 쉽고, 한 칸 버리는 값이 그보다 싸다.
//
// [범위] 접근자는 clamp 하지 않고 단언만 둔다. 조용히 잘라 주면 "표 밖 레벨"을 셀 수가 없어
//        계수기가 거짓말을 한다. 범위를 내리는 일은 불러오는 자리에서 한 번만 하고,
//        그 뒤로는 "레벨은 항상 표 안"이 불변식이다.
// ==========================================================================
class CLevelTable
{
public:
    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    //   밸런스 값 없이 도는 서버는 조용히 틀린 게임이 되므로 기본값으로 때우지 않는다.
    bool Load(const std::string& path);

    int32_t MaxLevel() const { return static_cast<int32_t>(_rows.size()) - 1; }   // [0] 은 안 쓰므로 -1

    int64_t RequiredExp(int32_t level) const;
    int32_t MaxHp(int32_t level) const;
    int32_t AttackPower(int32_t level) const;
    int32_t Defense(int32_t level) const;

private:
    struct Row
    {
        int64_t requiredExp = 0;
        int32_t maxHp       = 0;
        int32_t attackPower = 0;
        int32_t defense     = 0;
    };

    bool InRange(int32_t level) const
    {
        return level >= 1 && level < static_cast<int32_t>(_rows.size());
    }

    std::vector<Row> _rows;   // 색인 = 레벨. [0] 은 자리만 차지한다
};

#endif // USE_EXP
