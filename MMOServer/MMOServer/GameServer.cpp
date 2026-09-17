#include "GameServer.h"
#include "Player.h"
#include <Protocol/Protocol.h>
#include "SerialBuffer.h"
#include <Common/ErrorLog.h>
#include "CoreAffinity.h"
#include <iostream>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstddef>  // offsetof
#include <cassert>  // 직렬화 필드 누락/폭 검증

// ==========================================================================
// S2C 직렬화 헬퍼 (파일 로컬)
//
// 와이어 = payload 영역 바이트 그대로 (WSASend가 GetReadBufferPtr()+GetDataSize()만 전송).
// pack(1)이라 패딩이 없으므로 필드를 선언 순서/폭대로 쓰면 구조체와 바이트 동일.
// 스칼라는 <<, 고정배열/문자열은 SetData(raw) — << 문자열 연산자는 길이접두(2B)가 끼어
// 와이어가 깨지므로 사용 금지.
//
// [소유권 계약] 빌더는 RefCount=1(소유권 1개)인 버퍼를 반환하고,
//   송신 함수(BroadcastAroundSector/SendPacket의 CSerialBuffer* 오버로드)가 이를 소비한다.
// ==========================================================================
namespace
{
    // 헤더 시작: size placeholder(아래서 백패치) + type
    inline void BeginPacket(CSerialBuffer* buf, MsgType type)
    {
        *buf << static_cast<uint16_t>(0);                 // header.size (placeholder)
        *buf << static_cast<uint16_t>(type);              // header.type
    }

    // 헤더 종료: size 백패치(= 전체 payload 바이트) + Seal
    // 소유권 1은 Alloc()/Clear()에서 이미 확보됨(RefCount=1) → 여기서 AddRef 불필요
    inline void FinalizePacket(CSerialBuffer* buf)
    {
        *reinterpret_cast<uint16_t*>(buf->GetPayloadBufferPtr()) =
            static_cast<uint16_t>(buf->GetDataSize());
        buf->Seal();
    }

    // MOVE_START / MOVE_STOP은 레이아웃 동일 — 3곳/2곳에서 재사용되므로 빌더로 DRY 처리
    CSerialBuffer* MakeMoveStart(int32_t playerId, uint8_t direction, float x, float y)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_MOVE_START::TYPE);
        *buf << static_cast<int>(playerId);
        *buf << static_cast<uint8_t>(direction);
        *buf << x;
        *buf << y;
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_MOVE_START));
        return buf;
    }

    CSerialBuffer* MakeMoveStop(int32_t playerId, uint8_t direction, float x, float y)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_MOVE_STOP::TYPE);
        *buf << static_cast<int>(playerId);
        *buf << static_cast<uint8_t>(direction);
        *buf << x;
        *buf << y;
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_MOVE_STOP));
        return buf;
    }

    CSerialBuffer* MakeSyncPosition(int32_t playerId, float x, float y)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_SYNC_POSITION::TYPE);
        *buf << static_cast<int>(playerId);
        *buf << x;
        *buf << y;
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_SYNC_POSITION));
        return buf;
    }

    CSerialBuffer* MakeZoneInfo(CZone* zone)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ZONE_INFO::TYPE);
        *buf << static_cast<int>(zone->GetMapId());
        *buf << static_cast<int>(CMapManager::GetChannelIndexFromZoneId(zone->GetZoneId()));
        *buf << static_cast<int>(zone->GetMapWidth());
        *buf << static_cast<int>(zone->GetMapHeight());
        *buf << static_cast<int>(zone->GetSectorManager().GetSectorSize());
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ZONE_INFO));
        return buf;
    }

    CSerialBuffer* MakeCreateMyPlayer(CPlayer* target)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_CREATE_MY_PLAYER::TYPE);
        *buf << static_cast<int>(target->_playerId);
        *buf << static_cast<uint8_t>(target->_direction);
        *buf << static_cast<uint8_t>(target->_displayChar);
        *buf << static_cast<uint8_t>(target->_colorIndex);
        *buf << target->_x;
        *buf << target->_y;
        *buf << static_cast<int>(target->_speed);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_CREATE_MY_PLAYER));
        return buf;
    }

    CSerialBuffer* MakeCreateOtherPlayer(CPlayer* player, SpawnReason reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_CREATE_OTHER_PLAYER::TYPE);
        *buf << static_cast<int>(player->_playerId);
        *buf << static_cast<uint8_t>(player->_direction);
        *buf << static_cast<uint8_t>(player->_moveState);
        *buf << static_cast<uint8_t>(player->_displayChar);
        *buf << static_cast<uint8_t>(player->_colorIndex);
        *buf << static_cast<uint8_t>(reason);
        *buf << player->_x;
        *buf << player->_y;
        *buf << static_cast<int>(player->_speed);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_CREATE_OTHER_PLAYER));
        return buf;
    }

    CSerialBuffer* MakeDeletePlayer(int32_t playerId)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_DELETE_PLAYER::TYPE);
        *buf << static_cast<int>(playerId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_DELETE_PLAYER));
        return buf;
    }

    CSerialBuffer* MakeZoneChangeOk(CPlayer* target, int32_t mapId, int32_t channelIndex)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ZONE_CHANGE_OK::TYPE);
        *buf << static_cast<int>(mapId);
        *buf << static_cast<int>(channelIndex);
        *buf << static_cast<int>(target->_playerId);
        *buf << static_cast<uint8_t>(target->_displayChar);
        *buf << static_cast<uint8_t>(target->_colorIndex);
        *buf << static_cast<uint8_t>(target->_direction);
        *buf << target->_x;
        *buf << target->_y;
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ZONE_CHANGE_OK));
        return buf;
    }

    CSerialBuffer* MakeZoneChangeFail(uint8_t reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ZONE_CHANGE_FAIL::TYPE);
        *buf << static_cast<uint8_t>(reason);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ZONE_CHANGE_FAIL));
        return buf;
    }

    CSerialBuffer* MakeAdminLoginOk()
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ADMIN_LOGIN_OK::TYPE);    // 페이로드 없음 (헤더만)
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ADMIN_LOGIN_OK));
        return buf;
    }

    CSerialBuffer* MakeAdminLoginFail()
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ADMIN_LOGIN_FAIL::TYPE);  // 페이로드 없음 (헤더만)
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ADMIN_LOGIN_FAIL));
        return buf;
    }

    // 가변 길이 — message는 고정배열/문자열이라 SetData(raw). msgLen은 null 제외 글자 수.
    CSerialBuffer* MakeChat(int32_t playerId, uint8_t displayChar, uint8_t colorIndex,
                             ChatChar* message, uint16_t msgLen)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_CHAT::TYPE);
        *buf << static_cast<int>(playerId);
        *buf << static_cast<uint8_t>(displayChar);
        *buf << static_cast<uint8_t>(colorIndex);
        buf->SetData(reinterpret_cast<char*>(message),
            (msgLen + 1) * sizeof(ChatChar));              // message (가변, null 포함, raw)
        FinalizePacket(buf);
        assert(buf->GetDataSize() ==
            offsetof(MSG_S2C_CHAT, message) + (msgLen + 1) * sizeof(ChatChar));
        return buf;
    }

#if USE_SECTOR_AGGREGATION
    // 섹터 묶음 — dirty 플레이어 [0,count)의 최종 상태(위치·방향·이동상태)를 직렬화.
    // 스칼라 시퀀스(<<)라 SetData 불필요. count는 선기록(백패치 불필요).
    // 좌표는 float 그대로가 아니라 눈금(QuantizePos)으로 싣는다 — 필드 순서는 SectorUpdateEntry와 같아야 한다.
    CSerialBuffer* MakeSectorUpdates(CPlayer** players, int count)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_SECTOR_UPDATES::TYPE);
        *buf << static_cast<uint16_t>(count);
        for (int i = 0; i < count; ++i)
        {
            CPlayer* p = players[i];
            *buf << static_cast<int>(p->_playerId);
            *buf << static_cast<uint16_t>(QuantizePos(p->_x));
            *buf << static_cast<uint16_t>(QuantizePos(p->_y));
            *buf << static_cast<uint8_t>(PackDirState(static_cast<uint8_t>(p->_direction),
                                                      static_cast<uint8_t>(p->_moveState)));
        }
        FinalizePacket(buf);
        // (정정) GetDataSize는 BeginPacket이 쓴 MsgHeader 4B를 포함 — offsetof 기준이 맞는 식
        assert(buf->GetDataSize() ==
            static_cast<int>(offsetof(MSG_S2C_SECTOR_UPDATES, entries) + count * sizeof(SectorUpdateEntry)));
        return buf;
    }
#endif

#if USE_MEMBERSHIP_INBOUND_BUNDLE
    // [Phase 2] 인바운드 멤버십 묶음 — 상대 [0,count)의 생성 정보를 배치 1패킷으로 직렬화.
    // 필드 순서는 MakeCreateOtherPlayer 바디와 동일 (클라가 엔트리를 기존 CREATE 이벤트로 분해).
    CSerialBuffer* MakeCreatePlayerBatch(CPlayer* const* players, int count)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_CREATE_PLAYER_BATCH::TYPE);
        *buf << static_cast<uint16_t>(count);
        for (int i = 0; i < count; ++i)
        {
            CPlayer* p = players[i];
            *buf << static_cast<int>(p->_playerId);
            *buf << static_cast<uint8_t>(p->_direction);
            *buf << static_cast<uint8_t>(p->_moveState);
            *buf << static_cast<uint8_t>(p->_displayChar);
            *buf << static_cast<uint8_t>(p->_colorIndex);
            *buf << static_cast<uint8_t>(SpawnReason::NORMAL);   // 인바운드(걸어서 시야 진입) 경로 고정 — OFF의 기본 인자와 동일
            *buf << p->_x;
            *buf << p->_y;
            *buf << static_cast<int>(p->_speed);
        }
        FinalizePacket(buf);
        assert(buf->GetDataSize() ==
            static_cast<int>(offsetof(MSG_S2C_CREATE_PLAYER_BATCH, entries) + count * sizeof(CreatePlayerBatchEntry)));
        return buf;
    }

    // [Phase 2] 인바운드 멤버십 묶음 — 상대 [0,count)의 playerId를 배치 1패킷으로 직렬화.
    CSerialBuffer* MakeDeletePlayerBatch(CPlayer* const* players, int count)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_DELETE_PLAYER_BATCH::TYPE);
        *buf << static_cast<uint16_t>(count);
        for (int i = 0; i < count; ++i)
            *buf << static_cast<int>(players[i]->_playerId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() ==
            static_cast<int>(offsetof(MSG_S2C_DELETE_PLAYER_BATCH, entries) + count * sizeof(DeletePlayerBatchEntry)));
        return buf;
    }
#endif
}

CGameServer::CGameServer(CMonitorManager& monitor)
    : _mode(ServerMode::GameServer)
    , _monitor(monitor)
    , _running(false)
{
}

CGameServer::~CGameServer()
{
    Stop();
}

bool CGameServer::Init(ServerMode mode, int port, int maxClients)
{
    return Init(mode, port, maxClients, nullptr, 0);
}

bool CGameServer::Init(ServerMode mode, int port, int maxClients,
                        const MapConfig* maps, int32_t mapCount, int workerThreads, int sendWorkers,
                        int rioWorkers, int completionBatch, int sendDepth)
{
    _mode = mode;

    _network = std::make_unique<NetIoModel>(port, maxClients, _mode, _monitor,
                                            workerThreads, sendWorkers, rioWorkers, completionBatch, sendDepth);

    // 게임 서버 모드일 때만 맵 등록
    if (_mode == ServerMode::GameServer)
    {
        if (maps == nullptr || mapCount <= 0)
            return false;

        for (int32_t i = 0; i < mapCount; ++i)
        {
            if (!_mapManager.RegisterMap(maps[i]))
                return false;
        }

        // 첫 번째 맵을 기본 접속 맵으로 설정
        _defaultMapId = maps[0].mapId;

        // 프레임 재사용 컨테이너 reserve (워밍업 realloc 방지)
        // 전체 서버 범위 버퍼도 maxPerChannel 기준 — 초과 시 자연 증가 후 capacity 유지
        int32_t maxPerChannel = maps[0].maxPlayersPerChannel;
        _sessionSlots.assign(maxClients, nullptr);   // 인덱스 직접 접근용 슬롯 (sessionId 인덱스로 접근)
        _broadcastBuffer.reserve(maxPerChannel);       // 주변 9섹터 단위
        _eventAroundBuffer.reserve(maxPerChannel);     // 주변 9섹터 단위
        _pendingSectorChanges.reserve(maxPerChannel);  // 전체 서버 프레임 이벤트
        _tickSectorChanges.reserve(maxPerChannel);     // 전체 서버 TickAll 결과
        _tickClampedPlayers.reserve(maxPerChannel);    // 전체 서버 TickAll 결과
        _sectorChangedSet.reserve(maxPerChannel);      // 전체 서버 dedup
        _sectorOutbox.dirtyMovers.reserve(maxClients);              // 전체 서버 dirty (이동/sync 묶음 대상)
    }

    return true;
}

