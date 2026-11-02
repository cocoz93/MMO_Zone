#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "BuildConfig.h"   // USE_SKILL — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include "Actor.h"         // Direction · DirectionStep — 범위 스킬의 칸은 바라보는 쪽 기준이다

#if USE_SKILL

class CLevelTable;
class CJobTable;
class CBuffTable;

// 스킬 번호 상한 — 와이어(u16)와 DB 쿨타임 묶음(2바이트)에 실리는 크기다. 배열 색인이 아니다(11-01 기술 목록).
//   번호는 이름표일 뿐이라 띄엄띄엄 매겨도 된다 — 캐릭터는 자기가 가진 기술 목록(CAttackComponent::_skillBook)을 들고,
//   지표는 표가 읽을 때 매긴 안쪽 순번(SkillDef::index)으로 센다.
constexpr int32_t MAX_SKILL_ID = 0xFFFF;

// 남는 상한은 번호 크기가 아니라 개수다.
//   MAX_SKILL_ROWS  = 표 줄 수(사람·몬스터 합) — 지표 칸 수(GameMonitor.h SkillCounters)
//   MAX_SKILL_BOOK  = 한 캐릭터가 가진 기술 수 — DB 쿨타임 묶음(2 + 24 × 10 = 242바이트 ≤ VARBINARY(255), DBWorker.h)이 묶는다.
//                     사람은 표의 사람 기술 전부를 가지므로 표 읽기가 사람 기술 수를 이 값과 견준다. 몬스터는 규칙 수 상한(8)이 그 안이다
constexpr int32_t MAX_SKILL_ROWS = 64;
constexpr int32_t MAX_SKILL_BOOK = 24;

// 범위 스킬의 칸 하나 — 시전자 기준 (앞, 옆). 옆은 바라보는 쪽 기준 오른쪽이 +. 표의 area 칸이 「앞:옆」 으로 적는다.
//   칸 크기는 1타일 — 대상 좌표(float)를 반올림해 어느 칸인지 정한다(IsInShape).
struct ShapeCell
{
    int8_t forward = 0;   // 1~MAX_SHAPE_REACH — 앞쪽만(0 이하는 표 읽기가 거부한다: 시전자 자리·뒤는 칸이 아니다)
    int8_t side    = 0;   // −MAX_SHAPE_REACH~MAX_SHAPE_REACH
};

// 주변 스킬의 반경 상한(칸) — 섹터 크기 20(MMOServerConfig.ini SectorSize)이다. 시전자 둘레 20칸까지는 서버가 늘 알려 주므로
//   이 값을 넘기면 시전자 클라가 모르는 적이 맞을 수 있다.
constexpr int32_t MAX_AROUND_RADIUS = 20;

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

// 주변 판정 — 대상이 시전자 둘레 반경 안에 있나. IsInShape 와 같이 순수 함수라 이 헤더만 넣고 시험한다.
//   칸 목록으로 안 그리는 까닭 — 칸은 앞쪽만 받고(forward ≥ 1) 16개가 상한이라 둘레 원을 못 그린다. 몸 반지름도 앞쪽 기준으로만 뺀다.
//   거리는 두 몸의 가장자리 사이다 = 중심 거리 − 시전자 반지름 − 대상 반지름. 이 값이 반경 이하면 맞는다. 방향은 안 본다.
inline bool IsInAround(float dx, float dy, int32_t radius, float casterRadius = 0.0f, float targetRadius = 0.0f)
{
    const float reach = static_cast<float>(radius) + casterRadius + targetRadius;
    return dx * dx + dy * dy <= reach * reach;
}

// 대상 종류 — 「누가 맞나」를 고르는 축. 새 종류 = 여기 한 줄 + 표 이름 한 줄 + CSkillSystem::SelectTargets 의 case 하나.
enum class SkillTarget : uint8_t
{
    FRONT,   // 앞의 한 명 — 일반 공격과 같은 사거리·정면 판정(CCombatSystem::FindFrontTarget), 단 사람이 쓰면 몬스터만·몬스터가 쓰면 사람만
    SHAPE,   // 칸 목록 — 표의 area 칸(시전자 기준 앞:옆)에 선 상대 전부(IsInShape). 한 칸에 여럿이면 다 맞는다. 상대 종류는 FRONT 와 같다
    SELF,    // 자기 자신 — 후보를 안 찾는다(비는 일이 없어 NO_TARGET 이 안 난다). 지속 효과(버프)가 쓴다. 남에게 거는 버프는 대지의기원 때 늘린다
    AROUND,  // 시전자 둘레 반경 안의 상대 전부(IsInAround) — 반경은 표의 area 칸. 방향을 안 본다. 상대 종류는 FRONT 와 같다. 사람 스킬만(표 읽기가 막는다)
    COUNT
};

