// ==========================================================================
// CombatSystem — 공격 흐름 (USE_COMBAT)
//
// [책임]
//  - 공격 한 번의 처음부터 끝까지: 쿨타임 판정 → 대상 찾기(섹터 훑기) → 피해 → 죽음 필드
//  - 대상 찾기(FindFrontTarget · CollectCandidates)와 맞힌 뒤 처리(ApplyHit)는 따로 열어 둔다 — 스킬도 같은 것을 부른다
//  - 맞지 않고 쓰러뜨리기(Kill — 운영자 /die) — 죽음은 맞아 죽을 때와 같은 한 길(Die)로 간다
//  - 그 과정에서 정한 것을 지표(CGameMonitor)로 센다 — 무엇을 정했는지는 여기서만 안다
//  - 송신·죽은 목록·보상은 하지 않는다 — 일이 생기면 ICombatEvents 로 알리고, 받는 쪽(전투 조각 CCombatHandler)이 한다
//
// [왜 떼어냈나]
//  플레이어(RecvAttack)와 몬스터(CMonsterManager::Think)가 같은 공격을 쓴다. 몬스터 코드가 존 쪽으로 가면서
//  공격도 게임 서버 밖에서 불려야 했는데, 게임 서버 함수로 두면 부르는 쪽이 게임 서버를 알아야 한다.
//
// [계층 경계]
//  CZone·CSectorManager·CCharacter 를 안다(대상 찾기). 세션·패킷(CSerialBuffer)·CGameServer 는 모른다.
//  판정 함수(쿨타임·사거리·피해 크기)는 namespace Combat 것을 부른다 — 거기는 순수 계산으로 남는다.
// ==========================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "BuildConfig.h"   // USE_COMBAT — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_COMBAT

class CZone;
class CCharacter;
class CGameMonitor;

// 공격 흐름이 일이 생길 때 부르는 알림 — 전투 조각 CCombatHandler 가 구현한다(패킷 조립·송신·죽은 목록·보상 부르기 — 게임 서버 나누기 9 단계).
//   "패킷을 보내라"가 아니라 "무슨 일이 있었다"로 알린다 — 패킷 조립 함수는 전투 조각(CombatHandler.cpp) 안에 있고,
//   플레이어 죽은 목록은 받는 쪽이 든다. 죽은 몬스터는 받는 쪽이 OnDeath 끝에서 지운다(되살리지 않고 스폰이 새로 채운다).
//   ★ 순수 가상이라 하나를 빠뜨리면 컴파일이 안 된다(함수를 넘기는 방식이면 첫 죽음 때 실행 중에야 터진다).
class ICombatEvents
{
public:
    virtual ~ICombatEvents() = default;

    // 휘둘렀다 — 대상 유무와 무관하게 온다(헛스윙도 그려야 한다). → S2C_ATTACK
    //   지금은 서버가 정면에서 대상을 고른다(논타겟팅). 꼭 필요해서 넣은 방식이 아니라서,
    //   시간 날 때 클라가 대상을 지정하는 타겟팅으로 고칠 예정이다.
    virtual void OnSwing(CZone* zone, CCharacter* attacker) = 0;

    // 맞혔다 — 피해를 적용한 뒤에 온다. remainHp 는 적용 뒤 체력(0 이면 곧이어 OnDeath). → S2C_DAMAGE
    virtual void OnHit(CZone* zone, CCharacter* attacker, CCharacter* target, int32_t damage, int32_t remainHp) = 0;

    // 죽었다 — 죽음 표시(체력 부품 TakeDamage — 생사·죽은 프레임)·걷기 멈춤(_moveState)과 죽음 계수를 적은 뒤에 온다.
    //   받는 쪽 몫: 플레이어 죽은 목록 넣기 → S2C_DEATH → 보상(경험치·드랍) → 몬스터면 지우기, 이 순서.
    //   attacker 는 때린 쪽 — 운영자 /die(Kill)면 당한 쪽 본인이다(때린 이가 없다 · 보상은 당한 쪽이 몬스터일 때만이라 안 생긴다).
    //   ★ 몬스터면 이 호출 안에서 해제된다 — 부른 쪽은 이 뒤로 target 을 쓰지 말 것(ApplyHit 은 여기가 마지막 줄이다).
    virtual void OnDeath(CZone* zone, CCharacter* attacker, CCharacter* target) = 0;
};

class CCombatSystem
{
public:
    CCombatSystem(CGameMonitor& gameMonitor, ICombatEvents& events);

