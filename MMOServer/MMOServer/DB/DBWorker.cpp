#include "DBWorker.h"

#if USE_DB_WORKER

#ifdef _WIN32
#include <winsock2.h>   // mysql.h 이전에 — 구버전 winsock 자동 include 충돌 차단
#endif
#include <mysql.h>
#include <errmsg.h>     // CR_SERVER_GONE_ERROR / CR_SERVER_LOST (커넥션 유실 감지)
#include <mysqld_error.h>   // ER_NO_SUCH_TABLE — 가방 테이블이 없을 때 안내를 정확히 걸려고
#include <cassert>
#include <cstdio>
#include <cstdlib>      // _dupenv_s·getenv·free (실패 주입 계획 — USE_DB_FAULT_INJECT)
#include <stdexcept>    // std::stoi 가 던지는 것(실패 주입 계획)
#include <cstring>      // strlen (StmtDesc.sql 길이) · memcpy (열 표로 결과 복사)
#include <cstddef>      // offsetof (열 표 — 구조체 안의 멤버 자리)
#include <type_traits>  // is_same_v (저장·결과 구조체의 같은 열 멤버 타입 대조)
#include <chrono>

#include <Common/ErrorLog.h>
#include "CoreAffinity.h"
#include "GameMonitor.h"    // DBSaveCounters — 저장 지표의 그릇(게임 쪽). 코어 그릇(CMonitorManager)은 안 본다(10-06)

#pragma comment(lib, "libmysql.lib")

CDBWorker::CDBWorker(DBSaveCounters& counters)
    : _counters(counters)
{
}

CDBWorker::~CDBWorker()
{
    Shutdown(0);
}

