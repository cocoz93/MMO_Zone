#include "MonsterManager.h"
#if USE_MONSTER
#include "Zone.h"            // 나를 든 존 — 입장·섹터·맵 번호
#include "SectorManager.h"   // 시야·반경이 걸치는 섹터 훑기
#include "Monster.h"         // unique_ptr<CMonster> 를 지우려면 완전한 타입이 필요하다
#include "MonsterAI.h"       // 판단 (Combat 과 같은 자리의 순수 판정 모듈)
#include "Player.h"          // GetSectorPlayers 가 CPlayer* 를 돌려준다 — 후보의 생사·좌표를 읽는다
#include "GameMonitor.h"     // 판단·전이·다시 뽑기·부활 계수기
#include "SpawnRegionTable.h"
#if USE_COMBAT
#include "Combat.h"          // IsRespawnDue · Respawn — 플레이어 부활과 같은 판정을 쓴다
#include "CombatSystem.h"    // 공격 흐름 — 몬스터가 물 때 플레이어와 같은 것을 부른다
#endif
#include <Common/ErrorLog.h> // 스폰 로그 한 줄
#include <cassert>
#include <random>            // 스폰 구역 안 자리 뽑기
#include <string>

CMonsterManager::CMonsterManager(CZone& zone)
    : _zone(zone)
{
}

// 몬스터는 목록이 멤버 소멸 때 해제한다 — 존이 지워질 때(빈 채널 정리·서버 종료) 같이.
//   밖에서 빼 줄 대기열이 없다: 몬스터를 틱 너머로 포인터로 드는 것(부활 대기열)이 이 객체 안에 있다.
CMonsterManager::~CMonsterManager() = default;

// 게임 루프 1-1) — 아직 몬스터를 안 깐 존이면 깔고, 몬스터마다 판단한다.
//   "존이 만들어질 때 깐다"가 아니라 루프가 훑는 이유 — 존은 CMapInstance::CreateChannel
//   (private)에서만 생기고 그 호출처가 전부 MapManager 안이다. CGameServer 는
//   FindOrCreateChannel 의 반환값만 받으므로 그게 새 존인지 원래 있던 존인지 알 길이 없다.
//   받는 자리 셋에 "스폰했나"를 흩으면 하나를 빠뜨린 날 몬스터 없는 채널이 생긴다.
//   비용은 틱마다 존 수만큼의 플래그 비교이고, 스폰은 존마다 딱 한 번만 돈다.
void CMonsterManager::Tick(const MonsterContext& ctx, uint64_t frame)
{
    if (!_spawned)
        Spawn(ctx, frame);

    for (const auto& monster : _monsters)
        Think(ctx, monster.get(), frame);
}

// 몬스터 하나의 판단 — 대상 찾기와 알림은 여기가 하고, "무엇을 할까"는 MonsterAI 가 정한다.
void CMonsterManager::Think(const MonsterContext& ctx, CMonster* monster, uint64_t frame)
{
    // 매 틱 돌지 않는다 — 몬스터마다 0.4초에 한 번. 스폰 때 프레임을 흩어 한 틱에 안 몰린다.
    //   게임 루프가 단일 코어라 여기서 아끼는 것이 그대로 동접 여유가 된다.
    if (frame < monster->_nextThinkFrame)
        return;

    ctx.monitor._monster._thinkCalls.Inc();

    CCharacter* target = FindAITarget(monster);
    const MonsterAI::Decision d = MonsterAI::Think(*monster, target, frame);

    // 상태·대상·다음 판단 시점은 그대로 적는다 (와이어에 안 나가는 서버 내부 값)
    if (d.nextState != monster->_aiState)
        ctx.monitor._monster._stateTransitions.Inc();
    monster->_aiState        = d.nextState;
    monster->_targetActorId  = d.targetActorId;
    monster->_nextThinkFrame = d.nextThinkFrame;
    monster->_stateEndFrame  = d.stateEndFrame;   // 배회 기간 — 다음 판단 시점과 따로 들고 간다

    // ── 방향·이동 상태는 바뀐 것만 알린다 ──
    //   안 바뀐 것까지 보내면 뱀 한 마리가 0.4초마다 주변에 패킷을 뿌린다.
    const bool dirChanged  = (d.direction != monster->_direction);
    const bool moveChanged = (d.moveState != monster->_moveState);

    monster->_direction = d.direction;
    monster->_moveState = d.moveState;

    if (moveChanged || (dirChanged && d.moveState == MoveState::MOVING))
    {
        // 걷기 시작(또는 걷는 중에 방향을 틀었다) → MOVE_START 로 새 방향을 알린다.
        //   멈췄다면 MOVE_STOP. 좌표는 이미 서버 것이 권위라 그대로 실린다.
        if (d.moveState == MoveState::MOVING)
            ctx.zoneEvents.OnCharacterMoveStart(&_zone, monster);
        else
            ctx.zoneEvents.OnCharacterMoveStop(&_zone, monster);
    }

    // ── 공격 ──
    //   ★ USE_COMBAT 안에 둔다 — 공격 본체(CCombatSystem::PerformAttack)가 전투 토글 소유다.
    //     전투를 끄면 몬스터는 배회·추적까지만 하고 물지 않는다. 그게 맞는 동작이고,
    //     이 가드를 빼면 USE_COMBAT=0 + USE_MONSTER=1 조합이 컴파일되지 않는다.
#if USE_COMBAT
    if (d.attack)
        ctx.combat.PerformAttack(&_zone, monster, frame);
#endif
}

