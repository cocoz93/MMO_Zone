// ==========================================================================
// CMapManager — 맵 컨테이너 + 맵/채널 관리
//
// [책임]
//  - 맵 설정 등록 및 맵별 채널(존) 관리
//  - 채널 자동 배정 (인원 초과 시 동적 생성)
//  - 빈 동적 채널 자동 정리
//
// [설계]
//  - 싱글스레드 전제 (게임 루프 스레드에서만 호출)
//  - GameServer가 소유, Zone은 MapManager가 소유
//  - zoneId 인코딩: mapId * 1000 + channelIndex
// ==========================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Zone.h"

// 맵 설정 데이터
struct MapConfig
{
    int32_t mapId;
    int32_t mapWidth;
    int32_t mapHeight;
    int32_t sectorSize;
    int32_t maxPlayersPerChannel;  // 채널당 최대 인원 (소프트 리밋)
};

// ==========================================================================
// CMapInstance — 단일 맵의 채널(존) 관리
//
// [책임]
//  - 맵 내 채널 생성/조회/삭제
//  - channelIndex 단조 증가로 ID 충돌 방지
// ==========================================================================
class CMapInstance
{
public:
    CMapInstance() = default;

    // 맵 설정으로 초기화 + 기본 채널(channelIndex=0) 생성
    bool Init(const MapConfig& config);

    // 여유 있는 채널 반환, 없으면 동적 생성
    CZone* FindOrCreateChannel(bool isAdmin = false);

    // 특정 채널 조회 (없으면 nullptr)
    CZone* FindChannel(int32_t channelIndex);

    // 빈 동적 채널 정리 (channelIndex > 0이고 플레이어 0명)
    void CleanupEmptyChannels();

    // 전체 채널 Tick
    void TickAll(float deltaTime, std::vector<SectorChangeInfo>& outSectorChanges,
                 std::vector<CCharacter*>& outClampedCharacters);

    int32_t GetMaxPlayersPerChannel() const { return _config.maxPlayersPerChannel; }

    // 순회용 콜백
    template<typename Fn>
    void ForEachZone(Fn&& fn)
    {
        for (auto& [channelIndex, zone] : _channels)
            fn(zone.get());
    }

private:
    // 채널 생성 내부 함수
    CZone* CreateChannel(int32_t channelIndex);

    MapConfig _config{};
    std::unordered_map<int32_t, std::unique_ptr<CZone>> _channels;  // channelIndex → CZone
    int32_t _nextChannelIndex = 1;  // 단조 증가 — 충돌 원천 차단 (채널 0은 Init에서 별도 생성)
};

class CMapManager
{
public:
    CMapManager() = default;
    ~CMapManager() = default;

    // 맵 등록 (서버 시작 시) — 기본 채널(channelIndex=0) 자동 생성
    bool RegisterMap(const MapConfig& config);

    // 채널 자동 배정 — 여유 있는 채널 반환, 없으면 동적 생성
    CZone* FindOrCreateChannel(int32_t mapId, bool isAdmin = false);

    // 특정 채널 조회 — 존재하지 않으면 nullptr (자동 생성 안 함)
    CZone* FindChannel(int32_t mapId, int32_t channelIndex);

    // 맵의 채널당 최대 인원 조회 (-1: 맵 없음)
    int32_t GetMaxPlayersPerChannel(int32_t mapId) const;

    // 빈 동적 채널 정리 (channelIndex > 0이고 플레이어 0명)
    void CleanupEmptyChannels();

    // 존 조회 (zoneId로 직접 접근)
    CZone* GetZone(int32_t zoneId);

    // 전체 존 Tick — 섹터 변경 및 경계 클램핑 수집
    void TickAll(float deltaTime, std::vector<SectorChangeInfo>& outSectorChanges,
                 std::vector<CCharacter*>& outClampedCharacters);

    // 현재 맵을 제외한 랜덤 맵 ID 반환 (-1이면 선택 불가)
    int32_t GetRandomMapId(int32_t excludeMapId) const;

    // 전체 존 순회 콜백
    template<typename Fn>
    void ForEachZone(Fn&& fn)
    {
        for (auto& [mapId, mapInstance] : _maps)
            mapInstance.ForEachZone(std::forward<Fn>(fn));
    }

    // zoneId 인코딩/디코딩 헬퍼
    static int32_t MakeZoneId(int32_t mapId, int32_t channelIndex);
    static int32_t GetMapIdFromZoneId(int32_t zoneId);
    static int32_t GetChannelIndexFromZoneId(int32_t zoneId);

private:
    // mapId → CMapInstance
    std::unordered_map<int32_t, CMapInstance> _maps;

    // zoneId → CZone* 플랫 캐시 (GetZone 해시 2회 → 1회 최적화, 채널 생성/삭제 시 갱신)
    std::unordered_map<int32_t, CZone*> _zoneCache;
};
