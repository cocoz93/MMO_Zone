// ==========================================================================
// MonsterManager — 존 하나의 몬스터 (USE_MONSTER)
//
// [책임]
//  - 몬스터 객체 소유 (unique_ptr 목록) — 존과 같이 태어나고 같이 사라지므로 존이 이 관리자를 값으로 든다
//  - 깔기(스폰 — 구역 주기 리젠) · 판단(AI 적용) · 벽 앞 재판단 · 죽은 몬스터 지우기
//  - 스폰 구역마다 count 마리를 유지한다 — 구역의 리젠 주기(스폰 표 regenSec)가 돌아올 때마다 빈 점마다 하나씩
//    한꺼번에 깐다. 존의 첫 틱이 첫 리젠(처음 깔기)이고, 그 뒤로 주기마다. 죽은 몬스터는 자기 점을 비울 뿐
//    따로 시간을 재지 않는다 — 주기 사이에 여럿 죽으면 다음 리젠에 함께 돌아온다(10-09 사용자 결정).
//  - 자리는 점 — 스폰 표가 읽을 때 구역을 격자로 나눠 점(칸 안쪽 범위)을 계산해 둔다(SpawnRegion::spots).
//    몬스터 한 마리가 점 하나를 맡고, 깔 때는 그 범위에서 난수만 뽑는다. 구역 전체 무작위로 뽑으면 우연히 한쪽에
//    몰리고, 깔 때마다 계산할 이유가 없다(10-09 사용자 결정).
//  - 사람 근처를 피하지 않는다(10-09 사용자 결정) — 격자에선 칸 안에 사람이 서 있으면 다시 뽑아도 거의 다 걸려
//    효과가 없었고, 사람 옆에 깔려도 돌아다니던 뱀이 다가온 것과 같다. 깔 때 섹터를 훑는 일도 없어진다.
//  - 방 클리어(RequestClear — 남은 몬스터를 사라지게)와 되살리기(RequestRevive — 죽은 만큼 다시 깔기, 10-18 뱀왕)는 표시만 세우고
//    다음 Tick 이 한다 — 둘 다 목록을 도는 중(범위 스킬의 대상 버퍼 · 판단 순회)에 불리기 때문이다
//  - 송신·번호 발급은 하지 않는다 — 일이 생기면 IMonsterEvents·IZoneEvents(걷기·멈춤)로 알리고, 받는 쪽(몬스터 조각 CMonsterHandler · 걷기·멈춤은 시야 조각 CSightBroadcaster)이 한다
//
// [왜 떼어냈나]
//  몬스터는 존이 소유하는데 몬스터를 다루는 코드는 게임 서버에 있어, 존을 지우기 전에 게임 서버가
//  자기 대기열에서 몬스터를 빼 줘야 했다. 몬스터를 다루는 코드가 여기로 오면 그 약속이 통째로 없어진다 —
//  몬스터를 틱 너머로 포인터로 드는 것이 전부 몬스터와 같은 객체(존) 안에 있게 된다.
//
// [계층 경계]
//  CZone·CSectorManager·CMonster·MonsterAI 를 안다. 세션·패킷(CSerialBuffer)·CGameServer 는 모른다.
//  기획 데이터(몬스터 표·스폰 구역 표)는 존이 생길 때 받아 쥔다 — 기동 때 읽고 안 바뀌는 읽기 전용이라 언제든 꺼내 본다
//  (맵 관리자는 건네기만 하고 안 읽는다). 게임 서버 것(알림 통로·계수기·공격 흐름)은 부를 때마다
//  MonsterContext 로 받는다 — 존을 만드는 곳이 MapManager 안뿐이라, 만들 때 꽂으면 MapManager 가 그것들을 다 알아야 한다.
// ==========================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "BuildConfig.h"   // USE_MONSTER·USE_COMBAT — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_MONSTER

class CZone;
class IZoneEvents;         // Zone.h — 참조로만 든다
class CCharacter;
class CMonster;            // 전방 선언이면 된다 — unique_ptr 을 지우는 생성자·소멸자 몸체는 .cpp 에 둔다(Zone.h 와 같은 규칙)
class CGameMonitor;
class CGameDataFiles;      // 기획 데이터 — 참조로 쥔다(표를 읽는 .cpp 만 GameDataFiles.h 를 넣는다)
#if USE_COMBAT
class CCombatSystem;
#endif
#if USE_SKILL
class CSkillSystem;
#endif

