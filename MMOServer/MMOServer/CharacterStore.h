// ==========================================================================
// CCharacterStore — 캐릭터 저장소: DB 워커를 들고 불러오기·저장을 맡는다 (게임 서버 나누기 4 단계 · B3 · USE_DB_WORKER)
//
// [책임]
//  - DB 워커(CDBWorker)의 주인 — 띄우기(Init) · 멈추기(Shutdown: 전원 최종 저장 → 드레인 → 종료)
//  - 저장 — 바뀐 자리는 표시만 하고, 계기 넷(주기 · 틱 끝 요청 · 나갈 때 · 종료)이 묶음 하나(캐릭터 행 + 저장 안 된 칸)를 만든다.
//    표시는 DB 확정 회신 때만 지운다(실을 때 비행 기록으로 옮기고 실패면 되살림) · 캐릭터당 비행 중 묶음은 하나 ·
//    나간 사람의 묶음은 떠난 기록으로 들고 확정될 때까지 다시 싣는다(저장 플랜 C — save-unify-plan.md)
//    저장 대상 판정과 「마지막으로 실은 값」 사본(_dbSaved)도 여기서 다룬다
//  - 불러오기 요청 · 회신 꺼내기(ProcessDbResults) — 저장 회신을 먼저 처리하고, 불러오기 결과는 알림(ICharacterLoadEvents)으로 넘긴다
//    (누가 어떻게 들여보낼지는 모른다 · 그 계정의 떠난 기록이 확정 전이면 실패로 넘긴다)
//  - 불러온 행을 캐릭터 칸으로 — 쿨타임·버프 묶음 풀기(실제 시각 → 틱) · 가방 줄 넣기(검사하고 안 맞는 줄은 세고 건너뜀)
//
// [경계]
//  게임 중인 플레이어를 도는 것은 세션 허브(CSessionHub::ForEachPlayer)로 한다. 지속 효과 칸 채우기는 효과 규칙(CBuffSystem::Restore)에 맡긴다.
//  틱 번호는 빌려서 읽기만 한다 — 「틱 ↔ 실제 시각」 환산의 「지금」이다.
//
// [모르는 것]
//  입장 절차(대기표 · 어느 존에 들여보낼지 · 스폰) · 패킷. 불러오기 결과를 받는 쪽은 알림 통로 뒤에 있다(입장 조각 CEntryHandler — 게임 서버 나누기 12 단계).
//  존·세션은 값만 다룬다 — 존 번호를 맵 번호로 바꾸기(CMapManager::GetMapIdFromZoneId)만 빌려 쓰고, 세션 번호는 불러오기 열쇠로 실어 나르기만 한다.
// ==========================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "BuildConfig.h"   // USE_DB_WORKER · USE_COMBAT · USE_INVENTORY — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include "DB/DBWorker.h"   // CDBWorker · DBConfig · DBCharRow · DBSaveBundle · DBSaveReply · DBInvRow · DBLoadJob · DBResult

#if USE_DB_WORKER

class CPlayer;
class CSessionHub;
class CGameMonitor;
class CGameDataFiles;
enum class SaveReason : uint8_t;    // 틱 끝 요청의 사유 · 묶음의 계기 — 정의는 지표 라벨과 같이 GameMonitor.h
enum class SaveTrigger : uint8_t;
#if USE_COMBAT
class CBuffSystem;
#endif

