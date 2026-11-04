#include "DbPipeline.h"

#ifdef _WIN32
#include <winsock2.h>   // mysql.h 이전에 — 구버전 winsock 자동 include 충돌 차단
#endif
#include <mysql.h>

#include <cassert>
#include <chrono>

#include <Common/ErrorLog.h>
#include "CoreAffinity.h"
#include <Monitoring/MetricsText.h>

CDbPipeline::CDbPipeline(const DBConfig& config, int workerCount, int queueMax, std::vector<std::string> opNames)
    : _config(config), _workerCount(workerCount), _queueMax(queueMax), _opNames(std::move(opNames))
{
    assert(_workerCount > 0 && _queueMax > 0);
    for (size_t i = 0; i < _opNames.size(); ++i)
        _opLatency.push_back(std::make_unique<CLatencyHistogram>());
}

CDbPipeline::~CDbPipeline()
{
    Stop();
}

bool CDbPipeline::Start()
{
    // 멀티스레드 안전 초기화 — 워커 스레드 생성 "전"에 메인에서 1회
    if (mysql_library_init(0, nullptr, nullptr) != 0)
    {
        SLOG_ERROR("[DB] mysql_library_init failed");
        return false;
    }
    _libInited = true;

    _workers.reserve(_workerCount);
    for (int i = 0; i < _workerCount; ++i)
    {
        auto worker = std::make_unique<Worker>(_config);
        if (!worker->connection.Open())
        {
            SLOG_ERROR("[DB] worker {} connect failed", i);
            _workers.clear();
            mysql_library_end();   // library_init 롤백 (Start 실패 시 누수 방지)
            _libInited = false;
            return false;
        }
        _workers.push_back(std::move(worker));
    }

    // 커넥션 확보 후 워커 스레드 시작 (각 커넥션은 해당 워커만 단독 사용)
    _stop.store(false);
    for (auto& worker : _workers)
        worker->thread = std::thread(&CDbPipeline::WorkerThread, this, std::ref(*worker));

    SLOG_INFO("[DB] connected {}:{} db={} — {} worker(s), queueMax={}",
              _config.host, _config.port, _config.database, _workerCount, _queueMax);
    return true;
}

void CDbPipeline::Stop()
{
    _stop.store(true);
    for (auto& worker : _workers)
    {
        {
            std::lock_guard<std::mutex> lk(worker->mutex);   // 대기 진입 직전 구간에 끼어 알림이 유실되지 않게
        }
        worker->cv.notify_one();
    }
    for (auto& worker : _workers)
    {
        if (worker->thread.joinable())
            worker->thread.join();
    }
    _workers.clear();   // 연결은 소멸자가 닫는다 — 라이브러리 정리 앞이어야 한다
    if (_libInited)
    {
        mysql_library_end();
        _libInited = false;
    }
}

int CDbPipeline::WorkerIndexOf(int64_t key) const
{
    const uint64_t k = static_cast<uint64_t>(key);
    return static_cast<int>(k % static_cast<uint64_t>(_workerCount));
}

bool CDbPipeline::Submit(int64_t key, int opIndex, WorkFn work)
{
    assert(opIndex >= 0 && opIndex < static_cast<int>(_opNames.size()));
    if (_workers.empty())
        return false;

    Worker& worker = *_workers[WorkerIndexOf(key)];
    {
        std::lock_guard<std::mutex> lk(worker.mutex);
        if (static_cast<int>(worker.queue.size()) >= _queueMax)
            return false;
        worker.queue.push_back(Job{ opIndex, std::move(work) });
        worker.depth.store(static_cast<int64_t>(worker.queue.size()), std::memory_order_relaxed);
    }
    worker.cv.notify_one();
    return true;
}

void CDbPipeline::WorkerThread(Worker& worker)
{
    CoreAffinity::PinIoThread();   // DB 워커 → 게임코어 밖으로 (격리 off면 no-op)
    mysql_thread_init();

    while (true)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lk(worker.mutex);
            worker.cv.wait(lk, [this, &worker] { return !worker.queue.empty() || _stop.load(); });
            if (worker.queue.empty())
                break;   // 정지 요청 + 잔여 없음 → 종료(드레인 완료)
            job = std::move(worker.queue.front());
            worker.queue.pop_front();
            worker.depth.store(static_cast<int64_t>(worker.queue.size()), std::memory_order_relaxed);
        }

        // 끊겨 있으면 재연결 1회 — 실패하면 작업이 연결 없음을 보고 실패로 답한다(요청이 올 때만 시도하므로 자연스러운 백오프)
        if (!worker.connection.IsOpen())
            worker.connection.Open();

        const auto started = std::chrono::steady_clock::now();
        ReplyFn reply = job.work(worker.connection);
        const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

        _opLatency[job.opIndex]->Record(elapsedMs);
        if (worker.connection.TakeJobFailed())
            _failed.fetch_add(1, std::memory_order_relaxed);

        if (reply)
        {
            std::lock_guard<std::mutex> lk(_replyMutex);
            _replies.push_back(std::move(reply));
        }
    }

    worker.connection.Close();   // 만든 스레드가 닫는다
    mysql_thread_end();
}

int CDbPipeline::DrainReplies()
{
    {
        std::lock_guard<std::mutex> lk(_replyMutex);
        _localReplies.swap(_replies);
    }
    const int count = static_cast<int>(_localReplies.size());
    for (ReplyFn& reply : _localReplies)
        reply();
    _localReplies.clear();
    return count;
}

void CDbPipeline::WriteMetrics(std::ostringstream& ss) const
{
    MetricsText::WriteCounter(ss, "mmo_db_failed_total", "Total DB job failures", _failed.load(std::memory_order_relaxed));

    MetricsText::WriteHead(ss, "mmo_db_queue_depth", "Jobs waiting, per DB worker (sustained growth = worker falling behind)", "gauge");
    for (int i = 0; i < static_cast<int>(_workers.size()); ++i)
        ss << "mmo_db_queue_depth{dbworker=\"" << i << "\"} " << _workers[i]->depth.load(std::memory_order_relaxed) << "\n";
    ss << "\n";

    MetricsText::WriteHead(ss, "mmo_db_query_seconds", "DB job time on the worker, per operation", "histogram");
    for (size_t i = 0; i < _opNames.size(); ++i)
    {
        const std::string labels = "op=\"" + _opNames[i] + "\"";
        _opLatency[i]->WriteSeries(ss, "mmo_db_query_seconds", labels.c_str());
    }
    ss << "\n";
}
