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
#include <cstring>      // strlen (JobKindDesc.sql 길이) · memcpy (열 표로 결과 복사)
#include <cstddef>      // offsetof (열 표 — 구조체 안의 멤버 자리)
#include <type_traits>  // is_same_v (저장·결과 구조체의 같은 열 멤버 타입 대조)
#include <chrono>

#include <Common/ErrorLog.h>
#include "CoreAffinity.h"
#include "GameMonitor.h"    // DBSaveCounters · DBItemSaveCounters — 저장 지표의 그릇(게임 쪽). 코어 그릇(CMonitorManager)은 안 본다(10-06)

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
        _counters._droppedJobs.Add(static_cast<int64_t>(dropped));
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

        _counters._queueDepth[idx].Store(static_cast<int64_t>(local.size()));  // 워커별 게이지

        static_assert(DB_JOB_KIND_COUNT == (USE_INVENTORY ? 3 : 2), "DBJobKind 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
        for (const DBJob& job : local)
        {
            switch (job.kind)
            {
            case DBJobKind::SAVE_POSITION:
                if (ExecSave(slot, job.save))   // 유실 시 ExecSave가 stmt/mysql을 null로 만듦
                    _counters._savedJobs.Inc();
                else
                    _counters._failedJobs.Inc();
                break;

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

#if USE_INVENTORY
            case DBJobKind::SAVE_ITEM_SLOT:
                // 저장 지표(_counters._savedJobs·_failedJobs)는 "위치 저장"만 센다 — 아이템은 _itemCounters 로 따로(ExecItemSave)
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

// characters 행 — 저장 DBSaveJob · 결과 DBResult
static constexpr DbColumn kCharColumns[] = {
    DB_COL(DBSaveJob, DBResult, x,      x,     false),
    DB_COL(DBSaveJob, DBResult, y,      y,     false),
    DB_COL(DBSaveJob, DBResult, map_id, mapId, false),
    DB_COL(DBSaveJob, DBResult, level,  level, false),
    DB_COL(DBSaveJob, DBResult, exp,    exp,   false),
    DB_COL(DBSaveJob, DBResult, hp,     hp,    false),
    DB_COL(DBSaveJob, DBResult, mp,     mp,    false),
    DB_COL(DBSaveJob, DBResult, skill_cooldowns, skillCooldowns, false),   // 바이트 열 — 형식은 CooldownBuffBlob.h
    DB_COL(DBSaveJob, DBResult, buffs,           buffs,          false),
};
static constexpr int kCharColCount = static_cast<int>(sizeof(kCharColumns) / sizeof(kCharColumns[0]));

#if USE_INVENTORY
// inventory 행 — 저장 DBItemSlotJob · 결과 DBInvRow(한 줄). slot 은 기본 키 (account_id, slot) 의 일부라 UPDATE 에 안 들어간다
static constexpr DbColumn kInvColumns[] = {
    DB_COL(DBItemSlotJob, DBInvRow, slot,    slot,   true),
    DB_COL(DBItemSlotJob, DBInvRow, item_id, itemId, false),
    DB_COL(DBItemSlotJob, DBInvRow, count,   count,  false),
};
static constexpr int kInvColCount = static_cast<int>(sizeof(kInvColumns) / sizeof(kInvColumns[0]));
#endif
#undef DB_COL

static constexpr int kMaxCols = 9;   // 바인딩 배열 크기 — 표가 이보다 길어지면 아래 static_assert 가 막는다
static_assert(kCharColCount <= kMaxCols, "kCharColumns 가 kMaxCols 를 넘었다");
#if USE_INVENTORY
static_assert(kInvColCount <= kMaxCols, "kInvColumns 가 kMaxCols 를 넘었다");
#endif

// 표에서 문장을 조립한다 — 프로그램 시작 때 한 번. kKinds 가 c_str() 를 들고 있으므로 수명은 정적.
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

// ── 위치 저장 ─────────────────────────────────────────────────────────────
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
// ── 가방 칸 저장 ─────────────────────────────────────────────────────────
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

// DBJobKind 순서 그대로 — 인덱스가 곧 종류다.
static const JobKindDesc kKinds[] = {
    /* SAVE_POSITION  */ { "save_position",  kSaveSql.c_str(), BindPositionParams },   // 문장은 열 표(kCharColumns)에서 조립
    /* LOAD_CHARACTER */ { "load_character", kLoadSql.c_str(), BindLoadCharacter },
#if USE_INVENTORY
    /* SAVE_ITEM_SLOT */ { "save_item_slot", kItemSaveSql.c_str(), BindItemSlotParams },   // 열 표 kInvColumns
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
        if (mysql_stmt_prepare(st, kLoadInvSql.c_str(), static_cast<unsigned long>(kLoadInvSql.size())) != 0)
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

    // 바인딩 버퍼에 이번 잡 값 복사 (stmt는 이 버퍼의 멤버 주소를 이미 가리킴 → execute만 하면 됨)
    slot.bSave = job;

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

    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmtLoadInv);
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

CDBWorker::LoadOutcome CDBWorker::RunItemSaveOnce(DBWorkerSlot& slot, const DBItemSlotJob& job)
{
    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmt[kItemIdx]);
    slot.bItem = job;   // 잡과 버퍼가 같은 꼴이라 통째로 (stmt 는 이 버퍼의 멤버 주소를 이미 가리킨다)
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
    out.hp        = DB_HP_MP_UNSAVED;   // 새 캐릭터 = 가득(0 이면 죽은 채로 읽힌다)
    out.mp        = DB_HP_MP_UNSAVED;
    out.skillCooldowns.len = 0;   // 새 캐릭터·못 읽음 = 돌고 있는 것 없음
    out.buffs.len          = 0;
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
