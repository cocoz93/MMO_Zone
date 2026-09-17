#include "MapManager.h"
#include <Protocol/Protocol.h>
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
        if (isAdmin || baseIt->second->GetPlayerCount() < _config.maxPlayersPerChannel)
            return baseIt->second.get();
    }

    // 나머지 동적 채널 탐색
    for (auto& [channelIndex, zone] : _channels)
    {
        if (channelIndex == 0)
            continue;
        if (isAdmin || zone->GetPlayerCount() < _config.maxPlayersPerChannel)
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

void CMapInstance::CleanupEmptyChannels()
{
    // channelIndex=0 (기본 채널)은 제거하지 않음
    for (auto it = _channels.begin(); it != _channels.end(); )
    {
        if (it->first != 0 && it->second->GetPlayerCount() == 0)
            it = _channels.erase(it);
        else
            ++it;
    }
}

void CMapInstance::TickAll(float deltaTime, std::vector<SectorChangeInfo>& outSectorChanges,
                           std::vector<CPlayer*>& outClampedPlayers)
{
    for (auto& [channelIndex, zone] : _channels)
    {
        zone->Tick(deltaTime, outSectorChanges, outClampedPlayers);
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

    // 섹터 묶음이 좌표를 눈금(uint16)으로 실어 나른다 — 맵이 상한을 넘으면 좌표가 조용히 깨지므로 등록 단계에서 막는다.
    // 채널마다 반복되는 CZone::Init이 아니라 여기서 보는 건, 맵 크기가 채널이 아니라 맵의 속성이기 때문이다.
    if (config.mapWidth > POS_QUANT_MAX_MAP_SIZE || config.mapHeight > POS_QUANT_MAX_MAP_SIZE)
        return false;

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

int32_t CMapManager::GetMaxPlayersPerChannel(int32_t mapId) const
{
    auto it = _maps.find(mapId);
    if (it == _maps.end())
        return -1;
    return it->second.GetMaxPlayersPerChannel();
}

void CMapManager::CleanupEmptyChannels()
{
    for (auto& [mapId, mapInstance] : _maps)
    {
        // 삭제 전 캐시에서 빈 동적 채널 제거
        mapInstance.ForEachZone([&](CZone* zone)
        {
            if (zone->GetZoneId() != MakeZoneId(mapId, 0) && zone->GetPlayerCount() == 0)
                _zoneCache.erase(zone->GetZoneId());
        });

        mapInstance.CleanupEmptyChannels();
    }
}

CZone* CMapManager::GetZone(int32_t zoneId)
{
    auto it = _zoneCache.find(zoneId);
    if (it != _zoneCache.end())
        return it->second;

    return nullptr;
}

void CMapManager::TickAll(float deltaTime, std::vector<SectorChangeInfo>& outSectorChanges,
                           std::vector<CPlayer*>& outClampedPlayers)
{
    for (auto& [mapId, mapInstance] : _maps)
    {
        mapInstance.TickAll(deltaTime, outSectorChanges, outClampedPlayers);
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
