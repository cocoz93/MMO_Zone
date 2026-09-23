#pragma once

#include <cstdint>
#include <string>
#include <random>

#include "Character.h"   // CCharacter ← CActor · Direction · MoveState
#include "Inventory.h"   // USE_INVENTORY — 헤더 안에서 스스로 토글을 본다

// CPlayer — 사람이 조종하는 캐릭터.
//   좌표·섹터·이동은 CActor 에, HP·표시·조종 세션은 CCharacter 에 있다.
//   여기 남은 것은 계정·이동 예산·운영자처럼 "사람이 붙어 있어야" 말이 되는 것뿐이다.
class CPlayer : public CCharacter
{
public:
    CPlayer();
    ~CPlayer() override;

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
    //   ★ 맨 끝에 두는 이유 — 칸 60개(약 490B)라 앞에 끼우면 이동 쪽 필드들이 캐시 줄 여러 개 뒤로 밀린다.
    //     가방은 명령·물약 때만 본다.
    CInventory _inventory;
#endif
};
