// ==========================================================================
// MonsterManager — 존 하나의 몬스터 (USE_MONSTER)
//
// [책임]
//  - 몬스터 객체 소유 (unique_ptr 목록) — 존과 같이 태어나고 같이 사라지므로 존이 이 관리자를 값으로 든다
//  - 깔기(스폰) · 판단(AI 적용) · 부활 자리 옮기기 · 벽 앞 재판단
//  - 송신·번호 발급은 하지 않는다 — 일이 생기면 IMonsterEvents 로 알리고, 받는 쪽(CGameServer)이 한다
//
// [왜 떼어냈나]
//  몬스터는 존이 소유하는데 몬스터를 다루는 코드는 게임 서버에 있어, 존을 지우기 전에 게임 서버가
//  자기 대기열에서 몬스터를 빼 줘야 했다. 몬스터 부활 대기열까지 여기로 오면 그 약속이 통째로 없어진다 —
//  몬스터를 틱 너머로 포인터로 드는 것이 전부 몬스터와 같은 객체 안에 있게 된다.
//
// [계층 경계]
//  CZone·CSectorManager·CMonster·MonsterAI 를 안다. 세션·패킷(CSerialBuffer)·CGameServer 는 모른다.
//  게임 서버 것(알림 통로·계수기·스폰 구역 표·설정값·공격 흐름)은 부를 때마다 MonsterContext 로 받는다 —
//  존을 만드는 곳이 MapManager 안뿐이라, 만들 때 꽂으면 MapManager 가 그것들을 다 알아야 한다.
// ==========================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "BuildConfig.h"   // USE_MONSTER·USE_COMBAT — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_MONSTER

class CZone;
class CCharacter;
class CMonster;            // 전방 선언이면 된다 — unique_ptr 을 지우는 생성자·소멸자 몸체는 .cpp 에 둔다(Zone.h 와 같은 규칙)
class CGameMonitor;
class CSpawnRegionTable;
#if USE_COMBAT
class CCombatSystem;
#endif

// 몬스터 쪽이 게임 서버에 부탁하는 것 — CGameServer 가 구현한다(세션·송신·번호 계수기가 거기 있다).
//   1단계 ICombatEvents 와 같은 꼴: "패킷을 보내라"가 아니라 "무슨 일이 있었다"로 알리고, 패킷 조립은 받는 쪽이 한다.
//   ★ 순수 가상이라 하나를 빠뜨리면 컴파일이 안 된다.
class IMonsterEvents
{
public:
    virtual ~IMonsterEvents() = default;

    // 번호 하나 — 사건이 아니라 부탁이다. 번호 공간이 플레이어와 하나라서(단일 증가 계수기) 여기서 못 뽑는다.
    virtual int32_t AllocActorId() = 0;

    // 등장 — 지금 자리 주변에 CREATE(SPAWN). 처음 깔 때와 부활해 새 자리에 설 때.
    virtual void OnMonsterSpawn(CZone* zone, CMonster* monster) = 0;

    // 걷기 시작(또는 걷는 중에 방향을 틀었다) · 멈춤 — 방향·이동 상태가 바뀐 것만 온다.
    virtual void OnMonsterMoveStart(CZone* zone, CMonster* monster) = 0;
    virtual void OnMonsterMoveStop(CZone* zone, CMonster* monster) = 0;

    // 사라짐 — 지금 자리 주변에 DELETE. 부활로 다른 자리로 옮기기 직전(시체가 사라진다).
    virtual void OnMonsterVanish(CZone* zone, CMonster* monster) = 0;
};

// 부를 때마다 넘기는 묶음 — 게임 서버가 틱마다 만든다(존을 만들 때 꽂지 않는다).
//   설정값 둘은 Start() 전에 바뀌는 값이라 틱마다 만들면 낡을 일이 없다.
struct MonsterContext
{
    IMonsterEvents&          events;
    CGameMonitor&            monitor;
    const CSpawnRegionTable& spawnRegions;
    int32_t                  monsterPerZone;   // 0 = MONSTERS_PER_ZONE
    int32_t                  snakeSpeed;       // 0 = SNAKE_SPEED(Monster.h) 그대로
#if USE_COMBAT
    CCombatSystem&           combat;           // 공격 흐름 — 플레이어와 같은 것을 쓴다
#endif
};

class CMonsterManager
{
public:
    explicit CMonsterManager(CZone& zone);
    ~CMonsterManager();

    CMonsterManager(const CMonsterManager&) = delete;
    CMonsterManager& operator=(const CMonsterManager&) = delete;

    // 게임 루프 1-1) — 처음이면 깔고, 몬스터마다 판단한다.
    //   좌표 갱신(2) 앞이어야 한다 — AI 가 정한 방향이 같은 틱에 반영돼야 한 틱씩 밀리지 않는다.
    void Tick(const MonsterContext& ctx, uint64_t frame);

#if USE_COMBAT
    // 죽은 몬스터를 부활 대기열에 넣는다 — CCombatSystem 이 죽음을 확정한 자리에서 부른다(존 안의 일, 게임 서버를 안 거친다).
    void QueueRespawn(CMonster* monster);

