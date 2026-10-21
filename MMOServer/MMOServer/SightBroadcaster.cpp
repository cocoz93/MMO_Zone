#include "SightBroadcaster.h"
#include "SessionHub.h"
#include "MapManager.h"
#include "Player.h"
#include "MonitorManager.h"
#include "GameMonitor.h"
#include "SerialBuffer.h"
#include "NetIoModel.h"   // MAX_PACKET_SIZE — CREATE 묶음 한 장 검사(static_assert)
#include <Protocol/Protocol.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>  // offsetof
#include "PacketBuild.h" // 머리 쓰기·닫기 · 등장 조각(WriteCreateEntry — CREATE 와 CREATE 묶음이 같은 함수) — 게임 서버 나누기 0-②

using namespace PacketBuild;   // 아래 조립 함수들이 머리 쓰기·닫기를 이름만으로 부른다

// ==========================================================================
// 시야 방송 S2C 조립 — 이 조각만 부른다(GameServer.cpp 에서 옮김 · 몸·주석 그대로).
//   쓰는 법(스칼라는 << · 고정배열·문자열은 SetData(raw) · pack(1))은 PacketBuild.h 머리의 [규칙] 주석과 같다.
// ==========================================================================
namespace
{
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

    CSerialBuffer* MakeCreateOtherPlayer(CCharacter* player, SpawnReason reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_CREATE_OTHER_PLAYER::TYPE);
        WriteCreateEntry(buf, player, reason);
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

#if USE_DROP
    CSerialBuffer* MakeGroundItemCreate(const CGroundItem& g, GroundItemAppear reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_GROUND_ITEM_CREATE::TYPE);
        *buf << static_cast<int32_t>(g.groundId);
        *buf << static_cast<int32_t>(g.item.itemId);
        *buf << static_cast<int32_t>(g.item.count);
        *buf << g.x;
        *buf << g.y;
        *buf << static_cast<uint8_t>(reason);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_GROUND_ITEM_CREATE));
        return buf;
    }

    CSerialBuffer* MakeGroundItemDelete(int32_t groundId, GroundItemGone reason, int32_t pickerId)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_GROUND_ITEM_DELETE::TYPE);
        *buf << static_cast<int32_t>(groundId);
        *buf << static_cast<uint8_t>(reason);
        *buf << static_cast<int32_t>(pickerId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_GROUND_ITEM_DELETE));
        return buf;
    }

#endif // USE_DROP

#if USE_SECTOR_AGGREGATION
    // 섹터 묶음 — dirty 플레이어 [0,count)의 최종 상태(위치·방향·이동상태)를 직렬화.
    // 스칼라 시퀀스(<<)라 SetData 불필요. count는 선기록(백패치 불필요).
    // 좌표는 float 그대로가 아니라 눈금(QuantizePos)으로 싣는다 — 필드 순서는 SectorUpdateEntry와 같아야 한다.
    CSerialBuffer* MakeSectorUpdates(CCharacter** players, int count)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_SECTOR_UPDATES::TYPE);
        *buf << static_cast<uint16_t>(count);
        for (int i = 0; i < count; ++i)
        {
            CCharacter* p = players[i];
            *buf << static_cast<int>(p->_actorId);
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
    // 가득 찬 묶음(상한 × 엔트리)도 한 패킷에 들어가야 한다 — 엔트리를 늘리면 상한을 같이 줄일 것(Protocol.h 계산 주석).
    //   여기 두는 이유는 위 가방 단언과 같다(MAX_PACKET_SIZE 가 코어 헤더에 있다).
    static_assert(sizeof(MSG_S2C_CREATE_PLAYER_BATCH) <= MAX_PACKET_SIZE,
                  "가득 찬 CREATE 묶음이 한 패킷(MAX_PACKET_SIZE)에 안 들어간다 — CREATE_PLAYER_BATCH_MAX_ENTRIES 를 줄일 것");

    // [Phase 2] 인바운드 멤버십 묶음 — 상대 [0,count)의 생성 정보를 배치 1패킷으로 직렬화.
    // 한 명분은 WriteCreateEntry — CREATE_OTHER 와 같은 함수다 (클라가 엔트리를 기존 CREATE 이벤트로 분해).
    CSerialBuffer* MakeCreatePlayerBatch(CCharacter* const* players, int count)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_CREATE_PLAYER_BATCH::TYPE);
        *buf << static_cast<uint16_t>(count);
        for (int i = 0; i < count; ++i)
            WriteCreateEntry(buf, players[i], SpawnReason::NORMAL);   // 인바운드(걸어서 시야 진입) 경로 고정 — OFF의 기본 인자와 동일
        FinalizePacket(buf);
        assert(buf->GetDataSize() ==
            static_cast<int>(offsetof(MSG_S2C_CREATE_PLAYER_BATCH, entries) + count * sizeof(CreatePlayerEntry)));
        return buf;
    }

    // [Phase 2] 인바운드 멤버십 묶음 — 상대 [0,count)의 playerId를 배치 1패킷으로 직렬화.
    CSerialBuffer* MakeDeletePlayerBatch(CCharacter* const* players, int count)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_DELETE_PLAYER_BATCH::TYPE);
        *buf << static_cast<uint16_t>(count);
        for (int i = 0; i < count; ++i)
            *buf << static_cast<int>(players[i]->_actorId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() ==
            static_cast<int>(offsetof(MSG_S2C_DELETE_PLAYER_BATCH, entries) + count * sizeof(DeletePlayerBatchEntry)));
        return buf;
    }
#endif
}

// ==========================================================================
// 준비 · 틱 — 게임 루프가 부른다(차례는 CGameServer::GameLoopThread). 몸은 게임 루프에서 옮긴 그대로.
// ==========================================================================