    // 공격 한 번 — 플레이어(RecvAttack)와 몬스터(CMonsterManager::Think)가 같이 쓴다.
    //   frame 은 CGameServer::_frameCount — 쿨타임과 죽은 프레임의 기준.
    void PerformAttack(CZone* zone, CCharacter* attacker, uint64_t frame);

    // 대상 찾기(앞 한 칸 · 범위)에 넣을 후보 — 섹터의 어느 목록을 보나(SectorManager.h [목록 셋]).
    //   어느 갈래든 「그 밖」 목록(체력 부품 없음 — NPC)은 안 본다 — 맞을 수 없는 것은 후보가 아니다.
    enum class CandidateList : uint8_t
    {
        PLAYERS,        // 플레이어 목록만 — 몬스터가 때리거나 스킬을 쓸 때(몬스터는 몬스터를 못 때린다)
        HITTABLES,      // 맞을 수 있는 나머지만(지금은 몬스터뿐) — 사람이 쓰는 스킬(사람은 안 맞는다)
        ALL_HITTABLE,   // 플레이어 + 맞을 수 있는 나머지 — 사람이 일반 공격할 때(맞을 수 있는 것 전부)
    };

    // 앞 한 칸에서 대상 하나 — 사거리·정면 안에서 살아 있는 것 중 가장 가까운 것. 없으면 nullptr.
    //   본인과 같은 파티원(USE_PARTY)은 후보에서 뺀다. outSawDeadInRange = 사거리 안에 있긴 했는데 시체였다("아무도 없었다"와 가르는 값).
    //   세지도 알리지도 않는다 — 없을 때 무슨 사유로 셀지는 부른 쪽(일반 공격·스킬)마다 다르다.
    CCharacter* FindFrontTarget(CZone* zone, CCharacter* attacker, CandidateList which, bool& outSawDeadInRange) const;

    // 반경 안의 후보를 다 모은다 — 범위 스킬이 쓴다. 모양 판정은 안 한다(어느 칸이 맞나는 부른 쪽 몫 — 스킬의 칸 목록).
    //   (x, y) 둘레 range 가 걸치는 섹터의, which 가 고른 목록에서 살아 있는 것만. exclude(시전자 본인)와 그 파티원(USE_PARTY)은 뺀다.
    //   out 은 비우지 않고 뒤에 붙인다. 한 캐릭터는 한 섹터 목록에만 있어 같은 후보가 두 번 들지 않는다.
    //   ★ 모으기만 한다 — 맞히기(ApplyHit)는 다 모은 뒤에(죽은 몬스터가 섹터 목록에서 빠진다).
    void CollectCandidates(CZone* zone, float x, float y, float range, CandidateList which, const CCharacter* exclude,
                           std::vector<CCharacter*>& out) const;

    // 맞힌 뒤 처리 — 피해(attackValue − 방어력, 최소 1) 적용 → OnHit → 죽었으면 죽음 필드·계수 → OnDeath.
    //   attackValue = 일반 공격은 때린 쪽 _attackPower, 스킬은 스킬이 정한 값.
    //   ★ 몬스터가 죽으면 이 호출 안(OnDeath)에서 해제된다 — 부른 쪽은 이 뒤로 target 을 쓰지 말 것.
    //   ★ 섹터 목록을 도는 중에 부르지 말 것 — 죽은 몬스터가 그 목록에서 빠진다(대상을 다 고른 뒤 부른다).
    void ApplyHit(CZone* zone, CCharacter* attacker, CCharacter* target, int32_t attackValue, uint64_t frame);

    // 맞지 않고 쓰러뜨리기 — 운영자 /die(CCommandHandler · 10-26). 체력을 다 깎고(죽음 표시까지 체력 부품 TakeDamage 한 곳) → 아래 Die(ApplyHit 의 죽음과 같은 길).
    //   피해 알림(OnHit)은 없다 — 맞은 게 아니다. 죽인 쪽 자리에는 당한 쪽 본인을 넘긴다(죽음 방송의 중심 · 보상은 당한 쪽이 몬스터일 때만이라 안 생긴다).
    //   이미 죽었으면 아무것도 안 한다. ★ 몬스터를 넘기면 ApplyHit 처럼 이 안에서 해제된다.
    void Kill(CZone* zone, CCharacter* target, uint64_t frame);

private:
    // 죽음 — 걷기 멈춤 · 죽음 계수 · OnDeath. 맞아 죽든(ApplyHit) 바로 쓰러지든(Kill) 이 한 길이다. ★ 몬스터면 이 안에서 해제된다.
    void Die(CZone* zone, CCharacter* attacker, CCharacter* target);

    CGameMonitor&  _gameMonitor;
    ICombatEvents& _events;
};

#endif // USE_COMBAT
