#include "CombatSystem.h"

#if USE_COMBAT

#include "Combat.h"          // 판정 — 쿨타임·사거리·피해 크기 (순수 계산)
#include "Zone.h"            // GetSectorManager
#include "SectorManager.h"   // 사거리가 걸치는 섹터 훑기
#include "Player.h"          // GetSectorPlayers 가 CPlayer* 를 돌려준다 — CCharacter* 로 넘기려면 완전한 타입이 필요
#include "GameMonitor.h"     // 공격·죽음 계수기
#include "CharacterParts.h"  // 체력·공격 부품 — 입구에서 꺼내 쓴다
#include <cassert>
#if USE_MONSTER
#include "Monster.h"         // 죽음을 몬스터·플레이어로 갈라 셀 때 As<CMonster> — 완전한 타입이 필요
#endif

namespace
{
    // 체력 부품 꺼내기 — 전투 후보·대상은 「맞을 수 있는 것」 목록에서만 온다(사람 · 맞을 수 있는 나머지). 그래서 늘 있다.
    //   부품 없는 것(NPC)이 여기 오면 후보를 고르는 쪽(ForEachCandidate)의 잘못이다.
    CHealthPart& HealthOf(CCharacter* c)
    {
        assert(c->Health() != nullptr && "체력 부품 없는 것이 전투 후보·대상 — 후보 목록이 잘못됐다");
        return *c->Health();
    }

    // 후보 훑기 — 「어느 섹터를 도나」는 섹터 관리자(ForEachSectorInRange — 반경이 걸치는 칸만),
    //   「그 섹터의 어느 목록을 보나」(which)는 여기 한 곳이다. 후보를 훑는 자리가 늘어도 목록 고르기는 이 하나로 —
    //   한쪽 목록을 빠뜨리면 컴파일이 아니라 화면에서 드러나므로 빠뜨릴 자리를 줄인다(SectorManager.h ForEachInSector 와 같은 까닭).
    //   fn 은 CCharacter* 하나를 받는다. 도는 동안 목록을 바꾸지 말 것 — 맞히기(ApplyHit)는 다 고른 뒤에.
    template <typename Fn>
    void ForEachCandidate(const CSectorManager& sectors, float x, float y, float range, CCombatSystem::CandidateList which, Fn&& fn)
    {
        sectors.ForEachSectorInRange(x, y, range, [&](int32_t sx, int32_t sy)
        {
            switch (which)
            {
            case CCombatSystem::CandidateList::PLAYERS:
                for (CPlayer* candidate : sectors.GetSectorPlayers(sx, sy))
                    fn(candidate);
                break;

            case CCombatSystem::CandidateList::OTHERS:
                for (CCharacter* candidate : sectors.GetSectorOthers(sx, sy))
                    fn(candidate);
                break;

            case CCombatSystem::CandidateList::ALL:
                sectors.ForEachInSector(sx, sy, fn);
                break;
            }
        });
    }
}

CCombatSystem::CCombatSystem(CGameMonitor& gameMonitor, ICombatEvents& events)
    : _gameMonitor(gameMonitor)
    , _events(events)
{
}

