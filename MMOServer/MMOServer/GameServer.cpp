#include "GameServer.h"
#include "Player.h"
#include "Monster.h"     // USE_MONSTER — 헤더 안에서 스스로 토글을 본다
#include "MonsterAI.h"   // USE_MONSTER — 몬스터 표 속도 대조의 상한(MAX_SPEED)
#include "GameDataCsv.h" // USE_INVENTORY — 채팅 명령 인자를 숫자로 읽는다(표 파서와 같은 규칙)
#include "CooldownBuffBlob.h" // USE_DB_WORKER — 쿨타임·버프 열의 바이트 형식(토글을 안 보는 헤더)
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
#include <cstring>  // strcmp — 운영 로그인 열쇠 대조 (알림 문구 memcpy 는 SelfNotifier.cpp 로 옮겼다)
#include <string>
#include "PacketBuild.h" // 여러 조각이 같이 쓰는 조립 함수(머리 쓰기·닫기 · 등장 조각 · 파티 조각 · 부활 · 체력 바뀜) — 게임 서버 나누기 0-②

using namespace PacketBuild;   // 아래 조립 함수들이 머리 쓰기·닫기를 이름만으로 부른다(옮기기 전과 같은 꼴)

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
    // 머리 쓰기·닫기(BeginPacket · FinalizePacket)와 등장 조각(WriteSnapshot · WriteCreateEntry)은 PacketBuild.h 로 옮겼다
    //   — 여러 조각이 같이 쓴다(게임 서버 나누기 0-②). 강제 인라인(CREATE_FORCEINLINE)도 거기서 정한다.

    // MakeMoveStart · MakeMoveStop · MakeSyncPosition(이동 셋) 은 SightBroadcaster.cpp — 시야 방송 조각만 부른다(게임 서버 나누기 3 단계)

    // MakeZoneInfo · MakeCreateMyPlayer(존 정보 · 내 캐릭터 생성) 는 PlayerPlacement.cpp — 배치 조각만 부른다(게임 서버 나누기 5 단계)

    // MakeCreateOtherPlayer · MakeDeletePlayer(남의 캐릭터 생성·삭제) 는 SightBroadcaster.cpp — 시야 방송 조각만 부른다(게임 서버 나누기 3 단계)

#if USE_COMBAT
    // ── 전투 (USE_COMBAT) ──
    //   필드 순서는 Protocol.h의 구조체와 글자 그대로 같아야 한다. 아래 assert가 크기만 보므로
    //   순서가 뒤바뀌어도 크기가 같으면 안 걸린다 — 고칠 때 구조체를 나란히 열고 볼 것.

    CSerialBuffer* MakeAttack(int32_t attackerId, uint8_t direction)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ATTACK::TYPE);
        *buf << static_cast<int>(attackerId);
        *buf << static_cast<uint8_t>(direction);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ATTACK));
        return buf;
    }

    CSerialBuffer* MakeDamage(int32_t targetId, int32_t attackerId, int32_t damage, int32_t remainHp)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_DAMAGE::TYPE);
        *buf << static_cast<int>(targetId);
        *buf << static_cast<int>(attackerId);
        *buf << static_cast<int>(damage);
        *buf << static_cast<int>(remainHp);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_DAMAGE));
        return buf;
    }

    CSerialBuffer* MakeDeath(int32_t targetId)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_DEATH::TYPE);
        *buf << static_cast<int>(targetId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_DEATH));
        return buf;
    }

    // MakeRespawn 은 PacketBuild.h — 본인 알림(도착 때 본인)과 전투(부활 방송) 둘이 부른다

    // MakeExp(경험치) 는 SelfNotifier.cpp — 본인 알림 조각만 부른다(게임 서버 나누기 2 단계)

    // MakeActorStats 는 PacketBuild.h — 전투 · 처치 보상 · 가방 · 운영 명령이 같이 부른다

    // MakeMyStats(내 최종 공격력·방어력) 는 SelfNotifier.cpp — 본인 알림 조각만 부른다(게임 서버 나누기 2 단계)
#endif // USE_COMBAT

    // MakeZoneChangeOk · MakeZoneChangeFail(존 이동 성공·실패) 는 PlayerPlacement.cpp — 배치 조각만 부른다(게임 서버 나누기 5 단계)

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

    // 땅 아이템·줍기·쓰기·장착 거절 사유 → 와이어 값(ToGroundItemGone · ToPickupFail · ToUseItemFail · ToEquipFail) 은 ItemHandler.cpp
    //   — 가방·땅 조각만 부른다(게임 서버 나누기 6 단계)

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

    // MakeError(알림 한 줄) · MakeInventory(가방 전체) 는 SelfNotifier.cpp(2 단계) · MakeInventorySlot(칸 하나) · MakeUseItemFail · MakeEquipFail(쓰기·장착 거절) 는
    //   ItemHandler.cpp(6 단계) — 각각 그 조각만 부른다(게임 서버 나누기)

    // MakeActorBuff(지속 효과 통보 — 본인에게만) 는 SelfNotifier.cpp — 본인 알림 조각만 부른다(게임 서버 나누기 2 단계)

#if USE_SKILL
    // 스킬 거절 사유 → 와이어 값. 사유를 정하는 건 CSkillSystem(SkillSystem.h), 패킷 값으로 바꾸는 건 여기 —
    //   위 ToUseItemFail 과 같은 꼴. 통과(COUNT)는 실패 패킷이 아니다.
    SkillFail ToSkillFail(SkillReject reject)
    {
        switch (reject)
        {
        case SkillReject::BAD_SKILL: return SkillFail::BAD_SKILL;
        case SkillReject::NOT_FOR_CASTER: return SkillFail::BAD_SKILL;   // 와이어 값을 늘리지 않는다 — 사람 눈에는 「없는 스킬」(몬스터 스킬은 사람 막대에도 안 나온다)
        case SkillReject::DEAD:      return SkillFail::DEAD;
        case SkillReject::COOLDOWN:  return SkillFail::COOLDOWN;
        case SkillReject::LOW_HP:    return SkillFail::LOW_HP;
        case SkillReject::LOW_MP:    return SkillFail::LOW_MP;
        case SkillReject::NO_TARGET: return SkillFail::NO_TARGET;
        case SkillReject::COUNT:     break;
        }
        assert(false && "거절이 아닌 결과를 실패 패킷으로 바꾸려 했다");
        return SkillFail::BAD_SKILL;
    }

    // 필드 순서는 Protocol.h 의 구조체와 글자 그대로 — 위 전투 패킷과 같은 주의(assert 는 크기만 본다)
    CSerialBuffer* MakeSkillCast(int32_t casterId, uint8_t skillId, uint8_t direction)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_SKILL_CAST::TYPE);
        *buf << static_cast<int>(casterId);
        *buf << static_cast<uint8_t>(skillId);
        *buf << static_cast<uint8_t>(direction);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_SKILL_CAST));
        return buf;
    }

    CSerialBuffer* MakeSkillFail(uint8_t skillId, SkillFail reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_SKILL_FAIL::TYPE);
        *buf << static_cast<uint8_t>(skillId);
        *buf << static_cast<uint8_t>(reason);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_SKILL_FAIL));
        return buf;
    }

    // MakeSkillCooldownRemain(남은 스킬 쿨타임) · MakeActorMp(마력) 는 SelfNotifier.cpp — 본인 알림 조각만 부른다(게임 서버 나누기 2 단계)
#endif

    // MakePartyAsk · MakePartyAskEnd · MakePartyNotice · MakePartyInfo · MakePartyMember(파티 패킷) 는 PartyHandler.cpp
    //   — 파티·방 조각만 부른다(게임 서버 나누기 7 단계)

    // MakeGroundItemCreate · MakeGroundItemDelete(땅 아이템 보이기·지우기) 는 SightBroadcaster.cpp(3 단계) · MakePickupFail(줍기 거절) 은 ItemHandler.cpp(6 단계)

    // MakeSectorUpdates(섹터 묶음) · MakeCreatePlayerBatch · MakeDeletePlayerBatch(인바운드 멤버십 묶음 · 크기 static_assert 포함) 는
    //   SightBroadcaster.cpp — 시야 방송 조각만 부른다(게임 서버 나누기 3 단계)
}

CGameServer::CGameServer(CMonitorManager& monitor, CGameMonitor& gameMonitor)
    : _mode(ServerMode::GameServer)
    , _monitor(monitor)
    , _gameMonitor(gameMonitor)
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

    if (!RegisterRoutes())
    {
        SLOG_ERROR("[GameServer] 패킷 분기 표 등록 실패 — 표 범위 밖 번호이거나 같은 번호를 두 번 등록했다 (RegisterRoutes)");
        return false;
    }

    _network = std::make_unique<NetIoModel>(port, maxClients, _mode, _monitor,
                                            workerThreads, sendWorkers, rioWorkers, completionBatch, sendDepth);

    // 게임 서버 모드일 때만 맵 등록
    if (_mode == ServerMode::GameServer)
    {
        if (maps == nullptr || mapCount <= 0)
            return false;

        for (int32_t i = 0; i < mapCount; ++i)
        {
            // 섹터 묶음이 좌표를 눈금(uint16)으로 실어 나른다 — 맵이 상한(POS_QUANT_MAX_MAP_SIZE)을 넘으면 좌표가
            //   조용히 깨지므로 등록 전에 막는다. 와이어 사정이라 패킷을 모르는 맵 관리자가 아니라 여기서 본다(10-06).
            if (maps[i].mapWidth > POS_QUANT_MAX_MAP_SIZE || maps[i].mapHeight > POS_QUANT_MAX_MAP_SIZE)
            {
                SLOG_ERROR("[GameServer] 맵 {} 크기 {}×{} — 와이어 좌표 눈금이 실을 수 있는 {} 이하여야 합니다 (MapTable.csv)",
                           maps[i].mapId, maps[i].mapWidth, maps[i].mapHeight, POS_QUANT_MAX_MAP_SIZE);
                return false;
            }
            if (!_mapManager.RegisterMap(maps[i]))
                return false;
        }

        // 첫 번째 맵을 기본 접속 맵으로 설정
        _defaultMapId = maps[0].mapId;

        // 프레임 재사용 컨테이너 reserve (워밍업 realloc 방지)
        // 전체 서버 범위 버퍼도 maxPerChannel 기준 — 초과 시 자연 증가 후 capacity 유지
        int32_t maxPerChannel = maps[0].maxPlayersPerChannel;
        _sessionHub.Init(_network.get(), maxClients);   // 세션 칸 maxClients 개(전부 빈 칸) · 보내기에 쓸 네트워크
#if USE_DB_WORKER
        _pendingEnter.assign(maxClients, PendingEnter{});   // 대기표 — 위 슬롯과 같은 크기·같은 색인 (불변식 공유)
#endif
        _sightBroadcaster.Init(maxPerChannel, maxClients);   // 주변 9섹터 단위 버퍼 둘 · 전체 서버 dirty (이동/sync 묶음 대상)
    }

    return true;
}

