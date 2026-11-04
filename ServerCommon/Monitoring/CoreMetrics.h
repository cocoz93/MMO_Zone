#pragma once
// ==========================================================================
// CCoreMetrics — 코어(CMonitorManager)의 네트워크·세션·처리 지연·스레드 CPU 값을 지표 글자로 만든다
//
//   세 서버가 같은 이름으로 내고 프로메테우스의 job 으로 가른다.
//   틱·방송·전투처럼 게임 뜻이 있는 값은 CMonitorServer 몫이다.
//   Write 는 CMetricsEndpoint 의 뮤텍스 아래에서 한 번에 하나만 돈다 — CPU 직전 샘플을 잠금 없이 고친다.
// ==========================================================================

#include <cstdint>
#include <sstream>

#include "Platform/Platform.h"
#include "MonitorManager.h"

class CCoreMetrics
{
public:
    explicit CCoreMetrics(CMonitorManager& monitor) : _monitor(monitor) {}

    CCoreMetrics(const CCoreMetrics&) = delete;
    CCoreMetrics& operator=(const CCoreMetrics&) = delete;

    void Write(std::ostringstream& ss);

private:
    void WriteCounters(std::ostringstream& ss);
    void WriteGauges(std::ostringstream& ss);
    void WriteSendWorkerFlush(std::ostringstream& ss);
    void WriteHandleLatencyHistogram(std::ostringstream& ss);
    void WriteWorkerCounters(std::ostringstream& ss);
    void WriteThreadCpu(std::ostringstream& ss);

    // ══════════════════════════════════════════════════════════════
    // 스레드별 CPU 점유율 (gauge, 1.0 = 코어 1개 풀)
    //
    // HTTP 스레드(외부 관측자)가 각 스레드 핸들에 GetThreadTimes를 호출.
    // 두 스크레이프 사이의 ΔCPU시간 / Δ벽시계시간 = 그 구간 평균 점유율.
    // 게임루프가 드워커 루프에 갇혀도 외부에서 읽으니 동결되지 않음(진단정리 6 보강).
    // CPU 시간·벽시계 모두 ns 단위 → 무차원 비율. (측정은 Platform 뒤로 격리, 벽시계는 steady_clock)
    // ══════════════════════════════════════════════════════════════
    struct CpuSample
    {
        uint64_t lastCpuNs = 0;
        uint64_t lastKernelNs = 0;      // 직전 커널모드 CPU 누적 (syscall 실행분 격리용)
        uint64_t lastWallNs = 0;
        bool primed = false;
    };

    void SampleThreadCpu(std::ostringstream& ss, std::ostringstream& ssKernel, const char* label,
                         Platform::ThreadCpuHandle h, CpuSample& s, uint64_t wallNow);

    CMonitorManager& _monitor;   // 코어 지표 저장소 (ServerCore)

    // CPU 점유율 직전 샘플 상태.
    //   핸들러가 httplib ThreadPool 워커에서 도는 탓에 "HTTP 스레드 단독 접근"이 성립하지 않는다.
    //   → CMetricsEndpoint 가 지표 글자 만들기 전체를 뮤텍스로 직렬화해 보호한다.
    CpuSample _cpuGameLoop;
    CpuSample _cpuWorker[CMonitorManager::MAX_WORKER_THREADS];
    CpuSample _cpuSendWorker[CMonitorManager::MAX_SEND_WORKERS];   // [USE_SEND_THREAD] 송신 워커별 CPU 직전 샘플
};
