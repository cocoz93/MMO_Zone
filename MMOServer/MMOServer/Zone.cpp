#include "Zone.h"
#include "Character.h"
#include "Monster.h"   // USE_MONSTER — 벽에 막힌 캐릭터를 CMonster 로 내려 관리자에 넘긴다(헤더 안에서 스스로 토글을 본다)
#include <algorithm>
#include <cassert>
#include <cmath>       // std::isfinite — IsInsideMap
#include <random>

CZone::CZone()
    : _zoneId(-1)
    , _mapId(-1)
    , _mapWidth(0)
    , _mapHeight(0)
    , _playerCount(0)
{
}

CZone::~CZone()
{
    // 플레이어의 생명주기는 CGameServer가 관리 — 몬스터는 _monsterManager 가 멤버 소멸 때 해제한다
    _characterList.clear();
}

bool CZone::Init(int32_t zoneId, int32_t mapId, int32_t mapWidth, int32_t mapHeight, int32_t sectorSize)
{
    if (!_sectorManager.Init(mapWidth, mapHeight, sectorSize))
        return false;
#if USE_DROP
    // 땅 아이템 칸 — 섹터와 같은 칸 번호를 쓰므로 섹터를 세운 직후 그 칸 수로 맞춘다
    _groundItemManager.Init(_sectorManager.GetSectorCountX(), _sectorManager.GetSectorCountY());
#endif

    _zoneId = zoneId;
    _mapId = mapId;
    _mapWidth = mapWidth;
    _mapHeight = mapHeight;
    return true;
}

bool CZone::EnterZone(CCharacter* player)
{
    if (player == nullptr)
        return false;

    // 무작위 스폰 좌표를 뽑아 넘긴다 — 입장 절차 자체는 EnterZoneAt 하나뿐이다.
    float x = 0.0f;
    float y = 0.0f;
    CalcSpawnPos(x, y);
    return EnterZoneAt(player, x, y);
}

bool CZone::EnterZoneAt(CCharacter* player, float x, float y)
{
    if (player == nullptr)
        return false;

    // 존 설정 (playerId, 표시 속성은 부르는 쪽 — 플레이어는 CGameServer, 몬스터는 CMonsterManager — 이 부여 완료 상태)
    player->_zoneId = _zoneId;
    player->_direction = Direction::DOWN;
    player->_moveState = MoveState::IDLE;

    // 스폰 좌표 설정 (무작위 자리든 DB 에서 불러온 자리든 호출부가 정해서 준다)
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

    return true;
}

void CZone::LeaveZone(CCharacter* player)
{
    if (player == nullptr)
        return;

    // 섹터 해제
    _sectorManager.RemoveCharacter(player, player->_sectorX, player->_sectorY);

    // 섹터 이동 대기열에서 제거 — 틱 끝 처리(게임 루프 3)가 지워진 캐릭터를 읽지 않게.
    //   부르는 쪽(CGameServer::BroadcastLeaveZone)은 대기열을 다 읽은 뒤에 여기로 온다.
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

void CZone::Tick(float deltaTime, std::vector<CCharacter*>& outClampedCharacters)
{
    for (CCharacter* player : _characterList)
    {
        if (player->_moveState != MoveState::MOVING)
            continue;

        // 방향에 따라 좌표 갱신 (플레이어별 속도)
        float dist = player->_speed * deltaTime;
        switch (player->_direction)
        {
        case Direction::UP:    player->_y -= dist; break;
        case Direction::DOWN:  player->_y += dist; break;
        case Direction::LEFT:  player->_x -= dist; break;
        case Direction::RIGHT: player->_x += dist; break;
        default: break;
        }

        // 맵 경계 클램핑
        bool clamped = ClampToMap(player->_x, player->_y);

        // 경계에 닿으면 정지 → 클라이언트에 MOVE_STOP 통보 필요
        if (clamped)
        {
            player->_moveState = MoveState::IDLE;
            outClampedCharacters.push_back(player);
#if USE_MONSTER
            // 벽에 막힌 몬스터는 다음 틱에 바로 다시 판단하게 한다 — 까닭은 CMonsterManager::OnBlockedByWall
            if (player->_kind == CharacterKind::MONSTER)
                _monsterManager.OnBlockedByWall(static_cast<CMonster*>(player));
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

    switch (dir)
    {
    case Direction::LEFT:  return atLeft;
    case Direction::RIGHT: return atRight;
    case Direction::UP:    return atTop;
    case Direction::DOWN:  return atBottom;
    default: return false;
    }
}

void CZone::CalcSpawnPos(float& outX, float& outY) const
{
    // 맵 전체 랜덤 스폰 (경계 1칸 여유)
    thread_local std::mt19937 rng{ std::random_device{}() };
    std::uniform_real_distribution<float> distX(1.0f, static_cast<float>(_mapWidth - 1));
    std::uniform_real_distribution<float> distY(1.0f, static_cast<float>(_mapHeight - 1));
    outX = distX(rng);
    outY = distY(rng);
}

