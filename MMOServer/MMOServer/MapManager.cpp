#include "MapManager.h"
#include <random>

// ==========================================================================
// CChannelTable
// ==========================================================================

CZone* CChannelTable::Create(int32_t channelIndex)
{
    // channelIndex 상한 검증 — 존 번호 기수와 같은 값(CMapManager::CHANNELS_PER_MAP)
    if (channelIndex >= CMapManager::CHANNELS_PER_MAP)
        return nullptr;

    // 중복 방지
    if (_channels.find(channelIndex) != _channels.end())
        return nullptr;

    int32_t zoneId = CMapManager::MakeZoneId(_config.mapId, channelIndex);

    auto zone = std::make_unique<CZone>(_gameData);
    if (!zone->Init(zoneId, _config.mapId, _config.mapWidth, _config.mapHeight, _config.sectorSize, _config.start))
        return nullptr;

    CZone* ptr = zone.get();
    _channels[channelIndex] = std::move(zone);

    return ptr;
}

CZone* CChannelTable::Find(int32_t channelIndex)
{
    auto it = _channels.find(channelIndex);
    if (it == _channels.end())
        return nullptr;
    return it->second.get();
}

void CChannelTable::TickAll(float deltaTime, IZoneEvents& events)
{
    for (auto& [channelIndex, zone] : _channels)
    {
        // 쉬는 존(사람 0)은 안 걷는다 — 몬스터 판단도 같은 판정으로 쉰다(CMonsterManager::Tick). 하나만 쉬면 판단 없이 걷기만 이어진다
        if (zone->IsIdle())
            continue;
        zone->Tick(deltaTime, events);
    }
}

// ==========================================================================
// CFieldMap
// ==========================================================================

bool CFieldMap::Init(const MapConfig& config)
{
    _table.SetConfig(config);

    // 기본 채널(channelIndex=0) 자동 생성
    if (_table.Create(0) == nullptr)
        return false;

    return true;
}

CZone* CFieldMap::FindOrCreateChannel(bool isAdmin)
{
    // 기본 채널(0) 우선 배정
    CZone* base = _table.Find(0);
    if (base != nullptr && !IsChannelFull(base, isAdmin))
        return base;

    // 나머지 동적 채널 탐색 — 빈 채널도 여기서 다시 쓰인다(지우지 않으므로 채널 수가 몰릴 때의 최대로 묶인다)
    CZone* open = _table.FindIf([&](int32_t channelIndex, CZone* zone)
    {
        return channelIndex != 0 && !IsChannelFull(zone, isAdmin);
    });
    if (open != nullptr)
        return open;

    // 모든 채널이 가득 참 — 동적 채널 생성
    int32_t newChannelIndex = _nextChannelIndex++;
    return _table.Create(newChannelIndex);
}

bool CFieldMap::IsChannelFull(const CZone* zone, bool isAdmin) const
{
    return !isAdmin && zone->GetPlayerCount() >= _table.GetConfig().maxPlayersPerChannel;
}

// ==========================================================================
// CRoomMap
// ==========================================================================

bool CRoomMap::Init(const MapConfig& config)
{
    // 채널 0 을 안 만든다 — 만들면 누구나 들어가는 공용 방이 된다(헤더 [일반 맵과 다른 점]). 방은 CreateRoom 으로만 생긴다
    _table.SetConfig(config);
    return true;
}

CZone* CRoomMap::CreateRoom()
{
    // 1 부터 비어 있는 가장 작은 번호 — 방은 생겼다 지워지기를 되풀이하므로 단조 증가로는 상한에 닿는다(헤더 [일반 맵과 다른 점]).
    //   훑기는 열린 방 수 + 1 번 안에 끝난다(그 안에 빈 번호가 반드시 있다). 방을 만드는 건 파티장이 포털을 쓸 때뿐이라 드물다
    for (int32_t channelIndex = 1; channelIndex < CMapManager::CHANNELS_PER_MAP; ++channelIndex)
    {
        if (_table.Find(channelIndex) == nullptr)
            return _table.Create(channelIndex);
    }
    return nullptr;   // 번호가 다 찼다 — 게임 서버가 「모든 채널 가득 참」으로 거절한다
}

void CRoomMap::CloseEmptyRooms(const std::function<void(CZone*)>& onBeforeErase)
{
    // 방에는 채널 0 이 없어 빈 방이 전부 지울 것이다
    _table.EraseIf([](int32_t, CZone* zone) { return zone->GetPlayerCount() == 0; }, onBeforeErase);
}

