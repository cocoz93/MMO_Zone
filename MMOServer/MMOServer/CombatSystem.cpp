#include "CombatSystem.h"

#if USE_COMBAT

#include "Combat.h"          // 판정 — 쿨타임·사거리·피해 크기 (순수 계산)
#include "Zone.h"            // GetSectorManager
#include "SectorManager.h"   // 사거리가 걸치는 섹터 훑기
#include "Player.h"          // GetSectorPlayers 가 CPlayer* 를 돌려준다 — CCharacter* 로 넘기려면 완전한 타입이 필요
#include "GameMonitor.h"     // 공격·죽음 계수기
#if USE_MONSTER
#include "Monster.h"         // 죽은 몬스터를 부활 대기열에 넣을 때 CMonster* 로 내려 바꾼다 — 완전한 타입이 필요
#endif
#include <cassert>

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

    // ── 1) 휘두를 수 있나 (쿨타임·자기 생사) ──
    const Combat::AttackReject reject = Combat::CanAttack(*player, frame);
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

    player->_lastAttackFrame = frame;

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

    // ── 3) 앞 한 칸에서 대상 고르기 — 사거리·정면 중 가장 가까운 하나 ──
    //   사거리(1.5타일)가 실제로 걸치는 섹터만 훑는다. 섹터가 20타일이라 대부분 1개이고,
    //   섹터 경계에 서 있을 때만 2~4개가 된다.
    //   GetAroundPlayers(3×3)를 쓰지 않는 이유 — 그건 브로드캐스트용이라 60×60타일을 모으고,
    //   후보를 벡터에 전부 복사까지 한다(SectorManager.cpp GetAroundPlayers). 1.5타일 판정에 그 범위를
    //   뒤지면 동접이 커질수록 게임 루프(단일 코어)만 손해다. 여기서는 참조로 훑어 복사가 0이다.
    const CSectorManager& sectors = zone->GetSectorManager();

    CCharacter* target = nullptr;
    float bestDistSq = 0.0f;
    bool  sawDeadInRange = false;   // 사거리 안에 있긴 했는데 시체였다 — "아무도 없었다"와 다르다

    // 후보 하나를 재 본다 — 아래 두 순회가 같은 판정을 쓴다
    auto consider = [&](CCharacter* candidate)
    {
        if (candidate == player)
            return;   // 본인 제외 (GetAroundPlayers 의 exclude 인자를 대신한다)

        float distSq = 0.0f;
        if (!Combat::IsInAttackRange(*player, *candidate, distSq))
            return;

        if (candidate->_isDead)
        {
            sawDeadInRange = true;
            return;
        }

        if (target == nullptr || distSq < bestDistSq)
        {
            target = candidate;
            bestDistSq = distSq;
        }
    };

    // 어느 섹터를 도나는 섹터 관리자(ForEachSectorInRange — 사거리가 걸치는 칸만), 그 칸에서 무엇을 보나는 여기
    sectors.ForEachSectorInRange(player->_x, player->_y, Combat::ATTACK_RANGE, [&](int32_t sx, int32_t sy)
    {
#if USE_MONSTER
        // 몬스터는 플레이어 목록만 돈다 — 몬스터는 몬스터를 못 때린다(같은 편이다).
        //   섞인 목록에서 종류로 거르던 시절엔 이 검사를 빠뜨리면 뱀 여럿이 한 플레이어에게 몰릴 때
        //   뒤의 뱀이 "정면의 가장 가까운 것"인 앞의 뱀을 물었다(실측: 플레이어 셋 90초에 뱀→뱀 4~32회,
        //   뱀이 죽기까지 했다). 지금은 목록이 갈라져 있어 뱀은 후보에 아예 안 들어온다.
        //   AI 의 대상 찾기(FindAITarget)도 플레이어 목록만 돈다 — AI 는 "누구를 쫓을까"를 정할 뿐이고,
        //   실제로 무는 대상은 여기서 다시 고르므로 두 자리가 다 플레이어 목록이어야 한다.
        if (byMonster)
        {
            for (CPlayer* candidate : sectors.GetSectorPlayers(sx, sy))
                consider(candidate);
            return;
        }
#endif
        // 플레이어가 때릴 때는 보이는 것 전부(플레이어·몬스터)가 후보다
        sectors.ForEachInSector(sx, sy, consider);
    });

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

    // ── 4) 피해 적용 ──
    //   ★ 한 번 센 값을 적용에도 방송에도 쓴다 — 두 자리가 따로 세면 깎인 체력과 화면의 숫자가 갈릴 수 있다.
    //   최소 1 로 올린 타격은 때린 쪽 종류와 무관하게 센다(뱀이 방어력 높은 사람을 문 것도 여기로 온다).
    bool floored = false;
    const int32_t damage = Combat::CalcDamage(player->_attackPower, target->_defense, floored);
    if (floored)
        _gameMonitor._combat._damageFloored.Inc();

    int32_t remainHp = 0;
    const bool died = Combat::ApplyDamage(*target, damage, remainHp);

    _events.OnHit(zone, player, target, damage, remainHp);

    if (!died)
        return;

    // ── 5) 죽음 ──
    target->_isDead     = true;
    target->_deathFrame = frame;
    target->_moveState  = MoveState::IDLE;   // 시체가 계속 걸어가지 않게

    // ★ 죽음은 "당한 쪽"으로 가른다 — 때린 쪽(byMonster)으로 가르면
    //   플레이어가 뱀을 죽인 것이 "플레이어 죽음"으로 세어진다.
#if USE_MONSTER
    if (target->_kind == CharacterKind::MONSTER)
    {
        _gameMonitor._monster._deathsTotal.Inc();

        // 죽은 몬스터는 자기 존의 부활 대기열로 — 존 안의 일이라 여기(존 층)서 넣는다(10-06, 게임 서버 OnDeath 에서 옮김).
        //   게임 서버에 맡기면 몬스터가 죽는 길이 늘 때마다(스킬 등) 거기서 이 분기를 또 적어야 하고, 빠뜨리면 영영 안 살아난다.
        //   플레이어 죽은 목록은 게임 서버 것(OnDeath) — 부활 자리·시간이 아직 임시 사양이라 그쪽에 둔다(10-05 결정).
        assert(target->_zoneId == zone->GetZoneId());   // 사거리 안에서 맞았다 — 같은 존이다
        zone->GetMonsterManager().QueueRespawn(static_cast<CMonster*>(target));
    }
    else
    {
        _gameMonitor._combat._deathsTotal.Inc();
    }
#else
    _gameMonitor._combat._deathsTotal.Inc();
#endif

    // 플레이어 죽은 목록 · 죽음 방송 · 보상(경험치·드랍)은 받는 쪽 몫 — 순서는 ICombatEvents::OnDeath 주석
    _events.OnDeath(zone, player, target);
}

#endif // USE_COMBAT