// 공격 본체 — 플레이어와 몬스터가 같이 쓴다.
//   읽는 필드가 _zoneId·_lastAttackFrame·_direction·_attackPower·_actorId·_x·_y 로
//   전부 CCharacter 것이라 갈라낼 수 있었다(CPlayer 전용 필드가 하나도 없다).
//
//   ★ 지표는 종류별로 가른다. 기준이 둘이다 —
//     공격 계수기는 "때린 쪽"(attacker), 죽음 계수기는 "당한 쪽"(target)으로 간다.
//     죽음을 때린 쪽으로 가르면 플레이어가 뱀을 죽인 것이 "플레이어 죽음"으로 세어진다.
void CCombatSystem::PerformAttack(CZone* zone, CCharacter* player, uint64_t frame)
{
#if USE_MONSTER
    const bool byMonster = (player->_kind == CharacterKind::MONSTER);
#else
    constexpr bool byMonster = false;
#endif

    // 때리는 쪽의 부품 — 부르는 쪽이 사람 요청 처리 · 몬스터 관리자라 둘 다 든다(사람·몬스터 = 체력 + 공격)
    assert(player->Health() != nullptr && player->Attack() != nullptr && "부품 없는 것이 휘두름 — 부르는 쪽이 사람·몬스터가 아니다");
    const CHealthPart& selfHealth = *player->Health();
    CAttackPart&       selfAttack = *player->Attack();

    // ── 1) 휘두를 수 있나 (쿨타임·자기 생사) ──
    const Combat::AttackReject reject = Combat::CanAttack(selfHealth, selfAttack, frame);
    if (reject != Combat::AttackReject::NONE)
    {
#if USE_MONSTER
        // 몬스터 거부는 사유를 안 가르고 한 통에 센다 — AI 가 쿨타임을 먼저 보고
        //   시도하므로 거의 0 이어야 한다. 0 이 아니게 되면 그 전제가 깨진 것이다.
        if (byMonster)
        {
            _gameMonitor._monster._attacksRejected.Inc();
            return;
        }
#endif
        switch (reject)
        {
        case Combat::AttackReject::COOLDOWN:
            _gameMonitor._combat._attacksRejectedCooldown.Inc();
            return;

        case Combat::AttackReject::DEAD_SELF:
            _gameMonitor._combat._attacksRejectedDeadSelf.Inc();
            return;

        default:
            return;   // 여기 오는 나머지 사유는 없다(CanAttack 은 셋만 돌려준다)
        }
    }

    selfAttack._lastAttackFrame = frame;

    // ── 2) 휘두른 모션은 대상 유무와 무관하게 보낸다 (헛스윙도 그려야 한다) ──
    //   그래서 _attacksTotal은 "맞힌 수"가 아니라 "휘두른 수"다. 헛스윙은 여기서도 오르고
    //   아래 rejected{no_target}에서도 오른다 — 두 값의 뜻이 다르므로 겹치는 것이 맞다.
#if USE_MONSTER
    if (byMonster) _gameMonitor._monster._attacksTotal.Inc();
    else           _gameMonitor._combat._attacksTotal.Inc();
#else
    _gameMonitor._combat._attacksTotal.Inc();
#endif
    _events.OnSwing(zone, player);

    // ── 3) 앞 한 칸에서 대상 고르기 — 사거리·정면 중 가장 가까운 하나 (FindFrontTarget) ──
    //   몬스터는 플레이어 목록만 돈다 — 몬스터는 몬스터를 못 때린다(같은 편이다).
    //     섞인 목록에서 종류로 거르던 시절엔 이 검사를 빠뜨리면 뱀 여럿이 한 플레이어에게 몰릴 때
    //     뒤의 뱀이 "정면의 가장 가까운 것"인 앞의 뱀을 물었다(실측: 플레이어 셋 90초에 뱀→뱀 4~32회,
    //     뱀이 죽기까지 했다). 지금은 목록이 갈라져 있어 뱀은 후보에 아예 안 들어온다.
    //     AI 의 대상 찾기(FindAITarget)도 플레이어 목록만 돈다 — AI 는 "누구를 쫓을까"를 정할 뿐이고,
    //     실제로 무는 대상은 여기서 다시 고르므로 두 자리가 다 플레이어 목록이어야 한다.
    //   플레이어가 때릴 때는 보이는 것 전부(플레이어·몬스터)가 후보다
    const CandidateList which = byMonster ? CandidateList::PLAYERS : CandidateList::ALL;
    bool sawDeadInRange = false;   // 사거리 안에 있긴 했는데 시체였다 — "아무도 없었다"와 다르다
    CCharacter* target = FindFrontTarget(zone, player, which, sawDeadInRange);

    if (target == nullptr)
    {
#if USE_MONSTER
        if (byMonster)
        {
            _gameMonitor._monster._attacksRejected.Inc();
            return;
        }
#endif
        if (sawDeadInRange)
            _gameMonitor._combat._attacksRejectedDeadTarget.Inc();
        else
            _gameMonitor._combat._attacksRejectedNoTarget.Inc();
        return;
    }

    // ── 4)·5) 피해·죽음 (ApplyHit) — 스킬과 같이 쓰는 한 곳 ──
    //   ★ 마지막 줄이어야 한다 — 몬스터면 이 안에서 target 이 해제된다
    ApplyHit(zone, player, target, selfAttack._attackPower, frame);
}

