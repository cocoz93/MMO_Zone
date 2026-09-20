// ==========================================================================
// MonsterAI — 몬스터 판단 (USE_MONSTER)
//
// [책임]
//  - "지금 상태와 대상 하나"를 주면 "다음 상태·방향·행동"을 돌려준다
//  - 존·섹터·송신·대상 찾기를 하지 않는다 — 전부 CGameServer가 한다
//
// [Combat 과 같은 자리다]
//  Combat 이 "때릴 수 있나"를 판정하듯 여기는 "무엇을 할까"를 판정한다.
//  둘 다 CCharacter 몇 개만 알면 되는 순수 계산이라 GameServer.cpp 밖에 있을 수 있다.
//
// [계층 경계]
//  CZone·CSerialBuffer를 모른다. 그래서 "시야 안의 후보 목록"도 받지 않는다 —
//  후보를 훑어 가장 가까운 하나를 고르는 것은 CGameServer 가 하고, 여기는 그 하나만 본다.
//
// [정면 제약]
//  Combat::IsInAttackRange 는 정면만 인정한다(바라보는 쪽과의 내적 > 0).
//  그래서 ATTACK 상태로 가기 전에 반드시 대상 쪽을 보게 만들어야 한다 — FaceTo 가 그 일을 한다.
//  이걸 빠뜨리면 뱀이 대상 옆에 붙어서도 영원히 헛스윙만 한다.
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"

#if USE_MONSTER

#include "Monster.h"

namespace MonsterAI
{
    // 틱은 40ms (CGameServer::FRAME_INTERVAL_MS)
    constexpr uint64_t THINK_INTERVAL_FRAMES = 10;   // 0.4초마다 판단 — 매 틱 돌면 단일 코어 게임루프가 손해다
    //   0.4초 = 시야(6타일) ÷ 뱀 속도(15타일/초). 배회하는 뱀은 0.4초면 시야 끝에서 끝까지 가므로
    //   이보다 드물게 보면 시야 안의 플레이어를 한 번도 못 보고 지나칠 수 있다.
    //   ※ 아래 판단 주기들은 모두 "뱀 속도 15"를 전제로 계산했다. 속도는 INI([Game] SnakeSpeed)로 바뀐다 —
    //     느리게 하면(게임용 5) 판단 사이 이동이 줄어 더 안전하고, 15 보다 빠르게 하면 다시 따져야 한다.
    //   ★ 가만히(IDLE) 있는 뱀은 예외로 쉬는 시간(1~3초) 끝에만 본다 — 움직이지 않으니 누구 위로
    //     걸어갈 일이 없고, 늦게 알아채는 것은 "졸던 뱀"으로 보일 뿐이다(사용자 결정 2026-09-19).

    // ★ 쫓는 동안은 매 틱 본다.
    //   뱀 속도가 15타일/초라 0.4초에 6타일을 간다. 사거리는 1.5타일뿐이라
    //   10틱 주기로는 "사거리 안"인 순간을 통째로 건너뛴다 — 실제로 1.3타일까지 다가오고도
    //   20초 동안 한 번도 못 물었다.
    //   2틱으로 줄였더니 이번엔 판단 사이에 1.2타일을 가서, 사거리에 든 것을 알아챌 때는
    //   이미 0.3타일까지 파고들어 대상과 겹쳤다(웹 캡처에서 뱀이 캐릭터 발밑에 숨었다).
    //   1틱이면 판단 사이 0.6타일이라 0.9~1.5타일에서 선다 — 옆에 붙어 무는 모양이 된다.
    //   비용은 CHASE 인 몬스터에만 붙는다(대부분은 WANDER·IDLE 이라 10틱 그대로다).
    constexpr uint64_t CHASE_THINK_INTERVAL_FRAMES = 1;
    constexpr uint64_t IDLE_MIN_FRAMES       = 25;   // 1초
    constexpr uint64_t IDLE_MAX_FRAMES       = 75;   // 3초 — 이 사이에서 랜덤하게 쉬었다 움직인다
    constexpr uint64_t WANDER_MIN_FRAMES     = 25;   // 한 방향으로 걷는 최소 시간
    constexpr uint64_t WANDER_MAX_FRAMES     = 75;

    // 시야 6타일 — 화면 안에서 "따라붙는" 것이 보이는 거리.
    // 추적 포기 12타일 — 시야의 2배. 이 밖으로 나가면 대상을 놓고 배회로 돌아간다.
    //   (스폰 지점 귀환은 이번 범위 밖이다 — 그 자리에서 푼다)
    constexpr float SIGHT_RANGE     = 6.0f;
    constexpr float SIGHT_RANGE_SQ  = SIGHT_RANGE * SIGHT_RANGE;
    constexpr float LEASH_RANGE     = 12.0f;
    constexpr float LEASH_RANGE_SQ  = LEASH_RANGE * LEASH_RANGE;

    // 판단 결과 — CGameServer 가 이걸 받아 상태를 적용하고, 바뀐 것만 브로드캐스트한다.
    struct Decision
    {
        AIState   nextState;
        Direction direction;
        MoveState moveState;
        int32_t   targetActorId;    // -1 = 없음
        bool      attack;           // 이번 판단에 휘두를 것인가 (쿨타임까지 보고 정한다)
        uint64_t  nextThinkFrame;   // 다음 판단 시점 — "얼마나 자주 주변을 보나"
        uint64_t  stateEndFrame;    // 배회를 끝낼 시점 — "얼마나 오래 배회하나" (위와 따로다)
    };

    // 대상이 시야 안인가 (거리만 본다 — 방향은 안 본다. 뒤에서 다가와도 알아챈다)
    bool IsInSight(const CMonster& monster, const CCharacter& target, float& outDistSq);

    // 추적을 포기할 거리인가 (시야 2배 밖)
    bool IsTooFar(const CMonster& monster, const CCharacter& target);

    // 대상 쪽을 보는 4방향 — x·y 중 더 많이 벌어진 축을 고른다.
    //   대각선에 있어도 내적이 양수라 Combat::IsInAttackRange 를 통과한다.
    Direction FaceTo(const CActor& from, const CActor& to);

    // 다음에 무엇을 할까.
    //   target == nullptr 이면 "시야 안에 아무도 없다"는 뜻이다(대상을 놓친 경우 포함).
    //   frame 은 CGameServer::_frameCount — 쿨타임 판정에 쓴다.
    Decision Think(const CMonster& monster, const CCharacter* target, uint64_t frame);
}

#endif // USE_MONSTER
