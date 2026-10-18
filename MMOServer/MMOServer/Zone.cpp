#include "Zone.h"
#include "Character.h"
#include "Monster.h"   // USE_MONSTER — 벽에 막힌 캐릭터를 CMonster 로 내려 관리자에 넘긴다(헤더 안에서 스스로 토글을 본다)
#include <algorithm>
#include <cassert>
#include <cmath>       // std::isfinite — IsInsideMap
#include <random>

CZone::CZone(const CGameDataFiles& gameData)
    : _zoneId(-1)
    , _mapId(-1)
    , _mapWidth(0)
    , _mapHeight(0)
    , _playerCount(0)
#if USE_MONSTER
    , _monsterManager(*this, gameData)
#endif
{
#if !USE_MONSTER
    (void)gameData;
#endif
}

CZone::~CZone()
{
    // 플레이어의 생명주기는 CGameServer가 관리 — 몬스터는 _monsterManager 가 멤버 소멸 때 해제한다
    _characterList.clear();
}

bool CZone::Init(int32_t zoneId, int32_t mapId, int32_t mapWidth, int32_t mapHeight, int32_t sectorSize, const StartArea& start)
{
    if (!_sectorManager.Init(mapWidth, mapHeight, sectorSize))
        return false;
#if USE_DROP
    // 땅 아이템 칸 — 섹터와 같은 칸 번호(CellIndex)를 쓰므로 섹터를 세운 직후 부른다(칸 수는 섹터 관리자에서 읽는다)
    _groundItemManager.Init();
#endif

    _zoneId = zoneId;
    _mapId = mapId;
    _mapWidth = mapWidth;
    _mapHeight = mapHeight;
    _start = start;   // 맵 안인지는 맵 표가 읽을 때 봤다(CMapTable::Load ⑷)
#if USE_MONSTER
    // 몬스터 구역 상태(점마다 차 있나·다음 리젠) — 맵 번호를 안 뒤라야 그 맵의 스폰 구역을 읽는다. 첫 틱이 다 깐다(처음 깔기)
    _monsterManager.Init();
#endif
    return true;
}

void CZone::EnterZone(CCharacter* player)
{
    // 무작위 스폰 좌표를 뽑아 넘긴다 — 입장 절차 자체는 EnterZoneAt 하나뿐이다.
    float x = 0.0f;
    float y = 0.0f;
    CalcSpawnPos(x, y);
    EnterZoneAt(player, x, y);
}

void CZone::EnterZoneAt(CCharacter* player, float x, float y)
{
    // 빈 포인터는 부르는 쪽 잘못이다 — 셋 다(접속·존 이동·몬스터 스폰) 방금 만들었거나 들고 있는 객체를 넘긴다
    assert(player != nullptr);

    // 존 설정 (playerId 는 부르는 쪽이 부여 완료 상태. 표시 속성은 게임 서버가 넣는다 — 플레이어는 입장 전,
    //   몬스터는 입장 직후 스폰 콜백(OnMonsterSpawn)에서. 존은 표시 속성을 안 본다)
    player->_zoneId = _zoneId;
    player->_direction = Direction::DOWN;
    player->_moveState = MoveState::IDLE;

    // 스폰 좌표 설정 (맵의 들어오는 자리든 DB 에서 불러온 자리든 포털 도착 자리든 호출부가 정해서 준다)
    player->_x = x;
    player->_y = y;

    // 섹터 좌표 계산 및 등록
    player->_sectorX = _sectorManager.CalcSectorX(player->_x);
    player->_sectorY = _sectorManager.CalcSectorY(player->_y);
    _sectorManager.AddCharacter(player, player->_sectorX, player->_sectorY);

    // 캐릭터 목록에 추가
    player->_listIndex = static_cast<int32_t>(_characterList.size());
    _characterList.push_back(player);

    // 정원 카운터는 목록에 실제로 넣은 이 자리에서만 올린다 (Zone.h _playerCount)
    if (player->_kind == CharacterKind::PLAYER)
        ++_playerCount;
}

