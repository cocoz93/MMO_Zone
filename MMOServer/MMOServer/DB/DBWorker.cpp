#include "DBWorker.h"

#if USE_DB_WORKER

#ifdef _WIN32
#include <winsock2.h>   // mysql.h 이전에 — 구버전 winsock 자동 include 충돌 차단
#endif
#include <mysql.h>
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

bool CDBWorker::Start(const DBConfig& config, int workerCount, int queueMax, int reconnectMinSec, ZoneServerId zoneServerId)
{
    assert(workerCount > 0 && queueMax > 0);   // 1 이상인지는 CCharacterStore::Init 이 봤다(CGameServer::InitDB 가 넘긴다) — 여기에 기본값을 두지 않는다
    _config      = config;
    _workerCount = workerCount;
    if (_workerCount > DBSaveCounters::MAX_WORKERS)
        _workerCount = DBSaveCounters::MAX_WORKERS;   // 큐 깊이 게이지 배열 상한에 맞춤
    _queueMax    = queueMax;
    _zoneServerId = zoneServerId;
    _reconnectMin = std::chrono::seconds(reconnectMinSec);
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
    void* opened = CDbConnection::OpenRaw(_config);
    if (opened == nullptr)
        return false;
    outMysql = opened;
    return true;
}

bool CDBWorker::EnqueueLoad(const DBLoadJob& job)
{
    DBWorkerSlot& slot = *_workers[SlotIndex(job.characterId)];   // 저장과 같은 슬롯 — 캐릭터별 순서 보장

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
//   우편 문장(mail)도 같은 꼴 — 우편 문장 벡터 끝에 붙이고 자리를 머리에 적는다.
bool CDBWorker::PushBundleLocked(DBWorkerSlot& slot, DBSaveBundle head, const DBInvRow* rows, int rowCount, const DBMailStmt* mail)
{
    assert(rowCount >= 0 && (rowCount == 0 || rows != nullptr));
    assert((head.parts != 0 || rowCount > 0) && "실을 덩어리가 하나도 없는 묶음 — 만드는 자리(PrepareBundle · LeaveFinal)가 parts 를 정한다");
#if !USE_INVENTORY
    assert(rowCount == 0 && "가방을 끈 빌드의 묶음은 칸 줄이 없다");
#endif
#if !USE_MAIL
    assert(mail == nullptr && "우편을 끈 빌드의 묶음은 우편 문장이 없다");
#endif
    if (slot.bundleCount >= static_cast<size_t>(_queueMax))
        return false;
    head.slotBegin = static_cast<uint32_t>(slot.rows.size());
    head.slotCount = rowCount;
    if (rowCount > 0)
        slot.rows.insert(slot.rows.end(), rows, rows + rowCount);
    head.mailBegin = static_cast<uint32_t>(slot.mails.size());
    head.mailCount = (mail != nullptr) ? 1 : 0;
    if (mail != nullptr)
        slot.mails.push_back(*mail);
    slot.queue.push_back(DBJob::Bundle(head));
    ++slot.bundleCount;
    return true;
}

bool CDBWorker::EnqueueSave(const DBSaveBundle& head, const DBInvRow* rows, int rowCount, const DBMailStmt* mail)
{
    DBWorkerSlot& slot = *_workers[SlotIndex(CharacterId{ head.row.characterId })];   // 불러오기·저장과 같은 슬롯 — 캐릭터별 순서 보장

    bool taken;
    {
        std::lock_guard<std::mutex> lk(slot.mutex);
        taken = PushBundleLocked(slot, head, rows, rowCount, mail);
    }
    if (!taken)
    {
        _counters._droppedJobs.Inc();
        SLOG_ERROR("[DB] backpressure drop — save bundle refused (worker={}, character={}, max={})",
                   slot.index, head.row.characterId, _queueMax);
        return false;
    }
    slot.cv.notify_one();
    return true;
}

bool CDBWorker::EnqueueSavePair(const DBSaveBundle& a, const DBInvRow* aRows, int aCount,
                                const DBSaveBundle& b, const DBInvRow* bRows, int bCount)
{
    assert(a.row.characterId != b.row.characterId && "짝은 두 캐릭터다");
    // 작은 캐릭터 번호의 슬롯 — 같은 짝은 늘 같은 큐(헤더 주석)
    const int64_t route = (a.row.characterId < b.row.characterId) ? a.row.characterId : b.row.characterId;
    DBWorkerSlot& slot = *_workers[SlotIndex(CharacterId{ route })];

    bool taken = false;
    {
        std::lock_guard<std::mutex> lk(slot.mutex);
        // 둘 다 들어갈 때만 — 반쪽 짝은 워커가 읽을 수 없다(첫 머리의 pairNext 가 없는 뒤 머리를 가리킨다)
        if (slot.bundleCount + 2 <= static_cast<size_t>(_queueMax))
        {
            DBSaveBundle first = a;  first.pairNext = 1;
            DBSaveBundle second = b; second.pairNext = 0;
            // 짝 묶음은 우편 문장을 싣지 않는다 — 우편 동작 중인 사람은 거래를 못 연다(mail-plan.md 5절 · CTradeSystem::Busy)
            taken = PushBundleLocked(slot, first, aRows, aCount, nullptr) && PushBundleLocked(slot, second, bRows, bCount, nullptr);
            assert(taken && "상한을 위에서 봤다");
        }
    }
    if (!taken)
    {
        _counters._droppedJobs.Add(2);
        SLOG_ERROR("[DB] backpressure drop — save pair refused (worker={}, characters={}·{}, max={})",
                   slot.index, a.row.characterId, b.row.characterId, _queueMax);
        return false;
    }
    slot.cv.notify_one();
    return true;
}

void CDBWorker::EnqueueSaveBatch(const std::vector<DBSaveBundle>& heads, const std::vector<DBInvRow>& allRows,
                                 const std::vector<DBMailStmt>& allMails, std::vector<size_t>* outRefused)
{
    if (outRefused != nullptr)
        outRefused->clear();
    if (heads.empty())
        return;

    // 슬롯마다 락 1회 — 묶음 순서는 heads 순서 그대로(같은 캐릭터 묶음은 한 배치에 하나지만, 순서를 섞지 않는다)
    for (int w = 0; w < _workerCount; ++w)
    {
        DBWorkerSlot& slot = *_workers[w];
        size_t taken = 0, refused = 0;
        {
            std::lock_guard<std::mutex> lk(slot.mutex);
            for (size_t i = 0; i < heads.size(); ++i)
            {
                const DBSaveBundle& h = heads[i];
                if (SlotIndex(CharacterId{ h.row.characterId }) != w)
                    continue;
                assert(static_cast<size_t>(h.slotBegin) + static_cast<size_t>(h.slotCount) <= allRows.size());
                const DBInvRow* rows = (h.slotCount > 0) ? allRows.data() + h.slotBegin : nullptr;
                assert(h.mailCount <= 1 && (h.mailCount == 0 || static_cast<size_t>(h.mailBegin) < allMails.size()));
                const DBMailStmt* mail = (h.mailCount > 0) ? &allMails[h.mailBegin] : nullptr;
                if (PushBundleLocked(slot, h, rows, h.slotCount, mail))
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

void CDBWorker::PushMailReply(DBMailReply&& reply)
{
    std::lock_guard<std::mutex> lk(_results.mutex);
    _results.mails.push_back(std::move(reply));
}

bool CDBWorker::EnqueueMail(const DBMailJob& job)
{
#if !USE_MAIL
    (void)job;
    SLOG_ERROR("[DB] mail job in a build without USE_MAIL — refused");
    return false;
#else
    DBWorkerSlot& slot = *_workers[SlotIndex(job.characterId)];   // 그 사람의 저장·불러오기와 같은 슬롯 — 앞선 저장 뒤에 선다

    {
        std::lock_guard<std::mutex> lk(slot.mutex);
        // 불러오기처럼 잡끼리 센 수로 같은 상한 — swap-out 때 0 으로 되돌아간다(WorkerThread)
        if (slot.mailJobCount >= static_cast<size_t>(_queueMax))
        {
            SLOG_ERROR("[DB] backpressure — mail job refused (worker={}, character={}, op={}, max={})",
                       slot.index, job.characterId, static_cast<int>(job.op), _queueMax);
            return false;
        }
        slot.queue.push_back(DBJob::Mail(job));
        ++slot.mailJobCount;
    }

    slot.cv.notify_one();
    return true;
#endif
}

void CDBWorker::SwapOutResults(std::vector<DBResult>& loads, std::vector<DBSaveReply>& saves, std::vector<DBMailReply>& mails)
{
    std::lock_guard<std::mutex> lk(_results.mutex);
    loads.swap(_results.items);   // 셋 다 비어 있어야 한다 — 그 빈 벡터(용량 보존)가 회신 큐의 새 몸이 된다
    saves.swap(_results.saves);
    mails.swap(_results.mails);
}

void CDBWorker::WorkerThread(int idx)
{
    CoreAffinity::PinIoThread();   // DB 워커 → 게임코어 밖으로 (격리 off면 no-op)
    mysql_thread_init();

    DBWorkerSlot& slot = *_workers[idx];

    std::vector<DBJob> local;
    std::vector<DBInvRow> localRows;   // 저장 묶음의 칸 줄 — local 과 같이 맞바꾼다(묶음 머리의 시작 자리가 이 벡터를 가리킨다)
    std::vector<DBMailStmt> localMails;   // 저장 묶음의 우편 문장 — 같은 꼴(묶음 머리의 mailBegin)
    while (true)
    {
        {
            std::unique_lock<std::mutex> lk(slot.mutex);
            slot.cv.wait(lk, [this, &slot] { return !slot.queue.empty() || _stop.load(); });
            if (slot.queue.empty() && _stop.load())
                break;                   // 정지 요청 + 잔여 없음 → 종료(드레인 완료)
            local.swap(slot.queue);       // 누적분 통째 인출 (여러 주기분 병합 가능)
            localRows.swap(slot.rows);    // 같은 락 안에서 — 묶음 머리와 칸 줄이 어긋나지 않게
            localMails.swap(slot.mails);  // 우편 문장도 같은 까닭
            slot.loadCount = 0;           // 큐를 통째로 가져왔으므로 큐에 든 불러오기도 0 (EnqueueLoad 의 상한 기준)
            slot.bundleCount = 0;         // 같은 이유 — 저장 묶음 상한(EnqueueSave)의 기준
            slot.mailJobCount = 0;        // 같은 이유 — 우편 잡 상한(EnqueueMail)의 기준
        }

        // 배치 처리 전 — 커넥션이 끊겨 있으면 재연결 1회 시도.
        //   저장을 틱마다 나눠 실어 배치가 틱마다 오므로, 죽은 DB 는 ReconnectMinSec 에 한 번만 두드린다(world-server-plan.md 5.8).
        if (slot.mysql == nullptr && ReconnectDue(slot))
        {
            void* fresh = nullptr;
            if (Connect(fresh))
            {
                slot.mysql = fresh;
                if (PrepareStmt(slot))
                {
                    slot.lastFailedConnect = {};
                    SLOG_INFO("[DB] worker {} reconnected", idx);
                }
                else
                {
                    mysql_close(static_cast<MYSQL*>(slot.mysql));   // stmt 준비 실패 → 커넥션 롤백
                    slot.mysql = nullptr;
                    slot.lastFailedConnect = std::chrono::steady_clock::now();
                }
            }
            else
                slot.lastFailedConnect = std::chrono::steady_clock::now();
            // 실패하면 이번 배치는 전부 실패 처리되고, 간격이 지난 뒤 배치에서 다시 시도
        }

        // 이 묶음에서 재연결이 "실패한 적 있나". 위 머리 재연결을 했는데도 커넥션이 없으면 실패한 것이다.
        //   불러오기가 커넥션 없는 상태를 만났을 때, 그 자리에서 한 번 살려 볼지를 이 값으로 정한다(ExecLoad).
        //   머리에서 재연결을 "안 한" 경우(핸들은 살아 있었는데 서버 쪽에서 끊긴 커넥션)를 가려내려는 것이다 —
        //   그 커넥션을 앞선 저장이 먼저 밟아 내리면, 뒤의 불러오기는 아무도 재연결을 안 해 본 상태로 온다.
        bool reconnectFailedThisBatch = (slot.mysql == nullptr);

        _counters._queueDepth[idx].Store(static_cast<int64_t>(local.size()));  // 워커별 게이지

        static_assert(DB_JOB_KIND_COUNT == 3, "DBJobKind 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
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
                // 묶음 하나 = 트랜잭션 하나. 회신은 묶음마다 곧바로 — 같은 캐릭터의 뒤 불러오기 결과보다 먼저 결과 상자에 들어간다
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
                const DBMailStmt* mails[MAX_BUNDLE_HEADS] = { nullptr, nullptr };
                for (int h = 0; h < n; ++h)
                {
                    assert(static_cast<size_t>(heads[h]->slotBegin) + static_cast<size_t>(heads[h]->slotCount) <= localRows.size());
                    rows[h] = (heads[h]->slotCount > 0) ? localRows.data() + heads[h]->slotBegin : nullptr;
                    assert(heads[h]->mailCount <= 1 && (heads[h]->mailCount == 0 || static_cast<size_t>(heads[h]->mailBegin) < localMails.size()));
                    mails[h] = (heads[h]->mailCount > 0) ? &localMails[heads[h]->mailBegin] : nullptr;
                }
                const DBSaveResult result = ExecBundle(slot, heads, rows, mails, n, reconnectFailedThisBatch);
                static_assert(static_cast<int>(DBSaveResult::COUNT) == 3, "DBSaveResult 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
                for (int h = 0; h < n; ++h)
                {
                    const DBSaveBundle& b = *heads[h];
                    switch (result)
                    {
                    case DBSaveResult::OK:
                        _counters._savedJobs.Inc();
                        _counters._saveSlotRows.Add(b.slotCount);
                        _counters._saveMailStmts.Add(b.mailCount);   // 확정된 우편 문장 — 떠난 기록으로 확정된 것까지(mail-plan.md 5절)
                        if (b.parts & SAVE_PART_CHAR_ROW)
                            _counters._saveCharRows.Inc();
                        break;
                    case DBSaveResult::FAILED:
                        _counters._failedJobs.Inc();
                        break;
                    case DBSaveResult::FENCED:
                        _counters._fencedJobs.Inc();
                        break;
                    case DBSaveResult::COUNT:
                        break;   // 값이 아니라 개수 표시
                    }
                    PushSaveReply(DBSaveReply{ CharacterId{ b.row.characterId }, b.sessionId, b.bundleNo, result });
                }
                break;
            }

            case DBJobKind::MAIL:
            {
                // 우편 잡 — 불러오기처럼 회신을 반드시 하나 돌려준다(사람별 우편 상태가 이 회신으로 풀린다). 묶음 끝을 기다리지 않는다
                DBMailReply reply;
                ExecMailJob(slot, job.mail, reply, reconnectFailedThisBatch);
                PushMailReply(std::move(reply));
                break;
            }

            case DBJobKind::COUNT:
                break;   // 값이 아니라 개수 표시 — 큐에 들어올 수 없다
            }
        }
        local.clear();
        localRows.clear();
        localMails.clear();
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
//   ※ character_id 는 표 밖이다 — 저장에선 VALUES 의 첫 ?, 불러오기에선 WHERE 의 ? 로 자리가 다르다.
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
    DB_COL(DBCharRow, DBResult, job,             job,            false),   // 토글 없음 — 직업 칸이 늘 있다
    DB_COL(DBCharRow, DBResult, room_lockouts,   roomLockouts,   false),   // 바이트 열 — 형식은 RoomLockoutBlob.h. 토글 없음
#if USE_INVENTORY
    // 가진 돈 — 가방 토글 안(아래 가방 열 표와 같은 토글). 가방을 끈 빌드는 이 열을 쓰지도 읽지도 않는다 — 0 으로 덮지 않게
    //   (체력·마력의 「모름」 값 같은 장치가 돈엔 없다 · npc-shop-plan.md 「DB」)
    DB_COL(DBCharRow, DBResult, money,           money,          false),
#endif
};
static constexpr int kCharColCount = static_cast<int>(sizeof(kCharColumns) / sizeof(kCharColumns[0]));
// 열을 늘리면 여기서 멈춘다 — 저장소가 안 바뀐 행을 빼므로(save-skip-row-plan.md), 새 열은 그 열만 바뀐 변경을 놓치지 않게
//   행 판정(CCharacterStore::CharRowChanged)·사본(RecordSaved · Player.h DbSavedValues)에도 넣고 이 수를 고칠 것.
static_assert(kCharColCount == 11 + USE_INVENTORY,
              "characters 열이 바뀌었다 — CCharacterStore::CharRowChanged 의 판정·RecordSaved 사본에 새 열을 넣었는지 보고 이 수를 고칠 것");

#if USE_INVENTORY
// inventory 행 — 저장 DBInvRowBuf · 결과 DBInvRow(한 줄). slot 은 기본 키 (character_id, slot) 의 일부라 UPDATE 에 안 들어간다
static constexpr DbColumn kInvColumns[] = {
    DB_COL(DBInvRowBuf, DBInvRow, slot,    slot,   true),
    DB_COL(DBInvRowBuf, DBInvRow, item_id, itemId, false),
    DB_COL(DBInvRowBuf, DBInvRow, count,   count,  false),
};
static constexpr int kInvColCount = static_cast<int>(sizeof(kInvColumns) / sizeof(kInvColumns[0]));
#endif

// 소유 표 — 존은 읽기만 한다(쓰는 쪽은 캐릭터 서버). 버퍼 DBRosterRowBuf · 결과 DBResult
static constexpr DbColumn kRosterColumns[] = {
    DB_COL(DBRosterRowBuf, DBResult, name, rosterName, false),
    DB_COL(DBRosterRowBuf, DBResult, look, rosterLook, false),
};
static constexpr int kRosterColCount = static_cast<int>(sizeof(kRosterColumns) / sizeof(kRosterColumns[0]));
#undef DB_COL

static constexpr int kMaxCols = 12;  // 바인딩 배열 크기 — 표가 이보다 길어지면 아래 static_assert 가 막는다(10-21 돈 열로 9 → 10 · 11-02 직업 열로 10 → 11)
static_assert(kCharColCount <= kMaxCols, "kCharColumns 가 kMaxCols 를 넘었다");
#if USE_INVENTORY
static_assert(kInvColCount <= kMaxCols, "kInvColumns 가 kMaxCols 를 넘었다");
#endif

// 표에서 문장을 조립한다 — 프로그램 시작 때 한 번. kStmts 가 c_str() 를 들고 있으므로 수명은 정적.
//   저장: INSERT INTO <표> (character_id,열,…) VALUES (?,?,…) ON DUPLICATE KEY UPDATE 열=?,…(key 열은 뺀다)
//   불러오기: SELECT 열,… FROM <표> WHERE character_id=?<꼬리>
static std::string BuildUpsertSql(const char* table, const DbColumn* cols, int n)
{
    std::string names = "character_id", marks = "?", sets;
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
    return "SELECT " + names + " FROM " + table + " WHERE character_id=?" + tail;
}

static const std::string kSaveSql = BuildUpsertSql("characters", kCharColumns, kCharColCount);
static const std::string kLoadSql = BuildSelectSql("characters", kCharColumns, kCharColCount, "");
#if USE_INVENTORY
static const std::string kItemSaveSql = BuildUpsertSql("inventory", kInvColumns, kInvColCount);
// 가방 불러오기 — 빈 칸(item_id 0)은 안 읽는다. 칸 순서로 받는다(줄이 넘칠 때 어느 줄을 버릴지가 늘 같게).
static const std::string kLoadInvSql  = BuildSelectSql("inventory", kInvColumns, kInvColCount, " AND item_id<>0 ORDER BY slot");
#endif
static const std::string kLoadRosterSql = BuildSelectSql("character_roster", kRosterColumns, kRosterColCount, "");

// 주인 열 둘(펜스) — 열 표 밖이다(DBOwnerBuf 주석). 잠금 읽기는 UPDATE 와 같은 잠금이라 불러오기와 저장이 캐릭터 행 하나에서 차례를 선다.
static const char* const kOwnerSelectSql = "SELECT owner_zone,owner_seq FROM characters WHERE character_id=? FOR UPDATE";
static const char* const kOwnerUpdateSql = "UPDATE characters SET owner_zone=?,owner_seq=? WHERE character_id=?";

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

// 저장 파라미터 — ? 는 character_id 1개 + VALUES 의 열 n개 + UPDATE 의 열(key 제외). UPDATE 쪽은 VALUES 쪽과 같은 버퍼를
//   참조한다(값을 한 번만 복사하면 된다 — 바이트 열의 길이 자리도 같은 것을 가리킨다). saveBuf 는 저장 구조체 꼴의 슬롯 버퍼.
static bool BindUpsertParams(MYSQL_STMT* st, void* saveBuf, int64_t* characterIdBuf, const DbColumn* cols, int n)
{
    MYSQL_BIND b[1 + 2 * kMaxCols] = {};
    b[0].buffer_type = MYSQL_TYPE_LONGLONG; b[0].buffer = characterIdBuf;
    int k = 1 + n;   // UPDATE 쪽 ? 가 시작하는 자리
    for (int i = 0; i < n; ++i)
    {
        BindColumn(b[1 + i], saveBuf, cols[i]);
        if (!cols[i].key)
            b[k++] = b[1 + i];
    }
    return mysql_stmt_bind_param(st, b) == 0;
}

// 불러오기 — 파라미터는 character_id 1개, 결과는 표 순서 그대로 loadBuf(저장 구조체 꼴)의 멤버에 fetch 가 바로 채운다.
//   숫자 열은 NOT NULL 고정폭이고 열 타입과 버퍼 타입이 같아(FLOAT→float, INT→int32, BIGINT→int64) 변환·잘림이 없다.
//   바이트 열만 길이가 바뀐다 — 길이는 len 에 오고, 버퍼보다 길면 잘린다(RunLoadOnce 가 가른다).
//   is_null·error(와 숫자 열의 length)는 비워 두면 라이브러리가 내부 값으로 채운다.
static bool BindSelect(MYSQL_STMT* st, void* loadBuf, int64_t* characterIdBuf, const DbColumn* cols, int n)
{
    MYSQL_BIND p[1] = {};
    p[0].buffer_type = MYSQL_TYPE_LONGLONG; p[0].buffer = characterIdBuf;
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
    // ? = character_id + 열 n개 + UPDATE 열 n개 (key 열이 없는 표 — 지금 아홉 열이라 19개)
    return BindUpsertParams(st, &slot.bSave, &slot.bSave.characterId, kCharColumns, kCharColCount);
}

// ── 캐릭터 불러오기 ───────────────────────────────────────────────────────
static bool BindLoadCharacter(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    return BindSelect(st, &slot.bLoad, &slot.bLoad.characterId, kCharColumns, kCharColCount);
}

#if USE_INVENTORY
// ── 가방 칸 저장(저장 묶음의 칸 문장) ─────────────────────────────────────────────────────────
static bool BindItemSlotParams(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    // ? 6개 = character_id + slot·item_id·count + UPDATE item_id·count (slot 은 key 라 UPDATE 에 없다)
    return BindUpsertParams(st, &slot.bItem, &slot.bItem.characterId, kInvColumns, kInvColCount);
}

// ── 가방 불러오기 (불러오기 잡의 둘째 문장 — 작업 종류가 아니다) ──────────
//   문장은 열 표(kInvColumns)에서 조립한 kLoadInvSql — 빈 칸을 거르는 꼬리(item_id<>0 ORDER BY slot)는 거기 있다.
static bool BindLoadInventory(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    return BindSelect(st, &slot.bInvLoad, &slot.bInvLoad.characterId, kInvColumns, kInvColCount);
}
#endif

// ── 소유 표 불러오기 (불러오기 잡의 마지막 문장 — 입장 번호 길만) ──────────
static bool BindLoadRoster(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    return BindSelect(st, &slot.bRoster, &slot.bRoster.characterId, kRosterColumns, kRosterColCount);
}

// 주인 잠금 읽기 — 파라미터는 character_id, 결과는 두 열. owner_seq 는 BIGINT UNSIGNED 다
static bool BindOwnerSelect(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    MYSQL_BIND p[1] = {};
    p[0].buffer_type = MYSQL_TYPE_LONGLONG; p[0].buffer = &slot.bOwner.characterId;
    if (mysql_stmt_bind_param(st, p) != 0)
        return false;

    MYSQL_BIND r[2] = {};
    r[0].buffer_type = MYSQL_TYPE_LONG;     r[0].buffer = &slot.bOwner.ownerZone;
    r[1].buffer_type = MYSQL_TYPE_LONGLONG; r[1].buffer = &slot.bOwner.ownerSeq; r[1].is_unsigned = true;
    return mysql_stmt_bind_result(st, r) == 0;
}

// 주인 적기 — ? 순서는 owner_zone · owner_seq · character_id
static bool BindOwnerUpdate(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    MYSQL_BIND p[3] = {};
    p[0].buffer_type = MYSQL_TYPE_LONG;     p[0].buffer = &slot.bOwnerSet.ownerZone;
    p[1].buffer_type = MYSQL_TYPE_LONGLONG; p[1].buffer = &slot.bOwnerSet.ownerSeq; p[1].is_unsigned = true;
    p[2].buffer_type = MYSQL_TYPE_LONGLONG; p[2].buffer = &slot.bOwnerSet.characterId;
    return mysql_stmt_bind_param(st, p) == 0;
}

#if USE_MAIL
// ── 우편 문장 (mail-plan.md 5절 · 8절) ─────────────────────────────────────────────
//   열 표를 안 쓴다 — 문장마다 열 조합이 달라(보내기 · 받기 · 계정 우편 받기 · 잡 일곱) 「열 하나 = 한 줄」로 늘어나는 꼴이 아니다.
//   대신 ? 수와 결과 열 수를 바인딩 때 문장에서 직접 읽어 대조한다(MailBindCounts) — 손으로 센 배열이 SQL 과 어긋나면 기동이 멈춘다.
//   글자 파라미터는 STRING(열의 utf8mb4 변환·비교를 탄다) · 바이트 파라미터(첨부 · send_key)는 BLOB ·
//   글자 결과는 BLOB(소유 표 이름과 같은 꼴 — 바이트 그대로 받고 게임 스레드가 검사한다).
//   살아 있는 우편 = 안 받았고 · 안 지웠고 · 기한 전(schema.sql character_mail) — 문장마다 같은 글자를 쓴다.
#define MMO_MAIL_ALIVE "taken_at IS NULL AND deleted_at IS NULL AND expires_at>NOW(3)"
static const char* const kMailSendSql =
    "INSERT INTO character_mail (character_id,from_character_id,from_name,title,body,money,attach,send_key,expires_at) "
    "VALUES (?,?,?,?,?,?,?,?,DATE_ADD(NOW(3), INTERVAL ? DAY)) ON DUPLICATE KEY UPDATE send_key=send_key";
static const char* const kMailTakeSql =
    "UPDATE character_mail SET taken_at=COALESCE(taken_at, NOW(3)) WHERE mail_id=? AND character_id=?";
static const char* const kMailAccTakeSql =
    "INSERT INTO character_mail (character_id,title,body,money,attach,from_account_mail_id,taken_at,expires_at) "
    "VALUES (?,?,?,?,?,?,NOW(3),DATE_ADD(NOW(3), INTERVAL ? DAY)) ON DUPLICATE KEY UPDATE from_account_mail_id=from_account_mail_id";
static const char* const kMailFindReceiverSql =
    "SELECT r.character_id, r.deleted_at IS NOT NULL, "
    "(SELECT COUNT(*) FROM character_mail m WHERE m.character_id=r.character_id AND m.taken_at IS NULL AND m.deleted_at IS NULL AND m.expires_at>NOW(3)) "
    "FROM character_roster r WHERE r.name=?";
static_assert(DB_MAIL_LIST_MAX == 100, "목록 문장의 LIMIT 글자도 같이 고칠 것");
static const char* const kMailListSql =
    "SELECT mail_id,money,TIMESTAMPDIFF(SECOND,NOW(3),expires_at),read_at IS NOT NULL,from_name,title,attach "
    "FROM character_mail WHERE character_id=? AND " MMO_MAIL_ALIVE " ORDER BY mail_id DESC LIMIT 100";
static const char* const kMailReadSql =
    "SELECT mail_id,from_character_id,money,from_name,title,body,attach FROM character_mail "
    "WHERE mail_id=? AND character_id=? AND " MMO_MAIL_ALIVE;
static const char* const kMailMarkReadSql =
    "UPDATE character_mail SET read_at=COALESCE(read_at, NOW(3)) WHERE mail_id=? AND character_id=?";
// 지우기 = 표시만(DB 계정에 DELETE 권한이 없다). 첨부·돈이 남은 우편은 조건에서 걸러 바뀐 행이 0 이 된다(게임 스레드가 미리 막지만 DB 도 지킨다)
static const char* const kMailEraseSql =
    "UPDATE character_mail SET deleted_at=NOW(3) WHERE mail_id=? AND character_id=? AND money=0 AND attach='' AND deleted_at IS NULL";
// 붉은 수 — 안 읽었거나 첨부·돈을 아직 안 받은 우편(mail-plan.md 11-11 결정)
static const char* const kMailCountPendingSql =
    "SELECT COUNT(*) FROM character_mail WHERE character_id=? AND " MMO_MAIL_ALIVE " AND (read_at IS NULL OR money>0 OR attach<>'')";
static const char* const kMailFindMovedSql =
    "SELECT mail_id FROM character_mail WHERE from_account_mail_id=?";
#undef MMO_MAIL_ALIVE

template <size_t N>
static void BindMailBytes(MYSQL_BIND& b, DBBytes<N>& v, enum_field_types type)
{
    b.buffer_type   = type;
    b.buffer        = v.data;
    b.buffer_length = static_cast<unsigned long>(N);
    b.length        = &v.len;
}
static void BindMailI64(MYSQL_BIND& b, int64_t& v) { b.buffer_type = MYSQL_TYPE_LONGLONG; b.buffer = &v; }
static void BindMailI32(MYSQL_BIND& b, int32_t& v) { b.buffer_type = MYSQL_TYPE_LONG;     b.buffer = &v; }

// 손으로 센 파라미터·결과 수가 준비된 문장과 같은가 — 다르면 바인딩 실패로(PrepareStmt 가 기동을 멈춘다)
static bool MailBindCounts(MYSQL_STMT* st, unsigned long params, unsigned int fields)
{
    if (mysql_stmt_param_count(st) == params && mysql_stmt_field_count(st) == fields)
        return true;
    SLOG_ERROR("[DB] mail stmt bind count mismatch (params {}/{}, fields {}/{})",
               mysql_stmt_param_count(st), params, mysql_stmt_field_count(st), fields);
    return false;
}

// 보내기 — ? 순서는 SQL 그대로(받는 캐릭터 · 보낸 캐릭터 · 보낸 이름 · 제목 · 본문 · 돈 · 첨부 · send_key · 기한 일수)
static bool BindMailSend(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    if (!MailBindCounts(st, 9, 0))
        return false;
    DBMailStmt& s = slot.bMail;
    MYSQL_BIND p[9] = {};
    BindMailI64(p[0], s.characterId);
    BindMailI64(p[1], s.fromCharacterId);
    BindMailBytes(p[2], s.fromName, MYSQL_TYPE_STRING);
    BindMailBytes(p[3], s.title,    MYSQL_TYPE_STRING);
    BindMailBytes(p[4], s.body,     MYSQL_TYPE_STRING);
    BindMailI64(p[5], s.money);
    BindMailBytes(p[6], s.attach,   MYSQL_TYPE_BLOB);
    BindMailBytes(p[7], s.sendKey,  MYSQL_TYPE_BLOB);
    BindMailI32(p[8], s.expireDays);
    return mysql_stmt_bind_param(st, p) == 0;
}

// 받기 — 우편 번호 · 주인
static bool BindMailTake(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    if (!MailBindCounts(st, 2, 0))
        return false;
    MYSQL_BIND p[2] = {};
    BindMailI64(p[0], slot.bMail.mailId);
    BindMailI64(p[1], slot.bMail.characterId);
    return mysql_stmt_bind_param(st, p) == 0;
}

// 계정 우편 받기 — 주인 · 제목 · 본문 · 돈 · 첨부 · 계정 우편 번호 · 기한 일수
static bool BindMailAccTake(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    if (!MailBindCounts(st, 7, 0))
        return false;
    DBMailStmt& s = slot.bMail;
    MYSQL_BIND p[7] = {};
    BindMailI64(p[0], s.characterId);
    BindMailBytes(p[1], s.title,  MYSQL_TYPE_STRING);
    BindMailBytes(p[2], s.body,   MYSQL_TYPE_STRING);
    BindMailI64(p[3], s.money);
    BindMailBytes(p[4], s.attach, MYSQL_TYPE_BLOB);
    BindMailI64(p[5], s.fromAccountMailId);
    BindMailI32(p[6], s.expireDays);
    return mysql_stmt_bind_param(st, p) == 0;
}

// 받는 이름 찾기 — ? 이름 · 결과 번호 · 지움 표시 · 우편 수
static bool BindMailFindReceiver(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    if (!MailBindCounts(st, 1, 3))
        return false;
    DBMailJobBuf& j = slot.bMailJob;
    MYSQL_BIND p[1] = {};
    BindMailBytes(p[0], j.name, MYSQL_TYPE_STRING);
    if (mysql_stmt_bind_param(st, p) != 0)
        return false;
    MYSQL_BIND r[3] = {};
    BindMailI64(r[0], j.foundId);
    BindMailI32(r[1], j.flag);
    BindMailI64(r[2], j.count);
    return mysql_stmt_bind_result(st, r) == 0;
}

// 목록 — ? 캐릭터 · 결과는 한 줄 버퍼(bMailBrief)에 fetch 마다
static bool BindMailList(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    if (!MailBindCounts(st, 1, 7))
        return false;
    MYSQL_BIND p[1] = {};
    BindMailI64(p[0], slot.bMailJob.characterId);
    if (mysql_stmt_bind_param(st, p) != 0)
        return false;
    DBMailBrief& b = slot.bMailBrief;
    MYSQL_BIND r[7] = {};
    BindMailI64(r[0], b.mailId);
    BindMailI64(r[1], b.money);
    BindMailI32(r[2], b.remainSec);
    BindMailI32(r[3], b.read);
    BindMailBytes(r[4], b.fromName, MYSQL_TYPE_BLOB);
    BindMailBytes(r[5], b.title,    MYSQL_TYPE_BLOB);
    BindMailBytes(r[6], b.attach,   MYSQL_TYPE_BLOB);
    return mysql_stmt_bind_result(st, r) == 0;
}

// 한 통 읽기 — ? 우편 번호 · 주인 · 결과는 bMailRow
static bool BindMailRead(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    if (!MailBindCounts(st, 2, 7))
        return false;
    MYSQL_BIND p[2] = {};
    BindMailI64(p[0], slot.bMailJob.mailId);
    BindMailI64(p[1], slot.bMailJob.characterId);
    if (mysql_stmt_bind_param(st, p) != 0)
        return false;
    DBMailRow& w = slot.bMailRow;
    MYSQL_BIND r[7] = {};
    BindMailI64(r[0], w.mailId);
    BindMailI64(r[1], w.fromCharacterId);
    BindMailI64(r[2], w.money);
    BindMailBytes(r[3], w.fromName, MYSQL_TYPE_BLOB);
    BindMailBytes(r[4], w.title,    MYSQL_TYPE_BLOB);
    BindMailBytes(r[5], w.body,     MYSQL_TYPE_BLOB);
    BindMailBytes(r[6], w.attach,   MYSQL_TYPE_BLOB);
    return mysql_stmt_bind_result(st, r) == 0;
}

// 읽음 적기 · 지우기 — 둘 다 ? 우편 번호 · 주인
static bool BindMailIdOwner(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    if (!MailBindCounts(st, 2, 0))
        return false;
    MYSQL_BIND p[2] = {};
    BindMailI64(p[0], slot.bMailJob.mailId);
    BindMailI64(p[1], slot.bMailJob.characterId);
    return mysql_stmt_bind_param(st, p) == 0;
}

// 붉은 수 — ? 캐릭터 · 결과 수
static bool BindMailCountPending(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    if (!MailBindCounts(st, 1, 1))
        return false;
    MYSQL_BIND p[1] = {};
    BindMailI64(p[0], slot.bMailJob.characterId);
    if (mysql_stmt_bind_param(st, p) != 0)
        return false;
    MYSQL_BIND r[1] = {};
    BindMailI64(r[0], slot.bMailJob.count);
    return mysql_stmt_bind_result(st, r) == 0;
}

// 받은 줄 찾기 — ? 계정 우편 번호(잡의 mailId 칸) · 결과 번호
static bool BindMailFindMoved(MYSQL_STMT* st, DBWorkerSlot& slot)
{
    if (!MailBindCounts(st, 1, 1))
        return false;
    MYSQL_BIND p[1] = {};
    BindMailI64(p[0], slot.bMailJob.mailId);
    if (mysql_stmt_bind_param(st, p) != 0)
        return false;
    MYSQL_BIND r[1] = {};
    BindMailI64(r[0], slot.bMailJob.foundId);
    return mysql_stmt_bind_result(st, r) == 0;
}
#endif // USE_MAIL

// DBStmt 순서 그대로 — 인덱스가 곧 문장이다. 문장은 열 표(kCharColumns · kInvColumns)에서 조립.
static const StmtDesc kStmts[] = {
    /* CHAR_UPSERT */ { "char_upsert", kSaveSql.c_str(),     BindPositionParams },
    /* CHAR_SELECT */ { "char_select", kLoadSql.c_str(),     BindLoadCharacter },
#if USE_INVENTORY
    /* SLOT_UPSERT */ { "slot_upsert", kItemSaveSql.c_str(), BindItemSlotParams },
    /* INV_SELECT  */ { "inv_select",  kLoadInvSql.c_str(),  BindLoadInventory },
#endif
    /* ROSTER_SELECT */ { "roster_select", kLoadRosterSql.c_str(), BindLoadRoster },
    /* OWNER_SELECT  */ { "owner_select",  kOwnerSelectSql,        BindOwnerSelect },
    /* OWNER_UPDATE  */ { "owner_update",  kOwnerUpdateSql,        BindOwnerUpdate },
#if USE_MAIL
    /* MAIL_SEND_INSERT     */ { "mail_send",          kMailSendSql,         BindMailSend },
    /* MAIL_TAKE_UPDATE     */ { "mail_take",          kMailTakeSql,         BindMailTake },
    /* MAIL_ACC_TAKE_INSERT */ { "mail_acc_take",      kMailAccTakeSql,      BindMailAccTake },
    /* MAIL_FIND_RECEIVER   */ { "mail_find_receiver", kMailFindReceiverSql, BindMailFindReceiver },
    /* MAIL_LIST            */ { "mail_list",          kMailListSql,         BindMailList },
    /* MAIL_READ            */ { "mail_read",          kMailReadSql,         BindMailRead },
    /* MAIL_MARK_READ       */ { "mail_mark_read",     kMailMarkReadSql,     BindMailIdOwner },
    /* MAIL_ERASE           */ { "mail_erase",         kMailEraseSql,        BindMailIdOwner },
    /* MAIL_COUNT_PENDING   */ { "mail_count_pending", kMailCountPendingSql, BindMailCountPending },
    /* MAIL_FIND_MOVED      */ { "mail_find_moved",    kMailFindMovedSql,    BindMailFindMoved },
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
static constexpr int kRosterIdx = static_cast<int>(DBStmt::ROSTER_SELECT);
static constexpr int kOwnerSelectIdx = static_cast<int>(DBStmt::OWNER_SELECT);
static constexpr int kOwnerUpdateIdx = static_cast<int>(DBStmt::OWNER_UPDATE);
#if USE_MAIL
static constexpr int kMailSendIdx         = static_cast<int>(DBStmt::MAIL_SEND_INSERT);
static constexpr int kMailTakeIdx         = static_cast<int>(DBStmt::MAIL_TAKE_UPDATE);
static constexpr int kMailAccTakeIdx      = static_cast<int>(DBStmt::MAIL_ACC_TAKE_INSERT);
static constexpr int kMailFindReceiverIdx = static_cast<int>(DBStmt::MAIL_FIND_RECEIVER);
static constexpr int kMailListIdx         = static_cast<int>(DBStmt::MAIL_LIST);
static constexpr int kMailReadIdx         = static_cast<int>(DBStmt::MAIL_READ);
static constexpr int kMailMarkReadIdx     = static_cast<int>(DBStmt::MAIL_MARK_READ);
static constexpr int kMailEraseIdx        = static_cast<int>(DBStmt::MAIL_ERASE);
static constexpr int kMailCountPendingIdx = static_cast<int>(DBStmt::MAIL_COUNT_PENDING);
static constexpr int kMailFindMovedIdx    = static_cast<int>(DBStmt::MAIL_FIND_MOVED);

// 묶음의 우편 문장 종류 → 문장 자리. 값이 아닌 COUNT 는 -1(부르는 쪽이 실패로 돌린다)
static int MailStmtIndex(DBMailStmtKind kind)
{
    switch (kind)
    {
    case DBMailStmtKind::SEND_INSERT:     return kMailSendIdx;
    case DBMailStmtKind::TAKE_UPDATE:     return kMailTakeIdx;
    case DBMailStmtKind::ACC_TAKE_INSERT: return kMailAccTakeIdx;
    case DBMailStmtKind::COUNT:           break;
    }
    return -1;
}
#endif

// 주인 열 잠금 읽기 — 값은 slot.bOwner 에, 행이 있나는 found 에. false = 실패(오류는 st 에 남아 있다 — 부르는 쪽이 유실·그 밖을 가른 뒤 결과를 닫는다).
//   기본 키 조회라 행은 많아야 하나다. 숫자 두 열이라 잘림이 없다.
static bool ReadOwnerLocked(MYSQL_STMT* st, DBWorkerSlot& slot, int64_t characterId, bool& found)
{
    slot.bOwner.characterId = characterId;
    found = false;
    if (mysql_stmt_execute(st) != 0)
        return false;
    switch (mysql_stmt_fetch(st))
    {
    case 0:
        found = true;
        break;
    case MYSQL_NO_DATA:
        break;
    default:
        return false;
    }
    mysql_stmt_free_result(st);   // 안 닫으면 같은 커넥션의 다음 명령이 "Commands out of sync" 로 실패한다
    return true;
}

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
    // 불러오기 하나 = 트랜잭션 하나 — 주인 잠금을 쥔 채 읽고, 주인 바꾸기까지 한 번에 확정한다(펜스).
    MYSQL* m = static_cast<MYSQL*>(slot.mysql);
    auto classifyConn = [&](const char* step) -> LoadOutcome
    {
        const unsigned int err = mysql_errno(m);
        if (CDbConnection::IsConnectionLostErrno(err))
        {
            SLOG_WARN("[DB] connection lost during load {} (errno={}, character={})", step, err, job.characterId);
            return LoadOutcome::CONNECTION_LOST;
        }
        SLOG_ERROR("[DB] load {} failed (character={}): {}", step, job.characterId, mysql_error(m));
        return LoadOutcome::QUERY_ERROR;
    };

    if (mysql_real_query(m, "START TRANSACTION", static_cast<unsigned long>(strlen("START TRANSACTION"))) != 0)
        return classifyConn("start");

    const LoadOutcome outcome = RunLoadBody(slot, job, out);
    if (outcome == LoadOutcome::CONNECTION_LOST)
        return outcome;   // 열린 트랜잭션은 DB 가 되돌린다 — ExecLoad 가 커넥션을 버린다

    // 읽기에 실패하면 되돌린다 — 주인을 바꾼 채 못 들어온 사람을 남기지 않는다. 되돌리기도 안 되면 커넥션을 버린다(저장 묶음의 실패 가르기와 같은 까닭).
    const bool read = (outcome == LoadOutcome::FOUND || outcome == LoadOutcome::NOT_FOUND);
    if ((read ? mysql_commit(m) : mysql_rollback(m)) != 0)
    {
        const LoadOutcome e = classifyConn(read ? "commit" : "rollback");
        if (e != LoadOutcome::CONNECTION_LOST)
            DropConnection(slot);
        return e;
    }
    return outcome;
}

CDBWorker::LoadOutcome CDBWorker::RunLoadBody(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out)
{
    // 첫 문장 — 주인 잠금. 주인이 내가 아니면 나로 바꾸고 번호를 올린다. 나면 그대로 둔다 — 같은 존 서버로 다시 불러올 때 번호를 올리면
    //   그 존 서버의 확정 전 짝 기록이 자기 불러오기 때문에 거절되어 거래 한쪽만 남는다(world-server-plan.md 2절 3번).
    //   행이 없으면(시험 입장 길의 새 번호) 고칠 행이 없다 — (나, 0)을 들고 첫 저장이 적는다(2절 10번).
    MYSQL_STMT* stOwner = static_cast<MYSQL_STMT*>(slot.stmt[kOwnerSelectIdx]);
    bool ownerRow = false;
    if (!ReadOwnerLocked(stOwner, slot, ToInt(job.characterId), ownerRow))
    {
        const LoadOutcome e = ClassifyStmtError(stOwner, "load owner", job.characterId);
        if (e != LoadOutcome::CONNECTION_LOST)
            mysql_stmt_free_result(stOwner);
        return e;
    }
    out.ownerZone = ToInt(_zoneServerId);
    out.ownerSeq  = 0;
    if (ownerRow)
    {
        out.ownerSeq = slot.bOwner.ownerSeq;
        if (slot.bOwner.ownerZone != out.ownerZone)
        {
            ++out.ownerSeq;
            slot.bOwnerSet = DBOwnerBuf{ ToInt(job.characterId), out.ownerZone, out.ownerSeq };
            MYSQL_STMT* stSet = static_cast<MYSQL_STMT*>(slot.stmt[kOwnerUpdateIdx]);
            if (mysql_stmt_execute(stSet) != 0)
                return ClassifyStmtError(stSet, "load owner update", job.characterId);
        }
    }

    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmt[kLoadIdx]);
    slot.bLoad.characterId = ToInt(job.characterId);

    // 오류 분류 — 유실(재연결 후 다시 해 볼 값어치가 있다)과 그 밖(다시 해도 같다)을 가른다.
    //   errno·메시지는 free_result 가 지우기 전에 먼저 읽는다.
    auto classifyError = [&](const char* step) -> LoadOutcome
    {
        const unsigned int err = mysql_stmt_errno(st);
        if (CDbConnection::IsConnectionLostErrno(err))
        {
            SLOG_WARN("[DB] connection lost during load {} (errno={}, character={})", step, err, job.characterId);
            return LoadOutcome::CONNECTION_LOST;
        }
        SLOG_ERROR("[DB] load {} failed (character={}): {}", step, job.characterId, mysql_stmt_error(st));
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
        //   입장까지 막을 일이 아니다. 실패로 돌리면 그 캐릭터는 행을 고치기 전까지 영영 못 들어온다.
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

    // 마지막 문장 — 소유 표. 가방과 같은 규칙: 실패하면 캐릭터 행을 읽었어도 전체가 실패다(이름 없는 사람으로 들여보내지 않는다).
    if (job.readRoster && (outcome == LoadOutcome::FOUND || outcome == LoadOutcome::NOT_FOUND))
    {
        const LoadOutcome roster = RunRosterLoad(slot, job, out);
        if (roster != LoadOutcome::FOUND)
            return roster;
    }

    return outcome;
}

// 새로 붙은 문장들의 오류 분류 — 유실(재연결 후 다시 해 볼 값어치가 있다)과 그 밖을 가른다(RunLoadOnce 와 같은 규칙).
CDBWorker::LoadOutcome CDBWorker::ClassifyStmtError(void* stmt, const char* what, CharacterId characterId)
{
    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(stmt);
    const unsigned int err = mysql_stmt_errno(st);
    if (CDbConnection::IsConnectionLostErrno(err))
    {
        SLOG_WARN("[DB] connection lost during {} (errno={}, character={})", what, err, characterId);
        return LoadOutcome::CONNECTION_LOST;
    }
    SLOG_ERROR("[DB] {} failed (character={}): {}", what, characterId, mysql_stmt_error(st));
    return LoadOutcome::QUERY_ERROR;
}

#if USE_INVENTORY
CDBWorker::LoadOutcome CDBWorker::RunInventoryLoad(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out)
{
    // 재시도로 두 번 돌 수 있다 — 매번 비우고 시작한다(앞선 시도의 반쪽 줄이 섞이지 않게).
    out.invCount    = 0;
    out.invOverflow = 0;

    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmt[kInvLoadIdx]);
    if (st == nullptr)
        return LoadOutcome::QUERY_ERROR;   // PrepareStmt 가 캐릭터 문장과 함께 세우므로 닿지 않는다 — 닿으면 실패로
    slot.bInvLoad.characterId = ToInt(job.characterId);

    if (mysql_stmt_execute(st) != 0)
        return ClassifyStmtError(st, "load inventory execute", job.characterId);

    // 줄 여럿 — 끝(MYSQL_NO_DATA)까지 다 읽어야 다음 명령이 안 꼬인다. 넘치는 줄은 싣지 않고 수만 센다.
    for (;;)
    {
        const int rc = mysql_stmt_fetch(st);
        if (rc == MYSQL_NO_DATA)
            break;
        if (rc != 0)   // 1(오류) · MYSQL_DATA_TRUNCATED — 버퍼 값을 믿을 수 없다
        {
            const LoadOutcome e = ClassifyStmtError(st, "load inventory fetch", job.characterId);
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

CDBWorker::LoadOutcome CDBWorker::RunRosterLoad(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out)
{
    // 재시도로 두 번 돌 수 있다 — 매번 비우고 시작한다.
    out.rosterFound = false;

    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmt[kRosterIdx]);
    if (st == nullptr)
        return LoadOutcome::QUERY_ERROR;   // PrepareStmt 가 캐릭터 문장과 함께 세우므로 닿지 않는다 — 닿으면 실패로
    slot.bRoster.characterId = ToInt(job.characterId);

    if (mysql_stmt_execute(st) != 0)
        return ClassifyStmtError(st, "load roster execute", job.characterId);

    // 기본 키 조회 — 행은 많아야 하나. switch 인 까닭은 RunLoadOnce 와 같다(오류가 「없음」에 삼켜지면 안 된다).
    LoadOutcome outcome = LoadOutcome::FOUND;
    switch (mysql_stmt_fetch(st))
    {
    case 0:
    case MYSQL_DATA_TRUNCATED:
        // 칸보다 긴 값은 len 에 원래 길이가 남는다 — 게임 스레드가 불량으로 본다(ApplyRoster)
        CopyColumns(&out, &slot.bRoster, kRosterColumns, kRosterColCount);
        out.rosterFound = true;
        break;

    case MYSQL_NO_DATA:
        break;   // 소유 표에 없는 캐릭터 — 사실이다(이름·몸 없이 들어간다)

    default:
        outcome = ClassifyStmtError(st, "load roster fetch", job.characterId);
        break;
    }
    if (outcome != LoadOutcome::CONNECTION_LOST)
        mysql_stmt_free_result(st);
    return outcome;
}

bool CDBWorker::ReconnectDue(const DBWorkerSlot& slot) const
{
    // 성공하면 실패 기록을 지운다 — 살아 있는 DB 에서 연결만 끊긴 것은 곧바로 다시 붙는다. 막는 것은 죽은 DB 를 거푸 두드리는 일뿐이다
    return slot.lastFailedConnect == std::chrono::steady_clock::time_point{}
        || std::chrono::steady_clock::now() - slot.lastFailedConnect >= _reconnectMin;
}

bool CDBWorker::TryReconnectForLoad(DBWorkerSlot& slot, bool& reconnectFailedThisBatch)
{
    // 이 묶음에서 이미 실패했다 = DB 가 죽어 있다고 본다. 불러오기마다 접속 제한 시간(거절까지 약 2초,
    //   안 닿는 곳이면 connectTimeoutSec)을 기다리며 큐를 막지 않는다. 다음 묶음 머리에서 다시 해 본다.
    if (reconnectFailedThisBatch || !ReconnectDue(slot))
    {
        reconnectFailedThisBatch = true;
        return false;
    }

    void* fresh = nullptr;
    if (Connect(fresh))
    {
        slot.mysql = fresh;
        if (PrepareStmt(slot))
        {
            slot.lastFailedConnect = {};
            SLOG_INFO("[DB] worker {} reconnected (load retry)", slot.index);
            return true;
        }
        mysql_close(static_cast<MYSQL*>(slot.mysql));   // stmt 준비 실패 → 커넥션 롤백
        slot.mysql = nullptr;
    }

    slot.lastFailedConnect = std::chrono::steady_clock::now();
    reconnectFailedThisBatch = true;
    return false;
}

void CDBWorker::ExecLoad(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out, bool& reconnectFailedThisBatch)
{
    // 결과는 어떤 경로로 나가든 채워져 있어야 한다 — 기본값을 "실패"로 깔고 시작한다.
    out.kind      = DBJobKind::LOAD_CHARACTER;
    out.code      = DBResultCode::FAILED;
    out.characterId = job.characterId;
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
    out.roomLockouts.len   = 0;
    out.job                = 0;   // 새 캐릭터·못 읽음 = 아직 안 고름
    out.ownerZone          = 0;   // RunLoadBody 가 채운다(실패면 code 가 FAILED 라 안 쓰인다)
    out.ownerSeq           = 0;
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
// 우편 잡 — 읽기·표시(mail-plan.md 8절). 트랜잭션 없이 문장 하나(READ_BODY 만 둘 — 읽기 + 읽음 적기)다.
//   읽기는 두 번 해도 같고, 표시(read_at · deleted_at)도 COALESCE · 「deleted_at IS NULL」 조건이라 두 번 해도 같다 →
//   불러오기와 같은 재연결 규칙(없으면 그 자리 재연결 · 도중에 끊기면 재연결 뒤 한 번 더). 결말은 회신의 ok 하나로.
// ==========================================================================
void CDBWorker::ExecMailJob(DBWorkerSlot& slot, const DBMailJob& job, DBMailReply& out, bool& reconnectFailedThisBatch)
{
    out.op          = job.op;
    out.sessionId   = job.sessionId;
    out.characterId = job.characterId;
    out.mailId      = job.mailId;
    out.ok          = false;
#if !USE_MAIL
    (void)slot; (void)reconnectFailedThisBatch;
    SLOG_ERROR("[DB] mail job in a build without USE_MAIL (character={}, op={})", job.characterId, static_cast<int>(job.op));
#else
    if (slot.mysql == nullptr || slot.stmt[kMailReadIdx] == nullptr)
    {
        DropConnection(slot);   // 반쪽 상태는 PrepareStmt 가 안 만들지만 짝을 맞춰 비운다(ExecLoad 와 같다)
        if (!TryReconnectForLoad(slot, reconnectFailedThisBatch))
            return;             // 즉시 실패 — 매달리지 않는다
    }

    LoadOutcome outcome = RunMailOnce(slot, job, out);
    if (outcome == LoadOutcome::CONNECTION_LOST)
    {
        DropConnection(slot);
        if (TryReconnectForLoad(slot, reconnectFailedThisBatch))
        {
            outcome = RunMailOnce(slot, job, out);
            if (outcome == LoadOutcome::CONNECTION_LOST)
                DropConnection(slot);
        }
    }
    out.ok = (outcome == LoadOutcome::FOUND);   // FOUND = 성공(행이 없어도 — found 가 가른다)
#endif
}

#if USE_MAIL
CDBWorker::LoadOutcome CDBWorker::RunMailSelectOne(DBWorkerSlot& slot, int stmtIdx, const char* what, CharacterId characterId, bool& found)
{
    found = false;
    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmt[stmtIdx]);
    if (st == nullptr)
        return LoadOutcome::QUERY_ERROR;   // PrepareStmt 가 전부 함께 세우므로 닿지 않는다 — 닿으면 실패로
    if (mysql_stmt_execute(st) != 0)
        return ClassifyStmtError(st, what, characterId);

    // 결과는 많아야 한 줄(이름 · 기본 키 · UNIQUE · COUNT). switch 인 까닭은 RunLoadOnce 와 같다(오류가 「없음」에 삼켜지면 안 된다)
    LoadOutcome outcome = LoadOutcome::FOUND;
    switch (mysql_stmt_fetch(st))
    {
    case 0:
    case MYSQL_DATA_TRUNCATED:   // 글자·바이트 칸보다 긴 값 — len 에 원래 길이가 남아 게임 스레드가 불량으로 본다(소유 표 이름과 같다)
        found = true;
        break;
    case MYSQL_NO_DATA:
        break;
    default:
        outcome = ClassifyStmtError(st, what, characterId);
        break;
    }
    if (outcome != LoadOutcome::CONNECTION_LOST)
        mysql_stmt_free_result(st);   // 안 닫으면 같은 커넥션의 다음 명령이 "Commands out of sync" 로 실패한다
    return outcome;
}

CDBWorker::LoadOutcome CDBWorker::RunMailList(DBWorkerSlot& slot, const DBMailJob& job, DBMailReply& out)
{
    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmt[kMailListIdx]);
    if (st == nullptr)
        return LoadOutcome::QUERY_ERROR;
    if (mysql_stmt_execute(st) != 0)
        return ClassifyStmtError(st, "mail list execute", job.characterId);

    // 줄 여럿 — 끝(MYSQL_NO_DATA)까지 다 읽는다(가방 불러오기와 같다). LIMIT 이 상한을 지키지만 벡터도 상한에서 멈춘다
    out.lines.reserve(DB_MAIL_LIST_MAX);
    for (;;)
    {
        const int rc = mysql_stmt_fetch(st);
        if (rc == MYSQL_NO_DATA)
            break;
        if (rc != 0 && rc != MYSQL_DATA_TRUNCATED)
        {
            const LoadOutcome e = ClassifyStmtError(st, "mail list fetch", job.characterId);
            if (e != LoadOutcome::CONNECTION_LOST)
                mysql_stmt_free_result(st);
            return e;
        }
        if (out.lines.size() < static_cast<size_t>(DB_MAIL_LIST_MAX))
            out.lines.push_back(slot.bMailBrief);   // 잘린 칸은 len 이 칸보다 커서 게임 스레드가 그 줄을 불량으로 본다
    }
    mysql_stmt_free_result(st);
    out.found = !out.lines.empty();
    return LoadOutcome::FOUND;   // 줄이 0 이어도 성공 — 「우편함이 비었다」는 사실이다
}

CDBWorker::LoadOutcome CDBWorker::RunMailOnce(DBWorkerSlot& slot, const DBMailJob& job, DBMailReply& out)
{
    // 재연결 뒤 한 번 더 돌 수 있다 — 결과 칸을 매번 비우고 시작한다(앞선 시도의 반쪽 줄이 섞이지 않게)
    out.found = false;
    out.targetDeleted = false;
    out.foundId = 0;
    out.count = 0;
    out.name.len = 0;
    out.row = DBMailRow{};
    out.lines.clear();

    slot.bMailJob = DBMailJobBuf{};   // 값만 비운다 — 바인딩은 멤버 자리를 가리키므로 그대로다
    slot.bMailJob.characterId = ToInt(job.characterId);
    slot.bMailJob.mailId      = job.mailId;

    bool found = false;
    static_assert(static_cast<int>(DBMailOp::COUNT) == 8, "DBMailOp 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
    switch (job.op)
    {
    case DBMailOp::FIND_RECEIVER:
    {
        slot.bMailJob.name = job.name;
        const LoadOutcome r = RunMailSelectOne(slot, kMailFindReceiverIdx, "mail find receiver", job.characterId, found);
        if (r == LoadOutcome::FOUND && found)
        {
            out.found         = true;
            out.foundId       = slot.bMailJob.foundId;
            out.targetDeleted = (slot.bMailJob.flag != 0);
            out.count         = slot.bMailJob.count;
        }
        return r;
    }

    case DBMailOp::LIST:
        return RunMailList(slot, job, out);

    case DBMailOp::READ_BODY:
    case DBMailOp::READ_ONE:
    {
        const LoadOutcome r = RunMailSelectOne(slot, kMailReadIdx, "mail read", job.characterId, found);
        if (r != LoadOutcome::FOUND || !found)
            return r;
        out.found = true;
        out.row   = slot.bMailRow;
        if (job.op == DBMailOp::READ_BODY)
        {
            // 읽음 적기 — 실패하면 잡 전체가 실패다(본문은 보였는데 붉은 수가 안 줄면 다시 열 까닭이 없다 — 다시 읽게 둔다)
            MYSQL_STMT* stMark = static_cast<MYSQL_STMT*>(slot.stmt[kMailMarkReadIdx]);
            if (mysql_stmt_execute(stMark) != 0)
                return ClassifyStmtError(stMark, "mail mark read", job.characterId);
        }
        return LoadOutcome::FOUND;
    }

    case DBMailOp::ERASE:
    {
        MYSQL_STMT* st = static_cast<MYSQL_STMT*>(slot.stmt[kMailEraseIdx]);
        if (mysql_stmt_execute(st) != 0)
            return ClassifyStmtError(st, "mail erase", job.characterId);
        const uint64_t affected = static_cast<uint64_t>(mysql_stmt_affected_rows(st));
        out.count = static_cast<int64_t>(affected);
        out.found = (affected > 0);
        return LoadOutcome::FOUND;
    }

    case DBMailOp::COUNT_PENDING:
    {
        const LoadOutcome r = RunMailSelectOne(slot, kMailCountPendingIdx, "mail count pending", job.characterId, found);
        if (r == LoadOutcome::FOUND && found)
        {
            out.found = true;
            out.count = slot.bMailJob.count;
        }
        return r;
    }

    case DBMailOp::FIND_MOVED:
    {
        const LoadOutcome r = RunMailSelectOne(slot, kMailFindMovedIdx, "mail find moved", job.characterId, found);
        if (r == LoadOutcome::FOUND && found)
        {
            out.found   = true;
            out.foundId = slot.bMailJob.foundId;
        }
        return r;
    }

    case DBMailOp::FIND_NAME:
    {
        // 소유 표 문장을 같이 쓴다(이름 · 몸을 읽는다 — 몸은 버린다). 같은 워커 스레드의 일이라 불러오기 버퍼와 겹치지 않는다
        slot.bRoster.characterId = ToInt(job.otherCharacterId);
        const LoadOutcome r = RunMailSelectOne(slot, kRosterIdx, "mail find name", job.characterId, found);
        if (r == LoadOutcome::FOUND && found)
        {
            out.found = true;
            out.name  = slot.bRoster.rosterName;
        }
        return r;
    }

    case DBMailOp::COUNT:
        break;   // 값이 아니라 개수 표시
    }
    SLOG_ERROR("[DB] bad mail op {} (character={})", static_cast<int>(job.op), job.characterId);
    return LoadOutcome::QUERY_ERROR;
}
#endif // USE_MAIL

// ==========================================================================
// 저장 묶음 — 묶음 하나 = 트랜잭션 하나 (save-unify-plan.md 「워커」)
//   쓰는 것은 켜진 덩어리만 — 캐릭터 행은 parts 에 SAVE_PART_CHAR_ROW 가 있을 때만, 칸은 slotCount 줄(save-skip-row-plan.md).
//   늘 START TRANSACTION → 주인 잠금 읽기(머리마다) → (캐릭터 행 UPSERT) → 칸 UPSERT × N → COMMIT. 칸 줄은 최종값이라 두 번 써도 같다.
//   주인이 묶음이 든 값과 다르면 ROLLBACK 하고 FENCED 로 답한다(펜스 · world-server-plan.md 2절 4번).
//   워커는 묶음의 주인(세션)을 모른다 — 결말을 회신할 뿐, 다시 싣기는 저장소가 정한다.
// ==========================================================================
#if USE_DB_FAULT_INJECT
// 이 묶음에 걸린 주입(문장 뒤 오류·끊김)이 아직 안 쓰였나 — 워커 스레드마다 따로(묶음 처리 한 번 동안만 산다)
static thread_local bool t_faultArmed = false;
#endif

CDBWorker::SaveOutcome CDBWorker::RunBundleOnce(DBWorkerSlot& slot, const DBSaveBundle* const* heads, const DBInvRow* const* rows,
                                                const DBMailStmt* const* mails, int n)
{
    assert(n >= 1 && n <= MAX_BUNDLE_HEADS);
    MYSQL* m = static_cast<MYSQL*>(slot.mysql);
    const DBSaveBundle& b = *heads[0];   // 로그의 머리 — 짝이면 둘째 캐릭터도 같이 적는다(아래 fail)
    const int64_t pairCharacter = (n > 1) ? heads[1]->row.characterId : 0;

    // 실패 가르기 — 끊김이면 커넥션을 버린다(열린 트랜잭션은 DB 가 되돌린다). 그 밖이면 ROLLBACK,
    //   그것도 안 되면 커넥션을 버린다 — 다음 START TRANSACTION 은 열린 트랜잭션을 말없이 커밋하므로 반쪽 묶음을 들고 다음 잡으로 가지 않는다.
    auto fail = [&](unsigned int err, const char* what, const char* msg) -> SaveOutcome
    {
        if (CDbConnection::IsConnectionLostErrno(err))
        {
            SLOG_WARN("[DB] connection lost during save bundle {} (errno={}, character={})", what, err, b.row.characterId);
            DropConnection(slot);
            return SaveOutcome::CONNECTION_LOST;
        }
        SLOG_ERROR("[DB] save bundle {} failed (character={}, pair={}, bundle={}): {}", what, b.row.characterId, pairCharacter, b.bundleNo, msg);
        if (mysql_rollback(m) != 0)
        {
            SLOG_ERROR("[DB] rollback failed -> drop connection (character={}): {}", b.row.characterId, mysql_error(m));
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
            SLOG_WARN("[DB][fault] trigger error@{} (character={}, bundle={})", stmtNo, b.row.characterId, b.bundleNo);
            out = fail(0, "fault", "injected error");
        }
        else
        {
            SLOG_WARN("[DB][fault] trigger lost@{} (character={}, bundle={})", stmtNo, b.row.characterId, b.bundleNo);
            DropConnection(slot);
            out = SaveOutcome::CONNECTION_LOST;
        }
        return true;
    };
    SaveOutcome injected = SaveOutcome::OK;
#endif

    if (mysql_real_query(m, "START TRANSACTION", static_cast<unsigned long>(strlen("START TRANSACTION"))) != 0)
        return fail(mysql_errno(m), "start", mysql_error(m));

    // 주인 맞춰 보기 — 머리 전부를 쓰기 앞에 본다. 짝이면 한쪽이라도 다르면 묶음 전체를 되감는다(남은 사람 메모리에 거래 결과가 남으면 복제다).
    //   작은 번호부터 잠근다 — 두 존 서버의 짝 묶음이 같은 사람을 반대 순서로 잡아 서로 기다리지 않게.
    //   행이 없으면 통과한다 — 시험 입장 길의 새 번호뿐이다(world-server-plan.md 2절 10번).
    bool rowMissing[MAX_BUNDLE_HEADS] = {};
    MYSQL_STMT* stOwner = static_cast<MYSQL_STMT*>(slot.stmt[kOwnerSelectIdx]);
    const int first = (n > 1 && heads[1]->row.characterId < heads[0]->row.characterId) ? 1 : 0;
    for (int i = 0; i < n; ++i)
    {
        const int h = (first + i) % n;
        const DBSaveBundle& hb = *heads[h];
        bool ownerRow = false;
        if (!ReadOwnerLocked(stOwner, slot, hb.row.characterId, ownerRow))
        {
            const unsigned int err = mysql_stmt_errno(stOwner);
            const std::string msg = mysql_stmt_error(stOwner);   // free_result 가 지우기 전에
            if (!CDbConnection::IsConnectionLostErrno(err))
                mysql_stmt_free_result(stOwner);
            return fail(err, "owner", msg.c_str());
        }
        if (!ownerRow)
        {
            rowMissing[h] = true;
            continue;
        }
        if (slot.bOwner.ownerZone != hb.ownerZone || slot.bOwner.ownerSeq != hb.ownerSeq)
        {
            SLOG_WARN("[DB] save bundle FENCED (character={}, pair={}, bundle={}, mine={}/{}, owner={}/{})",
                      hb.row.characterId, pairCharacter, hb.bundleNo, hb.ownerZone, hb.ownerSeq, slot.bOwner.ownerZone, slot.bOwner.ownerSeq);
            if (mysql_rollback(m) != 0)
            {
                SLOG_ERROR("[DB] rollback failed -> drop connection (character={}): {}", hb.row.characterId, mysql_error(m));
                DropConnection(slot);
            }
            return SaveOutcome::FENCED;
        }
    }

    // 머리마다 — 캐릭터 행(켜졌을 때만 · 지금 행 저장과 같은 문장 · 같은 버퍼) → 칸 문장 × N. 문장 번호는 머리를 넘어 이어 센다
    //   (주입 error@k · lost@k 의 k — 실제로 보낸 문장만 센다. 짝이면 A 의 문장 뒤 B 의 문장이다. 짝 가운데서 끊는 시험이 그 번호로 고른다)
    int stmtNo = 0;
    for (int h = 0; h < n; ++h)
    {
        const DBSaveBundle& hb = *heads[h];
        if (hb.parts & SAVE_PART_CHAR_ROW)
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
            // 이 묶음이 행을 만들었다 — 든 주인을 적는다(UPSERT 는 주인 열을 안 쓴다). 칸만 바뀐 묶음은 행을 안 만드니 다음 행 저장이 적는다
            if (rowMissing[h])
            {
                MYSQL_STMT* stSet = static_cast<MYSQL_STMT*>(slot.stmt[kOwnerUpdateIdx]);
                slot.bOwnerSet = DBOwnerBuf{ hb.row.characterId, hb.ownerZone, hb.ownerSeq };
                if (mysql_stmt_execute(stSet) != 0)
                    return fail(mysql_stmt_errno(stSet), "owner row", mysql_stmt_error(stSet));
            }
        }

#if USE_INVENTORY
        // 칸 문장 × N — 칸마다 최종값(빈 칸은 item 0 으로 덮는다 — DB 계정에 DELETE 권한이 없다)
        MYSQL_STMT* stItem = static_cast<MYSQL_STMT*>(slot.stmt[kItemIdx]);
        const DBInvRow* hr = rows[h];
        for (int i = 0; i < hb.slotCount; ++i)
        {
            slot.bItem = DBInvRowBuf{ hb.row.characterId, hr[i].slot, hr[i].itemId, hr[i].count };
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

#if USE_MAIL
        // 우편 문장 — 칸 문장 뒤(mail-plan.md 5절). 칸·돈과 같은 트랜잭션이라 둘이 갈라지지 않는다.
        //   세 문장 다 두 번 써도 같다(send_key · COALESCE · from_account_mail_id) — COMMIT 끊김 뒤 묶음째 다시 써도 한 줄이다.
        if (hb.mailCount > 0)
        {
            assert(hb.mailCount == 1 && mails != nullptr && mails[h] != nullptr);
            const int mi = MailStmtIndex(mails[h]->kind);
            if (mi < 0)
                return fail(0, "mail stmt", "bad mail stmt kind");
            MYSQL_STMT* stMail = static_cast<MYSQL_STMT*>(slot.stmt[mi]);
            slot.bMail = *mails[h];
            if (mysql_stmt_execute(stMail) != 0)
                return fail(mysql_stmt_errno(stMail), "mail stmt", mysql_stmt_error(stMail));
            ++stmtNo;
#if USE_DB_FAULT_INJECT
            if (injectAfter(stmtNo, injected))
                return injected;
#endif
        }
#else
        (void)mails;
        assert(hb.mailCount == 0 && "우편을 끈 빌드의 묶음은 우편 문장이 없다");
#endif
    }

    // COMMIT 도중 끊기면 됐는지 모른다 — 끊김으로 돌려 묶음째 다시 쓴다(최종값이라 두 번 써도 같다)
    if (mysql_commit(m) != 0)
        return fail(mysql_errno(m), "commit", mysql_error(m));
    return SaveOutcome::OK;
}

DBSaveResult CDBWorker::ExecBundle(DBWorkerSlot& slot, const DBSaveBundle* const* heads, const DBInvRow* const* rows,
                                   const DBMailStmt* const* mails, int n, bool& reconnectFailedThisBatch)
{
    const DBSaveBundle& b = *heads[0];   // 로그·주입의 머리 — 짝이면 둘 중 하나라도 주입 캐릭터이면 건다
#if USE_DB_FAULT_INJECT
    t_faultArmed = TakeFault(CharacterId{ b.row.characterId }) || (n > 1 && TakeFault(CharacterId{ heads[1]->row.characterId }));
    if (t_faultArmed && _faultKind == FaultKind::DELAY)
    {
        SLOG_WARN("[DB][fault] trigger delay {}ms (character={}, bundle={})", _faultArg, b.row.characterId, b.bundleNo);
        t_faultArmed = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(_faultArg));
    }
    else if (t_faultArmed && _faultKind == FaultKind::DOWN)
    {
        // DB 가 죽은 흉내 — 커넥션을 버리고 다음 재연결 n 번을 실패시킨다(Connect 가 깎는다)
        SLOG_WARN("[DB][fault] trigger down x{} (character={}, bundle={})", _faultArg, b.row.characterId, b.bundleNo);
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
            r = RunBundleOnce(slot, heads, rows, mails, n);
        }
    }
    else
    {
        r = RunBundleOnce(slot, heads, rows, mails, n);
        if (r == SaveOutcome::CONNECTION_LOST)
        {
            DropConnection(slot);   // 실패 가르기가 이미 버렸다 — 짝을 맞춰 비운다(멱등)
            if (TryReconnectForLoad(slot, reconnectFailedThisBatch))
            {
                retried = true;
                r = RunBundleOnce(slot, heads, rows, mails, n);
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
    switch (r)
    {
    case SaveOutcome::OK:     return DBSaveResult::OK;
    case SaveOutcome::FENCED: return DBSaveResult::FENCED;   // 실패가 아니다 — 로그는 RunBundleOnce 가 남겼다
    case SaveOutcome::QUERY_ERROR:
    case SaveOutcome::CONNECTION_LOST:
        break;
    }
    SLOG_ERROR("[DB] save bundle FAILED (character={}, session={}, bundle={}, rows={}, pair={}) — 저장소가 다음 계기에 다시 싣는다",
               b.row.characterId, b.sessionId, b.bundleNo, b.slotCount, (n > 1) ? heads[1]->row.characterId : 0);
    return DBSaveResult::FAILED;
}

#if USE_DB_FAULT_INJECT
// 실패 주입 계획 — 환경 변수 MMO_DB_FAULT 한 줄("<종류>:<값>,character=<캐릭터>,times=<횟수>"). 없거나 못 읽으면 주입 없음.
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
        int64_t character = 0;
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
                else if (key == "character") { character = std::stoll(val); }
                else if (key == "times")   { times = std::stoi(val); }
            }
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
        _faultKind = kind;
        _faultArg = arg;
        _faultCharacter = CharacterId{ character };
        _faultTimesLeft.store(times);
        SLOG_WARN("[DB][fault] plan loaded: {} (kind={}, arg={}, character={}, times={})",
                  spec, static_cast<int>(kind), arg, character, times);
    }
    catch (const std::exception& e)
    {
        SLOG_ERROR("[DB][fault] bad MMO_DB_FAULT '{}': {} — 주입 없음", spec, e.what());
        _faultKind = FaultKind::NONE;
    }
}

bool CDBWorker::TakeFault(CharacterId characterId)
{
    if (_faultKind == FaultKind::NONE || characterId != _faultCharacter)
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