bool CGameServer::Start()
{
    if (!_network->Start())
        return false;

    // 게임 서버 모드일 때만 게임 루프 스레드 시작
    // 에코 테스트는 CIOCPServer 내부에서 자체 처리
    if (_mode == ServerMode::GameServer)
    {
        _running = true;
        _gameThread = std::thread(&CGameServer::GameLoopThread, this);
    }

    const char* modeName = "Unknown";
    switch (_mode)
    {
    case ServerMode::GameCodiEchoTest:    modeName = "GameCodiEchoTest";    break;
    case ServerMode::NetWorkLib_EchoTest: modeName = "NetWorkLib_EchoTest"; break;
    case ServerMode::GameServer:          modeName = "GameServer";          break;
    }
    SLOG_INFO("[GameServer] Started - Mode: {}", modeName);

    return true;
}

void CGameServer::Stop()
{
    _running = false;

    if (_gameThread.joinable())
        _gameThread.join();

#if USE_DB_WORKER
    // 게임 스레드 종료 후(단일 스레드 시점) 전원 최종 저장 → 드레인 → 워커 종료
    if (_dbWorker)
    {
        SaveAllPlayers();
        _dbWorker->Shutdown(0);
        _dbWorker.reset();
    }
#endif

    if (_network)
    {
        _network->ShutdownServer();

        // 미처리 이벤트의 CSerialBuffer 정리
        NetworkEvent event(NetworkEvent::Type::CONNECTED, 0);
        while (_network->PopNetworkEvent(event))
        {
            if (event.pMsg != nullptr)
                event.pMsg->SubRef();
        }
    }

    // 잔여 플레이어 정리 (CGameServer가 생명주기 소유)
    for (CPlayer* player : _sessionSlots)   // nullptr는 delete 무해
    {
        delete player;
    }
    _sessionSlots.clear();
}

#if USE_DB_WORKER
// ==========================================================================
// DB 저장 파이프라인 (dirty flag 기반 비동기 위치 저장)
// ==========================================================================

bool CGameServer::InitDB(const DBConfig& dbConfig, int savePeriodSec, int workerCount, int queueMax)
{
    _dbWorker = std::make_unique<CDBWorker>(_monitor);
    if (!_dbWorker->Start(dbConfig, workerCount, queueMax))
    {
        SLOG_ERROR("[DB] InitDB failed - check MySQL connection/config");
        _dbWorker.reset();
        return false;
    }
    _dbSavePeriodFrames = (savePeriodSec > 0 ? savePeriodSec : 10) * FRAME_PER_SEC;
    _dbSaveFrameCounter = 0;
    return true;
}

bool CGameServer::ShouldSave(const CPlayer* player) const
{
    // 저장 대상 판정(주기·종료 공통):
    //   MOVING   = 위치가 계속 변하는 중 → 저장 후 dirty가 리셋돼도 매 주기 재저장 필요
    //   _dbDirty = 정지/접속/존변경/클램프로 생긴 미저장 변경(일회성, 저장 후 리셋)
    return player->_moveState == MoveState::MOVING || player->_dbDirty;
}

DBSaveJob CGameServer::MakeSaveJob(const CPlayer* player) const
{
    DBSaveJob job;
    job.accountId = player->_accountId;
    job.x         = player->_x;
    job.y         = player->_y;
    job.mapId     = CMapManager::GetMapIdFromZoneId(player->_zoneId);
    return job;
}

void CGameServer::EnqueuePlayerSave(CPlayer* player)
{
    // 수용된 경우에만 dirty 해제 — 백프레셔로 드롭됐는데 지워버리면 그 변경은 복원 통로가 없다.
    if (_dbWorker->Enqueue(MakeSaveJob(player)))
        player->_dbDirty = false;
}

void CGameServer::TickPeriodicSave()
{
    if (++_dbSaveFrameCounter < _dbSavePeriodFrames)
        return;
    _dbSaveFrameCounter = 0;

    // 서버 메모리 = 진실. 저장 대상(ShouldSave)만 배치에 모아 워커에 1회 핸드오프.
    //   thread_local 재사용으로 매 주기 할당 회피 (게임 스레드 단독 접근).
    thread_local std::vector<DBSaveJob> batch;
    thread_local std::vector<CPlayer*>  owners;    // batch와 나란한 소유자 (드롭 시 dirty 복원용)
    thread_local std::vector<size_t>    dropped;
    batch.clear();
    owners.clear();
    for (CPlayer* player : _sessionSlots)
    {
        if (player == nullptr)
            continue;
        if (ShouldSave(player))
        {
            batch.push_back(MakeSaveJob(player));
            owners.push_back(player);
            player->_dbDirty = false;   // 저장 요청 후 리셋 (드롭되면 아래에서 되돌림)
        }
    }
    _dbWorker->EnqueueBatch(batch, &dropped);   // lock 1회 + notify 1회

    // 백프레셔로 버려진 잡은 dirty를 되살려 다음 주기에 재시도한다.
    //   되돌리지 않으면 정지(IDLE) 상태 플레이어의 변경은 ShouldSave 대상에서 빠져 영영 유실된다.
    //   호출은 전부 게임 스레드 단독이라 owners 포인터는 이 시점에 유효하다.
    for (size_t idx : dropped)
        owners[idx]->_dbDirty = true;
}

void CGameServer::SaveAllPlayers()
{
    // 종료는 유실 방지 우선 — dirty/MOVING 무관하게 전원 최종 저장(안전망).
    // 주기 저장(ShouldSave 필터)과 목적이 다름: 여기선 dirty 추적을 신뢰하지 않고 무조건 다 씀.
    std::vector<DBSaveJob> batch;
    batch.reserve(_sessionSlots.size());
    for (CPlayer* player : _sessionSlots)
    {
        if (player == nullptr)
            continue;
        batch.push_back(MakeSaveJob(player));
        player->_dbDirty = false;
    }
    _dbWorker->EnqueueBatch(batch);   // 배치 1회 핸드오프
}
#else
bool CGameServer::InitDB(const DBConfig&, int, int, int) { return true; }   // 토글 OFF: no-op
#endif

// ==========================================================================
// 게임 루프
// ==========================================================================

void CGameServer::GameLoopThread()
{
    using Clock = std::chrono::steady_clock;

    CoreAffinity::PinGameThread();   // 게임루프 전용 코어 고정 (격리 off면 no-op)

    // CPU 점유율 측정용: 자기 실핸들을 복제해 모니터에 등록 (진단정리 6 사각지대 보강)
    // GetCurrentThread()는 의사핸들(호출 스레드 기준)이라 HTTP 스레드에서 못 씀 → 실핸들로 복제
    {
        _monitor._gameLoopThreadHandle = Platform::CaptureCurrentThreadCpu();
    }

    auto prevTime = Clock::now();

    while (_running)
    {
        auto frameStart = Clock::now();

        // deltaTime 계산
        float deltaTime = std::chrono::duration<float>(frameStart - prevTime).count();
        prevTime = frameStart;
        ++_frameCount;   // 이동 예산의 경과 시간 산출 기준 (틱당 FRAME_INTERVAL_MS)

        // 1) 네트워크 이벤트 전부 소비
        _monitor._gameLoop._eventQueueSize.Store(static_cast<int64_t>(_network->GetEventQueueSize()));
        auto phaseT1 = Clock::now();
        ProcessNetworkEvents();
        auto phaseT2 = Clock::now();

        // 2) 게임 로직 갱신 (좌표 이동 + 섹터 변경 감지 + 경계 클램핑)
        _tickSectorChanges.clear();
        _tickClampedPlayers.clear();
        _mapManager.TickAll(deltaTime, _tickSectorChanges, _tickClampedPlayers);

        // 3) 섹터 변경 배치 처리 (RecvMoveStop + TickAll 병합 → 삽입 시 중복 차단)
        for (const auto& change : _tickSectorChanges)
        {
            PushSectorChange(change.player, change.oldSectorX, change.oldSectorY);
        }

        // [계측] 멤버십(섹터이동 CREATE/DELETE) 송신 시간 — 구간 1쌍으로만 측정.
        //   per-call 금지: 멤버십은 ~1.04M/s라 건당 now()를 넣으면 측정 오버헤드가 대상을 오염시킴.
        auto membT0 = Clock::now();
        for (const auto& change : _pendingSectorChanges)
        {
            // 출발 섹터 == 현재 섹터이면 원위치 복귀 → 브로드캐스트 불필요
            if (change.oldSectorX == change.player->_sectorX &&
                change.oldSectorY == change.player->_sectorY)
                continue;

            CZone* zone = _mapManager.GetZone(change.player->_zoneId);
            if (zone != nullptr)
            {
                ProcessSectorChange(zone, change.player,
                                    change.oldSectorX, change.oldSectorY);
            }
        }
        // 같은 틱에 함께 이동한 쌍은 위 루프가 서로를 놓친다(상대는 이미 새 섹터로 옮겨간 뒤라
        // 이탈/진입 섹터 조회에 안 걸림) → 누락된 CREATE/DELETE만 보충. 보정 비용도 멤버십 구간에 포함.
        FixSameTickMoverPairs();

        _tickMembershipUs += std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - membT0).count();
        _pendingSectorChanges.clear();
        _sectorChangedSet.clear();

        // 3-1) 맵 경계 클램핑으로 정지된 플레이어에게 MOVE_STOP 브로드캐스트
        for (CPlayer* player : _tickClampedPlayers)
        {
#if USE_DB_WORKER
            player->_dbDirty = true;   // 경계 클램프로 위치 변경 → 저장 대상
#endif
            CZone* zone = _mapManager.GetZone(player->_zoneId);
            if (zone != nullptr)
            {
                NotifyMoveStop(zone, player, false);  // 본인 포함
            }
        }

        auto phaseT3 = Clock::now();

        // 4) 주기적 위치 동기화 (MOVING 플레이어 → 주변 브로드캐스트)
        ++_syncFrameCount;
        if (_syncFrameCount >= SYNC_INTERVAL_FRAMES)
        {
            _syncFrameCount = 0;
            _mapManager.ForEachZone([&](CZone* zone)
            {
                for (CPlayer* player : zone->GetPlayers())
                {
                    if (player->_moveState != MoveState::MOVING)
                        continue;

                    // 델타 동기화: 마지막 동기화 이후 충분히 이동한 경우만 전송
                    float dx = player->_x - player->_lastSyncX;
                    float dy = player->_y - player->_lastSyncY;
                    if (dx * dx + dy * dy < SYNC_DISTANCE_THRESHOLD_SQ)
                        continue;

                    player->_lastSyncX = player->_x;
                    player->_lastSyncY = player->_y;

                    NotifyMoveSync(zone, player);  // 본인 포함 (이동 중 클라-서버 좌표 드리프트 보정)
                }
            });
        }

#if USE_BROADCAST_BUNDLE
        // [digest] 이번 틱 보류물(이동 번들+채팅)을 수신섹터 기준 연접으로 배포 (아래 flush가 묶어 보냄)
        FlushSectorSends();
#elif USE_SECTOR_AGGREGATION
        // [묶음] 이번 틱 dirty를 섹터별 묶음으로 송신 (RequestSendMsg는 Deferred → 아래 flush가 묶어 보냄)
        FlushSectorUpdates();
#endif

        // [coalescing] 이번 틱에 쌓인 송신을 세션당 1회 WSASend로 flush (broadcast_sync 페이즈에 계상)
        // [계측] flush 구간을 별도 측정 → 비용종류별 "송신(WSASend)" 분리. _phaseBroadcastSyncUs는 그대로 둠(이 값을 포함).
        auto flushT0 = Clock::now();
        _network->FlushPendingSends();
        auto flushT1 = Clock::now();

        auto phaseT4 = Clock::now();

        // 구간별 시간 기록 (마이크로초 누적)
        _monitor._gameLoop._phaseNetworkUs.Add(std::chrono::duration_cast<std::chrono::microseconds>(phaseT2 - phaseT1).count());
        _monitor._gameLoop._phaseGameLogicUs.Add(std::chrono::duration_cast<std::chrono::microseconds>(phaseT3 - phaseT2).count());
        _monitor._gameLoop._phaseBroadcastSyncUs.Add(std::chrono::duration_cast<std::chrono::microseconds>(phaseT4 - phaseT3).count());

        // [계측] 비용종류별 — 틱 내 누적분을 1회씩 원자 반영 후 리셋 (송신은 flush 구간 직접 측정)
        _monitor._gameLoop._broadcastGatherUs.Add(_tickBroadcastGatherUs);
        _monitor._gameLoop._broadcastEnqueueUs.Add(_tickBroadcastEnqueueUs);
        _monitor._gameLoop._flushSendUs.Add(std::chrono::duration_cast<std::chrono::microseconds>(flushT1 - flushT0).count());
        _monitor._gameLoop._membershipSends.Add(_tickMembershipSends);      // 멤버십 변경 복사량(횟수)
        _monitor._gameLoop._membershipCostUs.Add(_tickMembershipUs);        // 멤버십 변경 송신 시간
        _monitor._gameLoop._membershipPairFixes.Add(_tickPairFixes);        // 같은 틱 이동자 쌍 보정 건수
        _monitor._gameLoop._moveBudgetRejects.Add(_tickMoveBudgetRejects);  // 이동 예산 거부 건수
        _tickBroadcastGatherUs = 0;
        _tickBroadcastEnqueueUs = 0;
        _tickMembershipSends = 0;
        _tickMembershipUs = 0;
        _tickPairFixes = 0;
        _tickMoveBudgetRejects = 0;

        // [계측 오버헤드 절감] handle-latency 지역 누적분 → 전역 1회 반영
        _monitor._gameLoop.FlushHandleLatency();

        // 5) 빈 동적 채널 정리
        ++_cleanupFrameCount;
        if (_cleanupFrameCount >= CLEANUP_INTERVAL_FRAMES)
        {
            _cleanupFrameCount = 0;
            _mapManager.CleanupEmptyChannels();
        }

#if USE_DB_WORKER
        // 5-1) DB 주기 저장 — dirty(또는 MOVING) 플레이어만 워커에 enqueue (틱 임계경로 밖 I/O)
        TickPeriodicSave();
#endif

        // 6) Tick 시간 기록 + 프레임 제한
        auto frameEnd = Clock::now();
        double tickMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
        _monitor._gameLoop.RecordTickTime(tickMs);

        int sleepMs = FRAME_INTERVAL_MS - static_cast<int>(tickMs);
        if (sleepMs > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }
}