    // 게임 루프 3-2) — 때가 된 몬스터를 같은 구역 안 다른 자리로 되살린다.
    //   ★ 섹터 변경 대기열이 빈 자리에서만 부른다 — 차 있으면 이번 틱에 섹터를 넘은 사람의 화면과
    //     서버 등록이 어긋난 채라 옛 자리 DELETE·새 자리 CREATE 가 그 사람을 놓친다.
    //     그 대기열은 이 존 것이라(CZone::GetSectorChanges) 단언을 여기 맨 앞에 둔다.
    void TickRespawn(const MonsterContext& ctx, uint64_t frame);
#endif

    // 존 틱이 맵 경계에서 세웠다 → 다음 틱에 바로 다시 판단하게 한다(CZone::Tick 이 부른다).
    void OnBlockedByWall(CMonster* monster);

    // 목록을 밖에 내주는 함수는 두지 않는다 — 몬스터를 도는 자리(판단·정리)가 전부 이 클래스 안이다.

private:
    static constexpr int32_t MONSTERS_PER_ZONE = 30;   // 섹터 36 개에 마리당 1 개꼴
    static constexpr float   SPAWN_CLEAR_RANGE = 6.0f; // = MonsterAI::SIGHT_RANGE — 태어나자마자 누군가를 알아채지 않게(단언이 맞춘다)
    static constexpr int32_t SPAWN_MAX_TRIES   = 5;

    // 존에 몬스터를 깐다 — 위치·방향·섹터 등록은 CZone::EnterZoneAt 이 전부 해 준다.
    void Spawn(const MonsterContext& ctx, uint64_t frame);

    // 몬스터 하나의 판단 — 대상 찾기·상태 적용·알림은 여기가, "무엇을 할까"는 MonsterAI 가.
    void Think(const MonsterContext& ctx, CMonster* monster, uint64_t frame);

    // 시야 안에서 가장 가까운 산 플레이어 (없으면 nullptr). 플레이어 목록만 돈다.
    CCharacter* FindAITarget(CMonster* monster) const;

    // 태어날 자리 — 그 구역 안 무작위. 산 플레이어가 SPAWN_CLEAR_RANGE 안이면 다시 뽑고,
    //   SPAWN_MAX_TRIES 번 다 걸리면 마지막 자리를 그대로 쓴다(사람이 빽빽하면 어디든 걸린다).
    //   처음 깔 때와 부활 때 같이 쓴다 — 첫 사람이 채널을 만든 그 틱에 뱀이 깔리므로 처음에도 사람이 있을 수 있다.
    void PickSpawnPos(const MonsterContext& ctx, int32_t regionIndex, float& outX, float& outY) const;

    // (x, y) 에서 range 안에 산 플레이어가 있나 — 걸치는 섹터의 플레이어 목록만 참조로 훑는다
    bool IsLivePlayerNear(float x, float y, float range) const;

#if USE_COMBAT
    // 몬스터 부활 = 같은 구역 안 다른 자리로 옮기기. 옛 자리 주변에 DELETE → 새 자리 → 새 자리 주변에 CREATE(SPAWN).
    //   ★ 퇴장 경로(LeaveZone)를 타지 않는다 — 섹터만 옮긴다. 시체를 3초 둔 뒤 바로 옮기므로
    //     「섹터에 없는 몬스터」 상태가 생기지 않는다.
    void RespawnElsewhere(const MonsterContext& ctx, CMonster* monster);
#endif

    CZone& _zone;   // 나를 든 존 — 존이 나보다 오래 산다(멤버)

    // AI 를 돌릴 대상만 모은 것. 존의 _characterList 와 별개로 둔다.
    //   _characterList 를 안 가르는 이유는 틱의 이동 갱신이 종류를 안 가려서이고,
    //   여기는 "누가 스스로 판단하나"라 종류가 곧 조건이다. 몬스터는 두 목록에 다 들어간다
    //   (_characterList 에 있어야 움직인다). 섹터 쪽은 받는 쪽이 플레이어뿐이라
    //   플레이어·나머지 둘로 갈라져 있다(SectorManager.h [목록 둘]).
    // 소유 — 몬스터는 존과 수명이 같다(죽어도 지우지 않고 부활로 재사용하므로 사라지는 때는 존이 사라질 때뿐).
    //   원소는 존의 캐릭터 목록·섹터에도 들어 있다(같은 객체, 거기는 빌린 포인터).
    //   ★ 원소 타입이 CCharacter 가 아니라 CMonster 인 이유 — AI 가 _aiState 같은 몬스터 전용 필드를 읽는데,
    //     CCharacter* 로 담으면 순회마다 내려 캐스팅을 하게 되고 그 캐스팅은 "여기엔 몬스터만 들어 있다"는
    //     전제에 기댄다. 타입으로 담으면 컴파일러가 그 전제를 지킨다.
    std::vector<std::unique_ptr<CMonster>> _monsters;

#if USE_COMBAT
    // 죽어서 부활을 기다리는 몬스터 (빌린 포인터). 원소는 위 _monsters 의 것이라 수명이 같다 —
    //   존이 지워질 때 밖에서 빼 줄 일이 없다.
    std::vector<CMonster*> _respawnQueue;
#endif

    // 이 존에 몬스터를 이미 깔았나 — 존 생성 직후에 부를 자리가 없어서(CreateChannel 이 private 이고
    //   호출처가 전부 MapManager 안이다) 게임 루프가 존마다 Tick 을 부르고 처음 한 번만 깐다.
    bool _spawned = false;
};

#endif // USE_MONSTER
