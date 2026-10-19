// ==========================================================================
// CSightBroadcaster — 시야 방송: 누가 누구를 보나가 바뀔 때와 주변에 알릴 때 (게임 서버 나누기 3 단계 · B2)
//
// [책임]
//  - 섹터 바뀜 처리(시야 진입·이탈의 CREATE/DELETE) · 같은 틱 이동자 쌍 보정 — 무엇이 바뀌었나는 존이 정한다(CZone::TakeSightChanges)
//  - 주변에 보내기 · 존 입장/등장/퇴장 방송 · 이동 알림(묶음 dirty ↔ 즉시 방송) · 주기 위치 동기화
//  - 틱 지연 송신 묶음(섹터 묶음 · 수신섹터 digest · 멤버십 직송) — 틱 중 모으고 틱 끝에 비운다
//  - 땅의 아이템 보이기·지우기(떨어짐 · 시야 진입·이탈 · 주움·사라짐) · 존 알림(IZoneEvents — 존이 정한 걷기·멈춤)
//  - 방송 비용 계측(주변 모으기 · 수신자별 처리 · 멤버십 횟수·시간 · 쌍 보정) — 틱 끝에 한 번 지표로 반영
//
// [경계]
//  한 사람에게 보내기와 수신자 한 명에게 한 장 넣기는 세션 허브(CSessionHub)로만 한다 — 이 조각은 세션 번호를 모른다(허브가 안다).
//  존·섹터 관리자는 읽기만 한다(주민 목록 · 대기열이 정한 명단). 누가 존에 들어가고 나가는지는 부르는 쪽(배치·입장)이 정한다.
//  틱 안 차례(섹터 바뀜 → … → 주기 동기화 → 송신 비우기 → 틱 끝 지표)는 게임 루프(CGameServer::GameLoopThread)가 정한다.
//
// [모르는 것]
//  전투·보상·가방·파티·입장 절차. 받는 쪽은 언제나 플레이어다(몬스터는 받는 쪽이 못 된다 — 섹터 목록의 플레이어 칸만 돈다).
//  남의 몫을 얹은 자리 둘 — 경계에 막혀 멈춘 플레이어에 저장 표시(_dbDirty)를 켠다(OnCharacterMoveStop · USE_DB_WORKER) ·
//  주기 동기화 순회에 동접 게이지(_enter._inGame)를 얹었다(TickPeriodicSync). 둘 다 옮기기 전 게임 서버에서 그 자리에 있던 그대로다.
// ==========================================================================
#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "BuildConfig.h"         // 송신 경로 토글(USE_SECTOR_AGGREGATION 등) · USE_DROP — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include <Protocol/Protocol.h>   // SpawnReason · GroundItemAppear · GroundItemGone — 와이어 enum
#include "Zone.h"                // 존 — 그 알림(IZoneEvents)을 여기서 구현한다 · 시야 명단(SightMove · SightPairFix)
#include "SectorManager.h"       // SectorPos — 섹터 목록 인자
#if USE_DROP
#include "GroundItem.h"          // CGroundItem — 땅의 아이템 한 개(값)
#endif

class CPlayer;
class CCharacter;
class CSerialBuffer;
class CSessionHub;
class CMapManager;
class CMonitorManager;
class CGameMonitor;

