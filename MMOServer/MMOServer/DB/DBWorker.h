#pragma once

// ==========================================================================
// CDBWorker — dirty flag 기반 비동기 위치 저장 (1단계: 저장 파이프라인)
//
// [모델] 서버 메모리 = 진실, DB = 저장소.
//        게임루프가 "바뀐 플레이어"의 값 스냅샷만 큐에 넣으면,
//        전용 워커 스레드 1개가 MySQL에 UPSERT 한다. 게임 틱은 막지 않는다.
//
// [설계] 송신 워커(CIOCPServer::SendWorker) 패턴을 그대로 복제:
//        게임스레드 push(lock+notify) / 워커스레드 swap-out 배치 처리.
//        잡은 값 타입 스냅샷이라 플레이어 삭제 후 실행돼도 안전(수명 무관).
//
// [왕복] 저장은 던지고 끝(편도)이지만 불러오기는 답이 돌아와야 한다(왕복).
//        가는 길은 저장과 "같은 큐"를 쓴다 — 같은 계정은 같은 워커를 타고 큐는 앞에서부터
//        실행되므로 "옛 접속의 마지막 저장 → 새 접속의 불러오기" 순서가 공짜로 지켜진다.
//        오는 길은 회신 큐 하나다 — 워커가 넣고, 게임 루프가 틱마다 통째로 꺼낸다.
// ==========================================================================

#include "BuildConfig.h"

#include <cstdint>
#include <string>

// DB 접속 설정 — ServerConfig가 INI [DB] 섹션에서 채워 넘긴다.
// (USE_DB_WORKER 토글과 무관하게 정의 — GameServer::InitDB가 값으로 참조)
struct DBConfig
{
    std::string host              = "127.0.0.1";
    int         port              = 3306;
    std::string user              = "root";
    std::string password;
    std::string database          = "gamedb";
    int         connectTimeoutSec = 3;
    int         rwTimeoutSec       = 5;   // 읽기/쓰기 소켓 타임아웃(초) — DB 무응답 시 쿼리 무한대기 방지
};

#if USE_DB_WORKER

#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <memory>
#include <cstddef>

class CMonitorManager;

// 저장 잡 = 값 스냅샷 (포인터 없음 → 플레이어가 사라져도 안전)
//   ※ level·exp 는 USE_EXP 로 안 가른다 — 열은 테이블에 늘 있고, 숫자 둘을 싣는 값이
//     "토글마다 문장이 갈리는" 값보다 훨씬 싸다. 토글이 가르는 것은 "값이 바뀌는 코드" 쪽이다.
//   ※ DB 열과의 대응은 DBWorker.cpp 의 열 표(kCharColumns) 한 곳에만 있다 — 열을 늘리면
//     여기와 DBResult 에 같은 이름의 멤버를 넣고 표에 한 줄. SQL·바인딩·복사는 표에서 나온다.
struct DBSaveJob
{
    int64_t accountId;
    float   x;
    float   y;
    int32_t mapId;
    int32_t level;
    int64_t exp;
};

// 불러오기 잡 — 계정 번호로 캐릭터 행을 읽는다. 저장과 달리 결과가 회신 큐로 돌아온다.
struct DBLoadJob
{
    int64_t accountId;
    int64_t sessionId;   // 결과의 주인. 게임 스레드가 세션 번호 "전체"로 대조한다(자리 재사용 방어)
    int64_t requestNs;   // 요청 시각(steady_clock, ns) — 요청→결과 수령 지연을 재는 기준
};

#if USE_INVENTORY
// 가방 한 칸 저장 잡 (USE_INVENTORY) — 칸이 바뀐 "그 순간" 칸의 최종값을 싣는다(60초 주기와 따로).
//   증감이 아니라 최종값이라 같은 잡을 두 번 실행해도 결과가 같다 → 실행 실패 때 재연결 후 다시 써도 안전하다.
//   ★ 위치 저장과 달리 버리지 않는다 — 저장 상한을 건너뛰고 따로 센다(EnqueueItemSlot).
//     위치는 다음 주기가 덮지만, 아이템은 버리면 되돌릴 길이 없다.
struct DBItemSlotJob
{
    int64_t accountId;
    int32_t slot;        // 칸 번호 (Protocol.h INV_TAB_RANGES)
    int32_t itemId;      // 0 = 빈 칸 — 행을 지우지 않고 0 으로 덮는다(DB 계정에 DELETE 권한이 없다)
    int32_t count;
};