// ==========================================================================
// CMapManager
// ==========================================================================

bool CMapManager::RegisterMap(const MapConfig& config)
{
    // 설정 유효성 검증
    if (config.maxPlayersPerChannel <= 0)
        return false;

    // ※ 「맵이 패킷 좌표 양자화 값(uint16)에 담기는 크기인가」는 여기서 안 본다 — 와이어 사정이라 패킷을 아는
    //   게임 서버가 RegisterMap 을 부르기 전에 본다(CGameServer::Init). 맵 관리자는 그 상한을 모른다.

    // 중복 맵 등록 방지 — 두 표를 다 본다(한 번호는 한 표에만)
    if (_fieldMaps.find(config.mapId) != _fieldMaps.end() || _roomMaps.find(config.mapId) != _roomMaps.end())
        return false;

    if (config.kind == MapKind::INSTANCE)
    {
        // 방 맵 — 채널이 없어 조회표에 넣을 것도 없다(방은 CreateRoom 이 넣는다)
        CRoomMap& roomMap = _roomMaps.try_emplace(config.mapId, _gameData).first->second;
        if (!roomMap.Init(config))
        {
            _roomMaps.erase(config.mapId);
            return false;
        }
    }
    else
    {
        CFieldMap& fieldMap = _fieldMaps.try_emplace(config.mapId, _gameData).first->second;
        if (!fieldMap.Init(config))
        {
            _fieldMaps.erase(config.mapId);
            return false;
        }

        // 기본 채널(channelIndex=0) 캐시 등록
        int32_t zoneId = MakeZoneId(config.mapId, 0);
        CZone* baseZone = fieldMap.FindChannel(0);
        if (baseZone != nullptr)
            _zoneCache[zoneId] = baseZone;
    }

    // 처음 등록에 성공한 맵 = 기본 접속 맵(실패한 등록은 위에서 지워져 개수에 안 든다). 전에는 게임 서버가 maps[0] 으로 따로 들었다.
    if (_fieldMaps.size() + _roomMaps.size() == 1)
        _defaultMapId = config.mapId;

    return true;
}

CZone* CMapManager::FindOrCreateChannel(int32_t mapId, bool isAdmin)
{
    // 일반 표에서만 — 방 맵은 자동 배정이 없다. 로그인(저장된 자리가 방 맵) · 포털 · 운영자 무작위가 여기로 방에 들어가지 못하게.
    //   부르는 쪽은 nullptr 을 「그 맵에 자리 없음」으로 이미 다룬다(로그인은 기본 맵으로 — CEntryHandler::OnCharacterLoaded)
    auto it = _fieldMaps.find(mapId);
    if (it == _fieldMaps.end())
        return nullptr;

    CZone* zone = it->second.FindOrCreateChannel(isAdmin);

    // 새 채널이 생성되었을 수 있으므로 캐시 갱신
    if (zone != nullptr)
        _zoneCache[zone->GetZoneId()] = zone;

    return zone;
}

CZone* CMapManager::CreateRoom(int32_t mapId)
{
    // 방 표에서만 — 일반 맵 번호면 없음(nullptr)
    auto it = _roomMaps.find(mapId);
    if (it == _roomMaps.end())
        return nullptr;

    CZone* zone = it->second.CreateRoom();

    // 새 채널이 생겼으므로 캐시 갱신 — 다시 쓴 번호면 지울 때 이미 빠져 있다(CloseEmptyRooms)
    if (zone != nullptr)
        _zoneCache[zone->GetZoneId()] = zone;

    return zone;
}

bool CMapManager::IsRoomMap(int32_t mapId) const
{
    return _roomMaps.find(mapId) != _roomMaps.end();
}

CZone* CMapManager::FindChannel(int32_t mapId, int32_t channelIndex)
{
    // 두 표 중 그 번호가 있는 쪽 — 한 번호는 한 표에만 있다
    if (auto it = _fieldMaps.find(mapId); it != _fieldMaps.end())
        return it->second.FindChannel(channelIndex);
    if (auto it = _roomMaps.find(mapId); it != _roomMaps.end())
        return it->second.FindChannel(channelIndex);
    return nullptr;
}