// 존 알림(IZoneEvents)은 공개 상속 — 게임 서버가 이것을 존 틱·몬스터 묶음에 알림 통로로 넘긴다. 알림 함수 자체는 비공개(통로로만 불린다).
class CSightBroadcaster : public IZoneEvents
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 넷(세션 허브 · 맵 관리자 · 코어 지표 · 게임 지표)보다 뒤에 이것을 선언해야 한다
    //   (멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    CSightBroadcaster(CSessionHub& sessionHub, CMapManager& mapManager, CMonitorManager& monitor, CGameMonitor& gameMonitor)
        : _sessionHub(sessionHub), _mapManager(mapManager), _monitor(monitor), _gameMonitor(gameMonitor) {}

    // 프레임 재사용 컨테이너 reserve (워밍업 realloc 방지) — 게임 서버 모드의 Init 이 부른다.
    //   maxPerChannel = 첫 맵의 채널 정원(주변 9섹터 단위 버퍼) · maxClients = 서버 전체(이동 묶음 dirty)
    void Init(int32_t maxPerChannel, int maxClients);

    // ── 틱 — 게임 루프가 이 차례로 부른다(CGameServer::GameLoopThread) ──
    // 3) 섹터 변경 배치 처리 — 존마다 대기열이 정한 명단(이동 · 같은 틱 쌍)을 보내기만. 멤버십 구간 시간을 잰다.
    void ProcessSightChanges();
    // 4) 주기적 위치 동기화 — 13틱마다 MOVING 플레이어 중 2칸 넘게 간 것만 주변에. 같은 순회에 동접 게이지(_enter._inGame)를 얹었다.
    void TickPeriodicSync();
    // 4 뒤) 이번 틱 보류물 비우기 — digest(USE_BROADCAST_BUNDLE) 또는 섹터 묶음(USE_SECTOR_AGGREGATION). 실제 송신(flush)은 네트워크 몫.
    void FlushSends();
    // 틱 끝) 비용종류별 계측 다섯을 지표에 한 번 반영하고 0 으로.
    void ReportTickMetrics();
    // 틱 끝 정리가 빈 채널을 지우기 전에 보는 불변식 — 이동 묶음이 비어 있어야 존을 지울 수 있다.
    bool HasDirtyMovers() const { return !_sectorOutbox.dirtyMovers.empty(); }

    // ── 주변에 보내기 ──
    // 주변 브로드캐스트 (CSerialBuffer — 빌더 RefCount=1 버퍼를 소비)
    void BroadcastAroundSector(CZone* zone, CCharacter* player, CSerialBuffer* pMsg, bool excludeSelf = true);

    // ── 이동 알림 헬퍼 ──
    // 이동 시작/정지/동기화 통보의 "묶음(dirty 마킹) ↔ 즉시 브로드캐스트" 분기를 한 곳에 흡수.
    // 호출부는 의도만 표시, 실제 경로는 USE_SECTOR_AGGREGATION이 결정 (동작 불변).
    void NotifyMoveStart(CZone* zone, CCharacter* player);
    void NotifyMoveStop(CZone* zone, CCharacter* player, bool excludeSelf);
    void NotifyMoveSync(CZone* zone, CCharacter* player);

    // 존 입장/퇴장 브로드캐스트
    //   BroadcastEnterZone — 플레이어 입장: 양방향(주변 → 나를 생성, 나 → 주변을 생성)
    //   BroadcastSpawn     — 세션 없는 것(몬스터)의 등장: 주변에 알리기만. 받는 쪽이 없으니 인바운드가 없다
    //   BroadcastLeaveZone — 퇴장의 시야 몫: 주변(옛 섹터 시청자·같은 틱 이동자 포함)에 나를 삭제하고 이동 묶음에서 뺀다.
    //                        존 소속은 안 건드린다 — 존에서 빼는 것은 CPlayerPlacement::RemoveFromZone(게임 서버 나누기 0-③).
    void BroadcastEnterZone(CZone* zone, CPlayer* player, SpawnReason reason);
    void BroadcastSpawn(CZone* zone, CCharacter* who, SpawnReason reason);
    void BroadcastLeaveZone(CZone* zone, CCharacter* player);

    // 본인에게 내 자리(SYNC_POSITION) — 이동 시작 좌표를 받아 주지 않을 때 서버 자리로 되돌린다(CMoveHandler::RecvMoveStart).
    void SendSyncPosition(CPlayer* target);

#if USE_DROP
    // 땅의 아이템이 떨어졌다 — 주변에 CREATE(DROP). 땅 아이템 알림(CItemHandler::OnGroundItemDrop)이 부른다.
    void BroadcastGroundItemCreate(CZone* zone, const CGroundItem& item, int32_t sectorX, int32_t sectorY);
    // 땅의 아이템이 빠졌다 — 주변 + 같은 틱 이동자에게 DELETE. 와이어 사유로 바꾸는 것은 부르는 쪽(CItemHandler::OnGroundItemGone).
    void BroadcastGroundItemDelete(CZone* zone, int32_t sectorX, int32_t sectorY,
                                   int32_t groundId, GroundItemGone wire, int32_t pickerId);
#endif

#if USE_BROADCAST_BUNDLE
    // 보류 등록 — 수신섹터 digest 로 틱 끝에 배포(아래 「수신섹터 digest 경로」). 채팅(CChatHandler::RecvChat)도 이것으로 보낸다.
    void RegisterSectorItem(CZone* zone, int32_t zoneId, int32_t sectorX, int32_t sectorY,
                            CSerialBuffer* pMsg);   // 보류 등록 (버퍼 소유권 1을 digest 배포 후 회수)
#endif

