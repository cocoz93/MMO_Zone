#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "BuildConfig.h"   // USE_SKILL — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include "Actor.h"         // Direction · DirectionStep — 범위 스킬의 칸은 바라보는 쪽 기준이다

#if USE_SKILL

class CLevelTable;

// 스킬 번호 상한 — 번호가 곧 배열 색인이다(이 표도, 캐릭터마다의 쿨타임 배열 CCharacter::_skillReadyFrame 도).
//   표 읽기가 1~MAX_SKILL_ID 밖 번호를 거부한다. 스킬이 여덟을 넘는 날 올린다.
constexpr int32_t MAX_SKILL_ID = 8;

// 범위 스킬의 칸 하나 — 시전자 기준 (앞, 옆). 옆은 바라보는 쪽 기준 오른쪽이 +. 표의 shape 칸이 「앞:옆」 으로 적는다.
//   칸 크기는 1타일 — 대상 좌표(float)를 반올림해 어느 칸인지 정한다(IsInShape).
struct ShapeCell
{
    int8_t forward = 0;   // 1~MAX_SHAPE_REACH — 앞쪽만(0 이하는 표 읽기가 거부한다: 시전자 자리·뒤는 칸이 아니다)
    int8_t side    = 0;   // −MAX_SHAPE_REACH~MAX_SHAPE_REACH
};

constexpr int32_t MAX_SHAPE_REACH = 5;    // 칸 하나가 닿는 거리 상한(앞·옆 각각) — 오타를 거르는 값
constexpr int32_t MAX_SHAPE_CELLS = 16;   // 한 스킬의 칸 수 상한

// 칸 판정 — 대상이 모양 안의 칸에 서 있나. 로그도 서버도 모르는 순수 함수라 칸 시험이 이 헤더만 넣고 돌린다(RollDrops 와 같은 꼴).
//   dx, dy = 대상 − 시전자 좌표. 앞 = (dx,dy)·DirectionStep(facing), 옆 = (dx,dy)·(−앞.y, 앞.x) 를 각각 반올림해 칸 번호로 —
//   칸 k 는 [k−0.5, k+0.5) 다. 기준은 시전자의 실제 좌표다(타일에 붙이지 않는다 — 일반 공격의 사거리 판정과 같은 float 기준).
//   facing 이 NONE 이면 앞 성분이 늘 0 이라 어떤 칸(앞 ≥ 1)에도 안 든다.
//
//   몸 반지름(casterRadius · targetRadius, 둘 다 0 이면 위 점 판정 그대로)이 있으면 칸은 **두 몸의 가장자리 사이**로 잰다:
//     앞 = (앞 성분) − 시전자 반지름 − 대상 반지름 · 옆 = |옆 성분| 을 대상 반지름만큼 중심선 쪽으로 당긴 값(0 을 넘지 않는다).
//     대상 몸이 시전자 중심보다 앞쪽에 걸쳐 있는데(앞 성분 + 대상 반지름 > 0) 앞이 0.5 미만이면 — 몸이 닿았거나 겹쳤다 — 첫 칸(0.5)으로 친다.
//     안 그러면 큰 몸 시전자의 몸통에 붙어 선 대상은 아무 칸에도 안 들어 범위 스킬을 안 맞는 자리가 된다(10-14 「첫 칸 취급」).
//   대상 몸을 「가까운 가장자리 한 점」으로 보는 근사다 — 원과 칸의 정확한 겹침 계산은 안 한다.
inline bool IsInShape(Direction facing, float dx, float dy, const std::vector<ShapeCell>& cells,
                      float casterRadius = 0.0f, float targetRadius = 0.0f)
{
    const DirectionVec f = DirectionStep(facing);
    float along = dx * f.x + dy * f.y;            // 앞 성분
    float right = dx * -f.y + dy * f.x;           // 옆 성분 — 오른쪽 = (−앞.y, 앞.x)
    if (casterRadius > 0.0f || targetRadius > 0.0f)
    {
        const bool bodyAhead = (along + targetRadius > 0.0f);
        along -= casterRadius + targetRadius;
        if (bodyAhead && along < 0.5f)
            along = 0.5f;
        const float side = std::fabs(right) - targetRadius;
        right = (side > 0.0f) ? std::copysign(side, right) : 0.0f;
    }
    const int32_t cellForward = static_cast<int32_t>(std::floor(along + 0.5f));
    const int32_t cellSide    = static_cast<int32_t>(std::floor(right + 0.5f));
    for (const ShapeCell& c : cells)
    {
        if (c.forward == cellForward && c.side == cellSide)
            return true;
    }
    return false;
}