// AI 가 쫓을 대상 하나를 고른다 — 시야 안에서 가장 가까운 "산 플레이어".
//   ★ 플레이어 목록만 돈다. 섞인 목록에서 종류로 거르던 시절엔 안 거르면 뱀이 옆의 뱀을 쫓아가 물었다.
//     플레이어 목록만 도는 자리가 둘이다 — 여기(누구를 쫓을까)와 CCombatSystem::PerformAttack(실제로 누구를 무나).
//     여기만 그러면 플레이어를 쫓아간 뱀이 정면에 선 다른 뱀을 문다(실측으로 확인).
//     CCombatSystem::PerformAttack 쪽은 "때리는 쪽이 몬스터일 때만" 플레이어 목록이므로 플레이어는 뱀을 그대로 때린다.
CCharacter* CMonsterManager::FindAITarget(CMonster* monster) const
{
    // 시야(6타일)가 걸치는 섹터만 참조로 훑는다 — GetAroundPlayers(3×3, 60×60타일)를
    //   쓰지 않는 이유는 공격 본체와 같다(그건 브로드캐스트용이라 후보를 벡터에 전부 복사한다).
    //   어느 섹터를 도나는 섹터 관리자(ForEachSectorInRange), 그 칸에서 무엇을 보나는 여기.
    const CSectorManager& sectors = _zone.GetSectorManager();

    CCharacter* best = nullptr;
    float bestDistSq = 0.0f;

    sectors.ForEachSectorInRange(monster->_x, monster->_y, MonsterAI::SIGHT_RANGE, [&](int32_t sx, int32_t sy)
    {
        for (CPlayer* candidate : sectors.GetSectorPlayers(sx, sy))   // 플레이어만 — 몬스터끼리 싸우지 않고, 자기 자신도 여기 없다
        {
            if (candidate->_isDead)
                continue;   // 시체는 쫓지 않는다

            float distSq = 0.0f;
            if (!MonsterAI::IsInSight(*monster, *candidate, distSq))
                continue;

            if (best == nullptr || distSq < bestDistSq)
            {
                best = candidate;
                bestDistSq = distSq;
            }
        }
    });

    return best;
}