// ==========================================================================
// 이벤트 디스패치
// ==========================================================================

void CGameServer::ProcessNetworkEvents()
{
    // 프레임 진입 시점 스냅샷을 통째로 스왑 (락 1회). 처리 중 도착분은 다음 프레임으로 이월
    // _localEvents는 직전 프레임에 모두 소비되어 비어 있음 (멤버 재사용으로 deque 블록 보존)
    _network->SwapOutNetworkEvents(_localEvents);

    while (!_localEvents.empty())
    {
        NetworkEvent& event = _localEvents.front();
        switch (event.type)
        {
        case NetworkEvent::Type::CONNECTED:
            OnConnected(event.sessionId);
            break;

        case NetworkEvent::Type::DISCONNECTED:
            OnDisconnected(event.sessionId);
            break;

        case NetworkEvent::Type::RECEIVED:
            OnReceived(event.sessionId, event.pMsg);
            // handle-latency: enqueue(recv) → 처리완료(응답 송신 포함) 시간.
            // 틱 게이트 대기 + 핸들러 비용 = RTT의 서버 기여분. RECEIVED만 측정.
            if (event.enqueueTimeNs != 0)
            {
                int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                _monitor._gameLoop.RecordHandleLatencyLocal(
                    static_cast<double>(nowNs - event.enqueueTimeNs) / 1.0e6);
            }
            break;
        }
        _localEvents.pop();
    }
}

void CGameServer::OnConnected(int64_t sessionId)
{
    // 이미 등록된 세션이면 무시 (중복 접속 방어)
    if (FindPlayer(sessionId) != nullptr)
    {
        _network->RequestDisconnectSession(sessionId);
        return;
    }

    // 기본 맵의 여유 채널에 입장
    CZone* zone = _mapManager.FindOrCreateChannel(_defaultMapId);
    if (zone == nullptr)
    {
        _network->RequestDisconnectSession(sessionId);
        return;
    }

    // 플레이어 생성 + ID/표시 속성 부여 (CGameServer가 생명주기 소유)
    CPlayer* player = new CPlayer();
    player->_playerId = AllocPlayerId();
    player->_displayChar = CalcDisplayChar(player->_playerId);
    player->_colorIndex = CalcColorIndex(player->_playerId);
    if (!zone->EnterZone(player))
    {
        delete player;
        _network->RequestDisconnectSession(sessionId);
        return;
    }

    // 경계 매핑 등록
    player->_sessionId = sessionId;
    _sessionSlots[CSession::ExtractIndex(sessionId)] = player;

    // 델타 동기화 기준 좌표 초기화 (스폰 직후 불필요한 동기화 방지)
    player->_lastSyncX = player->_x;
    player->_lastSyncY = player->_y;

#if USE_DB_WORKER
    // 1단계 임시: playerId를 계정 식별자로 사용 (2단계는 접속 핸드셰이크로 수신)
    player->_accountId = player->_playerId;
    player->_dbDirty   = true;   // 접속 위치를 첫 주기에 저장(신규 행 INSERT 유도)
#endif

    // 1) 존 메타 정보 전송
    SendZoneInfo(player, zone);

    // 2) 본인에게 내 캐릭터 생성
    SendCreateMyPlayer(player);

    // 3) 주변 상호 CREATE 브로드캐스트
    BroadcastEnterZone(zone, player, SpawnReason::CONNECT);
}

void CGameServer::OnDisconnected(int64_t sessionId)
{
    CPlayer* player = FindPlayer(sessionId);
    if (player == nullptr)
        return;
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone != nullptr)
    {
        // 주변 DELETE 브로드캐스트 + 섹터/존 해제
        BroadcastLeaveZone(zone, player);
    }

    // 경계 매핑 해제 (zone 유무와 무관하게 반드시 수행)
    _sessionSlots[CSession::ExtractIndex(sessionId)] = nullptr;

#if USE_DB_WORKER
    // 로그아웃 최종 저장 (스냅샷은 값 복사라 직후 delete 안전)
    if (_dbWorker)
        EnqueuePlayerSave(player);
#endif

    // 플레이어 삭제 (CGameServer가 생명주기 소유)
    delete player;
}

void CGameServer::OnReceived(int64_t sessionId, CSerialBuffer* pMsg)
{
    // [불변식] RECEIVED 생산자는 ParsePackets 한 곳뿐이고, 그 버퍼는 직전에 역참조된 Alloc 결과다.
    assert(pMsg != nullptr);

    // 헤더에서 패킷 타입 읽기
    //   [불변식] PeekData는 항상 성공한다 — RECEIVED 이벤트를 만드는 곳은 ParsePackets 한 군데뿐이고
    //   (IOCPServer.cpp의 PushNetworkEvent(RECEIVED)), 거기 닿으려면 totalPacketSize >= sizeof(MsgHeader)
    //   검증과 그 크기만큼의 적재 성공을 모두 통과해야 하므로 _DataSize >= sizeof(MsgHeader)가 보장된다.
    //   ※ 호출을 assert 안에 넣지 말 것 — header를 채우는 부작용이라, 릴리즈에서 식째로 사라지면
    //     header가 스택 잔여값인 채로 아래 switch를 타 엉뚱한 핸들러가 돈다(원래 결함 그 자체).
    MsgHeader header{};
    const int peekedSize = pMsg->PeekData(reinterpret_cast<char*>(&header), sizeof(header));
    assert(peekedSize == static_cast<int>(sizeof(header)));
    (void)peekedSize;   // 릴리즈(NDEBUG) 미사용 경고 억제

    // 타입별 최소 패킷 크기 검증
    uint16_t expectedSize = GetExpectedSize(header.type);
    if (expectedSize == 0 || header.size < expectedSize)
    {
        _monitor._packetErrors.Inc();
        pMsg->SubRef();
        return;
    }

    // 경계 계층: sessionId → CPlayer* 변환 (이후 컨텐츠 로직은 CPlayer*만 사용)
    CPlayer* player = FindPlayer(sessionId);
    if (player == nullptr)
    {
        pMsg->SubRef();
        return;
    }

    switch (header.type)
    {
    case MsgType::C2S_MOVE_START:
        RecvMoveStart(player, pMsg);
        break;

    case MsgType::C2S_MOVE_STOP:
        RecvMoveStop(player, pMsg);
        break;

    case MsgType::C2S_CHAT:
        RecvChat(player, pMsg);
        break;

    case MsgType::C2S_ZONE_CHANGE:
        RecvZoneChange(player, pMsg);
        break;

    case MsgType::C2S_ADMIN_LOGIN:
        RecvAdminLogin(player, pMsg);
        break;

    default:
        // C2S_HEARTBEAT 등 — 게임 로직 처리 불필요 (타임아웃 갱신은 IOCPServer 수신 시점에서 처리)
        break;
    }

    // SerialBuffer 해제 (소유권 1 반환 — RefCount=0이면 프리리스트로)
    pMsg->SubRef();
}

uint16_t CGameServer::GetExpectedSize(MsgType type)
{
    switch (type)
    {
    case MsgType::C2S_MOVE_START:   return sizeof(MSG_C2S_MOVE_START);
    case MsgType::C2S_MOVE_STOP:    return sizeof(MSG_C2S_MOVE_STOP);
    case MsgType::C2S_CHAT:         return sizeof(MsgHeader) + sizeof(ChatChar); // 가변 길이: 최소 1글자
    case MsgType::C2S_ZONE_CHANGE:  return sizeof(MSG_C2S_ZONE_CHANGE);
    case MsgType::C2S_HEARTBEAT:    return sizeof(MSG_C2S_HEARTBEAT);
    case MsgType::C2S_ADMIN_LOGIN:  return sizeof(MSG_C2S_ADMIN_LOGIN);
    default:                        return 0;
    }
}

// ==========================================================================
// 패킷 핸들러
// ==========================================================================

void CGameServer::RecvMoveStart(CPlayer* player, CSerialBuffer* pMsg)
{
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    MSG_C2S_MOVE_START recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // Direction 범위 검증 (4방향)
    if (recvMsg.direction < static_cast<uint8_t>(Direction::UP) ||
        recvMsg.direction > static_cast<uint8_t>(Direction::RIGHT))
        return;

    Direction dir = static_cast<Direction>(recvMsg.direction);

    // 이동 중 방향 전환: 같은 방향이면 무시, 다른 방향이면 갱신 + 재브로드캐스트
    // ※ 좌표 수용보다 먼저 검사 — MOVING 중 반복 전송으로 서버 좌표를 밀어내는 치트 방지
    if (player->_moveState == MoveState::MOVING)
    {
        if (dir == player->_direction)
            return;

        // 벽 방향 검증: 벽 위치에서 벽 쪽으로 방향 전환 시 정지 처리
        if (IsBlockedByWall(zone, player, dir))
        {
            player->_moveState = MoveState::IDLE;
            player->_direction = dir;

#if USE_DB_WORKER
            player->_dbDirty = true;   // MOVING → IDLE 전이(주기저장 MOVING 조건에서 빠지므로 최종위치 마킹)
#endif

            NotifyMoveStop(zone, player, false);  // 본인 포함
            return;
        }

        player->_direction = dir;
        player->_lastSyncX = player->_x;
        player->_lastSyncY = player->_y;

        NotifyMoveStart(zone, player);
        return;
    }

    // 클라이언트 예측 좌표 수용 (IDLE → MOVING 전환 시에만):
    // 서버 좌표와의 오차가 허용 범위 내이고, 초당 이동량 예산이 남아 있으면 채택.
    //   거리 임계값만으로는 1회 한도밖에 못 막는다 — STOP/START를 연타하면 전이마다 다시 8타일이
    //   수용되므로, 시간에 비례해 적립되는 예산을 함께 봐야 자기 속도 이상으로 못 움직인다.
    {
        float dx = recvMsg.x - player->_x;
        float dy = recvMsg.y - player->_y;
        const float distSq = dx * dx + dy * dy;
        if (distSq <= MOVE_START_ACCEPT_DIST_SQ && ConsumeMoveBudget(player, distSq))
        {
            // 맵 경계 클램핑 후 수용
            float acceptX = recvMsg.x;
            float acceptY = recvMsg.y;
            float mapW = static_cast<float>(zone->GetMapWidth());
            float mapH = static_cast<float>(zone->GetMapHeight());
            if (acceptX < 0.0f)    acceptX = 0.0f;
            if (acceptX >= mapW)   acceptX = mapW - 1.0f;
            if (acceptY < 0.0f)    acceptY = 0.0f;
            if (acceptY >= mapH)   acceptY = mapH - 1.0f;
            player->_x = acceptX;
            player->_y = acceptY;

#if USE_DB_WORKER
            player->_dbDirty = true;   // 클라 예측 좌표 채택 → 위치 변경, 저장 대상
#endif

            // 좌표 수용으로 섹터가 변경되었을 수 있으므로 재계산
            int32_t newSectorX = zone->GetSectorManager().CalcSectorX(player->_x);
            int32_t newSectorY = zone->GetSectorManager().CalcSectorY(player->_y);
            if (newSectorX != player->_sectorX || newSectorY != player->_sectorY)
            {
                int32_t oldSectorX = player->_sectorX;
                int32_t oldSectorY = player->_sectorY;

                zone->GetSectorManager().RemovePlayer(player, oldSectorX, oldSectorY);
                player->_sectorX = newSectorX;
                player->_sectorY = newSectorY;
                zone->GetSectorManager().AddPlayer(player, newSectorX, newSectorY);

                PushSectorChange(player, oldSectorX, oldSectorY);
            }
        }
        // 범위 초과 또는 예산 부족 시 서버 좌표 유지 (치트 방지)
    }

    // 벽 방향 검증: 벽 위치에서 벽 쪽으로 이동 시도 시 차단
    if (IsBlockedByWall(zone, player, dir))
    {
        SendSyncPosition(player);        // 본인 — 서버 권위 좌표로 되돌림
        NotifyMoveSync(zone, player);    // 주변 — 위에서 채택한 좌표를 전파.
                                         //   이게 없으면 상태가 IDLE이라 주기 동기화(MOVING 대상)에도
                                         //   틱 끝 묶음(dirty 대상)에도 안 실려, 주변은 최대 8타일 떨어진
                                         //   옛 위치를 계속 렌더한다.
        return;
    }

    // 플레이어 상태 갱신
    player->_direction = dir;
    player->_moveState = MoveState::MOVING;

    // 델타 동기화 기준 좌표 갱신 (이동 시작 직후 즉시 동기화 방지)
    player->_lastSyncX = player->_x;
    player->_lastSyncY = player->_y;

    // 주변에 MOVE_START 브로드캐스트 (묶음 모드: dirty 마킹만)
    NotifyMoveStart(zone, player);
}