// 불러온 가방 한 줄 — DB 값 그대로(검사는 게임 스레드가 한다: 칸 범위·아이템 표·탭·쌓임 한도).
//   칸 번호를 int32 로 받는 이유 — 행이 망가져 있으면 음수나 큰 값일 수 있다. 줄이기 전에 걸러야 한다.
//   멤버 이름은 DBItemSlotJob 과 같아야 한다 — 열 표(DBWorker.cpp kInvColumns)가 이름으로 잇는다.
struct DBInvRow
{
    int32_t slot;
    int32_t itemId;
    int32_t count;
};

// 결과에 실을 수 있는 가방 줄 수 — 칸 60 의 두 배. 망가진 행이 섞여 있어도 멀쩡한 행을 잃지 않게 넉넉히 둔다.
//   넘는 줄은 싣지 않고 수만 센다(DBResult::invOverflow → 게임 쪽 "too_many_rows" 계수기).
constexpr int DB_INV_MAX_ROWS = 120;

// 아이템 저장 지표 — 워커 스레드가 센다. 정의는 게임 쪽(GameMonitor.h)이고 여기선 가리키기만 한다.
//   코어(CMonitorManager)에 안 넣는 이유 — 넣으면 별도 저장소(ServerCore)를 고치게 된다.
struct DBItemSaveCounters;
#endif

// 저장 파이프라인 지표(저장 성공·실패·드롭·큐 깊이·워커 수) — 위와 같은 쪽(GameMonitor.h), 생성자로 참조를 받는다.
//   10-06 코어 CMonitorManager::_db* 에서 옮김 — DB 워커는 게임 프로젝트 구성요소라 코어 그릇을 안 본다.
struct DBSaveCounters;

// 작업 종류 — 큐 원소에 붙는 표시. 워커와 게임 루프가 switch 로 가른다.
//   ※ 종류를 늘리면 COUNT 가 바뀌고, 각 switch 옆의 static_assert 가 컴파일을 막아
//     빠뜨린 갈래를 찾아가게 한다. (이 프로젝트 빌드 옵션엔 -Wswitch·C4062 가 꺼져 있어
//     컴파일러 경고로는 못 잡는다 — 그래서 경고 대신 단언에 맡긴다.)
enum class DBJobKind : uint8_t
{
    SAVE_POSITION,     // 위치 UPSERT — 편도. 손실 허용(넘치면 버리고 다음 주기가 덮는다)
    LOAD_CHARACTER,    // 캐릭터 행 SELECT — 왕복. 버리지 않는다(버리면 그 사람이 못 들어온다)
#if USE_INVENTORY
    SAVE_ITEM_SLOT,    // 가방 칸 UPSERT — 편도지만 버리지 않는다(버리면 아이템이 사라진다)
#endif
    COUNT
};
constexpr int DB_JOB_KIND_COUNT = static_cast<int>(DBJobKind::COUNT);

// 큐 원소 = 종류 + 몸. 몸은 둘 다 값 타입(POD)이라 union 으로 겹친다 — 수명 무관, 복사는 memcpy.
//   DBSaveJob 을 그대로 담으므로 게임 쪽 저장 호출부(MakeSaveJob·EnqueueBatch)는 바뀌지 않는다.
struct DBJob
{
    DBJobKind kind;
    union
    {
        DBSaveJob save;
        DBLoadJob load;
#if USE_INVENTORY
        DBItemSlotJob item;
#endif
    };

    static DBJob Save(const DBSaveJob& s) { DBJob j; j.kind = DBJobKind::SAVE_POSITION;  j.save = s; return j; }
    static DBJob Load(const DBLoadJob& l) { DBJob j; j.kind = DBJobKind::LOAD_CHARACTER; j.load = l; return j; }
#if USE_INVENTORY
    static DBJob Item(const DBItemSlotJob& i) { DBJob j; j.kind = DBJobKind::SAVE_ITEM_SLOT; j.item = i; return j; }
#endif
};

// 불러오기 결과 — 셋을 가른다.
//   ★ NOT_FOUND 와 FAILED 를 절대 섞지 않는다. 실패를 "없음"으로 받으면 새 캐릭터로 스폰되고,
//     다음 저장(UPSERT)이 진짜 행을 덮어쓴다. 0("없었다")과 "못 읽었다"는 다른 사실이다.
enum class DBResultCode : uint8_t
{
    FOUND,        // 행이 있다 — x·y·mapId 유효
    NOT_FOUND,    // 행이 없다 — 새 캐릭터(정상)
    FAILED,       // 조회 실패(커넥션 없음·쿼리 오류) — 들여보내면 안 된다
    COUNT
};