// 칸 목록이 닿는 가장 먼 거리 — 칸마다 바깥 모서리(|앞|+0.5, |옆|+0.5)까지. 대상 찾기가 이 반경으로 섹터를 고른다.
//   모서리까지 재므로 방향을 늘려도(대각선) 칸 안의 점은 다 이 반경 안이다.
inline float ShapeReach(const std::vector<ShapeCell>& cells)
{
    float reach = 0.0f;
    for (const ShapeCell& c : cells)
    {
        const float a = std::fabs(static_cast<float>(c.forward)) + 0.5f;
        const float b = std::fabs(static_cast<float>(c.side)) + 0.5f;
        const float d = std::sqrt(a * a + b * b);
        reach = (d > reach) ? d : reach;   // std::max 는 윈도 헤더의 max 매크로에 먹힌다
    }
    return reach;
}

// 범위 스킬이 후보를 훑는 반경(시전자 중심에서) — 칸 목록이 닿는 거리에 몸 크기를 더한다.
//   맞을 수 있는 대상의 중심은 칸 바깥 모서리 (a, b) 에서 앞으로 (시전자 반지름 + 대상 반지름) · 옆으로 대상 반지름만큼 더 나간 곳까지라
//   길이가 shapeReach + √((시전자 + 대상)² + 대상²) 를 넘지 않는다(삼각 부등식). 대상 반지름은 후보를 보기 전엔 모르므로 부르는 쪽이 상한을 넘긴다.
//   두 반지름이 0 이면 shapeReach 그대로다.
inline float ShapeScanRadius(float shapeReach, float casterRadius, float targetRadiusMax)
{
    const float forwardExtra = casterRadius + targetRadiusMax;
    return shapeReach + std::sqrt(forwardExtra * forwardExtra + targetRadiusMax * targetRadiusMax);
}

// 대상 종류 — 「누가 맞나」를 고르는 축. 새 종류 = 여기 한 줄 + 표 이름 한 줄 + CSkillSystem::SelectTargets 의 case 하나.
enum class SkillTarget : uint8_t
{
    FRONT,   // 앞의 한 명 — 일반 공격과 같은 사거리·정면 판정(CCombatSystem::FindFrontTarget), 단 사람이 쓰면 몬스터만·몬스터가 쓰면 사람만
    SHAPE,   // 칸 목록 — 표의 shape 칸(시전자 기준 앞:옆)에 선 상대 전부(IsInShape). 한 칸에 여럿이면 다 맞는다. 상대 종류는 FRONT 와 같다
    SELF,    // 자기 자신 — 후보를 안 찾는다(비는 일이 없어 NO_TARGET 이 안 난다). 지속 효과(버프)가 쓴다. 남에게 거는 버프는 대지의기원 때 늘린다
    COUNT
};

// 효과 종류 — 「맞으면 무슨 일이 일어나나」의 축. 새 종류 = 여기 한 줄 + 표 이름 한 줄 + CSkillSystem::ApplyEffect 의 case 하나.
enum class SkillEffect : uint8_t
{
    DAMAGE,            // 피해 — 공격값 = 시전 전 체력 × power%. 방어력은 맞힌 뒤 처리(CCombatSystem::ApplyHit)가 뺀다
    BUFF_ATTACK_MUL,   // 지속 효과 — 공격력 × power 를 durationMs 동안(CBuffSystem::Apply, 종류 BuffKind::ATTACK_MUL). 대상은 self 만(표 읽기가 막는다)
    DAMAGE_ATK,        // 피해 — 공격값 = 시전자 공격력(최종값) × power%. 방어력은 DAMAGE 와 같이 맞힌 뒤에 뺀다.
                       //   DAMAGE 는 시전자의 현재 체력이 식에 들어가(체력을 떼고 그 체력으로 치는 스킬) 체력이 줄수록 약해진다 — 몬스터(보스)가 쓰면 안 맞아서 따로 둔다(10-13)
    REVIVE,            // 되살리기 — 시전자 존에서 죽은 power 번 몬스터를 빈 점 수만큼 되살린다(10-18 뱀왕). 대상은 self · 쓰는 쪽은 monster 만(표 읽기가 막는다).
                       //   직접 깔지 않고 알림(ISkillEvents::OnRevive)으로 넘긴다 — 시전은 몬스터 판단 순회 안이라 그 자리에서 깔면 순회가 깨진다(MonsterManager.h RequestRevive)
    COUNT
};

// 누가 쓰는 스킬인가 — 이 표는 사람과 몬스터가 한 장을 같이 쓴다(번호 한 줄·쿨타임 칸 하나·와이어 의미 하나).
//   사람은 PLAYER 줄만, 몬스터는 MONSTER 줄만 쓴다(CSkillSystem::CheckCaster 가 NOT_FOR_CASTER 로 막는다 — 몬스터 스킬 표 읽기가 한 번 더 막는다).
//   칸이 비면 PLAYER 다(옛 줄이 그대로 사람 스킬). 새 종류 = 여기 한 줄 + 표 이름 한 줄(SkillTable.cpp USER_NAMES).
enum class SkillUser : uint8_t
{
    PLAYER,
    MONSTER,
    COUNT
};

