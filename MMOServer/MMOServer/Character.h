#pragma once

#include <cstdint>

#include "Actor.h"

// ==========================================================================
// CCharacter — 살아 있고, 싸우고, 화면에 보이는 것
//
// [경계] 여기 두는 것 = HP 를 갖고 맞고 죽을 수 있으며 클라가 그리는 것이면 다 갖는 것.
//        플레이어·몬스터·펫·NPC 가 전부 여기서 갈라진다.
//        위로는 CActor(좌표·섹터·이동만), 아래로는 CPlayer(계정·이동 예산·운영자)가 있다.
//        가르는 물음 하나 — "나무나 포탈에도 말이 되나" → 되면 CActor, 안 되면 여기.
//
// [세션] _sessionId 는 "이 캐릭터를 조종하는 세션"이다. 조종하는 세션이 없으면(몬스터·NPC) -1.
//        CPlayer 가 아니라 여기 두는 이유 — 섹터를 도는 송신 코드가 이미 _sessionId == -1 을
//        "못 보냄"으로 거르고 있어서(CGameServer 의 세션 가드들 — _sessionId 로 grep), 이 필드가 여기 있으면
//        그 가드가 그대로 몬스터 필터가 된다. 순회마다 종류를 묻는 분기를 새로 넣을 필요가 없다.
//
// [종류] _kind 는 _sessionId 와 뜻이 다르다 — 헷갈리면 아래 물음으로 고른다.
//          "지금 이 캐릭터에게 패킷을 보낼 수 있나"  → _sessionId   (송신 가드)
//          "이 캐릭터가 무엇이냐"                    → _kind        (종류 판정)
//        둘이 실제로 갈리는 자리가 있다. 접속 경로(CGameServer::OnConnected)가 EnterZone →
//        _sessionId 부여 순이라, EnterZone 시점엔 플레이어도 _sessionId == -1 이다.
//        그래서 존의 정원 카운터(_playerCount)는 _sessionId 로 가르면 플레이어도 안 세어진다.
//        파생 생성자가 자기 것을 잡는다 — 여기 기본값은 PLAYER 이고 CMonster 가 MONSTER 로 덮는다.
//
// [컨테이너] 섹터·존은 vector<CCharacter*> 를 담는다. CActor* 가 아닌 이유 — 전투 판정이 _hp 를
//        봐야 하는데, 섹터에서 꺼낸 것이 곧바로 CCharacter 여야 캐스팅이 없다.
//        "위치는 있는데 HP 없는 것"이 생기면 그때 CActor* 컨테이너를 따로 둔다.
//
// [가상 함수] 소멸자 말고는 두지 않는다 — CActor 의 원칙 그대로. 종류별 동작이 필요해지면 그때 정한다.
// ==========================================================================

// 스탯 기본값 — 종류별 값은 파생이 정한다 (플레이어는 지금 이 값 그대로)
constexpr int32_t CHARACTER_DEFAULT_MAX_HP        = 100;
constexpr int32_t CHARACTER_DEFAULT_ATTACK_POWER  = 10;

// 캐릭터 종류 — 위 [종류] 참조. 와이어에는 안 실린다(클라는 displayChar 로 종류를 안다).
enum class CharacterKind : uint8_t
{
    PLAYER  = 0,
    MONSTER = 1,
};

class CCharacter : public CActor
{
    friend class CGameServer;  // 경계 계층만 _sessionId 접근 허용

public:
    CCharacter();
    ~CCharacter() override;

    // ── 종류 ── 순회에서 종류를 가를 때 쓴다 (정원 카운터, AI 대상 거르기 등)
    CharacterKind _kind;

    // ── 표시 ── 클라가 그리는 데 쓰는 것. CREATE 패킷에 실린다.
    uint8_t _displayChar;   // 표시 문자 (ASCII: A-Z, a-z, 0-9)
    uint8_t _colorIndex;    // 색상 인덱스 (0-6)

    // ── 전투 스탯 ──
    int32_t _hp          = CHARACTER_DEFAULT_MAX_HP;        // 현재 체력 (0 이하 = 죽음)
    int32_t _maxHp       = CHARACTER_DEFAULT_MAX_HP;        // 최대 체력
    int32_t _attackPower = CHARACTER_DEFAULT_ATTACK_POWER;  // 한 대당 피해량

    bool     _isDead = false;        // 죽은 상태 (이동·공격 거부, 공격 대상에서 제외)
    uint64_t _lastAttackFrame = 0;   // 마지막 공격 프레임 (쿨타임 판정 — CGameServer::_frameCount 기준)
    uint64_t _deathFrame = 0;        // 죽은 프레임 (부활 시점 판정 기준)

private:
    int64_t _sessionId;   // 이 캐릭터를 조종하는 네트워크 세션 (-1: 없음 — 몬스터·NPC). CGameServer 만 접근
};