private:
    // 존 알림 (IZoneEvents) — 뜻은 Zone.h. 존이 정한 걷기·멈춤을 위 헬퍼로 보낸다.
    void OnCharacterMoveStart(CZone* zone, CCharacter* character) override;  // 주변에 MOVE_START
    void OnCharacterMoveStop(CZone* zone, CCharacter* character) override;   // 주변에 MOVE_STOP · 플레이어면 저장 대상

    // 섹터 변경 시 시야 진입/이탈 브로드캐스트 — 어느 섹터인가는 존이 정했다(CZone::TakeSightChanges 의 moves)
    void ProcessSectorChange(CZone* zone, const CZone::SightMove& move);

    // 같은 틱에 함께 이동한 쌍 보정 — 틱 끝 일괄 통보가 구조적으로 놓치는 CREATE/DELETE를 보충.
    //   ProcessSectorChange는 이탈/진입 섹터의 "틱 끝 시점" 주민을 읽는데, 상대도 같은 틱에
    //   움직였으면 그 조회에 안 걸린다 → 상호 DELETE 누락(유령) / 상호 CREATE 누락(투명인간).
    //   기존 송신 경로는 그대로 두고 두 패스 모두 놓친 쌍만 보충한다 (중복 없음).
    //   어느 쌍인가는 존이 정했다(CZone::TakeSightChanges 의 pairFixes) — 여기는 보내기와 계수만.
    //   존마다 부른다 — 섹터 이동 대기열이 존 것이고, 다른 존끼리는 서로 안 보인다.
    void SendSightPairFixes(const std::vector<CZone::SightPairFix>& fixes);

    // ── 섹터 묶음 경로 (USE_SECTOR_AGGREGATION) ──
    void MarkMoveDirty(CCharacter* player);   // 즉시 브로드캐스트 대신 dirty 등록 (중복 방지)
    void FlushSectorUpdates();             // 틱 끝: dirty를 섹터별로 묶어 주변에 송신
    // 섹터 좌표 기준 주변 9섹터에 묶음 패킷 전송 (BroadcastAroundSector의 소유권/계측 패턴 복제)
    void BroadcastSectorPacket(CZone* zone, int32_t sectorX, int32_t sectorY, CSerialBuffer* pMsg);

#if USE_BROADCAST_BUNDLE
    // ── 수신섹터 digest 경로 (Phase 3) — FlushSectorUpdates를 대체 ──
    // 소스 섹터별 보류물(이동 번들 + 채팅)을 수신 섹터 기준으로 뒤집어, 이웃 9섹터 내용을
    // raw 바이트 1덩어리로 연접 후 주민당 RequestSendRaw 1회. 같은 섹터 주민은 수신 집합이 동일해 공유 가능.
    //   (보류 등록 RegisterSectorItem 은 위 공개 쪽 — 채팅도 부른다)
    void FlushSectorSends();                        // 틱 끝: 번들 빌드 → 수신섹터 연접 → 배포 → 일괄 해제
#if USE_MEMBERSHIP_DIGEST
    // ── 멤버십 직송 경로 (Phase 4) — 아웃바운드 CREATE/DELETE를 digest에 합류 ──
    // 방송(3×3 이웃 배포)과 달리 "정확히 그 섹터 주민에게만" 전달. 연접 순서는 직송이 방송보다 앞.
    void RegisterSectorDirectItem(CZone* zone, int32_t zoneId, int32_t sectorX, int32_t sectorY,
                                  CSerialBuffer* pMsg);   // 직송 보류 등록 (버퍼 소유권 1을 배포 후 회수)
#endif
#endif

#if USE_MEMBERSHIP_FANOUT_DEDUP
    // 멤버십 아웃바운드 팬아웃 (Phase 1) — 미리 빌드한 버퍼 1개를 여러 섹터의 플레이어에게 배치 AddRef로 전송.
    // BroadcastSectorPacket의 소유권/계측 패턴을 (주변 9섹터가 아닌) 임의 섹터 배열 버전으로 확장. 버퍼 소유권 1을 소비.
    void FanoutToSectors(CZone* zone,
                         const CSectorManager::SectorPos* sectors, int32_t sectorCount,
                         CSerialBuffer* pMsg, CCharacter* exclude);
#if USE_MEMBERSHIP_DIGEST
    // [Phase 4] 아웃바운드 멤버십을 직송 보류로 등록 (+자기 섹터 점프 폴백). 버퍼 소유권 1을 소비.
    void RegisterOutboundToSectors(CZone* zone, int32_t zoneId,
                                   const CSectorManager::SectorPos* sectors, int32_t sectorCount,
                                   CSerialBuffer* pMsg, CCharacter* exclude);
