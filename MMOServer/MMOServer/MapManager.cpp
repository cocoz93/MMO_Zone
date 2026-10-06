#include "MapManager.h"
#include <random>

// ==========================================================================
// CMapInstance
// ==========================================================================

bool CMapInstance::Init(const MapConfig& config)
{
    _config = config;

    // 기본 채널(channelIndex=0) 자동 생성
    if (CreateChannel(0) == nullptr)
        return false;

    return true;
}

CZone* CMapInstance::FindOrCreateChannel(bool isAdmin)
{
    // 기본 채널(0) 우선 배정
    auto baseIt = _channels.find(0);
    if (baseIt != _channels.end())
    {
        if (HasRoom(baseIt->second.get(), isAdmin))
            return baseIt->second.get();
    }

    // 나머지 동적 채널 탐색
    for (auto& [channelIndex, zone] : _channels)
    {
        if (channelIndex == 0)
            continue;
        if (HasRoom(zone.get(), isAdmin))
            return zone.get();
    }

    // 모든 채널이 가득 참 — 동적 채널 생성
    int32_t newChannelIndex = _nextChannelIndex++;
    return CreateChannel(newChannelIndex);
}

CZone* CMapInstance::FindChannel(int32_t channelIndex)
{
    auto it = _channels.find(channelIndex);
    if (it == _channels.end())
        return nullptr;
    return it->second.get();
}

bool CMapInstance::HasRoom(const CZone* zone, bool isAdmin) const
{
    return isAdmin || zone->GetPlayerCount() < _config.maxPlayersPerChannel;
}

void CMapInstance::CleanupEmptyChannels(const std::function<void(CZone*)>& onBeforeErase)
{
    // channelIndex=0 (기본 채널)은 제거하지 않음
    for (auto it = _channels.begin(); it != _channels.end(); )
    {
        if (it->first != 0 && it->second->GetPlayerCount() == 0)
        {
            // 지우기 직전 — 밖에서 이 존의 것을 정리할 마지막 기회다(땅의 아이템 세기).
            //   판정(위 if)과 같은 자리에 둬야 둘이 어긋나지 않는다.
            if (onBeforeErase)
                onBeforeErase(it->second.get());

            it = _channels.erase(it);
        }
        else
            ++it;
    }
}

void CMapInstance::TickAll(float deltaTime, std::vector<CCharacter*>& outClampedCharacters)
{
    for (auto& [channelIndex, zone] : _channels)
    {
        zone->Tick(deltaTime, outClampedCharacters);
    }
}

CZone* CMapInstance::CreateChannel(int32_t channelIndex)
{
    // channelIndex 상한 검증 (zoneId = mapId * 1000 + channelIndex)
    if (channelIndex >= 1000)
        return nullptr;

    // 중복 방지
    if (_channels.find(channelIndex) != _channels.end())
        return nullptr;

    int32_t zoneId = CMapManager::MakeZoneId(_config.mapId, channelIndex);

    auto zone = std::make_unique<CZone>();
    if (!zone->Init(zoneId, _config.mapId, _config.mapWidth, _config.mapHeight, _config.sectorSize))
        return nullptr;

    CZone* ptr = zone.get();
    _channels[channelIndex] = std::move(zone);

    return ptr;
}

// ==========================================================================
// CMapManager
// ==========================================================================

bool CMapManager::RegisterMap(const MapConfig& config)
{
    // 설정 유효성 검증
    if (config.maxPlayersPerChannel <= 0)
        return false;

    // ※ 「맵이 패킷 좌표 눈금(uint16)에 담기는 크기인가」는 여기서 안 본다 — 와이어 사정이라 패킷을 아는
    //   게임 서버가 RegisterMap 을 부르기 전에 본다(CGameServer::Init). 맵 관리자는 그 상한을 모른다.

    // 중복 맵 등록 방지
    if (_maps.find(config.mapId) != _maps.end())
        return false;

    CMapInstance& mapInstance = _maps[config.mapId];
    if (!mapInstance.Init(config))
    {
        _maps.erase(config.mapId);
        return false;
    }

    // 기본 채널(channelIndex=0) 캐시 등록
    int32_t zoneId = MakeZoneId(config.mapId, 0);
    CZone* baseZone = mapInstance.FindChannel(0);
    if (baseZone != nullptr)
        _zoneCache[zoneId] = baseZone;

    return true;
}

CZone* CMapManager::FindOrCreateChannel(int32_t mapId, bool isAdmin)
{
    auto it = _maps.find(mapId);
    if (it == _maps.end())
        return nullptr;

    CZone* zone = it->second.FindOrCreateChannel(isAdmin);

    // 새 채널이 생성되었을 수 있으므로 캐시 갱신
    if (zone != nullptr)
        _zoneCache[zone->GetZoneId()] = zone;

    return zone;
}

CZone* CMapManager::FindChannel(int32_t mapId, int32_t channelIndex)
{
    auto it = _maps.find(mapId);
    if (it == _maps.end())
        return nullptr;

    return it->second.FindChannel(channelIndex);
}