// 불러오기 결과를 넘기는 통로 — 받는 쪽이 판정(찾음·없음·실패)하고 들여보낸다(게임 서버 나누기 0-③ ⑷ — 저장소가 입장 절차를 모르게).
//   입장 조각(CEntryHandler)이 구현한다(게임 서버 나누기 12 단계 — 전에는 CGameServer). 시스템 알림(ICombatEvents 등)과 같은 꼴 — 순수 가상이라 빠뜨리면 컴파일이 안 된다.
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

    // ── 저장 — 바뀐 자리는 표시만, 계기 넷이 묶음 하나(캐릭터 행 + 저장 안 된 칸)를 만든다(save-unify-plan.md) ──
    //   틱 끝 요청 — 목록에 올리기만 한다(세션 번호로 · 같은 틱 중복은 한 번). 같은 틱 끝 TickSaves 가 판정해 묶음을 넣는다.
    void RequestSave(CPlayer* player, SaveReason reason);
    //   틱 끝 — 주기(그 틱이면 판정 참인 전원 · 실패한 떠난 기록 다시 싣기) → 목록. GameServer 틱의 마지막 일.
    void TickSaves();
    //   나갈 때 — 마지막 묶음(행 + 표시 칸 ∪ 비행 칸)을 떠난 기록으로 바로 큐에. 부른 뒤 플레이어가 해제된다(ReleasePlayer).
    //   ★ 짝 고리가 걸려 있으면 떠난 기록을 붙잡는다(큐에 안 넣음 — 아래 「짝 저장」).
    void SaveOnLeave(CPlayer* player);

    // ── 짝 저장 (거래 · trade-plan.md 「저장 — 왜 붙잡나」) ──
    //   두 사람의 맞바꾼 가방은 짝 묶음 하나 = 트랜잭션 하나로 쓴다(따로 쓰다 그 사이 죽으면 복사·증발).
    //   짝 묶음은 한 큐(두 계정 중 작은 번호의 워커)에만 들어가 다른 쪽 사람의 순서를 큐가 못 지킨다 — 그래서 「한 캐릭터의 DB 쓰기는
    //   늘 하나만 떠 있다」를 짝에도 지킨다: 짝 고리가 걸린 동안 두 사람의 혼자 묶음(주기 · 틱 끝)도, 떠난 기록도 안 낸다(붙잡음).
    //   성공 회신 = 고리를 풀고 붙잡아 둔 것을 낸다 · 실패 회신 = 다음 주기에 짝째로 다시(혼자씩 따로 쓰는 길이 없다).
    //   종료 = 고리가 걸린 짝은 두 사람의 최종을 짝 묶음으로 같은 큐에(떠 있던 짝 묶음 뒤에 줄 선다).
    //   맞바꾸기 직전에 묻는다 — 둘 다 떠 있는 묶음이 없고 고리도 없나(워커가 없으면 늘 참). 거짓이면 부르는 쪽이 다음 틱으로 미룬다.
    bool CanSaveTradeNow(const CPlayer* a, const CPlayer* b) const;
    //   맞바꾼 바로 뒤(같은 틱) — 고리를 걸고 짝 묶음을 낸다. 큐가 거절해도 고리는 남아 다음 주기에 다시 낸다. 워커가 없으면 아무 일도 안 한다.
    void SaveTrade(CPlayer* a, CPlayer* b);

#if USE_INVENTORY
    // 불러온 가방 줄을 검사해 넣는다 — 칸 범위·아이템 표·탭·쌓임 한도. 안 맞는 줄은 세고 건너뛴다(깎아 넣지 않는다).
    //   바뀐 칸 표시를 안 켠다(DB 와 같은 값) — 건너뛴 줄은 DB 에 그대로 남고, 그 칸에 새 아이템이 들어오면 덮인다.
    void LoadInventoryRows(CPlayer* player, const DBResult& result);
#endif

    // 입장 — 묶음을 풀어 실제 시각 → 틱으로 칸을 채운다(SpawnPlayer).
    //   cooldownsAndBuffs = 불러온 행(찾음 갈래에서만 — nullptr 이면 되살릴 것 없음) · dropBuffs = 죽은 채 저장된 행이라 버프는 버린다
    //   (전에는 입장 배치 구조체(지금 CEntryHandler::EnterPlacement)를 통째로 받았다 — 저장소가 입장 구조체를 모르게 쓰는 두 칸만 받는다, 4 단계).
    void RestoreCooldownsAndBuffs(CPlayer* player, const DBResult* cooldownsAndBuffs, bool dropBuffs);

    // DB 왕복 — 캐릭터 불러오기 (요청은 워커 큐로, 결과는 회신 큐로 돌아와 다음 틱에 꺼낸다)
    bool RequestCharacterLoad(int64_t sessionId, int64_t accountId);   // 조회 1건 투입 + 계수. false = 거절됨(결과가 안 온다)
    void ProcessDbResults();                                           // 틱마다: 회신 큐를 통째로 꺼내 종류별로 넘긴다(불러오기 → _loadEvents)