// 앞 한 칸에서 대상 하나 — 일반 공격(PerformAttack 3)과 스킬이 같이 쓴다. 후보 목록만 부른 쪽이 고른다.
//   사거리(1.5타일 + 몸 반지름)가 실제로 걸치는 섹터만 훑는다. 섹터가 20타일이라 몸이 작으면 대부분 1개이고,
//   섹터 경계에 서 있을 때만 2~4개가 된다(몸이 큰 대상을 때리는 길은 후보 반경이 상한만큼 넓어져 경계에서 더 자주 늘어난다 — 안 쟀다).
//   GetAroundPlayers(3×3)를 쓰지 않는 이유 — 그건 브로드캐스트용이라 60×60타일을 모으고,
//   후보를 벡터에 전부 복사까지 한다(SectorManager.cpp GetAroundPlayers). 1.5타일 판정에 그 범위를
//   뒤지면 동접이 커질수록 게임 루프(단일 코어)만 손해다. 여기서는 참조로 훑어 복사가 0이다.
CCharacter* CCombatSystem::FindFrontTarget(CZone* zone, CCharacter* attacker, CandidateList which, bool& outSawDeadInRange) const
{
    const CSectorManager& sectors = zone->GetSectorManager();

    CCharacter* target = nullptr;
    float bestDistSq = 0.0f;
    outSawDeadInRange = false;
#if USE_PARTY
    const int32_t myParty = PartyIdOf(attacker);   // 0 = 파티 없음(몬스터 포함) — 그러면 아래 거르기를 안 탄다
#endif

    // 후보 하나를 재 본다 — 어느 목록에서 온 후보든 같은 판정이다(ForEachCandidate 가 목록마다 부른다)
    auto consider = [&](CCharacter* candidate)
    {
        if (candidate == attacker)
            return;   // 본인 제외 (GetAroundPlayers 의 exclude 인자를 대신한다)

        float distSq = 0.0f;
        if (!Combat::IsInAttackRange(*attacker, *candidate, distSq))
            return;

#if USE_PARTY
        // 같은 파티는 대상이 아니다(10-17 사용자 결정 — 평타·공격 스킬 둘 다) — 건너뛰면 그 뒤의 다음 가까운 것(보스)이 맞는다.
        //   사거리 안의 파티원만 센다(시체여도 — 시체 대상 사유로 안 세고 여기서 센다). 범위 스킬 쪽은 CollectCandidates 가 같은 판정으로 거른다.
        if (myParty != 0 && PartyIdOf(candidate) == myParty)
        {
            _gameMonitor._party._mateSkippedFront.Inc();
            return;
        }
#endif

        if (HealthOf(candidate).IsDead())
        {
            outSawDeadInRange = true;
            return;
        }

        if (target == nullptr || distSq < bestDistSq)
        {
            target = candidate;
            bestDistSq = distSq;
        }
    };

    // 사거리가 걸치는 섹터의, which 가 고른 목록만 훑는다(섹터·목록 고르기는 ForEachCandidate 한 곳)
    //   후보 반경 = ATTACK_RANGE + 내 반지름 + 맞는 쪽 반지름. 맞는 쪽은 후보를 보기 전엔 모르므로 상한(MAX_BODY_RADIUS)으로 넓힌다.
    //   단 PLAYERS 목록은 사람뿐이라 반지름이 0 이다(사람은 0 고정 — CCharacter::_bodyRadius) — 몬스터가 때리는 길은 더하지 않는다
    const float targetRadiusMax = (which == CandidateList::PLAYERS) ? 0.0f : Combat::MAX_BODY_RADIUS;
    ForEachCandidate(sectors, attacker->_x, attacker->_y,
                     Combat::ATTACK_RANGE + attacker->_bodyRadius + targetRadiusMax, which, consider);

    return target;
}

