// ==========================================================================
// Combat — 전투 판정 (USE_COMBAT)
//
// [책임]
//  - 쿨타임·사거리·생사만 보고 "되나 / 왜 안 되나"를 돌려준다 · 피해 크기를 센다(공격력 − 방어력)
//  - 송신도, 죽은 목록 등록도, 존·섹터 조회도 하지 않는다 — 대상 찾기는 CCombatSystem, 송신·죽은 목록은 CGameServer가 한다
//
// [왜 떼어냈나]
//  GameServer.cpp는 이미 2,000줄이 넘고 실험 토글이 수십 곳이다. 판정까지 거기 넣으면
//  컨텐츠와 실험이 한 파일에서 섞인다. 여기는 CCharacter 둘만 알면 되는 순수 계산이라 떼어낼 수 있다.
//
// [계층 경계]
//  CPlayer·CZone·CSerialBuffer를 모른다. 그래서 대상 "목록"도 받지 않는다 —
//  후보를 도는 루프는 CCombatSystem(CombatSystem.h)이 돌고, 여기는 한 쌍씩만 판정한다.
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"   // USE_COMBAT — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include "Character.h"
#include "CharacterComponents.h"   // 규칙 함수는 컴포넌트를 받는다(체력·공격)

namespace Combat
{
    // "앞 한 칸"을 좌표 일치로 잡지 않는 이유 — 서버 좌표가 float이라 정수 칸이 거의 안 맞는다.
    //   거리와 바라보는 방향으로 대신 잡는다.
    //   ★ 사거리만 USE_COMBAT 밖에 둔다 — 몬스터 AI 가 "쫓아가서 어디서 멈추나"를 이 값으로 정한다(MonsterAI.cpp InReach).
    //     전투를 꺼도 몬스터는 쫓아와 곁에 선다(물지만 않는다). 가드 안에 넣으면 멈출 거리를 잃는다.
    constexpr float ATTACK_RANGE     = 1.5f;                        // 타일 — 두 몸의 가장자리 사이 거리. 중심 사이는 AttackReach

    // 몸 반지름 상한(타일) — 몬스터 표의 bodyRadius 가 넘으면 읽기가 거부한다(오타 거르기).
    //   사거리를 훑는 후보 반경이 「맞는 쪽 반지름을 모르는 채」 이 값만큼 넓어지는 상한이기도 하다(CCombatSystem::FindFrontTarget) —
    //   그래서 표가 이 값을 넘는 몬스터를 못 만들어야 후보를 놓치지 않는다.
    constexpr float MAX_BODY_RADIUS  = 4.0f;

    // 닿는 거리 — 중심 사이 거리가 이 값 이하면 사거리 안이다. 때리는 쪽·맞는 쪽 반지름을 둘 다 더해 한 식이 양방향이다(사람→보스 · 보스→사람).
    //   반지름이 0 이면 ATTACK_RANGE 그대로(점끼리). 몬스터 AI 의 쫓다 멈출 거리도 같은 식이다(MonsterAI.cpp InReach).
    inline float AttackReach(const CCharacter& attacker, const CCharacter& target)
    {
        return ATTACK_RANGE + attacker._bodyRadius + target._bodyRadius;
    }

#if USE_COMBAT

    // 틱은 40ms (BuildConfig.h FRAME_INTERVAL_MS)
    constexpr uint64_t ATTACK_COOLDOWN_FRAMES = 12;   // 12 × 40ms ≈ 480ms — 바람의나라류 체감(틱 수로 정한 값이라 유도하지 않는다)
    constexpr uint64_t RESPAWN_DELAY_FRAMES   = 3 * FRAME_PER_SEC;   // 3초