void CMonsterManager::Spawn(const MonsterContext& ctx, uint64_t frame)
{
    _spawned = true;   // 실패하더라도 매 틱 재시도하지 않게 먼저 세운다

    const int32_t spawnCount = (ctx.monsterPerZone > 0) ? ctx.monsterPerZone : MONSTERS_PER_ZONE;

    // 구역마다 몇 마리 — 총수(INI)를 스폰 구역 표의 비율대로 정수로 딱 나눈다.
    //   이 맵에 구역이 없는 일은 없다 — 기동 때 CheckMaps 가 설정된 맵마다 구역 ≥ 1 을 보고, 아니면 서버를 안 띄웠다.
    std::vector<int32_t> perRegion;
    ctx.spawnRegions.SplitCount(_zone.GetMapId(), spawnCount, perRegion);

    int32_t i = 0;   // 존 안 순번 — 첫 판단 시점을 흩는 데 쓴다
    for (size_t region = 0; region < perRegion.size(); ++region)
    {
        for (int32_t k = 0; k < perRegion[region]; ++k, ++i)
        {
            auto owned = std::make_unique<CMonster>();
            CMonster* monster = owned.get();               // 빌려 쓰는 포인터 — 소유는 아래에서 목록으로 넘어간다
            monster->_actorId = ctx.events.AllocActorId(); // 단일 증가 카운터라 플레이어와 충돌하지 않는다
            if (ctx.snakeSpeed > 0)
                monster->_speed = ctx.snakeSpeed;          // 게임용 속도 — 0 이면 SNAKE_SPEED(15) 그대로

            // 자리는 배정받은 구역 안 무작위(사람 근처면 다시 뽑음). 방향·정지 상태·섹터 등록·주민 목록 추가는
            //   EnterZoneAt 이 전부 한다.
            float x = 0.0f;
            float y = 0.0f;
            PickSpawnPos(ctx, static_cast<int32_t>(region), x, y);
            if (!_zone.EnterZoneAt(monster, x, y))
                continue;   // 입장 실패 — owned 가 여기서 해제한다(섹터·목록 어디에도 안 들어갔다)
            _monsters.push_back(std::move(owned));

            monster->_spawnRegion = static_cast<int32_t>(region);   // 부활할 때 같은 구역 안으로 간다
            monster->_spawnX = monster->_x;   // 귀환을 넣을 때 쓴다
            monster->_spawnY = monster->_y;

            // 첫 판단 시점을 흩는다 — 다 같은 프레임에 깨어나면 한 존의 몬스터가 한 틱에 몰려
            //   그 틱만 튄다. 판단 주기 안에서 고르게 퍼뜨린다.
            monster->_nextThinkFrame = frame + (i % MonsterAI::THINK_INTERVAL_FRAMES);

            // 주변에 알린다 — 이미 그 자리를 보고 있던 플레이어에게 CREATE 가 간다.
            //   보는 사람이 없으면 아무에게도 안 가고, 나중에 걸어온 사람은 섹터 진입 때 받는다.
            ctx.events.OnMonsterSpawn(&_zone, monster);
        }
    }

    // 구역별 마릿수 — 표를 바꾼 뒤 비율이 뜻대로 나뉘었는지 로그 한 줄로 본다(시험도 이 줄을 읽는다)
    std::string split;
    for (size_t region = 0; region < perRegion.size(); ++region)
    {
        if (region > 0)
            split += " / ";
        split += std::to_string(perRegion[region]);
    }
    SLOG_INFO("[Monster] zone {} spawned {} (regions {})", _zone.GetZoneId(), i, split);
}

// 태어날 자리 — 구역 안 균등 무작위. 산 플레이어가 가까우면 다시 뽑는다(헤더 주석).
void CMonsterManager::PickSpawnPos(const MonsterContext& ctx, int32_t regionIndex, float& outX, float& outY) const
{
    static_assert(SPAWN_CLEAR_RANGE == MonsterAI::SIGHT_RANGE,
                  "태어나자마자 누군가를 알아채지 않게 — 비우는 반경은 뱀 시야와 같아야 한다");

    const std::vector<SpawnRegion>& regions = ctx.spawnRegions.RegionsOf(_zone.GetMapId());
    // [불변식] 색인은 Spawn 이 같은 목록에서 준 것이고, 목록은 기동 때 CheckMaps 가 비어 있지 않음을 봤다
    assert(regionIndex >= 0 && regionIndex < static_cast<int32_t>(regions.size()));
    const SpawnRegion& r = regions[static_cast<size_t>(regionIndex)];

    // 게임 스레드 전용 — 서버의 다른 난수 자리(Zone.cpp CalcSpawnPos · MonsterAI.cpp)와 같은 꼴
    thread_local std::mt19937 rng{ std::random_device{}() };
    std::uniform_real_distribution<float> distX(static_cast<float>(r.x0), static_cast<float>(r.x1));
    std::uniform_real_distribution<float> distY(static_cast<float>(r.y0), static_cast<float>(r.y1));

    for (int32_t attempt = 1; ; ++attempt)
    {
        outX = distX(rng);
        outY = distY(rng);
        if (!IsLivePlayerNear(outX, outY, SPAWN_CLEAR_RANGE))
            return;

        if (attempt >= SPAWN_MAX_TRIES)
        {
            ctx.monitor._monster._spawnRerollExhausted.Inc();   // 다 걸렸다 — 마지막 자리를 그대로 쓴다
            return;
        }
        ctx.monitor._monster._spawnRerolls.Inc();
    }
}

// (x, y) 에서 range 안에 산 플레이어가 있나 — 반경이 걸치는 섹터의 플레이어 목록만 참조로 훑는다(복사 0).
//   어느 섹터를 도나는 섹터 관리자(ForEachSectorInRange). 하나 찾으면 남은 칸은 들여다보지 않는다(found).
bool CMonsterManager::IsLivePlayerNear(float x, float y, float range) const
{
    const CSectorManager& sectors = _zone.GetSectorManager();
    const float rangeSq = range * range;

    bool found = false;
    sectors.ForEachSectorInRange(x, y, range, [&](int32_t sx, int32_t sy)
    {
        if (found)
            return;
        for (const CPlayer* p : sectors.GetSectorPlayers(sx, sy))
        {
            if (p->_isDead)
                continue;   // 시체 옆에 태어나는 건 괜찮다 — 알아챌 대상이 아니다
            const float dx = p->_x - x;
            const float dy = p->_y - y;
            if (dx * dx + dy * dy < rangeSq)
            {
                found = true;
                return;
            }
        }
    });
    return found;
}