bool CDBWorker::Start(const DBConfig& config, int workerCount, int queueMax)
{
    assert(workerCount > 0 && queueMax > 0);   // 1 이상인지는 CCharacterStore::Init 이 봤다(CGameServer::InitDB 가 넘긴다) — 여기에 기본값을 두지 않는다
    _config      = config;
    _workerCount = workerCount;
    if (_workerCount > DBSaveCounters::MAX_WORKERS)
        _workerCount = DBSaveCounters::MAX_WORKERS;   // 큐 깊이 게이지 배열 상한에 맞춤
    _queueMax    = queueMax;
#if USE_DB_FAULT_INJECT
    LoadFaultPlan();   // 시험 빌드 — 워커 스레드가 뜨기 전에 한 번
#endif

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

    _counters._workerCount.Store(_workerCount);   // 지표 노출 상한

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
#if USE_DB_FAULT_INJECT
    // DB 가 죽은 흉내(down 주입) — 남은 수만큼 접속을 실패시킨다. 기동(Start) 때는 0 이라 안 걸린다.
    {
        int left = _faultDownLeft.load();
        while (left > 0 && !_faultDownLeft.compare_exchange_weak(left, left - 1)) {}
        if (left > 0)
        {
            SLOG_WARN("[DB][fault-down] connect refused (left={})", left - 1);
            return false;
        }
    }
#endif
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

// 묶음 하나를 슬롯 큐에(락을 쥔 채) — 묶음끼리 센 수로 상한. 칸 줄은 큐 옆 줄 벡터 끝에 붙이고 시작 자리를 머리에 적는다.
bool CDBWorker::PushBundleLocked(DBWorkerSlot& slot, DBSaveBundle head, const DBInvRow* rows, int rowCount)
{
    assert(rowCount >= 0 && (rowCount == 0 || rows != nullptr));
#if !USE_INVENTORY
    assert(rowCount == 0 && "가방을 끈 빌드의 묶음은 칸 줄이 없다");
#endif
    if (slot.bundleCount >= static_cast<size_t>(_queueMax))
        return false;
    head.slotBegin = static_cast<uint32_t>(slot.rows.size());
    head.slotCount = rowCount;
    if (rowCount > 0)
        slot.rows.insert(slot.rows.end(), rows, rows + rowCount);
    slot.queue.push_back(DBJob::Bundle(head));
    ++slot.bundleCount;
    return true;
}

bool CDBWorker::EnqueueSave(const DBSaveBundle& head, const DBInvRow* rows, int rowCount)
{
    DBWorkerSlot& slot = *_workers[SlotIndex(head.row.accountId)];   // 불러오기·저장과 같은 슬롯 — 계정별 순서 보장

    bool taken;
    {
        std::lock_guard<std::mutex> lk(slot.mutex);
        taken = PushBundleLocked(slot, head, rows, rowCount);
    }
    if (!taken)
    {
        _counters._droppedJobs.Inc();
        SLOG_ERROR("[DB] backpressure drop — save bundle refused (worker={}, account={}, max={})",
                   slot.index, head.row.accountId, _queueMax);
        return false;
    }
    slot.cv.notify_one();
    return true;
}

bool CDBWorker::EnqueueSavePair(const DBSaveBundle& a, const DBInvRow* aRows, int aCount,
                                const DBSaveBundle& b, const DBInvRow* bRows, int bCount)
{
    assert(a.row.accountId != b.row.accountId && "짝은 두 캐릭터다");
    // 작은 계정 번호의 슬롯 — 같은 짝은 늘 같은 큐(헤더 주석)
    const int64_t route = (a.row.accountId < b.row.accountId) ? a.row.accountId : b.row.accountId;
    DBWorkerSlot& slot = *_workers[SlotIndex(route)];

    bool taken = false;
    {
        std::lock_guard<std::mutex> lk(slot.mutex);
        // 둘 다 들어갈 때만 — 반쪽 짝은 워커가 읽을 수 없다(첫 머리의 pairNext 가 없는 뒤 머리를 가리킨다)
        if (slot.bundleCount + 2 <= static_cast<size_t>(_queueMax))
        {
            DBSaveBundle first = a;  first.pairNext = 1;
            DBSaveBundle second = b; second.pairNext = 0;
            taken = PushBundleLocked(slot, first, aRows, aCount) && PushBundleLocked(slot, second, bRows, bCount);
            assert(taken && "상한을 위에서 봤다");
        }
    }
    if (!taken)
    {
        _counters._droppedJobs.Add(2);
        SLOG_ERROR("[DB] backpressure drop — save pair refused (worker={}, accounts={}·{}, max={})",
                   slot.index, a.row.accountId, b.row.accountId, _queueMax);
        return false;
    }
    slot.cv.notify_one();
    return true;
}

void CDBWorker::EnqueueSaveBatch(const std::vector<DBSaveBundle>& heads, const std::vector<DBInvRow>& allRows,
                                 std::vector<size_t>* outRefused)
{
    if (outRefused != nullptr)
        outRefused->clear();
    if (heads.empty())
        return;

    // 슬롯마다 락 1회 — 묶음 순서는 heads 순서 그대로(같은 계정 묶음은 한 배치에 하나지만, 순서를 섞지 않는다)
    for (int w = 0; w < _workerCount; ++w)
    {
        DBWorkerSlot& slot = *_workers[w];
        size_t taken = 0, refused = 0;
        {
            std::lock_guard<std::mutex> lk(slot.mutex);
            for (size_t i = 0; i < heads.size(); ++i)
            {
                const DBSaveBundle& h = heads[i];
                if (SlotIndex(h.row.accountId) != w)
                    continue;
                assert(static_cast<size_t>(h.slotBegin) + static_cast<size_t>(h.slotCount) <= allRows.size());
                const DBInvRow* rows = (h.slotCount > 0) ? allRows.data() + h.slotBegin : nullptr;
                if (PushBundleLocked(slot, h, rows, h.slotCount))
                    ++taken;
                else
                {
                    ++refused;
                    if (outRefused != nullptr)
                        outRefused->push_back(i);
                }
            }
        }
        if (taken > 0)
            slot.cv.notify_one();
        if (refused > 0)
        {
            _counters._droppedJobs.Add(static_cast<int64_t>(refused));
            SLOG_ERROR("[DB] backpressure drop — save bundles refused (worker={}, refused={}, max={})",
                       slot.index, refused, _queueMax);
        }
    }
}

void CDBWorker::PushResult(const DBResult& result)
{
    std::lock_guard<std::mutex> lk(_results.mutex);
    _results.items.push_back(result);
}

void CDBWorker::PushSaveReply(const DBSaveReply& reply)
{
    std::lock_guard<std::mutex> lk(_results.mutex);
    _results.saves.push_back(reply);
}

void CDBWorker::SwapOutResults(std::vector<DBResult>& loads, std::vector<DBSaveReply>& saves)
{
    std::lock_guard<std::mutex> lk(_results.mutex);
    loads.swap(_results.items);   // 둘 다 비어 있어야 한다 — 그 빈 벡터(용량 보존)가 회신 큐의 새 몸이 된다
    saves.swap(_results.saves);
}

void CDBWorker::WorkerThread(int idx)
{
    CoreAffinity::PinIoThread();   // DB 워커 → 게임코어 밖으로 (격리 off면 no-op)
    mysql_thread_init();

    DBWorkerSlot& slot = *_workers[idx];

    std::vector<DBJob> local;
    std::vector<DBInvRow> localRows;   // 저장 묶음의 칸 줄 — local 과 같이 맞바꾼다(묶음 머리의 시작 자리가 이 벡터를 가리킨다)
    while (true)
    {
        {
            std::unique_lock<std::mutex> lk(slot.mutex);
            slot.cv.wait(lk, [this, &slot] { return !slot.queue.empty() || _stop.load(); });
            if (slot.queue.empty() && _stop.load())
                break;                   // 정지 요청 + 잔여 없음 → 종료(드레인 완료)
            local.swap(slot.queue);       // 누적분 통째 인출 (여러 주기분 병합 가능)
            localRows.swap(slot.rows);    // 같은 락 안에서 — 묶음 머리와 칸 줄이 어긋나지 않게
            slot.loadCount = 0;           // 큐를 통째로 가져왔으므로 큐에 든 불러오기도 0 (EnqueueLoad 의 상한 기준)
            slot.bundleCount = 0;         // 같은 이유 — 저장 묶음 상한(EnqueueSave)의 기준
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

        _counters._queueDepth[idx].Store(static_cast<int64_t>(local.size()));  // 워커별 게이지

        static_assert(DB_JOB_KIND_COUNT == 2, "DBJobKind 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
        for (size_t j = 0; j < local.size(); ++j)
        {
            const DBJob& job = local[j];
            switch (job.kind)
            {
            case DBJobKind::LOAD_CHARACTER:
            {
                // 저장 지표(_counters._savedJobs·_failedJobs)는 건드리지 않는다 — 그 둘은 "저장"만 세는 기존 게이트다.
                //   불러오기는 결과에 코드만 실어 보내고, 세는 일은 게임 스레드가 결과를 꺼낼 때 한다.
                //   성공이든 실패든 결과는 반드시 하나 돌려준다 — 안 돌려주면 요청자가 영영 기다린다.
                DBResult result{};
                ExecLoad(slot, job.load, result, reconnectFailedThisBatch);
                PushResult(result);   // 묶음 끝까지 기다리지 않고 바로 — 뒤에 저장 수천 건이 있어도 입장이 안 밀린다
                break;
            }

            case DBJobKind::SAVE_BUNDLE:
            {
                // 묶음 하나 = 트랜잭션 하나. 회신은 묶음마다 곧바로 — 같은 계정의 뒤 불러오기 결과보다 먼저 결과 상자에 들어간다
                //   (받는 쪽이 회신을 먼저 처리해 불러오기 차례를 지킨다 — SwapOutResults 주석).
                //   짝 묶음(pairNext)이면 바로 뒤 머리까지 트랜잭션 하나로 — 둘은 한 락 안에 이어 넣었다(EnqueueSavePair).
                //   회신은 머리마다 하나씩(같은 성패) · 지표도 머리마다 센다(Σtxn = saved + failed = Σreply 등식이 머리 단위라서).
                const DBSaveBundle* heads[MAX_BUNDLE_HEADS] = { &job.bundle, nullptr };
                int n = 1;
                if (job.bundle.pairNext != 0)
                {
                    assert(j + 1 < local.size() && local[j + 1].kind == DBJobKind::SAVE_BUNDLE && local[j + 1].bundle.pairNext == 0
                           && "짝 묶음의 뒤 머리는 한 락 안에 바로 뒤에 넣었다");
                    heads[1] = &local[++j].bundle;
                    n = 2;
                }
                const DBInvRow* rows[MAX_BUNDLE_HEADS] = { nullptr, nullptr };
                for (int h = 0; h < n; ++h)
                {
                    assert(static_cast<size_t>(heads[h]->slotBegin) + static_cast<size_t>(heads[h]->slotCount) <= localRows.size());
                    rows[h] = (heads[h]->slotCount > 0) ? localRows.data() + heads[h]->slotBegin : nullptr;
                }
                const bool ok = ExecBundle(slot, heads, rows, n, reconnectFailedThisBatch);
                for (int h = 0; h < n; ++h)
                {
                    const DBSaveBundle& b = *heads[h];
                    if (ok)
                    {
                        _counters._savedJobs.Inc();
                        _counters._saveSlotRows.Add(b.slotCount);
                    }
                    else
                        _counters._failedJobs.Inc();
                    PushSaveReply(DBSaveReply{ b.row.accountId, b.sessionId, b.bundleNo, ok });
                }
                break;
            }

            case DBJobKind::COUNT:
                break;   // 값이 아니라 개수 표시 — 큐에 들어올 수 없다
            }
        }
        local.clear();
        localRows.clear();
    }

    mysql_thread_end();
}

// ==========================================================================
// 문장 표 — 워커 몸통(스레드·큐·재연결·분배)과 "무슨 문장을 돌리나"를 분리.
//   문장마다 SQL 하나 + 바인딩 함수 하나(DBStmt 순서). 작업 종류와 따로 센다 — 저장 묶음은 캐릭터 행 + 칸 문장을,
//   불러오기는 캐릭터 행 + 가방 문장을 쓴다. 새 문장이 필요하면 DBStmt 에 값을 넣고 여기에 한 줄
//   (빠뜨리면 아래 static_assert 가 컴파일을 막는다).
//   ※ 저장은 묶음 하나 = 트랜잭션 하나 · 회신 하나(저장 플랜 C — 예전 「손실 허용·무트랜잭션 레인」을 대신함).
//     실패를 회신으로 돌려받아 저장소가 표시를 되살린다 — 무손실 데이터(가방)도 이 길로 간다.
//     여러 캐릭터를 한 트랜잭션으로 써야 하는 기능(거래·우편)이 생기면 묶음 여럿을 한 잡으로.
//   ※ 둘째 종류(불러오기)가 들어오면서 큐 원소에 종류 표시(DBJob::kind)가 생겼다.
//     예고대로 "둘째 종류 도입 시점"에 넣은 것이고, 셋째부터는 같은 자리에 줄만 늘어난다.
// ==========================================================================
struct StmtDesc
{
    const char* name;                                      // 로그에 찍는 이름
    const char* sql;                                       // 이 문장
    bool (*bind)(MYSQL_STMT* st, DBWorkerSlot& slot);      // 파라미터(·결과) 버퍼 바인딩 — prepare 시 1회
};

// ── 행의 열 표 — characters · inventory─────────────────────────────────────────────────
//   테이블마다 표 하나 — 저장 SQL·불러오기 SQL·바인딩 배열·결과 복사가 전부 표에서 나온다. 열 하나 = 한 줄.
//   열을 늘리면 저장 구조체·결과 구조체에 같은 이름의 멤버를 넣고 표에 한 줄 + schema.sql 한 줄이 전부다.
//   SQL 의 ? 순서와 바인딩 배열 색인이 같은 배열을 같은 루프로 돌아 나오므로 둘이 어긋날 길이 없다
//   (전에는 SQL 글자·b[4]·r[3]·복사 줄을 손으로 맞췄다 — 개수가 같고 순서만 틀리면 아무도 못 잡았다).
//   MySQL 버퍼 타입은 손으로 안 적는다 — 멤버의 C++ 타입에서 MysqlType 이 정한다
//   (float→FLOAT, int32→LONG, int64→LONGLONG, DBBytes<N>→BLOB. 다른 타입의 멤버를 표에 올리면 컴파일이 막는다).
//   바이트 열(DBBytes)만 길이가 따로 있다 — BindColumn 이 buffer 를 data 에, length 를 len 에 대고 buffer_length 에 N 을 준다.
//   ※ account_id 는 표 밖이다 — 저장에선 VALUES 의 첫 ?, 불러오기에선 WHERE 의 ? 로 자리가 다르다.
//   ※ key = 기본 키의 일부인 열(가방의 slot) — ON DUPLICATE KEY UPDATE 쪽 ? 에서 뺀다.
template <class T> struct MysqlType;   // 정의 없음 — 표에 못 올리는 타입이면 여기서 막힌다
template <> struct MysqlType<float>   { static constexpr enum_field_types value = MYSQL_TYPE_FLOAT; };
template <> struct MysqlType<int32_t> { static constexpr enum_field_types value = MYSQL_TYPE_LONG; };
template <> struct MysqlType<int64_t> { static constexpr enum_field_types value = MYSQL_TYPE_LONGLONG; };
template <size_t N> struct MysqlType<DBBytes<N>> { static constexpr enum_field_types value = MYSQL_TYPE_BLOB; };

// 바이트 열이면 버퍼 크기 N, 고정폭 숫자면 0 — BindColumn 이 이 값으로 길이 자리를 붙일지 가른다.
template <class T> struct BytesCap { static constexpr unsigned long value = 0; };
template <size_t N> struct BytesCap<DBBytes<N>> { static constexpr unsigned long value = static_cast<unsigned long>(N); };

// DBBytes 의 data·len 자리는 N 과 무관하다(len 뒤에 바이트 배열) — BindColumn 이 DBBytes<1> 로 자리를 잰다.
static_assert(offsetof(DBBytes<1>, data) == offsetof(DBBytes<DB_SKILL_COOLDOWNS_BYTES>, data) &&
              offsetof(DBBytes<1>, data) == offsetof(DBBytes<DB_BUFFS_BYTES>, data), "DBBytes 의 data 자리가 N 에 따라 바뀌었다");

// 저장 구조체와 결과 구조체의 같은 이름 멤버는 타입이 같아야 한다 — 다르면 여기서 컴파일이 막힌다
template <class Save, class Load> constexpr size_t SameTypeSize()
{
    static_assert(std::is_same_v<Save, Load>, "저장 구조체와 결과 구조체의 같은 열 멤버는 타입이 같아야 한다");
    return sizeof(Save);
}

struct DbColumn
{
    const char*      name;      // DB 열 이름 — SQL 에 그대로 들어간다
    enum_field_types type;      // MYSQL_BIND::buffer_type
    size_t           saveOff;   // 저장 구조체 안의 자리 — 저장 파라미터 버퍼·불러오기 결과 버퍼 둘 다 이 꼴(슬롯의 bSave/bLoad·bItem/bInvLoad)
    size_t           loadOff;   // 결과 구조체 안의 자리 — 불러온 값을 게임 루프에 돌려줄 자리
    size_t           size;
    bool             key;       // true = 기본 키의 일부 — ON DUPLICATE KEY UPDATE 쪽 ? 에서 뺀다
    unsigned long    bytesCap;  // 0 = 고정폭 숫자 · N = 바이트 열(DBBytes<N>) — 길이 자리를 붙인다(BindColumn)
};

#define DB_COL(SaveT, LoadT, sqlName, member, isKey)                                           \
    { #sqlName, MysqlType<decltype(SaveT::member)>::value,                                     \
      offsetof(SaveT, member), offsetof(LoadT, member),                                        \
      SameTypeSize<decltype(SaveT::member), decltype(LoadT::member)>(), isKey,                 \
      BytesCap<decltype(SaveT::member)>::value }

// characters 행 — 저장 DBCharRow · 결과 DBResult
static constexpr DbColumn kCharColumns[] = {
    DB_COL(DBCharRow, DBResult, x,      x,     false),
    DB_COL(DBCharRow, DBResult, y,      y,     false),
    DB_COL(DBCharRow, DBResult, map_id, mapId, false),
    DB_COL(DBCharRow, DBResult, level,  level, false),
    DB_COL(DBCharRow, DBResult, exp,    exp,   false),
    DB_COL(DBCharRow, DBResult, hp,     hp,    false),
    DB_COL(DBCharRow, DBResult, mp,     mp,    false),
    DB_COL(DBCharRow, DBResult, skill_cooldowns, skillCooldowns, false),   // 바이트 열 — 형식은 CooldownBuffBlob.h
    DB_COL(DBCharRow, DBResult, buffs,           buffs,          false),
#if USE_INVENTORY
    // 가진 돈 — 가방 토글 안(아래 가방 열 표와 같은 토글). 가방을 끈 빌드는 이 열을 쓰지도 읽지도 않는다 — 0 으로 덮지 않게
    //   (체력·마력의 「모름」 값 같은 장치가 돈엔 없다 · npc-shop-plan.md 「DB」)
    DB_COL(DBCharRow, DBResult, money,           money,          false),
#endif
};
static constexpr int kCharColCount = static_cast<int>(sizeof(kCharColumns) / sizeof(kCharColumns[0]));

#if USE_INVENTORY
// inventory 행 — 저장 DBInvRowBuf · 결과 DBInvRow(한 줄). slot 은 기본 키 (account_id, slot) 의 일부라 UPDATE 에 안 들어간다
static constexpr DbColumn kInvColumns[] = {
    DB_COL(DBInvRowBuf, DBInvRow, slot,    slot,   true),
    DB_COL(DBInvRowBuf, DBInvRow, item_id, itemId, false),
    DB_COL(DBInvRowBuf, DBInvRow, count,   count,  false),
};
static constexpr int kInvColCount = static_cast<int>(sizeof(kInvColumns) / sizeof(kInvColumns[0]));
#endif
#undef DB_COL

static constexpr int kMaxCols = 10;  // 바인딩 배열 크기 — 표가 이보다 길어지면 아래 static_assert 가 막는다(10-21 돈 열로 9 → 10)
static_assert(kCharColCount <= kMaxCols, "kCharColumns 가 kMaxCols 를 넘었다");
#if USE_INVENTORY
static_assert(kInvColCount <= kMaxCols, "kInvColumns 가 kMaxCols 를 넘었다");
#endif

// 표에서 문장을 조립한다 — 프로그램 시작 때 한 번. kStmts 가 c_str() 를 들고 있으므로 수명은 정적.
//   저장: INSERT INTO <표> (account_id,열,…) VALUES (?,?,…) ON DUPLICATE KEY UPDATE 열=?,…(key 열은 뺀다)
//   불러오기: SELECT 열,… FROM <표> WHERE account_id=?<꼬리>
static std::string BuildUpsertSql(const char* table, const DbColumn* cols, int n)
{
    std::string names = "account_id", marks = "?", sets;
    for (int i = 0; i < n; ++i)
    {
        names += ','; names += cols[i].name;
        marks += ",?";
        if (cols[i].key)
            continue;
        if (!sets.empty()) sets += ',';
        sets += cols[i].name; sets += "=?";
    }
    return std::string("INSERT INTO ") + table + " (" + names + ") VALUES (" + marks + ") ON DUPLICATE KEY UPDATE " + sets;
}

static std::string BuildSelectSql(const char* table, const DbColumn* cols, int n, const char* tail)
{
    std::string names;
    for (int i = 0; i < n; ++i)
    {
        if (i > 0) names += ',';
        names += cols[i].name;
    }
    return "SELECT " + names + " FROM " + table + " WHERE account_id=?" + tail;
}

static const std::string kSaveSql = BuildUpsertSql("characters", kCharColumns, kCharColCount);
static const std::string kLoadSql = BuildSelectSql("characters", kCharColumns, kCharColCount, "");
#if USE_INVENTORY
static const std::string kItemSaveSql = BuildUpsertSql("inventory", kInvColumns, kInvColCount);
// 가방 불러오기 — 빈 칸(item_id 0)은 안 읽는다. 칸 순서로 받는다(줄이 넘칠 때 어느 줄을 버릴지가 늘 같게).
static const std::string kLoadInvSql  = BuildSelectSql("inventory", kInvColumns, kInvColCount, " AND item_id<>0 ORDER BY slot");
#endif

// 열 하나를 바인딩 칸에 댄다 — 저장 파라미터와 불러오기 결과가 같은 꼴이다(base = 저장 구조체 꼴의 슬롯 버퍼).
//   숫자 열은 멤버 자리 하나, 바이트 열은 data(buffer)·len(length)·N(buffer_length) 셋.
//   저장 때 MySQL 은 *length 만큼 보내고, 불러올 때는 *length 에 원래 길이를 적는다(N 보다 길면 잘림 — RunLoadOnce 주석).
static void BindColumn(MYSQL_BIND& b, void* base, const DbColumn& col)
{
    char* p = static_cast<char*>(base) + col.saveOff;
    b.buffer_type = col.type;
    if (col.bytesCap == 0)
    {
        b.buffer = p;
        return;
    }
    b.buffer        = p + offsetof(DBBytes<1>, data);
    b.buffer_length = col.bytesCap;
    b.length        = reinterpret_cast<unsigned long*>(p + offsetof(DBBytes<1>, len));
}

// 저장 파라미터 — ? 는 account_id 1개 + VALUES 의 열 n개 + UPDATE 의 열(key 제외). UPDATE 쪽은 VALUES 쪽과 같은 버퍼를
//   참조한다(값을 한 번만 복사하면 된다 — 바이트 열의 길이 자리도 같은 것을 가리킨다). saveBuf 는 저장 구조체 꼴의 슬롯 버퍼.
static bool BindUpsertParams(MYSQL_STMT* st, void* saveBuf, int64_t* accountIdBuf, const DbColumn* cols, int n)
{
    MYSQL_BIND b[1 + 2 * kMaxCols] = {};
    b[0].buffer_type = MYSQL_TYPE_LONGLONG; b[0].buffer = accountIdBuf;
    int k = 1 + n;   // UPDATE 쪽 ? 가 시작하는 자리
    for (int i = 0; i < n; ++i)
    {
        BindColumn(b[1 + i], saveBuf, cols[i]);
        if (!cols[i].key)
            b[k++] = b[1 + i];
    }
    return mysql_stmt_bind_param(st, b) == 0;
}

// 불러오기 — 파라미터는 account_id 1개, 결과는 표 순서 그대로 loadBuf(저장 구조체 꼴)의 멤버에 fetch 가 바로 채운다.
//   숫자 열은 NOT NULL 고정폭이고 열 타입과 버퍼 타입이 같아(FLOAT→float, INT→int32, BIGINT→int64) 변환·잘림이 없다.
//   바이트 열만 길이가 바뀐다 — 길이는 len 에 오고, 버퍼보다 길면 잘린다(RunLoadOnce 가 가른다).
//   is_null·error(와 숫자 열의 length)는 비워 두면 라이브러리가 내부 값으로 채운다.
static bool BindSelect(MYSQL_STMT* st, void* loadBuf, int64_t* accountIdBuf, const DbColumn* cols, int n)
{
    MYSQL_BIND p[1] = {};
    p[0].buffer_type = MYSQL_TYPE_LONGLONG; p[0].buffer = accountIdBuf;
    if (mysql_stmt_bind_param(st, p) != 0)
        return false;

    MYSQL_BIND r[kMaxCols] = {};
    for (int i = 0; i < n; ++i)
        BindColumn(r[i], loadBuf, cols[i]);
    return mysql_stmt_bind_result(st, r) == 0;
}

// fetch 가 채운 loadBuf 의 열을 결과 구조체 dst 로 — 멤버 이름이 같고 두 구조체 안의 자리만 다르다
static void CopyColumns(void* dst, const void* loadBuf, const DbColumn* cols, int n)
{
    for (int i = 0; i < n; ++i)
        memcpy(static_cast<char*>(dst) + cols[i].loadOff, static_cast<const char*>(loadBuf) + cols[i].saveOff, cols[i].size);
}

// ── 캐릭터 행 저장 ─────────────────────────────────────────────────────────────
static bool BindPositionParams(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    // ? = account_id + 열 n개 + UPDATE 열 n개 (key 열이 없는 표 — 지금 아홉 열이라 19개)
    return BindUpsertParams(st, &slot.bSave, &slot.bSave.accountId, kCharColumns, kCharColCount);
}

// ── 캐릭터 불러오기 ───────────────────────────────────────────────────────
static bool BindLoadCharacter(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    return BindSelect(st, &slot.bLoad, &slot.bLoad.accountId, kCharColumns, kCharColCount);
}

#if USE_INVENTORY
// ── 가방 칸 저장(저장 묶음의 칸 문장) ─────────────────────────────────────────────────────────
static bool BindItemSlotParams(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    // ? 6개 = account_id + slot·item_id·count + UPDATE item_id·count (slot 은 key 라 UPDATE 에 없다)
    return BindUpsertParams(st, &slot.bItem, &slot.bItem.accountId, kInvColumns, kInvColCount);
}

// ── 가방 불러오기 (불러오기 잡의 둘째 문장 — 작업 종류가 아니다) ──────────
//   문장은 열 표(kInvColumns)에서 조립한 kLoadInvSql — 빈 칸을 거르는 꼬리(item_id<>0 ORDER BY slot)는 거기 있다.
static bool BindLoadInventory(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    return BindSelect(st, &slot.bInvLoad, &slot.bInvLoad.accountId, kInvColumns, kInvColCount);
}
#endif

// DBStmt 순서 그대로 — 인덱스가 곧 문장이다. 문장은 열 표(kCharColumns · kInvColumns)에서 조립.
static const StmtDesc kStmts[] = {
    /* CHAR_UPSERT */ { "char_upsert", kSaveSql.c_str(),     BindPositionParams },
    /* CHAR_SELECT */ { "char_select", kLoadSql.c_str(),     BindLoadCharacter },
#if USE_INVENTORY
    /* SLOT_UPSERT */ { "slot_upsert", kItemSaveSql.c_str(), BindItemSlotParams },
    /* INV_SELECT  */ { "inv_select",  kLoadInvSql.c_str(),  BindLoadInventory },
#endif
};
static_assert(sizeof(kStmts) / sizeof(kStmts[0]) == DB_STMT_COUNT,
              "DBStmt 를 늘렸으면 kStmts 에도 같은 순서로 한 줄을 넣을 것");

static constexpr int kSaveIdx = static_cast<int>(DBStmt::CHAR_UPSERT);
static constexpr int kLoadIdx = static_cast<int>(DBStmt::CHAR_SELECT);
#if USE_INVENTORY
static constexpr int kItemIdx    = static_cast<int>(DBStmt::SLOT_UPSERT);
static constexpr int kInvLoadIdx = static_cast<int>(DBStmt::INV_SELECT);
#endif

// 문장 준비 실패 로그 — 테이블이 없으면(ER_NO_SUCH_TABLE) 할 일을 같이 적는다.
//   게임 계정에는 CREATE 권한이 없어 서버가 테이블을 만들 수 없다 — 사람이 root 로 한 번 돌려야 한다.
static void LogPrepareFailure(const char* name, MYSQL_STMT* st)
{
    if (mysql_stmt_errno(st) == ER_NO_SUCH_TABLE)
        SLOG_ERROR("[DB] stmt_prepare failed ({}): {} — root 로 MMOServer/MMOServer/DB/schema.sql 을 먼저 돌리세요 "
                   "(게임 계정은 테이블을 못 만든다)", name, mysql_stmt_error(st));
    else
        SLOG_ERROR("[DB] stmt_prepare failed ({}): {}", name, mysql_stmt_error(st));
}

bool CDBWorker::PrepareStmt(DBWorkerSlot& slot)
{
    MYSQL* m = static_cast<MYSQL*>(slot.mysql);

    // 문장마다 하나씩. 하나라도 실패하면 앞서 만든 것까지 전부 닫고 false —
    //   "저장은 되는데 불러오기만 안 되는" 반쪽 상태로 워커를 돌리지 않는다.
    for (int i = 0; i < DB_STMT_COUNT; ++i)
    {
        const StmtDesc& desc = kStmts[i];

        MYSQL_STMT* st = mysql_stmt_init(m);
        if (st == nullptr)
        {
            SLOG_ERROR("[DB] stmt_init failed ({}): {}", desc.name, mysql_error(m));
            CloseStmts(slot);
            return false;
        }

        if (mysql_stmt_prepare(st, desc.sql, static_cast<unsigned long>(strlen(desc.sql))) != 0)
        {
            LogPrepareFailure(desc.name, st);
            mysql_stmt_close(st);
            CloseStmts(slot);
            return false;
        }

        if (!desc.bind(st, slot))   // 문장별 바인딩(버퍼는 슬롯 멤버)
        {
            SLOG_ERROR("[DB] stmt_bind failed ({}): {}", desc.name, mysql_stmt_error(st));
            mysql_stmt_close(st);
            CloseStmts(slot);
            return false;
        }

        slot.stmt[i] = st;
    }
    return true;
}

void CDBWorker::CloseStmts(DBWorkerSlot& slot)
{
    for (int i = 0; i < DB_STMT_COUNT; ++i)
    {
        if (slot.stmt[i] != nullptr)
        {
            mysql_stmt_close(static_cast<MYSQL_STMT*>(slot.stmt[i]));
            slot.stmt[i] = nullptr;
        }
    }
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

CDBWorker::LoadOutcome CDBWorker::RunLoadOnce(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out)
{
    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmt[kLoadIdx]);
    slot.bLoad.accountId = job.accountId;

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
    case MYSQL_DATA_TRUNCATED:
        // 잘림은 바이트 열(쿨타임·버프)에서만 난다 — 숫자 열은 열 타입과 버퍼 타입이 같아 잘릴 수 없다(BindSelect 주석).
        //   바이트 열이 버퍼보다 길면 len 에 원래 길이가 남아 게임 스레드가 불량으로 버린다(SpawnPlayer) — 쿨타임·버프가 풀릴 뿐
        //   입장까지 막을 일이 아니다. 실패로 돌리면 그 계정은 행을 고치기 전까지 영영 못 들어온다.
        CopyColumns(&out, &slot.bLoad, kCharColumns, kCharColCount);   // 표의 열을 slot.bLoad 에서 out 으로
        outcome = LoadOutcome::FOUND;
        break;

    case MYSQL_NO_DATA:
        outcome = LoadOutcome::NOT_FOUND;
        break;

    default:   // 1(오류) — 버퍼 값을 믿을 수 없다
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

    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmt[kInvLoadIdx]);
    if (st == nullptr)
        return LoadOutcome::QUERY_ERROR;   // PrepareStmt 가 캐릭터 문장과 함께 세우므로 닿지 않는다 — 닿으면 실패로
    slot.bInvLoad.accountId = job.accountId;

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
            CopyColumns(&out.inv[out.invCount++], &slot.bInvLoad, kInvColumns, kInvColCount);   // 표의 열을 한 줄로
        else
            ++out.invOverflow;
    }
    mysql_stmt_free_result(st);
    return LoadOutcome::FOUND;   // 줄이 0 이어도 성공 — "가방이 비었다"는 사실이다
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
    out.hp        = DB_HP_MP_UNSAVED;   // 새 캐릭터 = 가득(0 이면 죽은 채로 읽힌다)
    out.mp        = DB_HP_MP_UNSAVED;
    out.skillCooldowns.len = 0;   // 새 캐릭터·못 읽음 = 돌고 있는 것 없음
    out.buffs.len          = 0;
    out.money              = 0;   // 새 캐릭터 = 빈손(돈 열은 가방을 켠 빌드만 읽는다 — 열 표 kCharColumns)
#if USE_INVENTORY
    out.invCount    = 0;   // 가방 줄 — RunInventoryLoad 가 채운다(실패면 code 가 FAILED 라 안 쓰인다)
    out.invOverflow = 0;
#endif

    // 읽기는 멱등이라 두 번 해도 안전하다 → 커넥션이 없거나 도중에 죽으면 그 자리에서 재연결해 본다.
    //   노는 커넥션은 MySQL 이 끊는다(wait_timeout). 서버를 밤새 켜 두면 아침 첫 접속이 여기에 걸리는데,
    //   다시 해 보지 않으면 그 사람이 영문 없이 튕긴다. 저장 묶음(ExecBundle)도 같은 규칙이다.
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

// ==========================================================================
// 저장 묶음 — 묶음 하나 = 트랜잭션 하나 (save-unify-plan.md 「워커」)
//   칸이 0줄이면 캐릭터 행 문장 하나 — 자동 커밋 아래서 그 자체가 트랜잭션이라 START/COMMIT 을 안 보낸다(주기 묶음의 대부분).
//   칸이 1줄 이상이면 START TRANSACTION → 캐릭터 행 UPSERT → 칸 UPSERT × N → COMMIT. 칸 줄은 최종값이라 두 번 써도 같다.
//   워커는 주인을 모른다 — 성공·실패를 회신할 뿐, 다시 싣기는 저장소가 정한다.
// ==========================================================================
static bool IsConnectionLostErrno(unsigned int err)
{
    return err == CR_SERVER_GONE_ERROR || err == CR_SERVER_LOST;
}

#if USE_DB_FAULT_INJECT
// 이 묶음에 걸린 주입(문장 뒤 오류·끊김)이 아직 안 쓰였나 — 워커 스레드마다 따로(묶음 처리 한 번 동안만 산다)
static thread_local bool t_faultArmed = false;
#endif

CDBWorker::SaveOutcome CDBWorker::RunBundleOnce(DBWorkerSlot& slot, const DBSaveBundle* const* heads, const DBInvRow* const* rows, int n)
{
    assert(n >= 1 && n <= MAX_BUNDLE_HEADS);
    MYSQL* m = static_cast<MYSQL*>(slot.mysql);
    const DBSaveBundle& b = *heads[0];   // 로그의 머리 — 짝이면 둘째 계정도 같이 적는다(아래 fail)
    // 짝이면 칸이 0줄이어도 트랜잭션 — 문장이 둘 이상이라 자동 커밋이면 반쪽이 남는다
    const bool txn = (n > 1 || b.slotCount > 0);
    const int64_t pairAccount = (n > 1) ? heads[1]->row.accountId : 0;

    // 실패 가르기 — 끊김이면 커넥션을 버린다(열린 트랜잭션은 DB 가 되돌린다). 그 밖이면 ROLLBACK,
    //   그것도 안 되면 커넥션을 버린다 — 다음 START TRANSACTION 은 열린 트랜잭션을 말없이 커밋하므로 반쪽 묶음을 들고 다음 잡으로 가지 않는다.
    auto fail = [&](unsigned int err, const char* what, const char* msg) -> SaveOutcome
    {
        if (IsConnectionLostErrno(err))
        {
            SLOG_WARN("[DB] connection lost during save bundle {} (errno={}, account={})", what, err, b.row.accountId);
            DropConnection(slot);
            return SaveOutcome::CONNECTION_LOST;
        }
        SLOG_ERROR("[DB] save bundle {} failed (account={}, pair={}, bundle={}): {}", what, b.row.accountId, pairAccount, b.bundleNo, msg);
        if (txn && mysql_rollback(m) != 0)
        {
            SLOG_ERROR("[DB] rollback failed -> drop connection (account={}): {}", b.row.accountId, mysql_error(m));
            DropConnection(slot);
        }
        return SaveOutcome::QUERY_ERROR;
    };

#if USE_DB_FAULT_INJECT
    // k 번째 문장 뒤 — 오류면 위 실패 가르기를 그대로 타고(ROLLBACK), 끊김이면 커넥션을 버린다(진짜 끊김과 같은 뒤처리)
    auto injectAfter = [&](int stmtNo, SaveOutcome& out) -> bool
    {
        if (!t_faultArmed || stmtNo != _faultArg)
            return false;
        t_faultArmed = false;   // 한 묶음에 한 번 — 재연결 뒤 다시 하는 판에는 안 건다
        if (_faultKind == FaultKind::ERROR_AFTER)
        {
            SLOG_WARN("[DB][fault] trigger error@{} (account={}, bundle={})", stmtNo, b.row.accountId, b.bundleNo);
            out = fail(0, "fault", "injected error");
        }
        else
        {
            SLOG_WARN("[DB][fault] trigger lost@{} (account={}, bundle={})", stmtNo, b.row.accountId, b.bundleNo);
            DropConnection(slot);
            out = SaveOutcome::CONNECTION_LOST;
        }
        return true;
    };
    SaveOutcome injected = SaveOutcome::OK;
#endif

    if (txn && mysql_real_query(m, "START TRANSACTION", static_cast<unsigned long>(strlen("START TRANSACTION"))) != 0)
        return fail(mysql_errno(m), "start", mysql_error(m));

    // 머리마다 — 캐릭터 행(지금 행 저장과 같은 문장 · 같은 버퍼) → 칸 문장 × N. 문장 번호는 머리를 넘어 이어 센다
    //   (주입 error@k · lost@k 의 k — 짝이면 A 의 문장 뒤 B 의 문장이다. 짝 가운데서 끊는 시험이 그 번호로 고른다)
    int stmtNo = 0;
    for (int h = 0; h < n; ++h)
    {
        const DBSaveBundle& hb = *heads[h];
        {
            MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmt[kSaveIdx]);
            slot.bSave = hb.row;
            if (mysql_stmt_execute(st) != 0)
                return fail(mysql_stmt_errno(st), "char row", mysql_stmt_error(st));
            ++stmtNo;
#if USE_DB_FAULT_INJECT
            if (injectAfter(stmtNo, injected))
                return injected;
#endif
        }

#if USE_INVENTORY
        // 칸 문장 × N — 칸마다 최종값(빈 칸은 item 0 으로 덮는다 — DB 계정에 DELETE 권한이 없다)
        MYSQL_STMT* stItem = static_cast<MYSQL_STMT*>(slot.stmt[kItemIdx]);
        const DBInvRow* hr = rows[h];
        for (int i = 0; i < hb.slotCount; ++i)
        {
            slot.bItem = DBInvRowBuf{ hb.row.accountId, hr[i].slot, hr[i].itemId, hr[i].count };
            if (mysql_stmt_execute(stItem) != 0)
                return fail(mysql_stmt_errno(stItem), "slot row", mysql_stmt_error(stItem));
            ++stmtNo;
#if USE_DB_FAULT_INJECT
            if (injectAfter(stmtNo, injected))
                return injected;
#endif
        }
#else
        (void)rows;
#endif
    }

    // COMMIT 도중 끊기면 됐는지 모른다 — 끊김으로 돌려 묶음째 다시 쓴다(최종값이라 두 번 써도 같다)
    if (txn && mysql_commit(m) != 0)
        return fail(mysql_errno(m), "commit", mysql_error(m));
    return SaveOutcome::OK;
}

bool CDBWorker::ExecBundle(DBWorkerSlot& slot, const DBSaveBundle* const* heads, const DBInvRow* const* rows, int n, bool& reconnectFailedThisBatch)
{
    const DBSaveBundle& b = *heads[0];   // 로그·주입의 머리 — 짝이면 둘 중 하나라도 주입 계정이면 건다
#if USE_DB_FAULT_INJECT
    t_faultArmed = TakeFault(b.row.accountId) || (n > 1 && TakeFault(heads[1]->row.accountId));
    if (t_faultArmed && _faultKind == FaultKind::DELAY)
    {
        SLOG_WARN("[DB][fault] trigger delay {}ms (account={}, bundle={})", _faultArg, b.row.accountId, b.bundleNo);
        t_faultArmed = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(_faultArg));
    }
    else if (t_faultArmed && _faultKind == FaultKind::DOWN)
    {
        // DB 가 죽은 흉내 — 커넥션을 버리고 다음 재연결 n 번을 실패시킨다(Connect 가 깎는다)
        SLOG_WARN("[DB][fault] trigger down x{} (account={}, bundle={})", _faultArg, b.row.accountId, b.bundleNo);
        t_faultArmed = false;
        DropConnection(slot);
        _faultDownLeft.store(_faultArg);
    }
#endif

    // 불러오기와 같은 재연결 규칙(ExecLoad 와 같은 꼴) — 없으면 그 자리 재연결, 도중에 끊기면 재연결 뒤 묶음째 한 번 더.
    //   이 묶음 처리에서 재연결이 이미 실패했으면 곧바로 실패로(죽은 DB 에 묶음마다 접속 제한 시간을 쓰지 않는다 — TryReconnectForLoad).
    bool retried = false;
    SaveOutcome r = SaveOutcome::CONNECTION_LOST;

    if (slot.mysql == nullptr || slot.stmt[kSaveIdx] == nullptr)
    {
        DropConnection(slot);
        if (TryReconnectForLoad(slot, reconnectFailedThisBatch))
        {
            retried = true;
            r = RunBundleOnce(slot, heads, rows, n);
        }
    }
    else
    {
        r = RunBundleOnce(slot, heads, rows, n);
        if (r == SaveOutcome::CONNECTION_LOST)
        {
            DropConnection(slot);   // 실패 가르기가 이미 버렸다 — 짝을 맞춰 비운다(멱등)
            if (TryReconnectForLoad(slot, reconnectFailedThisBatch))
            {
                retried = true;
                r = RunBundleOnce(slot, heads, rows, n);
            }
        }
    }
    if (r == SaveOutcome::CONNECTION_LOST)
        DropConnection(slot);   // 다시 해 본 것까지 끊겼다 — 다음 묶음 처리 머리가 다시 붙는다
#if USE_DB_FAULT_INJECT
    t_faultArmed = false;
#endif

    if (retried)
        _counters._saveRetried.Inc();
    if (r != SaveOutcome::OK)
        SLOG_ERROR("[DB] save bundle FAILED (account={}, session={}, bundle={}, rows={}, pair={}) — 저장소가 다음 계기에 다시 싣는다",
                   b.row.accountId, b.sessionId, b.bundleNo, b.slotCount, (n > 1) ? heads[1]->row.accountId : 0);
    return r == SaveOutcome::OK;
}