bool CGameServer::LoadGameDataFiles(const std::string& dir)
{
    if (!_gameDataFiles.LoadAll(dir))
        return false;

#if USE_MONSTER
    // 몬스터 속도는 AI 판단 주기가 전제하는 상한 안이어야 한다 — 상한은 AI 사정이라 표만으로는 못 본다(MonsterAI.h MAX_SPEED).
    if (!_gameDataFiles.Monsters().CheckSpeed(MonsterAI::MAX_SPEED))
        return false;
#endif
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
    // 게임 스레드 종료 후(단일 스레드 시점) 전원 최종 저장 → 드레인 → 워커 종료 — 캐릭터 저장소(CCharacterStore::Shutdown)
    _characterStore.Shutdown();
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

    // 잔여 플레이어 정리 — 슬롯(unique_ptr)이 주인이라 비우면 해제된다(DB 를 켠 빌드면 최종 저장은 위 저장소 Shutdown 의 SaveAllPlayers 가 이미 했다)
    _sessionHub.RemoveAll();

    // 몬스터는 여기서 안 치운다 — 소유가 존이라 _mapManager 가 소멸할 때 존과 같이 해제된다.
    //   게임 스레드가 이미 끝나(위 join) 그 사이에 몬스터를 읽는 코드가 없다.
}

#if USE_DB_WORKER
// ==========================================================================
// DB 저장 파이프라인 — 워커 띄우기·멈추기 · 저장 세 길 · 저장 대상 판정 · 쿨타임·버프 묶기·되살리기는 캐릭터 저장소 조각으로 옮겼다
//   — CCharacterStore (CharacterStore.cpp · 게임 서버 나누기 4 단계). 여기 남은 것은 입장 대기표와 불러오기 결과 처리(입장 몫)
// ==========================================================================

// 서버의 공개 입구 — main 이 부른다(Start 전). 몸은 저장소가 든다.
bool CGameServer::InitDB(const DBConfig& dbConfig, int savePeriodSec, int workerCount, int queueMax)
{
    return _characterStore.Init(dbConfig, savePeriodSec, workerCount, queueMax);
}

// ==========================================================================
// DB 왕복 — 캐릭터 불러오기
//   가는 길: CCharacterStore::RequestCharacterLoad → 워커 큐(저장과 같은 큐 — 계정별 순서 보장)
//   오는 길: 워커 → 회신 큐 → CCharacterStore::ProcessDbResults(틱마다 통째로 꺼냄) → 알림(ICharacterLoadEvents) → OnCharacterLoaded
// ==========================================================================

// 불러오기 요청(RequestCharacterLoad) 은 캐릭터 저장소 조각으로 옮겼다 — CCharacterStore (게임 서버 나누기 4 단계)

// 상태별 수와 게이지를 한 자리에서만 움직인다 — 세는 자리가 갈라지면 한 곳만 빠져도 영영 안 맞는다.
void CGameServer::AdjustPendingCount(EnterState state, int delta)
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

void CGameServer::SetPendingEnter(int64_t sessionId, int64_t accountId, int32_t actorId, EnterState state)
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
bool CGameServer::PromotePendingEnter(int64_t sessionId, int64_t accountId, int32_t actorId)
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

bool CGameServer::TakePendingEnter(int64_t sessionId, PendingEnter& out)
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

// 회신 꺼내기(ProcessDbResults) 는 캐릭터 저장소 조각으로 옮겼다 — 불러오기 결과는 알림(ICharacterLoadEvents)으로 아래 OnCharacterLoaded 에 온다 (게임 서버 나누기 4 단계)