// ==========================================================================
// CSkillTable — 스킬 정의 (Run/bin/SkillTable.csv)
//
// [어디 것이냐] 기획 데이터다. 「백호참은 체력 몇 %를 떼나」는 전원 공통이라 여기,
//        「이 캐릭터가 백호참을 언제 다시 쓸 수 있나」는 캐릭터 쪽(CCharacter::_skillReadyFrame)이다.
//
// [색인] 번호가 곧 색인이다(아이템·몬스터 표와 같은 꼴). 0 = 없음, 구멍은 skillId 0 인 빈 행.
//        번호는 와이어에도 실린다(C2S_USE_SKILL · S2C_SKILL_CAST) — 웹이 같은 파일에서 이름을 읽는다.
//
// [스킬마다 클래스 없음] 스킬마다 다른 것은 숫자(체력·마력 소모·위력·쿨타임·지속)와 범위 스킬의 칸 목록뿐이라 표 한 줄이다.
//        코드로 늘어나는 것은 대상 종류(SkillTarget)와 효과 종류(SkillEffect) 두 축이다.
// ==========================================================================
struct SkillDef
{
    int32_t     skillId        = 0;   // 0 = 이 번호의 스킬은 없다(배열 구멍)
    std::string name;                 // UTF-8. 서버는 로그에만 쓴다
    SkillTarget target         = SkillTarget::COUNT;
    SkillEffect effect         = SkillEffect::COUNT;
    SkillUser   user           = SkillUser::PLAYER;   // 누가 쓰나 — 표의 11번째 칸(빈 칸 = PLAYER)
    int32_t     costHpPct      = 0;   // 시전 때 떼는 현재 체력의 % (0 = 소모 없음)
    int32_t     costMp         = 0;   // 시전 때 떼는 마력 — 고정값(0 = 소모 없음). 모자라면 LOW_MP
    int32_t     power          = 0;   // DAMAGE = 시전 전 체력의 % · DAMAGE_ATK = 시전자 공격력의 % · BUFF_ATTACK_MUL = 배수(2 = 이중공격)
                                      //   REVIVE = 되살릴 몬스터 번호(몬스터 표 — 있는지·보스 아닌지는 몬스터 스킬 표 읽기가 본다. 이 표는 몬스터 표를 모른다)
    int32_t     cooldownMs     = 0;   // 파일에 적힌 값 그대로 — 로그용
    uint64_t    cooldownFrames = 0;   // 쿨타임(틱) — ms 를 올림해서 담는다(짧아지지 않게)
    int32_t     durationMs     = 0;   // 지속(ms) — 지속 효과(BUFF_*)만. 그 밖은 0 이어야 한다(표 읽기가 막는다) — 로그용
    uint64_t    durationFrames = 0;   // 지속(틱) — 올림(쿨타임과 같은 까닭). 30000 → 750
    std::vector<ShapeCell> shape;     // SHAPE 의 칸 목록(다른 대상 종류는 빈 목록)
    float       shapeReach     = 0.0f;   // ShapeReach(shape) — 대상 찾기가 훑을 반경(몸 크기는 ShapeScanRadius 가 더한다). 표 읽기가 한 번 센다
};

class CSkillTable
{
public:
    static constexpr int32_t MAX_POWER       = 1000;      // % — 체력 × power 를 int64 로 세지만 오타 하나가 값을 터뜨리지 않게
    static constexpr int32_t MAX_COOLDOWN_MS = 3600000;   // 1시간 — 그 위는 오타로 본다(지속 시간 상한도 같다)
    static constexpr int32_t MAX_BUFF_MUL    = 10;        // 배수 상한 — 1 은 아무 일도 안 하고(표 읽기가 막는다) 그 위는 오타. 원본 오중공격이 5

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    //   levels = 먼저 읽은 레벨 표 — costMp 가 닿을 수 있는 최대 마력을 넘는지 대조한다(드랍 표가 아이템 표를 받는 꼴).
    bool Load(const std::string& path, const CLevelTable& levels);

    // 없는 번호면 nullptr — 와이어로 온 번호(아무 값이나 올 수 있다)를 그대로 넣어도 된다.
    const SkillDef* Find(int32_t skillId) const
    {
        if (skillId <= 0 || skillId >= static_cast<int32_t>(_rows.size()))
            return nullptr;
        const SkillDef& d = _rows[static_cast<size_t>(skillId)];
        return d.skillId == 0 ? nullptr : &d;
    }

    int32_t Count() const { return _count; }

private:
    std::vector<SkillDef> _rows;   // 색인 = 스킬 번호. 구멍은 skillId 0
    int32_t _count = 0;            // 실제로 정의된 스킬 수
};

#endif // USE_SKILL
