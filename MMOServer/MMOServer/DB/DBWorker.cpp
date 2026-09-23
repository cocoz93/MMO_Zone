#include "DBWorker.h"

#if USE_DB_WORKER

#ifdef _WIN32
#include <winsock2.h>   // mysql.h 이전에 — 구버전 winsock 자동 include 충돌 차단
#endif
#include <mysql.h>
#include <errmsg.h>     // CR_SERVER_GONE_ERROR / CR_SERVER_LOST (커넥션 유실 감지)
#include <mysqld_error.h>   // ER_NO_SUCH_TABLE — 가방 테이블이 없을 때 안내를 정확히 걸려고
#include <cstdio>
#include <cstring>      // strlen (JobKindDesc.sql 길이)
#include <chrono>

#include "MonitorManager.h"
#include <Common/ErrorLog.h>
#include "CoreAffinity.h"
#if USE_INVENTORY
#include "GameMonitor.h"    // DBItemSaveCounters — 아이템 저장 계수기(게임 쪽 그릇)
#endif

#pragma comment(lib, "libmysql.lib")

CDBWorker::CDBWorker(CMonitorManager& monitor)
    : _monitor(monitor)
{
}

CDBWorker::~CDBWorker()
{
    Shutdown(0);
}

bool CDBWorker::Start(const DBConfig& config, int workerCount, int queueMax)
{
    _config      = config;
    _workerCount = (workerCount > 0) ? workerCount : 1;
    if (_workerCount > CMonitorManager::MAX_DB_WORKERS)
        _workerCount = CMonitorManager::MAX_DB_WORKERS;   // 지표 배열 상한에 맞춤
    _queueMax    = (queueMax > 0) ? queueMax : 20000;

    // 멀티스레드 안전 초기화 — 워커 스레드 생성 "전"에 메인에서 1회
    if (mysql_library_init(0, nullptr, nullptr) != 0)
    {
        SLOG_ERROR("[DB] mysql_library_init failed");
        return false;
    }
    _libInited = true;   // 이후 어느 경로로 실패하든 library_end로 롤백 (짝 맞춤)

    // K개 커넥션을 메인 스레드에서 생성(=fail-fast). 하나라도 실패하면 이미 연 것 정리 후 false.
    _workers.reserve(_workerCount);
    for (int i = 0; i < _workerCount; ++i)
    {
        auto slot = std::make_unique<DBWorkerSlot>();
        slot->index = i;
        if (!Connect(slot->mysql) || !PrepareStmt(*slot))
        {
            SLOG_ERROR("[DB] worker {} connect/prepare failed", i);
            DropConnection(*slot);
            for (auto& s : _workers)
                DropConnection(*s);
            _workers.clear();
            mysql_library_end();       // library_init 롤백 (Start 실패 시 누수 방지)
            _libInited = false;
            return false;
        }
        _workers.push_back(std::move(slot));
    }

    _perWorker.assign(_workerCount, {});
    _perWorkerIdx.assign(_workerCount, {});
    _monitor._dbWorkerCount.Store(_workerCount);   // 지표 노출 상한

    // 커넥션 확보 후 워커 스레드 시작 (각 슬롯 커넥션은 해당 워커만 단독 사용)
    _stop.store(false);
    for (int i = 0; i < _workerCount; ++i)
        _workers[i]->thread = std::thread(&CDBWorker::WorkerThread, this, i);

    _started = true;
    SLOG_INFO("[DB] connected {}:{} db={} — {} worker(s), queueMax={}",
              _config.host, _config.port, _config.database, _workerCount, _queueMax);
    return true;
}

bool CDBWorker::Connect(void*& outMysql)
{
    MYSQL* m = mysql_init(nullptr);
    if (m == nullptr)
    {
        SLOG_ERROR("[DB] mysql_init failed");
        return false;
    }

    unsigned int connTimeout = static_cast<unsigned int>(_config.connectTimeoutSec);
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &connTimeout);
    // 읽기/쓰기 타임아웃 — DB가 응답 없이 멈추면 쿼리가 무한 대기하지 않고 실패 반환(재연결 판단 가능)
    unsigned int rwTimeout = static_cast<unsigned int>(_config.rwTimeoutSec);
    mysql_options(m, MYSQL_OPT_READ_TIMEOUT, &rwTimeout);
    mysql_options(m, MYSQL_OPT_WRITE_TIMEOUT, &rwTimeout);
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (mysql_real_connect(m,
            _config.host.c_str(), _config.user.c_str(), _config.password.c_str(),
            _config.database.c_str(), static_cast<unsigned int>(_config.port),
            nullptr, 0) == nullptr)
    {
        SLOG_ERROR("[DB] connect failed: {}", mysql_error(m));
        mysql_close(m);
        return false;
    }

    outMysql = m;
    return true;
}

size_t CDBWorker::PushToSlot(DBWorkerSlot& slot, const DBJob* jobs, size_t count)
{
    if (count == 0)
        return 0;

    size_t take = 0;
    {
        std::lock_guard<std::mutex> lk(slot.mutex);
        const size_t cur  = slot.queue.size();
        const size_t cap  = static_cast<size_t>(_queueMax);
        const size_t room = (cur < cap) ? (cap - cur) : 0;
        take = (count <= room) ? count : room;    // 백프레셔: 여유만큼만 수용
        if (take > 0)
            slot.queue.insert(slot.queue.end(), jobs, jobs + take);
    }

    if (take > 0)
        slot.cv.notify_one();                     // 실제로 넣었을 때만 기상

    const size_t dropped = count - take;
    if (dropped > 0)
    {
        _monitor._dbDroppedJobs.Add(static_cast<int64_t>(dropped));
        SLOG_ERROR("[DB] backpressure drop — worker={} dropped={} (queue full, max={})",
                   slot.index, dropped, _queueMax);
    }

    return take;
}

