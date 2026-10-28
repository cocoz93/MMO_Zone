// ==========================================================================
// CEntryHandler — 입장·존 이동 요청: 접속 · 입장 대기표 · 불러온 캐릭터 세우기 · 중복 접속 · 존 이동·포털 요청 (게임 서버 나누기 12 단계 · C1)
//
// [책임]
//  - 받은 패킷 셋의 처리기와 그 등록(RegisterRoutes — 존 이동 · 포털 · 입장 요청(USE_LOGIN — 끄면 크기만)) — 컨텐츠 모듈이 자기 줄을 등록한다
//  - 접속 이벤트(OnConnected) → 대기표에 자리 잡기 · 불러오기 요청(USE_DB_WORKER) 또는 곧바로 스폰
//  - 입장 대기표(_pendingEnter — 입장 대기 · 불러오는 중)와 그 게이지 둘 · 끊긴 세션의 자리 비우기(DropPendingEnter) ·
//    동접 게이지(_enter._inGame — 존 정원 카운터의 합, 약 0.5초마다 · TickInGameGauge) · 같은 순회의 존 수 게이지 둘(_enter._zones · _zonesIdle)
//  - 불러오기 결과 알림(ICharacterLoadEvents — 찾음 · 없음 · 실패 판정 → 자리·레벨·가방·쿨타임) → 스폰(SpawnPlayer — 표시 속성 · 스탯 세우기 · 도착 차례)
//  - 같은 계정 걷어내기(USE_LOGIN) · 존 이동 요청(맵 관리자의 목적지 고르기 → 배치) · 포털 요청(판정 넷 → 방 맵이면 CPartyHandler)
//
// [경계]
//  저장·불러오기 왕복은 캐릭터 저장소(CCharacterStore), 존에 넣기·옮기기·떠나보내기와 도착 차례는 배치(CPlayerPlacement), 세션 칸·번호 발급·끊기 요청은 세션 허브.
//  목적지 고르기(정원 · 같은 존 · 운영자 맵 지정)는 맵 관리자, 포털 표는 기획 데이터, 방 들어가기 판정은 CPartyHandler.
//  끊김 이벤트의 분기(플레이어가 있으면 떠나보내기 · 없으면 대기표만)는 게임 서버가 든다 — 대기표 몫만 이 클래스 함수를 부른다.
//
// [모르는 것]
//  전투·보상·가방 규칙. 레벨·스탯 규칙은 CStatSystem 에 맡기고 부르기만 한다.
// ==========================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "BuildConfig.h"     // USE_DB_WORKER · USE_LOGIN · USE_INVENTORY · USE_COMBAT · USE_EXP · USE_PARTY — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include "DB/DBWorker.h"     // DBResult · DB_HP_MP_UNSAVED(입장 배치 구조체의 기본값 — 토글을 끈 빌드도 쓴다)
#include "CharacterStore.h"  // USE_DB_WORKER — ICharacterLoadEvents (헤더 안에서 스스로 토글을 본다)

class CPlayer;
class CZone;
class CSerialBuffer;
class CPacketRoutes;
class CSessionHub;
class CPlayerPlacement;
class CMapManager;
class CGameMonitor;
class CGameDataFiles;
#if USE_PARTY
class CPartyHandler;
#endif
#if USE_COMBAT
class CStatSystem;
#endif

// 불러오기 결과 알림은 공개 상속 — 게임 서버가 캐릭터 저장소(_characterStore)에 이것을 알림 받는 쪽으로 꽂는다
//   (CSightBroadcaster 의 IZoneEvents 와 같은 꼴). 알림 함수 자체는 비공개 — 저장소가 인터페이스로만 부른다.
class CEntryHandler
#if USE_DB_WORKER
    : public ICharacterLoadEvents
#endif
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(세션 허브 · 배치 · 저장소 · CPartyHandler · 스탯 규칙 · 맵 관리자 · 게임 지표 · 기획 데이터)보다
    //   뒤에 이것을 선언해야 한다(멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    //   거꾸로 저장소는 이것보다 앞에 세워지며 이것의 참조를 묶기만 한다(알림 받는 쪽 — 게임 서버가 *this 를 넘기던 것과 같은 꼴).
    //   기본 맵 번호(새 캐릭터 · 마을 부활 · 저장 맵 없음)는 맵 관리자에게 묻는다(CMapManager::GetDefaultMapId — 게임 서버 나누기 14 단계 전에는 게임 서버가 들고 빌려 줬다).
    CEntryHandler(CSessionHub& sessionHub, CPlayerPlacement& placement,
#if USE_DB_WORKER
                  CCharacterStore& characterStore,
#endif
#if USE_PARTY
                  CPartyHandler& partyHandler,
#endif
#if USE_COMBAT
                  CStatSystem& statSystem,
#endif
                  CMapManager& mapManager, CGameMonitor& gameMonitor, const CGameDataFiles& gameDataFiles)
        : _sessionHub(sessionHub), _placement(placement)