#if USE_DB_FAULT_INJECT
// 실패 주입 계획 — 환경 변수 MMO_DB_FAULT 한 줄("<종류>:<값>,account=<계정>,times=<횟수>"). 없거나 못 읽으면 주입 없음.
void CDBWorker::LoadFaultPlan()
{
    std::string spec;
#ifdef _WIN32
    char* raw = nullptr;
    size_t len = 0;
    if (_dupenv_s(&raw, &len, "MMO_DB_FAULT") != 0 || raw == nullptr)
        return;
    spec = raw;
    free(raw);
#else
    const char* raw = std::getenv("MMO_DB_FAULT");
    if (raw == nullptr)
        return;
    spec = raw;
#endif
    try
    {
        int64_t account = 0;
        int times = 1;
        FaultKind kind = FaultKind::NONE;
        int arg = 0;
        size_t pos = 0;
        while (pos <= spec.size())
        {
            const size_t end = spec.find(',', pos);
            const std::string tok = spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            const size_t sep = tok.find_first_of(":=");
            if (sep != std::string::npos)
            {
                const std::string key = tok.substr(0, sep), val = tok.substr(sep + 1);
                if      (key == "error")   { kind = FaultKind::ERROR_AFTER; arg = std::stoi(val); }
                else if (key == "lost")    { kind = FaultKind::LOST_AFTER;  arg = std::stoi(val); }
                else if (key == "down")    { kind = FaultKind::DOWN;        arg = std::stoi(val); }
                else if (key == "delay")   { kind = FaultKind::DELAY;       arg = std::stoi(val); }
                else if (key == "account") { account = std::stoll(val); }
                else if (key == "times")   { times = std::stoi(val); }
            }
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
        _faultKind = kind;
        _faultArg = arg;
        _faultAccount = account;
        _faultTimesLeft.store(times);
        SLOG_WARN("[DB][fault] plan loaded: {} (kind={}, arg={}, account={}, times={})",
                  spec, static_cast<int>(kind), arg, account, times);
    }
    catch (const std::exception& e)
    {
        SLOG_ERROR("[DB][fault] bad MMO_DB_FAULT '{}': {} — 주입 없음", spec, e.what());
        _faultKind = FaultKind::NONE;
    }
}

bool CDBWorker::TakeFault(int64_t accountId)
{
    if (_faultKind == FaultKind::NONE || accountId != _faultAccount)
        return false;
    int left = _faultTimesLeft.load();
    while (left > 0 && !_faultTimesLeft.compare_exchange_weak(left, left - 1)) {}
    return left > 0;
}
#endif

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