bool CDBWorker::Enqueue(const DBSaveJob& job)
{
    const DBJob wrapped = DBJob::Save(job);
    return PushToSlot(*_workers[SlotIndex(job.accountId)], &wrapped, 1) == 1;
}

void CDBWorker::EnqueueBatch(const std::vector<DBSaveJob>& batch,
                             std::vector<size_t>* outDropped)
{
    if (outDropped != nullptr)
        outDropped->clear();
    if (batch.empty())
        return;

    // 단일 워커: 분류 생략 (K=1 fast path). 큐 원소(DBJob)로 바꿔 담는 일만 락 밖에서 하고 통째 push.
    if (_workerCount == 1)
    {
        std::vector<DBJob>& wrapped = _perWorker[0];
        wrapped.clear();
        wrapped.reserve(batch.size());
        for (const DBSaveJob& job : batch)
            wrapped.push_back(DBJob::Save(job));

        const size_t take = PushToSlot(*_workers[0], wrapped.data(), wrapped.size());
        if (outDropped != nullptr)
            for (size_t i = take; i < batch.size(); ++i)   // 앞에서부터 수용 → 드롭분은 꼬리
                outDropped->push_back(i);
        return;
    }

    // K>1: accountId%K로 워커별 분류 후 슬롯당 1회 핸드오프
    for (auto& v : _perWorker)
        v.clear();
    for (auto& v : _perWorkerIdx)
        v.clear();
    for (size_t i = 0; i < batch.size(); ++i)
    {
        const int slot = SlotIndex(batch[i].accountId);
        _perWorker[slot].push_back(DBJob::Save(batch[i]));
        _perWorkerIdx[slot].push_back(i);   // 원본 인덱스를 나란히 보존 (드롭 피드백용)
    }
    for (int i = 0; i < _workerCount; ++i)
    {
        if (_perWorker[i].empty())
            continue;
        const size_t take = PushToSlot(*_workers[i], _perWorker[i].data(), _perWorker[i].size());
        if (outDropped != nullptr)
            for (size_t k = take; k < _perWorkerIdx[i].size(); ++k)   // 앞에서부터 수용 → 드롭분은 꼬리
                outDropped->push_back(_perWorkerIdx[i][k]);
    }
}

bool CDBWorker::EnqueueLoad(const DBLoadJob& job)
{
    DBWorkerSlot& slot = *_workers[SlotIndex(job.accountId)];   // 저장과 같은 슬롯 — 계정별 순서 보장

    {
        std::lock_guard<std::mutex> lk(slot.mutex);
        // 저장 상한(queue.size() 기준)은 보지 않는다 — 저장이 큐를 가득 채워도 불러오기는 들어간다.
        //   대신 불러오기끼리만 센 수로 같은 상한을 건다. swap-out 때 0 으로 되돌아간다(WorkerThread).
        if (slot.loadCount >= static_cast<size_t>(_queueMax))
            return false;
        slot.queue.push_back(DBJob::Load(job));
        ++slot.loadCount;
    }

    slot.cv.notify_one();
    return true;
}

#if USE_INVENTORY
bool CDBWorker::EnqueueItemSlot(const DBItemSlotJob& job)
{
    DBWorkerSlot& slot = *_workers[SlotIndex(job.accountId)];   // 저장·불러오기와 같은 슬롯 — 계정별 순서 보장

    {
        std::lock_guard<std::mutex> lk(slot.mutex);
        // 저장 상한(queue.size() 기준)은 보지 않는다 — 위치 저장이 큐를 채워도 아이템 저장은 들어간다.
        //   아이템 저장끼리만 센 수로 같은 상한을 건다. 정상 상태에선 안 닿는 값이다(닿았다면 DB 가 멈춘 것).
        if (slot.itemCount >= static_cast<size_t>(_queueMax))
            return false;
        slot.queue.push_back(DBJob::Item(job));
        ++slot.itemCount;
    }

    slot.cv.notify_one();
    return true;
}
#endif

void CDBWorker::PushResult(const DBResult& result)
{
    std::lock_guard<std::mutex> lk(_results.mutex);
    _results.items.push_back(result);
}

void CDBWorker::SwapOutResults(std::vector<DBResult>& out)
{
    std::lock_guard<std::mutex> lk(_results.mutex);
    out.swap(_results.items);   // out 은 비어 있어야 한다 — 그 빈 벡터(용량 보존)가 회신 큐의 새 몸이 된다
}

