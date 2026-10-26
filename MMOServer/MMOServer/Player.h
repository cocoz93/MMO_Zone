#pragma once

#include <cstdint>
#include <string>
#include <random>

#include "Character.h"   // CCharacter ← CActor · Direction · MoveState
#include "CharacterParts.h"   // 부품을 값으로 든다
#include "Inventory.h"   // USE_INVENTORY — 헤더 안에서 스스로 토글을 본다

// CPlayer — 사람이 조종하는 캐릭터.
//   좌표·섹터·이동은 CActor 에, HP·표시는 CCharacter 에 있다.
//   여기 남은 것은 조종 세션·계정·이동 예산·운영자처럼 "사람이 붙어 있어야" 말이 되는 것뿐이다.
//
// [세션] _sessionId 는 여기에만 있다 — 패킷을 받는 쪽은 플레이어뿐이라 보내는 함수
//        (CSessionHub::SendPacket 과 게임 서버·그 조각들의 SendXxx)가 전부 CPlayer* 만 받는다. 몬스터를 넘기면 컴파일이 안 된다.
//        섹터 목록(CCharacter*)에서 받는 쪽을 고를 때는 As<CPlayer>(Character.h) 로 확인 후 내려 바꾼다.
class CPlayer : public CCharacter
{
    friend class CSessionHub;  // 세션 칸의 주인 — 칸에 넣을 때 세션을 붙이고, 찾기(재사용 가드)·보내기가 읽는다
    friend class CEntryHandler; // 접속 세션 → 플레이어 관문 — 같은 계정 걷어내기가 옛 세션을 읽어 끊고, 존 이동 요청이 세션 없는 플레이어를 거른다(게임 서버 나누기 12 단계)
    friend class CCharacterStore; // 캐릭터 저장소 — 저장 묶음에 주인(세션)을 적고, 회신과 틱 끝 목록이 그 세션으로 주인을 찾는다(저장 플랜 C)

public:
    CPlayer();
    ~CPlayer() override;

    // As<CPlayer> 판정 — Character.h As
    static bool IsA(const CCharacter& c) { return c._kind == CharacterKind::PLAYER; }

    // ── 부품 — 사람은 셋을 다 든다(CharacterParts.h). 생성자 몸에서 CCharacter 부품 자리에 붙인다 ──
    CHealthPart _health;
    CAttackPart _attack;
#if USE_COMBAT
    CBuffPart   _buffs;
#endif

    bool _isAdmin;        // 운영자 여부

    // ── GM 모드 — 운영자가 채팅 /gm 으로 켜고 끈다(CChatHandler::RunGmCommand) ──
    //   켜면 속도·최대 체력·공격력을 아래 값으로 — 체력·공격력은 스탯을 세우는 한 곳(CStatSystem::RecalcStats)이 레벨 표 대신 쓴다.
    //   저장하지 않는다 — 접속마다 꺼진 채 시작한다. 존을 넘어도(포털·마을 부활) 같은 CPlayer 라 그대로 켜져 있다.
    //   값은 운영 도구 값이라 기획 표가 아니라 여기 둔다.
    static constexpr int32_t GM_SPEED     = 21;        // 칸/초 — 게임 기본 7([Game] PlayerSpeed)의 3배
    static constexpr int32_t GM_MAX_HP    = 1000000;   // 보스 평타(30)로는 닳지 않는 크기
    static constexpr int32_t GM_ATTACK    = 3000;      // 구렁이(체력 30000 · 방어 0)를 열 대 — 체력 반에서 거는 격노를 볼 틈이 남는다
                                                       //   (10-15 기준. 10-18 부터 구렁이 300 · 뱀왕 1000 이라 둘 다 한 대 — 값을 낮출지는 정하지 않았다)
    bool    _gmMode        = false;
    int32_t _speedBeforeGm = 0;   // 켜기 직전 속도 — 끌 때 되돌린다(속도는 레벨 표 값이 아니라 입장 때 정해진 값이라 다시 셈할 곳이 없다)