void CGameServer::OnCharacterLoaded(const DBResult& result, int64_t nowNs)
{
    CGameMonitor::EnterCounters& enter = _gameMonitor._enter;

    // 지연은 주인이 있든 없든 잰다 — "꺼낸 결과 전부"가 분모라야 아래 갈래 합과 맞대 볼 수 있다.
    enter.RecordLatencyUs((nowNs - result.requestNs) / 1000);

    // 주인 확인 — 대기표의 세션 번호 "전체"가 같아야 한다(같은 자리에 다른 사람이 들어왔으면 안 맞는다).
    //   불러오는 사이에 나갔으면 OnDisconnected 가 대기표를 이미 비웠다 → 결과는 버린다.
    //   코드(찾음·없음·실패)는 보지 않고 이 갈래 하나로 센다.
    PendingEnter pending;
    if (!TakePendingEnter(result.sessionId, pending))
    {
        enter._orphaned.Inc();
        return;
    }

    // [2단계] 결과를 "어느 존에 · 어느 자리에 · 저장할까"로 바꾼 뒤 스폰한다.
    //   존 조회(FindOrCreateChannel)는 찾음·없음 갈래에서만 한다 — 채널을 만들 수도 있는 함수라
    //   들여보내지 않을 갈래(주인 없음·실패)에서는 부르면 안 된다.
    CZone* zone = nullptr;
    EnterPlacement place;   // 기본값 = 그 맵의 들어오는 자리(시작 구역 · 없으면 무작위) · 다음 주기에 저장
    bool toTown = false;    // 찾음 갈래 — 마을 밖에서 죽은 채 저장된 행이라 저장된 맵·자리를 버린다(부활 규칙, 아래 USE_COMBAT)

    static_assert(static_cast<int>(DBResultCode::COUNT) == 3, "DBResultCode 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
    switch (result.code)
    {
    case DBResultCode::FOUND:
        enter._found.Inc();

        // 레벨·경험치는 자리(좌표·맵) 판정과 무관하게 행의 값을 그대로 쓴다.
        //   좌표가 못 쓸 값이어도 레벨까지 버릴 이유는 없다 — 다른 사실이다.
        //   ★ 표 범위 검사와 레벨 정산은 여기가 아니라 SpawnPlayer 가 한다 — 새 캐릭터·못 읽음 갈래도 거기를 지난다.
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
            place.hp = DB_HP_MP_UNSAVED;   // 가득 — 마을이면 제자리(아래 저장된 자리 갈래)에서
            toTown   = RespawnsToTown(result.mapId);
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
        else if (zone != nullptr && zone->IsInsideMap(result.x, result.y))
        {
            place.hasPos  = true;
            place.x       = result.x;
            place.y       = result.y;
            place.dbDirty = false;   // DB 와 같은 값으로 들어간다 — 움직이기 전엔 다시 쓸 것이 없다
        }
        else
        {
            // 맵이 없어졌거나 좌표가 맵 밖·숫자 아님 → 그 맵의 들어오는 자리(시작 구역 · 없으면 무작위 — CZone::CalcSpawnPos). 그 자리는 DB 와 다르므로 dbDirty 는 기본값(true) 그대로.
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
        _network->RequestDisconnectSession(result.sessionId);
        return;

    case DBResultCode::COUNT:
        // 값이 아니라 개수 표시 — 올 수 없다. 대기표는 이미 비웠으니 세션을 매달아 두지 않고 끊는다.
        SLOG_ERROR("[DB] unexpected load result code (session={})", result.sessionId);
        _network->RequestDisconnectSession(result.sessionId);
        return;
    }

    if (zone == nullptr)
        zone = _mapManager.FindOrCreateChannel(_defaultMapId);

    if (SpawnPlayer(result.sessionId, pending.actorId, pending.accountId, zone, place) == nullptr)
    {
        // 갈래 계수기는 위에서 이미 올랐다. 여기 오는 길은 채널 생성 실패뿐이라 계수기를 따로 두지 않는다(로그만).
        SLOG_ERROR("[DB] spawn failed after load -> disconnect (account={}, session={})", result.accountId, result.sessionId);
        _network->RequestDisconnectSession(result.sessionId);
    }
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

#if USE_DB_WORKER
        // 1-0) DB 회신 소비 — 워커가 돌려준 결과(캐릭터 불러오기)를 통째로 꺼낸다 (락 1회).
        //   네트워크 이벤트 "뒤"에 두는 이유: 같은 틱에 그 세션의 끊김과 결과가 같이 오면 끊김을 먼저
        //   처리해 결과가 주인 없는 것으로 버려지게 한다. 거꾸로면 들여보냈다가 곧바로 지우게 된다.
        //   네트워크 구간(phaseT1~T2) "안"에 두는 이유: 접속 처리(스폰) 비용이 원래 이 구간에 있었다.
        //   결과를 받아 스폰하게 바뀐 뒤에도 _phaseNetworkUs 가 같은 것을 재도록 자리를 맞춘다.
        _characterStore.ProcessDbResults();
#endif
        auto phaseT2 = Clock::now();

#if USE_MONSTER
        // 1-1) 몬스터 — 존마다 관리자가 리젠 주기가 된 구역을 채우고, 몬스터마다 판단한다(CMonsterManager::Tick).
        //   좌표 갱신(2) 앞에 두는 이유: AI 가 정한 방향이 같은 틱에 반영돼야 한 틱씩 밀리지 않는다.
        TickMonsters();
#endif

        // 2) 게임 로직 갱신 (좌표 이동 + 섹터 변경 감지 + 경계 클램핑) — 섹터 변경은 존마다 대기열에 쌓인다
        //   경계에 막혀 선 캐릭터는 존이 그 자리에서 알린다(OnCharacterMoveStop — 그 알림은 시야 조각이 받는다)
        _mapManager.TickAll(deltaTime, _sightBroadcaster);

        // 3) 섹터 변경 배치 처리 — 존마다 대기열(이동 패킷 + 존 틱, 넣을 때 중복 차단)을 읽어 보내고 비운다.
        //   존마다 따로 도는 까닭: 대기열의 섹터 좌표는 그 존의 격자이고, 보내는 대상도 그 존 사람뿐이다.
        //   보내기 · 같은 틱 쌍 보정 · 멤버십 구간 계측은 시야 조각(CSightBroadcaster::ProcessSightChanges)
        _sightBroadcaster.ProcessSightChanges();

#if USE_COMBAT
        // 3-2) 부활 대기자 훑기 (게임 로직 페이즈에 계상 — 브로드캐스트를 동반한다)
        //   플레이어만 — 몬스터는 되살리지 않는다(죽는 순간 지우고, 스폰이 새로 채운다 — 1-1).
        TickRespawn();

        // 3-2b) 지속 효과 만료 — 효과가 걸린 캐릭터 목록만 훑는다(비면 그 자리에서 끝). 풀리면 알림(OnBuffChanged)으로 본인에게 간다.
        //   존마다 돌지 않는다 — 목록이 게임 서버에 하나라 존을 옮긴 사람도 그대로 따라온다(BuffSystem.h [왜 따로인가]).
        _buffSystem.Tick(_frameCount);
#endif

#if USE_PARTY
        // 3-2c) 파티 — 요청 시간 지남 · 파티원 상태 비교(약 0.5초마다). 파티·요청이 없으면 바로 끝난다(부하 시험엔 파티가 없다).
        //   존마다 돌지 않는다 — 파티는 존을 넘는다(목록이 게임 서버에 하나, PartySystem.h [왜 따로인가]).
        _partySystem.Tick(_frameCount);

        // 방 다시 들어가기 잠금 — 초마다 풀린 것 빼기는 파티·방 조각(CPartyHandler::TickRoomLocks · 게임 서버 나누기 7 단계)
        _partyHandler.TickRoomLocks();
#endif

#if USE_DROP
        // 3-3) 땅의 아이템 사라지기 — 섹터 변경 대기열을 비운 뒤라 "같은 틱 이동자" 보정이 필요 없다
        _itemHandler.TickGroundItems();
#endif

        auto phaseT3 = Clock::now();

        // 4) 주기적 위치 동기화 (MOVING 플레이어 → 주변 브로드캐스트) — 시야 조각. 같은 순회에 동접 게이지(_enter._inGame)가 얹혀 있다
        _sightBroadcaster.TickPeriodicSync();

        // 이번 틱 보류물 비우기 — digest(USE_BROADCAST_BUNDLE) 또는 섹터 묶음(USE_SECTOR_AGGREGATION). 아래 flush가 묶어 보냄
        _sightBroadcaster.FlushSends();

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
        //   방송 쪽 다섯(주변 모으기 · 수신자별 처리 · 멤버십 횟수·시간 · 쌍 보정)은 시야 조각이 반영한다
        _sightBroadcaster.ReportTickMetrics();
        _monitor._gameLoop._flushSendUs.Add(std::chrono::duration_cast<std::chrono::microseconds>(flushT1 - flushT0).count());
        _gameMonitor._sector._moveBudgetRejects.Add(_tickMoveBudgetRejects);  // 이동 예산 거부 건수
        _tickMoveBudgetRejects = 0;

        // [계측 오버헤드 절감] handle-latency 지역 누적분 → 전역 1회 반영
        _monitor._gameLoop.FlushHandleLatency();

        // 5) 빈 동적 채널 정리
        ++_cleanupFrameCount;
        if (_cleanupFrameCount >= CLEANUP_INTERVAL_FRAMES)
        {
            _cleanupFrameCount = 0;

            // 정리는 틱 끝(5)이라 몬스터를 가리키는 게임 서버 대기열이 없다 — 존을 그대로 지워도 된다.
            //   이동 묶음은 틱 끝 송신이 비웠다. 섹터 이동 대기열은 존 안이라
            //   존과 같이 사라진다. 누가 정리를 틱 앞쪽으로 옮기면 여기서 걸린다.
            assert(!_sightBroadcaster.HasDirtyMovers());
            // 지우기 직전 콜백 — 존 밖에서 이 존의 것을 든 곳을 정리하는 마지막 기회다. 둘 다 토글이 따로라 한 콜백에 합친다
            //   (땅 아이템만 USE_DROP 안에 묶어 두었더니 USE_DROP 0 이면 콜백이 아예 없어 버프 목록 정리도 같이 빠졌을 것이다).
#if USE_DROP
            const GroundItemContext groundCtx = _itemHandler.MakeGroundItemContext();
#endif
#if USE_DROP || USE_COMBAT || USE_PARTY
            _mapManager.CleanupEmptyChannels([&](CZone* zone)
            {
#if USE_DROP
                zone->GetGroundItemManager().ForgetAll(groundCtx);   // 땅의 아이템은 세기만 — 목록은 존과 함께 사라진다(알릴 사람이 없다)
#endif
#if USE_COMBAT
                // 효과 목록은 틱 너머로 주소를 든다 — 이 존의 몬스터가 같이 해제되니 목록에서 먼저 뺀다(BuffSystem.h 「셋째 길」). 사람은 이미 퇴장으로 빠졌다
                _buffSystem.ForgetZone(zone->GetZoneId());
#endif
#if USE_PARTY
                // 빈 방 — 열린 방 수를 내린다(등식: 들어감 = 닫힘 + 열림 — GameMonitor.h _room). 방 번호는 이 뒤 다시 쓸 수 있다(CMapInstance::CreateRoom)
                if (_mapManager.IsRoomMap(zone->GetMapId()))
                {
                    _gameMonitor._room._active.Add(-1);
                    _gameMonitor._room._closed.Inc();
                }
#endif
            });
#else
            _mapManager.CleanupEmptyChannels(nullptr);
#endif
        }

#if USE_DB_WORKER
        // 5-1) DB 주기 저장 — dirty(또는 MOVING) 플레이어만 워커에 enqueue (틱 임계경로 밖 I/O)
        _characterStore.TickPeriodicSave();
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

CPlayer* CGameServer::SpawnPlayer(int64_t sessionId, int32_t actorId, int64_t accountId,
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
    //   가방과 계수기만 만지는 일이라 앞당겨도 다른 것이 안 바뀐다. 전체 목록(SendInventory)은 그대로 아래에서 보낸다.
    if (place.loaded != nullptr)
        _characterStore.LoadInventoryRows(player, *place.loaded);
#endif

#if USE_EXP
    // ── 레벨 정산 — 스탯을 표에서 다시 세우고, 불러온 값이 표와 어긋나면 바로잡는다 ──
    //   ★ NotifyArrival 보다 앞이다. 그 안의 SendCreateMyPlayer 가 곧바로 최대 체력을 실어 보내므로
    //     여기서 안 세우면 첫 화면의 막대가 생성자 기본값(100)으로 나간다.
    //   갈래 둘 — ⑴ 레벨이 표 밖(표를 줄인 패치) ⑵ 경험치가 이미 다음 레벨 몫을 넘음
    //     (표의 필요치를 낮춘 패치, 또는 만렙 상한에 걸려 안 오른 채 저장된 행).
    //     둘 다 "행은 멀쩡한데 표가 바뀐" 경우라 평소엔 한 번도 안 도는 갈래다.
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
    _statSystem.RestoreHpMp(player, place.hp, place.mp);
#endif

    // 본인에게 존·내 캐릭터·숫자·가방·스탯 → 주변과 서로 생성 (순서와 「접속만 보내는 것」은 NotifyArrival 한 곳)
    //   불러온 가방은 위에서 이미 채웠다(스탯 세우기 앞 — 무기 몫 때문). 여기서는 보내기만 한다.
    _placement.NotifyArrival(player, zone, CPlayerPlacement::ArrivalKind::CONNECT);

    return player;
}

void CGameServer::OnConnected(int64_t sessionId)
{
    // 이미 등록된 세션이면 무시 (중복 접속 방어)
    if (_sessionHub.FindPlayer(sessionId) != nullptr)
    {
        _network->RequestDisconnectSession(sessionId);
        return;
    }

#if USE_LOGIN
    // [입장 요청] 접속만으로는 아무것도 안 만든다 — 캐릭터 번호도 계정 번호도 조회도
    //   입장 요청(C2S_ENTER_GAME)을 받아야 정해진다. 여기서는 대기표에 자리만 잡는다.
    //   ★ 자리를 안 잡으면 이 세션이 어느 표에도 없어서, 유령 플레이어를 잡는 유일한 장치인
    //     "동접 = 게임 중 + 불러오는 중 + 입장 대기" 등식이 늘 틀린 채로 울린다.
    //   ※ 입장 요청을 끝내 안 보내는 세션은 여기 남는다. 네트워크 계층의 유휴 타임아웃은
    //     하트비트만 보내도 갱신되므로 안 끊는다 — 그래서 게이지(mmo_enter_waiting)로 지켜본다.
    //     값이 올라가서 안 내려오면 그때 쓸어내기를 넣는다(지금 넣으면 한 번도 안 도는 갈래다).
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
            _network->RequestDisconnectSession(sessionId);
            return;
        }
        SetPendingEnter(sessionId, accountId, actorId, EnterState::LOADING);
        return;
    }
#endif

    // DB 워커가 없으면(토글 OFF·미기동) 예전 그대로 곧바로 스폰 — 기본 맵(마을)의 여유 채널, 시작 구역.
    CZone* zone = _mapManager.FindOrCreateChannel(_defaultMapId);
    if (SpawnPlayer(sessionId, actorId, actorId, zone, EnterPlacement{}) == nullptr)
        _network->RequestDisconnectSession(sessionId);
#endif
}

#if USE_LOGIN
// 같은 계정으로 이미 들어와 있는 세션을 걷어낸다. 걷을 곳이 둘이다 — 게임 중, 그리고 불러오는 중.
//   ★ 맵(계정 → 플레이어)을 안 만들고 훑는 이유: 넣고 빼야 할 표가 둘이라 한쪽이 어긋나면
//     해제된 주소를 들게 된다. 훑기는 어긋날 표가 없다. 그리고 번호를 안 들고 온 접속은 이 길을
//     아예 안 타므로(부하 더미가 전부 그렇다) 훑는 횟수가 사람 수준에 머문다.
//   ※ 모든 계정이 지정 번호로 들어오는 날(로그인 서버가 붙는 날, 또는 부하 더미를 고정 계정으로
//     바꾸는 날)에는 접속마다 전체를 훑게 된다. 그때 재 보고 계정 → 플레이어 맵으로 바꾼다.
void CGameServer::KickDuplicateAccount(int64_t accountId, int64_t exceptSessionId)
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
        _network->RequestDisconnectSession(oldSession);
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
        _network->RequestDisconnectSession(oldSession);
        _gameMonitor._enter._duplicate.Inc();
        break;
    }
}

void CGameServer::RecvEnterGame(int64_t sessionId, CSerialBuffer* pMsg)
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
        _network->RequestDisconnectSession(sessionId);
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
    //   ※ 널 대비 분기를 두지 않는다 — 한 번도 안 타는 갈래이고, 혹시 탄다면 불러오기 없이 스폰해
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
        _network->RequestDisconnectSession(sessionId);
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

void CGameServer::OnDisconnected(int64_t sessionId)
{
    CPlayer* player = _sessionHub.FindPlayer(sessionId);
    if (player == nullptr)
    {
#if USE_DB_WORKER
        // 불러오는 중에 나간 세션 — 플레이어가 아직 없으니 저장도 방송도 없다. 대기표만 비운다.
        //   뒤늦게 오는 결과는 대기표에서 주인을 못 찾아 orphaned 로 세어진다.
        PendingEnter gone;
        TakePendingEnter(sessionId, gone);
#endif
        return;
    }
    _placement.LeaveGame(player);
}

// 떠나보내기(LeaveGame) · 놓기(ReleasePlayer) 는 배치 조각으로 옮겼다 — CPlayerPlacement (게임 서버 나누기 5 단계)

// ── 「존에서 빠짐 · 떠남」 알림 (IPlacementEvents) ── 배치 조각(CPlayerPlacement)이 부른다 — 몸은 옮기기 전 RemoveFromZone · ReleasePlayer 의
//   그 자리 그대로(게임 서버 나누기 5 단계 · 0-③ ⑴⑶). 전투·파티 조각을 떼면 그쪽으로 간다.

