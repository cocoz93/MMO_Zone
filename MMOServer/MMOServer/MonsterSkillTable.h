#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BuildConfig.h"   // USE_MONSTER · USE_SKILL — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_MONSTER && USE_SKILL

class CMonsterTable;
class CSkillTable;

// ==========================================================================
// CMonsterSkillTable — 몬스터가 쓰는 스킬 규칙 (Run/bin/MonsterSkill.csv)
//
// [무엇] 한 줄 = 「이 몬스터가 · 이 스킬을 · 대상이 이 거리 안이고 · 내 체력이 이 % 이하일 때 쓴다」.
//        스킬이 무엇인지(범위·피해·쿨타임)는 스킬 표(SkillTable.csv, user=monster 줄)에 있고, 여기는 **언제 쓰나**만 적는다 —
//        「스킬 정의」와 「몬스터의 쓰는 규칙」을 가른다(드랍 표가 아이템 정의와 몬스터별 확률을 가르는 것과 같다).
//
// [우선순위] 줄 순서다 — 같은 몬스터의 줄 중 **위에서부터 먼저** 검사해 처음 통과하는 것을 쓴다(MonsterAI::PickSkill).
//        한 번의 판단에 스킬 하나다.
//
// [AI 가 표 객체를 모른다] MonsterAI 는 규칙 목록(RulesOf 가 돌려주는 것)만 입력으로 받는다 — 표·존·스킬 시스템을 모르는 순수 계산이라는
//        그쪽 약속(MonsterAI.h [책임])을 지킨다. 목록은 몬스터 관리자가 몬스터 번호로 꺼내 넘긴다.
//
// [읽을 때 대조] 몬스터 번호는 몬스터 표에, 스킬 번호는 스킬 표에 있어야 하고, 그 스킬은 user=monster 여야 한다(사람 스킬을 몬스터가
//        쓰면 CheckCaster 가 막는다 — 여기서 먼저 막아 기동 때 걸린다). 몬스터는 마력이 0 이고 체력을 떼면 안 되므로 costMp·costHpPct 는 0 이어야 한다.
//        maxDist 는 스킬이 닿는 거리 안이어야 한다 — 모양이 안 닿는 거리에서 시도해 거절만 쌓이는 것을 읽기에서 막는다
//        (FRONT 는 일반 공격 사거리 · SHAPE 는 칸 목록이 닿는 거리 shapeReach · SELF 는 시야까지).
//        스킬 표를 먼저 읽고 그 결과를 받는다(CGameDataFiles::LoadAll 순서).
//
// [값 줄 0 도 된다] 스킬을 쓰는 몬스터가 아직 없는 세계다(포털 표와 같다). 파일이 없으면 오류다(다른 표와 같다).
// ==========================================================================
struct MonsterSkillRule
{
    int32_t skillId    = 0;       // 스킬 표 번호(user=monster)
    float   maxDist    = 0.0f;    // 대상까지 이 거리(타일) 이내일 때만 — 0 초과 MAX_DIST 이하. 두 몸의 **가장자리 사이** 거리다
                                  //   (AI 가 중심 사이로는 시전자·대상 반지름을 더해 본다 — MonsterAI.cpp PickSkill. 몸 반지름 0 이면 중심 사이와 같다)
    int32_t hpBelowPct = 100;     // 시전자 체력이 이 % 이하일 때만 — 1..100 (100 = 늘)
    int32_t fileLine   = 0;       // 틀렸을 때 가리킬 줄
};

class CMonsterSkillTable
{
public:
    static constexpr int32_t MAX_RULES_PER_MONSTER = 8;     // 몬스터 하나의 규칙 수 상한 — 한 번의 판단이 훑는 길이를 묶는다.
                                                            //   지금은 스킬 번호 상한(MAX_SKILL_ID 8)과 같고 같은 스킬 줄은 겹침으로 막혀 닿을 수 없다 — 스킬 번호를 올리는 날 뜻이 생긴다
    static constexpr float   MAX_DIST              = 6.0f;  // 몬스터 시야(MonsterAI::SIGHT_RANGE) — 그 밖의 대상은 AI 가 아예 안 본다(.cpp 의 단언이 맞춘다)

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    //   monsters · skills — 번호를 대조할 표(먼저 읽은 것)
    bool Load(const std::string& path, const CMonsterTable& monsters, const CSkillTable& skills);

    // 이 몬스터의 규칙들(파일 순서 = 우선순위). 없으면 빈 목록 — 규칙 없는 몬스터(뱀)는 곧바로 빈 목록이다.
    const std::vector<MonsterSkillRule>& RulesOf(int32_t monsterId) const
    {
        if (monsterId < 0 || monsterId >= static_cast<int32_t>(_byMonster.size()))
            return EMPTY;
        return _byMonster[static_cast<size_t>(monsterId)];
    }

    int32_t Count() const { return _count; }

private:
    std::vector<std::vector<MonsterSkillRule>> _byMonster;   // 색인 = 몬스터 번호
    int32_t _count = 0;

    static const std::vector<MonsterSkillRule> EMPTY;
};

#endif // USE_MONSTER && USE_SKILL