void CDBWorker::WorkerThread(int idx)
{
    CoreAffinity::PinIoThread();   // DB 워커 → 게임코어 밖으로 (격리 off면 no-op)
    mysql_thread_init();

    DBWorkerSlot& slot = *_workers[idx];

    std::vector<DBJob> local;
    while (true)
    {
        {
            std::unique_lock<std::mutex> lk(slot.mutex);
            slot.cv.wait(lk, [this, &slot] { return !slot.queue.empty() || _stop.load(); });
            if (slot.queue.empty() && _stop.load())
                break;                   // 정지 요청 + 잔여 없음 → 종료(드레인 완료)
            local.swap(slot.queue);       // 누적분 통째 인출 (여러 주기분 병합 가능)
            slot.loadCount = 0;           // 큐를 통째로 가져왔으므로 큐에 든 불러오기도 0 (EnqueueLoad 의 상한 기준)
#if USE_INVENTORY
            slot.itemCount = 0;           // 같은 이유 — 아이템 저장 상한(EnqueueItemSlot)의 기준
#endif
        }

        // 배치 처리 전 — 커넥션이 끊겨 있으면 재연결 1회 시도.
        //   배치=저장 주기 단위라, 죽은 DB엔 주기마다 한 번만 재시도(자연스러운 백오프).
        if (slot.mysql == nullptr)
        {
            void* fresh = nullptr;
            if (Connect(fresh))
            {
                slot.mysql = fresh;
                if (PrepareStmt(slot))
                    SLOG_INFO("[DB] worker {} reconnected", idx);
                else
                {
                    mysql_close(static_cast<MYSQL*>(slot.mysql));   // stmt 준비 실패 → 커넥션 롤백
                    slot.mysql = nullptr;
                }
            }
            // 실패하면 이번 배치는 전부 실패 처리되고, 다음 배치에서 다시 시도
        }

        // 이 묶음에서 재연결이 "실패한 적 있나". 위 머리 재연결을 했는데도 커넥션이 없으면 실패한 것이다.
        //   불러오기가 커넥션 없는 상태를 만났을 때, 그 자리에서 한 번 살려 볼지를 이 값으로 정한다(ExecLoad).
        //   머리에서 재연결을 "안 한" 경우(핸들은 살아 있었는데 서버 쪽에서 끊긴 커넥션)를 가려내려는 것이다 —
        //   그 커넥션을 앞선 저장이 먼저 밟아 내리면, 뒤의 불러오기는 아무도 재연결을 안 해 본 상태로 온다.
        bool reconnectFailedThisBatch = (slot.mysql == nullptr);

        _monitor._dbQueueDepth[idx].Store(static_cast<int64_t>(local.size()));  // 워커별 게이지

        static_assert(DB_JOB_KIND_COUNT == (USE_INVENTORY ? 3 : 2), "DBJobKind 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
        for (const DBJob& job : local)
        {
            switch (job.kind)
            {
            case DBJobKind::SAVE_POSITION:
                if (ExecSave(slot, job.save))   // 유실 시 ExecSave가 stmt/mysql을 null로 만듦
                    _monitor._dbSavedJobs.Inc();
                else
                    _monitor._dbFailedJobs.Inc();
                break;

            case DBJobKind::LOAD_CHARACTER:
            {
                // 코어 지표(_dbSavedJobs·_dbFailedJobs)는 건드리지 않는다 — 그 둘은 "저장"만 세는 기존 게이트다.
                //   불러오기는 결과에 코드만 실어 보내고, 세는 일은 게임 스레드가 결과를 꺼낼 때 한다.
                //   성공이든 실패든 결과는 반드시 하나 돌려준다 — 안 돌려주면 요청자가 영영 기다린다.
                DBResult result{};
                ExecLoad(slot, job.load, result, reconnectFailedThisBatch);
                PushResult(result);   // 묶음 끝까지 기다리지 않고 바로 — 뒤에 저장 수천 건이 있어도 입장이 안 밀린다
                break;
            }

#if USE_INVENTORY
            case DBJobKind::SAVE_ITEM_SLOT:
                // 코어 지표(_dbSavedJobs·_dbFailedJobs)는 "위치 저장"만 센다 — 아이템은 게임 쪽 계수기로 따로(ExecItemSave)
                ExecItemSave(slot, job.item, reconnectFailedThisBatch);
                break;
#endif

            case DBJobKind::COUNT:
                break;   // 값이 아니라 개수 표시 — 큐에 들어올 수 없다
            }
        }
        local.clear();
    }

    mysql_thread_end();
}

// ==========================================================================
// 작업 "종류" 표 — 워커 몸통(스레드·큐·재연결·분배)과 "무슨 문장을 돌리나"를 분리.
//   종류마다 문장 하나 + 바인딩 함수 하나. 새 종류가 필요해지면 DBJobKind 에 값을 넣고
//   여기에 한 줄을 형제로 추가한다 (빠뜨리면 아래 static_assert 가 컴파일을 막는다).
//   ※ 저장 쪽은 손실 허용·무트랜잭션 데이터 전용 레인. 재화·거래 같은 트랜잭션/무손실
//     데이터는 이 레인이 아니라 별도 레인(pending 상태머신)으로 가야 함.
//     그 레인이 생기면 이번에 만든 회신 큐를 그대로 쓴다(저장 결과를 돌려받아야 실패를 안다).
//   ※ 둘째 종류(불러오기)가 들어오면서 큐 원소에 종류 표시(DBJob::kind)가 생겼다.
//     예고대로 "둘째 종류 도입 시점"에 넣은 것이고, 셋째부터는 같은 자리에 줄만 늘어난다.
// ==========================================================================
struct JobKindDesc
{
    const char* name;                                      // 로그에 찍는 이름
    const char* sql;                                       // 이 종류의 문장
    bool (*bind)(MYSQL_STMT* st, DBWorkerSlot& slot);      // 파라미터(·결과) 버퍼 바인딩 — prepare 시 1회
};

// ── 위치 저장 ─────────────────────────────────────────────────────────────
static bool BindPositionParams(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    // VALUES 6개 + ON DUPLICATE KEY UPDATE 5개 = ? 11개
    //   (x·y·map_id·level·exp 는 양쪽이 같은 버퍼를 참조한다 — 값을 한 번만 복사하면 된다)
    MYSQL_BIND b[11] = {};
    b[0].buffer_type = MYSQL_TYPE_LONGLONG; b[0].buffer = &slot.bAccountId;
    b[1].buffer_type = MYSQL_TYPE_FLOAT;    b[1].buffer = &slot.bX;
    b[2].buffer_type = MYSQL_TYPE_FLOAT;    b[2].buffer = &slot.bY;
    b[3].buffer_type = MYSQL_TYPE_LONG;     b[3].buffer = &slot.bMapId;
    b[4].buffer_type = MYSQL_TYPE_LONG;     b[4].buffer = &slot.bLevel;
    b[5].buffer_type = MYSQL_TYPE_LONGLONG; b[5].buffer = &slot.bExp;
    b[6]  = b[1];  // UPDATE x      = 같은 bX
    b[7]  = b[2];  // UPDATE y      = 같은 bY
    b[8]  = b[3];  // UPDATE map_id = 같은 bMapId
    b[9]  = b[4];  // UPDATE level  = 같은 bLevel
    b[10] = b[5];  // UPDATE exp    = 같은 bExp
    return mysql_stmt_bind_param(st, b) == 0;
}

static void CopyPositionValues(DBWorkerSlot& slot, const DBSaveJob& job)
{
    slot.bAccountId = job.accountId;
    slot.bX         = job.x;
    slot.bY         = job.y;
    slot.bMapId     = job.mapId;
    slot.bLevel     = job.level;
    slot.bExp       = job.exp;
}

// ── 캐릭터 불러오기 ───────────────────────────────────────────────────────
static bool BindLoadCharacter(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    // 파라미터 1개 (WHERE account_id=?)
    MYSQL_BIND p[1] = {};
    p[0].buffer_type = MYSQL_TYPE_LONGLONG; p[0].buffer = &slot.bLoadAccountId;
    if (mysql_stmt_bind_param(st, p) != 0)
        return false;

    // 결과 5열 (x, y, map_id, level, exp) — fetch 가 슬롯 버퍼에 바로 채운다.
    //   다섯 열 모두 NOT NULL 고정폭이고 열 타입과 버퍼 타입이 같아(FLOAT→float, INT→int32,
    //   BIGINT→int64) 변환·잘림이 없다. is_null·length·error 는 비워 두면 라이브러리가 내부 값으로 채운다.
    MYSQL_BIND r[5] = {};
    r[0].buffer_type = MYSQL_TYPE_FLOAT;    r[0].buffer = &slot.bLoadX;
    r[1].buffer_type = MYSQL_TYPE_FLOAT;    r[1].buffer = &slot.bLoadY;
    r[2].buffer_type = MYSQL_TYPE_LONG;     r[2].buffer = &slot.bLoadMapId;
    r[3].buffer_type = MYSQL_TYPE_LONG;     r[3].buffer = &slot.bLoadLevel;
    r[4].buffer_type = MYSQL_TYPE_LONGLONG; r[4].buffer = &slot.bLoadExp;
    return mysql_stmt_bind_result(st, r) == 0;
}

#if USE_INVENTORY
// ── 가방 칸 저장 ─────────────────────────────────────────────────────────
static bool BindItemSlotParams(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    // VALUES 4개 + ON DUPLICATE KEY UPDATE 2개 = ? 6개 (item_id·count 는 양쪽이 같은 버퍼를 참조한다)
    MYSQL_BIND b[6] = {};
    b[0].buffer_type = MYSQL_TYPE_LONGLONG; b[0].buffer = &slot.bItemAccountId;
    b[1].buffer_type = MYSQL_TYPE_LONG;     b[1].buffer = &slot.bItemSlot;
    b[2].buffer_type = MYSQL_TYPE_LONG;     b[2].buffer = &slot.bItemId;
    b[3].buffer_type = MYSQL_TYPE_LONG;     b[3].buffer = &slot.bItemCount;
    b[4] = b[2];   // UPDATE item_id = 같은 bItemId
    b[5] = b[3];   // UPDATE count   = 같은 bItemCount
    return mysql_stmt_bind_param(st, b) == 0;
}

// ── 가방 불러오기 (불러오기 잡의 둘째 문장 — 작업 종류가 아니다) ──────────
//   빈 칸(item_id 0)은 안 읽는다. 칸 순서로 받는다 — 줄이 넘칠 때 어느 줄을 버릴지가 늘 같게.
static const char* kLoadInvSql =
    "SELECT slot,item_id,count FROM inventory WHERE account_id=? AND item_id<>0 ORDER BY slot";

static bool BindLoadInventory(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    MYSQL_BIND p[1] = {};
    p[0].buffer_type = MYSQL_TYPE_LONGLONG; p[0].buffer = &slot.bInvAccountId;
    if (mysql_stmt_bind_param(st, p) != 0)
        return false;

    // 결과 3열 — 셋 다 NOT NULL INT 라 int32 버퍼로 변환·잘림 없이 받는다
    MYSQL_BIND r[3] = {};
    r[0].buffer_type = MYSQL_TYPE_LONG; r[0].buffer = &slot.bInvSlot;
    r[1].buffer_type = MYSQL_TYPE_LONG; r[1].buffer = &slot.bInvItemId;
    r[2].buffer_type = MYSQL_TYPE_LONG; r[2].buffer = &slot.bInvCount;
    return mysql_stmt_bind_result(st, r) == 0;
}
#endif

// DBJobKind 순서 그대로 — 인덱스가 곧 종류다.
static const JobKindDesc kKinds[] = {
    /* SAVE_POSITION  */ { "save_position",
                           "INSERT INTO characters (account_id,x,y,map_id,level,exp) VALUES (?,?,?,?,?,?) "
                           "ON DUPLICATE KEY UPDATE x=?,y=?,map_id=?,level=?,exp=?",
                           BindPositionParams },
    /* LOAD_CHARACTER */ { "load_character",
                           "SELECT x,y,map_id,level,exp FROM characters WHERE account_id=?",
                           BindLoadCharacter },
#if USE_INVENTORY
    /* SAVE_ITEM_SLOT */ { "save_item_slot",
                           "INSERT INTO inventory (account_id,slot,item_id,count) VALUES (?,?,?,?) "
                           "ON DUPLICATE KEY UPDATE item_id=?,count=?",
                           BindItemSlotParams },
#endif
};
static_assert(sizeof(kKinds) / sizeof(kKinds[0]) == DB_JOB_KIND_COUNT,
              "DBJobKind 를 늘렸으면 kKinds 에도 같은 순서로 한 줄을 넣을 것");

static constexpr int kSaveIdx = static_cast<int>(DBJobKind::SAVE_POSITION);
static constexpr int kLoadIdx = static_cast<int>(DBJobKind::LOAD_CHARACTER);
#if USE_INVENTORY
static constexpr int kItemIdx = static_cast<int>(DBJobKind::SAVE_ITEM_SLOT);

// 문장 준비 실패 로그 — 가방 테이블이 없으면(ER_NO_SUCH_TABLE) 할 일을 같이 적는다.
//   게임 계정에는 CREATE 권한이 없어 서버가 테이블을 만들 수 없다 — 사람이 root 로 한 번 돌려야 한다.
static void LogPrepareFailure(const char* name, MYSQL_STMT* st)
{
    if (mysql_stmt_errno(st) == ER_NO_SUCH_TABLE)
        SLOG_ERROR("[DB] stmt_prepare failed ({}): {} — root 로 MMOServer/MMOServer/DB/schema.sql 을 먼저 돌리세요 "
                   "(게임 계정은 테이블을 못 만든다)", name, mysql_stmt_error(st));
    else
        SLOG_ERROR("[DB] stmt_prepare failed ({}): {}", name, mysql_stmt_error(st));
}
#endif

bool CDBWorker::PrepareStmt(DBWorkerSlot& slot)
{
    MYSQL* m = static_cast<MYSQL*>(slot.mysql);

    // 종류마다 하나씩. 하나라도 실패하면 앞서 만든 것까지 전부 닫고 false —
    //   "저장은 되는데 불러오기만 안 되는" 반쪽 상태로 워커를 돌리지 않는다.
    for (int i = 0; i < DB_JOB_KIND_COUNT; ++i)
    {
        const JobKindDesc& kind = kKinds[i];

        MYSQL_STMT* st = mysql_stmt_init(m);
        if (st == nullptr)
        {
            SLOG_ERROR("[DB] stmt_init failed ({}): {}", kind.name, mysql_error(m));
            CloseStmts(slot);
            return false;
        }

        if (mysql_stmt_prepare(st, kind.sql, static_cast<unsigned long>(strlen(kind.sql))) != 0)
        {
#if USE_INVENTORY
            LogPrepareFailure(kind.name, st);
#else
            SLOG_ERROR("[DB] stmt_prepare failed ({}): {}", kind.name, mysql_stmt_error(st));
#endif
            mysql_stmt_close(st);
            CloseStmts(slot);
            return false;
        }

        if (!kind.bind(st, slot))   // 종류별 바인딩(버퍼는 슬롯 멤버)
        {
            SLOG_ERROR("[DB] stmt_bind failed ({}): {}", kind.name, mysql_stmt_error(st));
            mysql_stmt_close(st);
            CloseStmts(slot);
            return false;
        }

        slot.stmt[i] = st;
    }

#if USE_INVENTORY
    // 불러오기 잡의 둘째 문장 — 종류 표 밖이라 따로 준비한다. 실패 규칙은 위와 같다(반쪽 상태로 안 돈다).
    {
        MYSQL_STMT* st = mysql_stmt_init(m);
        if (st == nullptr)
        {
            SLOG_ERROR("[DB] stmt_init failed (load_inventory): {}", mysql_error(m));
            CloseStmts(slot);
            return false;
        }
        if (mysql_stmt_prepare(st, kLoadInvSql, static_cast<unsigned long>(strlen(kLoadInvSql))) != 0)
        {
            LogPrepareFailure("load_inventory", st);
            mysql_stmt_close(st);
            CloseStmts(slot);
            return false;
        }
        if (!BindLoadInventory(st, slot))
        {
            SLOG_ERROR("[DB] stmt_bind failed (load_inventory): {}", mysql_stmt_error(st));
            mysql_stmt_close(st);
            CloseStmts(slot);
            return false;
        }
        slot.stmtLoadInv = st;
    }
#endif
    return true;
}

void CDBWorker::CloseStmts(DBWorkerSlot& slot)
{
    for (int i = 0; i < DB_JOB_KIND_COUNT; ++i)
    {
        if (slot.stmt[i] != nullptr)
        {
            mysql_stmt_close(static_cast<MYSQL_STMT*>(slot.stmt[i]));
            slot.stmt[i] = nullptr;
        }
    }
#if USE_INVENTORY
    if (slot.stmtLoadInv != nullptr)
    {
        mysql_stmt_close(static_cast<MYSQL_STMT*>(slot.stmtLoadInv));
        slot.stmtLoadInv = nullptr;
    }
#endif
}

void CDBWorker::DropConnection(DBWorkerSlot& slot)
{
    // stmt 는 커넥션에 매여 있다 — 커넥션을 닫기 "전에" 먼저 닫는다.
    CloseStmts(slot);
    if (slot.mysql != nullptr)
    {
        mysql_close(static_cast<MYSQL*>(slot.mysql));
        slot.mysql = nullptr;
    }
}

bool CDBWorker::ExecSave(DBWorkerSlot& slot, const DBSaveJob& job)
{
    if (slot.mysql == nullptr || slot.stmt[kSaveIdx] == nullptr)
        return false;   // 커넥션/stmt 없음(재연결 대기 중) — 즉시 실패, 무한 대기 없음

    // 바인딩 버퍼에 이번 잡 값 복사 (stmt는 이 버퍼 주소를 이미 가리킴 → execute만 하면 됨)
    CopyPositionValues(slot, job);

    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmt[kSaveIdx]);
    if (mysql_stmt_execute(st) == 0)
        return true;

    // 저장 실패 — 커넥션 유실인지, 단순 오류(제약 위반 등)인지 구분
    const unsigned int err = mysql_stmt_errno(st);
    if (err == CR_SERVER_GONE_ERROR || err == CR_SERVER_LOST)
    {
        // 커넥션이 죽음 → stmt·커넥션 닫고 null로. 재연결은 WorkerThread가 다음 배치 시작 때 시도(백오프).
        SLOG_WARN("[DB] connection lost (errno={}) — will reconnect next batch", err);
        DropConnection(slot);
    }
    else
    {
        SLOG_ERROR("[DB] save failed (account={}): {}", job.accountId, mysql_stmt_error(st));
    }
    return false;
}