#if USE_DB_WORKER
        , _characterStore(characterStore)
#endif
#if USE_PARTY
        , _partyHandler(partyHandler)
#endif
#if USE_COMBAT
        , _statSystem(statSystem)
#endif
        , _mapManager(mapManager), _gameMonitor(gameMonitor), _gameDataFiles(gameDataFiles) {}

    // 대기표 잡기 — 게임 서버 모드의 Init 이 세션 허브의 칸을 잡은 뒤 부른다(같은 크기·같은 색인). DB 를 끈 빌드는 할 일이 없다.
    void Init(int maxClients);

    // 플레이어 이동 속도(칸/초) — 게임 서버의 SetPlayerSpeed(main 이 Start 전에 부른다)가 넘긴다. 새로 만드는 캐릭터부터 적용된다.
    void SetPlayerSpeed(int playerSpeed) { _playerSpeed = playerSpeed; }

    // 받은 패킷 등록 — 존 이동 · 포털 · 입장 요청. 게임 서버의 등록(RegisterRoutes)이 부른다. false = 등록 실패(기동 거절).
    bool RegisterRoutes(CPacketRoutes& routes);

    // 접속 이벤트 — 게임 서버의 네트워크 이벤트 꺼내기(ProcessNetworkEvents)가 부른다.
    void OnConnected(int64_t sessionId);

#if USE_DB_WORKER
    // 끊김 — 플레이어가 아직 없는 세션(입장 대기 · 불러오는 중)의 대기표 자리를 비운다. 게임 서버의 끊김 분기가 부른다.
    //   뒤늦게 오는 불러오기 결과는 대기표에서 주인을 못 찾아 orphaned 로 세어진다.
    void DropPendingEnter(int64_t sessionId);
#endif

    // 틱 4(주기 위치 동기화 바로 뒤) — 약 0.5초마다 동접 게이지(_enter._inGame)를 존 정원 카운터의 합으로 맞춘다.
    //   대기표 게이지 둘과 한 묶음이라 여기 둔다 — 조용해지면 동접 = 게임 중 + 불러오는 중 + 입장 대기(GameMonitor.h).
    //   들고 날 때 ±1 하지 않고 존에서 다시 더하는 까닭: 존 정원 카운터가 정본이다 — 자리마다 ±1 하면 한 자리만 빠져도 영영 어긋나고,
    //   그러면 이 게이지를 쓰는 탐지(세션 없이 남은 플레이어 — GameMonitor.h 의 등식)가 제 숫자를 못 믿게 된다.
    //   같은 순회에서 존 수 · 쉬는 존 수(CZone::IsIdle)도 센다 — 존을 따로 한 번 더 훑지 않으려고. 쉬기가 실제로 도는지 보는 값이다.
    void TickInGameGauge();

    // 동접 게이지 주기 — 13 × 40ms ≈ 0.5초. 옮기기 전 CSightBroadcaster 의 주기 동기화(SYNC_INTERVAL_FRAMES)에 얹혀 돌던 주기 그대로다
    //   (노출 설명 mmo_players_ingame 의 「약 0.5초마다」). 존마다 덧셈 한 번이라 값을 바꿔도 부담은 없다.
    static constexpr int IN_GAME_GAUGE_INTERVAL_FRAMES = 13;