// 회신 큐 원소 — 워커가 채워 넣고 게임 루프가 꺼낸다. 값 타입이라 요청자가 사라져도 안전하다.
struct DBResult
{
    DBJobKind    kind;
    DBResultCode code;
    int64_t      accountId;
    int64_t      sessionId;   // 요청 때 실어 보낸 값 그대로
    int64_t      requestNs;   // 요청 때 실어 보낸 값 그대로
    float        x;           // ↓ FOUND 일 때만 유효 — 멤버 이름이 DBSaveJob 과 같아야 한다(열 표가 이름으로 잇는다)
    float        y;
    int32_t      mapId;
    int32_t      level;
    int64_t      exp;
#if USE_INVENTORY
    // ↓ FOUND·NOT_FOUND 둘 다 유효 — 캐릭터 행이 없어도 가방은 읽는다(가방은 바뀌자마자 쓰고 캐릭터 행은
    //   주기·로그아웃에 쓰므로, 첫 저장 전에 서버가 죽으면 가방 행만 있는 계정이 생긴다).
    //   가방 조회가 실패하면 code 는 FAILED 다 — 빈 가방으로 들여보내면 첫 아이템이 진짜 행을 덮는다.
    int32_t      invCount;     // inv 에 실린 줄 수
    int32_t      invOverflow;  // DB_INV_MAX_ROWS 를 넘어 못 실은 줄 수 (정상이면 0)
    DBInvRow     inv[DB_INV_MAX_ROWS];
#endif
};

// 워커 슬롯 — 스레드/커넥션/큐 1세트 (CIOCPServer::SendWorker 패턴).
//   mutex/cv는 이동불가라 unique_ptr로 힙 고정 후 포인터만 벡터에 보관.
struct DBWorkerSlot
{
    std::thread              thread;
    std::mutex               mutex;
    std::condition_variable  cv;
    std::vector<DBJob>       queue;           // 게임 push / 워커 swap-out
    size_t                   loadCount = 0;   // queue 안의 불러오기 수(mutex 보호) — 저장 상한과 따로 센다
    void*                    mysql = nullptr; // 실제 MYSQL* (헤더 오염 차단 위해 void*)
    void*                    stmt[DB_JOB_KIND_COUNT] = {};   // MYSQL_STMT* — 종류별 prepared 핸들 (커넥션 종속)
    DBSaveJob                bSave{};         // 저장 stmt 파라미터 버퍼 — execute 직전 잡을 통째로 복사. 열 표(kCharColumns)가 멤버 자리를 가리킨다
    DBSaveJob                bLoad{};         // 불러오기 stmt — accountId 가 WHERE 파라미터, 나머지 멤버가 결과 열(fetch 가 채운다)
#if USE_INVENTORY
    size_t                   itemCount = 0;         // queue 안의 아이템 저장 수(mutex 보호) — 저장 상한과 따로 센다
    DBItemSlotJob            bItem{};               // 가방 칸 저장 stmt 파라미터 버퍼 — 잡을 통째로 복사. 열 표(kInvColumns)가 멤버 자리를 가리킨다
    void*                    stmtLoadInv = nullptr; // 가방 불러오기 stmt — 작업 종류가 아니라 불러오기 잡의 "둘째 문장"
    DBItemSlotJob            bInvLoad{};            // 가방 불러오기 stmt — accountId 가 WHERE 파라미터, 나머지 멤버가 결과 열(fetch 가 줄마다 채운다)
#endif
    int                      index = 0;
};

// K개 워커 스레드 + 커넥션. accountId % K 고정 분배(같은 유저=같은 워커=UPSERT 순서 보장).
//   백프레셔: 슬롯 큐가 queueMax 도달 시 초과분 드롭(카운터+ERROR). DB는 관대(다음 주기/종료가 커버).
class CDBWorker
{
public:
    explicit CDBWorker(DBSaveCounters& counters);   // 저장 파이프라인 지표 자리 — 게임 그릇(CGameMonitor::_db)
    ~CDBWorker();

    // K 커넥션 + K 워커 스레드 시작. 하나라도 접속 실패 시 전체 롤백 후 false (fail-fast).
    bool Start(const DBConfig& config, int workerCount, int queueMax);