void CSightBroadcaster::Init(int32_t maxPerChannel, int maxClients)
{
    // 프레임 재사용 컨테이너 reserve (워밍업 realloc 방지)
    // 전체 서버 범위 버퍼도 maxPerChannel 기준 — 초과 시 자연 증가 후 capacity 유지
    _broadcastBuffer.reserve(maxPerChannel);       // 주변 9섹터 단위
    _eventAroundBuffer.reserve(maxPerChannel);     // 주변 9섹터 단위
    _sectorOutbox.dirtyMovers.reserve(maxClients);              // 전체 서버 dirty (이동/sync 묶음 대상)
}

void CSightBroadcaster::ProcessSightChanges()
{
    using Clock = std::chrono::steady_clock;

    // [계측] 멤버십(섹터이동 CREATE/DELETE) 송신 시간 — 구간 1쌍으로만 측정.
    //   per-call 금지: 멤버십은 ~1.04M/s라 건당 now()를 넣으면 측정 오버헤드가 대상을 오염시킴.
    auto membT0 = Clock::now();
    _mapManager.ForEachZone([this](CZone* zone)
    {
        // 명단은 존이 대기열로 만들고 그 자리에서 대기열을 비운다(원위치 복귀 빼기 · 섹터 차이 · 같은 틱 쌍 판정).
        //   여기는 보내기만 — 아래 송신은 대기열을 안 읽는다.
        const CZone::SightChanges& sight = zone->TakeSightChanges();
        for (const CZone::SightMove& move : sight.moves)
            ProcessSectorChange(zone, move);

        // 같은 틱에 함께 이동한 쌍은 위 루프가 서로를 놓친다(상대는 이미 새 섹터로 옮겨간 뒤라
        // 이탈/진입 섹터 조회에 안 걸림) → 누락된 CREATE/DELETE만 보충. 보정 비용도 멤버십 구간에 포함.
        SendSightPairFixes(sight.pairFixes);
    });

    _tickMembershipUs += std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - membT0).count();
}