// 존에서 빠짐 — 시야 몫 뒤 · CZone::LeaveZone 앞(CPlayerPlacement::RemoveFromZone). 종류를 안 가린다(플레이어·몬스터).
void CGameServer::OnLeavingZone(CCharacter* player)
{
#if USE_COMBAT
    // 부활 대기열에서도 제거 — 위 이동 묶음·아래 LeaveZone 의 섹터 이동 대기열과 같은 이유다(틱 끝 TickRespawn dangling 방지).
    //   죽은 채로 퇴장·존이동하는 경우가 실제 경로다(부활 3초 안에 창을 닫으면 여기로 온다).
    RemoveFromDeadActors(player);
#endif
    (void)player;   // 전투를 끈 빌드엔 할 일이 없다
}

// 떠남 — 최종 저장 뒤 · 칸 비우기(해제) 앞(CPlayerPlacement::ReleasePlayer). 이 사람에게 보낼 세션은 이미 없다.
void CGameServer::OnLeavingGame(CPlayer* player)
{
#if USE_COMBAT
    // 부활 대기열 회수 — 끊김 길에서는 RemoveFromZone 이 이미 하지만, 존을 못 찾은 갈래는 그걸 건너뛴다.
    //   그 상태로 죽어 있을 수는 없어도, 아래 해제 뒤에 대기열이 그 주소를 들고 있으면
    //   다음 틱이 해제된 메모리를 밟는다. 두 번 불러도 무해하므로 여기서도 뺀다.
    RemoveFromDeadActors(player);

    // 지속 효과 목록도 같은 까닭으로 여기서 뺀다(알림 없음 — 세션이 없다). RemoveFromZone 에서는 안 뺀다 — 존 이동이 그 길을 지난다.
    _buffSystem.Forget(player);
#endif

#if USE_PARTY
    // 파티원·요청 표도 같은 까닭으로 여기서 뺀다 — 남은 파티원·요청 상대에게는 알림이 간다(이 사람에게는 안 간다 — 세션이 없다).
    //   끊기면 바로 파티에서 빠지는 규칙(10-16 사용자 결정)이 이 한 줄이다. 존 이동·죽음은 이 길을 안 지난다(파티는 존을 넘는다).
    _partySystem.Forget(player);
#endif
    (void)player;   // 전투·파티를 다 끈 빌드엔 할 일이 없다
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

    // 타입별 최소 패킷 크기 검증 — 분기 표에서 찾는다(크기와 처리기가 한 줄이라 둘이 어긋날 수 없다 — 게임 서버 나누기 0-④).
    //   등록 안 된 번호·크기 미달은 버리고 오류로 센다(전과 같다).
    const CPacketRoutes::Route* route = _routes.Find(header.type);
    if (route == nullptr || header.size < route->minSize)
    {
        _monitor._packetErrors.Inc();
        pMsg->SubRef();
        return;
    }

    // 입장 요청만 아래 문턱 "앞"에서 가른다 — 이 패킷이 올 때는 아직 플레이어가 없어서,
    //   문턱 뒤에 두면 FindPlayer 가 nullptr 을 돌려 영영 핸들러에 못 닿는다.
    //   나머지 패킷은 지금까지와 똑같이 플레이어를 찾은 뒤에 간다.
    //   (표에서는 「찾기 전 처리기」가 있는 줄 — 지금은 USE_LOGIN 의 입장 요청 하나뿐이다. RegisterRoutes)
    if (route->onSession)
    {
        route->onSession(sessionId, pMsg);
        pMsg->SubRef();
        return;
    }

    // 경계 계층: sessionId → CPlayer* 변환은 여기 한 번뿐 — 이후 컨텐츠 로직은 세션 ID 를 다시 쓰지 않는다.
    //   핸들러(RecvXxx)는 CPlayer* 를 받고, 브로드캐스트·섹터 쪽은 CCharacter* 로 받는다(몬스터도 같은 길).
    CPlayer* player = _sessionHub.FindPlayer(sessionId);
    if (player == nullptr)
    {
        pMsg->SubRef();
        return;
    }

    // 처리기가 없는 줄 — C2S_HEARTBEAT 등 — 게임 로직 처리 불필요 (타임아웃 갱신은 IOCPServer 수신 시점에서 처리)
    if (route->onPlayer)
        route->onPlayer(player, pMsg);

    // SerialBuffer 해제 (소유권 1 반환 — RefCount=0이면 프리리스트로)
    pMsg->SubRef();
}

// 받은 패킷의 분기 표 — 번호마다 최소 크기와 처리기를 한 줄로(전에는 크기 표 GetExpectedSize 의 switch 와 OnReceived 의 switch 가 따로였다 — 게임 서버 나누기 0-④).
//   크기 줄을 빠뜨려 패킷이 조용히 버려지던 함정(아래 줄마다 남긴 옛 주석)은 처리기와 크기가 한 줄이 되며 구조로 막힌다.
//   토글이 꺼진 컨텐츠는 줄 자체가 없다 — 그 번호는 등록 안 된 번호로 버려지고 _packetErrors 가 오른다(전과 같다).
bool CGameServer::RegisterRoutes()
{
    bool ok = true;
    ok &= _routes.OnPlayer(MsgType::C2S_MOVE_START, sizeof(MSG_C2S_MOVE_START), [this](CPlayer* p, CSerialBuffer* m) { RecvMoveStart(p, m); });
    ok &= _routes.OnPlayer(MsgType::C2S_MOVE_STOP,  sizeof(MSG_C2S_MOVE_STOP),  [this](CPlayer* p, CSerialBuffer* m) { RecvMoveStop(p, m); });
    ok &= _routes.OnPlayer(MsgType::C2S_CHAT,       sizeof(MsgHeader) + sizeof(ChatChar),   // 가변 길이: 최소 1글자
                           [this](CPlayer* p, CSerialBuffer* m) { RecvChat(p, m); });
    ok &= _routes.OnPlayer(MsgType::C2S_ZONE_CHANGE, sizeof(MSG_C2S_ZONE_CHANGE), [this](CPlayer* p, CSerialBuffer* m) { RecvZoneChange(p, m); });
    // 이 줄이 없으면 포털 키가 조용히 버려진다(아래 물약과 같은 함정 — 답도 로그도 없다). 토글 없음(포털 표처럼)
    ok &= _routes.OnPlayer(MsgType::C2S_PORTAL_USE, sizeof(MSG_C2S_PORTAL_USE), [this](CPlayer* p, CSerialBuffer* m) { RecvPortalUse(p, m); });
    ok &= _routes.Size(MsgType::C2S_HEARTBEAT, sizeof(MSG_C2S_HEARTBEAT));   // 처리기 없음 — 타임아웃 갱신은 IOCPServer 수신 시점
#if USE_COMBAT
    // 이 줄을 빠뜨리면 등록 안 된 번호가 되어 OnReceived가 패킷을 조용히 버린다(로그 없음, _packetErrors만 증가).
    ok &= _routes.OnPlayer(MsgType::C2S_ATTACK, sizeof(MSG_C2S_ATTACK), [this](CPlayer* p, CSerialBuffer* m) { RecvAttack(p, m); });
#endif
    ok &= _routes.OnPlayer(MsgType::C2S_ADMIN_LOGIN, sizeof(MSG_C2S_ADMIN_LOGIN), [this](CPlayer* p, CSerialBuffer* m) { RecvAdminLogin(p, m); });
    // 이 줄이 없으면 입장 요청이 0 을 받아 파싱 오류로 샌다 — 로그도 안 남고 _packetErrors 만 오른다.
    //   1단계에서는 크기만 등록하고 핸들러는 없다(OnReceived 에서 처리기 없는 줄로 넘어가거나,
    //   플레이어가 아직 없으면 그 앞 문턱에서 버려진다). 서버 동작은 그대로다.
    //   (USE_LOGIN 이면 플레이어를 찾기 「전」에 처리한다 — 이 패킷이 올 때는 아직 플레이어가 없다. 끄면 크기만)
#if USE_LOGIN
    ok &= _routes.OnSession(MsgType::C2S_ENTER_GAME, sizeof(MSG_C2S_ENTER_GAME), [this](int64_t s, CSerialBuffer* m) { RecvEnterGame(s, m); });
#else
    ok &= _routes.Size(MsgType::C2S_ENTER_GAME, sizeof(MSG_C2S_ENTER_GAME));
#endif
#if USE_INVENTORY
    // 물약 · 끼우기·빼기(USE_EQUIP) · 줍기(USE_DROP) — 가방·땅 조각이 자기 줄을 등록한다(CItemHandler::RegisterRoutes · 게임 서버 나누기 6 단계)
    ok &= _itemHandler.RegisterRoutes(_routes);
#endif
#if USE_SKILL
    // 이 줄이 없으면 스킬 요청이 조용히 버려진다(위 물약과 같은 함정 — 거절 패킷도 안 간다)
    ok &= _routes.OnPlayer(MsgType::C2S_USE_SKILL, sizeof(MSG_C2S_USE_SKILL), [this](CPlayer* p, CSerialBuffer* m) { RecvUseSkill(p, m); });
#endif
#if USE_PARTY
    // 파티 요청 여섯 — 파티·방 조각이 자기 줄을 등록한다(CPartyHandler::RegisterRoutes · 게임 서버 나누기 7 단계)
    ok &= _partyHandler.RegisterRoutes(_routes);
#endif
    return ok;
}

// ==========================================================================
// 패킷 핸들러
// ==========================================================================

