#pragma once
// ==========================================================================
// CDbPipeline — 로비 서버의 DB 왕복: 요청 한 건 → 워커에서 실행 → 로직 스레드로 회신
//
//   작업은 「워커에서 돌 함수」와 「로직 스레드에서 받을 함수」 한 쌍이고 SQL 을 모른다.
//   같은 열쇠(계정 번호)는 같은 워커로 간다 — 한 계정의 요청 차례가 뒤바뀌지 않는다.
//   큐가 상한에 닿으면 넣기를 거절한다 → 부르는 쪽이 BUSY 로 답한다.
//   연결이 끊기면 그 작업은 실패로 돌아가고, 다음 작업이 다시 연결한다(재시도는 사람의 다음 요청이 맡는다).
//
//   존의 CDBWorker 는 작업이 불러오기·저장 묶음뿐이고 거래 짝 저장·다시 싣기 규칙이 들어 있어 합치지 않는다.
//
// [지표] mmo_db_queue_depth{dbworker} · mmo_db_failed_total · mmo_db_query_seconds{op}
// ==========================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <Db/DbConnection.h>
#include <Monitoring/LatencyHistogram.h>

class CDbPipeline
{
public:
    using ReplyFn = std::function<void()>;
    using WorkFn = std::function<ReplyFn(CDbConnection&)>;

    // opNames = mmo_db_query_seconds 의 op 라벨 값 — 시작부터 0 으로 내보내려고 미리 받는다
    CDbPipeline(const DBConfig& config, int workerCount, int queueMax, std::vector<std::string> opNames);
    ~CDbPipeline();

    CDbPipeline(const CDbPipeline&) = delete;
    CDbPipeline& operator=(const CDbPipeline&) = delete;

    // 연결을 전부 열고 워커를 띄운다. 하나라도 못 열면 false(서버가 뜨지 않는다 — 존과 같은 규칙)
    bool Start();

    // 큐에 남은 작업을 다 돌린 뒤 워커를 내린다. 회신은 더 안 꺼낸다(받을 쪽이 먼저 멈췄다)
    void Stop();

    // 로직 스레드 전용. 큐가 차면 false(작업은 안 돈다)
    //   work 는 워커 스레드에서 돈다 — 결과를 잡은 회신 함수를 돌려준다
    bool Submit(int64_t key, int opIndex, WorkFn work);

    // 결과 값 하나를 돌려주는 흔한 꼴 — 워커에서 work(conn) · 로직 스레드에서 reply(결과)
    template <typename Result>
    bool Run(int64_t key, int opIndex, std::function<Result(CDbConnection&)> work, std::function<void(Result&)> reply)
    {
        return Submit(key, opIndex,
            [work = std::move(work), reply = std::move(reply)](CDbConnection& conn) -> ReplyFn
            {
                auto result = std::make_shared<Result>(work(conn));
                return [reply, result]() { reply(*result); };
            });
    }

    // 쌓인 회신을 지금 스레드에서 부른다 — 로직 스레드 전용. 부른 수를 돌려준다
    int DrainReplies();

    // 지표 스레드
    void WriteMetrics(std::ostringstream& ss) const;

    int64_t FailedCount() const { return _failed.load(std::memory_order_relaxed); }

private:
    struct Job
    {
        int opIndex = 0;
        WorkFn work;
    };

    struct Worker
    {
        explicit Worker(const DBConfig& config) : connection(config) {}

        CDbConnection connection;   // 이 워커 스레드만 쓴다
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Job> queue;
        std::atomic<int64_t> depth{ 0 };
        std::thread thread;
    };

    void WorkerThread(Worker& worker);
    int  WorkerIndexOf(int64_t key) const;

    DBConfig _config;
    int _workerCount;
    int _queueMax;
    std::vector<std::string> _opNames;
    std::vector<std::unique_ptr<CLatencyHistogram>> _opLatency;   // _opNames 와 같은 차례

    std::vector<std::unique_ptr<Worker>> _workers;
    std::atomic<bool> _stop{ false };
    bool _libInited = false;

    std::mutex _replyMutex;
    std::vector<ReplyFn> _replies;
    std::vector<ReplyFn> _localReplies;   // 로직 스레드 전용 — 맞바꿔 잠금 구간을 줄인다

    std::atomic<int64_t> _failed{ 0 };
};