    // USE_DB_WORKER: 계정 식별자 (1단계는 스폰 시 서버가 임시 부여, 2단계는 접속 핸드셰이크로 수신)
    int64_t _accountId = 0;
    // USE_DB_WORKER: 마지막 저장 이후 위치가 바뀌었는지 (주기 저장 대상 선별, 저장 후 리셋)
    //   정지·접속·존 변경·경계 클램프 때 켠다. 레벨·경험치·체력·마력은 여기가 아니라 아래 _dbSaved 비교로 본다.
    bool _dbDirty = false;

    // USE_DB_WORKER: 마지막으로 저장 잡에 실은 레벨·경험치·체력·마력·돈·쿨타임 칸·버프 칸 — 주기 저장 판정(CCharacterStore::ShouldSave)이 지금 값과 비교한다.
    //   ★ 비교로 보는 이유 — 레벨·경험치·체력·마력은 바뀌는 자리가 여럿이다(피해·부활·물약·스킬 비용·레벨업·입장 보정).
    //     자리마다 _dbDirty 를 켜면 하나를 빠뜨려도 컴파일·화면이 멀쩡하고, 정지한 사람의 그 변경만 조용히 안 써진다.
    //   위치는 비교하지 않는다 — 이동 중(MOVING)은 매 주기 저장하고, 멈출 때 위 _dbDirty 를 켠다.
    //   쿨타임·버프는 틱 값 그대로 든다 — 묶은 바이트(실제 시각)로 견주지 않는 까닭은 CCharacterStore::CooldownsAndBuffsChanged.
    struct DbSavedValues
    {
        int32_t level = 0;
        int64_t exp   = 0;
        int32_t hp    = 0;
        int32_t mp    = 0;
        int64_t money = 0;   // 가진 돈 — 가방을 끈 빌드는 늘 0 끼리 견준다(CCharacterStore::MakeCharRow)
#if USE_SKILL
        uint64_t skillReadyFrame[MAX_SKILL_ID + 1] = {};   // _attack._skillReadyFrame 사본
#endif
#if USE_COMBAT
        BuffSlot buffs[BUFF_KIND_COUNT];                    // _buffs._slots 사본
#endif
    };
    DbSavedValues _dbSaved;

    // USE_DB_WORKER: 비행 기록 — 워커에 넘겼고 회신을 아직 못 받은 저장 묶음(캐릭터당 하나). 저장소(CCharacterStore)만 읽고 쓴다.
    //   「표시는 DB 확정 회신 때만 지운다」를 이 칸으로 지킨다 — 실을 때 표시를 여기로 옮기고, 성공 회신에 버리고,
    //   실패 회신에 되살린다(save-unify-plan.md 「비행 기록」 · 상태 전이 표).
    struct DbFlight
    {
        uint32_t      nextNo      = 1;       // 다음 묶음 번호(캐릭터마다 1 부터)
        uint32_t      bundleNo    = 0;       // 비행 중 묶음 번호 · 0 = 비행 없음
        bool          again       = false;   // 비행 중에 계기가 와서 판정이 참이었다 = 다시 필요(성공 회신 뒤 틱 끝에 다시)
        bool          listed      = false;   // 이번 틱 끝 목록에 올라 있다(목록 중복 막기)
        bool          posDirty    = false;   // 실을 때의 _dbDirty — 실패면 되살린다
        bool          paired      = false;   // 짝 고리(거래)가 걸려 있다 — 짝 묶음이 확정될 때까지 혼자 묶음을 안 낸다(CCharacterStore 「짝 저장」)
        DbSavedValues savedBefore;           // 실을 때의 _dbSaved(싣기 전 값) — 실패면 되돌린다
#if USE_INVENTORY
        CInventory::SlotBits slots;          // 실은 가방 칸 — 실패면 가방 저장 표시로 되살리고, 나갈 때·종료 묶음엔 표시와 합쳐 싣는다
#endif
    };
    DbFlight _dbFlight;

