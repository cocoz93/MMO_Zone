#pragma once

#include <cstdint>

#include "Actor.h"
#include "SkillTable.h"  // USE_SKILL — MAX_SKILL_ID(쿨타임 배열 크기). 헤더 안에서 스스로 토글을 본다
#include "Buff.h"        // USE_COMBAT — BuffSlot·BUFF_KIND_COUNT(지속 효과 칸). 같은 규칙

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
//        As<CPlayer>(아래 As) 로 확인 후 내려 바꾼다. 코드 전체에서 내려 바꾸는 자리는 그 한 곳뿐이다.
//
// [종류] _kind 로 종류를 가른다 — 정원 카운터(_playerCount), As(각 타입의 IsA), 섹터 목록 고르기(CSectorManager::AddCharacter).
//        파생 생성자가 자기 것을 잡는다 — 여기 기본값은 PLAYER 이고 CMonster 가 MONSTER 로 덮는다.
//        생성자 뒤에는 안 바뀐다 — As 와 섹터 목록 선택(넣을 때·뺄 때 같은 목록)이 이 전제에 기댄다.
//
// [컨테이너] 존(_characterList)은 vector<CCharacter*> 하나(틱 순회용 — 이동 갱신은 종류가 상관없다).
//        섹터는 목록 둘 — 플레이어(CPlayer*)와 나머지(CCharacter*), SectorManager.h [목록 둘] 참조.
//        CActor* 가 아닌 이유 — 전투 판정이 _hp 를 봐야 하는데, 섹터에서 꺼낸 것이 곧바로 CCharacter 여야 캐스팅이 없다.
//        "위치는 있는데 HP 없는 것"이 생기면 그때 CActor* 컨테이너를 따로 둔다.
//
// [가상 함수] 소멸자 말고는 두지 않는다 — CActor 의 원칙 그대로. 종류별 동작이 필요해지면 그때 정한다.
// ==========================================================================

// 스탯 기본값은 코드에 두지 않는다(10-06, 아키텍처 전수검사 6번 — 전에는 CHARACTER_DEFAULT_* 상수가 표의 1레벨 칸과 두 벌이었다).
//   플레이어는 레벨 표(LevelTable.csv)가 정본이고 CStatSystem::RecalcStats 가 채운다. 몬스터는 파생 생성자가 종류 값을 넣는다.
//   아래 멤버의 0 은 「아직 안 정해짐」이다 — 전투를 끈 빌드는 0 그대로 두고, 스냅샷도 0 을 싣는다(GameServer.cpp WriteSnapshot).

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
    //   공격력·방어력은 "최종값"이다 — CStatSystem::RecalcStats 가 「바탕(플레이어 = 레벨 표 · 몬스터 = 몬스터 표) + 착용 장비(플레이어)
    //   → 지속 효과 배수(아래 _buffs)」 순서로 세운다. 몬스터는 생성자가 표 값을 먼저 넣고, 효과가 걸리면 같은 함수가 표 값에서 다시 세운다.
    //   때릴 때마다 다시 세지 않는다(바뀌는 자리가 입장·레벨업·끼우기·빼기·효과 걸림·풀림뿐이라 때리는 경로는 필드만 읽는다).
    //   피해 = Combat::CalcDamage(때린 쪽 _attackPower, 맞는 쪽 _defense) — 최소 1.
    //   초기값 0 = 아직 안 정해짐 — 플레이어는 RecalcStats(표), 몬스터는 파생 생성자가 채운다(위 주석).
    int32_t _hp          = 0;   // 현재 체력 (0 이하 = 죽음)
    int32_t _maxHp       = 0;   // 최대 체력
    int32_t _attackPower = 0;   // 공격력 (최종값)
    int32_t _defense     = 0;   // 방어력 (최종값) — 맞을 때 공격력에서 뺀다

    // 몸 반지름(타일) — 사거리를 잴 때 중심 거리에서 몸 크기만큼 뺀다. 0 = 점(사람·작은 몬스터).
    //   몬스터는 생성자가 몬스터 표의 bodyRadius 를 복사하고, 사람은 0 으로 둔다. 웹의 그림 크기(MON_META.scale)와는 따로 사는 값이다.
    float _bodyRadius = 0.0f;

