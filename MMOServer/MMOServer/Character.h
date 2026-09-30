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
// [세션] 조종 세션(_sessionId)은 여기 없고 CPlayer 에 있다. 패킷을 받는 쪽은 플레이어뿐이라
//        보내는 함수(CGameServer::SendPacket 과 SendXxx)는 전부 CPlayer* 만 받는다 — 몬스터에게
//        보내는 코드는 컴파일이 안 된다. 섹터 목록(CCharacter*)에서 받는 쪽을 고를 때는
//        AsPlayer(Player.h) 로 확인 후 내려 바꾼다. 코드 전체에서 내려 바꾸는 자리는 그 한 곳뿐이다.
//
// [종류] _kind 로 종류를 가른다 — 정원 카운터(_playerCount), AsPlayer, 섹터 목록 고르기(CSectorManager::AddCharacter).
//        파생 생성자가 자기 것을 잡는다 — 여기 기본값은 PLAYER 이고 CMonster 가 MONSTER 로 덮는다.
//        생성자 뒤에는 안 바뀐다 — AsPlayer 와 섹터 목록 선택(넣을 때·뺄 때 같은 목록)이 이 전제에 기댄다.
//
// [컨테이너] 존(_characterList)은 vector<CCharacter*> 하나(틱 순회용 — 이동 갱신은 종류가 상관없다).
//        섹터는 목록 둘 — 플레이어(CPlayer*)와 나머지(CCharacter*), SectorManager.h [목록 둘] 참조.
//        CActor* 가 아닌 이유 — 전투 판정이 _hp 를 봐야 하는데, 섹터에서 꺼낸 것이 곧바로 CCharacter 여야 캐스팅이 없다.
//        "위치는 있는데 HP 없는 것"이 생기면 그때 CActor* 컨테이너를 따로 둔다.
//
// [가상 함수] 소멸자 말고는 두지 않는다 — CActor 의 원칙 그대로. 종류별 동작이 필요해지면 그때 정한다.
// ==========================================================================

// 스탯 기본값 — 종류별 값은 파생이 정한다 (플레이어는 지금 이 값 그대로)
//   ★ USE_EXP 가 켜져 있으면 스폰 때 레벨 표(LevelTable.csv)가 곧바로 덮어쓴다.
//     그래서 이 값은 표의 1레벨 칸과 반드시 같아야 한다 — 어긋나면 토글을 끈 빌드와
//     켠 빌드의 1레벨 체력이 달라져서 "같은 캐릭터인데 빌드마다 다른" 상태가 된다.
constexpr int32_t CHARACTER_DEFAULT_MAX_HP        = 800;
constexpr int32_t CHARACTER_DEFAULT_ATTACK_POWER  = 10;
constexpr int32_t CHARACTER_DEFAULT_DEFENSE       = 0;

// 캐릭터 종류 — 위 [종류] 참조. 와이어에는 안 실린다(클라는 displayChar 로 종류를 안다).
enum class CharacterKind : uint8_t
{
    PLAYER  = 0,
    MONSTER = 1,
};

class CCharacter : public CActor
{
public:
    CCharacter();
    ~CCharacter() override;

    // ── 종류 ── 순회에서 종류를 가를 때 쓴다 (정원 카운터, AI 대상 거르기 등)
    CharacterKind _kind;

    // ── 표시 ── 클라가 그리는 데 쓰는 것. CREATE 패킷에 실린다.
    uint8_t _displayChar;   // 표시 문자 (ASCII: A-Z, a-z, 0-9)
    uint8_t _colorIndex;    // 색상 인덱스 (0-6)

    // ── 전투 스탯 ──
    //   공격력·방어력은 "최종값"이다 — 플레이어는 CGameServer::RecalcStats 가 레벨 표 + 착용 장비로 세운다.
    //   때릴 때마다 다시 세지 않는다(바뀌는 자리가 입장·레벨업·끼우기뿐이라 때리는 경로는 필드만 읽는다).
    //   피해 = Combat::CalcDamage(때린 쪽 _attackPower, 맞는 쪽 _defense) — 최소 1.
    int32_t _hp          = CHARACTER_DEFAULT_MAX_HP;        // 현재 체력 (0 이하 = 죽음)
    int32_t _maxHp       = CHARACTER_DEFAULT_MAX_HP;        // 최대 체력
    int32_t _attackPower = CHARACTER_DEFAULT_ATTACK_POWER;  // 공격력 (최종값)
    int32_t _defense     = CHARACTER_DEFAULT_DEFENSE;       // 방어력 (최종값) — 맞을 때 공격력에서 뺀다

    bool     _isDead = false;        // 죽은 상태 (이동·공격 거부, 공격 대상에서 제외)
    uint64_t _lastAttackFrame = 0;   // 마지막 공격 프레임 (쿨타임 판정 — CGameServer::_frameCount 기준)
    uint64_t _deathFrame = 0;        // 죽은 프레임 (부활 시점 판정 기준)
};