void CZone::LeaveZone(CCharacter* player)
{
    if (player == nullptr)
        return;

    // 섹터 해제
    _sectorManager.RemoveCharacter(player, player->_sectorX, player->_sectorY);

    // 섹터 이동 대기열에서 제거 — 틱 끝 처리(게임 루프 3)가 지워진 캐릭터를 읽지 않게.
    //   부르는 쪽(CGameServer::RemoveFromZone)은 대기열을 다 읽은 뒤에 여기로 온다.
    _sectorChangedSet.erase(player);
    _pendingSectorChanges.erase(
        std::remove_if(_pendingSectorChanges.begin(), _pendingSectorChanges.end(),
            [player](const SectorChangeInfo& c) { return c.character == player; }),
        _pendingSectorChanges.end());

    // 순회용 리스트에서 O(1) 삭제 (swap-and-pop, 인덱스 캐시 활용)
    int32_t idx = player->_listIndex;
    if (idx >= 0 && idx < static_cast<int32_t>(_characterList.size()))
    {
        CCharacter* back = _characterList.back();
        _characterList[idx] = back;
        back->_listIndex = idx;
        _characterList.pop_back();

        // 카운터는 이 블록 안에서만 내린다 — 밖으로 빼면 목록은 그대로인데
        //   카운터만 내려가는 길이 열린다(같은 캐릭터로 두 번 부르는 경우).
        if (player->_kind == CharacterKind::PLAYER)
            --_playerCount;
    }
    player->_listIndex = -1;
}

void CZone::Tick(float deltaTime, IZoneEvents& events)
{
    for (CCharacter* player : _characterList)
    {
        if (player->_moveState != MoveState::MOVING)
            continue;

        // 방향에 따라 좌표 갱신 (플레이어별 속도) — 부호는 DirectionStep(Actor.h) 한 곳
        float dist = player->_speed * deltaTime;
        const DirectionVec step = DirectionStep(player->_direction);
        player->_x += step.x * dist;
        player->_y += step.y * dist;

        // 맵 경계 클램핑
        bool clamped = ClampToMap(player->_x, player->_y);

        // 경계에 닿으면 정지 → 클라이언트에 MOVE_STOP 통보 필요
        if (clamped)
        {
            player->_moveState = MoveState::IDLE;
            events.OnCharacterMoveStop(this, player);
#if USE_MONSTER
            // 벽에 막힌 몬스터는 다음 틱에 바로 다시 판단하게 한다 — 까닭은 CMonsterManager::OnBlockedByWall
            if (CMonster* monster = As<CMonster>(player))
                _monsterManager.OnBlockedByWall(monster);
#endif
        }

        // 섹터 변경 판정
        UpdateSectorAndQueue(player);
    }
}

// ==========================================================================
// 섹터 이동
// ==========================================================================

bool CZone::UpdateSector(CCharacter* character)
{
    const int32_t newSectorX = _sectorManager.CalcSectorX(character->_x);
    const int32_t newSectorY = _sectorManager.CalcSectorY(character->_y);
    if (newSectorX == character->_sectorX && newSectorY == character->_sectorY)
        return false;

    _sectorManager.RemoveCharacter(character, character->_sectorX, character->_sectorY);
    character->_sectorX = newSectorX;
    character->_sectorY = newSectorY;
    _sectorManager.AddCharacter(character, newSectorX, newSectorY);
    return true;
}

void CZone::UpdateSectorAndQueue(CCharacter* character)
{
    const int32_t oldSectorX = character->_sectorX;
    const int32_t oldSectorY = character->_sectorY;
    if (UpdateSector(character))
        PushSectorChange(character, oldSectorX, oldSectorY);
}

void CZone::PushSectorChange(CCharacter* character, int32_t oldSectorX, int32_t oldSectorY)
{
    // 이 존 캐릭터만 — 대기열을 읽는 쪽(CGameServer)이 존 비교 없이 섹터 좌표를 맞대 보는 근거다
    assert(character->_zoneId == _zoneId);

    if (!_sectorChangedSet.insert(character).second)
        return;  // 이미 기록됨 → 최초 출발 섹터 유지

    _pendingSectorChanges.push_back({ character, oldSectorX, oldSectorY });
}