CDBWorker::LoadOutcome CDBWorker::RunLoadOnce(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out)
{
    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmt[kLoadIdx]);
    slot.bLoadAccountId = job.accountId;

    // 오류 분류 — 유실(재연결 후 다시 해 볼 값어치가 있다)과 그 밖(다시 해도 같다)을 가른다.
    //   errno·메시지는 free_result 가 지우기 전에 먼저 읽는다.
    auto classifyError = [&](const char* step) -> LoadOutcome
    {
        const unsigned int err = mysql_stmt_errno(st);
        if (err == CR_SERVER_GONE_ERROR || err == CR_SERVER_LOST)
        {
            SLOG_WARN("[DB] connection lost during load {} (errno={}, account={})", step, err, job.accountId);
            return LoadOutcome::CONNECTION_LOST;
        }
        SLOG_ERROR("[DB] load {} failed (account={}): {}", step, job.accountId, mysql_stmt_error(st));
        return LoadOutcome::QUERY_ERROR;
    };

    if (mysql_stmt_execute(st) != 0)
        return classifyError("execute");

    // 기본 키 조회라 행은 많아야 하나다. fetch 반환값이 셋 이상이라 switch 로 가른다 —
    //   if/else 로 "0 이면 찾음, 아니면 없음"이라 쓰면 오류(1)가 "없음"에 삼켜져 새 캐릭터로 스폰된다.
    LoadOutcome outcome;
    const int rc = mysql_stmt_fetch(st);
    switch (rc)
    {
    case 0:
        out.x     = slot.bLoadX;
        out.y     = slot.bLoadY;
        out.mapId = slot.bLoadMapId;
        out.level = slot.bLoadLevel;
        out.exp   = slot.bLoadExp;
        outcome   = LoadOutcome::FOUND;
        break;

    case MYSQL_NO_DATA:
        outcome = LoadOutcome::NOT_FOUND;
        break;

    default:   // 1(오류) · MYSQL_DATA_TRUNCATED — 어느 쪽이든 버퍼 값을 믿을 수 없다
        outcome = classifyError("fetch");
        break;
    }

    // 결과 세트를 닫는다 — 안 닫으면 같은 커넥션의 다음 명령(바로 뒤의 저장)이
    //   "Commands out of sync" 로 실패한다. 커넥션이 이미 죽었으면 어차피 곧 닫히므로 건너뛴다.
    if (outcome != LoadOutcome::CONNECTION_LOST)
        mysql_stmt_free_result(st);

