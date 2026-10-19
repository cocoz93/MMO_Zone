// ==========================================================================
// CCharacterStore — 캐릭터 저장소: DB 워커를 들고 불러오기·저장을 맡는다 (게임 서버 나누기 4 단계 · B3 · USE_DB_WORKER)
//
// [책임]
//  - DB 워커(CDBWorker)의 주인 — 띄우기(Init) · 멈추기(Shutdown: 전원 최종 저장 → 드레인 → 종료)
//  - 저장 세 길: 주기(TickPeriodicSave — 저장 대상만) · 즉시(EnqueuePlayerSave — 나갈 때·레벨업) · 종료(전원)
//    저장 대상 판정과 「마지막으로 실은 값」 사본(_dbSaved)도 여기서 다룬다
//  - 불러오기 요청 · 회신 꺼내기(ProcessDbResults) — 결과는 알림(ICharacterLoadEvents)으로 넘긴다(누가 어떻게 들여보낼지는 모른다)
//  - 불러온 행을 캐릭터 칸으로 — 쿨타임·버프 묶음 풀기(실제 시각 → 틱) · 가방 줄 넣기(검사하고 안 맞는 줄은 세고 건너뜀)
//  - 가방 칸 하나 즉시 저장 요청(EnqueueItemSlot — 저장 플랜 C 가 없앨 길)
//
// [경계]
//  게임 중인 플레이어를 도는 것은 세션 허브(CSessionHub::ForEachPlayer)로 한다. 지속 효과 칸 채우기는 효과 규칙(CBuffSystem::Restore)에 맡긴다.
//  틱 번호는 빌려서 읽기만 한다 — 「틱 ↔ 실제 시각」 환산의 「지금」이다.
//
// [모르는 것]
//  입장 절차(대기표 · 어느 존에 들여보낼지 · 스폰) · 패킷. 불러오기 결과를 받는 쪽은 알림 통로 뒤에 있다(지금은 CGameServer).
//  존·세션은 값만 다룬다 — 존 번호를 맵 번호로 바꾸기(CMapManager::GetMapIdFromZoneId)만 빌려 쓰고, 세션 번호는 불러오기 열쇠로 실어 나르기만 한다.
// ==========================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "BuildConfig.h"   // USE_DB_WORKER · USE_COMBAT · USE_INVENTORY — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include "DB/DBWorker.h"   // CDBWorker · DBConfig · DBSaveJob · DBLoadJob · DBResult · DBItemSlotJob

#if USE_DB_WORKER

class CPlayer;
class CSessionHub;
class CGameMonitor;
class CGameDataFiles;
#if USE_COMBAT
class CBuffSystem;
#endif

// 불러오기 결과를 넘기는 통로 — 받는 쪽이 판정(찾음·없음·실패)하고 들여보낸다(게임 서버 나누기 0-③ ⑷ — 저장소가 입장 절차를 모르게).
//   지금은 CGameServer 가 구현한다(입장 조각 C1 을 떼면 그쪽으로). 시스템 알림(ICombatEvents 등)과 같은 꼴 — 순수 가상이라 빠뜨리면 컴파일이 안 된다.
class ICharacterLoadEvents
{
public:
    virtual ~ICharacterLoadEvents() = default;

    // 불러오기 결과 1건 처리 — 판정 후 스폰. 회신 큐에서 꺼낸 차례대로 온다.
    //   result 는 이 부름 동안만 살아 있다(회신 큐 원소 — 들고 있으면 안 된다). nowNs = 꺼낸 순간(지연 계측의 끝).
    virtual void OnCharacterLoaded(const DBResult& result, int64_t nowNs) = 0;
};

class CCharacterStore
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(세션 허브 · 게임 지표 · 기획 데이터 · 효과 규칙 · 틱 번호)보다 뒤에
    //   이것을 선언해야 한다(멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    CCharacterStore(CSessionHub& sessionHub, CGameMonitor& gameMonitor, const CGameDataFiles& gameDataFiles,
#if USE_COMBAT
                    CBuffSystem& buffSystem,
#endif
                    const uint64_t& frameCount, ICharacterLoadEvents& loadEvents)
        : _sessionHub(sessionHub), _gameMonitor(gameMonitor), _gameDataFiles(gameDataFiles)
#if USE_COMBAT
        , _buffSystem(buffSystem)
#endif
        , _frameCount(frameCount), _loadEvents(loadEvents) {}

    // DB 저장 파이프라인 초기화. Start() 전에 호출(CGameServer::InitDB 가 넘긴다). 세 값은 1 이상 — 아니면 false(기동 거절).
    bool Init(const DBConfig& dbConfig, int savePeriodSec, int workerCount, int queueMax);
    // 게임 스레드 종료 후(단일 스레드 시점) 전원 최종 저장 → 드레인 → 워커 종료. 워커가 없으면 아무 일도 안 한다.
    void Shutdown();
    // 워커가 떠 있나 — 띄우기 전·실패·멈춘 뒤는 false. 저장·불러오기를 부르는 쪽이 먼저 본다.
    bool IsRunning() const { return _dbWorker != nullptr; }

    // DB 저장 파이프라인 (dirty flag 기반 비동기 위치 저장)
    void EnqueuePlayerSave(CPlayer* player);   // 단건: 스냅샷 enqueue + dirty clear (나갈 때·레벨업)
    void TickPeriodicSave();                   // 주기: 저장 대상 배치를 1회 핸드오프