const SectorChangeInfo* CZone::FindSectorChange(CCharacter* character) const
{
    if (_sectorChangedSet.count(character) == 0)
        return nullptr;

    for (const SectorChangeInfo& change : _pendingSectorChanges)
    {
        if (change.character == character)
            return &change;
    }
    return nullptr;
}

void CZone::ClearSectorChanges()
{
    if (_pendingSectorChanges.empty())
        return;

    _pendingSectorChanges.clear();
    _sectorChangedSet.clear();
}

bool CZone::IsReturnedHome(const SectorChangeInfo& change)
{
    return change.oldSectorX == change.character->_sectorX &&
           change.oldSectorY == change.character->_sectorY;
}

// ==========================================================================
// 시야 변화 — 명단만 만든다. 보내기·계수는 CSightBroadcaster(ProcessSectorChange · SendSightPairFixes · BroadcastLeaveZone)
// ==========================================================================

const CZone::SightChanges& CZone::TakeSightChanges()
{
    _sightChanges.moves.clear();
    _sightChanges.pairFixes.clear();

    for (const SectorChangeInfo& change : _pendingSectorChanges)
    {
        // 출발 섹터 == 현재 섹터이면 원위치 복귀 → 브로드캐스트 불필요
        if (IsReturnedHome(change))
            continue;

        SightMove& move = _sightChanges.moves.emplace_back();
        move.mover = change.character;
        _sectorManager.GetSectorDiff(
            change.oldSectorX, change.oldSectorY,
            change.character->_sectorX, change.character->_sectorY,
            move.added, move.addedCount, move.removed, move.removedCount);
    }

    // 쌍 판정이 읽는 것은 대기열과 섹터 좌표뿐이다 — 부르는 쪽이 moves 를 보내도 둘 다 안 바뀌므로
    //   보내기 전에 미리 정해 둬도 보낸 뒤에 정하는 것과 같다.
    CollectPairFixes(_sightChanges.pairFixes);

    // 명단을 다 만들었으니 비운다 — 틱 끝 송신은 대기열을 안 읽는다(명단만 읽는다).
    ClearSectorChanges();
    return _sightChanges;
}

void CZone::CollectLeaveOldOnly(CCharacter* leaver, CSectorManager::SectorPos* outSectors, int32_t& outCount,
                                int32_t& outOldX, int32_t& outOldY) const
{
    outCount = 0;
    outOldX = leaver->_sectorX;
    outOldY = leaver->_sectorY;

    const SectorChangeInfo* const change = FindSectorChange(leaver);
    if (change == nullptr)
        return;

    outOldX = change->oldSectorX;
    outOldY = change->oldSectorY;

    // 이전 섹터 전용 = old 주변에만 있고 현재 주변에는 없는 섹터
    CSectorManager::SectorPos currentOnly[CSectorManager::MAX_AROUND_SECTORS];
    int32_t currentOnlyCount = 0;
    _sectorManager.GetSectorDiff(
        leaver->_sectorX, leaver->_sectorY,       // 현재 섹터 (이미 갱신된 값)
        change->oldSectorX, change->oldSectorY,   // 이전 섹터
        outSectors, outCount,                     // old 전용 섹터
        currentOnly, currentOnlyCount);
}

void CZone::CollectLateMovers(int32_t targetOldX, int32_t targetOldY, int32_t targetCurX, int32_t targetCurY,
                              const CCharacter* exclude, std::vector<CCharacter*>& out) const
{
    out.clear();

    for (const SectorChangeInfo& change : _pendingSectorChanges)
    {
        CCharacter* mover = change.character;
        if (mover == exclude)
            continue;
        if (IsReturnedHome(change))
            continue;   // 원위치 복귀 — 시야 변화 없음

        // 이동 전엔 대상을 보고 있었는데(클라가 들고 있음), 대상의 지금 자리 주변에도 옛 자리 주변에도 안 걸리는 경우만
        if (CSectorManager::IsAdjacent(change.oldSectorX, change.oldSectorY, targetOldX, targetOldY) &&
            !CSectorManager::IsAdjacent(mover->_sectorX, mover->_sectorY, targetCurX, targetCurY) &&
            !CSectorManager::IsAdjacent(mover->_sectorX, mover->_sectorY, targetOldX, targetOldY))
        {
            out.push_back(mover);
        }
    }
}