#if USE_INVENTORY
    // 둘째 문장 — 가방 줄. ★ 캐릭터 행이 없어도(NOT_FOUND) 읽는다(DBResult 주석 참조).
    //   ★ 가방이 실패하면 캐릭터 행을 읽었어도 전체가 실패다 — 빈 가방으로 들여보내면 첫 아이템이 진짜 행을 덮는다.
    if (outcome == LoadOutcome::FOUND || outcome == LoadOutcome::NOT_FOUND)
    {
        const LoadOutcome inv = RunInventoryLoad(slot, job, out);
        if (inv != LoadOutcome::FOUND)
            return inv;
    }
#endif

    return outcome;
}

#if USE_INVENTORY
// 새로 붙은 문장들의 오류 분류 — 유실(재연결 후 다시 해 볼 값어치가 있다)과 그 밖을 가른다(RunLoadOnce 와 같은 규칙).
CDBWorker::LoadOutcome CDBWorker::ClassifyStmtError(void* stmt, const char* what, int64_t accountId)
{
    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(stmt);
    const unsigned int err = mysql_stmt_errno(st);
    if (err == CR_SERVER_GONE_ERROR || err == CR_SERVER_LOST)
    {
        SLOG_WARN("[DB] connection lost during {} (errno={}, account={})", what, err, accountId);
        return LoadOutcome::CONNECTION_LOST;
    }
    SLOG_ERROR("[DB] {} failed (account={}): {}", what, accountId, mysql_stmt_error(st));
    return LoadOutcome::QUERY_ERROR;
}

