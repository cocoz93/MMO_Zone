// ==========================================================================
// Combat — 전투 판정 (USE_COMBAT)
//
// [책임]
//  - 쿨타임·사거리·생사만 보고 "되나 / 왜 안 되나"를 돌려준다
//  - 송신도, 죽은 목록 등록도, 존·섹터 조회도 하지 않는다 — 전부 CGameServer가 한다
//
// [왜 떼어냈나]
//  GameServer.cpp는 이미 2,000줄이 넘고 실험 토글이 수십 곳이다. 판정까지 거기 넣으면
//  컨텐츠와 실험이 한 파일에서 섞인다. 여기는 CActor 둘만 알면 되는 순수 계산이라 떼어낼 수 있다.
//
// [계층 경계]
//  CPlayer·CZone·CSerialBuffer를 모른다. 그래서 대상 "목록"도 받지 않는다 —
//  후보를 도는 루프는 CGameServer가 돌고, 여기는 한 쌍씩만 판정한다.
// ==========================================================================
#pragma once

#include <cstdint>

#include "Actor.h"

namespace Combat
{
    // 틱은 40ms (CGameServer::FRAME_INTERVAL_MS)
    constexpr uint64_t ATTACK_COOLDOWN_FRAMES = 12;   // 12 × 40ms ≈ 480ms — 바람의나라류 체감
    constexpr uint64_t RESPAWN_DELAY_FRAMES   = 75;   // 75 × 40ms = 3초

    // "앞 한 칸"을 좌표 일치로 잡지 않는 이유 — 서버 좌표가 float이라 정수 칸이 거의 안 맞는다.
    //   거리와 바라보는 방향으로 대신 잡는다.
    constexpr float ATTACK_RANGE     = 1.5f;                        // 타일
    constexpr float ATTACK_RANGE_SQ  = ATTACK_RANGE * ATTACK_RANGE;

    // 공격이 안 먹힌 사유 — 갈래가 넷이다. 하나로 뭉치면 제일 흔한 하나가 나머지를 삼켜
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
    //   그래서 돌아올 수 있는 값은 NONE·COOLDOWN·DEAD_SELF 셋뿐이다.
    AttackReject CanAttack(const CActor& attacker, uint64_t frame);

    // target이 attacker의 사거리 안이면서 정면에 있나. 맞으면 outDistSq에 거리제곱을 담는다.
    //   생사는 보지 않는다 — 시체를 걸러내는 건 호출자 몫이다(거부 사유를
    //   no_target / dead_target 으로 갈라야 해서, 여기서 조용히 빼면 그 구분이 사라진다).
    bool IsInAttackRange(const CActor& attacker, const CActor& target, float& outDistSq);

    // 피해 적용 — 남은 HP를 outRemainHp에 담고, 이 타격으로 죽었으면 true.
    //   _isDead·_deathFrame 기록과 부활 대기열 등록은 호출자 몫이다(대기열이 CGameServer에 있다).
    bool ApplyDamage(CActor& target, int32_t damage, int32_t& outRemainHp);

    // 부활할 때가 됐나 (살아 있으면 항상 false)
    bool IsRespawnDue(const CActor& actor, uint64_t frame);

    // 제자리 만피 부활 — 좌표는 건드리지 않는다. 부활 지점을 옮기려면 호출자가 좌표를 정하고 부른다.
    void Respawn(CActor& actor);
}
