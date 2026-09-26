#pragma once

#include <cstdint>

#include "BuildConfig.h"

#if USE_MONSTER

#include "Character.h"

// ==========================================================================
// CMonster — 사람이 조종하지 않는 적
//
// [경계] 여기 두는 것 = 스스로 판단해서 움직이는 데 필요한 것.
//        좌표·섹터·이동은 CActor 에, HP·표시는 CCharacter 에 있다.
//        가르는 물음 하나 — "플레이어에게도 말이 되나" → 되면 위로, 안 되면 여기.
//        CCharacter 에 올리지 않는 이유 — 플레이어 5,000 명이 안 쓰는 필드를 이고 다닌다.
//
// [세션] 몬스터에겐 세션이 없다 — _sessionId 는 CPlayer 에만 있다. 보내는 함수가 CPlayer* 만
//        받으므로 몬스터에게 패킷을 보내는 코드는 컴파일이 안 된다(Character.h [세션]).
//
// [대상] 추적 대상을 포인터가 아니라 _targetActorId 로 든다. 포인터로 들면 대상이 퇴장할 때
//        매달린 포인터가 되고, 그걸 막으려면 퇴장 경로가 훑어야 할 대기열이 하나 더 는다
//        (지금도 셋을 훑는다 — CGameServer::BroadcastLeaveZone).
//        AI 는 어차피 판단할 때마다 시야 섹터를 훑으므로, 그 훑기에서 같은 id 를 다시 찾으면 된다.
//        못 찾으면 대상을 잃은 것이고 배회로 돌아간다.
// ==========================================================================

// 몬스터 종류 — 지금은 뱀 하나. 늘어나면 스탯표를 따로 뺀다.
enum class MonsterKind : uint8_t
{
    SNAKE = 0,
};

// AI 상태 — 갈래가 넷이라 if/else 가 아니라 switch 로 다룬다(하나가 나머지를 삼키지 않게).
//   3단계에서 쓴다. 2단계는 스폰까지라 전부 IDLE 로 서 있다.
enum class AIState : uint8_t
{
    IDLE = 0,   // 가만히 — 일정 시간 뒤 WANDER
    WANDER,     // 배회   — 시야 안에 산 플레이어가 들어오면 CHASE
    CHASE,      // 추적   — 사거리 안이면 ATTACK, 너무 멀어지거나 놓치면 WANDER
    ATTACK,     // 공격   — 사거리 밖이면 CHASE
};

// 뱀 기본값 — 근거는 플랜(monster-snake.md 결정 9). 부하 실측 뒤 조정한다.
constexpr int32_t SNAKE_MAX_HP        = 30;   // 플레이어 3 대(10×3)
constexpr int32_t SNAKE_ATTACK_POWER  = 5;    // 플레이어를 20 대에 눕힌다
constexpr int32_t SNAKE_SPEED         = 15;   // 플레이어 최저(25)보다 느리다 — 도망칠 수 있어야 한다

#if USE_EXP
// 잡으면 주는 경험치. 레벨 표(LevelTable.csv)는 2레벨을 100 으로 두므로 뱀 열 마리다.
//   ★ 이 값만 파일로 빼지 않는 이유 — 같은 몬스터의 값이 두 곳(코드·파일)으로 갈린다.
//     위 :29 주석이 예고한 대로 "둘째 종류가 생기는 날" 몬스터 스탯을 통째로 파일로 옮긴다.
//     그때 이 값도 같이 간다.
constexpr int32_t SNAKE_EXP_REWARD    = 10;
#endif

class CMonster : public CCharacter
{
public:
    CMonster();
    ~CMonster() override;

    MonsterKind _kindId = MonsterKind::SNAKE;   // 무슨 몬스터냐 (CCharacter::_kind 는 "몬스터냐"다)

    AIState  _aiState        = AIState::IDLE;
    uint64_t _nextThinkFrame = 0;   // 다음 판단 프레임 (CGameServer::_frameCount 기준)
    uint64_t _stateEndFrame  = 0;   // 배회를 언제 끝낼까 — 다음 판단 시점과 따로 둔다.
                                    //   한 값으로 묶었더니 "1~3초 배회"가 곧 "1~3초 동안 주변을 안 봄"이 돼서,
                                    //   시야 안에 들어온 뱀의 절반이 플레이어를 못 알아채고 지나갔다.
    int32_t  _targetActorId  = -1;  // 추적 대상 (-1: 없음). [대상] 참조

    // 스폰 지점 — 귀환(RETURN 상태)을 넣을 때 쓴다. 지금은 기록만 한다.
    float _spawnX = 0.0f;
    float _spawnY = 0.0f;
};

#endif // USE_MONSTER