void CSightBroadcaster::TickPeriodicSync()
{
    // 4) 주기적 위치 동기화 (MOVING 플레이어 → 주변 브로드캐스트)
    ++_syncFrameCount;
    if (_syncFrameCount >= SYNC_INTERVAL_FRAMES)
    {
        _syncFrameCount = 0;
        _mapManager.ForEachZone([&](CZone* zone)
        {
            for (CCharacter* player : zone->GetCharacters())
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
        // 동접 게이지(_enter._inGame)는 이 순회에 얹혀 있다가 입장 조각으로 갔다 — CEntryHandler::TickInGameGauge (게임 서버 나누기 14 단계)
    }
}

void CSightBroadcaster::FlushSends()
{
#if USE_BROADCAST_BUNDLE
    // [digest] 이번 틱 보류물(이동 번들+채팅)을 수신섹터 기준 연접으로 배포 (아래 flush가 묶어 보냄)
    FlushSectorSends();
#elif USE_SECTOR_AGGREGATION
    // [묶음] 이번 틱 dirty를 섹터별 묶음으로 송신 (RequestSendMsg는 Deferred → 아래 flush가 묶어 보냄)
    FlushSectorUpdates();
#endif
}

void CSightBroadcaster::ReportTickMetrics()
{
    // [계측] 비용종류별 — 틱 내 누적분을 1회씩 원자 반영 후 리셋 (송신 flush 구간은 게임 루프가 직접 잰다)
    _monitor._gameLoop._broadcastGatherUs.Add(_tickBroadcastGatherUs);
    _monitor._gameLoop._broadcastEnqueueUs.Add(_tickBroadcastEnqueueUs);
    _gameMonitor._sector._membershipSends.Add(_tickMembershipSends);      // 멤버십 변경 복사량(횟수) — 게임 그릇(10-06 코어에서 옮김)
    _gameMonitor._sector._membershipCostUs.Add(_tickMembershipUs);        // 멤버십 변경 송신 시간
    _gameMonitor._sector._membershipPairFixes.Add(_tickPairFixes);        // 같은 틱 이동자 쌍 보정 건수
    _tickBroadcastGatherUs = 0;
    _tickBroadcastEnqueueUs = 0;
    _tickMembershipSends = 0;
    _tickMembershipUs = 0;
    _tickPairFixes = 0;
}

// 주변 브로드캐스트 (CSerialBuffer — 빌더 RefCount=1 버퍼를 소비)
void CSightBroadcaster::BroadcastAroundSector(CZone* zone, CCharacter* player, CSerialBuffer* pMsg, bool excludeSelf)
{
    // [계측] 비용종류별 — gather(주변 모으기) / enqueue(수신자별 복사) 구간을 호출당 2~3회 now()로 분리.
    //   per-call 측정이라 오버헤드가 수신자 수가 아닌 호출 수에 비례. 누적은 틱 끝 1회만 원자반영.
    auto _measGatherT0 = std::chrono::steady_clock::now();

    _broadcastBuffer.clear();
    const CCharacter* exclude = excludeSelf ? player : nullptr;
    zone->GetSectorManager().GetAroundPlayers(
        player->_sectorX, player->_sectorY, _broadcastBuffer, exclude);

    auto _measGatherT1 = std::chrono::steady_clock::now();
    _tickBroadcastGatherUs += std::chrono::duration_cast<std::chrono::microseconds>(
        _measGatherT1 - _measGatherT0).count();

    _monitor._gameLoop._broadcastCalls.Inc();
    _monitor._gameLoop._broadcastTargets.Add(static_cast<int64_t>(_broadcastBuffer.size()));   // 플레이어만 센다 — 뱀까지 세던 옛 값과 단위가 다르다

    // 타겟별 AddRef를 1회 배치 AddRef로 압축 (원자연산 N→1) — 목록이 플레이어뿐이라 선카운트가 곧 크기다
    const size_t validCount = _broadcastBuffer.size();
    if (validCount > 0)
        pMsg->AddRef(static_cast<int64_t>(validCount));   // 타겟별 소유권 일괄 확보

    // 송신 메트릭은 타겟별 원자증가 대신 성공분을 지역 누적 후 1회 반영 (원자연산 N→1)
    const int dataSize = pMsg->GetDataSize();
    size_t sentPkts = 0;
    for (CPlayer* other : _broadcastBuffer)
    {
        if (_sessionHub.RequestSend(other, pMsg))
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
void CSightBroadcaster::NotifyMoveStart(CZone* zone, CCharacter* player)
{
#if USE_SECTOR_AGGREGATION
    MarkMoveDirty(player);
#else
    BroadcastAroundSector(zone, player,
        MakeMoveStart(player->_actorId, static_cast<uint8_t>(player->_direction),
            player->_x, player->_y));
#endif
}

void CSightBroadcaster::NotifyMoveStop(CZone* zone, CCharacter* player, bool excludeSelf)
{
#if USE_SECTOR_AGGREGATION
    MarkMoveDirty(player);
#else
    BroadcastAroundSector(zone, player,
        MakeMoveStop(player->_actorId, static_cast<uint8_t>(player->_direction),
            player->_x, player->_y), excludeSelf);
#endif
}

void CSightBroadcaster::NotifyMoveSync(CZone* zone, CCharacter* player)
{
#if USE_SECTOR_AGGREGATION
    MarkMoveDirty(player);
#else
    BroadcastAroundSector(zone, player,
        MakeSyncPosition(player->_actorId, player->_x, player->_y), false);
#endif
}

// ── 존 알림 (IZoneEvents) ── 존이 정한 걷기·멈춤 — 몬스터 판단(1-1)과 맵 경계에 막힘(2).
void CSightBroadcaster::OnCharacterMoveStart(CZone* zone, CCharacter* character)
{
    NotifyMoveStart(zone, character);
}

void CSightBroadcaster::OnCharacterMoveStop(CZone* zone, CCharacter* character)
{
#if USE_DB_WORKER
    // 플레이어가 여기 오는 건 경계에 막혔을 때뿐이다 — 위치가 바뀌었으니 저장 대상
    if (CPlayer* player = As<CPlayer>(character))
        player->_dbDirty = true;
#endif
    NotifyMoveStop(zone, character, false);   // 본인 포함
}

void CSightBroadcaster::SendCreateOtherPlayer(CPlayer* target, CCharacter* who, SpawnReason reason)
{
    ++_tickMembershipSends;   // 멤버십 변경 복사 집계 (BroadcastAroundSector 밖 경로)
    _sessionHub.SendPacket(target, MakeCreateOtherPlayer(who, reason));
}

void CSightBroadcaster::SendDeletePlayer(CPlayer* target, CCharacter* who)
{
    ++_tickMembershipSends;   // 멤버십 변경 복사 집계 (BroadcastAroundSector 밖 경로)
    _sessionHub.SendPacket(target, MakeDeletePlayer(who->_actorId));
}

#if USE_MEMBERSHIP_INBOUND_BUNDLE
// [Phase 2] 인바운드 멤버십 배치 송신 — 상대 목록을 배치 상한씩 잘라 mover 1명에게 송신.
// _membershipSends는 OFF의 개별 SendXxx 호출당 1과 동일하게 엔트리당 1 집계 (A/B 통제지표 불변).
// count=0이면 아무것도 안 보냄 — OFF의 빈 루프와 동일.
void CSightBroadcaster::SendCreatePlayerBatch(CPlayer* target, CCharacter* const* others, int count)
{
    _tickMembershipSends += count;
    for (int i = 0; i < count; i += CREATE_PLAYER_BATCH_MAX_ENTRIES)
    {
        int chunk = (std::min)(count - i, CREATE_PLAYER_BATCH_MAX_ENTRIES);
        _sessionHub.SendPacket(target, MakeCreatePlayerBatch(others + i, chunk));
    }
}

void CSightBroadcaster::SendDeletePlayerBatch(CPlayer* target, CCharacter* const* others, int count)
{
    _tickMembershipSends += count;
    for (int i = 0; i < count; i += DELETE_PLAYER_BATCH_MAX_ENTRIES)
    {
        int chunk = (std::min)(count - i, DELETE_PLAYER_BATCH_MAX_ENTRIES);
        _sessionHub.SendPacket(target, MakeDeletePlayerBatch(others + i, chunk));
    }
}
#endif // USE_MEMBERSHIP_INBOUND_BUNDLE

void CSightBroadcaster::SendSyncPosition(CPlayer* target)
{
    _sessionHub.SendPacket(target, MakeSyncPosition(target->_actorId, target->_x, target->_y));
}

#if USE_DROP
void CSightBroadcaster::SendGroundItemCreate(CPlayer* target, const CGroundItem& item, GroundItemAppear reason)
{
    _sessionHub.SendPacket(target, MakeGroundItemCreate(item, reason));
}

void CSightBroadcaster::SendGroundItemDelete(CPlayer* target, int32_t groundId, GroundItemGone reason, int32_t pickerId)
{
    _sessionHub.SendPacket(target, MakeGroundItemDelete(groundId, reason, pickerId));
}

// 땅의 아이템이 떨어졌다 — 주변에 CREATE(DROP). 몸은 땅 아이템 알림(3 단계 당시 CGameServer · 지금 CItemHandler::OnGroundItemDrop)에서 옮긴 그대로.
void CSightBroadcaster::BroadcastGroundItemCreate(CZone* zone, const CGroundItem& item, int32_t sectorX, int32_t sectorY)
{
    _eventAroundBuffer.clear();
    zone->GetSectorManager().GetAroundPlayers(sectorX, sectorY, _eventAroundBuffer);
    for (CPlayer* other : _eventAroundBuffer)
        SendGroundItemCreate(other, item, GroundItemAppear::DROP);
}

// 땅의 아이템이 빠졌다 — 주변 + 같은 틱 이동자에게 DELETE. 몸은 땅 아이템 알림(3 단계 당시 CGameServer · 지금 CItemHandler::OnGroundItemGone)에서 옮긴 그대로(와이어 사유 변환은 거기 남음).
void CSightBroadcaster::BroadcastGroundItemDelete(CZone* zone, int32_t sectorX, int32_t sectorY,
                                                  int32_t groundId, GroundItemGone wire, int32_t pickerId)
{
    _eventAroundBuffer.clear();
    zone->GetSectorManager().GetAroundPlayers(sectorX, sectorY, _eventAroundBuffer);
    for (CPlayer* other : _eventAroundBuffer)
        SendGroundItemDelete(other, groundId, wire, pickerId);

    // 같은 틱에 섹터를 넘은 사람 — 틱 1단계(줍기)에서만 대기열이 차 있다. 사라지기(3-3)는 비운 뒤라 존 조회가 빈 목록을 돌려준다.
    SendGroundItemDeleteToLateMovers(zone, sectorX, sectorY, groundId, wire, pickerId);
}

// 같은 틱에 섹터를 넘은 사람에게 땅의 아이템 DELETE — 헤더 주석.
//   판정은 존(CZone::CollectLateMovers — BroadcastLeaveZone 셋째 덩이와 같은 함수)이고, 아이템은 안 움직이니
//   "대상의 옛 섹터 = 지금 섹터"로 부른다. 옛 섹터로는 보였고(화면에 있다) 새 섹터로는 안 보이는(방금 보낸 주변 DELETE 에서 빠진) 이동자가 온다.
void CSightBroadcaster::SendGroundItemDeleteToLateMovers(CZone* zone, int32_t sectorX, int32_t sectorY,
                                                   int32_t groundId, GroundItemGone wire, int32_t pickerId)
{
    zone->CollectLateMovers(sectorX, sectorY, sectorX, sectorY, nullptr, _lateMoverBuffer);
    for (CCharacter* character : _lateMoverBuffer)
    {
        CPlayer* mover = As<CPlayer>(character);   // 대기열은 섞여 있다(몬스터도 섹터를 넘는다) — 받는 쪽만
        if (mover == nullptr)
            continue;

        SendGroundItemDelete(mover, groundId, wire, pickerId);
        _gameMonitor._drop._lateMoverDeletes.Inc();
    }
}
#endif

void CSightBroadcaster::BroadcastEnterZone(CZone* zone, CPlayer* player, SpawnReason reason)
{
    const CSectorManager& sectors = zone->GetSectorManager();

    // 주변 플레이어 → 나를 본다 (아웃바운드) — 받는 쪽은 플레이어 목록뿐이다
    _eventAroundBuffer.clear();
    sectors.GetAroundPlayers(player->_sectorX, player->_sectorY, _eventAroundBuffer, player);
    for (CPlayer* other : _eventAroundBuffer)
        SendCreateOtherPlayer(other, player, reason);   // 기존 플레이어에게 신규 플레이어 생성

    // 나 → 주변 전부(플레이어·몬스터·NPC)를 본다 (인바운드) — 섹터 변경의 인바운드와 같은 도우미로 세 목록을 돈다
    CSectorManager::SectorPos around[CSectorManager::MAX_AROUND_SECTORS];
    int32_t aroundCount = 0;
    sectors.GetAroundSectorList(player->_sectorX, player->_sectorY, around, aroundCount);
    for (int32_t i = 0; i < aroundCount; ++i)
    {
        sectors.ForEachInSector(around[i].x, around[i].y, [&](CCharacter* other)
        {
            if (other != player)
                SendCreateOtherPlayer(player, other);   // 신규 플레이어에게 기존 상대 생성
        });
#if USE_DROP
        // 땅의 아이템도 보인다 — 캐릭터가 아니라 땅 아이템 관리자의 같은 칸 목록을 따로 돈다(입장·존 이동 공통)
        for (const CGroundItem& g : zone->GetGroundItemManager().GetSectorItems(around[i].x, around[i].y))
            SendGroundItemCreate(player, g, GroundItemAppear::SEEN);
#endif
    }
}

// 세션 없는 것의 등장 — 주변 플레이어에게 알리기만 한다. 받는 쪽이 없으니 "나에게 주변을 생성"은 없다.
void CSightBroadcaster::BroadcastSpawn(CZone* zone, CCharacter* who, SpawnReason reason)
{
    _eventAroundBuffer.clear();
    zone->GetSectorManager().GetAroundPlayers(who->_sectorX, who->_sectorY, _eventAroundBuffer, who);

    for (CPlayer* other : _eventAroundBuffer)
        SendCreateOtherPlayer(other, who, reason);
}

void CSightBroadcaster::BroadcastLeaveZone(CZone* zone, CCharacter* player)
{
    // 현재 섹터 기준 주변 DELETE — 받는 쪽은 플레이어 목록뿐이다
    _eventAroundBuffer.clear();
    zone->GetSectorManager().GetAroundPlayers(
        player->_sectorX, player->_sectorY, _eventAroundBuffer, player);

    for (CPlayer* other : _eventAroundBuffer)
        SendDeletePlayer(other, player);

    // 미처리 섹터 변경이 있으면 이전 섹터 전용 뷰어에게도 DELETE
    // (같은 프레임에 섹터 변경 + 존 이동/접속해제 시 고스트 방지)
    //   이전 섹터 전용 섹터와 퇴장자의 옛 섹터는 존이 대기열로 정한다(CZone::CollectLeaveOldOnly) — 옛 섹터는 아래 셋째 덩이도 쓴다.
    //   대기열에서 빼는 것은 맨 끝 LeaveZone 이라 그때까지 유효하다.
    CSectorManager::SectorPos oldOnly[CSectorManager::MAX_AROUND_SECTORS];
    int32_t oldOnlyCount = 0;
    int32_t leaverOldX = 0;
    int32_t leaverOldY = 0;
    zone->CollectLeaveOldOnly(player, oldOnly, oldOnlyCount, leaverOldX, leaverOldY);

    for (int32_t i = 0; i < oldOnlyCount; ++i)
    {
        for (CPlayer* other : zone->GetSectorManager().GetSectorPlayers(oldOnly[i].x, oldOnly[i].y))
            SendDeletePlayer(other, player);
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
    //   고르는 것은 존(CZone::CollectLateMovers — 이동 전엔 퇴장자를 보고 있었는데 위 두 조회 어디에도 안 걸리는 이동자).
    zone->CollectLateMovers(leaverOldX, leaverOldY, player->_sectorX, player->_sectorY, player, _lateMoverBuffer);
    for (CCharacter* mover : _lateMoverBuffer)
    {
        if (CPlayer* m = As<CPlayer>(mover))
            SendDeletePlayer(m, player);
        ++_tickPairFixes;
    }
}

// ==========================================================================
// 같은 틱 이동자 쌍 보정 — 보내기와 계수만. 어느 쌍인가(판정)와 그 까닭은 존(Zone.cpp CZone::CollectPairFixes 머리 주석)
// ==========================================================================

void CSightBroadcaster::SendSightPairFixes(const std::vector<CZone::SightPairFix>& fixes)
{
    for (const CZone::SightPairFix& fix : fixes)
    {
        if (fix.visible)
        {
            // 안 보이다가 보임 — 서로 CREATE
            if (CPlayer* pa = As<CPlayer>(fix.a))
                SendCreateOtherPlayer(pa, fix.b);
            if (CPlayer* pb = As<CPlayer>(fix.b))
                SendCreateOtherPlayer(pb, fix.a);
        }
        else
        {
            // 보였다가 안 보임 — 서로 DELETE
            if (CPlayer* pa = As<CPlayer>(fix.a))
                SendDeletePlayer(pa, fix.b);
            if (CPlayer* pb = As<CPlayer>(fix.b))
                SendDeletePlayer(pb, fix.a);
        }
        ++_tickPairFixes;
    }
}

// ==========================================================================
// 섹터 변경 브로드캐스트
// ==========================================================================

void CSightBroadcaster::ProcessSectorChange(CZone* zone, const CZone::SightMove& move)
{
    CCharacter* const player = move.mover;

    // 인바운드(상대→나)는 내가 플레이어일 때만 있다 — 몬스터·NPC 는 받는 쪽이 못 되므로
    //   수집·직렬화 자체를 건너뛴다. 아웃바운드(나→상대)는 받는 쪽이 남이라 종류와 무관하게 그대로 간다.
    CPlayer* const me = As<CPlayer>(player);

    // 못 보게 된 섹터(removed) · 새로 보게 된 섹터(added) — 존이 정했다(CZone::TakeSightChanges, GetSectorDiff 출력 그대로)
    const CSectorManager::SectorPos* const added = move.added;
    const CSectorManager::SectorPos* const removed = move.removed;
    const int32_t addedCount = move.addedCount;
    const int32_t removedCount = move.removedCount;

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
                                  MakeDeletePlayer(player->_actorId), nullptr);
#else
        // 아웃바운드: "나를 삭제" 1개 빌드 → 이탈 섹터 전원에게 팬아웃 (player는 이탈 섹터에 없어 exclude 불필요)
        FanoutToSectors(zone, removed, removedCount, MakeDeletePlayer(player->_actorId), nullptr);
#endif

        // 아래는 전부 "나에게" 보내는 것이다 — 내가 플레이어가 아니면 통째로 건너뛴다
        if (me != nullptr)
        {
#if USE_MEMBERSHIP_INBOUND_BUNDLE
            // [Phase 2] 인바운드: 수신자가 mover 1명 → 상대들을 배치 패킷(상한 초과 시 청크 분할)으로 접음.
            //           엔트리 순서 = OFF의 개별 송신 순서(섹터 순회순) 그대로.
            _membershipInboundBuffer.clear();
            for (int32_t i = 0; i < removedCount; ++i)
            {
                zone->GetSectorManager().ForEachInSector(removed[i].x, removed[i].y,
                    [&](CCharacter* other) { _membershipInboundBuffer.push_back(other); });   // 보이는 것 전부(플레이어·몬스터·NPC)
            }
            SendDeletePlayerBatch(me, _membershipInboundBuffer.data(),
                                  static_cast<int>(_membershipInboundBuffer.size()));
#else
            // 인바운드: "상대를 삭제"는 상대마다 내용이 달라 개별 송신
            for (int32_t i = 0; i < removedCount; ++i)
            {
                zone->GetSectorManager().ForEachInSector(removed[i].x, removed[i].y,
                    [&](CCharacter* other) { SendDeletePlayer(me, other); });   // 나에게 상대를 삭제 — 보이는 것 전부
            }
#endif
        }
    }

    // 진입 섹터 — 상호 CREATE
    if (addedCount > 0)
    {
#if USE_MEMBERSHIP_DIGEST
        // [Phase 4] 아웃바운드: "나를 생성" 1개 빌드 → 진입 섹터들에 직송 등록.
        //           1칸 이동은 진입 섹터에 자신이 없고, 멀티섹터 점프로 자기 현재 섹터가 끼면 그 섹터만 개별 폴백
        //   ★ 이 경로는 SendCreateOtherPlayer 를 안 거치고 빌더를 직접 쓴다. 예전엔 체력(STATS)을 여기서 따로 실어야 했고,
        //     빠뜨렸더니 "내 HP 막대는 뜨는데 남의 것만 안 뜨는" 증상이 실제로 났다(라이브 캡처로 잡음).
        //     지금은 빌더가 체력을 싣는다(ActorSnapshot) — 여기서 따로 붙일 것이 없다.
        RegisterOutboundToSectors(zone, player->_zoneId, added, addedCount,
                                  MakeCreateOtherPlayer(player, SpawnReason::NORMAL), player);
#else
        // 아웃바운드: "나를 생성" 1개 빌드 → 진입 섹터 전원에게 팬아웃 (자기 자신 제외)
        FanoutToSectors(zone, added, addedCount, MakeCreateOtherPlayer(player, SpawnReason::NORMAL), player);
#endif

        // 아래는 전부 "나에게" 보내는 것이다 — 내가 플레이어가 아니면 통째로 건너뛴다
        if (me != nullptr)
        {
#if USE_MEMBERSHIP_INBOUND_BUNDLE
            // [Phase 2] 인바운드: 상대들을 배치 패킷(상한 초과 시 청크 분할)으로 — 자기 자신 제외는 OFF와 동일.
            _membershipInboundBuffer.clear();
            for (int32_t i = 0; i < addedCount; ++i)
            {
                zone->GetSectorManager().ForEachInSector(added[i].x, added[i].y, [&](CCharacter* other)
                {
                    if (other != player)   // 멀티섹터 점프 시 자기 자신 방지
                        _membershipInboundBuffer.push_back(other);   // 보이는 것 전부(플레이어·몬스터·NPC)
                });
            }
            SendCreatePlayerBatch(me, _membershipInboundBuffer.data(),
                                  static_cast<int>(_membershipInboundBuffer.size()));
#else
            // 인바운드: "상대를 생성"은 상대마다 내용이 달라 개별 송신
            for (int32_t i = 0; i < addedCount; ++i)
            {
                zone->GetSectorManager().ForEachInSector(added[i].x, added[i].y, [&](CCharacter* other)
                {
                    if (other != player)   // 멀티섹터 점프 시 자기 자신 방지
                        SendCreateOtherPlayer(me, other);   // 나에게 상대를 생성 — 보이는 것 전부
                });
            }
#endif
        }
    }
#else
    const CSectorManager& sectors = zone->GetSectorManager();

    // 이탈 섹터 — 상호 DELETE
    for (int32_t i = 0; i < removedCount; ++i)
    {
        for (CPlayer* other : sectors.GetSectorPlayers(removed[i].x, removed[i].y))
            SendDeletePlayer(other, player);   // 상대에게 나를 삭제 — 아웃바운드(받는 쪽은 플레이어뿐)
        if (me != nullptr)
            sectors.ForEachInSector(removed[i].x, removed[i].y,
                [&](CCharacter* other) { SendDeletePlayer(me, other); });   // 나에게 상대를 삭제 — 인바운드(보이는 것 전부)
    }

    // 진입 섹터 — 상호 CREATE
    for (int32_t i = 0; i < addedCount; ++i)
    {
        for (CPlayer* other : sectors.GetSectorPlayers(added[i].x, added[i].y))
        {
            if (other != player)   // 멀티섹터 점프 시 자기 자신 방지
                SendCreateOtherPlayer(other, player);   // 상대에게 나를 생성 — 아웃바운드
        }
        if (me != nullptr)
            sectors.ForEachInSector(added[i].x, added[i].y, [&](CCharacter* other)
            {
                if (other != player)
                    SendCreateOtherPlayer(me, other);   // 나에게 상대를 생성 — 인바운드
            });
    }
#endif

#if USE_DROP
    // ── 땅의 아이템 — 인바운드만 있다 ──
    //   아이템은 움직이지 않으니 "남이 나를 본다(아웃바운드)"가 없고, 받는 쪽은 늘 나 하나다.
    //   그래서 위 묶음·digest 토글 덩이들과 상관없이 여기 한 번만 둔다(꺼진 경로를 안 건드린다).
    //   같은 틱에 섹터를 넘으며 방금 떨어진 것을 두 번 받거나, 모르는 것의 삭제를 받을 수 있다 —
    //   클라는 땅 번호로 덮어쓰고 모르는 번호의 삭제는 버린다(Protocol.h MSG_S2C_GROUND_ITEM_CREATE).
    if (me != nullptr)
    {
        const CGroundItemManager& groundItems = zone->GetGroundItemManager();   // 섹터와 같은 칸 번호
        for (int32_t i = 0; i < removedCount; ++i)
        {
            for (const CGroundItem& g : groundItems.GetSectorItems(removed[i].x, removed[i].y))
                SendGroundItemDelete(me, g.groundId, GroundItemGone::OUT_OF_SIGHT, 0);
        }
        for (int32_t i = 0; i < addedCount; ++i)
        {
            for (const CGroundItem& g : groundItems.GetSectorItems(added[i].x, added[i].y))
                SendGroundItemCreate(me, g, GroundItemAppear::SEEN);
        }
    }
#endif
}

#if USE_MEMBERSHIP_FANOUT_DEDUP
// [Phase 1] 미리 빌드한 버퍼 1개를 여러 섹터의 플레이어에게 팬아웃 (아웃바운드 멤버십 전용).
// BroadcastSectorPacket과 동일한 2패스 배치 AddRef 패턴: 유효 타겟 선카운트 → AddRef(N) → 타겟별 송신 → 최종 SubRef.
// 두 패스 사이 세션 변동 없음(단일 게임루프 스레드) → 카운트 정합 보장. _membershipSends는 OFF와 동일하게 타겟당 1 집계.
void CSightBroadcaster::FanoutToSectors(CZone* zone,
                                  const CSectorManager::SectorPos* sectors, int32_t sectorCount,
                                  CSerialBuffer* pMsg, CCharacter* exclude)
{
    // 1패스: 유효 타겟(exclude 제외) 선카운트 — 목록이 플레이어뿐이다
    size_t validCount = 0;
    for (int32_t i = 0; i < sectorCount; ++i)
    {
        for (CPlayer* other : zone->GetSectorManager().GetSectorPlayers(sectors[i].x, sectors[i].y))
        {
            if (other != exclude)
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
        for (CPlayer* other : zone->GetSectorManager().GetSectorPlayers(sectors[i].x, sectors[i].y))
        {
            if (other == exclude)
                continue;
            ++_tickMembershipSends;   // 멤버십 복사량(횟수) — OFF의 SendXxx 호출당 1과 동일 집계 (플레이어만 — 뱀까지 세던 옛 값과 단위가 다르다)
            if (_sessionHub.RequestSend(other, pMsg))
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
void CSightBroadcaster::RegisterOutboundToSectors(CZone* zone, int32_t zoneId,
                                            const CSectorManager::SectorPos* sectors, int32_t sectorCount,
                                            CSerialBuffer* pMsg, CCharacter* exclude)
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
void CSightBroadcaster::MarkMoveDirty(CCharacter* player)
{
    if (player->_moveDirty)
        return;
    player->_moveDirty = true;
    _sectorOutbox.dirtyMovers.push_back(player);
}

// 틱 끝: dirty 플레이어를 (zone, sector)별로 묶어 그 섹터 주변에 송신.
// ← 생산: MarkMoveDirty (틱 중 _sectorOutbox.dirtyMovers 적재).
void CSightBroadcaster::FlushSectorUpdates()
{
    if (_sectorOutbox.dirtyMovers.empty())
        return;

    // [계측] 묶음 빌드+송신 전체를 enqueue 축에 합산 (baseline의 broadcast_enqueue와 같은 비용종류로 A/B 비교)
    auto measT0 = std::chrono::steady_clock::now();

    // (zoneId, sectorY, sectorX)로 정렬 → 같은 섹터가 연속 구간이 되어 그룹 단위로 처리
    std::sort(_sectorOutbox.dirtyMovers.begin(), _sectorOutbox.dirtyMovers.end(),
        [](CCharacter* a, CCharacter* b)
        {
            if (a->_zoneId != b->_zoneId)   return a->_zoneId < b->_zoneId;
            if (a->_sectorY != b->_sectorY) return a->_sectorY < b->_sectorY;
            return a->_sectorX < b->_sectorX;
        });

    const size_t total = _sectorOutbox.dirtyMovers.size();
    size_t i = 0;
    while (i < total)
    {
        CCharacter* head = _sectorOutbox.dirtyMovers[i];
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
    for (CCharacter* p : _sectorOutbox.dirtyMovers)
        p->_moveDirty = false;
    _sectorOutbox.dirtyMovers.clear();

    auto measT1 = std::chrono::steady_clock::now();
    _tickBroadcastEnqueueUs += std::chrono::duration_cast<std::chrono::microseconds>(measT1 - measT0).count();
}

// 섹터 좌표 기준 주변 9섹터에 묶음 패킷 전송.
// BroadcastAroundSector의 소유권(배치 AddRef)·송신 메트릭 패턴을 그대로 복제 (본인 포함).
void CSightBroadcaster::BroadcastSectorPacket(CZone* zone, int32_t sectorX, int32_t sectorY, CSerialBuffer* pMsg)
{
    _broadcastBuffer.clear();
    zone->GetSectorManager().GetAroundPlayers(sectorX, sectorY, _broadcastBuffer, nullptr);

    _monitor._gameLoop._broadcastCalls.Inc();
    _monitor._gameLoop._broadcastTargets.Add(static_cast<int64_t>(_broadcastBuffer.size()));   // 플레이어만 센다

    // 배치 AddRef (원자연산 N→1) — 목록이 플레이어뿐이라 선카운트가 곧 크기다
    const size_t validCount = _broadcastBuffer.size();
    if (validCount > 0)
        pMsg->AddRef(static_cast<int64_t>(validCount));

    const int dataSize = pMsg->GetDataSize();
    size_t sentPkts = 0;
    for (CPlayer* other : _broadcastBuffer)
    {
        if (_sessionHub.RequestSend(other, pMsg))
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

// 존 최초 등록 — 칸 배열 크기 확정 (맵 크기는 부팅 후 불변). 방송(items)/직송(directItems)
// 어느 쪽이 먼저 등록해도 1회만 초기화 (ready 플래그 — items.empty()는 직송 선행 시 오판).
//   격자 크기·칸 번호 식·주변 9섹터 규칙은 전부 섹터 관리자 것(CellCount·CellIndex·CellPos·GetAroundSectorList)을 쓴다 —
//   여기 사본을 두지 않는다(아키텍처 전수검사 2번, 10-06).
void CSightBroadcaster::EnsureZonePending(CZone* zone, SectorOutbox::ZonePending& zp)
{
    if (zp.ready)
        return;
    zp.ready = true;
    const size_t total = static_cast<size_t>(zone->GetSectorManager().CellCount());
    zp.items.resize(total);
    zp.recvEpoch.assign(total, 0);
#if USE_MEMBERSHIP_DIGEST
    zp.directItems.resize(total);
#endif
}

// 보류 등록 — (zoneId, 섹터)에 버퍼 적재. 버퍼 소유권 1은 FlushSectorSends 4)에서 회수.
// _broadcastCalls는 소스 아이템 단위로 집계 (기존 per-청크/per-채팅 호출 카운트와 동일 의미 → A/B 비교 가능).
// → 배출: 틱 끝 FlushSectorSends에서 _sectorOutbox.pendingByZone을 수신섹터 digest로 연접·배포.
void CSightBroadcaster::RegisterSectorItem(CZone* zone, int32_t zoneId, int32_t sectorX, int32_t sectorY,
                                     CSerialBuffer* pMsg)
{
    auto& zp = _sectorOutbox.pendingByZone[zoneId];
    EnsureZonePending(zone, zp);
    const int32_t idx = zone->GetSectorManager().CellIndex(sectorX, sectorY);
    if (zp.items[idx].empty())
        _sectorOutbox.touchedSectors.emplace_back(zoneId, idx);
    zp.items[idx].push_back(pMsg);
    _monitor._gameLoop._broadcastCalls.Inc();
}

#if USE_MEMBERSHIP_DIGEST
// [Phase 4] 직송 보류 등록 — (zoneId, 섹터)에 "그 섹터 주민에게만" 전달할 버퍼 적재 (멤버십 CREATE/DELETE 전용).
// 버퍼 소유권 1은 FlushSectorSends 4)에서 회수. _broadcastCalls는 안 늘린다(방송 지표 오염 방지) —
// 멤버십 횟수는 배포 시점에 _tickMembershipSends로 승계 (OFF의 타겟당 1 집계와 동일 산식).
void CSightBroadcaster::RegisterSectorDirectItem(CZone* zone, int32_t zoneId, int32_t sectorX, int32_t sectorY,
                                           CSerialBuffer* pMsg)
{
    auto& zp = _sectorOutbox.pendingByZone[zoneId];
    EnsureZonePending(zone, zp);
    const int32_t idx = zone->GetSectorManager().CellIndex(sectorX, sectorY);
    if (zp.directItems[idx].empty())
        _sectorOutbox.touchedDirectSectors.emplace_back(zoneId, idx);
    zp.directItems[idx].push_back(pMsg);
}
#endif

// 틱 끝: ① 이동 dirty → 섹터별 번들 빌드·보류 ② 수신섹터 후보 수집 ③ 연접·배포 ④ 일괄 해제.
// ← 생산: MarkMoveDirty(_sectorOutbox.dirtyMovers) + RegisterSectorItem(_sectorOutbox.pendingByZone)
//         + RegisterSectorDirectItem(직송, USE_MEMBERSHIP_DIGEST).
void CSightBroadcaster::FlushSectorSends()
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
            [](CCharacter* a, CCharacter* b)
            {
                if (a->_zoneId != b->_zoneId)   return a->_zoneId < b->_zoneId;
                if (a->_sectorY != b->_sectorY) return a->_sectorY < b->_sectorY;
                return a->_sectorX < b->_sectorX;
            });

        const size_t total = _sectorOutbox.dirtyMovers.size();
        size_t i = 0;
        while (i < total)
        {
            CCharacter* head = _sectorOutbox.dirtyMovers[i];
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

        for (CCharacter* p : _sectorOutbox.dirtyMovers)
            p->_moveDirty = false;
        _sectorOutbox.dirtyMovers.clear();
    }

    // ── 2) 수신섹터 후보 = 소스(touched)의 주변 9섹터 합집합 (epoch 마킹으로 중복 제거, clear 불필요) ──
    //   어느 9칸인가(시야 규칙)와 칸 번호 ↔ 좌표는 섹터 관리자가 정한다(GetAroundSectorList · CellPos · CellIndex) —
    //   여기 사본을 두지 않는다. 존 조회는 3)과 같은 꼴(틱 중 존 소멸이면 건너뜀 — 보류물 해제는 4)가 책임).
    ++_sectorOutbox.flushEpoch;
    _sectorOutbox.receiverSectors.clear();
    for (const auto& t : _sectorOutbox.touchedSectors)
    {
        CZone* zone = _mapManager.GetZone(t.first);
        if (zone == nullptr)
            continue;
        const CSectorManager& sectors = zone->GetSectorManager();
        auto& zp = _sectorOutbox.pendingByZone[t.first];
        int32_t sx = 0, sy = 0;
        sectors.CellPos(t.second, sx, sy);
        CSectorManager::SectorPos around[CSectorManager::MAX_AROUND_SECTORS];
        int32_t aroundCount = 0;
        sectors.GetAroundSectorList(sx, sy, around, aroundCount);
        for (int32_t i = 0; i < aroundCount; ++i)
        {
            const int32_t ridx = sectors.CellIndex(around[i].x, around[i].y);
            if (zp.recvEpoch[ridx] != _sectorOutbox.flushEpoch)
            {
                zp.recvEpoch[ridx] = _sectorOutbox.flushEpoch;
                _sectorOutbox.receiverSectors.emplace_back(t.first, ridx);
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
        const CSectorManager& sectors = zone->GetSectorManager();
        auto& zp = _sectorOutbox.pendingByZone[r.first];
        int32_t rx = 0, ry = 0;
        sectors.CellPos(r.second, rx, ry);

        const std::vector<CPlayer*>& residents = sectors.GetSectorPlayers(rx, ry);
        if (residents.empty())
            continue;   // 받을 플레이어가 없으면(뱀만 있는 섹터 포함) 연접 비용도 생략

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
        // 이웃 9섹터 — 시야 규칙은 섹터 관리자 것(GetAroundSectorList). 순서(y 바깥·x 안쪽, 오름차순)도 그쪽이 정한다
        CSectorManager::SectorPos around[CSectorManager::MAX_AROUND_SECTORS];
        int32_t aroundCount = 0;
        sectors.GetAroundSectorList(rx, ry, around, aroundCount);
        for (int32_t i = 0; i < aroundCount; ++i)
        {
            for (CSerialBuffer* buf : zp.items[sectors.CellIndex(around[i].x, around[i].y)])
            {
                const char* p = buf->GetReadBufferPtr();
                _sectorOutbox.concatBuf.insert(_sectorOutbox.concatBuf.end(), p, p + buf->GetDataSize());
                ++itemCount;
            }
        }
        if (itemCount == 0)
            continue;

        const int concatSize = static_cast<int>(_sectorOutbox.concatBuf.size());
        // targets = (아이템 × 주민) 전달 쌍 수 — 기존 Σ(호출별 gather 인원)과 재배열만 다르고 총량 동일. 주민 = 플레이어만
#if USE_MEMBERSHIP_DIGEST
        // [Phase 4] 직송(멤버십)은 방송 지표에서 제외 — OFF의 FanoutToSectors도 _broadcastTargets를 안 늘렸다.
        // 대신 멤버십 횟수를 OFF의 타겟당 1 집계(플레이어만, FanoutToSectors 2패스와 동일 산식)로 승계
        // → membership_sends_rate가 A/B 등가 통제지표로 유지된다.
        targets += static_cast<int64_t>(residents.size()) * (itemCount - directCount);
        _tickMembershipSends += static_cast<int64_t>(residents.size()) * directCount;
#else
        targets += static_cast<int64_t>(residents.size()) * itemCount;
#endif
        for (CPlayer* other : residents)
        {
            if (_sessionHub.RequestSendRaw(other, _sectorOutbox.concatBuf.data(), concatSize))
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
