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
struct DBSaveJob
{
    int64_t accountId;
    float   x;
    float   y;
    int32_t mapId;
};

// 워커 슬롯 — 스레드/커넥션/큐 1세트 (CIOCPServer::SendWorker 패턴).
//   mutex/cv는 이동불가라 unique_ptr로 힙 고정 후 포인터만 벡터에 보관.
struct DBWorkerSlot
{
    std::thread              thread;
    std::mutex               mutex;
    std::condition_variable  cv;
    std::vector<DBSaveJob>   queue;           // 게임 push / 워커 swap-out
    void*                    mysql = nullptr; // 실제 MYSQL* (헤더 오염 차단 위해 void*)
    void*                    stmt  = nullptr; // MYSQL_STMT* — prepared 핸들 (커넥션 종속)
    int64_t                  bAccountId = 0;  // ↓ stmt 파라미터 바인딩 버퍼 (execute 직전 잡 값 복사)
    float                    bX = 0.0f;
    float                    bY = 0.0f;
    int32_t                  bMapId = 0;
    int                      index = 0;
};

// K개 워커 스레드 + 커넥션. accountId % K 고정 분배(같은 유저=같은 워커=UPSERT 순서 보장).
//   백프레셔: 슬롯 큐가 queueMax 도달 시 초과분 드롭(카운터+ERROR). DB는 관대(다음 주기/종료가 커버).
class CDBWorker
{
public:
    explicit CDBWorker(CMonitorManager& monitor);
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

    // 모든 슬롯의 잔여 잡을 비우고 워커 종료 (멱등 — 소멸자에서도 호출).
    void Shutdown(int drainTimeoutSec);

private:
    void WorkerThread(int idx);
    bool Connect(void*& outMysql);            // 슬롯 커넥션 1개 생성
    bool PrepareStmt(DBWorkerSlot& slot);     // slot.mysql로 prepared stmt 준비 + 파라미터 바인딩
    bool ExecSave(DBWorkerSlot& slot, const DBSaveJob& job);   // 커넥션 유실 시 stmt/mysql을 null로(재연결은 WorkerThread)
    // 백프레셔 포함 — 큐 여유만큼만 수용하고 실제 수용 개수를 반환 (나머지는 드롭).
    size_t PushToSlot(DBWorkerSlot& slot, const DBSaveJob* jobs, size_t count);

    int SlotIndex(int64_t accountId) const    // accountId % K (음수 방어)
    {
        return static_cast<int>(static_cast<uint64_t>(accountId) % static_cast<uint64_t>(_workerCount));
    }

    CMonitorManager& _monitor;
    DBConfig         _config;
    int              _workerCount = 1;
    int              _queueMax    = 20000;

    std::vector<std::unique_ptr<DBWorkerSlot>> _workers;
    std::atomic<bool> _stop{ false };
    bool              _started = false;
    bool              _libInited = false;   // mysql_library_init 성공 여부 (library_end 짝 맞춤)

    // EnqueueBatch 분류 버퍼 재사용 (게임스레드/종료 시 순차 접근 — 동시 호출 없음).
    std::vector<std::vector<DBSaveJob>> _perWorker;
    // _perWorker와 나란한 원본 batch 인덱스 — 드롭분을 호출부에 되돌려주기 위함
    // (PushToSlot은 앞에서부터 수용하므로 드롭분은 각 슬롯 목록의 꼬리).
    std::vector<std::vector<size_t>>    _perWorkerIdx;
};

#endif // USE_DB_WORKER
