#include "EntryHandler.h"

#include "SessionHub.h"
#include "PlayerPlacement.h"
#include "PartyHandler.h"     // USE_PARTY — 방 맵 포털
#include "StatSystem.h"       // USE_COMBAT — 레벨 정산 · 스탯 세우기
#include "Combat.h"           // USE_COMBAT — 부활 자리 규칙(RespawnsToTown — 죽은 채 저장된 행)
#include "MapManager.h"
#include "Zone.h"
#include "Player.h"
#include "PortalTable.h"      // PortalDef — 포털 요청
#include "GameMonitor.h"
#include "GameDataFiles.h"    // 포털 표
#include "PacketRoutes.h"
#include "SerialBuffer.h"
#include "NetIoModel.h"       // CSession::ExtractIndex — 대기표 색인(세션 허브의 칸과 같은 색인)
#include <Protocol/Protocol.h>
#include <Common/ErrorLog.h>
#include <cassert>
#include <memory>             // std::make_unique — 스폰

// ==========================================================================
// 존 이동 거절 사유 → 와이어 값 — 이 클래스만 부른다(GameServer.cpp 에서 옮김 · 몸·주석 그대로).
// ==========================================================================
namespace
{
    // 존 이동 거절 사유 → 와이어 값 (MSG_S2C_ZONE_CHANGE_FAIL::reason — 0: 없음, 1: 가득 참, 2: 이미 해당 채널)
    uint8_t ToZoneChangeFailReason(ZoneChangeReject reject)
    {
        switch (reject)
        {
        case ZoneChangeReject::NOT_FOUND: return 0;
        case ZoneChangeReject::FULL:      return 1;
        case ZoneChangeReject::SAME_ZONE: return 2;
        case ZoneChangeReject::NOT_ALLOWED: return 3;
        case ZoneChangeReject::NONE:      break;
        case ZoneChangeReject::COUNT:     break;
        }
        assert(false && "거절이 아닌 결과를 실패 패킷으로 바꾸려 했다");
        return 0;
    }
}

// 받은 패킷 등록 — 옛 등록의 줄과 함정 주석을 그대로 옮겼다(게임 서버 나누기 0-④ 의 등록에서 이 클래스 몫 셋 · 12 단계).
bool CEntryHandler::RegisterRoutes(CPacketRoutes& routes)
{
    bool ok = true;
    ok &= routes.OnPlayer(MsgType::C2S_ZONE_CHANGE, sizeof(MSG_C2S_ZONE_CHANGE), [this](CPlayer* p, CSerialBuffer* m) { RecvZoneChange(p, m); });
    // 이 줄이 없으면 포털 키가 조용히 버려진다(등록 안 된 번호는 OnReceived 가 조용히 버린다 — 답도 로그도 없다). 토글 없음(포털 표처럼)
    ok &= routes.OnPlayer(MsgType::C2S_PORTAL_USE, sizeof(MSG_C2S_PORTAL_USE), [this](CPlayer* p, CSerialBuffer* m) { RecvPortalUse(p, m); });
    // 이 줄이 없으면 입장 요청이 0 을 받아 파싱 오류로 샌다 — 로그도 안 남고 _packetErrors 만 오른다.
    //   1단계에서는 크기만 등록하고 핸들러는 없다(OnReceived 에서 처리기 없는 줄로 넘어가거나,
    //   플레이어가 아직 없으면 그 앞 문턱에서 버려진다). 서버 동작은 그대로다.
    //   (USE_LOGIN 이면 플레이어를 찾기 「전」에 처리한다 — 이 패킷이 올 때는 아직 플레이어가 없다. 끄면 크기만)
#if USE_LOGIN
    ok &= routes.OnSession(MsgType::C2S_ENTER_GAME, sizeof(MSG_C2S_ENTER_GAME), [this](int64_t s, CSerialBuffer* m) { RecvEnterGame(s, m); });
#else
    ok &= routes.Size(MsgType::C2S_ENTER_GAME, sizeof(MSG_C2S_ENTER_GAME));
#endif
    return ok;
}

// 대기표 잡기 — 게임 서버 Init 에 있던 한 줄(세션 허브의 칸과 같은 크기·같은 색인 — 불변식 공유 · 게임 서버 나누기 12 단계)
void CEntryHandler::Init(int maxClients)
{
#if USE_DB_WORKER
    _pendingEnter.assign(maxClients, PendingEnter{});   // 대기표 — 세션 허브의 칸과 같은 크기·같은 색인 (불변식 공유)
#else
    (void)maxClients;
#endif
}

#if USE_DB_WORKER
// 끊김의 대기표 몫 — 게임 서버 끊김 분기에 있던 두 줄 그대로(게임 서버 나누기 12 단계)
void CEntryHandler::DropPendingEnter(int64_t sessionId)
{
    PendingEnter gone;
    TakePendingEnter(sessionId, gone);
}
#endif

// 동접 게이지 — CSightBroadcaster::TickPeriodicSync(주기 동기화 순회)에 얹혀 있던 계산을 떼어 옴(게임 서버 나누기 14 단계).
//   주기·자리(틱 4 그 순회 바로 뒤)가 같아 같은 틱에 같은 값을 쓴다 — 사이에 존 인원을 바꾸는 일이 없다.
void CEntryHandler::TickInGameGauge()
{
    ++_inGameGaugeFrameCount;
    if (_inGameGaugeFrameCount >= IN_GAME_GAUGE_INTERVAL_FRAMES)
    {
        _inGameGaugeFrameCount = 0;
        int64_t inGame = 0;   // 게임 중인 플레이어 수 — 존마다 덧셈 한 번
        int64_t zones = 0, idle = 0;   // 존 수 · 그중 쉬는 존(사람 0 — 걷기·몬스터 판단을 건너뜀)
        _mapManager.ForEachZone([&](CZone* zone)
        {
            inGame += zone->GetPlayerCount();
            ++zones;
            if (zone->IsIdle())
                ++idle;
        });
        _gameMonitor._enter._inGame.Store(inGame);   // 게이지 — 동접 = 게임 중 + 불러오는 중 (GameMonitor.h)
        _gameMonitor._enter._zones.Store(zones);
        _gameMonitor._enter._zonesIdle.Store(idle);
    }
}