void CGameServer::RecvMoveStart(CPlayer* player, CSerialBuffer* pMsg)
{
#if USE_COMBAT
    if (player->_isDead)
        return;   // 시체는 안 움직인다 — 부활 전까지 조작을 막는다
#endif

    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    MSG_C2S_MOVE_START recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // Direction 범위 검증 — 유효 값은 Actor.h 한 곳(IsMoveDirection)
    if (!IsMoveDirection(recvMsg.direction))
        return;

    Direction dir = static_cast<Direction>(recvMsg.direction);

    // 이동 중 방향 전환: 같은 방향이면 무시, 다른 방향이면 갱신 + 재브로드캐스트
    // ※ 좌표 수용보다 먼저 검사 — MOVING 중 반복 전송으로 서버 좌표를 밀어내는 치트 방지
    if (player->_moveState == MoveState::MOVING)
    {
        if (dir == player->_direction)
            return;

        // 벽 방향 검증: 벽 위치에서 벽 쪽으로 방향 전환 시 정지 처리
        if (zone->IsBlockedByWall(player->_x, player->_y, dir))
        {
            player->_moveState = MoveState::IDLE;
            player->_direction = dir;

#if USE_DB_WORKER
            player->_dbDirty = true;   // MOVING → IDLE 전이(주기저장 MOVING 조건에서 빠지므로 최종위치 마킹)
#endif

            _sightBroadcaster.NotifyMoveStop(zone, player, false);  // 본인 포함
            return;
        }

        player->_direction = dir;
        player->_lastSyncX = player->_x;
        player->_lastSyncY = player->_y;

        _sightBroadcaster.NotifyMoveStart(zone, player);
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
            zone->ClampToMap(acceptX, acceptY);
            player->_x = acceptX;
            player->_y = acceptY;

#if USE_DB_WORKER
            player->_dbDirty = true;   // 클라 예측 좌표 채택 → 위치 변경, 저장 대상
#endif

            // 좌표 수용으로 섹터가 변경되었을 수 있으므로 재계산
            zone->UpdateSectorAndQueue(player);
        }
        // 범위 초과 또는 예산 부족 시 서버 좌표 유지 (치트 방지)
    }

    // 벽 방향 검증: 벽 위치에서 벽 쪽으로 이동 시도 시 차단
    if (zone->IsBlockedByWall(player->_x, player->_y, dir))
    {
        _sightBroadcaster.SendSyncPosition(player);        // 본인 — 서버 권위 좌표로 되돌림
        _sightBroadcaster.NotifyMoveSync(zone, player);    // 주변 — 위에서 채택한 좌표를 전파.
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
    _sightBroadcaster.NotifyMoveStart(zone, player);
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

    // Direction 범위 검증 — 유효 값은 Actor.h 한 곳(IsMoveDirection)
    if (!IsMoveDirection(recvMsg.direction))
        return;

    // 서버 권위 모델: 서버 좌표 유지, 상태만 변경
    player->_direction = static_cast<Direction>(recvMsg.direction);
    player->_moveState = MoveState::IDLE;

#if USE_DB_WORKER
    player->_dbDirty = true;   // 정지 → IDLE 전이(주기저장 MOVING 조건에서 빠지므로 최종위치 마킹)
#endif

    // 섹터 이동 판정 — 넘었으면 시야 진입/이탈을 대기열에 추가 (틱 끝에 배치 처리)
    zone->UpdateSectorAndQueue(player);

    // 주변에 MOVE_STOP 브로드캐스트 (묶음 모드: dirty 마킹만)
    _sightBroadcaster.NotifyMoveStop(zone, player, true);
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

#if USE_INVENTORY
    // '/' 로 시작하면 명령이다 — 방송하지 않는다. 운영자가 아닌 사람의 /item 이 채팅으로 새지 않게,
    //   권한 검사보다 "방송 안 함"이 먼저다. 부하 더미의 채팅 문구에는 '/' 로 시작하는 것이 없다.
    if (msgLen > 0 && recvMsg.message[0] == u'/')
    {
        HandleChatCommand(player, recvMsg.message, msgLen);
        return;
    }
#endif

#if USE_BROADCAST_BUNDLE
    // [digest] 즉시 브로드캐스트 대신 보류 등록 — 틱 끝 digest에 합류 (실송신 시점은 기존과 동일한 틱 끝 flush).
    // 소스 섹터는 수신 시점 좌표로 고정 → 기존(즉시 배포)의 AOI 의미 보존.
    _sightBroadcaster.RegisterSectorItem(zone, player->_zoneId, player->_sectorX, player->_sectorY,
        MakeChat(player->_actorId, player->_displayChar, player->_colorIndex,
            recvMsg.message, msgLen));
#else
    // 본인 포함 브로드캐스트 (송신 함수가 소유권 소비)
    _sightBroadcaster.BroadcastAroundSector(zone, player,
        MakeChat(player->_actorId, player->_displayChar, player->_colorIndex,
            recvMsg.message, msgLen), false);
#endif
}

#if USE_INVENTORY
// ==========================================================================
// 채팅 명령 · 인벤토리 (USE_INVENTORY)
// ==========================================================================

namespace
{
    // 채팅(UTF-16) → 낱말들. 명령과 인자가 ASCII 라 그 밖의 글자는 '?' 로 바꾼다
    //   (한글 인자를 받는 명령이 생기면 그때 UTF-8 로 옮긴다).
    //   ★ NUL 에서 멈춘다 — 웹·콘솔 클라는 끝에 NUL 을 붙여 보내고(proto.js chat()), RecvChat 의
    //     글자 수는 헤더 크기로 역산해 그 NUL 까지 센다. 안 멈추면 마지막 낱말이 "5\0" 이 되어 숫자로 안 읽힌다.
    void SplitCommandWords(const ChatChar* msg, uint16_t len, std::vector<std::string>& out)
    {
        out.clear();
        std::string cur;
        for (uint16_t i = 0; i < len; ++i)
        {
            const ChatChar c = msg[i];
            if (c == u'\0')
                break;
            if (c == u' ' || c == u'\t')
            {
                if (!cur.empty())
                {
                    out.push_back(cur);
                    cur.clear();
                }
                continue;
            }
            cur += (c < 0x80) ? static_cast<char>(c) : '?';
        }
        if (!cur.empty())
            out.push_back(cur);
    }

    // 속도 — 본인 + 주변(BroadcastAroundSector, excludeSelf=false — Protocol.h MSG_S2C_ACTOR_SPEED).
    //   부르는 곳이 /gm 하나라 명령 구역에 둔다. 속도를 바꾸는 둘째 길(효과 등)이 생기면 위의 Make 묶음으로 옮긴다.
    CSerialBuffer* MakeActorSpeed(int32_t actorId, int32_t speed)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ACTOR_SPEED::TYPE);
        *buf << static_cast<int>(actorId);
        *buf << static_cast<int>(speed);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ACTOR_SPEED));
        return buf;
    }
}

void CGameServer::HandleChatCommand(CPlayer* player, const ChatChar* msg, uint16_t len)
{
    CGameMonitor::InventoryCounters& counters = _gameMonitor._inventory;

    // 명령은 사람이 치는 것이라 드물다 — 할당을 아끼지 않는다
    std::vector<std::string> words;
    SplitCommandWords(msg, len, words);
    const std::string cmd = words.empty() ? std::string("/") : words[0];   // 첫 글자가 '/' 라 비지 않는다

    // 명령 표 — 이름 → 처리 함수(if 사슬 금지 — 명령이 늘면 한 줄 더한다).
    //   지금 명령은 전부 운영자 전용이라 권한은 표에서 찾은 뒤 한 번 본다. 누구나 쓰는 명령이 생기면 칸을 더한다.
    using Runner = void (CGameServer::*)(CPlayer*, const std::vector<std::string>&);
    struct Command
    {
        const char* name;
        Runner      run;
    };
    static constexpr Command COMMANDS[] =
    {
        { "/item", &CGameServer::RunItemCommand },   // /item <아이템번호> [개수]
        { "/gm",   &CGameServer::RunGmCommand   },   // /gm — GM 모드 켜고 끄기
    };

    const Command* found = nullptr;
    for (const Command& c : COMMANDS)
    {
        if (cmd == c.name)
        {
            found = &c;
            break;
        }
    }

    if (found == nullptr)
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::UNKNOWN_COMMAND)].Inc();
        _selfNotifier.SendSystemMessage(player, "모르는 명령입니다: " + cmd);
        return;
    }

    if (!player->_isAdmin)
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::NOT_ADMIN)].Inc();
        _selfNotifier.SendSystemMessage(player, "운영자만 쓸 수 있는 명령입니다: " + cmd);
        return;
    }

    (this->*(found->run))(player, words);
}

// /gm — GM 모드 켜고 끄기(토글). 값과 저장하지 않는 까닭은 Player.h 의 GM 칸.
//   켜면 속도·최대 체력·공격력을 GM 값으로 바꾸고 체력·마력을 가득 채운다. 끄면 속도를 되돌리고 스탯을 레벨 표로 다시 세운다
//   (체력·마력은 넘는 몫만 깎는다 — RecalcStats refill=false).
//   알림은 레벨업과 같은 셋(체력 본인+주변 · 공격력 본인 · 마력 본인)에 속도(본인+주변)를 더한다 — 남의 화면도 그 속도로 앞질러 그린다.
void CGameServer::RunGmCommand(CPlayer* player, const std::vector<std::string>& words)
{
    CGameMonitor::InventoryCounters& counters = _gameMonitor._inventory;

    if (words.size() != 1)
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::BAD_ARGS)].Inc();
        _selfNotifier.SendSystemMessage(player, "쓰는 법: /gm (켜고 끈다 — 인자 없음)");
        return;
    }

    // 쓰러진 채 켜면 아래 「가득 채움」이 부활 절차(마을로 보내기 · 부활 방송) 없이 되살린다 — 거절한다.
    //   끄기도 같이 막는다(쓰러진 채로는 상태를 안 바꾼다는 한 줄 규칙 — 부활하면 그때 끄면 된다).
    if (player->_isDead)
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::DEAD)].Inc();
        _selfNotifier.SendSystemMessage(player, "쓰러진 채로는 GM 모드를 바꿀 수 없습니다");
        return;
    }

    player->_gmMode = !player->_gmMode;
    if (player->_gmMode)
    {
        player->_speedBeforeGm = player->_speed;
        player->_speed         = CPlayer::GM_SPEED;
    }
    else
    {
        player->_speed = player->_speedBeforeGm;
    }
    _statSystem.RecalcStats(player, player->_gmMode);   // 켤 때만 가득 채운다
    counters._gmToggled[player->_gmMode ? 1 : 0].Inc();

    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone != nullptr)
    {
        _sightBroadcaster.BroadcastAroundSector(zone, player, MakeActorStats(player->_actorId, player->_hp, player->_maxHp), false);
        _sightBroadcaster.BroadcastAroundSector(zone, player, MakeActorSpeed(player->_actorId, player->_speed), false);
    }
    _selfNotifier.SendMyStats(player);
#if USE_SKILL
    _selfNotifier.SendActorMp(player);   // 켤 때 가득 채운 마력 — 끌 때는 안 바뀌지만 같은 한 벌로 보낸다
#endif

    SLOG_INFO("[GM] /gm (account={}, player={}) -> {} (speed {}, maxHp {}, attack {})",
              player->_accountId, player->_actorId, player->_gmMode ? "on" : "off",
              player->_speed, player->_maxHp, player->_attackPower);
    // 웹 채팅창 한 줄(400px)에 값이 다 들어가는 길이로 — 길면 끝이 「…」로 잘려 공격력이 안 보인다(10-16 헤드리스 사진)
    _selfNotifier.SendSystemMessage(player, std::string(player->_gmMode ? "GM 켬" : "GM 끔")
        + " — 속도 " + std::to_string(player->_speed) + " · 체력 " + std::to_string(player->_maxHp)
        + " · 공격력 " + std::to_string(player->_attackPower));
}