// ==========================================================================
// 같은 틱 이동자 쌍 보정 — 판정은 여기, 보내기와 계수는 CSightBroadcaster::SendSightPairFixes
//
// [왜 필요한가]
//   섹터 점유는 이동 즉시 갱신되지만(CZone::Tick 의 섹터 변경 판정, RecvMoveStart/Stop) 통보는 틱 끝에 몰아서
//   한다. 이때 ProcessSectorChange는 이탈/진입 섹터의 "틱 끝 시점" 주민을 읽으므로, 상대도 같은
//   틱에 움직였다면 그 조회에 안 걸린다.
//     · 서로 멀어짐 → 상호 DELETE 누락 → 클라에 유령(멈춘 캐릭터)이 남는다
//     · 서로 같은 섹터로 수렴 → 상호 CREATE 누락 → 바로 옆인데 서로 안 보인다
//   4방향으로 한 칸씩 걷다 같은 틱에 섹터를 넘기만 해도 발생한다. 클라에는 타임아웃 제거도
//   전체 재동기화도 없어 스스로 복구되지 않는다.
//
// [무엇을 하는가]
//   A 패스는 B의 "현재" 섹터로, B 패스는 A의 "현재" 섹터로 판정하므로
//     A 패스 발동 = 인접(A이전, B현재),  B 패스 발동 = 인접(A현재, B이전)
//   이다. 정말 필요한 것은 인접(A이전,B이전) → 인접(A현재,B현재)의 변화이므로,
//   "시야가 바뀌었는데 두 패스 다 발동하지 않는" 쌍만 골라 보충한다. 기존 송신 경로는 그대로
//   두므로 중복은 생기지 않는다(보내던 것을 다시 보내지 않음).
//
// [비용] 이동자 수 M에 대해 O(M²) 정수 비교. 실측 1,000명 M≈62 → 10us/틱(프레임의 0.02%),
//   5,000명 M≈310 → 532us/틱(1.3%). 유의미해지면 이전/현재 섹터로 버킷팅해 후보를 줄일 것.
// ==========================================================================

void CZone::CollectPairFixes(std::vector<SightPairFix>& out) const
{
    const std::vector<SectorChangeInfo>& changes = _pendingSectorChanges;
    const size_t count = changes.size();
    if (count < 2)
        return;

    for (size_t i = 0; i < count; ++i)
    {
        CCharacter* a = changes[i].character;
        const int32_t aOldX = changes[i].oldSectorX;
        const int32_t aOldY = changes[i].oldSectorY;
        if (IsReturnedHome(changes[i]))
            continue;   // 원위치 복귀 — 틱 끝 루프의 skip 조건과 동일

        for (size_t j = i + 1; j < count; ++j)
        {
            CCharacter* b = changes[j].character;
            const int32_t bOldX = changes[j].oldSectorX;
            const int32_t bOldY = changes[j].oldSectorY;
            if (IsReturnedHome(changes[j]))
                continue;

            const bool wasVisible = CSectorManager::IsAdjacent(aOldX, aOldY, bOldX, bOldY);
            const bool isVisible  = CSectorManager::IsAdjacent(a->_sectorX, a->_sectorY, b->_sectorX, b->_sectorY);
            if (wasVisible == isVisible)
                continue;   // 시야 변화 없음 → 통보 자체가 불필요

            const bool aPassHits = CSectorManager::IsAdjacent(aOldX, aOldY, b->_sectorX, b->_sectorY);
            const bool bPassHits = CSectorManager::IsAdjacent(a->_sectorX, a->_sectorY, bOldX, bOldY);

            if (!isVisible)
            {
                // 보였다가 안 보임 — 두 패스 다 못 잡았을 때만 보충
                if (!aPassHits && !bPassHits)
                    out.push_back({ a, b, false });
            }
            else
            {
                // 안 보이다가 보임 — 두 패스 다 못 잡았을 때만 보충
                if (aPassHits && bPassHits)
                    out.push_back({ a, b, true });
            }
        }
    }
}