ZoneChangeTarget CMapManager::PickZoneChangeTarget(int32_t currentZoneId, int32_t targetMapId, int32_t targetChannelIndex, bool isAdmin)
{
    const int32_t currentMapId = GetMapIdFromZoneId(currentZoneId);

    CZone* zone = nullptr;
    CFieldMap* pickedMap = nullptr;   // 일반 맵 채널 지정일 때만 — 정원은 아래에서 이 맵의 규칙으로 본다

    if (targetChannelIndex >= 0)
    {
        // ── 채널 지정 이동 (같은 맵 내) ──
        if (auto rit = _roomMaps.find(currentMapId); rit != _roomMaps.end())
        {
            // 방 맵 안에서는 운영자만 — 방 번호를 불러 남의 방에 들어가지 못하게(CRoomMap [일반 맵과 다른 점]). 운영자는 구경·시험용으로 둔다
            //   정원은 안 본다 — 여기까지 오는 건 운영자뿐이고 운영자는 정원에 안 걸린다(CFieldMap::IsChannelFull 과 같은 규칙)
            if (!isAdmin)
                return { nullptr, ZoneChangeReject::NOT_ALLOWED };

            zone = rit->second.FindChannel(targetChannelIndex);
        }
        else if (auto fit = _fieldMaps.find(currentMapId); fit != _fieldMaps.end())
        {
            pickedMap = &fit->second;
            zone = pickedMap->FindChannel(targetChannelIndex);
        }
    }
    else
    {
        // ── 기존 맵 이동 (자동 채널 배정) ──

        // 맵을 고르는 요청은 운영자만 — 다른 맵으로 가는 길은 포털이다(CEntryHandler::RecvPortalUse).
        //   「가득 차면 다음 채널」은 이 요청이 아니라 입장 때 FindOrCreateChannel 이 고른다. 채널 지정(위 분기)은 누구나
        if (!isAdmin)
            return { nullptr, ZoneChangeReject::NOT_ALLOWED };

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

    // 인원 제한 체크 (admin은 스킵) — 자동 배정은 FindOrCreateChannel 이 같은 규칙(IsChannelFull)으로 이미 골랐다
    if (pickedMap != nullptr && pickedMap->IsChannelFull(zone, isAdmin))
        return { nullptr, ZoneChangeReject::FULL };

    return { zone, ZoneChangeReject::NONE };
}

void CMapManager::CloseEmptyRooms(const std::function<void(CZone*)>& onBeforeErase)
{
    // 「빈 방인가」 판정은 실제로 지우는 CRoomMap 한 곳뿐이다 — 여기는 지우기 직전 알림을 받아
    //   조회표(_zoneCache)에서 빼고 게임 서버의 콜백을 이어 부른다. 같은 판정을 여기서 한 번 더 돌리지 않는다
    //   (전에는 캐시 빼기용으로 조건을 한 벌 더 들고 있었다 — 10-06, 아키텍처 전수검사 5번).
    //   순서 — 조회표에서 먼저 빼고 콜백: 콜백이 하는 일(CGameServer::OnErasingZone — 땅 아이템 세기 · 지속 효과 목록에서 빼기 · 방 닫힘 세기)은
    //   zone 포인터와 그 번호로만 하고 GetZone 을 안 거치므로 무관하다. 존 객체는 콜백이 끝난 뒤 지워진다.
    //   일반 표는 안 훑는다 — 일반 맵 채널은 비어도 안 지운다(CFieldMap [책임] ★)
    for (auto& entry : _roomMaps)
    {
        entry.second.CloseEmptyRooms([&](CZone* zone)
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

void CMapManager::TickAll(float deltaTime, IZoneEvents& events)
{
    for (auto& [mapId, fieldMap] : _fieldMaps)
        fieldMap.TickAll(deltaTime, events);
    for (auto& [mapId, roomMap] : _roomMaps)
        roomMap.TickAll(deltaTime, events);
}

int32_t CMapManager::GetRandomMapId(int32_t excludeMapId) const
{
    static constexpr int32_t MAX_MAPS = 64;
    int32_t candidates[MAX_MAPS];
    int32_t count = 0;

    // 일반 표만 훑는다 — 방 맵은 자동 배정이 없어(FindOrCreateChannel nullptr) 뽑히면 이동이 늘 「없음」으로 끝난다
    for (const auto& [mapId, fieldMap] : _fieldMaps)
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
    return mapId * CHANNELS_PER_MAP + channelIndex;
}

int32_t CMapManager::GetMapIdFromZoneId(int32_t zoneId)
{
    return zoneId / CHANNELS_PER_MAP;
}

int32_t CMapManager::GetChannelIndexFromZoneId(int32_t zoneId)
{
    return zoneId % CHANNELS_PER_MAP;
}