// 효과 종류 — 「맞으면 무슨 일이 일어나나」의 축. 새 종류 = 여기 한 줄 + 표 이름 한 줄 + CSkillSystem::ApplyEffect 의 case 하나.
enum class SkillEffect : uint8_t
{
    DAMAGE,            // 피해 — 공격값 = 시전 전 체력 × power%. 방어력은 맞힌 뒤 처리(CCombatSystem::ApplyHit)가 뺀다
    BUFF,              // 지속 효과 — power 번 버프를 건다(CBuffSystem::Apply · 무엇을 얼마나 몇 초는 버프 표). 대상은 self 만(표 읽기가 막는다)
    DAMAGE_ATK,        // 피해 — 공격값 = 시전자 공격력(최종값) × power%. 방어력은 DAMAGE 와 같이 맞힌 뒤에 뺀다.
                       //   DAMAGE 는 시전자의 현재 체력이 식에 들어가(체력을 떼고 그 체력으로 치는 스킬) 체력이 줄수록 약해진다 — 몬스터(보스)가 쓰면 안 맞아서 따로 둔다(10-13)
    REVIVE,            // 되살리기 — 시전자 존에서 죽은 power 번 몬스터를 빈 점 수만큼 되살린다(10-18 뱀왕). 대상은 self · 쓰는 쪽은 monster 만(표 읽기가 막는다).
                       //   직접 깔지 않고 알림(ISkillEvents::OnRevive)으로 넘긴다 — 시전은 몬스터 판단 순회 안이라 그 자리에서 깔면 순회가 깨진다(MonsterManager.h RequestRevive)
    COUNT
};

// 누가 쓰는 스킬인가 — 이 표는 사람과 몬스터가 한 장을 같이 쓴다(번호 한 줄·와이어 의미 하나).
//   사람은 PLAYER 줄만, 몬스터는 MONSTER 줄만 쓴다 — 이 칸이 기술 목록에 들어갈 자격이다(사람 = 입장 때 자기 직업의 PLAYER 줄 ·
//   몬스터 = 자기 규칙의 줄, 몬스터 스킬 표 읽기가 MONSTER 인지 본다). 목록에 없는 번호는 CSkillSystem::CheckCaster 가 NOT_FOR_CASTER 로 막는다.
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
//        「이 캐릭터가 백호참을 언제 다시 쓸 수 있나」는 캐릭터 쪽(공격 컴포넌트의 기술 목록 CAttackComponent::_skillBook)이다.
//
// [색인] 번호는 색인이 아니다 — 줄은 파일 순서로 들고, 번호로 찾을 땐 번호 → 줄 표(_rowById)를 본다. 0 = 없음.
//        줄마다 안쪽 순번(index, 0~)을 매긴다 — 지표 칸이 이 순번이다.
//        번호는 와이어에도 실린다(C2S_USE_SKILL · S2C_SKILL_CAST — u16) — 웹이 같은 파일에서 이름을 읽는다.
//
// [스킬마다 클래스 없음] 스킬마다 다른 것은 숫자(체력·마력 소모·위력·쿨타임)와 범위 스킬의 칸 목록뿐이라 표 한 줄이다.
//        코드로 늘어나는 것은 대상 종류(SkillTarget)와 효과 종류(SkillEffect) 두 축이다.
// ==========================================================================
struct SkillDef
{
    int32_t     skillId        = 0;   // 1~MAX_SKILL_ID — 표 읽기가 겹침을 막는다
    int32_t     index          = 0;   // 안쪽 순번 0~(줄 수 − 1) — 파일 순서. 지표 칸(GameMonitor.h SkillCounters)이 이 값이다
    std::string name;                 // UTF-8. 서버는 로그에만 쓴다
    SkillTarget target         = SkillTarget::COUNT;
    SkillEffect effect         = SkillEffect::COUNT;
    SkillUser   user           = SkillUser::PLAYER;   // 누가 쓰나 — 표의 10번째 칸(빈 칸 = PLAYER)
    int32_t     job            = 0;   // 어느 직업의 기술인가 — 표의 11번째 칸(0 = 빈 칸 = 모든 직업, 고르기 전 포함)
    int32_t     costHpPct      = 0;   // 시전 때 떼는 현재 체력의 % (0 = 소모 없음)
    int32_t     costMp         = 0;   // 시전 때 떼는 마력 — 고정값(0 = 소모 없음). 모자라면 LOW_MP
    int32_t     power          = 0;   // DAMAGE = 시전 전 체력의 % · DAMAGE_ATK = 시전자 공격력의 % · BUFF = 버프 번호(버프 표 — 표 읽기가 대조한다)
                                      //   REVIVE = 되살릴 몬스터 번호(몬스터 표 — 있는지·보스 아닌지는 몬스터 스킬 표 읽기가 본다. 이 표는 몬스터 표를 모른다)
    int32_t     cooldownMs     = 0;   // 파일에 적힌 값 그대로 — 로그용
    uint64_t    cooldownFrames = 0;   // 쿨타임(틱) — ms 를 올림해서 담는다(짧아지지 않게)
    std::vector<ShapeCell> shape;     // SHAPE 의 칸 목록(다른 대상 종류는 빈 목록)
    float       shapeReach     = 0.0f;   // ShapeReach(shape) — 대상 찾기가 훑을 반경(몸 크기는 ShapeScanRadius 가 더한다). 표 읽기가 한 번 센다
    int32_t     aroundRadius   = 0;      // AROUND 의 반경(칸) — 1~MAX_AROUND_RADIUS. 다른 대상 종류는 0
};

