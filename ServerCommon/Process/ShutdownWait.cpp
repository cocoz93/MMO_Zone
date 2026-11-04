#include "ShutdownWait.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "Platform/Platform.h"   // affinity·종료 시그널 격리

namespace
{
    std::atomic<bool> g_running{ true };
    std::mutex g_mutex;
    std::condition_variable g_cv;

    // main이 정리(server.Stop)를 끝냈는지 — 콘솔 종료 핸들러가 이걸 기다린다.
    std::atomic<bool> g_shutdownComplete{ false };
}

// 프로세스 전체 종료 컨트롤러이므로 메인문에 빼둔다
//   running 변경은 반드시 mutex 안에서 — 락 없이 바꾸면 main이 wait 술어를 평가한 직후
//   대기 큐에 등록되기 전 구간에 끼어들어 notify가 유실된다(기상 유실).
void CShutdownWait::Signal()
{
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_running = false;
    }
    g_cv.notify_one();
}

void CShutdownWait::Install(void (*onWaitTimeout)())
{
    Platform::InstallShutdownHandler(&CShutdownWait::Signal, &g_shutdownComplete, WAIT_MAX_MS, onWaitTimeout);
}

void CShutdownWait::Wait()
{
    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait(lock, [] { return !g_running; });
}

void CShutdownWait::MarkCleanedUp()
{
    g_shutdownComplete.store(true, std::memory_order_release);   // 콘솔 종료 핸들러 해제
}

// 코어가 부르는 종료 알림(IOCPServer.cpp 의 extern). 이 파일을 잇는 프로그램은 따로 정의하지 않는다
void SignalProcessShutdown()
{
    CShutdownWait::Signal();
}