CDBWorker::LoadOutcome CDBWorker::RunInventoryLoad(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out)
{
    // 재시도로 두 번 돌 수 있다 — 매번 비우고 시작한다(앞선 시도의 반쪽 줄이 섞이지 않게).
    out.invCount    = 0;
    out.invOverflow = 0;

    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmtLoadInv);
    if (st == nullptr)
        return LoadOutcome::QUERY_ERROR;   // PrepareStmt 가 캐릭터 문장과 함께 세우므로 닿지 않는다 — 닿으면 실패로
    slot.bInvAccountId = job.accountId;

    if (mysql_stmt_execute(st) != 0)
        return ClassifyStmtError(st, "load inventory execute", job.accountId);

    // 줄 여럿 — 끝(MYSQL_NO_DATA)까지 다 읽어야 다음 명령이 안 꼬인다. 넘치는 줄은 싣지 않고 수만 센다.
    for (;;)
    {
        const int rc = mysql_stmt_fetch(st);
        if (rc == MYSQL_NO_DATA)
            break;
        if (rc != 0)   // 1(오류) · MYSQL_DATA_TRUNCATED — 버퍼 값을 믿을 수 없다
        {
            const LoadOutcome e = ClassifyStmtError(st, "load inventory fetch", job.accountId);
            if (e != LoadOutcome::CONNECTION_LOST)
                mysql_stmt_free_result(st);
            return e;
        }
        if (out.invCount < DB_INV_MAX_ROWS)
            out.inv[out.invCount++] = DBInvRow{ slot.bInvSlot, slot.bInvItemId, slot.bInvCount };
        else
            ++out.invOverflow;
    }
    mysql_stmt_free_result(st);
    return LoadOutcome::FOUND;   // 줄이 0 이어도 성공 — "가방이 비었다"는 사실이다
}