class CSkillTable
{
public:
    static constexpr int32_t MAX_POWER       = 1000;      // % — 체력 × power 를 int64 로 세지만 오타 하나가 값을 터뜨리지 않게
    static constexpr int32_t MAX_COOLDOWN_MS = 3600000;   // 1시간 — 그 위는 오타로 본다

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    //   levels = 먼저 읽은 레벨 표 — costMp 가 닿을 수 있는 최대 마력을 넘는지 대조한다(드랍 표가 아이템 표를 받는 꼴).
    //   buffs  = 먼저 읽은 버프 표 — 효과 buff 줄의 power(버프 번호)가 표에 있나 본다.
    //   jobs   = 먼저 읽은 직업 표 — job 칸의 번호가 표에 있나 본다.
    bool Load(const std::string& path, const CLevelTable& levels, const CBuffTable& buffs, const CJobTable& jobs);

    // 없는 번호면 nullptr — 와이어로 온 번호(아무 값이나 올 수 있다)를 그대로 넣어도 된다.
    const SkillDef* Find(int32_t skillId) const
    {
        const auto it = _rowById.find(skillId);
        return (it == _rowById.end()) ? nullptr : &_rows[it->second];
    }

    int32_t Count() const { return static_cast<int32_t>(_rows.size()); }

    // 안쪽 순번으로 — 0~Count()−1. 지표 이름표(스킬 번호)를 적어 둘 때 훑는다(CGameServer::LoadGameDataFiles)
    const SkillDef& Row(int32_t index) const { return _rows[static_cast<size_t>(index)]; }

    // 사람 기술 번호 — 파일 순서.
    const std::vector<int32_t>& PlayerSkillIds() const { return _playerSkillIds; }

    // 이 직업이 가진 사람 기술 번호 — 파일 순서. 공용(job 칸이 빈 줄) 포함. job 0(아직 안 고름)은 공용만.
    //   입장과 직업 고르기 때 기술 목록을 채운다(CEntryHandler::SpawnPlayer · CJobHandler)
    std::vector<int32_t> PlayerSkillIdsOf(int32_t job) const;

private:
    std::vector<SkillDef> _rows;                        // 파일 순서 — 색인 = SkillDef::index. 읽은 뒤로 안 늘어 Find 의 포인터가 안 흔들린다
    std::unordered_map<int32_t, size_t> _rowById;       // 스킬 번호 → _rows 색인
    std::vector<int32_t> _playerSkillIds;               // user=player 줄의 번호 — 파일 순서
};

#endif // USE_SKILL
