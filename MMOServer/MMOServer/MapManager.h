// ==========================================================================
// CMapManager — 맵 컨테이너 + 맵/채널 관리
//
// [책임]
//  - 맵 설정 등록 및 맵별 채널(존) 관리
//  - 채널 자동 배정 (인원 초과 시 동적 생성)
//  - 존 이동 목적지 고르기 (지정 채널 · 자동 배정 · 무작위 맵) — 거절 사유까지 정하고, 패킷 값으로 바꾸는 건 게임 서버
//  - 빈 동적 채널 자동 정리
//
// [설계]
//  - 싱글스레드 전제 (게임 루프 스레드에서만 호출)
//  - GameServer가 소유, Zone은 MapManager가 소유
//  - zoneId 인코딩: mapId * 1000 + channelIndex
// ==========================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Zone.h"

class CGameDataFiles;   // GameDataFiles.h — 존을 만들 때 건네기만 한다(여기서는 안 읽는다)

// 맵 설정 데이터
struct MapConfig
{
    int32_t mapId;
    int32_t mapWidth;
    int32_t mapHeight;
    int32_t sectorSize;
    int32_t maxPlayersPerChannel;  // 채널당 최대 인원 (소프트 리밋)
};

// 존 이동 목적지 고르기의 거절 사유 — CMapManager::PickZoneChangeTarget
//   사유를 정하는 건 채널 정책(여기), 패킷 값(MSG_S2C_ZONE_CHANGE_FAIL::reason)으로 바꾸는 건 게임 서버다.
enum class ZoneChangeReject : uint8_t
{
    NONE,        // 거절 없음 — zone 이 목적지
    NOT_FOUND,   // 맵·채널이 없음 (무작위 맵 후보가 없는 경우 포함)
    FULL,        // 지정한 채널이 정원 초과 (운영자는 안 걸림)
    SAME_ZONE,   // 지금 있는 존과 같음
};

struct ZoneChangeTarget
{
    CZone* zone = nullptr;                              // reject == NONE 일 때만 유효
    ZoneChangeReject reject = ZoneChangeReject::NONE;
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
    //   gameData 는 이 맵의 존마다 건넨다 — 뜻은 CMapManager 생성자 주석.
    bool Init(const MapConfig& config, const CGameDataFiles* gameData);

    // 여유 있는 채널 반환, 없으면 동적 생성
    CZone* FindOrCreateChannel(bool isAdmin = false);

    // 특정 채널 조회 (없으면 nullptr)
    CZone* FindChannel(int32_t channelIndex);

    // 정원 규칙 — 운영자거나 인원 < 정원이면 자리가 있다. 자동 배정과 지정 채널 이동이 이것 하나로 본다
    bool HasRoom(const CZone* zone, bool isAdmin) const;

    // 빈 동적 채널 정리 (channelIndex > 0이고 플레이어 0명)
    //   onBeforeErase: 존을 실제로 지우기 직전에 부른다(비어 있으면 안 부른다).
    //     CGameServer 가 여기서 그 존의 땅 아이템 관리자(ForgetAll)를 불러 센다(알릴 사람이 없어 세기만). 몬스터는 넘길 일이 없다 —
    //     존의 몬스터 관리자가 몬스터를 들고 존과 같이 해제된다. "후보를 돌려주고 밖에서 지운다"로
    //     안 하는 이유는 그러면 "빈 채널인가/기본 채널인가" 판정이 호출자 쪽에도 한 벌 더 생기기 때문이다.
    //     맵 관리자의 조회표(_zoneCache) 빼기도 같은 까닭으로 이 콜백을 거친다 — 판정은 여기 하나뿐이어야 한다.
    //   ★ 이 인자는 USE_MONSTER 밖에 둔다 — MapManager 가 빌드 토글을 알 이유가 없다.
    //     끄면 CGameServer 가 빈 콜백을 넘긴다.
    void CleanupEmptyChannels(const std::function<void(CZone*)>& onBeforeErase);