private:
    bool ShouldSave(const CPlayer* player) const;        // 저장 대상 판정(MOVING||dirty||레벨·경험치·체력·마력·쿨타임·버프 칸이 _dbSaved 와 다름||저장 안 된 가방 칸) — 주기·틱 끝
    DBCharRow MakeCharRow(const CPlayer* player) const;  // 캐릭터 행 값 — 위치·레벨·경험치·체력·마력·쿨타임·버프·돈 스냅샷(묶음 머리 · 판정 공용)
    static void RecordSaved(CPlayer* player, const DBCharRow& row);      // 「마지막으로 실은 값」 사본(_dbSaved) 갱신 — 묶음을 만들 때
    void SaveAllPlayers();                     // 종료: 전원 최종 저장(안전망, dirty 무관) — 떠난 기록으로

    // ── 저장 묶음 · 비행 기록 · 떠난 기록 ──
    //   묶음 하나를 만든다 — 표시를 비행 기록으로 옮기고 머리·칸 줄을 채운다(넣기 전). 넣기가 거절되면 AbortBundle 이 되돌린다.
    void PrepareBundle(CPlayer* player, DBSaveBundle& head, std::vector<DBInvRow>& rows);
    void AbortBundle(CPlayer* player);                                   // 넣기 거절 — 비행 기록을 표시로 되돌림
    void CommitBundle(CPlayer* player, const DBSaveBundle& head, SaveTrigger trigger);   // 넣기 받음 — 비행 중으로
    bool SendBundle(CPlayer* player, SaveTrigger trigger);               // 주기·틱 끝 하나 — 비행 중이면 다시 필요만. 반환 = 큐가 받았나
    void OnSaveReply(const DBSaveReply& reply);                          // 회신 하나 — 살아 있는 사람 · 떠난 기록 · 주인 없음
    void FillFinalRows(CPlayer* player, std::vector<DBInvRow>& rows);    // 나갈 때·종료 — 표시 칸 ∪ 비행 칸(최종값)
    void LeaveFinal(CPlayer* player, SaveTrigger trigger);                // 나갈 때·종료 — 마지막 묶음을 떠난 기록으로(짝이면 붙잡음)
    void SetInflight(int delta);                                          // 비행 중 게이지

    // 떠난 기록 — 나간 사람의 마지막 묶음. 플레이어가 해제되므로 저장소가 계정 열쇠로 든다(계정당 하나).
    //   비행(큐에 있음) / 실패(다음 주기 · 같은 계정 불러오기 앞에 다시 싣는다). 성공 회신에 지운다.
    struct LeaveRecord
    {
        DBSaveBundle          head{};
        std::vector<DBInvRow> rows;
        bool                  inFlight = false;
        bool                  paired   = false;   // 짝 고리에 걸려 붙잡힘 — 혼자 안 낸다(짝 묶음의 한쪽으로만 · 고리가 풀리면 혼자)
    };
    void PutLeaveRecord(DBSaveBundle head, std::vector<DBInvRow>&& rows, SaveTrigger trigger, bool hold = false);
    void SendLeaveRecord(LeaveRecord& rec, SaveTrigger trigger);

    // 짝 고리 — 거래 한 건의 확정 안 된 저장. 쪽마다 살아 있는 사람(세션) 또는 떠난 기록(계정 열쇠)이다.
    struct PairLink
    {
        int64_t  account[2]  = {};
        int64_t  session[2]  = {};      // 살아 있으면 세션 번호 · 0 = 나갔다(그쪽 최종 = 붙잡힌 떠난 기록)
        uint32_t bundleNo[2] = {};      // 떠 있는 짝 묶음의 머리 번호 — 회신 짝 맞추기(inFlight 일 때만 뜻이 있다)
        bool     replied[2]  = {};
        bool     ok          = true;    // 받은 회신이 다 성공인가(트랜잭션 하나라 둘은 같다)
        bool     inFlight    = false;
    };
    PairLink* FindPair(int64_t accountId, int* outSide = nullptr);
    bool SendPair(PairLink& link, SaveTrigger trigger);   // 짝 묶음 하나를 낸다 — 반환 = 큐가 받았나
    void OnPairReply(const DBSaveReply& reply);           // 회신이 떠 있는 짝 묶음의 머리면 고리에 적고, 둘 다 오면 푼다(ProcessDbResults — OnSaveReply 바로 뒤)
    void ReleasePairSide(int64_t accountId, int64_t sessionId);   // 고리가 풀린 한쪽 — 혼자 묶음·붙잡힌 떠난 기록을 다시 흐르게

    // 쿨타임·버프 ↔ DB 묶음(characters.skill_cooldowns · buffs, 형식은 CooldownBuffBlob.h). 게임 안의 시간은 틱이고 DB 는 실제 시각이다.
    void PackCooldownsAndBuffs(const CPlayer* player, DBCharRow& row) const;      // 저장 — 돌고 있는 칸을 틱 → 실제 시각으로 묶는다(MakeCharRow)
    static bool CooldownsAndBuffsChanged(const CPlayer* player);                  // 주기 판정 — 칸이 마지막 저장 때 사본(_dbSaved)과 다른가(틱끼리)
    static void RecordSavedCooldownsAndBuffs(CPlayer* player);                    // 지금 칸을 _dbSaved 사본으로(RecordSaved · 입장)

    CSessionHub&          _sessionHub;      // 게임 중인 플레이어 돌기(주기·종료 저장)
    CGameMonitor&         _gameMonitor;     // 입장(불러오기·되살리기) · 가방 불러오기 · DB 지표 그릇
    const CGameDataFiles& _gameDataFiles;   // 스킬 표(쿨타임 되살리기) · 아이템 표(가방 줄 검사)