    // 게임루프 전용 — 스냅샷 1건 (로그아웃 등 단건). accountId%K 슬롯에 push.
    //   반환 false = 백프레셔로 드롭됨. 호출부는 dirty를 되돌려 다음 기회에 재시도해야 한다
    //   (드롭을 그냥 삼키면 그 변경은 영영 사라진다).
    bool Enqueue(const DBSaveJob& job);

    // 게임루프 전용 — 배치를 accountId%K로 분류 후 슬롯별 1회 lock+append+notify (주기/종료 저장).
    //   outDropped가 있으면 백프레셔로 버려진 잡의 batch 내 인덱스를 담아 준다
    //   → 호출부가 해당 플레이어의 dirty를 복원해 다음 주기에 자동 재시도.
    void EnqueueBatch(const std::vector<DBSaveJob>& batch,
                      std::vector<size_t>* outDropped = nullptr);

    // 게임루프 전용 — 불러오기 1건. 저장과 같은 슬롯 큐(accountId%K)에 넣어 계정별 순서를 지킨다.
    //   저장 상한(queueMax)은 건너뛴다 — 저장은 버려도 다음 주기가 덮지만, 불러오기는 버리면 그 사람이
    //   영영 못 들어온다. 대신 큐에 든 불러오기 수를 따로 세서 같은 상한을 넘으면 거절한다
    //   (끝없이 쌓이는 큐를 만들지 않는다. 정상 상태에선 안 닿는 값 — 닿았다면 DB 가 멈춘 것).
    //   반환 false = 거절됨. 결과가 오지 않으므로 호출부가 그 자리에서 처리해야 한다.
    bool EnqueueLoad(const DBLoadJob& job);

#if USE_INVENTORY
    // 게임루프 전용 — 가방 칸 저장 1건. 저장과 같은 슬롯 큐(accountId%K) — 계정별 순서가 불러오기와 맞물린다
    //   (같은 계정이 다시 들어오면 불러오기가 이 저장들 "뒤"에 줄 선다 → 방금 얻은 아이템을 본다).
    //   저장 상한(queueMax)은 건너뛰고 아이템 저장끼리만 센 수로 같은 상한을 건다(EnqueueLoad 와 같은 대우).
    //   반환 false = 거절(= DB 가 멈춘 것). 호출부가 세고 ERROR 를 남긴다 — 아이템을 되살릴 길은 이번 범위 밖.
    bool EnqueueItemSlot(const DBItemSlotJob& job);

    // 아이템 저장 계수기 자리를 알려 준다 — Start 전에 한 번. 없으면(nullptr) 세지 않는다.
    void SetItemSaveCounters(DBItemSaveCounters* counters) { _itemCounters = counters; }
#endif

    // 게임루프 전용 — 쌓인 결과를 통째로 꺼낸다(락 1회). out 은 비어 있어야 한다.
    //   벡터를 맞바꾸므로 양쪽 다 용량이 보존된다 → 틱마다 할당하지 않는다
    //   (SwapOutNetworkEvents 와 같은 방식). 처리 중에 도착한 결과는 다음 틱으로 넘어간다.
    void SwapOutResults(std::vector<DBResult>& out);

    // 모든 슬롯의 잔여 잡을 비우고 워커 종료 (멱등 — 소멸자에서도 호출).
    //   드레인 중에 끝난 불러오기의 결과는 회신 큐에 쌓인 채 버려진다(게임 스레드가 이미 끝난 뒤다).
    void Shutdown(int drainTimeoutSec);

private:
    // ExecLoad 한 번의 결말 — 커넥션 유실만 따로 가른다(읽기는 멱등이라 재연결 후 한 번 더 해 볼 수 있다).
    enum class LoadOutcome : uint8_t { FOUND, NOT_FOUND, QUERY_ERROR, CONNECTION_LOST };