    // ── 레벨과 경험치 ──
    // 사람만 쌓는다. CCharacter 에 안 올리는 이유 — 몬스터가 레벨을 쓸 자리가 아직 없고,
    //   몬스터가 주는 경험치는 몬스터의 레벨이 아니라 종류별 기획 값이다(Monster.h 의 경계 물음과 같은 결).
    // ★ _exp 는 "지금까지 얻은 총합"이고 표의 필요치도 같은 기준이다 — 레벨 안에서 다시 0부터 안 센다.
    //   그래서 레벨업 판정이 한 줄이고, 한 번에 여러 레벨이 올라도 넘친 경험치가 안 사라진다.
    int32_t _level = 1;
    int64_t _exp   = 0;

#if USE_PARTY
    // ── 파티 ── 0 = 파티 없음. 쓰는 곳은 CPartySystem 뿐이다(가입·빠짐·해산). 저장하지 않는다(접속마다 0 — 끊기면 파티에서 빠진다).
    //   파티원 목록의 진실은 CPartySystem 이고 이것은 사본이다 — 공격 후보 거르기(CombatSystem.cpp)가 매번 파티 목록을 안 뒤지게 하려는 것.
    int32_t _partyId = 0;
#endif

    // C2S_MOVE_START 좌표 수용의 초당 이동량 예산 (치트 방지).
    //   수용은 IDLE 전이 때마다 일어나므로 STOP/START 연타로 무제한 순간이동이 가능했다.
    //   경과 시간만큼 자기 속도에 비례해 적립하고, 수용한 거리만큼 차감한다.
    //   초기값 0 + 프레임 0 = 첫 수용 시 경과가 커서 상한까지 적립됨(스폰 직후 만충과 동일).
    float    _moveBudget = 0.0f;       // 남은 예산 (타일)
    uint64_t _moveBudgetFrame = 0;     // 마지막 적립 프레임 (CGameServer::_frameCount 기준)

#if USE_INVENTORY
    // ── 가방 ──
    // 사람만 갖는다(몬스터가 떨어뜨리는 것은 몬스터의 가방이 아니라 종류별 기획 값이 된다).
    //   칸을 바꾼 자리는 반드시 CItemHandler::FlushInventory 를 불러야 클라·DB 에 나간다.
    //   ★ 맨 끝에 두는 이유 — 칸 61개(가방 60 + 무기 칸, 약 500B)라 앞에 끼우면 이동 쪽 필드들이 캐시 줄 여러 개 뒤로 밀린다.
    //     가방은 명령·물약 때만 본다.
    CInventory _inventory;

    // 아이템을 다시 쓸 수 있는 프레임(CGameServer::_frameCount 기준) — 성공했을 때만 앞으로 민다.
    //   서버가 틱 수로 잰다(클라 연타·매크로로 물약을 한꺼번에 마시지 못하게). 0 = 바로 쓸 수 있다.
    uint64_t _itemUseReadyFrame = 0;

#if USE_EQUIP
    // 다시 끼우거나 뺄 수 있는 프레임 — 물약과 따로 센다(물약 직후에 무기를 바꿔도 안 막히게).
    //   성공했을 때만 앞으로 민다. 연타 한 번이 칸 패킷 둘이라 사람당 초당 상한을 두는 것이다
    //   (DB 는 틱 끝 묶음 하나라 연타가 저장 수를 늘리진 않는다 — 저장 플랜 C 전에는 「DB 두 줄」도 까닭이었다).
    uint64_t _equipReadyFrame = 0;
#endif
#endif

private:
    // 이 플레이어를 조종하는 네트워크 세션. -1 = 아직 없음 — 스폰 경로(CEntryHandler::SpawnPlayer)가
    //   EnterZone → 세션 부여(CSessionHub::AddPlayer) 순이라 EnterZone 시점엔 잠시 -1 이다. SendPacket 이 이 값을 검사한다.
    int64_t _sessionId = -1;
};

#if USE_PARTY
// 캐릭터의 파티 번호 — 사람이면 _partyId, 몬스터면 0(파티가 없다). 공격 후보 거르기(CombatSystem.cpp 후보 함수 둘)가
//   「때린 쪽과 같은 파티인가」를 볼 때 쓴다 — 0 은 「파티 없음」이라 0 끼리는 같은 파티가 아니다(부르는 쪽이 때린 쪽 0 을 먼저 거른다).
inline int32_t PartyIdOf(const CCharacter* c)
{
    const CPlayer* p = As<CPlayer>(c);
    return (p != nullptr) ? p->_partyId : 0;
}
#endif