void CGameServer::RecvMoveStop(CPlayer* player, CSerialBuffer* pMsg)
{
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    MSG_C2S_MOVE_STOP recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // 이동 중이 아니면 무시 (서버 벽 클램핑으로 이미 IDLE 처리됨)
    if (player->_moveState != MoveState::MOVING)
        return;

    // Direction 범위 검증 (4방향)
    if (recvMsg.direction < static_cast<uint8_t>(Direction::UP) ||
        recvMsg.direction > static_cast<uint8_t>(Direction::RIGHT))
        return;

    // 서버 권위 모델: 서버 좌표 유지, 상태만 변경
    player->_direction = static_cast<Direction>(recvMsg.direction);
    player->_moveState = MoveState::IDLE;

#if USE_DB_WORKER
    player->_dbDirty = true;   // 정지 → IDLE 전이(주기저장 MOVING 조건에서 빠지므로 최종위치 마킹)
#endif

    // 섹터 이동 판정
    int32_t newSectorX = zone->GetSectorManager().CalcSectorX(player->_x);
    int32_t newSectorY = zone->GetSectorManager().CalcSectorY(player->_y);

    if (newSectorX != player->_sectorX || newSectorY != player->_sectorY)
    {
        int32_t oldSectorX = player->_sectorX;
        int32_t oldSectorY = player->_sectorY;

        // 섹터 데이터 갱신
        zone->GetSectorManager().RemovePlayer(player, oldSectorX, oldSectorY);
        player->_sectorX = newSectorX;
        player->_sectorY = newSectorY;
        zone->GetSectorManager().AddPlayer(player, newSectorX, newSectorY);

        // 시야 진입/이탈 — 대기열에 추가 (틱 끝에 배치 처리)
        PushSectorChange(player, oldSectorX, oldSectorY);
    }

    // 주변에 MOVE_STOP 브로드캐스트 (묶음 모드: dirty 마킹만)
    NotifyMoveStop(zone, player, true);
}

void CGameServer::RecvChat(CPlayer* player, CSerialBuffer* pMsg)
{
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    // 가변 길이 수신: header.size 기반으로 실제 메시지 길이 역산
    MsgHeader header{};
    pMsg->PeekData(reinterpret_cast<char*>(&header), sizeof(header));

    uint16_t recvSize = header.size;
    if (recvSize > sizeof(MSG_C2S_CHAT))
        recvSize = sizeof(MSG_C2S_CHAT);  // 최대 크기 제한

    MSG_C2S_CHAT recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), recvSize);

    // 메시지 길이 계산 (바이트 → wchar_t 글자 수)
    uint16_t msgBytes = recvSize - sizeof(MsgHeader);
    msgBytes -= msgBytes % sizeof(ChatChar);  // 문자 경계 정렬 (홀수 바이트 방어)
    uint16_t msgLen = msgBytes / sizeof(ChatChar);
    if (msgLen > CHAT_MSG_MAX_LEN - 1)
        msgLen = CHAT_MSG_MAX_LEN - 1;
    recvMsg.message[msgLen] = L'\0';  // null 종단 보장

#if USE_BROADCAST_BUNDLE
    // [digest] 즉시 브로드캐스트 대신 보류 등록 — 틱 끝 digest에 합류 (실송신 시점은 기존과 동일한 틱 끝 flush).
    // 소스 섹터는 수신 시점 좌표로 고정 → 기존(즉시 배포)의 AOI 의미 보존.
    RegisterSectorItem(zone, player->_zoneId, player->_sectorX, player->_sectorY,
        MakeChat(player->_playerId, player->_displayChar, player->_colorIndex,
            recvMsg.message, msgLen));
#else
    // 본인 포함 브로드캐스트 (송신 함수가 소유권 소비)
    BroadcastAroundSector(zone, player,
        MakeChat(player->_playerId, player->_displayChar, player->_colorIndex,
            recvMsg.message, msgLen), false);
#endif
}

// ==========================================================================
// 패킷 전송 추상화
// ==========================================================================

// ==========================================================================
// 계층 경계 헬퍼
// ==========================================================================

int32_t CGameServer::AllocPlayerId()
{
    return _nextPlayerId++;
}