    void WorkerThread(int idx);
    bool Connect(void*& outMysql);            // 슬롯 커넥션 1개 생성
    bool PrepareStmt(DBWorkerSlot& slot);     // slot.mysql로 종류별 prepared stmt 준비 + 바인딩 (하나라도 실패하면 전부 닫고 false)
    void CloseStmts(DBWorkerSlot& slot);      // 종류별 stmt 전부 닫기 (커넥션은 건드리지 않음)
    void DropConnection(DBWorkerSlot& slot);  // stmt 전부 + 커넥션 닫고 null 로 — 유실 확인 시
    bool ExecSave(DBWorkerSlot& slot, const DBSaveJob& job);   // 커넥션 유실 시 stmt/mysql을 null로(재연결은 WorkerThread)
    // 결과는 항상 채운다(실패도 결과다). reconnectFailedThisBatch = 이 묶음에서 재연결이 실패한 적 있나(WorkerThread 지역값).
    void ExecLoad(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out, bool& reconnectFailedThisBatch);
    LoadOutcome RunLoadOnce(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out);
#if USE_INVENTORY
    // 불러오기의 둘째 문장 — 가방 줄을 out.inv 에 채운다. 결말은 LoadOutcome 로(FOUND = 성공, 줄이 0 이어도).
    LoadOutcome RunInventoryLoad(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out);
    // 가방 칸 저장 — 커넥션이 없거나 도중에 죽으면 재연결 한 번 하고 다시 쓴다(최종값이라 두 번 써도 같다).
    void ExecItemSave(DBWorkerSlot& slot, const DBItemSlotJob& job, bool& reconnectFailedThisBatch);
    LoadOutcome RunItemSaveOnce(DBWorkerSlot& slot, const DBItemSlotJob& job);   // FOUND = 성공
    // 새 문장들의 오류 분류(유실 / 그 밖). stmt 는 MYSQL_STMT* — 헤더를 mysql.h 로 더럽히지 않으려고 void*
    static LoadOutcome ClassifyStmtError(void* stmt, const char* what, int64_t accountId);
    DBItemSaveCounters* _itemCounters = nullptr;
#endif
    // 불러오기용 재연결 — 이 묶음에서 이미 실패했으면 시도하지 않는다(죽은 DB 에 불러오기마다 접속 제한 시간을 쓰지 않게).
    bool TryReconnectForLoad(DBWorkerSlot& slot, bool& reconnectFailedThisBatch);
    void PushResult(const DBResult& result);  // 워커 스레드 → 회신 큐
    // 백프레셔 포함 — 큐 여유만큼만 수용하고 실제 수용 개수를 반환 (나머지는 드롭). 저장 전용.
    size_t PushToSlot(DBWorkerSlot& slot, const DBJob* jobs, size_t count);

    int SlotIndex(int64_t accountId) const    // accountId % K (음수 방어)
    {
        return static_cast<int>(static_cast<uint64_t>(accountId) % static_cast<uint64_t>(_workerCount));
    }

    DBSaveCounters&  _counters;   // 저장 성공·실패·드롭·큐 깊이·워커 수 — 게임 그릇(GameMonitor.h DBSaveCounters)
    DBConfig         _config;
    int              _workerCount = 1;
    int              _queueMax    = 20000;

    std::vector<std::unique_ptr<DBWorkerSlot>> _workers;
    std::atomic<bool> _stop{ false };
    bool              _started = false;
    bool              _libInited = false;   // mysql_library_init 성공 여부 (library_end 짝 맞춤)

    // EnqueueBatch 분류 버퍼 재사용 (게임스레드/종료 시 순차 접근 — 동시 호출 없음).
    //   저장 잡을 큐 원소(DBJob)로 바꿔 담는 일도 여기서, 즉 "락 밖에서" 한다 → 락을 쥐는 시간은
    //   범위 삽입 한 번 그대로다.
    std::vector<std::vector<DBJob>>     _perWorker;
    // _perWorker와 나란한 원본 batch 인덱스 — 드롭분을 호출부에 되돌려주기 위함
    // (PushToSlot은 앞에서부터 수용하므로 드롭분은 각 슬롯 목록의 꼬리).
    std::vector<std::vector<size_t>>    _perWorkerIdx;

    // 회신 큐 — 워커 K개가 넣고 게임 스레드 하나가 꺼낸다. 워커가 소유하므로 종료 때 안전하다:
    //   게임 스레드가 먼저 끝나고 워커가 잔여를 비우는 동안에도 결과를 넣을 자리가 살아 있다.
    //   alignas(64): 워커가 쓰는 이 묶음이, 게임 스레드가 읽는 위 멤버들과 캐시 줄을 나눠 쓰지 않게 띄운다.
    struct alignas(64) ResultBox
    {
        std::mutex            mutex;
        std::vector<DBResult> items;
    };
    ResultBox _results;
};

#endif // USE_DB_WORKER
