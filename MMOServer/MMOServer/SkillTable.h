#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BuildConfig.h"   // USE_SKILL — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_SKILL

class CLevelTable;

// 스킬 번호 상한 — 번호가 곧 배열 색인이다(이 표도, 캐릭터마다의 쿨타임 배열 CCharacter::_skillReadyFrame 도).
//   표 읽기가 1~MAX_SKILL_ID 밖 번호를 거부한다. 스킬이 여덟을 넘는 날 올린다.
constexpr int32_t MAX_SKILL_ID = 8;

// 대상 종류 — 「누가 맞나」를 고르는 축. 새 종류 = 여기 한 줄 + 표 이름 한 줄 + CSkillSystem::SelectTargets 의 case 하나.
enum class SkillTarget : uint8_t
{
    FRONT,   // 앞의 한 명 — 일반 공격과 같은 사거리·정면 판정(CCombatSystem::FindFrontTarget), 단 사람이 쓰면 몬스터만·몬스터가 쓰면 사람만
    COUNT
};

// 효과 종류 — 「맞으면 무슨 일이 일어나나」의 축. 새 종류 = 여기 한 줄 + 표 이름 한 줄 + CSkillSystem::ApplyEffect 의 case 하나.
enum class SkillEffect : uint8_t
{
    DAMAGE,   // 피해 — 공격값 = 시전 전 체력 × power%. 방어력은 맞힌 뒤 처리(CCombatSystem::ApplyHit)가 뺀다
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
// [스킬마다 클래스 없음] 스킬마다 다른 것은 숫자(체력·마력 소모·위력·쿨타임)뿐이라 표 한 줄이다.
//        코드로 늘어나는 것은 대상 종류(SkillTarget)와 효과 종류(SkillEffect) 두 축이다.
// ==========================================================================
struct SkillDef
{
    int32_t     skillId        = 0;   // 0 = 이 번호의 스킬은 없다(배열 구멍)
    std::string name;                 // UTF-8. 서버는 로그에만 쓴다
    SkillTarget target         = SkillTarget::COUNT;
    SkillEffect effect         = SkillEffect::COUNT;
    int32_t     costHpPct      = 0;   // 시전 때 떼는 현재 체력의 % (0 = 소모 없음)
    int32_t     costMp         = 0;   // 시전 때 떼는 마력 — 고정값(0 = 소모 없음). 모자라면 LOW_MP
    int32_t     power          = 0;   // DAMAGE = 시전 전 체력의 %
    int32_t     cooldownMs     = 0;   // 파일에 적힌 값 그대로 — 로그용
    uint64_t    cooldownFrames = 0;   // 쿨타임(틱) — ms 를 올림해서 담는다(짧아지지 않게)
};

class CSkillTable
{
public:
    static constexpr int32_t MAX_POWER       = 1000;      // % — 체력 × power 를 int64 로 세지만 오타 하나가 값을 터뜨리지 않게
    static constexpr int32_t MAX_COOLDOWN_MS = 3600000;   // 1시간 — 그 위는 오타로 본다

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
