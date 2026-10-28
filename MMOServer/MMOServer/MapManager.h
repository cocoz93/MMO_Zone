// ==========================================================================
// CMapManager — 맵 컨테이너 + 맵/채널 관리
//
// [책임]
//  - 맵 설정 등록 및 맵별 채널(존) 관리 · 기본 접속 맵(처음 등록한 맵)
//  - 채널 자동 배정 (인원 초과 시 동적 생성)
//  - 존 이동 목적지 고르기 (지정 채널 · 자동 배정 · 무작위 맵) — 거절 사유까지 정하고, 패킷 값으로 바꾸는 건 게임 서버
//  - 빈 방 정리 — 일반 맵 채널은 비어도 안 지운다(쉬기만 — CZone::IsIdle)
//
// [짜임] 맵 종류로 정책 클래스가 둘이다(channel-split-plan.md · 10-27). 공통인 채널 표는 둘이 값으로 든다(상속 아님 · 가상 함수 없음).
//
//   CMapManager
//    ├─ _fieldMaps : mapId → CFieldMap   (마을·사냥터) — 채널 0 · 다 차면 자동 생성 · 단조 번호 · 정원 · 안 지움
//    ├─ _roomMaps  : mapId → CRoomMap    (방 맵 instance) — 빈 번호로 방 만들기 · 비면 지움
//    └─ _zoneCache : zoneId → CZone*
//   CFieldMap · CRoomMap 은 각자 CChannelTable(채널 표 — 담기·찾기·틱·순회, 정책 없음)을 든다.
//
//   「방 맵인가」 = 「_roomMaps 에 있나」. 갈리는 정책은 두 클래스 중 한쪽에만 있고(예전엔 한 클래스가 IsRoomMap() 으로 다섯 곳에서 갈렸다),
//   여기 남은 갈림은 「어느 표를 보나」뿐이다. 호출부(입장·포털·파티·전투)는 이 클래스 함수만 부르므로 둘이 갈린 것을 모른다.
//
// [설계]
//  - 싱글스레드 전제 (게임 루프 스레드에서만 호출)
//  - GameServer가 소유, Zone은 MapManager가 소유(맵 정책 클래스의 채널 표가 unique_ptr 로)
//  - zoneId 인코딩: mapId * CHANNELS_PER_MAP(10000) + channelIndex — 기수는 CMapManager::CHANNELS_PER_MAP 하나
// ==========================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
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
    StartArea start;               // 맵 표의 시작 구역 — 채널(존)마다 그대로 넘긴다(CZone::CalcSpawnPos)
    MapKind kind = MapKind::DUNGEON;   // 맵 표의 종류 — 방 맵(INSTANCE)이면 CRoomMap, 나머지는 CFieldMap 으로 등록된다(CMapManager::RegisterMap)
};

// 존 이동 목적지 고르기의 거절 사유 — CMapManager::PickZoneChangeTarget
//   사유를 정하는 건 채널 정책(여기), 패킷 값(MSG_S2C_ZONE_CHANGE_FAIL::reason)으로 바꾸는 건 게임 서버다.
enum class ZoneChangeReject : uint8_t
{
    NONE,        // 거절 없음 — zone 이 목적지
    NOT_FOUND,   // 맵·채널이 없음 (무작위 맵 후보가 없는 경우 포함)
    FULL,        // 지정한 채널이 정원 초과 (운영자는 안 걸림)
    SAME_ZONE,   // 지금 있는 존과 같음
    NOT_ALLOWED, // 다른 맵 지정(맵 번호·무작위)은 운영자만 — 다른 맵으로 가는 길은 포털이다. 같은 맵 채널 지정은 누구나
    COUNT       // 개수 표시 — 요청 결과 계수기가 이 수만큼 칸을 잡는다(NONE 칸 = 성공, GameMonitor.h ZoneChangeResultName)
};

struct ZoneChangeTarget
{
    CZone* zone = nullptr;                              // reject == NONE 일 때만 유효
    ZoneChangeReject reject = ZoneChangeReject::NONE;
};