    // 전체 채널 Tick — 섹터 변경은 존마다 대기열에 쌓인다(CZone::GetSectorChanges)
    void TickAll(float deltaTime, IZoneEvents& events);

    const MapConfig& GetConfig() const { return _config; }

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
    const CGameDataFiles* _gameData = nullptr;  // 존을 만들 때 건넨다 (CMapManager 가 Init 으로 준다)
    std::unordered_map<int32_t, std::unique_ptr<CZone>> _channels;  // channelIndex → CZone
    int32_t _nextChannelIndex = 1;  // 단조 증가 — 충돌 원천 차단 (채널 0은 Init에서 별도 생성)
};

class CMapManager
{
public:
    // gameData — 기획 데이터(기동 때 읽고 안 바뀐다). 존을 만들 때마다 건네, 존 쪽(몬스터 관리자)이 몬스터·스폰 표를
    //   쥐고 언제든 꺼내 보게 한다. 여기서는 읽지 않고 건네기만 한다 — 그래서 빌드 토글을 모르고(CMapInstance::CleanupEmptyChannels 의 ★ 과 같은 규칙),
    //   기획 데이터가 없는 빌드(전투·몬스터 다 끔)의 nullptr 도 그대로 넘긴다. 그 빌드엔 받는 쪽(몬스터 관리자)도 없다.
    explicit CMapManager(const CGameDataFiles* gameData) : _gameData(gameData) {}
    ~CMapManager() = default;

    // 맵 등록 (서버 시작 시) — 기본 채널(channelIndex=0) 자동 생성
    bool RegisterMap(const MapConfig& config);

    // 채널 자동 배정 — 여유 있는 채널 반환, 없으면 동적 생성
    CZone* FindOrCreateChannel(int32_t mapId, bool isAdmin = false);

    // 특정 채널 조회 — 존재하지 않으면 nullptr (자동 생성 안 함)
    CZone* FindChannel(int32_t mapId, int32_t channelIndex);

    // 존 이동 목적지 고르기 — 지금 존에서 받은 요청(맵·채널)으로 목적지 존 또는 거절 사유를 돌려준다
    //   targetChannelIndex >= 0 : 지금 맵의 그 채널 (targetMapId 는 안 본다) — 없음 · 같은 존 · 정원 순으로 거절
    //   targetChannelIndex <  0 : 자동 배정 — targetMapId == -1 이면 지금 맵을 뺀 무작위 맵. 없음 · 같은 존 순으로 거절
    //   자동 배정은 채널을 새로 만들 수 있다(FindOrCreateChannel)
    ZoneChangeTarget PickZoneChangeTarget(int32_t currentZoneId, int32_t targetMapId, int32_t targetChannelIndex, bool isAdmin);

    // 빈 동적 채널 정리 (channelIndex > 0이고 플레이어 0명) — 판정은 CMapInstance 한 곳.
    //   여기는 지우기 직전 알림을 가로채 조회표(_zoneCache)에서 빼고 onBeforeErase 를 이어 부른다 — 뜻은 CMapInstance 주석 참조.
    void CleanupEmptyChannels(const std::function<void(CZone*)>& onBeforeErase);

    // 존 조회 (zoneId로 직접 접근)
    CZone* GetZone(int32_t zoneId);

    // 전체 존 Tick — 섹터 변경은 존마다 대기열에 쌓인다(CZone::GetSectorChanges). 걷기·멈춤 알림은 events 로 건네기만
    void TickAll(float deltaTime, IZoneEvents& events);

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
    const CGameDataFiles* _gameData;   // 생성자 주석 — 맵마다 Init 으로 건넨다

    // mapId → CMapInstance
    std::unordered_map<int32_t, CMapInstance> _maps;

    // zoneId → CZone* 플랫 캐시 (GetZone 해시 2회 → 1회 최적화, 채널 생성/삭제 시 갱신)
    std::unordered_map<int32_t, CZone*> _zoneCache;
};