CDBWorker::LoadOutcome CDBWorker::RunItemSaveOnce(DBWorkerSlot& slot, const DBItemSlotJob& job)
{
    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmt[kItemIdx]);
    slot.bItemAccountId = job.accountId;
    slot.bItemSlot      = job.slot;
    slot.bItemId        = job.itemId;
    slot.bItemCount     = job.count;
    if (mysql_stmt_execute(st) == 0)
        return LoadOutcome::FOUND;   // 성공
    return ClassifyStmtError(st, "save item slot", job.accountId);
}

void CDBWorker::ExecItemSave(DBWorkerSlot& slot, const DBItemSlotJob& job, bool& reconnectFailedThisBatch)
{
    // 위치 저장(ExecSave)과 다르게 "버리지 않는" 쪽 규칙 — 불러오기(ExecLoad)와 같은 재연결을 쓴다.
    //   커넥션이 없거나(앞선 잡이 유실을 밟아 내렸다) 도중에 죽으면 그 자리에서 한 번 살려 보고 다시 쓴다.
    //   칸의 최종값이라 두 번 써도 결과가 같다. 이 묶음에서 재연결이 이미 실패했으면 곧바로 실패로 센다
    //   (죽은 DB 에 잡마다 접속 제한 시간을 쓰며 큐를 막지 않는다 — TryReconnectForLoad).
    bool retried = false;
    LoadOutcome r = LoadOutcome::CONNECTION_LOST;

    if (slot.mysql == nullptr || slot.stmt[kItemIdx] == nullptr)
    {
        DropConnection(slot);
        if (TryReconnectForLoad(slot, reconnectFailedThisBatch))
        {
            retried = true;
            r = RunItemSaveOnce(slot, job);
        }
    }
    else
    {
        r = RunItemSaveOnce(slot, job);
        if (r == LoadOutcome::CONNECTION_LOST)
        {
            DropConnection(slot);
            if (TryReconnectForLoad(slot, reconnectFailedThisBatch))
            {
                retried = true;
                r = RunItemSaveOnce(slot, job);
            }
        }
    }
    if (r == LoadOutcome::CONNECTION_LOST)
        DropConnection(slot);   // 다시 해 본 것까지 죽었다 — 다음 묶음 머리가 다시 붙는다

    if (_itemCounters != nullptr)
    {
        if (retried)
            _itemCounters->_retried.Inc();
        if (r == LoadOutcome::FOUND)
            _itemCounters->_saved.Inc();
        else
            _itemCounters->_failed.Inc();
    }
    if (r != LoadOutcome::FOUND)
        SLOG_ERROR("[DB] item slot save LOST (account={}, slot={}, item={}, count={}) — 이 칸은 다음에 바뀔 때 다시 써진다",
                   job.accountId, job.slot, job.itemId, job.count);
}
#endif

bool CDBWorker::TryReconnectForLoad(DBWorkerSlot& slot, bool& reconnectFailedThisBatch)
{
    // 이 묶음에서 이미 실패했다 = DB 가 죽어 있다고 본다. 불러오기마다 접속 제한 시간(거절까지 약 2초,
    //   안 닿는 곳이면 connectTimeoutSec)을 기다리며 큐를 막지 않는다. 다음 묶음 머리에서 다시 해 본다.
    if (reconnectFailedThisBatch)
        return false;

    void* fresh = nullptr;
    if (Connect(fresh))
    {
        slot.mysql = fresh;
        if (PrepareStmt(slot))
        {
            SLOG_INFO("[DB] worker {} reconnected (load retry)", slot.index);
            return true;
        }
        mysql_close(static_cast<MYSQL*>(slot.mysql));   // stmt 준비 실패 → 커넥션 롤백
        slot.mysql = nullptr;
    }

    reconnectFailedThisBatch = true;
    return false;
}

