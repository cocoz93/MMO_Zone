#pragma once

#include <cstdint>

#include "BuildConfig.h"   // USE_COMBAT — 효과 컴포넌트 자리. 안 읽으면 아래 #if 가 조용히 0 이 된다
#include "Actor.h"

// ==========================================================================
// CCharacter — 화면에 그려지고 섹터에 사는 것
//
// [경계] 여기 두는 것 = 종류 · 표시 글자 · 색 · 몸 반지름 · 컴포넌트 자리. 클라가 그리는 것이면 다 갖는 것.
//        플레이어·몬스터·NPC 가 전부 여기서 갈라진다.
//        위로는 CActor(좌표·섹터·이동만), 아래로는 CPlayer(계정·이동 예산·운영자)가 있다.
//        가르는 물음 하나 — "나무나 포탈에도 말이 되나" → 되면 CActor, 안 되면 여기.
//
// [규칙 셋] 상속은 여기서 끝 · 능력은 컴포넌트 · 행동은 시스템.
//        맞는다(체력 컴포넌트) · 때린다(공격 컴포넌트) · 지속 효과를 받는다(효과 컴포넌트)는 컴포넌트로 붙인다(CharacterComponents.h) —
//        상속으로 능력을 가르면 축이 둘(맞는다 × 때린다)이 될 때 합친 클래스가 생긴다. 능력이 늘면 컴포넌트가 는다.
//        컴포넌트는 파생이 값으로 들고 여기는 가리키기만 한다(아래 컴포넌트 자리). 시스템의 규칙 함수는 컴포넌트를 받고,
//        알림·자리·종류가 필요한 시스템 입구는 CCharacter* 를 받아 입구에서 컴포넌트를 꺼낸다(없으면 단언).
//
// [세션] 조종 세션(_sessionId)은 여기 없고 CPlayer 에 있다. 패킷을 받는 쪽은 플레이어뿐이라
//        보내는 함수(CSessionHub::SendPacket 과 게임 서버·그 모듈들의 SendXxx)는 전부 CPlayer* 만 받는다 — 몬스터에게
//        보내는 코드는 컴파일이 안 된다. 섹터 목록(CCharacter*)에서 받는 쪽을 고를 때는
//        As<CPlayer>(아래 As) 로 확인 후 내려 바꾼다. 코드 전체에서 내려 바꾸는 자리는 그 한 곳뿐이다.
//
// [종류] _kind 로 종류를 가른다 — 정원 카운터(_playerCount), As(각 타입의 IsA), 섹터 목록 고르기(CSectorManager::AddCharacter).
//        파생 생성자가 자기 것을 잡는다 — 여기 기본값은 PLAYER 이고 CMonster 가 MONSTER, CNpc 가 NPC 로 덮는다.
//        종류가 셋이라 종류로 가르는 자리는 switch 로 셋을 다 적는다(StatSystem.cpp RecalcStats) — if/else 면 하나가 나머지를 삼킨다.
//        생성자 뒤에는 안 바뀐다 — As 와 섹터 목록 선택(넣을 때·뺄 때 같은 목록)이 이 전제에 기댄다.
//
// [컨테이너] 존(_characterList)은 vector<CCharacter*> 하나(틱 순회용 — 이동 갱신은 종류가 상관없다).
//        섹터는 목록 셋 — 플레이어(CPlayer*) · 맞을 수 있는 나머지 · 그 밖(CCharacter*), SectorManager.h [목록 셋] 참조.
//        CActor* 가 아닌 이유 — 전투 판정이 체력 컴포넌트를 꺼내야 하는데, 섹터에서 꺼낸 것이 곧바로 CCharacter 여야 캐스팅이 없다.
//        체력 컴포넌트가 없는 것(NPC)은 「그 밖」 목록에 들어가 전투 후보에서 빠진다.
//
// [가상 함수] 소멸자 말고는 두지 않는다 — CActor 의 원칙 그대로. 종류별 동작이 필요해지면 그때 정한다.
// ==========================================================================

// 스탯 기본값은 코드에 두지 않는다(10-06, 아키텍처 전수검사 6번 — 전에는 CHARACTER_DEFAULT_* 상수가 표의 1레벨 칸과 두 벌이었다).
//   플레이어는 레벨 표(LevelTable.csv)가 정본이고 CStatSystem::RecalcStats 가 채운다. 몬스터는 파생 생성자가 종류 값을 넣는다.
//   컴포넌트 칸(CharacterComponents.h)의 0 은 「아직 안 정해짐」이다 — 전투를 끈 빌드는 0 그대로 두고, 스냅샷도 0 을 싣는다(PacketBuild.h WriteSnapshot).

// 컴포넌트(CharacterComponents.h) — 여기는 가리키기만 하므로 선언만. 값으로 드는 파생(Player.h · Monster.h)이 정의를 포함한다.
class CHealthComponent;
class CAttackComponent;
#if USE_COMBAT
class CBuffComponent;
#endif
struct NpcDef;   // NpcTable.h — 말 걸기 칸이 가리키는 NPC 표 한 줄(컴포넌트가 아니라 표 줄이라 상태가 없다)

// 캐릭터 종류 — 위 [종류] 참조. 와이어에는 안 실린다(클라는 displayChar 의 부류로 종류를 안다).
enum class CharacterKind : uint8_t
{
    PLAYER  = 0,
    MONSTER = 1,
    NPC     = 2,   // 마을 NPC — 컴포넌트 없음 · 말 걸기 칸(Npc.h)
};