// playerId → 고유 표시 문자 (A-Z, a-z, 0-9 = 62종)
uint8_t CGameServer::CalcDisplayChar(int32_t playerId)
{
    static constexpr char CHARS[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    return static_cast<uint8_t>(CHARS[static_cast<uint32_t>(playerId) % 62]);
}

// playerId → 고유 색상 인덱스 (0-6, 7종)
uint8_t CGameServer::CalcColorIndex(int32_t playerId)
{
    return static_cast<uint8_t>((static_cast<uint32_t>(playerId) / 62) % 7);
}

// ── 패킷 전송 추상화 ──
// 단일 플레이어에게 패킷 전송 (CSerialBuffer — 소유권 1을 소비)
// 빌더가 반환한 RefCount=1 버퍼를 넘기면, 송신 후 SubRef로 소유권을 회수한다.
void CGameServer::SendPacket(CPlayer* target, CSerialBuffer* pMsg)
{
    if (target->_sessionId != -1)
    {
        pMsg->AddRef();   // 송신용 소유권 — RequestSendMsg(CSerialBuffer*)가 소비
        // 게임루프 송신은 묶어 보낼 수 있음을 표시(Deferred) — 실제 지연 여부는 USE_SEND_COALESCING이 결정
        const int dataSize = pMsg->GetDataSize();
        if (_network->RequestSendMsg(target->_sessionId, pMsg, SendFlush::Deferred))
        {
            _monitor._sendPackets.Inc();
            _monitor._sendEnqueuedBytes.Add(static_cast<int64_t>(dataSize));
        }
    }
    pMsg->SubRef();       // 빌더가 넘긴 소유권 1 회수 (세션 무효여도 안전 회수)
}

// 주변 브로드캐스트 (CSerialBuffer — 빌더 RefCount=1 버퍼를 소비)
void CGameServer::BroadcastAroundSector(CZone* zone, CPlayer* player, CSerialBuffer* pMsg, bool excludeSelf)
{
    // [계측] 비용종류별 — gather(주변 모으기) / enqueue(수신자별 복사) 구간을 호출당 2~3회 now()로 분리.
    //   per-call 측정이라 오버헤드가 수신자 수가 아닌 호출 수에 비례. 누적은 틱 끝 1회만 원자반영.
    auto _measGatherT0 = std::chrono::steady_clock::now();

    _broadcastBuffer.clear();
    CPlayer* exclude = excludeSelf ? player : nullptr;
    zone->GetSectorManager().GetAroundPlayers(
        player->_sectorX, player->_sectorY, _broadcastBuffer, exclude);

    auto _measGatherT1 = std::chrono::steady_clock::now();
    _tickBroadcastGatherUs += std::chrono::duration_cast<std::chrono::microseconds>(
        _measGatherT1 - _measGatherT0).count();

    _monitor._gameLoop._broadcastCalls.Inc();
    _monitor._gameLoop._broadcastTargets.Add(static_cast<int64_t>(_broadcastBuffer.size()));

    // 유효 타겟(세션 보유) 수 선카운트 → 타겟별 AddRef를 1회 배치 AddRef로 압축 (원자연산 N→1)
    // 단일 게임루프 스레드 내 호출이라 두 패스 사이 _sessionId 변동 없음 → 카운트 정합 보장
    size_t validCount = 0;
    for (CPlayer* other : _broadcastBuffer)
    {
        if (other->_sessionId != -1)
            ++validCount;
    }

    if (validCount > 0)
        pMsg->AddRef(static_cast<int64_t>(validCount));   // 타겟별 소유권 일괄 확보

    // 송신 메트릭은 타겟별 원자증가 대신 성공분을 지역 누적 후 1회 반영 (원자연산 N→1)
    const int dataSize = pMsg->GetDataSize();
    size_t sentPkts = 0;
    for (CPlayer* other : _broadcastBuffer)
    {
        if (other->_sessionId == -1)
            continue;
        if (_network->RequestSendMsg(other->_sessionId, pMsg, SendFlush::Deferred))
            ++sentPkts;
    }
    if (sentPkts > 0)
    {
        _monitor._sendPackets.Add(static_cast<int64_t>(sentPkts));
        _monitor._sendEnqueuedBytes.Add(static_cast<int64_t>(sentPkts) * dataSize);
    }
    pMsg->SubRef();   // 빌더가 넘긴 소유권 1 회수 (타겟 0명이어도 안전 회수)

    // [계측] enqueue 구간 = gather 직후 ~ 여기 (precount + 배치 AddRef + 수신자별 RequestSendMsg 복사 + 메트릭).
    //   복사가 곧 USE_LOCKFREE_SENDQ 토글이 없애려는 비용 → A/B로 이 값의 변화를 본다.
    auto _measEnqueueT1 = std::chrono::steady_clock::now();
    _tickBroadcastEnqueueUs += std::chrono::duration_cast<std::chrono::microseconds>(
        _measEnqueueT1 - _measGatherT1).count();
}

// ── 이동 알림 헬퍼 ──
// 묶음 모드(USE_SECTOR_AGGREGATION)면 dirty 마킹만, 아니면 즉시 주변 브로드캐스트.
// 호출부의 #if/#else 산재를 이 3개로 흡수 (OFF 경로 바이트·동작 불변).
void CGameServer::NotifyMoveStart(CZone* zone, CPlayer* player)
{
#if USE_SECTOR_AGGREGATION
    MarkMoveDirty(player);
#else
    BroadcastAroundSector(zone, player,
        MakeMoveStart(player->_playerId, static_cast<uint8_t>(player->_direction),
            player->_x, player->_y));
#endif
}

void CGameServer::NotifyMoveStop(CZone* zone, CPlayer* player, bool excludeSelf)
{
#if USE_SECTOR_AGGREGATION
    MarkMoveDirty(player);
#else
    BroadcastAroundSector(zone, player,
        MakeMoveStop(player->_playerId, static_cast<uint8_t>(player->_direction),
            player->_x, player->_y), excludeSelf);
#endif
}

void CGameServer::NotifyMoveSync(CZone* zone, CPlayer* player)
{
#if USE_SECTOR_AGGREGATION
    MarkMoveDirty(player);
#else
    BroadcastAroundSector(zone, player,
        MakeSyncPosition(player->_playerId, player->_x, player->_y), false);
#endif
}

void CGameServer::SendZoneInfo(CPlayer* target, CZone* zone)
{
    SendPacket(target, MakeZoneInfo(zone));
}

void CGameServer::SendCreateMyPlayer(CPlayer* target)
{
    SendPacket(target, MakeCreateMyPlayer(target));
}

void CGameServer::SendCreateOtherPlayer(CPlayer* target, CPlayer* player, SpawnReason reason)
{
    ++_tickMembershipSends;   // 멤버십 변경 복사 집계 (BroadcastAroundSector 밖 경로)
    SendPacket(target, MakeCreateOtherPlayer(player, reason));
}

void CGameServer::SendDeletePlayer(CPlayer* target, CPlayer* player)
{
    ++_tickMembershipSends;   // 멤버십 변경 복사 집계 (BroadcastAroundSector 밖 경로)
    SendPacket(target, MakeDeletePlayer(player->_playerId));
}

#if USE_MEMBERSHIP_INBOUND_BUNDLE
// [Phase 2] 인바운드 멤버십 배치 송신 — 상대 목록을 배치 상한씩 잘라 mover 1명에게 송신.
// _membershipSends는 OFF의 개별 SendXxx 호출당 1과 동일하게 엔트리당 1 집계 (A/B 통제지표 불변).
// count=0이면 아무것도 안 보냄 — OFF의 빈 루프와 동일.
void CGameServer::SendCreatePlayerBatch(CPlayer* target, CPlayer* const* others, int count)
{
    _tickMembershipSends += count;
    for (int i = 0; i < count; i += CREATE_PLAYER_BATCH_MAX_ENTRIES)
    {
        int chunk = (std::min)(count - i, CREATE_PLAYER_BATCH_MAX_ENTRIES);
        SendPacket(target, MakeCreatePlayerBatch(others + i, chunk));
    }
}

void CGameServer::SendDeletePlayerBatch(CPlayer* target, CPlayer* const* others, int count)
{
    _tickMembershipSends += count;
    for (int i = 0; i < count; i += DELETE_PLAYER_BATCH_MAX_ENTRIES)
    {
        int chunk = (std::min)(count - i, DELETE_PLAYER_BATCH_MAX_ENTRIES);
        SendPacket(target, MakeDeletePlayerBatch(others + i, chunk));
    }
}
#endif // USE_MEMBERSHIP_INBOUND_BUNDLE

void CGameServer::SendSyncPosition(CPlayer* target)
{
    SendPacket(target, MakeSyncPosition(target->_playerId, target->_x, target->_y));
}

void CGameServer::RecvZoneChange(CPlayer* player, CSerialBuffer* pMsg)
{
    int64_t sessionId = player->_sessionId;
    if (sessionId == -1)
        return;

    CZone* oldZone = _mapManager.GetZone(player->_zoneId);
    if (oldZone == nullptr)
        return;

    MSG_C2S_ZONE_CHANGE recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    int32_t targetMapId = recvMsg.targetMapId;
    int32_t targetChannelIndex = recvMsg.targetChannelIndex;

    CZone* newZone = nullptr;

    if (targetChannelIndex >= 0)
    {
        // ── 채널 지정 이동 (같은 맵 내) ──
        int32_t currentMapId = CMapManager::GetMapIdFromZoneId(oldZone->GetZoneId());

        newZone = _mapManager.FindChannel(currentMapId, targetChannelIndex);
        if (newZone == nullptr)
        {
            SendZoneChangeFail(player, 0);  // 채널 없음
            return;
        }
        if (newZone->GetZoneId() == oldZone->GetZoneId())
        {
            SendZoneChangeFail(player, 2);  // 이미 해당 채널
            return;
        }

        // 인원 제한 체크 (admin은 스킵)
        if (!player->_isAdmin)
        {
            int32_t maxPlayers = _mapManager.GetMaxPlayersPerChannel(currentMapId);
            if (maxPlayers > 0 && newZone->GetPlayerCount() >= maxPlayers)
            {
                SendZoneChangeFail(player, 1);  // 채널 가득 참
                return;
            }
        }

        targetMapId = currentMapId;
    }
    else
    {
        // ── 기존 맵 이동 (자동 채널 배정) ──

        // 랜덤 맵 이동 요청 처리
        if (targetMapId == -1)
        {
            int32_t currentMapId = CMapManager::GetMapIdFromZoneId(oldZone->GetZoneId());
            targetMapId = _mapManager.GetRandomMapId(currentMapId);
            if (targetMapId == -1)
            {
                SendZoneChangeFail(player, 0);
                return;
            }
        }

        newZone = _mapManager.FindOrCreateChannel(targetMapId, player->_isAdmin);
        if (newZone == nullptr)
        {
            SendZoneChangeFail(player, 0);
            return;
        }

        // 자동배정이 "지금 있는 존"을 그대로 돌려준 경우 — 채널 지정 분기(reason 2)와 동일하게 거부.
        //   통과시키면 아래 LeaveZone→EnterZone의 CalcSpawnPos가 좌표를 맵 전역 난수로 재추첨해,
        //   요청만으로 순간이동이 된다(MOVE_START 이동량 예산 우회).
        if (newZone->GetZoneId() == oldZone->GetZoneId())
        {
            SendZoneChangeFail(player, 2);  // 이미 해당 채널
            return;
        }
    }

    // ── 현재 존에서 퇴장 ──
    BroadcastLeaveZone(oldZone, player);

    // ── 새 존에 입장 (player 객체 재활용, playerId 유지) ──

    if (!newZone->EnterZone(player))
    {
        // 입장 실패 시 원래 맵의 여유 채널로 복귀 시도
        CZone* fallback = _mapManager.FindOrCreateChannel(CMapManager::GetMapIdFromZoneId(oldZone->GetZoneId()));
        if (fallback != nullptr && fallback->EnterZone(player))
        {
            SendZoneChangeFail(player, 1);

            // 델타 동기화 기준 좌표 초기화
            player->_lastSyncX = player->_x;
            player->_lastSyncY = player->_y;

            // 복귀한 존에서 본인 + 주변 상호 통보
            SendZoneInfo(player, fallback);
            SendCreateMyPlayer(player);
            BroadcastEnterZone(fallback, player, SpawnReason::NORMAL);

            return;
        }

        // 복귀도 실패 → 좀비 방지를 위해 연결 해제
        _sessionSlots[CSession::ExtractIndex(sessionId)] = nullptr;
        delete player;
        _network->RequestDisconnectSession(sessionId);
        return;
    }

    _monitor._gameLoop._zoneChangeCount.Inc();

#if USE_DB_WORKER
    player->_dbDirty = true;   // 존/맵 이동 → 위치·map_id 변경, 저장 대상
#endif

    // 델타 동기화 기준 좌표 초기화
    player->_lastSyncX = player->_x;
    player->_lastSyncY = player->_y;

    // 존 메타 정보 + 존 이동 성공 통보
    SendZoneInfo(player, newZone);
    int32_t channelIndex = CMapManager::GetChannelIndexFromZoneId(newZone->GetZoneId());
    SendZoneChangeOk(player, targetMapId, channelIndex);

    // 주변 상호 CREATE 브로드캐스트
    BroadcastEnterZone(newZone, player, SpawnReason::ZONE_TRANSFER);
}

// ── 운영자 인증 ──

// 부하 테스트 도구용 고정 키 — 실서비스 인증 아님.
// 와이어에 평문으로 흐르고 해시·시도 제한도 없다. admin 권한 효과는 채널 인원 제한 우회뿐(CMapInstance::FindOrCreateChannel).
// 실계정 연동 단계에서 계정 DB 조회 기반 인증으로 교체한다.
static constexpr char ADMIN_KEY[] = "admin1234";

void CGameServer::RecvAdminLogin(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_ADMIN_LOGIN recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // null-terminate 보장
    recvMsg.key[ADMIN_KEY_MAX_LEN - 1] = '\0';

    if (strcmp(recvMsg.key, ADMIN_KEY) == 0)
    {
        player->_isAdmin = true;
        SendPacket(player, MakeAdminLoginOk());
    }
    else
    {
        SendPacket(player, MakeAdminLoginFail());
    }
}

void CGameServer::SendZoneChangeOk(CPlayer* target, int32_t mapId, int32_t channelIndex)
{
    SendPacket(target, MakeZoneChangeOk(target, mapId, channelIndex));
}

void CGameServer::SendZoneChangeFail(CPlayer* target, uint8_t reason)
{
    SendPacket(target, MakeZoneChangeFail(reason));
}

// ==========================================================================
// 벽 방향 검증 — 경계 위치에서 벽 쪽 이동 차단 (클라이언트 IsBlockedByWall과 동일)
// ==========================================================================

bool CGameServer::IsBlockedByWall(CZone* zone, CPlayer* player, Direction dir)
{
    float maxX = static_cast<float>(zone->GetMapWidth()) - 1.0f;
    float maxY = static_cast<float>(zone->GetMapHeight()) - 1.0f;

    bool atLeft   = (player->_x <= 0.0f);
    bool atRight  = (player->_x >= maxX);
    bool atTop    = (player->_y <= 0.0f);
    bool atBottom = (player->_y >= maxY);

    switch (dir)
    {
    case Direction::LEFT:  return atLeft;
    case Direction::RIGHT: return atRight;
    case Direction::UP:    return atTop;
    case Direction::DOWN:  return atBottom;
    default: return false;
    }
}

// C2S_MOVE_START 좌표 수용의 이동량 예산 — 경과분을 적립하고, 통과하면 소비까지 수행.
//   적립: 마지막 검사 이후 흐른 프레임 × 자기 속도 × 여유계수. 상한이 1회 수용 한도(8타일)와 같아
//         정상 클라는 예산이 늘 만충이다(서버가 이동을 직접 계산하므로 예측 오차 소모가 거의 없음).
//   소비: 실제 수용한 거리만큼. 연타해도 장기 평균은 적립 속도(자기 속도 × 여유계수)로 수렴한다.
bool CGameServer::ConsumeMoveBudget(CPlayer* player, float distSq)
{
    const uint64_t elapsedFrames = _frameCount - player->_moveBudgetFrame;
    if (elapsedFrames > 0)
    {
        player->_moveBudgetFrame = _frameCount;
        const float elapsedSec = static_cast<float>(elapsedFrames) * (FRAME_INTERVAL_MS / 1000.0f);
        player->_moveBudget += static_cast<float>(player->_speed) * MOVE_BUDGET_SLACK * elapsedSec;
        if (player->_moveBudget > MOVE_BUDGET_CAP)
            player->_moveBudget = MOVE_BUDGET_CAP;
    }

    const float dist = std::sqrt(distSq);
    if (dist > player->_moveBudget)
    {
        ++_tickMoveBudgetRejects;
        return false;
    }

    player->_moveBudget -= dist;
    return true;
}

// 두 섹터가 서로의 시야(주변 9섹터) 안인가 — 체비쇼프 거리 ≤ 1.
//   GetAroundSectorList(SectorManager.cpp:166)가 dx,dy ∈ [-1,1]을 모으는 것과 같은 판정을
//   섹터 목록을 만들지 않고 좌표만으로 계산한다(맵 밖 섹터는 애초에 주민이 없어 결과 동일).
//   ※ 섹터 좌표는 존마다 별개 격자이므로, 호출부에서 반드시 같은 존인지 먼저 확인할 것.
namespace
{
    inline bool IsSectorAdjacent(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
    {
        int32_t dx = x1 - x2;
        int32_t dy = y1 - y2;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        return dx <= 1 && dy <= 1;
    }
}

// ==========================================================================
// 존 입장/퇴장 브로드캐스트
// ==========================================================================

void CGameServer::BroadcastEnterZone(CZone* zone, CPlayer* player, SpawnReason reason)
{
    _eventAroundBuffer.clear();
    zone->GetSectorManager().GetAroundPlayers(
        player->_sectorX, player->_sectorY, _eventAroundBuffer, player);

    for (CPlayer* other : _eventAroundBuffer)
    {
        SendCreateOtherPlayer(other, player, reason);  // 기존 플레이어에게 신규 플레이어 생성
        SendCreateOtherPlayer(player, other);           // 신규 플레이어에게 기존 플레이어 생성
    }
}

void CGameServer::BroadcastLeaveZone(CZone* zone, CPlayer* player)
{
    // 현재 섹터 기준 주변 DELETE
    _eventAroundBuffer.clear();
    zone->GetSectorManager().GetAroundPlayers(
        player->_sectorX, player->_sectorY, _eventAroundBuffer, player);

    for (CPlayer* other : _eventAroundBuffer)
    {
        SendDeletePlayer(other, player);
    }

    // 미처리 섹터 변경이 있으면 이전 섹터 전용 뷰어에게도 DELETE
    // (같은 프레임에 섹터 변경 + 존 이동/접속해제 시 고스트 방지)
    if (_sectorChangedSet.count(player))
    {
        for (const auto& change : _pendingSectorChanges)
        {
            if (change.player != player)
                continue;

            // 이전 섹터 전용 = old 주변에만 있고 현재 주변에는 없는 섹터
            CSectorManager::SectorPos added[CSectorManager::MAX_AROUND_SECTORS];
            CSectorManager::SectorPos removed[CSectorManager::MAX_AROUND_SECTORS];
            int32_t addedCount = 0;
            int32_t removedCount = 0;

            zone->GetSectorManager().GetSectorDiff(
                player->_sectorX, player->_sectorY,    // 현재 섹터 (이미 갱신된 값)
                change.oldSectorX, change.oldSectorY,   // 이전 섹터
                added, addedCount,                       // old 전용 섹터
                removed, removedCount);

            for (int32_t i = 0; i < addedCount; ++i)
            {
                const auto& players = zone->GetSectorManager().GetSectorPlayers(
                    added[i].x, added[i].y);
                for (CPlayer* other : players)
                {
                    SendDeletePlayer(other, player);
                }
            }
            break;
        }
    }

#if USE_SECTOR_AGGREGATION
    // 섹터 묶음 dirty에서도 제거 (틱 끝 FlushSectorUpdates dangling 방지 — 같은 틱 move→퇴장/존이동)
    if (player->_moveDirty)
    {
        player->_moveDirty = false;
        _sectorOutbox.dirtyMovers.erase(std::remove(_sectorOutbox.dirtyMovers.begin(), _sectorOutbox.dirtyMovers.end(), player),
            _sectorOutbox.dirtyMovers.end());
    }
#endif

    // 같은 틱에 먼저 이동해 위 두 조회에서 모두 빠진 이동자에게도 DELETE (치유 불가 유령 방지)
    //   위 조회는 (1) 퇴장자의 현재 섹터 주변, (2) 퇴장자의 이전 섹터 전용 뷰어 — 둘 다 이동자의
    //   "이동 후" 위치로만 걸린다. 퇴장 전엔 퇴장자를 보고 있었지만 같은 틱에 서로 멀어진 이동자는
    //   어느 쪽에도 안 걸리고, 퇴장자는 곧 사라지므로 나중에 치유될 통로조차 없다.
    {
        int32_t leaverOldX = player->_sectorX;
        int32_t leaverOldY = player->_sectorY;
        if (_sectorChangedSet.count(player))
        {
            for (const auto& change : _pendingSectorChanges)
            {
                if (change.player == player)
                {
                    leaverOldX = change.oldSectorX;
                    leaverOldY = change.oldSectorY;
                    break;
                }
            }
        }

        for (const auto& change : _pendingSectorChanges)
        {
            CPlayer* mover = change.player;
            if (mover == player || mover->_zoneId != player->_zoneId)
                continue;   // 대기열은 서버 전역 — 섹터 좌표는 존마다 별개 격자라 같은 존만 비교
            if (change.oldSectorX == mover->_sectorX && change.oldSectorY == mover->_sectorY)
                continue;   // 원위치 복귀 — 시야 변화 없음

            // 이동 전엔 퇴장자를 보고 있었는데(클라가 들고 있음), 위 두 조회 어디에도 안 걸리는 경우만
            if (IsSectorAdjacent(change.oldSectorX, change.oldSectorY, leaverOldX, leaverOldY) &&
                !IsSectorAdjacent(mover->_sectorX, mover->_sectorY, player->_sectorX, player->_sectorY) &&
                !IsSectorAdjacent(mover->_sectorX, mover->_sectorY, leaverOldX, leaverOldY))
            {
                SendDeletePlayer(mover, player);
                ++_tickPairFixes;
            }
        }
    }

    // 섹터 변경 대기열에서 제거
    _sectorChangedSet.erase(player);
    _pendingSectorChanges.erase(
        std::remove_if(_pendingSectorChanges.begin(), _pendingSectorChanges.end(),
            [player](const SectorChangeInfo& c) { return c.player == player; }),
        _pendingSectorChanges.end());

    zone->LeaveZone(player);
}

// ==========================================================================
// 섹터 변경 대기열 삽입 — 같은 플레이어는 최초 출발 섹터만 기록
// ==========================================================================

void CGameServer::PushSectorChange(CPlayer* player, int32_t oldSectorX, int32_t oldSectorY)
{
    if (!_sectorChangedSet.insert(player).second)
        return;  // 이미 기록됨 → 최초 출발 섹터 유지

    _pendingSectorChanges.push_back({ player, oldSectorX, oldSectorY });
}

// ==========================================================================
// 같은 틱 이동자 쌍 보정
//
// [왜 필요한가]
//   섹터 점유는 이동 즉시 갱신되지만(Zone.cpp:120-123, RecvMoveStart/Stop) 통보는 틱 끝에 몰아서
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

void CGameServer::FixSameTickMoverPairs()
{
    const size_t count = _pendingSectorChanges.size();
    if (count < 2)
        return;

    for (size_t i = 0; i < count; ++i)
    {
        CPlayer* a = _pendingSectorChanges[i].player;
        const int32_t aOldX = _pendingSectorChanges[i].oldSectorX;
        const int32_t aOldY = _pendingSectorChanges[i].oldSectorY;
        if (aOldX == a->_sectorX && aOldY == a->_sectorY)
            continue;   // 원위치 복귀 — 틱 끝 루프의 skip 조건과 동일

        for (size_t j = i + 1; j < count; ++j)
        {
            CPlayer* b = _pendingSectorChanges[j].player;
            if (a->_zoneId != b->_zoneId)
                continue;   // 대기열은 서버 전역 — 섹터 좌표는 존마다 별개 격자라 같은 존만 비교

            const int32_t bOldX = _pendingSectorChanges[j].oldSectorX;
            const int32_t bOldY = _pendingSectorChanges[j].oldSectorY;
            if (bOldX == b->_sectorX && bOldY == b->_sectorY)
                continue;

            const bool wasVisible = IsSectorAdjacent(aOldX, aOldY, bOldX, bOldY);
            const bool isVisible  = IsSectorAdjacent(a->_sectorX, a->_sectorY, b->_sectorX, b->_sectorY);
            if (wasVisible == isVisible)
                continue;   // 시야 변화 없음 → 통보 자체가 불필요

            const bool aPassHits = IsSectorAdjacent(aOldX, aOldY, b->_sectorX, b->_sectorY);
            const bool bPassHits = IsSectorAdjacent(a->_sectorX, a->_sectorY, bOldX, bOldY);

            if (!isVisible)
            {
                // 보였다가 안 보임 — 두 패스 다 못 잡았을 때만 보충
                if (!aPassHits && !bPassHits)
                {
                    SendDeletePlayer(a, b);
                    SendDeletePlayer(b, a);
                    ++_tickPairFixes;
                }
            }
            else
            {
                // 안 보이다가 보임 — 두 패스 다 못 잡았을 때만 보충
                if (aPassHits && bPassHits)
                {
                    SendCreateOtherPlayer(a, b);
                    SendCreateOtherPlayer(b, a);
                    ++_tickPairFixes;
                }
            }
        }
    }
}

// ==========================================================================
// 섹터 변경 브로드캐스트
// ==========================================================================

void CGameServer::ProcessSectorChange(CZone* zone, CPlayer* player,
                                      int32_t oldSectorX, int32_t oldSectorY)
{
    CSectorManager::SectorPos added[CSectorManager::MAX_AROUND_SECTORS];
    CSectorManager::SectorPos removed[CSectorManager::MAX_AROUND_SECTORS];
    int32_t addedCount = 0;
    int32_t removedCount = 0;
    zone->GetSectorManager().GetSectorDiff(
        oldSectorX, oldSectorY,
        player->_sectorX, player->_sectorY,
        added, addedCount, removed, removedCount);

#if USE_MEMBERSHIP_FANOUT_DEDUP
    // [Phase 1] 아웃바운드(나→상대, 수신자마다 내용 동일)는 1회 빌드 후 팬아웃,
    //           인바운드(상대→나, 수신자마다 내용 제각각)는 개별 송신 유지.
    //           수신자별 순서·수신자·횟수는 OFF와 동일 — 재빌드(Alloc+직렬화) 반복만 제거.

    // 이탈 섹터 — 상호 DELETE
    if (removedCount > 0)
    {
#if USE_MEMBERSHIP_DIGEST
        // [Phase 4] 아웃바운드: "나를 삭제" 1개 빌드 → 이탈 섹터들에 직송 등록 (주민 열람 없음, 배포는 틱 끝 digest)
        //           player는 이탈 섹터에 없어(현재 섹터는 새 AOI 소속) exclude 불필요 = 폴백 경로 없음
        RegisterOutboundToSectors(zone, player->_zoneId, removed, removedCount,
                                  MakeDeletePlayer(player->_playerId), nullptr);
#else
        // 아웃바운드: "나를 삭제" 1개 빌드 → 이탈 섹터 전원에게 팬아웃 (player는 이탈 섹터에 없어 exclude 불필요)
        FanoutToSectors(zone, removed, removedCount, MakeDeletePlayer(player->_playerId), nullptr);
#endif

#if USE_MEMBERSHIP_INBOUND_BUNDLE
        // [Phase 2] 인바운드: 수신자가 mover 1명 → 상대들을 배치 패킷(상한 초과 시 청크 분할)으로 접음.
        //           엔트리 순서 = OFF의 개별 송신 순서(섹터 순회순) 그대로.
        _membershipInboundBuffer.clear();
        for (int32_t i = 0; i < removedCount; ++i)
        {
            const auto& players = zone->GetSectorManager().GetSectorPlayers(removed[i].x, removed[i].y);
            for (CPlayer* other : players)
                _membershipInboundBuffer.push_back(other);
        }
        SendDeletePlayerBatch(player, _membershipInboundBuffer.data(),
                              static_cast<int>(_membershipInboundBuffer.size()));
#else
        // 인바운드: "상대를 삭제"는 상대마다 내용이 달라 개별 송신
        for (int32_t i = 0; i < removedCount; ++i)
        {
            const auto& players = zone->GetSectorManager().GetSectorPlayers(removed[i].x, removed[i].y);
            for (CPlayer* other : players)
                SendDeletePlayer(player, other);   // 나에게 상대를 삭제
        }
#endif
    }

    // 진입 섹터 — 상호 CREATE
    if (addedCount > 0)
    {
#if USE_MEMBERSHIP_DIGEST
        // [Phase 4] 아웃바운드: "나를 생성" 1개 빌드 → 진입 섹터들에 직송 등록.
        //           1칸 이동은 진입 섹터에 자신이 없고, 멀티섹터 점프로 자기 현재 섹터가 끼면 그 섹터만 개별 폴백
        RegisterOutboundToSectors(zone, player->_zoneId, added, addedCount,
                                  MakeCreateOtherPlayer(player, SpawnReason::NORMAL), player);
#else
        // 아웃바운드: "나를 생성" 1개 빌드 → 진입 섹터 전원에게 팬아웃 (자기 자신 제외)
        FanoutToSectors(zone, added, addedCount, MakeCreateOtherPlayer(player, SpawnReason::NORMAL), player);
#endif

#if USE_MEMBERSHIP_INBOUND_BUNDLE
        // [Phase 2] 인바운드: 상대들을 배치 패킷(상한 초과 시 청크 분할)으로 — 자기 자신 제외는 OFF와 동일.
        _membershipInboundBuffer.clear();
        for (int32_t i = 0; i < addedCount; ++i)
        {
            const auto& players = zone->GetSectorManager().GetSectorPlayers(added[i].x, added[i].y);
            for (CPlayer* other : players)
            {
                if (other == player)
                    continue;  // 멀티섹터 점프 시 자기 자신 방지
                _membershipInboundBuffer.push_back(other);
            }
        }
        SendCreatePlayerBatch(player, _membershipInboundBuffer.data(),
                              static_cast<int>(_membershipInboundBuffer.size()));
#else
        // 인바운드: "상대를 생성"은 상대마다 내용이 달라 개별 송신
        for (int32_t i = 0; i < addedCount; ++i)
        {
            const auto& players = zone->GetSectorManager().GetSectorPlayers(added[i].x, added[i].y);
            for (CPlayer* other : players)
            {
                if (other == player)
                    continue;  // 멀티섹터 점프 시 자기 자신 방지
                SendCreateOtherPlayer(player, other);   // 나에게 상대를 생성
            }
        }
#endif
    }
#else
    // 이탈 섹터 — 상호 DELETE
    for (int32_t i = 0; i < removedCount; ++i)
    {
        const auto& players = zone->GetSectorManager().GetSectorPlayers(removed[i].x, removed[i].y);
        for (CPlayer* other : players)
        {
            SendDeletePlayer(other, player);  // 상대에게 나를 삭제
            SendDeletePlayer(player, other);   // 나에게 상대를 삭제
        }
    }

    // 진입 섹터 — 상호 CREATE
    for (int32_t i = 0; i < addedCount; ++i)
    {
        const auto& players = zone->GetSectorManager().GetSectorPlayers(added[i].x, added[i].y);
        for (CPlayer* other : players)
        {
            if (other == player)
                continue;  // 멀티섹터 점프 시 자기 자신 방지

            SendCreateOtherPlayer(other, player);  // 상대에게 나를 생성
            SendCreateOtherPlayer(player, other);   // 나에게 상대를 생성
        }
    }
#endif
}

#if USE_MEMBERSHIP_FANOUT_DEDUP
// [Phase 1] 미리 빌드한 버퍼 1개를 여러 섹터의 플레이어에게 팬아웃 (아웃바운드 멤버십 전용).
// BroadcastSectorPacket과 동일한 2패스 배치 AddRef 패턴: 유효 타겟 선카운트 → AddRef(N) → 타겟별 송신 → 최종 SubRef.
// 두 패스 사이 세션 변동 없음(단일 게임루프 스레드) → 카운트 정합 보장. _membershipSends는 OFF와 동일하게 타겟당 1 집계.
void CGameServer::FanoutToSectors(CZone* zone,
                                  const CSectorManager::SectorPos* sectors, int32_t sectorCount,
                                  CSerialBuffer* pMsg, CPlayer* exclude)
{
    // 1패스: 유효 타겟(세션 보유, exclude 제외) 선카운트
    size_t validCount = 0;
    for (int32_t i = 0; i < sectorCount; ++i)
    {
        const auto& players = zone->GetSectorManager().GetSectorPlayers(sectors[i].x, sectors[i].y);
        for (CPlayer* other : players)
        {
            if (other == exclude || other->_sessionId == -1)
                continue;
            ++validCount;
        }
    }

    if (validCount > 0)
        pMsg->AddRef(static_cast<int64_t>(validCount));   // 타겟별 소유권 일괄 확보 (원자연산 N→1)

    // 2패스: 팬아웃 — RequestSendMsg가 타겟별 소유권 1 소비 (성공/실패 모든 경로에서 SubRef)
    const int dataSize = pMsg->GetDataSize();
    size_t sentPkts = 0;
    for (int32_t i = 0; i < sectorCount; ++i)
    {
        const auto& players = zone->GetSectorManager().GetSectorPlayers(sectors[i].x, sectors[i].y);
        for (CPlayer* other : players)
        {
            if (other == exclude)
                continue;
            ++_tickMembershipSends;   // 멤버십 복사량(횟수) — OFF의 SendXxx 호출당 1과 동일 집계 (세션 무효 포함)
            if (other->_sessionId == -1)
                continue;
            if (_network->RequestSendMsg(other->_sessionId, pMsg, SendFlush::Deferred))
                ++sentPkts;
        }
    }

    if (sentPkts > 0)
    {
        _monitor._sendPackets.Add(static_cast<int64_t>(sentPkts));
        _monitor._sendEnqueuedBytes.Add(static_cast<int64_t>(sentPkts) * dataSize);
    }

    pMsg->SubRef();   // 빌더가 넘긴 소유권 1 회수 (타겟 0명이어도 안전)
}

#if USE_MEMBERSHIP_DIGEST
// [Phase 4] 아웃바운드 멤버십을 직송 보류로 등록 — 섹터당 등록 1건(주민 열람 없음), 배포·회수는 틱 끝 digest.
// 소유권: 소비자(직송 등록·폴백 팬아웃)마다 AddRef로 +1씩 공급하고, 끝에서 빌더 몫 1을 회수.
// exclude 처리: 직송은 공유 버퍼라 수신자별 제외가 불가 → exclude가 그 섹터의 주민인 경우
// (멀티섹터 점프로 진입 목록에 자기 현재 섹터가 포함될 때뿐 — 1칸 이동은 진입 섹터에 자신이 없음)만
// 그 1개 섹터를 기존 FanoutToSectors(개별 송신, exclude 지원)로 폴백해 와이어 내용을 보존한다.
void CGameServer::RegisterOutboundToSectors(CZone* zone, int32_t zoneId,
                                            const CSectorManager::SectorPos* sectors, int32_t sectorCount,
                                            CSerialBuffer* pMsg, CPlayer* exclude)
{
    for (int32_t i = 0; i < sectorCount; ++i)
    {
        if (exclude != nullptr &&
            sectors[i].x == exclude->_sectorX && sectors[i].y == exclude->_sectorY)
        {
            // 점프 폴백 — FanoutToSectors가 빌더 몫 1을 소비하므로 AddRef로 별도 소유권 공급
            pMsg->AddRef();
            FanoutToSectors(zone, &sectors[i], 1, pMsg, exclude);
            continue;
        }
        pMsg->AddRef();   // 직송 등록 1건당 소유권 1 — FlushSectorSends 4)에서 회수
        RegisterSectorDirectItem(zone, zoneId, sectors[i].x, sectors[i].y, pMsg);
    }
    pMsg->SubRef();   // 빌더가 넘긴 소유권 1 회수 (등록 0건이어도 안전)
}
#endif // USE_MEMBERSHIP_DIGEST
#endif // USE_MEMBERSHIP_FANOUT_DEDUP

#if USE_SECTOR_AGGREGATION
// ==========================================================================
// 섹터 묶음 (USE_SECTOR_AGGREGATION)
// ==========================================================================

// 즉시 브로드캐스트 대신 dirty 등록 — 같은 플레이어는 틱당 1회만.
// 묶음은 틱 끝 최종 상태를 읽으므로 여기선 등록만 (상태는 호출 직전에 이미 갱신됨).
// → 배출: 틱 끝 FlushSectorUpdates(BUNDLE off) / FlushSectorSends(BUNDLE on)에서 _sectorOutbox.dirtyMovers 소비.
void CGameServer::MarkMoveDirty(CPlayer* player)
{
    if (player->_moveDirty)
        return;
    player->_moveDirty = true;
    _sectorOutbox.dirtyMovers.push_back(player);
}

// 틱 끝: dirty 플레이어를 (zone, sector)별로 묶어 그 섹터 주변에 송신.
// ← 생산: MarkMoveDirty (틱 중 _sectorOutbox.dirtyMovers 적재).
void CGameServer::FlushSectorUpdates()
{
    if (_sectorOutbox.dirtyMovers.empty())
        return;

    // [계측] 묶음 빌드+송신 전체를 enqueue 축에 합산 (baseline의 broadcast_enqueue와 같은 비용종류로 A/B 비교)
    auto measT0 = std::chrono::steady_clock::now();

    // (zoneId, sectorY, sectorX)로 정렬 → 같은 섹터가 연속 구간이 되어 그룹 단위로 처리
    std::sort(_sectorOutbox.dirtyMovers.begin(), _sectorOutbox.dirtyMovers.end(),
        [](CPlayer* a, CPlayer* b)
        {
            if (a->_zoneId != b->_zoneId)   return a->_zoneId < b->_zoneId;
            if (a->_sectorY != b->_sectorY) return a->_sectorY < b->_sectorY;
            return a->_sectorX < b->_sectorX;
        });

    const size_t total = _sectorOutbox.dirtyMovers.size();
    size_t i = 0;
    while (i < total)
    {
        CPlayer* head = _sectorOutbox.dirtyMovers[i];
        const int32_t zoneId = head->_zoneId;
        const int32_t sx = head->_sectorX;
        const int32_t sy = head->_sectorY;

        // head와 같은 섹터가 이어지는 끝(j)을 찾는다 → [i, j)가 한 섹터 묶음
        size_t j = i;
        while (j < total &&  // 배열 끝을 넘지 않는 동안
            _sectorOutbox.dirtyMovers[j]->_zoneId == zoneId &&  // j번째가 기준과 같은 zone이고
            _sectorOutbox.dirtyMovers[j]->_sectorX == sx &&     // 같은 sectorX이고
            _sectorOutbox.dirtyMovers[j]->_sectorY == sy)       // 같은 sectorY이면
            ++j;

        // [i, j) 묶음을 SECTOR_UPDATE_MAX_ENTRIES씩 잘라 패킷화 → 섹터(sx,sy) 주변에 broadcast
        CZone* zone = _mapManager.GetZone(zoneId);
        if (zone != nullptr)
        {
            // 한 섹터를 SECTOR_UPDATE_MAX_ENTRIES씩 끊어 송신 (버퍼 한계 가드 — 균등 부하엔 1청크)
            size_t k = i;
            while (k < j)
            {
                int chunk = static_cast<int>((std::min)(j - k, static_cast<size_t>(SECTOR_UPDATE_MAX_ENTRIES)));
                CSerialBuffer* buf = MakeSectorUpdates(&_sectorOutbox.dirtyMovers[k], chunk);
                BroadcastSectorPacket(zone, sx, sy, buf);
                k += chunk;
            }
        }
        i = j;
    }

    // dirty 리셋 (다음 틱 이월 없음)
    for (CPlayer* p : _sectorOutbox.dirtyMovers)
        p->_moveDirty = false;
    _sectorOutbox.dirtyMovers.clear();

    auto measT1 = std::chrono::steady_clock::now();
    _tickBroadcastEnqueueUs += std::chrono::duration_cast<std::chrono::microseconds>(measT1 - measT0).count();
}

// 섹터 좌표 기준 주변 9섹터에 묶음 패킷 전송.
// BroadcastAroundSector의 소유권(배치 AddRef)·송신 메트릭 패턴을 그대로 복제 (본인 포함).
void CGameServer::BroadcastSectorPacket(CZone* zone, int32_t sectorX, int32_t sectorY, CSerialBuffer* pMsg)
{
    _broadcastBuffer.clear();
    zone->GetSectorManager().GetAroundPlayers(sectorX, sectorY, _broadcastBuffer, nullptr);

    _monitor._gameLoop._broadcastCalls.Inc();
    _monitor._gameLoop._broadcastTargets.Add(static_cast<int64_t>(_broadcastBuffer.size()));

    // 유효 타겟(세션 보유) 선카운트 → 배치 AddRef (원자연산 N→1)
    size_t validCount = 0;
    for (CPlayer* other : _broadcastBuffer)
    {
        if (other->_sessionId != -1)
            ++validCount;
    }
    if (validCount > 0)
        pMsg->AddRef(static_cast<int64_t>(validCount));

    const int dataSize = pMsg->GetDataSize();
    size_t sentPkts = 0;
    for (CPlayer* other : _broadcastBuffer)
    {
        if (other->_sessionId == -1)
            continue;
        if (_network->RequestSendMsg(other->_sessionId, pMsg, SendFlush::Deferred))
            ++sentPkts;
    }
    if (sentPkts > 0)
    {
        _monitor._sendPackets.Add(static_cast<int64_t>(sentPkts));
        _monitor._sendEnqueuedBytes.Add(static_cast<int64_t>(sentPkts) * dataSize);
    }
    pMsg->SubRef();   // 빌더가 넘긴 소유권 1 회수 (타겟 0명이어도 안전 회수)
}

#if USE_BROADCAST_BUNDLE
// ==========================================================================
// 수신섹터 digest (USE_BROADCAST_BUNDLE, Phase 3)
//
// 기존(OFF): 소스 아이템(번들 청크·채팅)마다 주변 3×3 주민에게 개별 RequestSendMsg
//            → (아이템 × 수신자)회의 세션 핀(원자연산)+링 뮤텍스+복사 반복이 broadcast_copy의 지배분.
// digest(ON): 같은 섹터 주민은 수신 집합이 완전히 동일(본인 포함 정책)하다는 성질을 이용,
//            수신 섹터별로 이웃 9섹터 보류물을 raw 바이트 1덩어리로 연접 → 주민당 1회 RequestSendRaw.
//            와이어 바이트·논리 패킷 수·순서 의미 불변 (coalescing이 이미 틱 끝 연접 송신이라 클라 무변경).
// ==========================================================================

// 존 최초 등록 — 그리드 크기 확정 (맵 크기는 부팅 후 불변). 방송(items)/직송(directItems)
// 어느 쪽이 먼저 등록해도 1회만 초기화 (countX==0 센티널 — items.empty()는 직송 선행 시 오판).
void CGameServer::EnsureZonePending(CZone* zone, SectorOutbox::ZonePending& zp)
{
    if (zp.countX != 0)
        return;
    zp.countX = zone->GetSectorManager().GetSectorCountX();
    zp.countY = zone->GetSectorManager().GetSectorCountY();
    const size_t total = static_cast<size_t>(zp.countX) * zp.countY;
    zp.items.resize(total);
    zp.recvEpoch.assign(total, 0);
#if USE_MEMBERSHIP_DIGEST
    zp.directItems.resize(total);
#endif
}

// 보류 등록 — (zoneId, 섹터)에 버퍼 적재. 버퍼 소유권 1은 FlushSectorSends 4)에서 회수.
// _broadcastCalls는 소스 아이템 단위로 집계 (기존 per-청크/per-채팅 호출 카운트와 동일 의미 → A/B 비교 가능).
// → 배출: 틱 끝 FlushSectorSends에서 _sectorOutbox.pendingByZone을 수신섹터 digest로 연접·배포.
void CGameServer::RegisterSectorItem(CZone* zone, int32_t zoneId, int32_t sectorX, int32_t sectorY,
                                     CSerialBuffer* pMsg)
{
    auto& zp = _sectorOutbox.pendingByZone[zoneId];
    EnsureZonePending(zone, zp);
    const int32_t idx = sectorY * zp.countX + sectorX;
    if (zp.items[idx].empty())
        _sectorOutbox.touchedSectors.emplace_back(zoneId, idx);
    zp.items[idx].push_back(pMsg);
    _monitor._gameLoop._broadcastCalls.Inc();
}

#if USE_MEMBERSHIP_DIGEST
// [Phase 4] 직송 보류 등록 — (zoneId, 섹터)에 "그 섹터 주민에게만" 전달할 버퍼 적재 (멤버십 CREATE/DELETE 전용).
// 버퍼 소유권 1은 FlushSectorSends 4)에서 회수. _broadcastCalls는 안 늘린다(방송 지표 오염 방지) —
// 멤버십 횟수는 배포 시점에 _tickMembershipSends로 승계 (OFF의 타겟당 1 집계와 동일 산식).
void CGameServer::RegisterSectorDirectItem(CZone* zone, int32_t zoneId, int32_t sectorX, int32_t sectorY,
                                           CSerialBuffer* pMsg)
{
    auto& zp = _sectorOutbox.pendingByZone[zoneId];
    EnsureZonePending(zone, zp);
    const int32_t idx = sectorY * zp.countX + sectorX;
    if (zp.directItems[idx].empty())
        _sectorOutbox.touchedDirectSectors.emplace_back(zoneId, idx);
    zp.directItems[idx].push_back(pMsg);
}
#endif

// 틱 끝: ① 이동 dirty → 섹터별 번들 빌드·보류 ② 수신섹터 후보 수집 ③ 연접·배포 ④ 일괄 해제.
// ← 생산: MarkMoveDirty(_sectorOutbox.dirtyMovers) + RegisterSectorItem(_sectorOutbox.pendingByZone)
//         + RegisterSectorDirectItem(직송, USE_MEMBERSHIP_DIGEST).
void CGameServer::FlushSectorSends()
{
    if (_sectorOutbox.dirtyMovers.empty() && _sectorOutbox.touchedSectors.empty()
#if USE_MEMBERSHIP_DIGEST
        && _sectorOutbox.touchedDirectSectors.empty()
#endif
        )
        return;

    // [계측] 빌드+연접+배포 전체를 enqueue 축에 합산 — OFF의 FlushSectorUpdates·채팅 enqueue와
    //   같은 비용종류이므로 broadcast_copy_ms_per_tick으로 A/B 직접 비교 가능.
    auto measT0 = std::chrono::steady_clock::now();

    // ── 1) 이동 번들 빌드 → 보류 등록 (FlushSectorUpdates의 빌드 로직 그대로, 송신만 보류로 대체) ──
    if (!_sectorOutbox.dirtyMovers.empty())
    {
        std::sort(_sectorOutbox.dirtyMovers.begin(), _sectorOutbox.dirtyMovers.end(),
            [](CPlayer* a, CPlayer* b)
            {
                if (a->_zoneId != b->_zoneId)   return a->_zoneId < b->_zoneId;
                if (a->_sectorY != b->_sectorY) return a->_sectorY < b->_sectorY;
                return a->_sectorX < b->_sectorX;
            });

        const size_t total = _sectorOutbox.dirtyMovers.size();
        size_t i = 0;
        while (i < total)
        {
            CPlayer* head = _sectorOutbox.dirtyMovers[i];
            const int32_t zoneId = head->_zoneId;
            const int32_t sx = head->_sectorX;
            const int32_t sy = head->_sectorY;

            size_t j = i;
            while (j < total &&
                _sectorOutbox.dirtyMovers[j]->_zoneId == zoneId &&
                _sectorOutbox.dirtyMovers[j]->_sectorX == sx &&
                _sectorOutbox.dirtyMovers[j]->_sectorY == sy)
                ++j;

            CZone* zone = _mapManager.GetZone(zoneId);
            if (zone != nullptr)
            {
                size_t k = i;
                while (k < j)
                {
                    int chunk = static_cast<int>((std::min)(j - k, static_cast<size_t>(SECTOR_UPDATE_MAX_ENTRIES)));
                    RegisterSectorItem(zone, zoneId, sx, sy, MakeSectorUpdates(&_sectorOutbox.dirtyMovers[k], chunk));
                    k += chunk;
                }
            }
            i = j;
        }

        for (CPlayer* p : _sectorOutbox.dirtyMovers)
            p->_moveDirty = false;
        _sectorOutbox.dirtyMovers.clear();
    }

    // ── 2) 수신섹터 후보 = 소스(touched)의 3×3 합집합 (epoch 마킹으로 중복 제거, clear 불필요) ──
    ++_sectorOutbox.flushEpoch;
    _sectorOutbox.receiverSectors.clear();
    for (const auto& t : _sectorOutbox.touchedSectors)
    {
        auto& zp = _sectorOutbox.pendingByZone[t.first];
        const int32_t sy = t.second / zp.countX;
        const int32_t sx = t.second % zp.countX;
        const int32_t y0 = (std::max)(sy - 1, 0), y1 = (std::min)(sy + 1, zp.countY - 1);
        const int32_t x0 = (std::max)(sx - 1, 0), x1 = (std::min)(sx + 1, zp.countX - 1);
        for (int32_t ny = y0; ny <= y1; ++ny)
        {
            for (int32_t nx = x0; nx <= x1; ++nx)
            {
                const int32_t ridx = ny * zp.countX + nx;
                if (zp.recvEpoch[ridx] != _sectorOutbox.flushEpoch)
                {
                    zp.recvEpoch[ridx] = _sectorOutbox.flushEpoch;
                    _sectorOutbox.receiverSectors.emplace_back(t.first, ridx);
                }
            }
        }
    }

#if USE_MEMBERSHIP_DIGEST
    // [Phase 4] 직송 섹터 자체도 수신 후보 — removed 섹터는 mover의 새 3×3 밖이라
    // 위 방송 union에 안 잡힌다 (여기 없으면 DELETE가 통째로 증발).
    for (const auto& t : _sectorOutbox.touchedDirectSectors)
    {
        auto& zp = _sectorOutbox.pendingByZone[t.first];
        if (zp.recvEpoch[t.second] != _sectorOutbox.flushEpoch)
        {
            zp.recvEpoch[t.second] = _sectorOutbox.flushEpoch;
            _sectorOutbox.receiverSectors.emplace_back(t.first, t.second);
        }
    }
#endif

    // ── 3) 각 수신섹터: 이웃 9섹터 보류물 연접(digest) → 주민당 RequestSendRaw 1회 ──
    int64_t sentPkts = 0, sentBytes = 0, targets = 0;
    _sectorOutbox.concatBuf.reserve(16384);   // 최초 1회만 실할당 (동접 5000 기준 digest ~5KB)
    for (const auto& r : _sectorOutbox.receiverSectors)
    {
        CZone* zone = _mapManager.GetZone(r.first);
        if (zone == nullptr)
            continue;   // 틱 중 존 소멸 — 보류물 해제는 4)가 책임
        auto& zp = _sectorOutbox.pendingByZone[r.first];
        const int32_t ry = r.second / zp.countX;
        const int32_t rx = r.second % zp.countX;

        const std::vector<CPlayer*>& residents = zone->GetSectorManager().GetSectorPlayers(rx, ry);
        if (residents.empty())
            continue;   // 주민 없으면 연접 비용도 생략

        _sectorOutbox.concatBuf.clear();
        int32_t itemCount = 0;
#if USE_MEMBERSHIP_DIGEST
        // [Phase 4] 직송 선연접 — CREATE가 같은 digest 안의 이동 번들(방송 구간)보다 앞서도록 순서 보장
        int32_t directCount = 0;
        for (CSerialBuffer* buf : zp.directItems[r.second])
        {
            const char* p = buf->GetReadBufferPtr();
            _sectorOutbox.concatBuf.insert(_sectorOutbox.concatBuf.end(), p, p + buf->GetDataSize());
            ++directCount;
        }
        itemCount += directCount;
#endif
        const int32_t y0 = (std::max)(ry - 1, 0), y1 = (std::min)(ry + 1, zp.countY - 1);
        const int32_t x0 = (std::max)(rx - 1, 0), x1 = (std::min)(rx + 1, zp.countX - 1);
        for (int32_t ny = y0; ny <= y1; ++ny)
        {
            for (int32_t nx = x0; nx <= x1; ++nx)
            {
                for (CSerialBuffer* buf : zp.items[ny * zp.countX + nx])
                {
                    const char* p = buf->GetReadBufferPtr();
                    _sectorOutbox.concatBuf.insert(_sectorOutbox.concatBuf.end(), p, p + buf->GetDataSize());
                    ++itemCount;
                }
            }
        }
        if (itemCount == 0)
            continue;

        const int concatSize = static_cast<int>(_sectorOutbox.concatBuf.size());
        // targets = (아이템 × 주민) 전달 쌍 수 — 기존 Σ(호출별 gather 인원)과 재배열만 다르고 총량 동일
#if USE_MEMBERSHIP_DIGEST
        // [Phase 4] 직송(멤버십)은 방송 지표에서 제외 — OFF의 FanoutToSectors도 _broadcastTargets를 안 늘렸다.
        // 대신 멤버십 횟수를 OFF의 타겟당 1 집계(세션 무효 포함, FanoutToSectors 2패스와 동일 산식)로 승계
        // → membership_sends_rate가 A/B 등가 통제지표로 유지된다.
        targets += static_cast<int64_t>(residents.size()) * (itemCount - directCount);
        _tickMembershipSends += static_cast<int64_t>(residents.size()) * directCount;
#else
        targets += static_cast<int64_t>(residents.size()) * itemCount;
#endif
        for (CPlayer* other : residents)
        {
            if (other->_sessionId == -1)
                continue;
            if (_network->RequestSendRaw(other->_sessionId, _sectorOutbox.concatBuf.data(), concatSize))
            {
                sentPkts  += itemCount;     // 논리 패킷 수 보존 (digest = 기존 패킷 itemCount개의 연접)
                sentBytes += concatSize;    // Σ아이템 크기와 동일 → avg_pkt_bytes 통제지표 불변
            }
        }
    }

    // ── 4) 보류물 일괄 해제 — 존 소멸·주민 0이어도 무조건 회수 (빌더 소유권 1을 여기서 소비) ──
    for (const auto& t : _sectorOutbox.touchedSectors)
    {
        auto& zp = _sectorOutbox.pendingByZone[t.first];
        for (CSerialBuffer* buf : zp.items[t.second])
            buf->SubRef();
        zp.items[t.second].clear();
    }
    _sectorOutbox.touchedSectors.clear();

#if USE_MEMBERSHIP_DIGEST
    // [Phase 4] 직송 보류물도 동일 규칙으로 일괄 해제 (등록 1건당 소유권 1)
    for (const auto& t : _sectorOutbox.touchedDirectSectors)
    {
        auto& zp = _sectorOutbox.pendingByZone[t.first];
        for (CSerialBuffer* buf : zp.directItems[t.second])
            buf->SubRef();
        zp.directItems[t.second].clear();
    }
    _sectorOutbox.touchedDirectSectors.clear();
#endif

    // 송신 메트릭 배치 반영 (기존 BroadcastSectorPacket의 원자연산 N→1 패턴과 동일)
    if (targets > 0)
        _monitor._gameLoop._broadcastTargets.Add(targets);
    if (sentPkts > 0)
    {
        _monitor._sendPackets.Add(sentPkts);
        _monitor._sendEnqueuedBytes.Add(sentBytes);
    }

    auto measT1 = std::chrono::steady_clock::now();
    _tickBroadcastEnqueueUs += std::chrono::duration_cast<std::chrono::microseconds>(measT1 - measT0).count();
}
#endif // USE_BROADCAST_BUNDLE
#endif // USE_SECTOR_AGGREGATION
