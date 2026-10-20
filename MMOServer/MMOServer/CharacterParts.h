// ==========================================================================
// 캐릭터 부품 — 「무엇을 할 수 있나」 하나에 부품 하나 (부품 나누기 · 10-20)
//
// [부품 셋] 체력(CHealthPart · 맞는 쪽) · 공격(CAttackPart · 때리는 쪽) · 효과(CBuffPart · 지속 효과를 받는 쪽).
//        사람·몬스터는 셋을 다 값으로 들고(Player.h · Monster.h), CCharacter 는 가리키기만 한다(Character.h 부품 자리).
//        능력이 늘면 부품이 는다 — 상속을 늘리지 않는다(덫 = 공격만 · 부서지는 문 = 체력만 같은 조합이 상속 없이 된다).
//
// [감추는 칸] 규칙이 있는 칸(체력·마력 — 0 ~ 최대 · 죽음)은 감추고 함수만 연다. 규칙을 쓰는 자리가 여기 하나다.
//        규칙이 없는 칸(공격력 · 쿨타임 · 효과 칸)은 공개 칸이다 — CCharacter 의 지금 꼴(공개 칸 + 행동은 시스템)과 같다.
//
// [토글] 칸은 원래 칸의 토글을 따른다 — 마력·스킬 쿨타임 = USE_SKILL · 효과 부품 = USE_COMBAT. 체력 부품·공격력·일반 쿨타임은 토글 없음.
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"   // USE_SKILL · USE_COMBAT — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include "SkillTable.h"    // USE_SKILL — MAX_SKILL_ID(쿨타임 배열 크기). 헤더 안에서 스스로 토글을 본다
#include "Buff.h"          // USE_COMBAT — BuffSlot·BUFF_KIND_COUNT(효과 칸). 같은 규칙

// ── 체력 부품 — 맞는 쪽 ──
//   체력은 0 ~ 최대. 0 이 되는 순간이 죽음이다 — 체력 0 과 죽음 표시를 TakeDamage 한 곳이 같이 적는다
//   (전에는 체력은 Combat::ApplyDamage, 죽음 표시는 호출자 CCombatSystem::ApplyHit 로 갈라져 「체력 0 인데 산 것」이 칸으로 표현됐다).
class CHealthPart
{
public:
    int32_t  Hp() const         { return _hp; }
    int32_t  MaxHp() const      { return _maxHp; }
    int32_t  Defense() const    { return _defense; }     // 맞을 때 공격력에서 뺀다(Combat::CalcDamage)
    bool     IsDead() const     { return _isDead; }      // 죽은 상태 (이동·공격 거부, 공격 대상에서 제외)
    uint64_t DeathFrame() const { return _deathFrame; }  // 죽은 프레임 (부활 시점 판정 기준)

    // 피해 — 0 밑이면 0(와이어로 음수 HP 가 나가면 클라 막대가 뒤집힌다). 0 이 되면 죽음 표시까지 하고 true.
    bool TakeDamage(int32_t damage, uint64_t frame);

    // 스킬 비용 — 죽이지 않는다. 「떼고 나서 1 이상」은 부르는 쪽 앞선 검사(CSkillSystem::CheckCaster LOW_HP)가 봤다는 전제.
    void SpendHp(int32_t cost);

    void Heal(int32_t amount);    // 채우기 — 최대 넘으면 최대 (물약)
    void FillToMax();             // 최대로 (레벨업 · 몬스터 생성)
    void ClampToMax();            // 최대를 넘은 것만 깎기 (최대가 낮아졌을 때 — 표를 낮춘 패치 · 효과 풀림)
    void RestoreHp(int32_t saved);// 저장값 넣기 — 0 이하(저장된 적 없음 · 죽은 채 저장은 부르는 쪽이 이미 바꿈)·최대 초과면 최대
    void Revive();                // 부활 — 최대로 · 죽음 지움

    void SetMaxHp(int32_t v)      { _maxHp = v; }       // 체력은 안 건드린다 — 맞추기는 FillToMax · ClampToMax
    void SetDefense(int32_t v)    { _defense = v; }

private:
    // 초기값 0 = 아직 안 정해짐 — 플레이어는 CStatSystem::RecalcStats(레벨 표), 몬스터는 생성자가 채운다(Character.h 주석).
    int32_t  _hp         = 0;   // 현재 체력 (0 이하 = 죽음)
    int32_t  _maxHp      = 0;   // 최대 체력
    int32_t  _defense    = 0;   // 방어력 (최종값)
    bool     _isDead     = false;
    uint64_t _deathFrame = 0;
};

