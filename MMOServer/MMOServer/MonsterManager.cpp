#include "MonsterManager.h"
#if USE_MONSTER
#include "Zone.h"            // 나를 든 존 — 입장·섹터·맵 번호
#include "SectorManager.h"   // 시야·반경이 걸치는 섹터 훑기
#include "Monster.h"         // unique_ptr<CMonster> 를 지우려면 완전한 타입이 필요하다
#include "MonsterTable.h"    // MonsterDef — 깔 때 몬스터 표의 행을 넘긴다
#include "MonsterAI.h"       // 판단 (Combat 과 같은 자리의 순수 판정 모듈)
#include "Player.h"          // GetSectorPlayers 가 CPlayer* 를 돌려준다 — 후보의 생사·좌표를 읽는다
#include "GameMonitor.h"     // 판단·전이·깐 수 계수기
#include "GameDataFiles.h"   // 쥔 기획 데이터에서 스폰 구역 표를 꺼낸다
#if USE_COMBAT
#include "CombatSystem.h"    // 공격 흐름 — 몬스터가 물 때 플레이어와 같은 것을 부른다
#endif
#if USE_SKILL
#include "SkillSystem.h"     // 스킬 쓰기 — 몬스터가 스킬을 쓸 때 플레이어와 같은 것을 부른다(규칙 목록은 몬스터 스킬 표 · GameDataFiles.h)
#endif
#include <Common/ErrorLog.h> // 스폰 로그 한 줄
#include <algorithm>         // find_if — Despawn
#include <cassert>
#include <random>            // 점의 범위 안 자리 뽑기
#include <string>

CMonsterManager::CMonsterManager(CZone& zone, const CGameDataFiles& gameData)
    : _zone(zone)
    , _gameData(gameData)
{
}

// 몬스터는 목록이 멤버 소멸 때 해제한다 — 존이 지워질 때(빈 채널 정리·서버 종료) 같이.
//   밖에서 빼 줄 대기열이 없다: 몬스터를 틱 너머로 포인터로 드는 것은 이 목록과 존(캐릭터 목록·섹터)뿐이다.
//   (죽은 몬스터는 이 소멸을 기다리지 않는다 — 죽는 순간 Despawn 이 지운다)
CMonsterManager::~CMonsterManager() = default;

// 존이 생길 때 한 번 — 구역마다 「점 전부 빔 · 다음 리젠 = 첫 틱」. 첫 Tick 이 다 깐다(처음 깔기).
//   점의 자리 범위는 스폰 표가 읽을 때 계산해 뒀다(SpawnRegion::spots) — 여기서는 차 있나만 세운다.
//   처음 깔기를 따로 두지 않는 이유 — 처음과 다시 채우기가 같은 길이면 「처음엔 깔렸는데 다시는 안 찬다」가 없다.
//   존 생성 순간에 바로 깔지 않는 이유 — 깔려면 게임 서버 연락처(번호·등장 알림)와 지금 프레임이 필요한데
//   존을 만드는 곳(MapManager)은 그것을 모른다. 첫 틱까지 기다리는 값은 한 틱(40ms)이다.
void CMonsterManager::Init()
{
    // 이 맵에 구역이 없는 일은 없다 — 기동 때 스폰 표를 읽으며(CSpawnRegionTable::Load) 맵 표의 맵마다 구역 ≥ 1 을 봤다.
    const std::vector<SpawnRegion>& regions = _gameData.SpawnRegions().RegionsOf(_zone.GetMapId());

    _regions.assign(regions.size(), RegionState{});   // 다음 리젠 0 = 첫 틱에 바로
    _capacity = 0;
    for (size_t i = 0; i < regions.size(); ++i)
    {
        _regions[i].taken.assign(static_cast<size_t>(regions[i].count), 0);
        _capacity += regions[i].count;
    }
}

// 게임 루프 1-1) — 리젠 주기가 된 구역을 채우고, 몬스터마다 판단한다.
//   채우기를 판단 앞에 두는 이유 — 막 깐 몬스터 중 첫 판단 시점이 이번 틱인 것(_spawnSeq 로 흩은 0번)도
//   같은 틱에 판단하게(예전 처음 깔기와 같은 순서).
void CMonsterManager::Tick(const MonsterContext& ctx, uint64_t frame)
{
    // 클리어된 방 — 남은 몬스터를 지우고(세운 뒤 첫 Tick 한 번) 그 뒤로는 아무것도 안 한다. 리젠도 안 돌아 되살리기 표시도 여기서 끝난다.
    //   지우기를 판단 순회 앞에 두는 이유 — Despawn 과 같은 약속(순회 중엔 안 지운다)
    if (_cleared)
    {
        if (!_monsters.empty())
            VanishAll(ctx);
        return;
    }

    RegenDue(ctx, frame);

    _thinking = true;   // 이 순회 중엔 Despawn 금지(헤더 ★) — 몬스터 공격이 죽이는 건 플레이어뿐이라 지금은 안 불린다
    for (const auto& monster : _monsters)
        Think(ctx, monster.get(), frame);
    _thinking = false;
}