#endif
#endif

    // 패킷별 전송 함수 (Fill + Send)
    // target(받는 쪽)은 CPlayer*, who(그려질 대상)는 CCharacter* — 몬스터가 섹터를 옮기면 who 가 몬스터다.
    //   받는 쪽에 몬스터를 넘기는 코드는 타입이 막는다.
    void SendCreateOtherPlayer(CPlayer* target, CCharacter* who, SpawnReason reason = SpawnReason::NORMAL);
    void SendDeletePlayer(CPlayer* target, CCharacter* who);
#if USE_MEMBERSHIP_INBOUND_BUNDLE
    // [Phase 2] 인바운드 멤버십 배치 — 상대 목록을 배치 상한씩 잘라 mover 1명에게 송신 (엔트리당 _membershipSends 1 집계)
    void SendCreatePlayerBatch(CPlayer* target, CCharacter* const* others, int count);
    void SendDeletePlayerBatch(CPlayer* target, CCharacter* const* others, int count);
#endif

#if USE_DROP
    // 땅의 아이템 하나를 한 사람에게 보여 준다 — 떨어질 때(DROP)·시야에 들어올 때(SEEN) 공용
    void SendGroundItemCreate(CPlayer* target, const CGroundItem& item, GroundItemAppear reason);
    // 한 사람 화면에서 지운다 — 시야 밖(OUT_OF_SIGHT) · 주움(PICKED, pickerId) · 사라짐(EXPIRED) 공용
    void SendGroundItemDelete(CPlayer* target, int32_t groundId, GroundItemGone reason, int32_t pickerId);

    // 같은 틱에 섹터를 넘은 사람 보정 — 땅 아이템이 빠질 때(BroadcastGroundItemDelete) 주변에 DELETE 를 보낸 뒤 부른다.
    //   RecvMoveStart 는 네트워크 처리 자리에서 섹터 등록을 바꾸고 알림은 틱 끝이라, 틱 1단계(줍기)에서는
    //   서버 등록이 그 사람 화면보다 앞서 있다. 「옛 섹터로는 보였고 새 섹터로는 안 보이는」 이동자는
    //   주변 DELETE 에서 빠지고, 틱 끝 이탈 처리 때는 아이템이 이미 없어 영영 안 지워진다 → 여기서 보낸다.
    //   고르는 것은 존(CZone::CollectLateMovers — BroadcastLeaveZone 셋째 덩이와 같은 함수)이다. 대기열이 비면 아무 일도 없다.
    void SendGroundItemDeleteToLateMovers(CZone* zone, int32_t sectorX, int32_t sectorY,
                                          int32_t groundId, GroundItemGone wire, int32_t pickerId);