#if USE_DB_WORKER
// ==========================================================================
// DB 왕복 — 캐릭터 불러오기
//   가는 길: CCharacterStore::RequestCharacterLoad → 워커 큐(저장과 같은 큐 — 계정별 순서 보장)
//   오는 길: 워커 → 회신 큐 → CCharacterStore::ProcessDbResults(틱마다 통째로 꺼냄) → 알림(ICharacterLoadEvents) → OnCharacterLoaded
// ==========================================================================

// 상태별 수와 게이지를 한 자리에서만 움직인다 — 세는 자리가 갈라지면 한 곳만 빠져도 영영 안 맞는다.
void CEntryHandler::AdjustPendingCount(EnterState state, int delta)
{
    if (state == EnterState::LOADING)
    {
        _pendingLoadingCount += delta;
        _gameMonitor._enter._pending.Store(_pendingLoadingCount);
    }
    else
    {
        _pendingWaitingCount += delta;
        _gameMonitor._enter._waiting.Store(_pendingWaitingCount);
    }
}

void CEntryHandler::SetPendingEnter(int64_t sessionId, int64_t accountId, int32_t actorId, EnterState state)
{
    const uint16_t idx = CSession::ExtractIndex(sessionId);
    assert(idx < _pendingEnter.size());   // FindPlayer 와 같은 불변식 — 두 배열은 Init 에서 같은 크기로 잡힌다
    PendingEnter& slot = _pendingEnter[idx];

    if (slot.sessionId != -1)
    {
        // 있을 수 없다 — 같은 자리의 끊김 이벤트는 다음 접속보다 먼저 온다(세션 허브의 칸도 같은 순서에 기댄다).
        //   그래도 덮어쓰면 스스로 아문다: 옛 조회의 결과는 세션 번호가 안 맞아 orphaned 로 세어진다.
        SLOG_ERROR("[DB] pending enter overwritten (slot={}, old session={}, new session={})",
                   idx, slot.sessionId, sessionId);
        AdjustPendingCount(slot.state, -1);
    }

    slot.sessionId = sessionId;
    slot.accountId = accountId;
    slot.actorId   = actorId;
    slot.state     = state;
    AdjustPendingCount(state, +1);
}

// [입장 대기] → [불러오는 중]. 번호 둘은 이때 정해지므로 여기서 채운다.
//   상태 검사는 RecvEnterGame 앞 문턱과 겹친다. 일부러 남긴 둘째 겹이고 역할이 다르다 —
//   앞 문턱은 "번호를 뽑고 조회를 던지는 일 자체를 하지 말 것"이고, 여기는 "게이지 둘을 움직이기 전에
//   정말 [입장 대기]였나"를 보는 이 함수의 전제다. 이 줄이 없으면 [불러오는 중]에 또 불렸을 때
//   입장 대기 수가 음수로 내려간다(돌연변이로 확인: 요청 셋에 입장 대기 -2 · 불러오는 중 2).
//   ※ 지금은 앞 문턱이 먼저 막아서 이 검사에 도달하는 길이 없다 — 시험으로는 못 태운다.
bool CEntryHandler::PromotePendingEnter(int64_t sessionId, int64_t accountId, int32_t actorId)
{
    const uint16_t idx = CSession::ExtractIndex(sessionId);
    assert(idx < _pendingEnter.size());
    PendingEnter& slot = _pendingEnter[idx];

    if (slot.sessionId != sessionId || slot.state != EnterState::WAITING_ENTER)
        return false;

    AdjustPendingCount(EnterState::WAITING_ENTER, -1);
    slot.accountId = accountId;
    slot.actorId   = actorId;
    slot.state     = EnterState::LOADING;
    AdjustPendingCount(EnterState::LOADING, +1);
    return true;
}

bool CEntryHandler::TakePendingEnter(int64_t sessionId, PendingEnter& out)
{
    const uint16_t idx = CSession::ExtractIndex(sessionId);
    assert(idx < _pendingEnter.size());
    PendingEnter& slot = _pendingEnter[idx];

    // 빈 자리(-1)이거나 같은 자리에 들어온 다른 세션 — 세션 번호 "전체"가 같아야 주인이다.
    if (slot.sessionId != sessionId)
        return false;

    out  = slot;
    AdjustPendingCount(slot.state, -1);
    slot = PendingEnter{};
    return true;
}