class CCharacter : public CActor
{
public:
    CCharacter();
    ~CCharacter() override;

    // ── 종류 ── 순회에서 종류를 가를 때 쓴다 (정원 카운터, AI 대상 거르기 등)
    CharacterKind _kind;

    // ── 표시 ── 클라가 그리는 데 쓰는 것. CREATE 패킷에 실린다.
    uint8_t _displayChar;   // 표시 문자 (ASCII: A-Z, a-z, 0-9) · 사람이 아니면 부류(Protocol.h DISPLAY_MONSTER · DISPLAY_NPC)
    uint8_t _colorIndex;    // 색상 인덱스 (0-6)
    // 정의 번호 — 몬스터 = monsterId · NPC = npcId · 사람 = 0. 등장 엔트리 끝에 실린다(Protocol.h CreatePlayerEntry::defId).
    //   표시 문자와 같이 와이어 값이라 존 층은 안 넣는다 — 등장 알림을 받는 게임 서버 모듈이 넣는다(CMonsterHandler::OnMonsterSpawn).
    uint16_t _defId = 0;

    // 몸 반지름(타일) — 사거리를 잴 때 중심 거리에서 몸 크기만큼 뺀다. 0 = 점(사람·작은 몬스터).
    //   몬스터는 생성자가 몬스터 표의 bodyRadius 를 복사하고, 사람은 0 으로 둔다. 웹의 그림 크기(MON_META.scale)와는 따로 사는 값이다.
    float _bodyRadius = 0.0f;

    // ── 컴포넌트 자리 ── 능력은 컴포넌트가 든다(CharacterComponents.h). 여기는 가리키기만 한다 — 없으면 null.
    //   컴포넌트는 파생이 값으로 들고 생성자 몸에서 AttachComponents 로 한 번 붙인다. 생성 뒤 안 바뀐다(_kind 와 같은 전제).
    //   꺼내기는 CCharacter* 로 받은 자리 전용 — 사람·몬스터를 다루는 코드는 값 멤버(_health · _attack)를 바로 쓴다.
    CHealthComponent*       Health()       { return _healthComponent; }
    const CHealthComponent* Health() const { return _healthComponent; }
    CAttackComponent*       Attack()       { return _attackComponent; }
    const CAttackComponent* Attack() const { return _attackComponent; }
#if USE_COMBAT
    CBuffComponent*         Buffs()        { return _buffComponent; }
    const CBuffComponent*   Buffs() const  { return _buffComponent; }
#endif

    // ── 말 걸기 칸 ── 「말 건다」는 능력 — 자기 NPC 표 줄을 가리킨다(없으면 null). 사고팔기 판정은 타입이 아니라 이 칸으로 찾는다
    //   (싸우는 NPC 가 생기면 「체력 + 공격 + 말 걸기 칸」). 값 컴포넌트가 아니라 표 줄 포인터인 까닭 — 이름·인사·상점 번호가 다 표에 있고
    //   바뀌는 상태가 없다. 컴포넌트 자리와 같은 규칙(생성자 몸에서 한 번 · 생성 뒤 안 바뀐다).
    const NpcDef* Talk() const { return _talk; }

    // 복사·옮김 금지 — 컴포넌트 자리가 자기 멤버를 가리키므로 복사본은 옛 물체의 컴포넌트를 가리키게 된다.
    //   물체는 unique_ptr 로 만들어진 자리에서 안 움직인다(CSessionHub · CMonsterManager) — 그 성질을 타입이 지킨다.
    CCharacter(const CCharacter&) = delete;
    CCharacter& operator=(const CCharacter&) = delete;

protected:
    // 파생 생성자 몸에서 한 번 — 몸이 돌 때는 파생의 멤버(컴포넌트)가 이미 만들어져 있다.
#if USE_COMBAT
    void AttachComponents(CHealthComponent* health, CAttackComponent* attack, CBuffComponent* buffs);
#else
    void AttachComponents(CHealthComponent* health, CAttackComponent* attack);
#endif
    // 말 걸기 칸 — 파생(CNpc) 생성자 몸에서 한 번
    void AttachTalk(const NpcDef* def);

private:
    CHealthComponent* _healthComponent = nullptr;
    CAttackComponent* _attackComponent = nullptr;
#if USE_COMBAT
    CBuffComponent*   _buffComponent   = nullptr;
#endif
    const NpcDef* _talk      = nullptr;
};

// 내려 바꾸는 자리는 코드 전체에서 이 함수 하나뿐이다 — 다른 곳의 static_cast<CPlayer*>·<CMonster*> 는 잘못이다.
//   종류 판정은 각 타입의 IsA 가 한다. 상속이 한 단계 늘면(보스 : CMonster) 그 IsA 만 범위로 바꾼다.
template <class T>
T* As(CCharacter* c)
{
    return T::IsA(*c) ? static_cast<T*>(c) : nullptr;
}

// 읽기만 하는 자리용 — 같은 판정의 const 겹함수(10-17 파티 번호 읽기 PartyIdOf 가 처음 씀). 내려 바꾸는 함수는 여전히 이 이름 하나다.
template <class T>
const T* As(const CCharacter* c)
{
    return T::IsA(*c) ? static_cast<const T*>(c) : nullptr;
}