// ==========================================================================
// CChannelTable — 한 맵의 채널(존) 표. 담기 · 찾기 · 틱 · 순회 · 지우기만 하고 정책은 없다
//
// [책임]
//  - 채널 만들기(번호 상한 · 중복만 본다) · 찾기 · 지우기(무엇을 지울지는 부르는 쪽 조건) · 전체 틱 · 순회
//  - 몇 번을 쓸지 · 언제 만들지 · 언제 지울지는 든 쪽(CFieldMap · CRoomMap)이 정한다
// ==========================================================================
class CChannelTable
{
public:
    // gameData 는 이 맵의 존마다 건넨다 — 뜻은 CMapManager 생성자 주석.
    explicit CChannelTable(const CGameDataFiles& gameData) : _gameData(gameData) {}

    // 맵 설정 — 든 쪽의 Init 이 채널을 만들기 전에 넣는다. 존마다 크기·섹터·시작 구역을 그대로 넘긴다
    void SetConfig(const MapConfig& config) { _config = config; }
    const MapConfig& GetConfig() const { return _config; }

    // 그 번호로 채널 하나 — 번호가 상한(CMapManager::CHANNELS_PER_MAP) 이상이거나 이미 있거나 존 초기화가 실패하면 nullptr
    CZone* Create(int32_t channelIndex);

    // 특정 채널 조회 (없으면 nullptr)
    CZone* Find(int32_t channelIndex);

    // 조건(번호, 존)이 참인 첫 채널 — 없으면 nullptr. 도는 차례는 표 차례(정해지지 않음)
    template<typename Pred>
    CZone* FindIf(Pred&& pred)
    {
        for (auto& [channelIndex, zone] : _channels)
            if (pred(channelIndex, zone.get()))
                return zone.get();
        return nullptr;
    }