// /item <아이템번호> [개수] — 개수를 안 적으면 1. 가방에 바로 넣는다(드랍이 생기기 전까지 가방을 채우던 길).
void CGameServer::RunItemCommand(CPlayer* player, const std::vector<std::string>& words)
{
    CGameMonitor::InventoryCounters& counters = _gameMonitor._inventory;

    // /item <아이템번호> [개수]  — 개수를 안 적으면 1
    int64_t itemId = 0;
    int64_t count  = 1;
    const bool okId    = words.size() >= 2 && GameDataCsv::ParseInt(words[1], itemId);
    const bool okCount = words.size() < 3 || GameDataCsv::ParseInt(words[2], count);
    if (!okId || !okCount || words.size() > 3 || count < 1 || count > 9999)
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::BAD_ARGS)].Inc();
        _selfNotifier.SendSystemMessage(player, "쓰는 법: /item <아이템번호> [개수 1~9999]");
        return;
    }

    // 번호 범위를 먼저 본다 — int32 로 줄이기 전에 걸러야 큰 값이 엉뚱한 번호로 접히지 않는다
    const ItemDef* def = (itemId > 0 && itemId <= CItemTable::MAX_ITEM_ID)
                       ? _gameDataFiles.Item().Find(static_cast<int32_t>(itemId))
                       : nullptr;
    if (def == nullptr)
    {
        counters._addRejected[static_cast<int>(InvAddResult::UNKNOWN_ITEM)].Inc();
        _selfNotifier.SendSystemMessage(player, "없는 아이템 번호입니다: " + words[1]);
        return;
    }

    const InvAddResult result = player->_inventory.TryAdd(*def, static_cast<int32_t>(count));
    switch (result)
    {
    case InvAddResult::OK:
        counters._itemsAdded.Add(count);
        SLOG_INFO("[ITEM] /item (account={}, player={}) -> {} x{} (item {})",
                  player->_accountId, player->_actorId, def->name, count, def->itemId);
        _itemHandler.FlushInventory(player);
        return;

    case InvAddResult::TAB_FULL:
        _selfNotifier.SendSystemMessage(player, std::string("가방에 자리가 없습니다 — ") + InvTabDisplayName(def->tab)
                                  + " 탭에 " + def->name + " " + std::to_string(count) + "개를 다 둘 수 없습니다");
        break;

    case InvAddResult::UNKNOWN_ITEM:   // TryAdd 는 이 둘을 안 돌려준다 — 위에서 이미 걸렀다
    case InvAddResult::BAD_COUNT:
        _selfNotifier.SendSystemMessage(player, std::string("넣지 못했습니다: ") + InvAddResultName(result));
        break;

    case InvAddResult::COUNT:          // 값이 아니라 개수 표시 — 아래 배열 색인으로 쓰면 범위 밖이다
        assert(false && "TryAdd 가 COUNT 를 돌려줄 수는 없다");
        return;
    }
    counters._addRejected[static_cast<int>(result)].Inc();
}

// 바뀐 칸 내보내기(FlushInventory) 는 가방·땅 조각으로 옮겼다 — CItemHandler (게임 서버 나누기 6 단계)

// 불러온 가방 줄 넣기(LoadInventoryRows) 는 캐릭터 저장소 조각으로 옮겼다 — CCharacterStore (게임 서버 나누기 4 단계)

// 가방 전체(SendInventory) · 알림 한 줄(SendSystemMessage) 은 본인 알림 조각으로 옮겼다 — CSelfNotifier (게임 서버 나누기 2 단계)

// 물약 쓰기(RecvUseItem) · 끼우기·빼기(RecvEquip · RecvUnequip · ApplyEquip) 는 가방·땅 조각으로 옮겼다 — CItemHandler (게임 서버 나누기 6 단계)
#endif // USE_INVENTORY

// ==========================================================================
// 패킷 전송 추상화
// ==========================================================================

// ==========================================================================
// 계층 경계 헬퍼
// ==========================================================================

// 캐릭터 번호 발급(AllocPlayerId)은 세션 허브로 옮겼다 — CSessionHub::AllocActorId (게임 서버 나누기 1 단계)

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
// 단일 플레이어에게 패킷 전송(SendPacket)은 세션 허브로 옮겼다 — CSessionHub::SendPacket (게임 서버 나누기 1 단계)

// 주변 방송(BroadcastAroundSector) · 이동 알림 셋(NotifyMoveStart/Stop/Sync) · 존 알림(IZoneEvents) 은 시야 방송 조각으로 옮겼다 — CSightBroadcaster (게임 서버 나누기 3 단계)

// 존 정보 · 내 캐릭터 생성 보내기(SendZoneInfo · SendCreateMyPlayer) 는 배치 조각으로 옮겼다 — CPlayerPlacement (게임 서버 나누기 5 단계)

// 남의 캐릭터 생성·삭제 보내기 · 인바운드 멤버십 묶음 · 내 자리(SendSyncPosition) 는 시야 방송 조각으로 옮겼다 — CSightBroadcaster (게임 서버 나누기 3 단계)

void CGameServer::RecvZoneChange(CPlayer* player, CSerialBuffer* pMsg)
{
#if USE_COMBAT
    if (player->_isDead)
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
//   세는 곳은 여기 한 곳 — 칸의 합(_used + Σ _rejected) = 받은 요청 수(GameMonitor.h _portal). 방어 갈래 둘(존 없음 ·
//   목적지 채널 못 얻음)은 안 센다 — 지금 일어나는 길이 없고, 일어나면 앞엣것은 조용히, 뒤엣것은 로그로 드러난다.
void CGameServer::RecvPortalUse(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_PORTAL_USE recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    auto& pc = _gameMonitor._portal;
    auto reject = [&](PortalReject why) { pc._rejected[static_cast<int>(why)].Inc(); };

#if USE_COMBAT
    if (player->_isDead)
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

// 방 입장(EnterRoom) · 거절 사유 → 와이어(SendRoomReject) 는 파티·방 조각으로 옮겼다 — CPartyHandler (게임 서버 나누기 7 단계)

// 존에서 존으로 옮기기(TransferPlayer) 는 배치 조각으로 옮겼다 — CPlayerPlacement (게임 서버 나누기 5 단계)

// ── 운영자 인증 ──

// 부하 테스트 도구용 고정 키 — 실서비스 인증 아님.
// 와이어에 평문으로 흐르고 해시·시도 제한도 없다. admin 권한 효과는 채널 인원 제한 우회(CMapInstance::HasRoom)와
//   다른 맵 지정 이동(CMapManager::PickZoneChangeTarget — 일반은 포털로만) 둘, 그리고 채팅 운영 명령이다.
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
        _sessionHub.SendPacket(player, MakeAdminLoginOk());
    }
    else
    {
        _sessionHub.SendPacket(player, MakeAdminLoginFail());
    }
}

#if USE_COMBAT
// ==========================================================================
// 전투 (USE_COMBAT)
//
// 공격 흐름(쿨타임 → 대상 찾기 → 피해 → 죽음 · 지표)은 CCombatSystem(CombatSystem.h)이 하고,
//   여기서는 그 알림(ICombatEvents)을 받아 세계에 적용한다 — 송신 · 죽은 목록 등록 · 보상.
//   판정 함수(쿨타임·사거리·피해)는 namespace Combat 이 하고, 그쪽은 존·섹터·송신을 모른다.
//
// [수신자 범위의 한계] 전투 패킷은 전부 "공격자 주변 9섹터"에 뿌린다. 사거리가 짧아(1.5타일 + 두 몸 반지름 — 상한 4 로 최대 9.5)
//   맞은 쪽은 반드시 그 안에 있다(섹터 크기가 그 최대 사거리 이상일 때 — 지금 20. 범위 스킬은 칸이 더 멀어 칸 상한 5 · 대상 반지름 4 면
//   맞는 쪽 중심이 약 13.4 — 섹터를 그보다 작게 줄이면 이 가정부터 다시 볼 것). 다만 맞은 쪽 주변 구경꾼 중 공격자 9섹터 밖인 사람은 못 본다 —
//   섹터가 타일 여러 개라 실제로 겹치지 않는 경우가 드물고, 어긋나도 다음 CREATE 때
//   HP가 다시 맞춰진다(CREATE 가 체력을 싣는다 — ActorSnapshot). 이번 범위에서는 여기까지만 한다.
// ==========================================================================

// 세션 핸들러 — 세션이 있어야 성립하는 것만 보고 본체로 넘긴다.
//   몬스터는 패킷을 안 보내므로 이 경로를 못 탄다. 그래서 본체가 따로 있다.
void CGameServer::RecvAttack(CPlayer* player, CSerialBuffer* pMsg)
{
    (void)pMsg;   // C2S_ATTACK은 바디가 없다 — 방향은 서버가 들고 있고, 대상도 서버가 고른다

    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    _combatSystem.PerformAttack(zone, player, _frameCount);
}

// ── 공격 알림 셋 (ICombatEvents) ──
//   공격 흐름(쿨타임 → 대상 찾기 → 피해 → 죽음 · 지표)은 CCombatSystem 이 하고, 여기는 받은 일을 세계에 적용한다.
//   방송은 셋 다 공격자 주변 9섹터 · 본인 포함(excludeSelf=false) — 위 [수신자 범위의 한계] 참조.

// 휘둘렀다 — 대상 유무와 무관하게 온다(헛스윙도 그려야 한다)
void CGameServer::OnSwing(CZone* zone, CCharacter* attacker)
{
    _sightBroadcaster.BroadcastAroundSector(zone, attacker,
        MakeAttack(attacker->_actorId, static_cast<uint8_t>(attacker->_direction)), false);
}

// 맞혔다 — damage·remainHp 는 CCombatSystem 이 한 번 센 값 그대로다(깎인 체력과 화면의 숫자가 갈리지 않게)
void CGameServer::OnHit(CZone* zone, CCharacter* attacker, CCharacter* target, int32_t damage, int32_t remainHp)
{
    _sightBroadcaster.BroadcastAroundSector(zone, attacker,
        MakeDamage(target->_actorId, attacker->_actorId, damage, remainHp), false);
}

