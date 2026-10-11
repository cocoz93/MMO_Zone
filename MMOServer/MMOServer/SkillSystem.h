// ==========================================================================
// SkillSystem — 스킬 쓰기 규칙 (USE_SKILL)
//
// [책임]
//  - 스킬 한 번: 시전자 검사 → 대상 고르기 → 시전 전 체력 잡기 → 비용(체력·마력) 떼기·쿨타임 → 시전 알림 → 효과
//  - 그 과정에서 정한 것(시전·거절 사유·맞힌 수)을 지표(CGameMonitor)로 센다 — CCombatSystem·CItemSystem 과 같은 꼴
//  - 보내기는 하지 않는다 — 시전은 ISkillEvents 로 알리고, 피해·죽음·보상은 CCombatSystem::ApplyHit 가 ICombatEvents 로 알린다.
//    거절은 알림이 아니라 돌려주는 값이다(CItemSystem::UseItem 꼴) — 게임 서버가 와이어 SkillFail 로 바꿔 보낸다
//
// [세 칸] 정의 = 스킬 표(CSkillTable — 기획 데이터) · 진행 = 캐릭터마다의 쿨타임·마력(CCharacter::_skillReadyFrame·_mp) · 규칙 = 여기
//
// [시전자] CCharacter 다 — 사람도 몬스터(보스)도 같은 규칙으로 쓴다(10-11). 일반 공격(CCombatSystem::PerformAttack)과 같은 꼴.
//   종류로 갈리는 것은 대상 후보 하나뿐이다(SelectTargets). 몬스터가 스킬을 부르는 곳은 아직 없다(보스 단계).
//
// [두 축] 스킬마다 다른 것은 숫자뿐이라 클래스를 스킬마다 두지 않는다. 코드로 늘어나는 것은 둘이다 —
//   대상 종류(SkillTarget → SelectTargets) · 효과 종류(SkillEffect → ApplyEffect). 새 종류 = 그 축의 case 하나.
//
// [비용은 마지막에] 체력·마력·쿨타임은 대상 확인까지 모든 검사를 지난 뒤에만 뗀다 — 거절이면 아무것도 안 바뀐다.
//   게임 로직이 한 스레드라 검사와 떼기 사이에 끼어드는 일이 없다. 재료가 생겨도 같은 자리에서 뗀다.
//
// [계층 경계]
//  CCharacter·CZone(넘겨받아 전투에 건넬 뿐)·스킬 표·CCombatSystem·CGameMonitor 를 안다. 세션·패킷·CGameServer 는 모른다.
//  객체는 CGameServer 가 멤버로 든다 — CCombatSystem 을 참조로 받으므로 그 뒤에 선언한다(멤버는 선언 순서로 세워진다).
// ==========================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "BuildConfig.h"   // USE_SKILL — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_SKILL

class CZone;
class CCharacter;
class CGameDataFiles;
class CGameMonitor;
class CCombatSystem;
struct SkillDef;

// 스킬 거절 사유 — 열거 순서가 곧 확인 순서다(여럿이 겹치면 먼저 걸린 하나로 답한다). 통과 = COUNT.
//   이 층의 어휘다 — 와이어 SkillFail(Protocol.h)로 바꾸는 건 게임 서버다(UseItemReject 와 같은 꼴, 10-06 아키텍처 전수검사 7번).
//   그래서 확인 순서 자리에 새 사유를 끼워도 된다(와이어 쪽은 끝에만 붙인다).
enum class SkillReject : uint8_t
{
    BAD_SKILL,   // 표에 없는 번호(0 · 상한 밖 포함)
    DEAD,        // 죽어 있다
    COOLDOWN,    // 아직 쿨타임이다
    LOW_HP,      // 체력을 떼고 나면 1 미만이 된다 — 자기 스킬로는 안 죽는다
    LOW_MP,      // 마력이 비용보다 적다(몬스터는 마력이 0 이라 마력 드는 스킬이면 늘 여기 — 몬스터 마력은 보스 단계)
    NO_TARGET,   // 맞을 상대가 없다(사람이 쓰면 몬스터만, 몬스터가 쓰면 사람만 맞는다)
    COUNT
};

// 스킬이 일을 할 때 부르는 알림 — CGameServer 가 구현한다(패킷 조립·송신). ICombatEvents 와 같은 꼴.
//   ★ 순수 가상이라 하나를 빠뜨리면 컴파일이 안 된다.
class ISkillEvents
{
public:
    virtual ~ISkillEvents() = default;

    // 시전했다 — 비용(체력·마력·쿨타임)을 뗀 뒤, 효과를 적용하기 "전"에 온다.
    //   → S2C_SKILL_CAST(주변) + 체력이 줄었으면 S2C_ACTOR_STATS(주변) + 마력이 줄었으면 S2C_ACTOR_MP(시전자 본인만 — 사람일 때)
    //   ★ 효과(피해·죽음·보상)는 이 뒤에 ICombatEvents 로 따로 온다 — 와이어 순서가 시전 → 시전자 체력 → 시전자 마력 → 피해다.
    virtual void OnCast(CZone* zone, CCharacter* caster, const SkillDef& def) = 0;
};

class CSkillSystem
{
public:
    // 표는 게임 서버가 든 것을 참조한다 — 기동 때 LoadGameDataFiles 가 채운 뒤에만 부른다.
    CSkillSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor, CCombatSystem& combat, ISkillEvents& events);

    // 스킬 한 번 — 통과면 COUNT(비용을 떼고 효과까지 냈다), 아니면 거절 사유(아무것도 안 바뀌었다).
    //   skillId 는 와이어에서 온 값 그대로 넣어도 된다(표에 없으면 BAD_SKILL). frame = CGameServer::_frameCount.
    SkillReject UseSkill(CZone* zone, CCharacter* caster, int32_t skillId, uint64_t frame);

private:
    // 1) 시전자 — 번호·생사·쿨타임·체력·마력. 통과면 COUNT 이고 outDef 에 스킬, outHpCost 에 뗄 체력(4) 가 그대로 뗀다).
    //    뗄 마력은 표 값 그대로(SkillDef::costMp)라 따로 돌려주지 않는다.
    SkillReject CheckCaster(const CCharacter* caster, int32_t skillId, uint64_t frame, const SkillDef*& outDef, int32_t& outHpCost) const;

    // 2) 대상 축 — _targets 를 채운다. 하나도 없으면 NO_TARGET, 있으면 COUNT.
    SkillReject SelectTargets(CZone* zone, CCharacter* caster, const SkillDef& def);

    // 6) 효과 축 — _targets 에 효과를 내고 버퍼를 비운다. hpBefore = 비용을 떼기 전 체력(피해 식의 바탕).
    void ApplyEffect(CZone* zone, CCharacter* caster, const SkillDef& def, int32_t hpBefore, uint64_t frame);

    const CGameDataFiles& _tables;
    CGameMonitor&         _gameMonitor;
    CCombatSystem&        _combat;   // 대상 찾기(FindFrontTarget · CollectCandidates)와 맞힌 뒤 처리(ApplyHit)를 일반 공격과 같이 쓴다
    ISkillEvents&         _events;

    // 대상 버퍼 — 다 고른 뒤 하나씩 적용한다(섹터 목록을 도는 중에 맞히면 죽은 몬스터가 목록에서 빠져 순회가 깨진다).
    //   멤버로 두고 재사용한다(시전마다 할당하지 않게). 적용 뒤 비운다 — 해제된 몬스터를 가리키는 칸을 남기지 않는다.
    std::vector<CCharacter*> _targets;
};

#endif // USE_SKILL
