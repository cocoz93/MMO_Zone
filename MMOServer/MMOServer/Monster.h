#pragma once

#include <cstdint>

#include "BuildConfig.h"

#if USE_MONSTER

#include "Character.h"
#include "CharacterParts.h"   // 부품을 값으로 든다

struct MonsterDef;   // MonsterTable.h — 종류별 사양(몬스터 표의 한 행)

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
//        (지금도 셋을 훑는다 — CPlayerPlacement::RemoveFromZone).
//        AI 는 어차피 판단할 때마다 시야 섹터를 훑으므로, 그 훑기에서 같은 id 를 다시 찾으면 된다.
//        못 찾으면 대상을 잃은 것이고 배회로 돌아간다.
// ==========================================================================

// AI 상태 — 갈래가 넷이라 if/else 가 아니라 switch 로 다룬다(하나가 나머지를 삼키지 않게).
//   3단계에서 쓴다. 2단계는 스폰까지라 전부 IDLE 로 서 있다.
enum class AIState : uint8_t
{
    IDLE = 0,   // 가만히 — 일정 시간 뒤 WANDER
    WANDER,     // 배회   — 시야 안에 산 플레이어가 들어오면 CHASE
    CHASE,      // 추적   — 사거리 안이면 ATTACK, 너무 멀어지거나 놓치면 WANDER
    ATTACK,     // 공격   — 사거리 밖이면 CHASE
};

// 능력치(체력·공격·방어·속도)·경험치·겉모습은 몬스터 표에 있다(Run/bin/MonsterTable.csv · MonsterTable.h).

class CMonster : public CCharacter
{
public:
    // 내 종류의 사양(몬스터 표의 한 행)으로 만든다 — 능력치를 CCharacter 칸에 복사하고 그 행을 가리킨다.
    explicit CMonster(const MonsterDef& def);
    ~CMonster() override;

    // As<CMonster> 판정 — Character.h As
    static bool IsA(const CCharacter& c) { return c._kind == CharacterKind::MONSTER; }

    // ── 부품 — 몬스터도 셋을 다 든다(CharacterParts.h). 생성자 몸에서 CCharacter 부품 자리에 붙인다 ──
    CHealthPart _health;
    CAttackPart _attack;
#if USE_COMBAT
    CBuffPart   _buffs;
#endif

    // 무슨 몬스터냐 — 몬스터 표의 한 행 (CCharacter::_kind 는 "몬스터냐"다).
    //   번호(드랍 표 찾기)·경험치(잡은 사람)·겉모습(와이어)은 여기서 읽는다. 표는 기동 뒤 안 바뀌어 가리킨 행이 안 흔들린다.
    //   체력·공격·방어·속도는 생성자가 CCharacter 칸으로 복사한다 — 전투·이동이 플레이어와 같은 칸을 읽는다.
    const MonsterDef& _def;

    AIState  _aiState        = AIState::IDLE;
    uint64_t _nextThinkFrame = 0;   // 다음 판단 프레임 (CGameServer::_frameCount 기준)
    uint64_t _stateEndFrame  = 0;   // 배회를 언제 끝낼까 — 다음 판단 시점과 따로 둔다.
                                    //   한 값으로 묶었더니 "1~3초 배회"가 곧 "1~3초 동안 주변을 안 봄"이 돼서,
                                    //   시야 안에 들어온 뱀의 절반이 플레이어를 못 알아채고 지나갔다.
    int32_t  _targetActorId  = -1;  // 추적 대상 (-1: 없음). [대상] 참조

    // 스폰 지점 — 귀환(RETURN 상태)을 넣을 때 쓴다. 지금은 기록만 한다.
    float _spawnX = 0.0f;
    float _spawnY = 0.0f;

    // 나를 낳은 스폰 구역 — 이 맵 구역 목록(CSpawnRegionTable::RegionsOf)의 색인. -1 = 아직 안 깔림
    int32_t _spawnRegion = -1;
    // 내가 맡은 점 — 그 구역의 점 목록(SpawnRegion::spots)의 색인. 죽으면 몬스터 관리자가 이 점을 비우고
    //   (CMonsterManager::Despawn) 그 구역 다음 리젠 때 찬다. -1 = 아직 안 깔림
    int32_t _spawnPoint  = -1;
};

#endif // USE_MONSTER