// ==========================================================================
// 맵 경계 — 값 둘(맵 안 · 벽 선)은 Zone.h 「── 맵 경계 ──」 주석
// ==========================================================================

bool CZone::IsInsideMap(float x, float y) const
{
    // 이동 쪽 클램프(ClampToMap — CZone::Tick 이 부른다)와 같은 범위 — 0 <= 좌표 < 맵 크기.
    //   식이 다르면 "입장은 되는데 첫 틱에 클램프되는" 자리가 생긴다.
    //   NaN·무한대를 먼저 거른다 — NaN 은 비교가 전부 거짓이라 범위 검사만으로는 뜻이 안 읽힌다.
    return std::isfinite(x) && std::isfinite(y)
        && x >= 0.0f && x < static_cast<float>(_mapWidth)
        && y >= 0.0f && y < static_cast<float>(_mapHeight);
}

bool CZone::ClampToMap(float& x, float& y) const
{
    bool clamped = false;
    if (x < 0.0f)          { x = 0.0f; clamped = true; }
    if (x >= _mapWidth)     { x = static_cast<float>(_mapWidth) - 1.0f; clamped = true; }
    if (y < 0.0f)          { y = 0.0f; clamped = true; }
    if (y >= _mapHeight)    { y = static_cast<float>(_mapHeight) - 1.0f; clamped = true; }
    return clamped;
}

void CZone::ClampToWallLine(float& x, float& y) const
{
    // ClampToMap 과 식이 다르다 — 벽 선 초과·맵 크기 미만 구간도 벽 선으로 자른다(드랍이 쓰던 식 그대로).
    const float maxX = static_cast<float>(_mapWidth) - 1.0f;
    const float maxY = static_cast<float>(_mapHeight) - 1.0f;
    x = x < 0.0f ? 0.0f : (x > maxX ? maxX : x);
    y = y < 0.0f ? 0.0f : (y > maxY ? maxY : y);
}

// 벽 방향 검증 — 경계 위치에서 벽 쪽 이동 차단 (클라이언트 IsBlockedByWall과 동일)
bool CZone::IsBlockedByWall(float x, float y, Direction dir) const
{
    float maxX = static_cast<float>(_mapWidth) - 1.0f;
    float maxY = static_cast<float>(_mapHeight) - 1.0f;

    bool atLeft   = (x <= 0.0f);
    bool atRight  = (x >= maxX);
    bool atTop    = (y <= 0.0f);
    bool atBottom = (y >= maxY);

    // 가는 쪽 성분의 부호로 어느 벽인가를 고른다(부호는 DirectionStep 한 곳). NONE 은 (0, 0) 이라 false.
    //   대각선이면 한 축이라도 벽에 닿으면 막힘(멈춤) — 벽을 따라 미끄러지게 하려면 여기서 가른다.
    const DirectionVec s = DirectionStep(dir);
    return (s.x < 0.0f && atLeft) || (s.x > 0.0f && atRight)
        || (s.y < 0.0f && atTop)  || (s.y > 0.0f && atBottom);
}

void CZone::CalcSpawnPos(float& outX, float& outY) const
{
    thread_local std::mt19937 rng{ std::random_device{}() };

    // 시작 구역이 있으면 그 안 — 지정 자리 없이 들어오는 길(새 캐릭터 · 저장 자리 못 씀 · 다른 맵으로 옮겨 옴)이 다 여기를 지난다
    if (_start.has)
    {
        std::uniform_real_distribution<float> distSX(static_cast<float>(_start.x0), static_cast<float>(_start.x1));
        std::uniform_real_distribution<float> distSY(static_cast<float>(_start.y0), static_cast<float>(_start.y1));
        outX = distSX(rng);
        outY = distSY(rng);
        return;
    }

    // 없으면 맵 전체 랜덤 스폰 (경계 1칸 여유)
    std::uniform_real_distribution<float> distX(1.0f, static_cast<float>(_mapWidth - 1));
    std::uniform_real_distribution<float> distY(1.0f, static_cast<float>(_mapHeight - 1));
    outX = distX(rng);
    outY = distY(rng);
}

