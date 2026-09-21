#include "Zone.h"
#include "Character.h"
#include <algorithm>
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
    // CCharacter의 생명주기는 CGameServer가 관리
    _characterList.clear();
}

bool CZone::Init(int32_t zoneId, int32_t mapId, int32_t mapWidth, int32_t mapHeight, int32_t sectorSize)
{
    if (!_sectorManager.Init(mapWidth, mapHeight, sectorSize))
        return false;

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

    // 존 설정 (playerId, 표시 속성은 CGameServer가 부여 완료 상태)
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

void CZone::Tick(float deltaTime, std::vector<SectorChangeInfo>& outSectorChanges,
                 std::vector<CCharacter*>& outClampedCharacters)
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
        bool clamped = false;
        if (player->_x < 0.0f)          { player->_x = 0.0f; clamped = true; }
        if (player->_x >= _mapWidth)     { player->_x = static_cast<float>(_mapWidth) - 1.0f; clamped = true; }
        if (player->_y < 0.0f)          { player->_y = 0.0f; clamped = true; }
        if (player->_y >= _mapHeight)    { player->_y = static_cast<float>(_mapHeight) - 1.0f; clamped = true; }

        // 경계에 닿으면 정지 → 클라이언트에 MOVE_STOP 통보 필요
        if (clamped)
        {
            player->_moveState = MoveState::IDLE;
            outClampedCharacters.push_back(player);
        }

        // 섹터 변경 판정
        int32_t newSectorX = _sectorManager.CalcSectorX(player->_x);
        int32_t newSectorY = _sectorManager.CalcSectorY(player->_y);

        if (newSectorX != player->_sectorX || newSectorY != player->_sectorY)
        {
            int32_t oldSectorX = player->_sectorX;
            int32_t oldSectorY = player->_sectorY;

            // 섹터 데이터 갱신
            _sectorManager.RemoveCharacter(player, oldSectorX, oldSectorY);
            player->_sectorX = newSectorX;
            player->_sectorY = newSectorY;
            _sectorManager.AddCharacter(player, newSectorX, newSectorY);

            outSectorChanges.push_back({ player, oldSectorX, oldSectorY });
        }
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