    // 공격이 안 먹힌 사유 — 경우가 넷이다. 하나로 뭉치면 제일 흔한 하나가 나머지를 삼켜
    //   "헛스윙이 많은 건지 쿨타임에 막힌 건지"를 못 읽는다.
    enum class AttackReject : uint8_t
    {
        NONE = 0,     // 막힌 것 없음 — 휘두를 수 있다
        COOLDOWN,     // 쿨타임이 안 찼다
        DEAD_SELF,    // 공격자가 죽어 있다
        NO_TARGET,    // 사거리·방향 안에 아무도 없다 (헛스윙)
        DEAD_TARGET   // 앞에 있는 것이 이미 시체다
    };

    // 휘두를 수 있나 — 쿨타임과 자기 생사만 본다. 대상은 아직 안 본다.
    //   그래서 돌아올 수 있는 값은 NONE·COOLDOWN·DEAD_SELF 셋뿐이다. 때리는 쪽의 체력 컴포넌트(생사)·공격 컴포넌트(쿨타임)을 받는다.
    AttackReject CanAttack(const CHealthComponent& attackerHealth, const CAttackComponent& attackerAttack, uint64_t frame);

    // target이 attacker의 사거리(AttackReach — 몸 반지름 포함) 안이면서 정면에 있나. 맞으면 outDistSq에 거리제곱을 담는다.
    //   정면 조건은 몸 크기와 무관하게 두 중심의 방향이다.
    //   생사는 보지 않는다 — 시체를 걸러내는 건 호출자 몫이다(거부 사유를
    //   no_target / dead_target 으로 갈라야 해서, 여기서 조용히 빼면 그 구분이 사라진다).
    bool IsInAttackRange(const CCharacter& attacker, const CCharacter& target, float& outDistSq);

    // 피해 계산 — 공격력 − 방어력, 최소 MIN_DAMAGE. 방어력이 아무리 높아도 한 대는 한 대다
    //   (0 이면 "맞았는데 아무 일 없음"이라 클라가 그릴 것도, 시험이 셀 것도 없어진다).
    //   최소로 올렸으면 outFloored = true — 표로는 거의 안 닿는 분기라 호출자가 계수기로 센다.
    //   때린 쪽·맞는 쪽이 플레이어든 몬스터든 같은 식이다.
    constexpr int32_t MIN_DAMAGE = 1;
    int32_t CalcDamage(int32_t attack, int32_t defense, bool& outFloored);

    // 피해 적용은 체력 컴포넌트(CHealthComponent::TakeDamage)이 한다 — 체력 0 과 죽음 표시(생사·죽은 프레임)를 같이 적는다.
    //   플레이어의 죽은 목록은 CCombatHandler::OnDeath 몫이다.

    // 부활할 때가 됐나 (살아 있으면 항상 false)
    bool IsRespawnDue(const CHealthComponent& health, uint64_t frame);

    // 제자리 만피 부활 — 좌표는 건드리지 않는다. 부활 지점을 옮기려면 호출자가 좌표를 정하고 부른다.
    //   체력 컴포넌트의 Revive + 걷기 멈춤(CCharacter 칸)이라 캐릭터를 받는다 — 죽는 것은 체력 컴포넌트가 있는 것뿐이다.
    void Respawn(CCharacter& actor);

    // 부활 자리 규칙 한 곳 — 죽은 맵이 마을(기본 맵 = 맵 표 첫 줄)이 아니면 마을 시작 구역으로, 마을이면 제자리.
    //   부르는 곳 둘: CCombatHandler::TickRespawn(부활 틱 — 죽고 3초 뒤) · CEntryHandler::OnCharacterLoaded(죽은 채 저장된 행 —
    //   나가기·부활 전 주기 저장·죽은 채 서버 다운). 두 클래스가 같이 쓰는 규칙이라 서로 부르지 않게 여기 순수 함수로 둔다
    //   (게임 서버 나누기 9 단계 — 전에는 CGameServer 멤버 함수였다).
    inline bool RespawnsToTown(int32_t deathMapId, int32_t townMapId) { return deathMapId != townMapId; }

#endif // USE_COMBAT
}