// 죽었다 — 플레이어 죽은 목록 → 죽음 방송 → 경험치 → 드랍, 이 순서다.
//   ★ 순서를 바꾸면 레벨업 체력(ACTOR_STATS)이 죽음(DEATH)보다 먼저 가 화면이 뒤집힌다.
//   죽음 필드(_isDead·_deathFrame·_moveState)와 죽음 계수는 CCombatSystem 이 이미 적었다.
//   죽은 몬스터는 시체로 남는다(되살리지 않는다) — 죽은 목록은 플레이어 것만 든다.
void CGameServer::OnDeath(CZone* zone, CCharacter* attacker, CCharacter* target)
{
    // 플레이어 죽은 목록 — 부활 자리·시간이 임시 사양(제자리 3초)이라 아직 게임 서버가 든다(10-05 결정).
    //   몬스터를 여기 넣으면 존이 지워질 때 밖에서 빼 줘야 하는 약속이 다시 생긴다(MonsterManager.h [왜 떼어냈나]).
    if (As<CPlayer>(target) != nullptr)
        _deadActors.push_back(target);

    _sightBroadcaster.BroadcastAroundSector(zone, attacker, MakeDeath(target->_actorId), false);

    // 죽으면 지속 효과가 풀린다(10-01) — 사람·몬스터 공통. 와이어 = DEATH → ACTOR_BUFF(0) → MY_STATS(바탕 공격력), 아래 레벨업 MY_STATS 보다 앞.
    //   ★ 몬스터는 맨 끝 8) Despawn 이 해제하므로 그 "앞"인 여기서 목록에서 빼야 한다 — 뒤에 두면 해제된 주소가 목록에 남는다.
    _buffSystem.ClearAll(target);

#if USE_PARTY && USE_MONSTER
    // ── 6·7 의 갈림 — 방 맵의 보스(몬스터 표 boss)면 방 클리어다: 보상이 막타 한 명이 아니라 방 안 사람 각자(RewardRoom)이고,
    //   남은 몬스터는 사라진다(RequestClear — 지우기는 다음 몬스터 틱. 여기는 범위 스킬의 대상 버퍼를 도는 중일 수 있다 — MonsterManager.h).
    //   보스가 아닌 방 몬스터(가짜 뱀왕)는 필드와 같은 막타 길로 간다 — 경험치 0 · 드랍 줄 없음이면 아무것도 안 생긴다(10-18 · snakeking-plan.md D5).
    //   (10-17 엔 보스 표식이 없어 방의 몬스터면 종류를 안 가렸다 — room-plan.md 「내가 정한 기본값」 4)
    CMonster* const roomMonster = _mapManager.IsRoomMap(zone->GetMapId()) ? As<CMonster>(target) : nullptr;
    const bool roomReward = (roomMonster != nullptr) && roomMonster->_def.boss;
    if (roomReward)
    {
        _rewardHandler.RewardRoom(zone, roomMonster);
        zone->GetMonsterManager().RequestClear();
        _gameMonitor._room._cleared.Inc();
    }
#else
    const bool roomReward = false;
#endif

#if USE_EXP && USE_MONSTER
    // ── 6) 경험치 — 사람이 몬스터를 잡았을 때만 ──
    //   ★ 막타 한 명에게만 준다. 나눠 주려면 누가 얼마나 때렸는지를 몬스터가 들고 있어야 하고,
    //     그건 퇴장 경로가 훑어야 할 대기열이 하나 더 느는 일이다(Monster.h [대상] 과 같은 이유).
    //   때린 쪽은 CCharacter* 라 As<CPlayer> 로 확인 후 내려 바꾼다.
    //   PvP 는 안 준다: 당한 쪽이 몬스터일 때만이다. 방 맵은 위 RewardRoom 이 이미 줬다.
    CPlayer* killer = As<CPlayer>(attacker);
    if (!roomReward && killer != nullptr && target->_kind == CharacterKind::MONSTER)
        _rewardHandler.GrantMonsterExp(zone, killer, As<CMonster>(target)->_def.exp);   // 잡힌 몬스터 종류의 경험치(몬스터 표)
#endif

#if USE_DROP
    // ── 7) 드랍 — 사람이 몬스터를 잡았을 때만(경험치와 같은 조건) ──
    //   막타 한 명이 주인이다(경험치와 같은 이유 — 누가 얼마나 때렸는지 몬스터가 들고 있지 않다). 방 맵은 위 RewardRoom 이 이미 줬다.
    CPlayer*  looter = As<CPlayer>(attacker);
    CMonster* prey   = As<CMonster>(target);
    if (!roomReward && looter != nullptr && prey != nullptr)
        _rewardHandler.DropLoot(zone, looter, prey);
#endif
    (void)roomReward;   // 경험치·드랍을 다 끈 빌드에서도 쓰인 값으로 둔다

#if USE_MONSTER
    // ── 8) 죽은 몬스터 지우기 — 보상까지 끝낸 맨 끝(이 아래로, 그리고 부른 공격 흐름도 target 을 안 쓴다) ──
    //   시체는 클라 연출이다(웹 클라는 죽어 있는 상대의 DELETE 를 받으면 흐려지며 지운다). 서버가 들고 있으면
    //   나중에 다가온 사람 화면에 시체가 다시 나타나고, 스폰이 새로 채우니 객체가 쌓인다(10-09).
    //   존에서 빼는 길은 플레이어 퇴장과 같다 — 주변 DELETE · 옛 섹터 시청자 · 같은 틱 이동자 · 이동 묶음 빼기 · LeaveZone.
    //   객체를 놓고 그 점을 비우는 것은 존의 몬스터 관리자다 — 빈 점은 그 구역 다음 리젠 때 찬다.
    if (CMonster* dead = As<CMonster>(target))
    {
        _placement.RemoveFromZone(zone, dead);
        zone->GetMonsterManager().Despawn(dead);   // ← 여기서 해제된다
    }
#endif
}

// 몬스터 경험치 주기(GrantMonsterExp) 는 처치 보상 조각으로 옮겼다 — CRewardHandler (게임 서버 나누기 8 단계)

// 방 보상(RewardRoom) 은 처치 보상 조각으로 옮겼다 — CRewardHandler (게임 서버 나누기 8 단계)

#if USE_SKILL
// ==========================================================================
// 스킬 — 판정·비용·대상·효과는 CSkillSystem(SkillSystem.cpp), 여기는 받기와 거절 답만
//   성공의 결과는 알림으로 돌아온다 — 시전(OnCast, 아래) → 피해·죽음·보상(위 OnHit·OnDeath — 일반 공격과 같은 함수)
// ==========================================================================
void CGameServer::RecvUseSkill(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_USE_SKILL recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    // 거절이면 아무것도 안 바뀌었다(비용은 모든 검사 뒤에만 뗀다) — 사유만 본인에게
    const SkillReject reject = _skillSystem.UseSkill(zone, player, recvMsg.skillId, _frameCount);
    if (reject != SkillReject::COUNT)
        _sessionHub.SendPacket(player, MakeSkillFail(recvMsg.skillId, ToSkillFail(reject)));
}

// 시전했다 — 비용을 뗀 뒤, 효과 전에 온다. 와이어 순서 = 시전 모양 → 시전자 체력 → 시전자 마력(본인만) → (효과의 DAMAGE …)
void CGameServer::OnCast(CZone* zone, CCharacter* caster, const SkillDef& def)
{
    _sightBroadcaster.BroadcastAroundSector(zone, caster,
        MakeSkillCast(caster->_actorId, static_cast<uint8_t>(def.skillId), static_cast<uint8_t>(caster->_direction)), false);

    // 체력을 뗐으면 본인과 주변에 — 남의 머리 위 막대도 줄어야 한다(물약·레벨업 때 체력 방송과 같은 꼴)
    if (def.costHpPct > 0)
        _sightBroadcaster.BroadcastAroundSector(zone, caster, MakeActorStats(caster->_actorId, caster->_hp, caster->_maxHp), false);

    // 마력을 뗐으면 시전자 본인에게만 — 남의 마력은 화면에 안 그린다(주변 방송 금지, Protocol.h MSG_S2C_ACTOR_MP).
    //   시전자가 사람일 때만이다 — 몬스터는 받을 세션이 없다(몬스터 시전도 이 함수를 지나지만 몬스터 스킬은 마력이 0 이라 여기 안 온다).
    if (def.costMp > 0)
    {
        if (CPlayer* player = As<CPlayer>(caster))
            _selfNotifier.SendActorMp(player);
    }
}

// 되살려라 — 효과 REVIVE(시전 알림 뒤). 요청만 넘긴다: 깔기는 존의 몬스터 관리자가 다음 틱 리젠에서(빈 점마다 하나 — 등장 알림은 OnMonsterSpawn 그대로).
//   여기서 깔지 않는 이유 — 몬스터의 시전은 그 관리자의 판단 순회 안에서 온다(MonsterManager.h RequestRevive ★).
void CGameServer::OnRevive(CZone* zone, CCharacter* caster, int32_t monsterId)
{
    (void)caster;
#if USE_MONSTER
    zone->GetMonsterManager().RequestRevive(monsterId);
#else
    (void)zone;
    (void)monsterId;   // 몬스터를 끈 빌드엔 되살리기를 쓰는 몬스터가 없다(되살리기 줄은 user=monster)
#endif
}

// 마력 알림(SendActorMp) · 남은 쿨타임(SendActiveSkillCooldowns) 은 본인 알림 조각으로 옮겼다 — CSelfNotifier (게임 서버 나누기 2 단계)
#endif // USE_SKILL

// 스탯·경험치 보내기(SendMyStats · SendExp) 는 본인 알림 조각으로 옮겼다 — CSelfNotifier (게임 서버 나누기 2 단계)

#if USE_COMBAT
// ==========================================================================
// 지속 효과 — 걸기·만료·정리는 CBuffSystem(BuffSystem.cpp), 여기는 알림을 받아 본인 알림 조각에 넘기기만
// ==========================================================================

// 효과가 바뀌었다(걸림·되감김·만료·죽음) — 스탯은 이미 다시 선 뒤라 MY_STATS 가 새 공격력이다. 와이어 = ACTOR_BUFF → MY_STATS.
//   몬스터면 보낼 세션이 없어 둘 다 안 한다(스탯은 CBuffSystem 이 이미 세웠다 — 보스가 자기에게 건 버프가 이 길을 탄다. 다른 사람 화면엔 시전 방송의 고리만 순간 보이고 지속 표시는 아직 없다).
void CGameServer::OnBuffChanged(CCharacter* target, BuffKind kind)
{
    if (CPlayer* player = As<CPlayer>(target))
    {
        _selfNotifier.SendActorBuff(player, kind);
        _selfNotifier.SendMyStats(player);
    }
}

// 효과 칸 보내기(SendActorBuff · SendActiveBuffs) 는 본인 알림 조각으로 옮겼다 — CSelfNotifier (게임 서버 나누기 2 단계)
#endif // USE_COMBAT