// 몬스터 쪽이 게임 서버에 부탁하는 것 — 몬스터 조각 CMonsterHandler 가 구현한다(번호 발급기는 세션 허브 · 송신은 시야 조각 — 게임 서버 나누기 10 단계).
//   1단계 ICombatEvents 와 같은 꼴: "패킷을 보내라"가 아니라 "무슨 일이 있었다"로 알리고, 패킷 조립은 받는 쪽이 한다.
//   ★ 순수 가상이라 하나를 빠뜨리면 컴파일이 안 된다.
class IMonsterEvents
{
public:
    virtual ~IMonsterEvents() = default;

    // 번호 하나 — 사건이 아니라 부탁이다. 번호 공간이 플레이어와 하나라서(단일 증가 계수기) 여기서 못 뽑는다.
    virtual int32_t AllocActorId() = 0;

    // 등장 — 지금 자리 주변에 CREATE(SPAWN). 깔 때.
    virtual void OnMonsterSpawn(CZone* zone, CMonster* monster) = 0;

    // 사라짐 — 등장의 짝. 방이 클리어돼 남은 몬스터를 지울 때(VanishAll) 하나마다 온다 — 받는 쪽은 죽은 몬스터를 지우기 앞과 같은 일을 한다
    //   (효과 목록에서 빼기 · 존에서 빼기 = 주변 DELETE — CPlayerPlacement::RemoveFromZone). 이 뒤에 관리자가 객체를 해제한다.
    virtual void OnMonsterVanish(CZone* zone, CMonster* monster) = 0;

    // 걷기 시작·멈춤은 여기 없다 — 존 틱의 벽 멈춤과 같은 IZoneEvents 로 알린다(MonsterContext::zoneEvents).
};

// 부를 때마다 넘기는 묶음 — 게임 서버가 틱마다 만든다(존을 만들 때 꽂지 않는다).
//   몬스터 사양(능력치·마릿수)은 여기 없다 — 표에 있고, 관리자가 쥔 기획 데이터에서 읽는다([계층 경계]).
struct MonsterContext
{
    IMonsterEvents&          events;
    IZoneEvents&             zoneEvents;       // 걷기 시작·멈춤 — 방향·이동 상태가 바뀐 것만 보낸다
    CGameMonitor&            monitor;
#if USE_COMBAT
    CCombatSystem&           combat;           // 공격 흐름 — 플레이어와 같은 것을 쓴다
#endif
#if USE_SKILL
    CSkillSystem&            skill;            // 스킬 쓰기 — 플레이어와 같은 것을 쓴다(시전자는 CCharacter). 고르는 건 MonsterAI, 부르는 건 여기(Think)
#endif
};

class CMonsterManager
{
public:
    // gameData — 기획 데이터. 존이 생길 때 받아 끝까지 쥔다([계층 경계]).
    CMonsterManager(CZone& zone, const CGameDataFiles& gameData);
    ~CMonsterManager();

    CMonsterManager(const CMonsterManager&) = delete;
    CMonsterManager& operator=(const CMonsterManager&) = delete;

    // 존 초기화 때 한 번(CZone::Init) — 구역마다 「점 전부 빔 · 다음 리젠 = 첫 틱」으로 세운다. 첫 틱이 다 깐다(처음 깔기).
    //   생성자에서 안 하는 이유 — 그때는 맵 번호를 아직 모른다(CMapInstance::CreateChannel 이 만든 뒤 Init).
    void Init();

    // 게임 루프 1-1) — 리젠 주기가 된 구역을 채우고, 몬스터마다 판단한다.
    //   좌표 갱신(2) 앞이어야 한다 — AI 가 정한 방향이 같은 틱에 반영돼야 한 틱씩 밀리지 않는다.
    void Tick(const MonsterContext& ctx, uint64_t frame);