private:
    // ── 입장(스폰) ──
    // 불러온 결과를 "스폰이 쓸 꼴"로 바꾼 것. 판정하는 쪽(OnCharacterLoaded·OnConnected)이
    //   채우고 SpawnPlayer 가 쓴다. 판정과 스폰을 가른 이유 — 분기(찾음·없음·맵 없음·좌표 불량)는
    //   넷인데 스폰 절차는 하나다.
    //   ★ USE_DB_WORKER 밖에 둔다 — 토글을 끈 빌드의 접속 처리도 SpawnPlayer 를 부른다.
    //   ★ level·exp 가 여기 있는 이유 — 스폰이 패킷을 보내기 "전"에 값이 서 있어야 한다.
    //     스폰 뒤에 바깥에서 넣으면 그 패킷이 기본값을 싣고 나간다.
    struct EnterPlacement
    {
        bool    hasPos  = false;   // false = 그 맵의 들어오는 자리(CZone::CalcSpawnPos — 시작 구역 · 없으면 무작위)
        float   x       = 0.0f;
        float   y       = 0.0f;
        bool    dbDirty = true;    // 스폰 자리를 다음 주기에 저장할까 (DB 와 같은 값으로 들어왔으면 false)
        int32_t level   = 1;       // 불러온 값. 새 캐릭터·못 읽음이면 기본값 그대로
        int64_t exp     = 0;
        int32_t hp      = DB_HP_MP_UNSAVED;   // 불러온 체력 — 모름(-1)이면 가득. 죽은 채 저장된 0 은 판정하는 쪽이 부활 규칙대로 바꿔 넘긴다
        int32_t mp      = DB_HP_MP_UNSAVED;   // 불러온 마력 — 모름(-1)이면 가득
#if USE_DB_WORKER && USE_INVENTORY
        // 불러온 가방 줄이 든 결과 — 찾음·없음 분기에서 채운다(없음이어도 가방 줄은 있을 수 있다). nullptr = 가방 없음.
        //   회신 큐의 원소를 가리킨다 — OnCharacterLoaded 가 SpawnPlayer 를 동기로 부르는 동안만 살아 있다.
        const DBResult* loaded = nullptr;
#endif
#if USE_DB_WORKER
        // 불러온 쿨타임·버프 묶음(characters.skill_cooldowns · buffs)이 든 결과 — 찾음 분기에서만 채운다. nullptr = 되살릴 것 없음(새 캐릭터).
        //   loaded 와 같은 수명이다. 풀기·검사·칸 채우기는 SpawnPlayer 의 RestoreCooldownsAndBuffs 가 한다.
        const DBResult* cooldownsAndBuffs = nullptr;
        bool            dropBuffs         = false;   // 죽은 채 저장된 행 — 버프는 버린다(죽으면 버프가 지워지는 규칙). 쿨타임은 되살린다(죽어도 남는 규칙)
#endif
    };

    // 플레이어를 만들어 존에 넣고 본인·주변에 알린다 (OnConnected 의 몸이 옮겨 온 것).
    //   존은 호출부가 구해서 준다 — 좌표 검사에 그 존의 크기가 필요해서 판정하는 쪽이 이미 들고 있다.
    //   반환 nullptr = zone 이 nullptr(채널을 못 만듦) → 호출부가 세션을 끊는다. 존 입장 자체는 실패가 없다(CZone::EnterZone).
    CPlayer* SpawnPlayer(int64_t sessionId, int32_t actorId, int64_t accountId,
                         CZone* zone, const EnterPlacement& place);

#if USE_DB_WORKER
    // 불러오기 결과 알림 (ICharacterLoadEvents) — 1건 처리: 판정 후 스폰. 회신 꺼내기(CCharacterStore::ProcessDbResults)가 부른다.
    void OnCharacterLoaded(const DBResult& result, int64_t nowNs) override;

    // 대기표 — 접속은 했지만 아직 게임에 못 들어온 세션. 세션은 아래 둘 중 하나이거나 "게임 중"이고,
    //   셋 중 둘일 수는 없다. sessionId == -1 이 빈 자리다(CPlayer::_sessionId 와 같은 약속).
    //   세션 번호 "전체"를 들고 있어야 같은 자리에 다른 사람이 들어왔을 때 옛 결과를 가려낸다.
    //   ★ 접속한 세션은 반드시 이 표에 자리를 잡는다 — 어느 표에도 없는 세션이 생기면
    //     "동접 = 게임 중 + 불러오는 중 + 입장 대기" 등식이 늘 틀린 채로 울려 쓸모가 없어진다.
    enum class EnterState : uint8_t
    {
        WAITING_ENTER,   // 접속함. 입장 요청(C2S_ENTER_GAME)을 기다린다 — USE_LOGIN=1 에서만 생긴다
        LOADING,         // 입장 요청을 받아 조회를 던졌다. 결과를 기다린다
    };
    struct PendingEnter
    {
        int64_t    sessionId = -1;
        int64_t    accountId = 0;   // [입장 대기] 구간에서는 아직 0 이다 — 입장 요청이 와야 정해진다
        int32_t    actorId   = 0;   // 같음. 캐릭터 번호도 입장 요청 때 뽑는다
        EnterState state     = EnterState::WAITING_ENTER;
    };
    // 대기표를 바꾸는 자리는 이 셋뿐이다 — 게이지 둘(mmo_enter_waiting·mmo_enter_pending)도 여기서만 움직인다.
    //   접속·입장요청·결과·끊김 네 곳이 제각기 올리고 내리면 한 곳만 빠져도 게이지가 영영 안 맞는다.
    void SetPendingEnter(int64_t sessionId, int64_t accountId, int32_t actorId, EnterState state);
    bool PromotePendingEnter(int64_t sessionId, int64_t accountId, int32_t actorId);  // [입장 대기] → [불러오는 중]
    bool TakePendingEnter(int64_t sessionId, PendingEnter& out);                      // 세션 번호 전체가 같으면 꺼내고 비운다
    void AdjustPendingCount(EnterState state, int delta);                             // 상태별 수 + 게이지를 한 자리에서 움직인다