    // 조건(번호, 존)이 참인 채널을 지운다 — 지우기 직전에 onBeforeErase(존). 계약은 CRoomMap::CloseEmptyRooms 주석
    template<typename Pred>
    void EraseIf(Pred&& pred, const std::function<void(CZone*)>& onBeforeErase)
    {
        for (auto it = _channels.begin(); it != _channels.end(); )
        {
            if (pred(it->first, it->second.get()))
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

    // 전체 채널 Tick — 섹터 변경은 존마다 대기열에 쌓인다(CZone::GetSectorChanges). 쉬는 존(사람 0)은 건너뛴다(CZone::IsIdle)
    void TickAll(float deltaTime, IZoneEvents& events);

    // 순회용 콜백
    template<typename Fn>
    void ForEachZone(Fn&& fn)
    {
        for (auto& [channelIndex, zone] : _channels)
            fn(zone.get());
    }

private:
    MapConfig _config{};
    const CGameDataFiles& _gameData;  // 존을 만들 때 건넨다 (CMapManager 가 만들 때 준다)
    std::unordered_map<int32_t, std::unique_ptr<CZone>> _channels;  // channelIndex → CZone
};

// ==========================================================================
// CFieldMap — 일반 맵(마을 town · 사냥터 dungeon)의 채널 정책
//
// [책임]
//  - 기본 채널(0)을 Init 에서 만든다 · 자동 배정(정원이 찬 채널을 건너뛰고, 다 찼으면 새 번호로 만든다)
//  - channelIndex 단조 증가로 ID 충돌 방지
//  - 정원 규칙(IsChannelFull) — 자동 배정과 지정 채널 이동이 이것 하나로 본다
//  - ★ 안 지운다 — 사람이 다 나가도 채널을 그대로 두고 쉬게만 한다(CZone::IsIdle — 걷기·몬스터 판단 건너뜀).
//    채널 수는 몰릴 때 최대 접속 ÷ 정원으로 묶인다 — 자동 배정이 빈 채널도 다시 쓴다(FindOrCreateChannel).
//    10-27 전엔 빈 동적 채널(번호 > 0)도 지웠다(channel-split-plan.md)
// ==========================================================================
class CFieldMap
{
public:
    explicit CFieldMap(const CGameDataFiles& gameData) : _table(gameData) {}

    // 맵 설정으로 초기화 + 기본 채널(channelIndex=0) 생성
    bool Init(const MapConfig& config);

    // 여유 있는 채널 반환, 없으면 동적 생성 — 기본 채널(0)을 먼저 본다
    CZone* FindOrCreateChannel(bool isAdmin = false);

    // 특정 채널 조회 (없으면 nullptr)
    CZone* FindChannel(int32_t channelIndex) { return _table.Find(channelIndex); }

    // 정원 규칙 — 운영자가 아니고 인원 >= 정원이면 가득 찬 것이다(운영자는 항상 false). 자동 배정과 지정 채널 이동이 이것 하나로 본다
    bool IsChannelFull(const CZone* zone, bool isAdmin) const;

    void TickAll(float deltaTime, IZoneEvents& events) { _table.TickAll(deltaTime, events); }
    const MapConfig& GetConfig() const { return _table.GetConfig(); }

    template<typename Fn>
    void ForEachZone(Fn&& fn) { _table.ForEachZone(std::forward<Fn>(fn)); }

private:
    CChannelTable _table;
    int32_t _nextChannelIndex = 1;  // 단조 증가 — 충돌 원천 차단 (채널 0은 Init에서 별도 생성). 지우지 않으므로 빈 번호가 생기지 않는다
};

// ==========================================================================
// CRoomMap — 방 맵(MapKind::INSTANCE)의 채널 정책. 채널 하나가 파티 하나의 방이다(room-plan.md)
//
// [일반 맵과 다른 점]
//  - 채널 0 을 안 만든다 — 만들면 누구나 들어가는 공용 방이 된다
//  - 자동 배정이 없다 — 방은 CreateRoom 으로만 생긴다(파티장이 포털을 쓸 때, 게임 서버가 판정한 뒤).
//    CMapManager::FindOrCreateChannel 이 방 맵 번호에 nullptr 을 돌려준다(일반 표에 없어서)
//  - 빈 번호를 다시 쓴다(CreateRoom) — 방은 들어갈 때마다 하나라 단조 증가면 서버가 뜬 뒤 상한만큼 들어가고 끝난다.
//    번호를 다시 써도 되는 근거 = 존 번호를 틱 너머로 드는 곳이 전부 존과 같이 사라지거나(몬스터 · 땅 아이템) 지우기 직전에 비워지고
//    (조회표 · 버프 목록 — 송신 대기 칸은 틱 끝마다), 사람은 존이 사람 0 일 때만 지워져 옛 번호를 든 채 남지 않는다(room-progress.md 1-0 표)
//  - 비면 지운다(CloseEmptyRooms)
//  - 채널 지정·무작위 맵은 CMapManager 가 막는다(PickZoneChangeTarget · GetRandomMapId)
// ==========================================================================
class CRoomMap
{
public:
    explicit CRoomMap(const CGameDataFiles& gameData) : _table(gameData) {}

    // 맵 설정으로 초기화 — 채널은 안 만든다
    bool Init(const MapConfig& config);

    // 방 하나 만들기 — 1 부터 비어 있는 가장 작은 번호를 쓴다. 번호가 다 찼으면 nullptr
    CZone* CreateRoom();

    // 특정 방 조회 (없으면 nullptr)
    CZone* FindChannel(int32_t channelIndex) { return _table.Find(channelIndex); }

    // 빈 방 정리 (플레이어 0명 — 방은 채널 0 이 없어 빈 방이 전부)
    //   onBeforeErase: 존을 실제로 지우기 직전에 부른다(비어 있으면 안 부른다). 받는 쪽은 CGameServer::OnErasingZone 하나다.
    //   ★ 계약 — 존이 소유한 것(몬스터 관리자의 몬스터 · 땅 아이템 관리자의 목록)은 존과 함께 해제된다(Despawn 을 안 거친다).
    //     그 주소를 존 밖에서 틱 너머로 드는 쪽은 여기서 놓아야 한다 — 안 놓으면 다음 틱이 해제된 메모리를 밟는다. 지금 그런 곳은 둘:
    //       지속 효과 목록(CBuffSystem::ForgetZone 으로 뺀다 — 10-14 실서버에서 빠져 있던 길) ·
    //       CSightBroadcaster 의 이동 묶음(정리를 틱 끝에 두는 차례로 비어 있다 — 게임 루프의 assert 가 지킨다).
    //     존 밖에서 세던 수(땅 아이템 수 · 열린 방 수)도 여기서 맞춘다. 새로 존 소유 객체의 주소를 밖에 드는 곳을 만들면 OnErasingZone 에 한 줄을 더한다.
    //   "후보를 돌려주고 밖에서 지운다"로 안 하는 이유는 그러면 "빈 방인가" 판정이 호출자 쪽에도 한 벌 더 생기기 때문이다.
    //     맵 관리자의 조회표(_zoneCache) 빼기도 같은 까닭으로 이 콜백을 거친다 — 판정은 여기 하나뿐이어야 한다.
    //   ★ 이 인자는 빌드 토글 밖에 둔다 — MapManager 가 빌드 토글을 알 이유가 없다.
    //     토글을 다 끈 빌드에서도 CGameServer 는 같은 콜백을 넘기고, 받는 쪽 몸만 빈다.
    //   (10-27 전엔 일반 맵의 빈 동적 채널도 같은 함수로 지웠다 — 그때 이름 CleanupEmptyChannels)
    void CloseEmptyRooms(const std::function<void(CZone*)>& onBeforeErase);

    void TickAll(float deltaTime, IZoneEvents& events) { _table.TickAll(deltaTime, events); }
    const MapConfig& GetConfig() const { return _table.GetConfig(); }

    template<typename Fn>
    void ForEachZone(Fn&& fn) { _table.ForEachZone(std::forward<Fn>(fn)); }

private:
    CChannelTable _table;
};

class CMapManager
{
public:
    // gameData — 기획 데이터(기동 때 읽고 안 바뀐다). 존을 만들 때마다 건네, 존 쪽(몬스터 관리자)이 몬스터·스폰 표를
    //   쥐고 언제든 꺼내 보게 한다. 여기서는 읽지 않고 건네기만 한다 — 그래서 빌드 토글을 모른다(CRoomMap::CloseEmptyRooms 의 ★ 과 같은 규칙).
    //   모든 빌드에 있다 — 맵 표는 토글이 없다(MapTable.h). 10-09 전에는 전투·몬스터를 다 끈 빌드에서 nullptr 이었다.
    explicit CMapManager(const CGameDataFiles& gameData) : _gameData(gameData) {}
    ~CMapManager() = default;

    // 맵 등록 (서버 시작 시) — 종류(config.kind)로 표가 갈린다: 방 맵(INSTANCE)은 CRoomMap(채널 없음), 나머지는 CFieldMap(기본 채널 0 자동 생성).
    //   처음 등록에 성공한 맵이 기본 접속 맵이 된다(GetDefaultMapId).
    bool RegisterMap(const MapConfig& config);

    // 기본 접속 맵 = 처음 등록한 맵 = 맵 표 첫 줄(마을 — MapTable.h). 등록 차례가 곧 표 차례다(CGameServer::Init 이 표 순서로 부른다).
    //   새 캐릭터 · 저장된 맵이 없을 때 · 죽은 채 저장됐을 때 · 마을 부활이 여기로 간다. 등록 전엔 0.
    int32_t GetDefaultMapId() const { return _defaultMapId; }

    // 채널 자동 배정 — 여유 있는 채널 반환, 없으면 동적 생성(CFieldMap). 방 맵은 일반 표에 없어 nullptr(로그인·포털 자동 배정이 방에 안 들어가게)
    CZone* FindOrCreateChannel(int32_t mapId, bool isAdmin = false);

    // 방 만들기 — 방 맵의 빈 번호로 새 채널(CRoomMap::CreateRoom). 맵이 없거나 방 맵이 아니거나 번호가 다 찼으면 nullptr
    //   들어갈 자격(파티·생존·잠금)은 여기서 안 본다 — 게임 서버가 판정한 뒤 부른다
    CZone* CreateRoom(int32_t mapId);

    // 방 맵인가 = 방 표(_roomMaps)에 있나 — 없는 맵이면 false
    bool IsRoomMap(int32_t mapId) const;

    // 특정 채널 조회 — 존재하지 않으면 nullptr (자동 생성 안 함)
    CZone* FindChannel(int32_t mapId, int32_t channelIndex);

    // 존 이동 목적지 고르기 — 지금 존에서 받은 요청(맵·채널)으로 목적지 존 또는 거절 사유를 돌려준다
    //   targetChannelIndex >= 0 : 지금 맵의 그 채널 (targetMapId 는 안 본다) — 없음 · 같은 존 · 정원 순으로 거절.
    //                             지금 맵이 방 맵이면 운영자만(아니면 NOT_ALLOWED — 남의 방 번호를 불러 들어가지 못하게)
    //   targetChannelIndex <  0 : 자동 배정 — 운영자만(아니면 NOT_ALLOWED — 다른 맵으로 가는 길은 포털).
    //                             targetMapId == -1 이면 지금 맵을 뺀 무작위 맵. 없음 · 같은 존 순으로 거절
    //   자동 배정은 채널을 새로 만들 수 있다(FindOrCreateChannel)
    ZoneChangeTarget PickZoneChangeTarget(int32_t currentZoneId, int32_t targetMapId, int32_t targetChannelIndex, bool isAdmin);

    // 빈 방 정리 (플레이어 0명인 방) — 판정은 CRoomMap 한 곳. 일반 맵 채널은 안 지운다(CFieldMap [책임] ★).
    //   여기는 지우기 직전 알림을 가로채 조회표(_zoneCache)에서 빼고 onBeforeErase 를 이어 부른다 — 뜻·계약은 CRoomMap::CloseEmptyRooms 주석 참조.
    //   그래서 onBeforeErase 안에서는 그 존이 GetZone 으로 안 찾아진다(nullptr) — zone 포인터와 그 번호로만 할 것.
    void CloseEmptyRooms(const std::function<void(CZone*)>& onBeforeErase);

    // 존 조회 (zoneId로 직접 접근)
    CZone* GetZone(int32_t zoneId);

    // 전체 존 Tick — 섹터 변경은 존마다 대기열에 쌓인다(CZone::GetSectorChanges). 걷기·멈춤 알림은 events 로 건네기만
    void TickAll(float deltaTime, IZoneEvents& events);

    // 현재 맵을 제외한 랜덤 맵 ID 반환 (-1이면 선택 불가) — 일반 표에서만 뽑는다(방 맵은 자동 배정이 nullptr 이라 뽑히면 늘 실패)
    int32_t GetRandomMapId(int32_t excludeMapId) const;

    // 전체 존 순회 콜백 — 일반 맵 다음 방 맵
    template<typename Fn>
    void ForEachZone(Fn&& fn)
    {
        for (auto& [mapId, fieldMap] : _fieldMaps)
            fieldMap.ForEachZone(fn);
        for (auto& [mapId, roomMap] : _roomMaps)
            roomMap.ForEachZone(fn);
    }

    // zoneId 인코딩/디코딩 헬퍼 — zoneId = mapId × CHANNELS_PER_MAP + channelIndex
    //   기수는 이 상수 하나다(10-08 2차 검사 9번). 채널 상한 검사(CChannelTable::Create)도 같은 값을 본다 —
    //   상한만 따로 올리면 맵 1 채널 10000 이 맵 2 채널 0 과 같은 번호가 되어 조회표(_zoneCache)에서 서로 덮어쓴다.
    //   10-17 1000 → 10000 — 방 맵은 채널 하나가 파티 하나라, 2인 파티 999개(1,998명)면 차던 것을 넓혔다(room-plan.md 「채널 상한」).
    //   최대 존 번호 = 맵 번호 상한(CMapTable::MAX_MAP_ID 1000) × 10000 + 9999 ≈ 1천만 — int32 안. 와이어·DB 에는 존 번호가 안 나간다
    static constexpr int32_t CHANNELS_PER_MAP = 10000;
    static int32_t MakeZoneId(int32_t mapId, int32_t channelIndex);
    static int32_t GetMapIdFromZoneId(int32_t zoneId);
    static int32_t GetChannelIndexFromZoneId(int32_t zoneId);

private:
    const CGameDataFiles& _gameData;   // 생성자 주석 — 맵마다 만들 때 건넨다

    // mapId → 맵 정책 — 한 맵 번호는 둘 중 한 표에만 있다(RegisterMap 이 중복을 막는다)
    std::unordered_map<int32_t, CFieldMap> _fieldMaps;   // 마을·사냥터
    std::unordered_map<int32_t, CRoomMap>  _roomMaps;    // 방 맵(instance)

    // zoneId → CZone* 플랫 캐시 (GetZone 해시 2회 → 1회 최적화, 채널 생성/삭제 시 갱신)
    std::unordered_map<int32_t, CZone*> _zoneCache;

    int32_t _defaultMapId = 0;   // 기본 접속 맵 — RegisterMap 이 첫 성공 때 정한다(GetDefaultMapId)
};