#endif

    CSessionHub&     _sessionHub;    // 보내기 — 한 사람에게 한 패킷 · 수신자 한 명에게 한 장(RequestSend · RequestSendRaw)
    CMapManager&     _mapManager;    // 존 찾기(zoneId → 존) · 존마다 돌기 — 읽기만
    CMonitorManager& _monitor;       // 코어 지표 — 배치 송신 경로에서 코어 대신 세는 자리가 있다(보낸 수·바이트 · 방송 호출·대상 · 구간 시간)
    CGameMonitor&    _gameMonitor;   // 게임 지표 — 멤버십 셋 · 늦은 이동자 DELETE · 동접 게이지

    static constexpr int SYNC_INTERVAL_FRAMES = 13;          // 13 × 40ms ≈ 500ms 주기적 위치 동기화
    static constexpr float SYNC_DISTANCE_THRESHOLD_SQ = 4.0f; // 델타 동기화 임계값 제곱 (2타일)
    int _syncFrameCount = 0;

    // 프레임 재사용 버퍼 (힙 할당 방지 — clear()로 capacity 유지)
    std::vector<CPlayer*> _broadcastBuffer;          // BroadcastAroundSector 전용 — 받는 쪽은 플레이어뿐
    std::vector<CPlayer*> _eventAroundBuffer;        // 접속/해제/존이동 전용 — 같음
    std::vector<CCharacter*> _lateMoverBuffer;       // 늦은 이동자 조회(CZone::CollectLateMovers) 전용 — 퇴장·땅 아이템이 채우고 그 자리에서 다 쓴다

    // ── 섹터 아웃박스 ── 틱-지연 배칭 상태를 한 덩어리로 응집.
    //   생산(틱 중): MarkMoveDirty / RegisterSectorItem  →  배출(틱 끝): FlushSectorUpdates / FlushSectorSends
    //   흩어져 있던 상태를 묶어 "위에서 모아 아래서 비운다"의 대상이 한눈에 보이게 함 (동작 불변).
    struct SectorOutbox
    {
        // USE_SECTOR_AGGREGATION: 이번 틱 이동/sync로 상태가 바뀐 플레이어 (틱 끝에 섹터별 묶음 송신)
        std::vector<CCharacter*> dirtyMovers;

#if USE_BROADCAST_BUNDLE
        // USE_BROADCAST_BUNDLE: 존별 섹터 그리드에 이번 틱 보류물(번들+채팅)을 모았다가 틱 끝 digest로 배포.
        // zoneId 키로 보관 — 틱 중 존이 사라져도 해제 루프는 존 포인터 없이 안전 (기존 GetZone null 가드 패턴).
        struct ZonePending
        {
            bool ready = false;                              // 칸 배열을 잡았나(존 최초 등록 시 1회). 격자 크기·칸 번호는 섹터 관리자 것을 쓴다 — 사본 없음
            std::vector<std::vector<CSerialBuffer*>> items;  // flatIdx → 보류 버퍼들 (등록순)
            std::vector<uint64_t> recvEpoch;                 // 수신섹터 중복 마킹 (틱당 epoch 증가로 clear 생략)
#if USE_MEMBERSHIP_DIGEST
            // USE_MEMBERSHIP_DIGEST: 직송(그 섹터 주민 전용) 보류물 — 멤버십 CREATE/DELETE.
            std::vector<std::vector<CSerialBuffer*>> directItems;   // flatIdx → 직송 버퍼들 (등록순)
#endif
        };
        std::unordered_map<int32_t, ZonePending> pendingByZone;
        std::vector<std::pair<int32_t, int32_t>> touchedSectors;   // (zoneId, flatIdx) — 보류물 있는 소스 섹터
#if USE_MEMBERSHIP_DIGEST
        std::vector<std::pair<int32_t, int32_t>> touchedDirectSectors;  // (zoneId, flatIdx) — 직송물 있는 섹터
#endif
        std::vector<std::pair<int32_t, int32_t>> receiverSectors;  // (zoneId, flatIdx) — 이번 틱 수신 후보 (재사용)
        std::vector<char> concatBuf;                               // 수신섹터별 연접 버퍼 (재사용)
        uint64_t flushEpoch = 0;                                   // 64비트 — wrap 없음 (0 = 미마킹 초기값과 충돌 방지)
#endif
    } _sectorOutbox;

#if USE_BROADCAST_BUNDLE
    // 존 최초 등록 시 그리드 확정 — 방송(items)/직송(directItems) 어느 쪽이 먼저 와도 1회만.
    // (선언 위치: 중첩 타입 SectorOutbox::ZonePending을 인자로 받아 struct 정의 뒤에 와야 함)
    void EnsureZonePending(CZone* zone, SectorOutbox::ZonePending& zp);
#endif

#if USE_MEMBERSHIP_INBOUND_BUNDLE
    // USE_MEMBERSHIP_INBOUND_BUNDLE: 인바운드 멤버십 배치 수집 전용 (ProcessSectorChange 내 수집→직렬화→송신 완결, 이월 없음)
    std::vector<CCharacter*> _membershipInboundBuffer;
#endif

    // 비용종류별 계측 — 틱 내 누적 후 틱 끝 1회 모니터 반영 (단일 게임루프 스레드 전용 → 누적 자체엔 원자연산 불필요)
    // BroadcastAroundSector가 network/game_logic/broadcast_sync 어느 단계에서 호출돼도 이 멤버에 합산된다.
    int64_t _tickBroadcastGatherUs = 0;   // GetAroundPlayers 주변 모으기
    int64_t _tickBroadcastEnqueueUs = 0;  // 수신자별 처리(복사 포함)
    int64_t _tickMembershipSends = 0;     // 멤버십 변경 복사(BroadcastAroundSector 밖 경로) 송신 횟수
    int64_t _tickMembershipUs = 0;        // 멤버십 송신(ProcessSectorChange 구간) 시간 — 틱 끝 _membershipCostUs로 반영
    int64_t _tickPairFixes = 0;           // 같은 틱 이동자 쌍 보정 발동 수 — 틱 끝 _membershipPairFixes로 반영
};