void CEntryHandler::OnCharacterLoaded(const DBResult& result, int64_t nowNs)
{
    CGameMonitor::EnterCounters& enter = _gameMonitor._enter;

    // 지연은 주인이 있든 없든 잰다 — "꺼낸 결과 전부"가 분모라야 아래 분기 합과 맞대 볼 수 있다.
    enter.RecordLatencyUs((nowNs - result.requestNs) / 1000);

    // 주인 확인 — 대기표의 세션 번호 "전체"가 같아야 한다(같은 자리에 다른 사람이 들어왔으면 안 맞는다).
    //   불러오는 사이에 나갔으면 OnDisconnected 가 대기표를 이미 비웠다 → 결과는 버린다.
    //   코드(찾음·없음·실패)는 보지 않고 이 분기 하나로 센다.
    PendingEnter pending;
    if (!TakePendingEnter(result.sessionId, pending))
    {
        enter._orphaned.Inc();
        return;
    }

    // [2단계] 결과를 "어느 존에 · 어느 자리에 · 저장할까"로 바꾼 뒤 스폰한다.
    //   존 조회(FindOrCreateChannel)는 찾음·없음 분기에서만 한다 — 채널을 만들 수도 있는 함수라
    //   들여보내지 않을 분기(주인 없음·실패)에서는 부르면 안 된다.
    CZone* zone = nullptr;
    EnterPlacement place;   // 기본값 = 그 맵의 들어오는 자리(시작 구역 · 없으면 무작위) · 다음 주기에 저장
    bool toTown = false;    // 찾음 분기 — 마을 밖에서 죽은 채 저장된 행이라 저장된 맵·자리를 버린다(부활 규칙, 아래 USE_COMBAT)

    static_assert(static_cast<int>(DBResultCode::COUNT) == 3, "DBResultCode 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
    switch (result.code)
    {
    case DBResultCode::FOUND:
        enter._found.Inc();

        // 레벨·경험치는 자리(좌표·맵) 판정과 무관하게 행의 값을 그대로 쓴다.
        //   좌표가 못 쓸 값이어도 레벨까지 버릴 이유는 없다 — 다른 사실이다.
        //   ★ 표 범위 검사와 레벨 정산은 여기가 아니라 SpawnPlayer 가 한다 — 새 캐릭터·못 읽음 분기도 거기를 지난다.
        place.level = result.level;
        place.exp   = result.exp;
        place.hp    = result.hp;   // 최대치로 깎기는 SpawnPlayer 가 한다(최대치가 장비·레벨 정산 뒤에 선다)
        place.mp    = result.mp;
#if USE_INVENTORY
        place.loaded = &result;   // 가방 줄 — 검사와 적재는 SpawnPlayer 가 한다(이 함수 안에서 동기로 불리므로 가리켜도 안전)
#endif
        place.cooldownsAndBuffs = &result;   // 쿨타임·버프 묶음 — 풀기·검사·칸 채우기도 SpawnPlayer 가 한다(같은 수명)

#if USE_COMBAT
        // 죽은 채 저장된 행(체력 0) — 죽은 상태로 들여보내지 않고 부활 규칙(RespawnsToTown)을 여기서 적용한다.
        //   이 행이 생기는 길 셋 — 부활 전(3초)에 나가기 · 그 사이 주기 저장 뒤 서버 다운 · 죽은 채 정상 종료.
        //   나가는 쪽에서 막으면 둘째 길(주기 저장)이 남는다 — 그래서 들어오는 쪽 한 곳에서 본다.
        //   마력은 그대로 둔다(부활해도 그대로다 — Combat::Respawn 은 체력만 채운다).
        //   0 이하로 보지 않는 이유 — 음수(DB_HP_MP_UNSAVED)는 「저장된 적 없음」이다. 피해는 0 아래로 안 내려간다(Combat::ApplyDamage).
        if (result.hp == 0)
        {
            enter._deadRespawn.Inc();
            place.hp = DB_HP_MP_UNSAVED;   // 가득 — 마을이면 제자리(아래 저장된 자리 분기)에서
            toTown   = Combat::RespawnsToTown(result.mapId, _mapManager.GetDefaultMapId());
            place.dropBuffs = true;        // 버프는 버린다(죽으면 지워지는 규칙 — 쿨타임은 죽어도 남아 되살린다)
        }
#endif

        // 존 먼저, 좌표는 그 존의 크기로 본다. 저장된 맵이 없으면 zone 이 nullptr 로 남아 아래에서 기본 맵을 구한다.
        if (!toTown)
            zone = _mapManager.FindOrCreateChannel(result.mapId);

        if (toTown)
        {
            // 마을 밖에서 죽은 채 저장됨 → 저장된 맵·자리를 버린다. zone 이 nullptr 로 남아 아래에서 기본 맵(마을)의
            //   들어오는 자리(시작 구역)로 간다 — 접속 중에 죽어 3초 뒤 부활로 옮겨지는 자리와 같다. 자리가 DB 와 다르므로 dbDirty 는 기본값(true) 그대로.
            SLOG_INFO("[DB] saved dead -> town start area (account={}, map={}, x={}, y={})",
                      result.accountId, result.mapId, result.x, result.y);
        }
        else if (zone != nullptr && zone->IsWalkable(result.x, result.y))
        {
            place.hasPos  = true;
            place.x       = result.x;
            place.y       = result.y;
            place.dbDirty = false;   // DB 와 같은 값으로 들어간다 — 움직이기 전엔 다시 쓸 것이 없다
        }
        else
        {
            // 맵이 없어졌거나 좌표가 맵 밖·숫자 아님·막힌 칸(저장 뒤에 그 자리에 집이 생김) → 그 맵의 들어오는 자리(시작 구역 · 없으면 무작위 — CZone::CalcSpawnPos). 그 자리는 DB 와 다르므로 dbDirty 는 기본값(true) 그대로.
            enter._fallback.Inc();
            SLOG_WARN("[DB] saved position unusable -> map entry spot, start area or random (account={}, map={}, x={}, y={})",
                      result.accountId, result.mapId, result.x, result.y);
        }
        break;

    case DBResultCode::NOT_FOUND:
        enter._newCharacter.Inc();   // 새 캐릭터 — 기본 맵(마을) 시작 구역. 행은 첫 저장(UPSERT)이 만든다
#if USE_INVENTORY
        // ★ 캐릭터 행이 없어도 가방 줄은 있을 수 있다(첫 저장 전에 서버가 죽은 계정) — 워커가 읽어 왔다. 그대로 넣는다.
        place.loaded = &result;
#endif
        break;

    case DBResultCode::FAILED:
        // "없음"과 절대 섞지 않는다 — 들여보내면 새 캐릭터로 스폰되고 다음 저장이 진짜 행을 덮는다.
        //   재시도는 하지 않는다: 워커가 이미 재연결하고 한 번 더 해 봤다. 클라가 다시 접속하는 것이 재시도다.
        enter._failed.Inc();
        SLOG_ERROR("[DB] character load failed -> disconnect (account={}, session={})", result.accountId, result.sessionId);
        _sessionHub.RequestDisconnect(result.sessionId);
        return;

    case DBResultCode::COUNT:
        // 값이 아니라 개수 표시 — 올 수 없다. 대기표는 이미 비웠으니 세션을 매달아 두지 않고 끊는다.
        SLOG_ERROR("[DB] unexpected load result code (session={})", result.sessionId);
        _sessionHub.RequestDisconnect(result.sessionId);
        return;
    }

    if (zone == nullptr)
        zone = _mapManager.FindOrCreateChannel(_mapManager.GetDefaultMapId());

    if (SpawnPlayer(result.sessionId, pending.actorId, pending.accountId, zone, place) == nullptr)
    {
        // 분기 카운터는 위에서 이미 올랐다. 여기 오는 길은 채널 생성 실패뿐이라 카운터를 따로 두지 않는다(로그만).
        SLOG_ERROR("[DB] spawn failed after load -> disconnect (account={}, session={})", result.accountId, result.sessionId);
        _sessionHub.RequestDisconnect(result.sessionId);
    }
}
#endif // USE_DB_WORKER

CPlayer* CEntryHandler::SpawnPlayer(int64_t sessionId, int32_t actorId, int64_t accountId,
                                  CZone* zone, const EnterPlacement& place)
{
    if (zone == nullptr)
        return nullptr;

    // 플레이어 생성 + ID/표시 속성 부여 — 슬롯에 넣기 전까지는 이 지역 unique_ptr 가 주인이다.
    //   번호는 호출부가 준다 — 접속 때 이미 뽑았다(조회 열쇠로 먼저 필요했다).
    auto owned = std::make_unique<CPlayer>();
    CPlayer* player = owned.get();
    player->_actorId = actorId;
    player->_displayChar = CalcDisplayChar(player->_actorId);
    player->_colorIndex = CalcColorIndex(player->_actorId);
    if (_playerSpeed > 0)
        player->_speed = _playerSpeed;   // 게임용 속도 — 0 이면 생성자의 25~50 무작위(부하 시험 기준) 그대로

    // 저장된 자리가 있으면 그 자리에, 없으면 그 맵의 들어오는 자리에(시작 구역 · 없으면 무작위) (입장은 실패가 없다 — CZone::EnterZone 주석)
    if (place.hasPos)
        zone->EnterZoneAt(player, place.x, place.y);
    else
        zone->EnterZone(player);

    // 경계 매핑 등록 — 세션 번호를 붙이고 이제 슬롯이 주인이다(칸이 차 있을 때의 처리는 CSessionHub::AddPlayer)
    _sessionHub.AddPlayer(sessionId, std::move(owned));

    // 델타 동기화 기준 좌표 초기화 (스폰 직후 불필요한 동기화 방지)
    player->_lastSyncX = player->_x;
    player->_lastSyncY = player->_y;

    // 레벨·경험치 — 패킷을 보내기 "전"에 세운다. DB 토글 밖에 두는 이유는 토글을 끈 빌드에서도
    //   값이 서 있어야 하기 때문이다(그 빌드에선 늘 기본값 1·0 이고 메모리에서만 돈다).
    player->_level = place.level;
    player->_exp   = place.exp;

#if USE_DB_WORKER
    // 1단계 임시: playerId를 계정 식별자로 사용 (2단계는 접속 핸드셰이크로 수신)
    player->_accountId = accountId;
    // 새 캐릭터·들어오는 자리면 true(첫 주기에 저장 → 신규 행 INSERT 유도), DB 에서 불러온 자리 그대로면 false
    player->_dbDirty   = place.dbDirty;
    // DB 가 들고 있는 값 — 아래 레벨 정산·체력 깎기가 바꾼 몫은 다음 주기 판정(ShouldSave)이 이 값과 견주어 싣는다.
    player->_dbSaved.level = place.level;
    player->_dbSaved.exp   = place.exp;
    player->_dbSaved.hp    = place.hp;
    player->_dbSaved.mp    = place.mp;
#else
    (void)accountId;
#endif

#if USE_INVENTORY && USE_DB_WORKER
    // ── 불러온 가방 — 스탯을 세우기 "전"에 채운다 ──
    //   ★ 무기 칸(900)도 여기서 들어온다. 아래 레벨 정산·스탯 세우기(RecalcStats)가 무기 몫을 더하므로,
    //     이 줄이 그보다 뒤에 있으면 무기를 끼운 채 나갔다 들어온 사람이 맨손 공격력으로 입장한다
    //     (컴파일도 되고 화면도 멀쩡하다 — 입장 직후 S2C_MY_STATS 의 공격력만 틀린다).
    //   가방과 카운터만 만지는 일이라 앞당겨도 다른 것이 안 바뀐다. 전체 목록(SendInventory)은 그대로 아래에서 보낸다.
    if (place.loaded != nullptr)
        _characterStore.LoadInventoryRows(player, *place.loaded);
#endif

#if USE_EXP
    // ── 레벨 정산 — 스탯을 표에서 다시 세우고, 불러온 값이 표와 어긋나면 바로잡는다 ──
    //   ★ NotifyArrival 보다 앞이다. 그 안의 SendCreateMyPlayer 가 곧바로 최대 체력을 실어 보내므로
    //     여기서 안 세우면 첫 화면의 막대가 생성자 기본값(100)으로 나간다.
    //   경우 둘 — ⑴ 레벨이 표 밖(표를 줄인 패치) ⑵ 경험치가 이미 다음 레벨 몫을 넘음
    //     (표의 필요치를 낮춘 패치, 또는 만렙 상한에 걸려 안 오른 채 저장된 행).
    //     둘 다 "행은 멀쩡한데 표가 바뀐" 경우라 평소엔 한 번도 안 도는 분기다.
    //   규칙과 보정·레벨업 지표는 CStatSystem. 바로잡은 값은 DB 와 다르다 — 다음 주기 판정(ShouldSave)이
    //     위 _dbSaved 와 견주어 싣는다(예전엔 여기서 _dbDirty 를 켰다 — 안 켜면 영영 틀린 채로 남았다).
    _statSystem.ClampLevelToTable(player);

    // 0 을 줘서 레벨만 정산한다 — 경험치는 한 톨도 안 늘고, 올릴 수 있으면 올린다.
    _statSystem.GainExp(player, 0);
#endif

#if USE_DB_WORKER
    // ── 쿨타임·버프 되살리기 — 스탯 세우기 「앞」 ──
    //   버프 칸을 먼저 채워야 아래 RecalcStats 한 번에 배수까지 선다(첫 S2C_MY_STATS 가 2배 공격력으로 나간다).
    //   알림은 여기서 안 보낸다 — 아래 NotifyArrival 이 CREATE_MY_PLAYER 뒤에 SendActiveBuffs·SendActiveSkillCooldowns 로 보낸다.
    //   사본(_dbSaved)도 여기서 맞춘다 — 되살린 칸이 곧 DB 값이라 첫 주기에 헛저장하지 않게.
    _characterStore.RestoreCooldownsAndBuffs(player, place.cooldownsAndBuffs, place.dropBuffs);   // 저장소 — 배치 구조체에서 쓰는 두 칸만 넘긴다
#endif

#if USE_COMBAT
    // ── 스탯 세우기 — 레벨 정산 뒤 ──
    //   GainExp 는 레벨이 올랐을 때만 스탯을 세운다. 안 올랐으면 생성자 기본값 그대로라 여기서 세운다.
    //   refill=false — 최대치만 세우고, 체력·마력은 다음 줄이 저장값으로 넣는다(최대를 넘으면 깎고, 모름(-1)이면 가득).
    //     레벨 정산이 올렸어도(표를 낮춘 패치) 저장값을 넣는다 — 사냥으로 번 레벨업이 아니라 표 보정이다.
    //     죽은 채 저장된 행은 판정하는 쪽(OnCharacterLoaded)이 부활 규칙대로 「가득」으로 바꿔 넘겼다 — 죽은 채 들어오는 길은 없다.
    //   ★ NotifyArrival 보다 앞이다(위 레벨 정산 주석과 같은 이유 — CREATE 가 최대 체력을, ACTOR_MP 가 마력을 싣는다).
    //   USE_EXP 밖인 이유는 RecalcStats 선언 주석(StatSystem.h).
    _statSystem.RecalcStats(player, false);
    _statSystem.RestoreHpMp(player->_health, player->_attack, place.hp, place.mp);
#endif

    // 본인에게 존·내 캐릭터·숫자·가방·스탯 → 주변과 서로 생성 (순서와 「접속만 보내는 것」은 NotifyArrival 한 곳)
    //   불러온 가방은 위에서 이미 채웠다(스탯 세우기 앞 — 무기 몫 때문). 여기서는 보내기만 한다.
    _placement.NotifyArrival(player, zone, CPlayerPlacement::ArrivalKind::CONNECT);

    return player;
}

void CEntryHandler::OnConnected(int64_t sessionId)
{
    // 이미 등록된 세션이면 무시 (중복 접속 방어)
    if (_sessionHub.FindPlayer(sessionId) != nullptr)
    {
        _sessionHub.RequestDisconnect(sessionId);
        return;
    }

#if USE_LOGIN
    // [입장 요청] 접속만으로는 아무것도 안 만든다 — 캐릭터 번호도 계정 번호도 조회도
    //   입장 요청(C2S_ENTER_GAME)을 받아야 정해진다. 여기서는 대기표에 자리만 잡는다.
    //   ★ 자리를 안 잡으면 이 세션이 어느 표에도 없어서, 유령 플레이어를 잡는 유일한 장치인
    //     "동접 = 게임 중 + 불러오는 중 + 입장 대기" 등식이 늘 틀린 채로 울린다.
    //   ※ 입장 요청을 끝내 안 보내는 세션은 여기 남는다. 네트워크 계층의 유휴 타임아웃은
    //     하트비트만 보내도 갱신되므로 안 끊는다 — 그래서 게이지(mmo_enter_waiting)로 지켜본다.
    //     값이 올라가서 안 내려오면 그때 쓸어내기를 넣는다(지금 넣으면 한 번도 안 도는 분기다).
    SetPendingEnter(sessionId, 0, 0, EnterState::WAITING_ENTER);
#else
    // 번호는 접속 때 뽑는다 — 지금은 계정 번호 = 캐릭터 번호(임시)라 조회 열쇠로 먼저 필요하다.
    //   못 들어간 접속은 번호 하나를 버린다(단조 증가 카운터라 무해 — 몬스터도 같은 카운터를 쓴다).
    const int32_t actorId = _sessionHub.AllocActorId();

#if USE_DB_WORKER
    if (_characterStore.IsRunning())
    {
        // [DB 왕복 2단계] 접속 → 불러오는 중 → 게임 중. 여기서는 대기표만 남기고 조회를 던진다.
        //   플레이어도 존 입장도 패킷도 없다 — 스폰은 결과가 와야 한다(OnCharacterLoaded).
        //   그 사이에 온 패킷은 OnReceived 가 조용히 버린다(FindPlayer == nullptr).
        const int64_t accountId = actorId;
        if (!_characterStore.RequestCharacterLoad(sessionId, accountId))
        {
            // 거절 = 결과가 안 온다. 매달아 두면 영영 안 들어오므로 끊는다(rejected 는 위에서 셌다).
            _sessionHub.RequestDisconnect(sessionId);
            return;
        }
        SetPendingEnter(sessionId, accountId, actorId, EnterState::LOADING);
        return;
    }
#endif

    // DB 워커가 없으면(토글 OFF·미기동) 예전 그대로 곧바로 스폰 — 기본 맵(마을)의 여유 채널, 시작 구역.
    CZone* zone = _mapManager.FindOrCreateChannel(_mapManager.GetDefaultMapId());
    if (SpawnPlayer(sessionId, actorId, actorId, zone, EnterPlacement{}) == nullptr)
        _sessionHub.RequestDisconnect(sessionId);
#endif
}

#if USE_LOGIN
// 같은 계정으로 이미 들어와 있는 세션을 걷어낸다. 걷을 곳이 둘이다 — 게임 중, 그리고 불러오는 중.
//   ★ 맵(계정 → 플레이어)을 안 만들고 훑는 이유: 넣고 빼야 할 표가 둘이라 한쪽이 어긋나면
//     해제된 주소를 들게 된다. 훑기는 어긋날 표가 없다. 그리고 번호를 안 들고 온 접속은 이 길을
//     아예 안 타므로(부하 더미가 전부 그렇다) 훑는 횟수가 사람 수준에 머문다.
//   ※ 모든 계정이 지정 번호로 들어오는 날(로그인 서버가 붙는 날, 또는 부하 더미를 고정 계정으로
//     바꾸는 날)에는 접속마다 전체를 훑게 된다. 그때 재 보고 계정 → 플레이어 맵으로 바꾼다.
void CEntryHandler::KickDuplicateAccount(int64_t accountId, int64_t exceptSessionId)
{
    // ⑴ 게임 중 — 정리 코드를 새로 쓰지 않고 평소 끊김 처리와 같은 「떠나보내기」(LeaveGame)를 부른다. 그 안에 퇴장 방송·
    //    슬롯 해제·부활 대기열 회수·최종 저장 투입이 전부 있고, 무엇보다 **틱 중간 대기열에서 빼는
    //    일**이 들어 있다(RemoveFromZone 이 부르는 CZone::LeaveZone 이 존의 섹터 이동 대기열에서
    //    제거). 이 함수는 네트워크 이벤트 구간 한가운데서 플레이어를 지우는데, 같은 틱에
    //    그 플레이어가 MOVE_STOP 으로 그 대기열에 들어가 있을 수 있다 — 새로 짰으면 빠뜨렸을 자리다.
    //    뒤늦게 오는 진짜 끊김 이벤트는 플레이어가 이미 없어 FindPlayer 가 nullptr 이라 무해하다.
    //    첫 일치 하나만 본다 — 같은 계정이 둘 이상 게임 중일 수는 없다(여기서 막고 있기 때문이다).
    CPlayer* other = _sessionHub.FindPlayerIf([&](const CPlayer* p)
    {
        return p->_accountId == accountId && p->_sessionId != exceptSessionId;
    });
    if (other != nullptr)
    {
        const int64_t oldSession = other->_sessionId;
        SLOG_WARN("[LOGIN] duplicate account -> kick old session (account={}, old={}, new={})",
                  accountId, oldSession, exceptSessionId);
        _placement.LeaveGame(other);                                  // 최종 저장이 이 뒤의 조회보다 큐에서 앞선다 · 이 뒤로 other 는 못 쓴다(칸이 비며 해제)
        _sessionHub.RequestDisconnect(oldSession);
        _gameMonitor._enter._duplicate.Inc();
    }

    // ⑵ 불러오는 중 — 아직 플레이어가 없으니 저장도 방송도 없다. 대기표만 지우고 끊는다.
    //    탭 둘을 거의 동시에 열면 둘째가 첫째를 이 상태에서 걷어낸다. 뒤늦게 오는 그 조회 결과는
    //    대기표에서 주인을 못 찾아 orphaned 로 세어진다.
    //    [입장 대기] 자리는 계정 번호가 아직 0 이라 여기 안 걸린다(이 함수는 0 으로 안 불린다).
    for (const PendingEnter& slot : _pendingEnter)
    {
        if (slot.sessionId == -1 || slot.state != EnterState::LOADING
            || slot.accountId != accountId || slot.sessionId == exceptSessionId)
            continue;

        const int64_t oldSession = slot.sessionId;   // TakePendingEnter 가 slot 을 비우므로 먼저 복사한다
        SLOG_WARN("[LOGIN] duplicate account while loading -> drop (account={}, old={})", accountId, oldSession);
        PendingEnter gone;
        TakePendingEnter(oldSession, gone);
        _sessionHub.RequestDisconnect(oldSession);
        _gameMonitor._enter._duplicate.Inc();
        break;
    }
}

void CEntryHandler::RecvEnterGame(int64_t sessionId, CSerialBuffer* pMsg)
{
    MSG_C2S_ENTER_GAME recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // ⑴ 대기표가 [입장 대기]인가. 이미 요청했거나(불러오는 중) 이미 게임 중이면 조용히 무시한다 —
    //    두 번째 요청으로 캐릭터를 하나 더 만들지 않게 막는 자리다.
    {
        const uint16_t idx = CSession::ExtractIndex(sessionId);
        assert(idx < _pendingEnter.size());
        const PendingEnter& slot = _pendingEnter[idx];
        if (slot.sessionId != sessionId || slot.state != EnterState::WAITING_ENTER)
            return;
    }

    // ⑴-2 계정 번호 범위 — 0(미지정)이거나 ACCOUNT_ID_MIN 이상이어야 한다. 걸리면 캐릭터 번호도 안 뽑고 끊는다.
    //    지정 번호가 자동 번호 구간(int32 카운터, 2^31 미만)으로 내려오면 0 으로 들어온 남의 접속과 같은 행을
    //    서로 덮어쓰고, 아래 ⑶ 중복 정리가 그 접속을 끊는다. 음수(-1 등)도 여기서 걸린다.
    //    ※ 범위 검사일 뿐 본인 확인이 아니다 — 범위 안의 남의 번호는 인증이 붙기 전까지 못 거른다.
    if (recvMsg.accountId != 0 && recvMsg.accountId < ACCOUNT_ID_MIN)
    {
        SLOG_WARN("[LOGIN] enter rejected: account out of range -> disconnect (account={}, session={})",
                  recvMsg.accountId, sessionId);
        PendingEnter gone;
        TakePendingEnter(sessionId, gone);
        _sessionHub.RequestDisconnect(sessionId);
        return;
    }

    // ⑵ 번호 둘. 캐릭터 번호는 언제나 새로 뽑고, 계정 번호는 들고 온 값이 있으면 그것, 없으면(0)
    //    방금 뽑은 캐릭터 번호를 그대로 쓴다 — 그래야 번호를 안 들고 온 접속이 지금까지와 똑같이 돈다.
    //    자동과 지정은 구간이 타입으로 갈린다: 캐릭터 번호(CSessionHub::AllocActorId)는 int32 카운터라 2^31 위로 못 올라온다.
    const int32_t actorId   = _sessionHub.AllocActorId();
    const int64_t accountId = (recvMsg.accountId != 0) ? recvMsg.accountId : static_cast<int64_t>(actorId);

    // [불변식] 여기 오는 동안 저장소의 DB 워커는 떠 있다(_characterStore.IsRunning) — InitDB 가 실패하면 main 이 서버를 아예
    //   안 띄우고(main.cpp:109~113), 내리는 쪽은 게임 스레드를 join 한 "뒤"다(Stop, :428~437).
    //   USE_LOGIN 은 USE_DB_WORKER 를 요구하므로 "DB 없는 빌드"도 여기 못 온다.
    //   ※ 널 대비 분기를 두지 않는다 — 한 번도 안 타는 분기이고, 혹시 탄다면 불러오기 없이 스폰해
    //     다음 저장이 남의 행을 덮는 쪽이라 조용한 오동작이 된다. 차라리 여기서 멈추는 게 낫다.
    assert(_characterStore.IsRunning());

    // ⑶ 같은 계정 정리 — 반드시 조회를 던지기 "전"이다. 정리가 넣는 최종 저장과 이 조회는 같은
    //    워커 큐를 타고(계정 번호 % 워커 수) 앞에서부터 실행되므로, 순서가 뒤집히면 방금 저장한
    //    자리를 못 읽는다. 번호를 안 들고 온 접속은 새 번호라 겹칠 수가 없어 아예 안 훑는다.
    if (recvMsg.accountId != 0)
        KickDuplicateAccount(accountId, sessionId);

    // ⑷ 조회를 던진다. 거절되면 결과가 안 오므로 매달아 두지 않고 끊는다(rejected 는 안에서 셌다).
    if (!_characterStore.RequestCharacterLoad(sessionId, accountId))
    {
        PendingEnter gone;
        TakePendingEnter(sessionId, gone);
        _sessionHub.RequestDisconnect(sessionId);
        return;
    }

    // ⑸ 대기표를 [불러오는 중]으로. 조회 뒤에 해도 안전하다 — 결과는 같은 스레드가 네트워크 이벤트를
    //    다 돈 "뒤에야" 꺼내므로(ProcessNetworkEvents → ProcessDbResults), 그 전에 주인을 찾을 일이 없다.
    //    [불변식] 여기서 반드시 성공한다 — ⑴ 에서 [입장 대기]인 것을 봤고, 그 사이 이 자리를 건드리는
    //      것은 ⑶ 뿐인데 거기서 내 세션을 명시적으로 제외한다. 실패하면 조회는 던져 놓고 대기표가 없어
    //      결과가 주인을 못 찾고, 그 세션은 스폰도 끊김도 없이 영영 매달린다(클라가 하트비트를 보내
    //      유휴 타임아웃도 안 걸린다). 그래서 조용히 넘기지 않고 단언으로 잡는다.
    const bool promoted = PromotePendingEnter(sessionId, accountId, actorId);
    assert(promoted);
    (void)promoted;   // 릴리즈(NDEBUG) 미사용 경고 억제
}
#endif

// playerId → 고유 표시 문자 (A-Z, a-z, 0-9 = 62종)
uint8_t CEntryHandler::CalcDisplayChar(int32_t playerId)
{
    static constexpr char CHARS[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    return static_cast<uint8_t>(CHARS[static_cast<uint32_t>(playerId) % 62]);
}

// playerId → 고유 색상 인덱스 (0-6, 7종)
uint8_t CEntryHandler::CalcColorIndex(int32_t playerId)
{
    return static_cast<uint8_t>((static_cast<uint32_t>(playerId) / 62) % 7);
}

void CEntryHandler::RecvZoneChange(CPlayer* player, CSerialBuffer* pMsg)
{
#if USE_COMBAT
    if (player->_health.IsDead())
        return;   // 죽은 채로 존을 옮기면 부활 대기열이 옛 존을 가리킨 채 남는다
#endif

    int64_t sessionId = player->_sessionId;
    if (sessionId == -1)
        return;

    CZone* oldZone = _mapManager.GetZone(player->_zoneId);
    if (oldZone == nullptr)
        return;

    MSG_C2S_ZONE_CHANGE recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // 목적지 고르기(지정 채널 · 자동 배정 · 무작위 맵 · 정원 · 같은 존 거부 · 맵 지정은 운영자만)는 맵 관리자 몫 — 여기는 거절 사유를 패킷으로 바꿔 보낼 뿐
    const ZoneChangeTarget target = _mapManager.PickZoneChangeTarget(
        oldZone->GetZoneId(), recvMsg.targetMapId, recvMsg.targetChannelIndex, player->_isAdmin);
    _gameMonitor._sector._zoneChangeResults[static_cast<int>(target.reject)].Inc();   // 성공(NONE)·거절 모두 — 칸의 합 = 요청 수
    if (target.reject != ZoneChangeReject::NONE)
    {
        _placement.SendZoneChangeFail(player, ToZoneChangeFailReason(target.reject));
        return;
    }
    // 들어갈 수 있는지는 위 PickZoneChangeTarget 이 나가기 "전에" 다 봤다 — 옮기기는 TransferPlayer 한 곳.
    //   자리는 그 존의 규칙(at = nullptr — CZone::CalcSpawnPos)
    _placement.TransferPlayer(player, oldZone, target.zone, CPlayerPlacement::ArrivalKind::ZONE_CHANGE, nullptr);
}

// 포털 사용 — 어디로 가는지는 포털 표가 정한다(클라는 번호만 보낸다 — PortalTable.h [무엇]).
//   판정 = 살았나 · 표에 있나 · 지금 맵의 포털인가 · 반경 안인가. 넷을 다 지나야 옮긴다(옮긴 뒤 되돌리는 길은 없다 — TransferPlayer).
//   목적지가 방 맵이면 넷 뒤에 파티 판정이 하나 더 있다(EnterRoom — 파티장이 누르면 파티원 전원이 새 방으로).
//   실패 답은 S2C_ZONE_CHANGE_FAIL 을 같이 쓴다(4 포털에서 멂 · 5 포털 없음 — Protocol.h). 죽은 채면 답이 없다(존 이동 요청과 같다).
//   세는 곳은 여기 한 곳 — 칸의 합(_used + Σ _rejected) = 받은 요청 수(GameMonitor.h _portal). 방어 분기 둘(존 없음 ·
//   목적지 채널 못 얻음)은 안 센다 — 지금 일어나는 길이 없고, 일어나면 앞엣것은 조용히, 뒤엣것은 로그로 드러난다.
void CEntryHandler::RecvPortalUse(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_PORTAL_USE recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    auto& pc = _gameMonitor._portal;
    auto reject = [&](PortalReject why) { pc._rejected[static_cast<int>(why)].Inc(); };

#if USE_COMBAT
    if (player->_health.IsDead())
    {
        reject(PortalReject::DEAD);   // 죽은 채로 존을 옮기면 부활 대기열이 옛 존을 가리킨 채 남는다(RecvZoneChange 와 같은 까닭)
        return;
    }
#endif

    CZone* oldZone = _mapManager.GetZone(player->_zoneId);
    if (oldZone == nullptr)
        return;   // 게임 중인 플레이어는 늘 존에 있다 — 존 이동 요청과 같은 방어

    const PortalDef* portal = _gameDataFiles.Portals().Find(recvMsg.portalId);
    if (portal == nullptr)
    {
        reject(PortalReject::NO_PORTAL);
        _placement.SendZoneChangeFail(player, 5);   // 5: 포털 없음
        return;
    }
    if (portal->mapId != oldZone->GetMapId())
    {
        reject(PortalReject::WRONG_MAP);
        _placement.SendZoneChangeFail(player, 5);   // 5: 포털 없음 — 클라에겐 「없는 번호」와 같은 답이다(GameMonitor.h PortalReject)
        return;
    }
    if (!portal->InReach(player->_x, player->_y))
    {
        reject(PortalReject::TOO_FAR);
        _placement.SendZoneChangeFail(player, 4);   // 4: 포털에서 멂
        return;
    }

    // 방 맵(파티 전용 방)이면 자동 배정이 없다 — 파티 판정을 거쳐 새 방으로 전원이 같이 간다(EnterRoom · Room.h).
    //   세는 곳: 들어가면 _used, 걸리면 _rejected[ROOM](사유는 _room 칸) — 위 등식(_used + Σ _rejected = 받은 요청 수)이 그대로다
    if (_mapManager.IsRoomMap(portal->targetMapId))
    {
#if USE_PARTY
        _partyHandler.EnterRoom(player, oldZone, *portal);
#else
        // 방은 파티 위에 선다 — 파티를 끈 빌드는 방에 들어가는 길이 없다(맵 표의 방 맵은 그대로 읽힌다). 파티 알림이 없는 빌드라 「존재하지 않는 맵/채널」로 답한다
        reject(PortalReject::ROOM);
        _placement.SendZoneChangeFail(player, 0);   // 0: 존재하지 않는 맵/채널
#endif
        return;
    }

    // 목적지 채널은 자동 배정 — 정원 규칙은 존 이동 요청과 같다(FindOrCreateChannel 이 자리 있는 채널을 고르고, 다 차면 새로 만든다).
    //   다른 맵이라 지금 존과 같을 수 없다(포털 표가 같은 맵 포털을 막았다 — PortalTable.h [맵 대조]).
    CZone* newZone = _mapManager.FindOrCreateChannel(portal->targetMapId, player->_isAdmin);
    if (newZone == nullptr)
    {
        // 맵은 표가 대조했으니 채널 만들기 실패뿐이다 — 존 이동 요청의 NOT_FOUND 와 같은 답
        SLOG_ERROR("[Portal] 목적지 채널을 못 얻었습니다 (portal={}, map={})", portal->portalId, portal->targetMapId);
        _placement.SendZoneChangeFail(player, 0);   // 0: 존재하지 않는 맵/채널
        return;
    }

    pc._used.Inc();
    const CPlayerPlacement::SpawnAt at{ static_cast<float>(portal->targetX), static_cast<float>(portal->targetY) };
    _placement.TransferPlayer(player, oldZone, newZone, CPlayerPlacement::ArrivalKind::ZONE_CHANGE, &at);
}