// 죽은 몬스터 지우기 — 존에서는 이미 빠졌다(CGameServer::RemoveFromZone). 객체를 놓고 그 점을 비운다.
void CMonsterManager::Despawn(CMonster* monster)
{
    assert(!_thinking && "판단 순회 중에 몬스터를 지우면 순회가 깨진다 — 헤더 Despawn ★");
    assert(monster->_listIndex == -1 && "존에서 먼저 뺄 것 — CGameServer::RemoveFromZone");

    // [불변식] 구역·점 색인은 깔 때(Spawn) 같은 목록에서 준 것이다
    assert(monster->_spawnRegion >= 0 && monster->_spawnRegion < static_cast<int32_t>(_regions.size()));
    RegionState& st = _regions[static_cast<size_t>(monster->_spawnRegion)];
    assert(monster->_spawnPoint >= 0 && monster->_spawnPoint < static_cast<int32_t>(st.taken.size()));

    auto it = std::find_if(_monsters.begin(), _monsters.end(),
                           [monster](const std::unique_ptr<CMonster>& m) { return m.get() == monster; });
    assert(it != _monsters.end());
    if (it == _monsters.end())
        return;   // 내 몬스터가 아니다 — 점을 비우면 남의 점에 하나 더 깐다

    // 점만 비운다 — 그 구역 다음 리젠 때 다른 빈 점과 함께 찬다(죽은 때를 따로 재지 않는다)
    st.taken[static_cast<size_t>(monster->_spawnPoint)] = 0;

    // 목록에서 빼면 unique_ptr 가 객체를 해제한다 — 순서는 상관없어 맨 뒤와 바꿔 뺀다
    std::swap(*it, _monsters.back());
    _monsters.pop_back();   // ← monster 는 여기서 해제된다
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
#if USE_SKILL
    // 이 몬스터가 쓰는 스킬 규칙 — 몬스터 번호로 꺼내 AI 에 넘긴다(AI 는 표를 모른다). 규칙 없는 몬스터(뱀)는 빈 목록이다.
    //   되살리기 줄은 되살릴 빈 점 수를 같이 넘긴다(AI 는 존·구역을 모른다) — 0 이면 AI 가 그 줄을 건너뛴다. 다른 줄의 칸은 0 그대로 둔다(안 읽는다)
    const MonsterAI::SkillRules& rules = _gameData.MonsterSkills().RulesOf(monster->_def.monsterId);
    MonsterAI::RuleVacancy vacancy{};
    for (size_t i = 0; i < rules.size() && i < vacancy.size(); ++i)
    {
        if (rules[i].reviveMonsterId != 0)
            vacancy[i] = CountVacant(rules[i].reviveMonsterId);
    }
    const MonsterAI::Decision d = MonsterAI::Think(*monster, target, frame, rules, vacancy);
#else
    const MonsterAI::Decision d = MonsterAI::Think(*monster, target, frame);
#endif

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
#if USE_SKILL
    // ── 스킬 ── 한 번의 판단에 행동 하나 — 스킬을 골랐으면 그것이 이번 판단의 행동이고(평타를 대신한다), 아니면 평타.
    //   거절돼도(대상이 방금 비켰다 등) 아무것도 안 바뀐다(UseSkill 의 약속) — 결과는 지표(몬스터 몫 거절)가 센다.
    if (d.skillId != 0)
        ctx.skill.UseSkill(&_zone, monster, d.skillId, frame);
    else if (d.attack)
        ctx.combat.PerformAttack(&_zone, monster, frame);
#else
    if (d.attack)
        ctx.combat.PerformAttack(&_zone, monster, frame);
#endif
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

// 리젠 — 주기가 된 구역마다 빈 점에 하나씩 한꺼번에 깐다. 구역 수만큼 매 틱 비교 하나씩이다.
void CMonsterManager::RegenDue(const MonsterContext& ctx, uint64_t frame)
{
    const std::vector<SpawnRegion>& regions = _gameData.SpawnRegions().RegionsOf(_zone.GetMapId());
    assert(_regions.size() == regions.size() && "Init 이 같은 목록으로 세웠다");

    int32_t spawned = 0;
    for (size_t region = 0; region < _regions.size(); ++region)
    {
        RegionState& st = _regions[region];
        const bool due    = (frame >= st.nextRegenFrame);
        const bool revive = st.reviveDue;   // 되살리기 요청(RequestRevive) — 주기와 따로 빈 점만 채운다
        if (!due && !revive)
            continue;
        st.reviveDue = false;
        // 주기 0 = 한 번만(방 맵 줄 — 스폰 표가 방 맵에만 허용한다). 첫 채우기 뒤 다음 리젠을 끝없이 미룬다 — 잡은 보스가 다시 안 깔린다.
        //   되살리기만으로 온 패스는 주기를 안 민다(주기 리젠 구역이어도 그 박자가 그대로다)
        if (due)
        {
            st.nextRegenFrame = regions[region].regenFrames == 0
                ? UINT64_MAX
                : frame + static_cast<uint64_t>(regions[region].regenFrames);
        }

        // 빈 점마다 하나씩 — 주기 사이에 여럿 죽었으면 여럿이 이 틱에 함께 돌아온다. 되살리기도 같다(= 정확히 죽은 수).
        for (int32_t p = 0; p < regions[region].count; ++p)
        {
            if (st.taken[static_cast<size_t>(p)] == 0)
            {
                Spawn(ctx, static_cast<int32_t>(region), p, frame);
                ++spawned;
                if (revive)
                    ctx.monitor._monster._revivedTotal.Inc();   // 되살리기로 깐 수(깐 수 _spawnedTotal 에도 든다)
            }
        }
    }

    // 존을 통째로 채운 패스(처음 깔기)만 로그 한 줄 — 표를 바꾼 뒤 뜻대로 깔렸는지 본다(시험도 이 줄을 읽는다).
    //   주기마다 몇 마리 다시 채우기는 지표(mmo_monster_spawned_total)로만 센다 — 사냥할 때마다 로그가 쌓이지 않게.
    if (spawned > 0 && spawned == _capacity)
    {
        std::string split;
        for (size_t region = 0; region < regions.size(); ++region)
        {
            if (region > 0)
                split += " / ";
            split += std::to_string(regions[region].count);
        }
        SLOG_INFO("[Monster] zone {} spawned {} (regions {})", _zone.GetZoneId(), spawned, split);
    }
}

void CMonsterManager::Spawn(const MonsterContext& ctx, int32_t regionIndex, int32_t point, uint64_t frame)
{
    const std::vector<SpawnRegion>& regions = _gameData.SpawnRegions().RegionsOf(_zone.GetMapId());
    // [불변식] 색인은 RegenDue 가 Init 과 같은 목록에서 넘긴 것이다
    assert(regionIndex >= 0 && regionIndex < static_cast<int32_t>(regions.size()));

    // [불변식] 몬스터 번호는 기동 때 몬스터 표와 대조했다(CSpawnRegionTable::Load)
    const MonsterDef* def = _gameData.Monsters().Find(regions[static_cast<size_t>(regionIndex)].monsterId);
    assert(def != nullptr);

    auto owned = std::make_unique<CMonster>(*def);  // 능력치는 생성자가 표의 행에서 복사한다
    CMonster* monster = owned.get();               // 빌려 쓰는 포인터 — 소유는 아래에서 목록으로 넘어간다
    monster->_actorId = ctx.events.AllocActorId(); // 단일 증가 카운터라 플레이어와 충돌하지 않는다 — 다시 깔 때마다 새 번호
    monster->_spawnRegion = regionIndex;           // 죽으면 이 구역의
    monster->_spawnPoint  = point;                 //   이 점을 비운다(Despawn)

    // 자리는 그 점의 범위 안 무작위. 방향·정지 상태·섹터 등록·주민 목록 추가는 EnterZoneAt 이 전부 한다.
    float x = 0.0f;
    float y = 0.0f;
    PickSpawnPos(regionIndex, point, x, y);
    _zone.EnterZoneAt(monster, x, y);   // 실패가 없다 — CZone::EnterZone 주석
    _monsters.push_back(std::move(owned));
    _regions[static_cast<size_t>(regionIndex)].taken[static_cast<size_t>(point)] = 1;

    monster->_spawnX = monster->_x;   // 귀환을 넣을 때 쓴다
    monster->_spawnY = monster->_y;

    // 첫 판단 시점을 흩는다 — 다 같은 프레임에 깨어나면 한 존의 몬스터가 한 틱에 몰려
    //   그 틱만 튄다. 판단 주기 안에서 고르게 퍼뜨린다.
    monster->_nextThinkFrame = frame + (_spawnSeq++ % MonsterAI::THINK_INTERVAL_FRAMES);

    ctx.monitor._monster._spawnedTotal.Inc();

    // 주변에 알린다 — 이미 그 자리를 보고 있던 플레이어에게 CREATE(SPAWN) 가 간다.
    //   보는 사람이 없으면 아무에게도 안 가고, 나중에 걸어온 사람은 섹터 진입 때 받는다.
    ctx.events.OnMonsterSpawn(&_zone, monster);
}

// 태어날 자리 — 그 점의 범위 안 균등 무작위. 범위는 스폰 표가 읽을 때 계산해 뒀다(헤더 주석).
void CMonsterManager::PickSpawnPos(int32_t regionIndex, int32_t point, float& outX, float& outY) const
{
    const std::vector<SpawnRegion>& regions = _gameData.SpawnRegions().RegionsOf(_zone.GetMapId());
    // [불변식] 색인은 Spawn 이 받은 것을 그대로 넘긴다(RegenDue — Init 과 같은 목록, 점은 count 개)
    assert(regionIndex >= 0 && regionIndex < static_cast<int32_t>(regions.size()));
    const SpawnRegion& r = regions[static_cast<size_t>(regionIndex)];
    assert(point >= 0 && point < static_cast<int32_t>(r.spots.size()));
    const SpawnSpot& s = r.spots[static_cast<size_t>(point)];

    // 게임 스레드 전용 — 서버의 다른 난수 자리(Zone.cpp CalcSpawnPos · MonsterAI.cpp)와 같은 꼴
    thread_local std::mt19937 rng{ std::random_device{}() };
    outX = std::uniform_real_distribution<float>(s.x0, s.x1)(rng);
    outY = std::uniform_real_distribution<float>(s.y0, s.y1)(rng);
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

// 방 클리어 — 표시만(헤더 ★). 지우기는 다음 Tick 첫머리의 VanishAll.
void CMonsterManager::RequestClear()
{
    _cleared = true;
}

// 되살리기 — 그 몬스터의 구역마다 표시만(헤더 ★). 깔기는 다음 Tick 의 RegenDue(빈 점마다 하나 = 죽은 수).
void CMonsterManager::RequestRevive(int32_t monsterId)
{
    if (_cleared)
        return;   // 보스가 죽은 방 — 지울 몬스터를 다시 깔지 않는다

    const std::vector<SpawnRegion>& regions = _gameData.SpawnRegions().RegionsOf(_zone.GetMapId());
    assert(_regions.size() == regions.size() && "Init 이 같은 목록으로 세웠다");
    for (size_t i = 0; i < regions.size(); ++i)
    {
        if (regions[i].monsterId == monsterId)
            _regions[i].reviveDue = true;
    }
}

// 클리어 — 남은 몬스터를 하나씩 알리고(받는 쪽이 존에서 뺀다 — 주변 DELETE) 목록을 비운다. 점도 비운다(차 있음은 Spawn 이 세우고 지우는 쪽이 내린다).
void CMonsterManager::VanishAll(const MonsterContext& ctx)
{
    assert(!_thinking && "판단 순회 중에 몬스터를 지우면 순회가 깨진다 — 헤더 Despawn ★");

    for (const auto& monster : _monsters)
    {
        ctx.events.OnMonsterVanish(&_zone, monster.get());
        assert(monster->_listIndex == -1 && "받는 쪽이 존에서 뺀다 — CGameServer::RemoveFromZone");
    }
    _monsters.clear();   // ← 여기서 다 해제된다

    for (RegionState& st : _regions)
    {
        std::fill(st.taken.begin(), st.taken.end(), static_cast<uint8_t>(0));
        st.reviveDue = false;
    }
}

#if USE_SKILL
// 이 존에서 그 몬스터의 빈 점 수 — 구역이 몇 안 되고(방 맵 둘) 되살리기 줄이 있는 몬스터만 부른다.
int32_t CMonsterManager::CountVacant(int32_t monsterId) const
{
    const std::vector<SpawnRegion>& regions = _gameData.SpawnRegions().RegionsOf(_zone.GetMapId());
    int32_t vacant = 0;
    for (size_t i = 0; i < regions.size() && i < _regions.size(); ++i)
    {
        if (regions[i].monsterId != monsterId)
            continue;
        for (uint8_t taken : _regions[i].taken)
            vacant += (taken == 0) ? 1 : 0;
    }
    return vacant;
}
#endif

#endif // USE_MONSTER