#if USE_COMBAT
    // 지속 효과 칸 — 종류마다 하나(Buff.h BuffSlot · 색인 = BuffKind). 비우는 자리는 CBuffSystem(만료·죽음·퇴장)뿐 — 여기서 직접 손대지 않는다.
    //   공격력 최종값은 위 주석대로 RecalcStats 가 이 칸을 읽어 곱한다. 몬스터도 같은 칸이다(보스가 스킬로 자기에게 거는 버프 — MonsterAI 가 고르고 CMonsterManager 가 CSkillSystem 을 부른다).
    //   전투 토글 소유다 — 거는 쪽이 스킬만이 아니라서(아이템도 걸 수 있는 틀).
    //   사람 것은 DB 에 저장한다(characters.buffs — 실제 시각으로 묶어 접속하지 않은 동안에도 흐른다 · CGameServer::PackCooldownsAndBuffs).
    BuffSlot _buffs[BUFF_KIND_COUNT];
#endif

    bool     _isDead = false;        // 죽은 상태 (이동·공격 거부, 공격 대상에서 제외)
    uint64_t _lastAttackFrame = 0;   // 마지막 공격 프레임 (쿨타임 판정 — CGameServer::_frameCount 기준)
    uint64_t _deathFrame = 0;        // 죽은 프레임 (부활 시점 판정 기준)

#if USE_SKILL
    // 스킬마다 다시 쓸 수 있는 프레임 — 색인 = 스킬 번호(1~MAX_SKILL_ID, [0] 칸은 안 쓴다). 0 = 바로 쓸 수 있다.
    //   성공했을 때만 앞으로 민다(CSkillSystem::UseSkill — 거절은 쿨타임을 안 건다).
    //   사람 것은 DB 에 저장한다(characters.skill_cooldowns — 접속을 끊어도 실제 시각으로 계속 돈다 · CGameServer::PackCooldownsAndBuffs).
    //   일반 공격 쿨타임(_lastAttackFrame)과 따로 센다 — 스킬을 써도 일반 공격은 안 막힌다(10-10 기본값).
    //   여기 있는 까닭 — 몬스터·보스도 스킬을 쓴다(10-11). 시전자는 CCharacter 다(SkillSystem.h).
    uint64_t _skillReadyFrame[MAX_SKILL_ID + 1] = {};

    // 마력 — 스킬 비용(SkillDef::costMp)으로 쓰고 마력 물약으로만 찬다(자연 회복 없음 — 체력과 같다, 10-01).
    //   플레이어는 CStatSystem::RecalcStats 가 레벨 표 maxMp 로 세운다. 몬스터는 0 이다(몬스터 스킬은 비용 0 만 허용 — 마력 칸은 몬스터 표에 없다).
    //   부활해도 그대로다(Combat::Respawn 은 체력만 채운다). 체력과 같이 DB 에 저장한다(characters.mp — 들어올 때 CStatSystem::RestoreHpMp).
    //   스킬 토글 소유다 — 마력을 쓰는 곳이 스킬뿐이라서. 여기 있는 까닭은 위 쿨타임 배열과 같다.
    int32_t _mp    = 0;   // 현재 마력
    int32_t _maxMp = 0;   // 최대 마력
#endif
};

// 내려 바꾸는 자리는 코드 전체에서 이 함수 하나뿐이다 — 다른 곳의 static_cast<CPlayer*>·<CMonster*> 는 잘못이다.
//   종류 판정은 각 타입의 IsA 가 한다. 상속이 한 단계 늘면(보스 : CMonster) 그 IsA 만 범위로 바꾼다.
template <class T>
T* As(CCharacter* c)
{
    return T::IsA(*c) ? static_cast<T*>(c) : nullptr;
}