    // 죽은 몬스터를 지우고 그 점을 비운다 — 게임 서버가 죽음 처리 끝에 부른다. 빈 점은 그 구역 다음 리젠 때 찬다.
    //   시체는 클라 연출이라 서버는 죽는 순간 지운다(서버가 들고 있으면 나중에 다가온 사람 화면에 다시 나타난다).
    //   ★ 부르기 전에 존에서 빼 둘 것 — 주변 DELETE·대기열 빼기·LeaveZone 은 플레이어 퇴장과 같은 길
    //     (CPlayerPlacement::RemoveFromZone)이다. 여기는 객체(소유)와 점만 다룬다.
    //   ★ 판단 순회(Tick) 중에는 부르지 않는다 — 도는 목록에서 지우면 순회가 깨진다(단언).
    //     지금 몬스터를 죽이는 건 플레이어의 공격·스킬(틱 1 패킷 처리)뿐이다. 한 번에 여럿을 죽이는 범위 스킬(어검술)도
    //     「대상을 다 고른 뒤 피해 적용」으로 짰다(CSkillSystem 의 대상 버퍼 — SelectTargets 로 모은 뒤 ApplyEffect 가 하나씩).
    void Despawn(CMonster* monster);

    // 존 틱이 맵 경계에서 세웠다 → 다음 틱에 바로 다시 판단하게 한다(CZone::Tick 이 부른다).
    void OnBlockedByWall(CMonster* monster);

    // 방 클리어 — 보스가 죽었다(CCombatHandler::OnDeath 의 방 갈래). 표시만 세우고, 남은 몬스터 지우기는 다음 Tick 첫머리(판단 순회 앞)다.
    //   ★ 그 자리에서 지우지 않는 이유 — 죽음 처리는 범위 스킬이 대상 버퍼를 도는 중일 수 있다(CSkillSystem::HitTargets). 어검술 한 번에
    //     보스와 다른 몬스터가 같이 들어 있고 보스가 먼저 죽을 때 여기서 지우면 버퍼의 다음 칸이 해제된 주소다.
    //   그 뒤로 이 존은 리젠·판단을 안 한다 — 되살리기 요청도 버린다. 방은 사람이 다 나가면 빈 채널 정리가 지운다.
    //   다음 Tick 은 같은 프레임의 게임 루프 1-1) 일 수도 있다 — 사람의 공격·스킬(1) 패킷 처리)이 보스를 죽이면 그 뒤에 1-1) 이 온다.
    void RequestClear();

    // 되살리기 요청 — 이 존에서 그 몬스터의 구역마다 「다음 Tick 의 리젠에서 빈 점 채우기」 표시만 세운다(RegenDue 가 깐다 = 정확히 죽은 수).
    //   ★ 보스의 시전은 판단 순회(Think) 안에서 오므로 여기서 깔면 도는 목록에 넣게 된다 — 그래서 표시만. 주기는 안 민다(주기 리젠 구역이어도 그대로).
    //   클리어된 존이면 버린다 — 클리어 「뒤」에 온 요청이다. 보스가 죽기 직전에 세운 표시는 Tick(클리어면 리젠을 안 돈다)과
    //   VanishAll(표시를 내린다)이 막는다(세 겹).
    void RequestRevive(int32_t monsterId);

    // 목록을 밖에 내주는 함수는 두지 않는다 — 몬스터를 도는 자리(판단·정리)가 전부 이 클래스 안이다.

private:
    // 리젠 주기가 된 구역마다 빈 점에 하나씩 깐다. 존을 통째로 채운 때(처음 깔기)만 로그 한 줄을 남긴다.
    void RegenDue(const MonsterContext& ctx, uint64_t frame);

    // 그 구역의 그 점에 몬스터 하나를 깐다 — 위치·방향·섹터 등록은 CZone::EnterZoneAt 이 전부 해 준다(실패가 없다).
    void Spawn(const MonsterContext& ctx, int32_t regionIndex, int32_t point, uint64_t frame);

    // 몬스터 하나의 판단 — 대상 찾기·상태 적용·알림은 여기가, "무엇을 할까"는 MonsterAI 가.
    void Think(const MonsterContext& ctx, CMonster* monster, uint64_t frame);

    // 시야 안에서 가장 가까운 산 플레이어 (없으면 nullptr). 플레이어 목록만 돈다.
    CCharacter* FindAITarget(CMonster* monster) const;

    // 태어날 자리 — 그 점의 범위(스폰 표가 읽을 때 계산해 둔 칸 안쪽) 안 균등 무작위. 난수 두 번이 전부다.
    void PickSpawnPos(int32_t regionIndex, int32_t point, float& outX, float& outY) const;