void CDBWorker::ExecLoad(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out, bool& reconnectFailedThisBatch)
{
    // 결과는 어떤 경로로 나가든 채워져 있어야 한다 — 기본값을 "실패"로 깔고 시작한다.
    out.kind      = DBJobKind::LOAD_CHARACTER;
    out.code      = DBResultCode::FAILED;
    out.accountId = job.accountId;
    out.sessionId = job.sessionId;
    out.requestNs = job.requestNs;
    out.x         = 0.0f;
    out.y         = 0.0f;
    out.mapId     = 0;
    out.level     = 1;   // 못 읽었을 때의 값 — 새 캐릭터와 같다. FOUND 일 때만 행의 값으로 덮인다
    out.exp       = 0;
#if USE_INVENTORY
    out.invCount    = 0;   // 가방 줄 — RunInventoryLoad 가 채운다(실패면 code 가 FAILED 라 안 쓰인다)
    out.invOverflow = 0;
#endif

    // 읽기는 멱등이라 두 번 해도 안전하다 → 커넥션이 없거나 도중에 죽으면 그 자리에서 재연결해 본다.
    //   노는 커넥션은 MySQL 이 끊는다(wait_timeout). 서버를 밤새 켜 두면 아침 첫 접속이 여기에 걸리는데,
    //   다시 해 보지 않으면 그 사람이 영문 없이 튕긴다. 저장(ExecSave)의 재연결 동작은 건드리지 않는다.
    //
    // 커넥션이 없는 채로 들어오는 경우는 둘이다.
    //   (가) 묶음 머리의 재연결이 실패했다(DB 가 죽어 있다) → 플래그가 서 있어 아래가 곧바로 false → 즉시 실패
    //   (나) 같은 묶음의 앞선 "저장"이 유실을 밟아 커넥션을 내렸다. 서버 쪽에서 끊긴 커넥션은 핸들이 살아 있어
    //        머리에서는 재연결을 안 했다 → 아직 아무도 안 살려 봤으므로 여기서 한 번 해 본다.
    //        이걸 안 하면 같은 상황의 결과가 "묶음 안에서 저장이 먼저냐 불러오기가 먼저냐"에 달리게 된다.
    if (slot.mysql == nullptr || slot.stmt[kLoadIdx] == nullptr)
    {
        DropConnection(slot);   // 둘 중 하나만 null 인 반쪽 상태는 PrepareStmt 가 안 만들지만, 짝을 맞춰 비운다
        if (!TryReconnectForLoad(slot, reconnectFailedThisBatch))
            return;             // 즉시 실패 — 매달리지 않는다
    }

    LoadOutcome outcome = RunLoadOnce(slot, job, out);

    if (outcome == LoadOutcome::CONNECTION_LOST)
    {
        // 살아 있던 커넥션이 이 작업 중에 죽었다 → 재연결하고 "한 번만" 더.
        DropConnection(slot);
        if (TryReconnectForLoad(slot, reconnectFailedThisBatch))
        {
            outcome = RunLoadOnce(slot, job, out);
            if (outcome == LoadOutcome::CONNECTION_LOST)
                DropConnection(slot);
        }
    }

    switch (outcome)
    {
    case LoadOutcome::FOUND:            out.code = DBResultCode::FOUND;     break;
    case LoadOutcome::NOT_FOUND:        out.code = DBResultCode::NOT_FOUND; break;
    case LoadOutcome::QUERY_ERROR:      out.code = DBResultCode::FAILED;    break;
    case LoadOutcome::CONNECTION_LOST:  out.code = DBResultCode::FAILED;    break;
    }
}

void CDBWorker::Shutdown(int drainTimeoutSec)
{
    if (!_started)
        return;

    const auto t0 = std::chrono::steady_clock::now();

    // 모든 슬롯에 정지 신호 → 각 워커가 잔여 드레인 후 종료
    _stop.store(true);
    for (auto& s : _workers)
    {
        // 빈 lock — 막 wait에 진입하는 워커와의 race 방지.
        //   워커가 술어를 false로 평가한 직후 ~ 대기 큐에 등록되기 전 구간에 _stop+notify가 끼면
        //   기상이 유실되고, 타임아웃 없는 wait이라 아래 join이 영구 대기한다(종료 행).
        //   락을 한 번 통과시키면 그 구간이 닫힌다. 송신 워커 정지(IOCPServer.cpp)에 있는 것과 같은 브리지.
        { std::lock_guard<std::mutex> lk(s->mutex); }
        s->cv.notify_one();
    }

    for (auto& s : _workers)
        if (s->thread.joinable())
            s->thread.join();

    for (auto& s : _workers)
        DropConnection(*s);

    _workers.clear();

    if (_libInited)                    // library_init과 짝 — 모든 커넥션·스레드 정리 후 1회
    {
        mysql_library_end();
        _libInited = false;
    }
    _started = false;

    const auto drainMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    SLOG_INFO("[DB] {} worker(s) drained and stopped ({} ms)", _workerCount, drainMs);

    // 1단계: 워커가 잔여를 전부 비우고 종료하므로 타임아웃 미사용. 장애 대비 타임아웃은 3단계.
    (void)drainTimeoutSec;
}

#endif // USE_DB_WORKER