ZoneChangeTarget CMapManager::PickZoneChangeTarget(int32_t currentZoneId, int32_t targetMapId, int32_t targetChannelIndex, bool isAdmin)
{
    const int32_t currentMapId = GetMapIdFromZoneId(currentZoneId);

    CZone* zone = nullptr;
    CMapInstance* pickedMap = nullptr;   // 채널 지정일 때만 — 정원은 아래에서 이 맵의 규칙으로 본다

    if (targetChannelIndex >= 0)
    {
        // ── 채널 지정 이동 (같은 맵 내) ──
        auto it = _maps.find(currentMapId);
        if (it != _maps.end())
        {
            pickedMap = &it->second;
            zone = pickedMap->FindChannel(targetChannelIndex);
        }
    }
    else
    {
        // ── 기존 맵 이동 (자동 채널 배정) ──

        // 랜덤 맵 이동 요청 처리
        if (targetMapId == -1)
        {
            targetMapId = GetRandomMapId(currentMapId);
            if (targetMapId == -1)
                return { nullptr, ZoneChangeReject::NOT_FOUND };
        }

        zone = FindOrCreateChannel(targetMapId, isAdmin);
    }

    if (zone == nullptr)
        return { nullptr, ZoneChangeReject::NOT_FOUND };

    // 지금 있는 존을 그대로 고른 경우 — 채널 지정·자동 배정 둘 다 거부.
    //   통과시키면 게임 서버의 LeaveZone→EnterZone 에서 CalcSpawnPos가 좌표를 맵 전역 난수로 재추첨해,
    //   요청만으로 순간이동이 된다(MOVE_START 이동량 예산 우회).
    //   정원보다 먼저 본다 — 가득 찬 지금 채널을 다시 지정하면 「가득 참」이 아니라 「이미 해당 채널」이다.
    if (zone->GetZoneId() == currentZoneId)
        return { nullptr, ZoneChangeReject::SAME_ZONE };

    // 인원 제한 체크 (admin은 스킵) — 자동 배정은 FindOrCreateChannel 이 같은 규칙(HasRoom)으로 이미 골랐다
    if (pickedMap != nullptr && !pickedMap->HasRoom(zone, isAdmin))
        return { nullptr, ZoneChangeReject::FULL };

    return { zone, ZoneChangeReject::NONE };
}

void CMapManager::CleanupEmptyChannels(const std::function<void(CZone*)>& onBeforeErase)
{
    // 「빈 동적 채널인가」 판정은 실제로 지우는 CMapInstance 한 곳뿐이다 — 여기는 지우기 직전 알림을 받아
    //   조회표(_zoneCache)에서 빼고 게임 서버의 콜백을 이어 부른다. 같은 판정을 여기서 한 번 더 돌리지 않는다
    //   (전에는 캐시 빼기용으로 조건을 한 벌 더 들고 있었다 — 10-06, 아키텍처 전수검사 5번).
    //   순서 — 조회표에서 먼저 빼고 콜백: 콜백이 하는 일(땅의 아이템 세기 → 존의 관리자 ForgetAll)은
    //   zone 포인터로만 하고 GetZone 을 안 거치므로 무관하다. 존 객체는 콜백이 끝난 뒤 지워진다.
    for (auto& entry : _maps)
    {
        entry.second.CleanupEmptyChannels([&](CZone* zone)
        {
            _zoneCache.erase(zone->GetZoneId());
            if (onBeforeErase)
                onBeforeErase(zone);
        });
    }
}

CZone* CMapManager::GetZone(int32_t zoneId)
{
    auto it = _zoneCache.find(zoneId);
    if (it != _zoneCache.end())
        return it->second;

    return nullptr;
}

void CMapManager::TickAll(float deltaTime, std::vector<CCharacter*>& outClampedCharacters)
{
    for (auto& [mapId, mapInstance] : _maps)
    {
        mapInstance.TickAll(deltaTime, outClampedCharacters);
    }
}

int32_t CMapManager::GetRandomMapId(int32_t excludeMapId) const
{
    static constexpr int32_t MAX_MAPS = 64;
    int32_t candidates[MAX_MAPS];
    int32_t count = 0;

    for (const auto& [mapId, mapInstance] : _maps)
    {
        if (mapId != excludeMapId && count < MAX_MAPS)
            candidates[count++] = mapId;
    }
    if (count == 0)
        return -1;

    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int32_t> dist(0, count - 1);
    return candidates[dist(rng)];
}

int32_t CMapManager::MakeZoneId(int32_t mapId, int32_t channelIndex)
{
    return mapId * 1000 + channelIndex;
}

int32_t CMapManager::GetMapIdFromZoneId(int32_t zoneId)
{
    return zoneId / 1000;
}

int32_t CMapManager::GetChannelIndexFromZoneId(int32_t zoneId)
{
    return zoneId % 1000;
}