// ── 공격 부품 — 때리는 쪽 ──
//   공격력·방어력(방어력은 체력 부품)은 "최종값"이다 — CStatSystem::RecalcStats 가 「바탕(플레이어 = 레벨 표 · 몬스터 = 몬스터 표) + 착용 장비(플레이어)
//   → 지속 효과 배수(효과 부품)」 순서로 세운다. 몬스터는 생성자가 표 값을 먼저 넣고, 효과가 걸리면 같은 함수가 표 값에서 다시 세운다.
//   때릴 때마다 다시 세지 않는다(바뀌는 자리가 입장·레벨업·끼우기·빼기·효과 걸림·풀림뿐이라 때리는 경로는 필드만 읽는다).
//   피해 = Combat::CalcDamage(때린 쪽 _attackPower, 맞는 쪽 Defense()) — 최소 1.
//   마력은 스킬만 쓰므로 스킬 쿨타임과 같이 여기 둔다 — 체력과 같은 꼴의 규칙(채우기 · 최대 맞추기 · 저장)이라 함수 이름도 같은 꼴이다.
class CAttackPart
{
public:
    int32_t  _attackPower     = 0;   // 공격력 (최종값)
    uint64_t _lastAttackFrame = 0;   // 마지막 공격 프레임 (쿨타임 판정 — 틱 번호 기준). 0 = 아직 한 번도 안 휘둘렀다

#if USE_SKILL
    // 스킬마다 다시 쓸 수 있는 프레임 — 색인 = 스킬 번호(1~MAX_SKILL_ID, [0] 칸은 안 쓴다). 0 = 바로 쓸 수 있다.
    //   성공했을 때만 앞으로 민다(CSkillSystem::UseSkill — 거절은 쿨타임을 안 건다).
    //   사람 것은 DB 에 저장한다(characters.skill_cooldowns — 접속을 끊어도 실제 시각으로 계속 돈다 · CCharacterStore::PackCooldownsAndBuffs).
    //   일반 공격 쿨타임(_lastAttackFrame)과 따로 센다 — 스킬을 써도 일반 공격은 안 막힌다(10-10 기본값).
    //   몬스터·보스도 스킬을 쓴다(10-11) — 그래서 사람·몬스터가 같은 부품을 든다. 시전자는 CCharacter 다(SkillSystem.h).
    uint64_t _skillReadyFrame[MAX_SKILL_ID + 1] = {};

    // 마력 — 스킬 비용(SkillDef::costMp)으로 쓰고 마력 물약으로만 찬다(자연 회복 없음 — 체력과 같다, 10-01).
    //   플레이어는 CStatSystem::RecalcStats 가 레벨 표 maxMp 로 세운다. 몬스터는 0 이다(몬스터 스킬은 비용 0 만 허용 — 마력 칸은 몬스터 표에 없다).
    //   부활해도 그대로다(Combat::Respawn 은 체력만 채운다). 체력과 같이 DB 에 저장한다(characters.mp — 들어올 때 CStatSystem::RestoreHpMp).
    //   스킬 토글 소유다 — 마력을 쓰는 곳이 스킬뿐이라서.

    int32_t Mp() const    { return _mp; }
    int32_t MaxMp() const { return _maxMp; }

    void SpendMp(int32_t cost);    // 스킬 비용 — 「모자라지 않음」은 앞선 검사(CSkillSystem::CheckCaster LOW_MP)가 봤다는 전제
    void GainMp(int32_t amount);   // 채우기 — 최대 넘으면 최대 (마력 물약)
    void FillMpToMax();            // 최대로 (레벨업)
    void ClampMpToMax();           // 최대를 넘은 것만 깎기
    void RestoreMp(int32_t saved); // 저장값 넣기 — 음수(저장된 적 없음)·최대 초과면 최대. 0 은 그대로(다 쓰고 나간 값 — 마력 0 은 살아 있다)
    void SetMaxMp(int32_t v)       { _maxMp = v; }

private:
    int32_t _mp    = 0;   // 현재 마력 — 몬스터는 0(몬스터 스킬은 비용 0 만 허용)
    int32_t _maxMp = 0;   // 최대 마력
#endif
};

#if USE_COMBAT
// ── 효과 부품 — 지속 효과를 받는 쪽 ──
//   지속 효과 칸 — 종류마다 하나(Buff.h BuffSlot · 색인 = BuffKind). 비우는 자리는 CBuffSystem(만료·죽음·퇴장)뿐 — 여기서 직접 손대지 않는다.
//   공격력 최종값은 RecalcStats 가 이 칸을 읽어 곱한다. 몬스터도 같은 칸이다(보스가 스킬로 자기에게 거는 버프 — MonsterAI 가 고르고 CMonsterManager 가 CSkillSystem 을 부른다).
//   전투 토글 소유다 — 거는 쪽이 스킬만이 아니라서(아이템도 걸 수 있는 틀).
//   사람 것은 DB 에 저장한다(characters.buffs — 실제 시각으로 묶어 접속하지 않은 동안에도 흐른다 · CCharacterStore::PackCooldownsAndBuffs).
class CBuffPart
{
public:
    BuffSlot _slots[BUFF_KIND_COUNT];

    bool AnyActive() const;   // 켜진 칸이 하나라도 있나 — CBuffSystem 의 효과 목록(_buffed)에 들어 있어야 하는가
};
#endif