#pragma once
// ==========================================================================
// CShutdownWait — 프로세스 종료 기다림 (Ctrl+C · 창 닫기 · 코어의 종료 알림)
//
//   세 서버의 main 이 같은 전역을 복사해 들지 않게 여기에 모은다.
//   프로세스에 하나뿐인 상태라 정적 함수로 둔다(콘솔 핸들러·코어 알림이 함수 포인터로 부른다).
//
// [사용법]
//   CShutdownWait::Install();        // 스레드를 띄우기 전에
//   ...서버 Start...
//   CShutdownWait::Wait();           // 종료 신호까지 막힌다
//   ...서버 Stop...
//   CShutdownWait::MarkCleanedUp();  // 콘솔 핸들러 해제
// ==========================================================================

#include <cstdint>

class CShutdownWait
{
public:
    // CTRL_CLOSE/LOGOFF/SHUTDOWN에서 정리를 기다릴 상한.
    //   이 이벤트들은 OS 유예(기본 5초) 후 프로세스를 강제 종료하므로 그보다 짧게 잡는다.
    static constexpr uint32_t WAIT_MAX_MS = 4500;

    // onWaitTimeout — 정리 기다림이 상한에 닿았을 때 콘솔 핸들러 스레드에서 1회. 반환하면 곧 OS 가 죽이므로
    //   남길 것(로그)은 이 안에서 내보내기까지 끝낸다. nullptr 이면 안 부른다.
    static void Install(void (*onWaitTimeout)() = nullptr);

    // 종료 신호를 받을 때까지 막힌다 — main 스레드 전용
    static void Wait();

    // 프로세스 전체 종료 알림 — 아무 스레드에서나 불러도 된다
    static void Signal();

    // main 이 정리를 끝냈다 — 콘솔 종료 핸들러가 이걸 기다린다
    static void MarkCleanedUp();
};
