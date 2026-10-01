// ==========================================================================
// CombatSystem — 공격 흐름 (USE_COMBAT)
//
// [책임]
//  - 공격 한 번의 처음부터 끝까지: 쿨타임 판정 → 대상 찾기(섹터 훑기) → 피해 → 죽음 필드
//  - 그 과정에서 정한 것을 지표(CGameMonitor)로 센다 — 무엇을 정했는지는 여기서만 안다
//  - 송신·죽은 목록·보상은 하지 않는다 — 일이 생기면 ICombatEvents 로 알리고, 받는 쪽(CGameServer)이 한다
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

#include "BuildConfig.h"   // USE_COMBAT — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_COMBAT

class CZone;
class CCharacter;
class CGameMonitor;

// 공격 흐름이 일이 생길 때 부르는 알림 — CGameServer 가 구현한다(패킷 조립·송신·죽은 목록·보상).
//   "패킷을 보내라"가 아니라 "무슨 일이 있었다"로 알린다 — 패킷 조립 함수는 GameServer.cpp 안에 있고,
//   죽은 것을 어느 목록에 넣을지도 받는 쪽이 정한다.
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

    // 죽었다 — 죽음 필드(_isDead·_deathFrame·_moveState)와 죽음 계수를 적은 뒤에 온다.
    //   받는 쪽 몫: 죽은 목록 넣기 → S2C_DEATH → 보상(경험치·드랍), 이 순서.
    virtual void OnDeath(CZone* zone, CCharacter* attacker, CCharacter* target) = 0;
};

class CCombatSystem
{
public:
    CCombatSystem(CGameMonitor& gameMonitor, ICombatEvents& events);

    // 공격 한 번 — 플레이어(RecvAttack)와 몬스터(CMonsterManager::Think)가 같이 쓴다.
    //   frame 은 CGameServer::_frameCount — 쿨타임과 죽은 프레임의 기준.
    void PerformAttack(CZone* zone, CCharacter* attacker, uint64_t frame);

private:
    CGameMonitor&  _gameMonitor;
    ICombatEvents& _events;
};

#endif // USE_COMBAT