#endif

#if USE_LOGIN
    // ── 입장 요청 (USE_LOGIN) ──
    // 이 핸들러만 OnReceived 의 FindPlayer 문턱 "앞"에서 불린다 — 입장 요청이 올 때는 아직
    //   플레이어가 없어서, 문턱 뒤에 두면 영영 여기까지 못 온다.
    void RecvEnterGame(int64_t sessionId, CSerialBuffer* pMsg);

    // 같은 계정으로 이미 들어와 있는 세션을 걷어낸다 (게임 중 / 불러오는 중 양쪽).
    //   새 조회를 던지기 "전"에 불러야 한다 — 정리가 넣는 최종 저장이 그 조회보다 큐에서 앞서야
    //   새 접속이 방금 저장한 자리를 읽는다(같은 계정은 같은 워커를 탄다).
    void KickDuplicateAccount(int64_t accountId, int64_t exceptSessionId);

    // 지정 계정 번호의 하한 = 2^31. 입장 요청의 계정 번호는 0(미지정) 아니면 이 값 이상이어야 한다.
    //   자동 번호는 int32 카운터(CSessionHub::AllocActorId)라 2^31 위로 못 올라오므로, 이 아래로 내려온 지정 번호는
    //   0 으로 들어온 남의 접속과 같은 행을 쓰게 된다. 노션의 "100억 이상"은 운용 권장이고 서버가 강제하는 값이 아니다.
    static constexpr int64_t ACCOUNT_ID_MIN = int64_t{ 1 } << 31;
#endif

    // ── 표시 속성 ── (캐릭터 번호 발급은 세션 허브 — CSessionHub::AllocActorId)

    static uint8_t CalcDisplayChar(int32_t playerId);
    static uint8_t CalcColorIndex(int32_t playerId);

    // 존 이동 요청 · 포털 요청 — 목적지를 고르고(맵 관리자 · 포털 표) 배치의 TransferPlayer 로 옮긴다. 거절은 S2C_ZONE_CHANGE_FAIL.
    void RecvZoneChange(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPortalUse(CPlayer* player, CSerialBuffer* pMsg);   // 포털 — 판정 뒤 TransferPlayer 로 포털 표의 도착 자리에

    CSessionHub&          _sessionHub;       // 세션 칸 · 번호 발급 · 끊기 요청 · 찾기
    CPlayerPlacement&     _placement;        // 도착 차례 · 존 옮기기 · 떠나보내기(같은 계정 걷어내기) · 존 이동 실패
#if USE_DB_WORKER
    CCharacterStore&      _characterStore;   // 불러오기 요청 · 가방 줄 넣기 · 쿨타임·버프 되살리기
#endif
#if USE_PARTY
    CPartyHandler&        _partyHandler;     // 방 맵 포털 → 방 입장
#endif
#if USE_COMBAT
    CStatSystem&          _statSystem;       // 레벨 정산 · 스탯 세우기 · 체력·마력 넣기
#endif
    CMapManager&          _mapManager;       // 채널 찾기·만들기 · 존 이동 목적지 · 방 맵 판별 · 기본 맵 번호(GetDefaultMapId)
    CGameMonitor&         _gameMonitor;      // 입장 · 존 이동 · 포털 지표
    const CGameDataFiles& _gameDataFiles;    // 포털 표

    // 플레이어 이동 속도 덮어쓰기(칸/초) — 0 이면 생성자의 기본값을 그대로 쓴다(SetPlayerSpeed)
    int _playerSpeed = 0;

    int _inGameGaugeFrameCount = 0;   // TickInGameGauge 의 주기 세기

#if USE_DB_WORKER
    // 대기표 — 세션 허브의 칸(_sessionHub)과 같은 크기·같은 색인(해시맵 아님). Init 에서 나란히 잡는다.
    //   개수는 배열을 훑지 않으려고 상태별로 따로 센다. 접근은 게임 스레드 단독.
    std::vector<PendingEnter> _pendingEnter;
    int32_t _pendingLoadingCount = 0;   // [불러오는 중] — DB 결과를 기다린다
    int32_t _pendingWaitingCount = 0;   // [입장 대기]  — 입장 요청을 기다린다 (USE_LOGIN=1 에서만 0 이 아니다)
#endif
};