    // 클리어 — 남은 몬스터를 다 알리고(사라짐) 놓는다. Tick 첫머리에서만(판단 순회 밖 — Despawn 과 같은 약속).
    void VanishAll(const MonsterContext& ctx);

#if USE_SKILL
    // 이 존에서 그 몬스터의 빈 점 수 — 되살리기 줄을 AI 가 고를지의 입력(MonsterAI::Think 의 vacancy).
    int32_t CountVacant(int32_t monsterId) const;
#endif

    CZone& _zone;   // 나를 든 존 — 존이 나보다 오래 산다(멤버)
    const CGameDataFiles& _gameData;   // 기획 데이터 — 게임 서버가 들고 기동 뒤 안 바뀐다. 존보다 오래 산다

    // AI 를 돌릴 대상만 모은 것. 존의 _characterList 와 별개로 둔다.
    //   _characterList 를 안 가르는 이유는 틱의 이동 갱신이 종류를 안 가려서이고,
    //   여기는 "누가 스스로 판단하나"라 종류가 곧 조건이다. 몬스터는 두 목록에 다 들어간다
    //   (_characterList 에 있어야 움직인다). 섹터 쪽은 받는 쪽이 플레이어뿐이라
    //   플레이어·나머지 둘로 갈라져 있다(SectorManager.h [목록 둘]).
    // 소유 — 몬스터가 사라지는 때는 둘이다: 죽는 순간(Despawn)과 존이 사라질 때(멤버 소멸).
    //   원소는 존의 캐릭터 목록·섹터에도 들어 있다(같은 객체, 거기는 빌린 포인터).
    //   ★ 원소 타입이 CCharacter 가 아니라 CMonster 인 이유 — AI 가 _aiState 같은 몬스터 전용 필드를 읽는데,
    //     CCharacter* 로 담으면 순회마다 내려 캐스팅을 하게 되고 그 캐스팅은 "여기엔 몬스터만 들어 있다"는
    //     전제에 기댄다. 타입으로 담으면 컴파일러가 그 전제를 지킨다.
    std::vector<std::unique_ptr<CMonster>> _monsters;

    // 구역마다 — 점마다 차 있나 · 다음 리젠 틱. 색인 = 이 맵 구역 목록(CSpawnRegionTable::RegionsOf)의 색인.
    //   점의 자리 범위는 스폰 표에 있다(SpawnRegion::spots — 같은 점 번호). 여기는 차 있나만 든다.
    //   차 있음은 Spawn 이 세우고 Despawn 이 내린다(점마다라서 「산 수」를 따로 들지 않는다).
    //   다음 리젠은 0(첫 틱에 바로 — 처음 깔기)에서 시작해 리젠할 때마다 「그 틱 + 주기」로 민다.
    //   주기가 0 인 구역(방 맵 — 한 번만)은 처음 깔기 뒤 끝(UINT64_MAX)으로 민다.
    //   틱이 매 프레임 돌아 리젠은 늘 제 틱에 일어나므로 존이 생긴 때부터 일정 간격이 된다(죽은 때를 기준으로 삼지 않는다).
    //   되살리기 표시(reviveDue)는 주기와 따로다 — 세운 다음 Tick 의 RegenDue 가 빈 점을 채우고 내린다(주기는 안 민다).
    struct RegionState
    {
        std::vector<uint8_t> taken;            // 점(count 개)마다 몬스터가 있나
        uint64_t             nextRegenFrame = 0;
        bool                 reviveDue = false;   // 되살리기 요청(RequestRevive) — 다음 RegenDue 가 주기와 상관없이 빈 점을 채운다
    };
    std::vector<RegionState> _regions;

    int32_t  _capacity = 0;    // 이 존의 몬스터 정원 = 구역 count 의 합 — 로그를 남길 패스(존을 통째로 채운 때)를 가린다
    uint32_t _spawnSeq = 0;    // 깐 순번 — 첫 판단 시점을 흩는 데 쓴다
    bool     _thinking = false;   // 판단 순회 중인가 — 그동안 Despawn 이 불리면 단언이 잡는다
    bool     _cleared  = false;   // 방이 클리어됐나(RequestClear) — 세운 뒤 첫 Tick 이 남은 몬스터를 지우고, 그 뒤로 리젠·판단을 안 한다
};

#endif // USE_MONSTER