// 벽에 닿은 몬스터는 다음 틱에 바로 다시 판단하게 한다.
//   AI 는 맵 경계를 모른다(MonsterAI 는 맵 크기를 안 받는다). 그래서 배회 방향이
//   바깥을 향하면 클램프로 멈춘 채 다음 판단(최대 3초)까지 벽에 붙어 있게 된다.
//   실측으로 뱀의 50%가 가장자리 띠에 몰렸다(면적 비율은 36%). 여기서 판단을
//   앞당기면 40ms 만에 새 방향을 고른다 — 4방향 중 대부분이 안쪽이라 곧 벗어난다.
void CMonsterManager::OnBlockedByWall(CMonster* monster)
{
    monster->_nextThinkFrame = 0;
}

#if USE_COMBAT
void CMonsterManager::QueueRespawn(CMonster* monster)
{
    _respawnQueue.push_back(monster);
}

// 부활 대기자 훑기 — 틱마다 1회. 죽은 것만 따로 들고 있어 몬스터 전체를 돌지 않는다.
void CMonsterManager::TickRespawn(const MonsterContext& ctx, uint64_t frame)
{
    // 틱 3-2 — 섹터 변경 대기열을 처리하고 비운 뒤다(게임 루프 3). 까닭은 헤더 주석. 누가 이 호출을 앞으로 옮기면 여기서 걸린다.
    assert(_zone.GetSectorChanges().empty());

    if (_respawnQueue.empty())
        return;

    for (size_t i = 0; i < _respawnQueue.size(); )
    {
        CMonster* dead = _respawnQueue[i];
        assert(dead->_zoneId == _zone.GetZoneId());   // 넣는 쪽(CCombatSystem::PerformAttack)이 맞은 자리의 존 관리자에 넣었다

        if (!Combat::IsRespawnDue(*dead, frame))
        {
            ++i;
            continue;
        }

        Combat::Respawn(*dead);

        // 부활도 "되살아난 쪽"으로 가른다 (죽음과 같은 기준 — GameMonitor.h 참조)
        ctx.monitor._monster._respawnsTotal.Inc();

        // 몬스터는 같은 구역 안 다른 자리로 옮겨 되살아난다 — 제자리 부활이면 잡은 자리에 3초마다 다시 선다.
        RespawnElsewhere(ctx, dead);

        // 순서가 뜻을 갖지 않는 대기열이라 뒤엣것과 바꿔치기로 O(1) 제거.
        //   i를 올리지 않는다 — 방금 당겨온 것을 아직 안 봤다.
        _respawnQueue[i] = _respawnQueue.back();
        _respawnQueue.pop_back();
    }
}

// 몬스터 부활 — 같은 구역 안 다른 자리로 옮긴다. 불리는 자리·금지 사항은 헤더 주석.
void CMonsterManager::RespawnElsewhere(const MonsterContext& ctx, CMonster* monster)
{
    // ⑴ 옛 자리 — 시체가 사라진다. 그 자리를 보던 플레이어에게만 간다
    ctx.events.OnMonsterVanish(&_zone, monster);

    // ⑵ 새 자리 — 같은 구역 안. 섹터 옮기기는 존이 한다(UpdateSector).
    //   섹터 이동 대기열에는 안 넣는다 — 화면은 ⑴·⑶ 이 그 자리에서 고치므로, 넣으면 다음 틱에 한 번 더 나간다
    //   델타 동기화 기준점(_lastSyncX/Y)은 여기서 안 건드린다 — 「클라가 이 자리를 받았다」는 송신 쪽 사실이라
    //   CREATE 를 실제로 보내는 게임 서버(OnMonsterSpawn)가 맞춘다(10-06, 아키텍처 전수검사 3번)
    float x = 0.0f;
    float y = 0.0f;
    PickSpawnPos(ctx, monster->_spawnRegion, x, y);
    monster->_x = x;
    monster->_y = y;
    monster->_spawnX = x;
    monster->_spawnY = y;

    _zone.UpdateSector(monster);

    // ⑶ 새 자리 주변에 등장 — CREATE(SPAWN) 가 가득 찬 체력을 싣는다(ActorSnapshot).
    //   옛·새 자리를 둘 다 보는 사람은 ⑴ 의 DELETE 다음 이 CREATE 를 받는다 — 둘 다 곧바로 보내는 경로라 순서가 안 뒤집힌다.
    ctx.events.OnMonsterSpawn(&_zone, monster);
}
#endif // USE_COMBAT

#endif // USE_MONSTER