// 부활 대기자 훑기 — 틱마다 1회. 죽은 것만 따로 들고 있어 존 전체를 돌지 않는다.
void CGameServer::TickRespawn()
{
    if (_deadActors.empty())
        return;

    // 마을로 옮길 사람 — 대기열을 도는 동안엔 옮기지 않는다. 옮기기(TransferPlayer → RemoveFromZone)가
    //   이 대기열을 지우므로(RemoveFromDeadActors) 도는 자리가 어긋난다. 담아 두고 다 돈 뒤 아래에서 옮긴다.
    _respawnTransferBuffer.clear();

    for (size_t i = 0; i < _deadActors.size(); )
    {
        CCharacter* dead = _deadActors[i];
        assert(dead->_kind == CharacterKind::PLAYER);   // 몬스터는 OnDeath 가 안 넣는다 — 되살리지 않는다

        if (!Combat::IsRespawnDue(*dead, _frameCount))
        {
            ++i;
            continue;
        }

        Combat::Respawn(*dead);

        // 부활도 "되살아난 쪽"으로 가른다 (죽음과 같은 기준 — GameMonitor.h 참조).
        //   이 대기열엔 플레이어만 있다 — 몬스터는 되살리지 않는다.
        _gameMonitor._combat._respawnsTotal.Inc();

        // 어디서 되살아나나 — 죽은 맵이 마을(기본 맵 = 맵 표 첫 줄)이 아니면 마을 시작 구역으로, 마을이면 제자리.
        //   지금은 마을에서 죽는 길이 없다(몬스터 구역 0 · 대인전은 던전에서만 일어난다) — 그 갈래는 예전 그대로 남겨 둔다.
        //   마을 채널은 여기서 고른다(자리 있는 채널 — 존 이동 요청과 같은 규칙). 못 얻으면 제자리로 — 되살리지 못한 채 두지 않는다
        CZone* zone = _mapManager.GetZone(dead->_zoneId);
        CPlayer* player = As<CPlayer>(dead);
        CZone* town = nullptr;
        if (zone != nullptr && player != nullptr && RespawnsToTown(zone->GetMapId()))
            town = _mapManager.FindOrCreateChannel(_defaultMapId, player->_isAdmin);

        if (town != nullptr)
        {
            _gameMonitor._combat._respawnToTown.Inc();   // 부활 자리 — 합 = _respawnsTotal(GameMonitor.h)
            _respawnTransferBuffer.emplace_back(player, town);
        }
        else
        {
            _gameMonitor._combat._respawnInPlace.Inc();
            if (zone != nullptr)
            {
                // 제자리 — 본인 포함. 자기 부활을 자기가 알아야 클라가 조작을 다시 연다
                _sightBroadcaster.BroadcastAroundSector(zone, dead,
                    MakeRespawn(dead->_actorId, dead->_x, dead->_y, dead->_hp), false);
            }
        }

        // 순서가 뜻을 갖지 않는 대기열이라 뒤엣것과 바꿔치기로 O(1) 제거.
        //   i를 올리지 않는다 — 방금 당겨온 것을 아직 안 봤다.
        _deadActors[i] = _deadActors.back();
        _deadActors.pop_back();
    }

    // 마을로 — 대기열을 다 돈 뒤(위 주석). 이미 대기열에서 빠졌으니 퇴장의 RemoveFromDeadActors 는 할 일이 없다.
    //   틱 가운데서 옮겨도 되는 까닭 — 이 자리(3-2)는 섹터 이동 대기열을 다 비운 뒤라 같은 틱 이동자 보정이 필요 없다(3-3 주석과 같다).
    //   와이어: 던전 이웃 = DELETE(퇴장) · 본인 = ZONE_INFO → ZONE_CHANGE_OK → RESPAWN → … · 마을 이웃 = CREATE(체력 가득)
    for (const auto& [player, town] : _respawnTransferBuffer)
    {
        CZone* oldZone = _mapManager.GetZone(player->_zoneId);
        assert(oldZone != nullptr && oldZone != town);   // 위에서 「존이 있고 마을이 아님」을 보고 담았다
        _placement.TransferPlayer(player, oldZone, town, CPlayerPlacement::ArrivalKind::RESPAWN, nullptr);
    }
    _respawnTransferBuffer.clear();
}

// 부활 대기열에서 빼기 — 퇴장 경로가 부른다.
//   _isDead로 미리 거르지 않는다. 플래그와 대기열이 어긋나는 경로가 나중에 생겨도
//   여기서 조용히 새지 않게 하려는 것이고, 대기열 길이는 "지금 죽어 있는 수"라 훑는 비용이 없다.
void CGameServer::RemoveFromDeadActors(CCharacter* player)
{
    if (_deadActors.empty())
        return;

    _deadActors.erase(
        std::remove(_deadActors.begin(), _deadActors.end(), player),
        _deadActors.end());
}
#endif // USE_COMBAT

// 파티 요청 여섯(RecvParty*) · 주변 9섹터에서 찾기(FindPlayerAround) · 파티 알림(IPartyEvents) 은 파티·방 조각으로 옮겼다
//   — CPartyHandler (게임 서버 나누기 7 단계)

#if USE_MONSTER
// ==========================================================================
// 몬스터 (USE_MONSTER)
//   몬스터 코드(깔기·판단·벽 앞 재판단·죽은 몬스터 지우기)는 존마다 든 CMonsterManager 에 있다(MonsterManager.h).
//   여기는 틱마다 그걸 부르고, 알림을 받아 송신·번호 발급만 한다. 죽은 몬스터를 존에서 빼고 지우라 부르는 것은 OnDeath 끝.
// ==========================================================================

MonsterContext CGameServer::MakeMonsterContext()
{
    return MonsterContext{ *this, _sightBroadcaster, _gameMonitor   // 걷기·멈춤(IZoneEvents)은 시야 조각이 받는다
#if USE_COMBAT
                         , _combatSystem
#endif
#if USE_SKILL
                         , _skillSystem
#endif
    };
}

// 게임 루프 1-1) — 존마다 관리자 Tick. 리젠 주기가 된 구역을 채우고(처음 깔기 포함), 몬스터마다 판단한다.
void CGameServer::TickMonsters()
{
    const MonsterContext ctx = MakeMonsterContext();
    _mapManager.ForEachZone([&](CZone* zone)
    {
        zone->GetMonsterManager().Tick(ctx, _frameCount);
    });
}

// ── 몬스터 알림과 번호 부탁 (IMonsterEvents) ── 패킷 조립·송신은 이 파일 것을 그대로 쓴다.

int32_t CGameServer::AllocActorId()
{
    return _sessionHub.AllocActorId();   // 단일 증가 카운터라 플레이어와 충돌하지 않는다
}

void CGameServer::OnMonsterSpawn(CZone* zone, CMonster* monster)
{
    // 표시 문자 — 클라가 사람 대신 몬스터 스프라이트를 고르는 패킷 값(Protocol.h DISPLAY_MONSTER_BASE).
    //   플레이어의 CalcDisplayChar 처럼 게임 서버가 넣는다 — 존 층(몬스터 생성자)은 이 값을 모른다(10-06).
    //   깔 때 존 입장 직후 이 콜백을 곧바로 부르므로, 아래 CREATE 보다 늦는 일이 없다.
    //   값은 몬스터 표(_def.display)에 있다 — 표가 읽을 때 몬스터 구간(DISPLAY_MONSTER_BASE~)인지 봤다.
    monster->_displayChar = monster->_def.display;

    // 이미 그 자리를 보고 있던 플레이어에게 CREATE(SPAWN) 가 간다 — 가득 찬 체력을 싣는다(ActorSnapshot).
    //   보는 사람이 없으면 아무에게도 안 가고, 나중에 걸어온 사람은 섹터 진입 때 받는다.
    _sightBroadcaster.BroadcastSpawn(zone, monster, SpawnReason::SPAWN);

    // 델타 동기화 기준점을 지금 자리로 — 방금 보낸 CREATE 가 이 좌표를 실었으니 「클라가 아는 자리」가 여기다.
    //   플레이어의 입장·존 이동 때와 같은 꼴(SpawnPlayer·RecvZoneChange). 존 층은 이 값을 모른다 — 10-06 몬스터 관리자에서 옮김.
    //   안 맞추면 옛 자리와의 차이로 보정 패킷(MOVE_SYNC)이 한 번 더 나갈 뿐, 위치가 틀리지는 않는다.
    monster->_lastSyncX = monster->_x;
    monster->_lastSyncY = monster->_y;
}

// 사라짐 — 방 클리어로 남은 몬스터를 지울 때 하나마다(CMonsterManager::VanishAll). 죽은 몬스터 지우기 앞(OnDeath 3·8)과 같은 두 일:
//   효과 목록에서 빼고(이 뒤에 관리자가 해제한다 — 목록에 주소가 남으면 안 된다) · 존에서 뺀다(주변 DELETE · 같은 틱 이동자 · 대기열 — 플레이어 퇴장과 같은 길).
//   죽음(DEATH)은 안 보낸다 — 죽은 게 아니라 사라진 것이다.
void CGameServer::OnMonsterVanish(CZone* zone, CMonster* monster)
{
#if USE_COMBAT
    _buffSystem.ClearAll(monster);
#endif
    _placement.RemoveFromZone(zone, monster);
#if USE_PARTY
    _gameMonitor._room._minionsCleared.Inc();   // 클리어는 방 갈래만 건다(OnDeath) — 사라짐은 곧 방 클리어로 지운 수다
#endif
}
#endif // USE_MONSTER

// 드랍 굴림(DropLoot) 은 처치 보상 조각으로 옮겼다 — CRewardHandler (게임 서버 나누기 8 단계)
// 땅 아이템 관리자에 넘길 묶음(MakeGroundItemContext) 은 가방·땅 조각으로 옮겼다 — CItemHandler (게임 서버 나누기 6 단계)

// 땅 아이템 한 사람에게 보이기·지우기(SendGroundItemCreate · SendGroundItemDelete) 는 시야 방송 조각으로 옮겼다 — CSightBroadcaster (게임 서버 나누기 3 단계)

// 땅 아이템 알림(IGroundItemEvents — 번호 발급 · 떨어짐 · 빠짐) · 수명 훑기(TickGroundItems) · 줍기(RecvPickup) 는 가방·땅 조각으로 옮겼다
//   — CItemHandler (게임 서버 나누기 6 단계)

// 존 이동 성공·실패 보내기(SendZoneChangeOk · SendZoneChangeFail) 는 배치 조각으로 옮겼다 — CPlayerPlacement (게임 서버 나누기 5 단계)

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

// 같은 틱 이동자에게 땅 아이템 DELETE(SendGroundItemDeleteToLateMovers) 는 시야 방송 조각으로 옮겼다 — CSightBroadcaster (게임 서버 나누기 3 단계)

// 도착 차례(NotifyArrival — 존 정보 → 내 캐릭터 → 본인 상태 → 주변과 서로 생성) 는 배치 조각으로 옮겼다 — CPlayerPlacement (게임 서버 나누기 5 단계)

// 존 입장·등장·퇴장 방송(BroadcastEnterZone · BroadcastSpawn · BroadcastLeaveZone) 은 시야 방송 조각으로 옮겼다 — CSightBroadcaster (게임 서버 나누기 3 단계)

// 존에서 빼기(RemoveFromZone) 는 배치 조각으로 옮겼다 — CPlayerPlacement (게임 서버 나누기 5 단계)

// ==========================================================================
// 같은 틱 이동자 쌍 보정 · 섹터 변경 방송 · 멤버십 팬아웃·직송 · 섹터 묶음 · 수신섹터 digest 는 시야 방송 조각으로 옮겼다
//   — CSightBroadcaster (SightBroadcaster.cpp · 게임 서버 나누기 3 단계)
// ==========================================================================