#if USE_COMBAT
    CBuffSystem&          _buffSystem;      // 지속 효과 칸 되살리기(Restore — 칸·지표·목록은 그쪽이 맡는다)
#endif
    const uint64_t&       _frameCount;      // 게임 루프의 틱 번호 — 빌린 것, 읽기만(틱 ↔ 실제 시각 환산의 「지금」)
    ICharacterLoadEvents& _loadEvents;      // 불러오기 결과를 받는 쪽

    // DB 저장 파이프라인 — 워커(실행기)와 주기 계수
    std::unique_ptr<CDBWorker> _dbWorker;
    int _dbSavePeriodFrames = 0;     // SavePeriodSec × FRAME_PER_SEC (InitDB에서 설정)
    int _dbSaveFrameCounter  = 0;

    // 회신 큐에서 통째로 꺼내 받는 로컬 버퍼 (멤버 재사용 — 워커 쪽 벡터와 맞바꾸므로 양쪽 용량이 보존된다.
    //   _localEvents 와 같은 방식). 접근은 게임 스레드 단독.
    std::vector<DBResult> _dbResults;
    std::vector<DBSaveReply> _dbSaveReplies;   // 저장 회신 — 위와 같은 맞바꿈 버퍼(한 락에 같이 꺼낸다)

    // 저장 묶음 — 접근은 게임 스레드 단독(종료 때는 게임 스레드 join 뒤 메인 스레드 단독)
    std::vector<int64_t>                       _tickEndSessions;   // 이번 틱 끝 목록(세션 번호 — 같은 틱에 나간 사람은 틱 끝에 못 찾아 건너뜀)
    std::unordered_map<int64_t, LeaveRecord>   _leaveRecords;      // 계정 → 떠난 기록
    uint32_t                                   _leaveSeq = 0;      // 떠난 기록 묶음 번호(실을 때마다 새로 — 늦게 온 옛 회신이 새 시도를 확정하지 않게)
    int64_t                                    _inflightCount = 0; // 비행 중 묶음이 있는 살아 있는 캐릭터 수(게이지의 원본)
    std::vector<DBSaveBundle>                  _batchHeads;        // 주기·종료 묶음 머리 — 멤버 재사용(매 주기 할당 회피)
    std::vector<DBInvRow>                      _batchRows;         // 그 칸 줄(머리의 slotBegin 이 가리킨다)
    std::vector<CPlayer*>                      _batchOwners;       // _batchHeads 와 나란한 주인(거절 때 되돌릴 사람)
    std::vector<size_t>                        _batchRefused;      // 거절된 색인
    std::vector<PairLink>                      _pairs;             // 걸린 짝 고리 — 많아야 동시에 확정을 기다리는 거래 수
    std::vector<DBInvRow>                      _pairRows[2];       // 짝 묶음 쪽마다 칸 줄 — 멤버 재사용(SendPair)
};

#endif // USE_DB_WORKER
