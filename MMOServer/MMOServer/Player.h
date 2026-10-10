#pragma once

#include <cstdint>
#include <string>
#include <random>

#include "Character.h"   // CCharacter ← CActor · Direction · MoveState
#include "Inventory.h"   // USE_INVENTORY — 헤더 안에서 스스로 토글을 본다
#include "SkillTable.h"  // USE_SKILL — MAX_SKILL_ID(쿨타임 배열 크기). 헤더 안에서 스스로 토글을 본다

// CPlayer — 사람이 조종하는 캐릭터.
//   좌표·섹터·이동은 CActor 에, HP·표시는 CCharacter 에 있다.
//   여기 남은 것은 조종 세션·계정·이동 예산·운영자처럼 "사람이 붙어 있어야" 말이 되는 것뿐이다.
//
// [세션] _sessionId 는 여기에만 있다 — 패킷을 받는 쪽은 플레이어뿐이라 보내는 함수
//        (CGameServer::SendPacket 과 SendXxx)가 전부 CPlayer* 만 받는다. 몬스터를 넘기면 컴파일이 안 된다.
//        섹터 목록(CCharacter*)에서 받는 쪽을 고를 때는 As<CPlayer>(Character.h) 로 확인 후 내려 바꾼다.
class CPlayer : public CCharacter
{
    friend class CGameServer;  // 경계 계층만 _sessionId 접근 허용

public:
    CPlayer();
    ~CPlayer() override;

    // As<CPlayer> 판정 — Character.h As
    static bool Matches(const CCharacter& c) { return c._kind == CharacterKind::PLAYER; }

    bool _isAdmin;        // 운영자 여부

    // USE_DB_WORKER: 계정 식별자 (1단계는 스폰 시 서버가 임시 부여, 2단계는 접속 핸드셰이크로 수신)
    int64_t _accountId = 0;
    // USE_DB_WORKER: 마지막 저장 이후 값이 바뀌었는지 (주기 저장 대상 선별, 저장 후 리셋)
    //   위치뿐 아니라 경험치·레벨이 바뀔 때도 켠다 — 안 켜면 레벨업이 주기 저장에 안 실린다.
    bool _dbDirty = false;

    // ── 레벨과 경험치 ──
    // 사람만 쌓는다. CCharacter 에 안 올리는 이유 — 몬스터가 레벨을 쓸 자리가 아직 없고,
    //   몬스터가 주는 경험치는 몬스터의 레벨이 아니라 종류별 기획 값이다(Monster.h 의 경계 물음과 같은 결).
    // ★ _exp 는 "지금까지 얻은 총합"이고 표의 필요치도 같은 기준이다 — 레벨 안에서 다시 0부터 안 센다.
    //   그래서 레벨업 판정이 한 줄이고, 한 번에 여러 레벨이 올라도 넘친 경험치가 안 사라진다.
    int32_t _level = 1;
    int64_t _exp   = 0;

    // C2S_MOVE_START 좌표 수용의 초당 이동량 예산 (치트 방지).
    //   수용은 IDLE 전이 때마다 일어나므로 STOP/START 연타로 무제한 순간이동이 가능했다.
    //   경과 시간만큼 자기 속도에 비례해 적립하고, 수용한 거리만큼 차감한다.
    //   초기값 0 + 프레임 0 = 첫 수용 시 경과가 커서 상한까지 적립됨(스폰 직후 만충과 동일).
    float    _moveBudget = 0.0f;       // 남은 예산 (타일)
    uint64_t _moveBudgetFrame = 0;     // 마지막 적립 프레임 (CGameServer::_frameCount 기준)

#if USE_INVENTORY
    // ── 가방 ──
    // 사람만 갖는다(몬스터가 떨어뜨리는 것은 몬스터의 가방이 아니라 종류별 기획 값이 된다).
    //   칸을 바꾼 자리는 반드시 CGameServer::FlushInventory 를 불러야 클라·DB 에 나간다.
    //   ★ 맨 끝에 두는 이유 — 칸 61개(가방 60 + 무기 칸, 약 500B)라 앞에 끼우면 이동 쪽 필드들이 캐시 줄 여러 개 뒤로 밀린다.
    //     가방은 명령·물약 때만 본다.
    CInventory _inventory;

    // 아이템을 다시 쓸 수 있는 프레임(CGameServer::_frameCount 기준) — 성공했을 때만 앞으로 민다.
    //   서버가 틱 수로 잰다(클라 연타·매크로로 물약을 한꺼번에 마시지 못하게). 0 = 바로 쓸 수 있다.
    uint64_t _itemUseReadyFrame = 0;

#if USE_EQUIP
    // 다시 끼우거나 뺄 수 있는 프레임 — 물약과 따로 센다(물약 직후에 무기를 바꿔도 안 막히게).
    //   성공했을 때만 앞으로 민다. 연타 한 번이 DB 두 줄이라 사람당 초당 상한을 두는 것이다.
    uint64_t _equipReadyFrame = 0;
#endif
#endif

#if USE_SKILL
    // 스킬마다 다시 쓸 수 있는 프레임 — 색인 = 스킬 번호(1~MAX_SKILL_ID, [0] 칸은 안 쓴다). 0 = 바로 쓸 수 있다.
    //   성공했을 때만 앞으로 민다(CSkillSystem::UseSkill — 거절은 쿨타임을 안 건다). 저장하지 않는다(접속을 끊으면 풀린다).
    //   일반 공격 쿨타임(_lastAttackFrame)과 따로 센다 — 스킬을 써도 일반 공격은 안 막힌다(10-10 기본값).
    uint64_t _skillReadyFrame[MAX_SKILL_ID + 1] = {};
#endif

private:
    // 이 플레이어를 조종하는 네트워크 세션. -1 = 아직 없음 — 스폰 경로(CGameServer::SpawnPlayer)가
    //   EnterZone → 세션 부여 순이라 EnterZone 시점엔 잠시 -1 이다. SendPacket 이 이 값을 검사한다.
    int64_t _sessionId = -1;
};