#if USE_INVENTORY
    // 가방 칸 하나 저장 요청 — 받으면 true(셈은 부르는 쪽 · CItemHandler::FlushInventory). 저장 플랜 C 가 한 행 저장으로 합칠 길이다.
    bool EnqueueItemSlot(const DBItemSlotJob& job) { return _dbWorker->EnqueueItemSlot(job); }

    // 불러온 가방 줄을 검사해 넣는다 — 칸 범위·아이템 표·탭·쌓임 한도. 안 맞는 줄은 세고 건너뛴다(깎아 넣지 않는다).
    //   바뀐 칸 표시를 안 켠다(DB 와 같은 값) — 건너뛴 줄은 DB 에 그대로 남고, 그 칸에 새 아이템이 들어오면 덮인다.
    void LoadInventoryRows(CPlayer* player, const DBResult& result);
#endif

    // 입장 — 묶음을 풀어 실제 시각 → 틱으로 칸을 채운다(SpawnPlayer).
    //   cooldownsAndBuffs = 불러온 행(찾음 갈래에서만 — nullptr 이면 되살릴 것 없음) · dropBuffs = 죽은 채 저장된 행이라 버프는 버린다
    //   (전에는 입장 배치 구조체(CGameServer::EnterPlacement)를 통째로 받았다 — 저장소가 입장 구조체를 모르게 쓰는 두 칸만 받는다, 4 단계).
    void RestoreCooldownsAndBuffs(CPlayer* player, const DBResult* cooldownsAndBuffs, bool dropBuffs);

    // DB 왕복 — 캐릭터 불러오기 (요청은 워커 큐로, 결과는 회신 큐로 돌아와 다음 틱에 꺼낸다)
    bool RequestCharacterLoad(int64_t sessionId, int64_t accountId);   // 조회 1건 투입 + 계수. false = 거절됨(결과가 안 온다)
    void ProcessDbResults();                                           // 틱마다: 회신 큐를 통째로 꺼내 종류별로 넘긴다(불러오기 → _loadEvents)

private:
    bool ShouldSave(const CPlayer* player) const;        // 저장 대상 판정(MOVING||dirty||레벨·경험치·체력·마력·쿨타임·버프 칸이 _dbSaved 와 다름) — 주기 저장
    DBSaveJob MakeSaveJob(const CPlayer* player) const;  // 위치·레벨·경험치·체력·마력·쿨타임·버프 스냅샷 생성 (단건·배치 공용)
    static void MarkSaveQueued(CPlayer* player, const DBSaveJob& job);   // 큐에 들어간 잡 기록 — dirty clear + _dbSaved 갱신 (세 저장 길 공통)
    void SaveAllPlayers();                     // 종료: 전원 최종 저장(안전망, dirty 무관)

    // 쿨타임·버프 ↔ DB 묶음(characters.skill_cooldowns · buffs, 형식은 CooldownBuffBlob.h). 게임 안의 시간은 틱이고 DB 는 실제 시각이다.
    void PackCooldownsAndBuffs(const CPlayer* player, DBSaveJob& job) const;      // 저장 — 돌고 있는 칸을 틱 → 실제 시각으로 묶는다(MakeSaveJob)
    static bool CooldownsAndBuffsChanged(const CPlayer* player);                  // 주기 판정 — 칸이 마지막 저장 때 사본(_dbSaved)과 다른가(틱끼리)
    static void RecordSavedCooldownsAndBuffs(CPlayer* player);                    // 지금 칸을 _dbSaved 사본으로(MarkSaveQueued · 입장)

    CSessionHub&          _sessionHub;      // 게임 중인 플레이어 돌기(주기·종료 저장)
    CGameMonitor&         _gameMonitor;     // 입장(불러오기·되살리기) · 가방 불러오기 · DB 지표 그릇
    const CGameDataFiles& _gameDataFiles;   // 스킬 표(쿨타임 되살리기) · 아이템 표(가방 줄 검사)
#if USE_COMBAT
    CBuffSystem&          _buffSystem;      // 지속 효과 칸 되살리기(Restore — 칸·지표·목록은 그쪽이 맡는다)
#endif
    const uint64_t&       _frameCount;      // 게임 루프의 틱 번호 — 빌린 것, 읽기만(틱 ↔ 실제 시각 환산의 「지금」)
    ICharacterLoadEvents& _loadEvents;      // 불러오기 결과를 받는 쪽

    // DB 저장 파이프라인 (dirty flag 기반 비동기 위치 저장)
    std::unique_ptr<CDBWorker> _dbWorker;
    int _dbSavePeriodFrames = 0;     // SavePeriodSec × FRAME_PER_SEC (InitDB에서 설정)
    int _dbSaveFrameCounter  = 0;

    // 회신 큐에서 통째로 꺼내 받는 로컬 버퍼 (멤버 재사용 — 워커 쪽 벡터와 맞바꾸므로 양쪽 용량이 보존된다.
    //   _localEvents 와 같은 방식). 접근은 게임 스레드 단독.
    std::vector<DBResult> _dbResults;
};

#endif // USE_DB_WORKER