// 반경 안의 후보를 다 모은다 — 범위 스킬(CSkillSystem::SelectTargets 의 SHAPE)이 쓴다. 어느 칸이 맞나는 부른 쪽이 거른다.
//   FindFrontTarget 과 같은 훑기(ForEachCandidate)라 후보 목록 고르기가 일반 공격·스킬에서 갈리지 않는다.
void CCombatSystem::CollectCandidates(CZone* zone, float x, float y, float range, CandidateList which, const CCharacter* exclude,
                                      std::vector<CCharacter*>& out) const
{
#if USE_PARTY
    const int32_t myParty = (exclude != nullptr) ? PartyIdOf(exclude) : 0;   // exclude = 시전자 본인 — 그 파티원도 뺀다
#endif
    ForEachCandidate(zone->GetSectorManager(), x, y, range, which, [&](CCharacter* candidate)
    {
        if (candidate == exclude || HealthOf(candidate).IsDead())
            return;   // 본인 · 시체는 안 맞는다
#if USE_PARTY
        // 같은 파티는 대상이 아니다 — FindFrontTarget 과 같은 판정. 모양 판정 전이라 훑은 반경 안의 파티원을 센다(지금 사람 스킬은 몬스터 목록만 봐서 늘 0 — 그래도 둔다, MyDocs/claude-plans/party-plan.md ③).
        if (myParty != 0 && PartyIdOf(candidate) == myParty)
        {
            _gameMonitor._party._mateSkippedArea.Inc();
            return;
        }
#endif
        out.push_back(candidate);
    });
}

// 맞힌 뒤 처리 — 일반 공격(PerformAttack 4·5)과 스킬이 같이 쓴다. 공격값만 부른 쪽이 정한다.
void CCombatSystem::ApplyHit(CZone* zone, CCharacter* attacker, CCharacter* target, int32_t attackValue, uint64_t frame)
{
    // ── 4) 피해 적용 ──
    //   ★ 한 번 센 값을 적용에도 방송에도 쓴다 — 두 자리가 따로 세면 깎인 체력과 화면의 숫자가 갈릴 수 있다.
    //   최소 1 로 올린 타격은 때린 쪽 종류와 무관하게 센다(뱀이 방어력 높은 사람을 문 것도 여기로 온다).
    CHealthPart& targetHealth = HealthOf(target);   // 대상 = 후보 목록에서 고른 것(맞을 수 있는 것만 — 위 후보 훑기)
    bool floored = false;
    const int32_t damage = Combat::CalcDamage(attackValue, targetHealth.Defense(), floored);
    if (floored)
        _gameMonitor._combat._damageFloored.Inc();

    // 체력 0 이 되면 죽음 표시(생사·죽은 프레임)까지 체력 부품이 같이 적는다 — 아래 OnHit 이 불릴 때 이미 서 있다
    //   (OnHit 받는 쪽은 그 칸을 안 읽는다 — CCombatHandler::OnHit 은 피해 방송만).
    const bool died = targetHealth.TakeDamage(damage, frame);
    const int32_t remainHp = targetHealth.Hp();

    _events.OnHit(zone, attacker, target, damage, remainHp);

    if (!died)
        return;

    // ── 5) 죽음 ──
    target->_moveState  = MoveState::IDLE;   // 시체가 계속 걸어가지 않게

    // ★ 죽음은 "당한 쪽"으로 가른다 — 때린 쪽으로 가르면
    //   플레이어가 뱀을 죽인 것이 "플레이어 죽음"으로 세어진다.
#if USE_MONSTER
    if (As<CMonster>(target) != nullptr)
    {
        _gameMonitor._monster._deathsTotal.Inc();   // 죽은 몬스터는 OnDeath 끝에서 지운다 — 되살리지 않고 스폰이 새로 채운다
    }
    else
    {
        _gameMonitor._combat._deathsTotal.Inc();
    }
#else
    _gameMonitor._combat._deathsTotal.Inc();
#endif

    // 플레이어 죽은 목록 · 죽음 방송 · 보상(경험치·드랍) · 몬스터 지우기는 받는 쪽 몫 — 순서는 ICombatEvents::OnDeath 주석
    //   ★ 마지막 줄이어야 한다 — 몬스터면 이 안에서 target 이 해제된다
    _events.OnDeath(zone, attacker, target);
}

#endif // USE_COMBAT
