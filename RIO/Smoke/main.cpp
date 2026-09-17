// ==========================================================================
// RIO Phase 0 스모크 — 본구현 전 게이트 3종 검증 (루프백 자기연결, 독립 exe)
//
//  [1] RIO 함수 테이블 획득 (WSAIoctl SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER)
//  [2] accept()로 받은 소켓의 WSA_FLAG_REGISTERED_IO 상속 여부
//      → RIOCreateRequestQueue 성공이 판정 기준. 실패면 본설계는 AcceptEx 전환 필요.
//  [3a] 등록 버퍼(단일 슬랩+오프셋) 에코 왕복 + RIONotify 이벤트 통지
//  [3b] pending RIO 요청이 closesocket으로 에러 완료되어 CQ에 도착하는지
//      → CancelIoEx가 없는 RIO에서 종료 경로(드레인)의 근거. 미도착이면 설계 재검토.
//
//  주의(1차 실행 실측): WSA_FLAG_REGISTERED_IO 소켓은 RIO 전용 — 일반 send/recv가
//  거부된다(WSAENOTSOCK 10038 관찰). 따라서 peer(일반 I/O 상대)는 반드시 무플래그
//  소켓으로 만든다. 본서버 함의: RIO 세션 소켓에는 어떤 코드도 WSASend/WSARecv를
//  호출하면 안 된다 (전송층 일원화 전제와 일치).
//
//  출력은 파싱 편의상 영어. 모두 PASS면 exit 0.
// ==========================================================================
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <cstdio>
#include <cstring>

#include "../../ServerCore/Network/RioApi.h"   // 본서버용 로더/슬랩을 그대로 소비 (헤더 검증 겸용)

#pragma comment(lib, "ws2_32.lib")

// 생성한 소켓 전부 추적 — 정리 시 [3b]에서 닫은 것만 빼고 일괄 close
static SOCKET g_all[8];
static int    g_nAll = 0;
static void Track(SOCKET s) { if (g_nAll < 8) g_all[g_nAll++] = s; }

static void PrintWsaErr(const char* where)
{
    printf("    %s failed, WSAGetLastError=%d\n", where, WSAGetLastError());
}

// CQ에서 완료 1건 이상을 폴링으로 걷어온다 (판정용 — 이벤트와 도착을 분리 확인)
static ULONG PollDequeue(RIO_CQ cq, RIORESULT* results, ULONG cap, DWORD timeoutMs)
{
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs)
    {
        ULONG n = CRioApi::Rio().RIODequeueCompletion(cq, results, cap);
        if (n == RIO_CORRUPT_CQ)
        {
            printf("    RIODequeueCompletion returned RIO_CORRUPT_CQ\n");
            return 0;
        }
        if (n > 0)
            return n;
        Sleep(10);
    }
    return 0;
}

int main()
{
    int passCount = 0;
    bool gate1 = false, gate2 = false, gate3a = false, gate3b = false;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("WSAStartup failed\n");
        return 1;
    }

    printf("=== RIO Phase 0 Smoke ===\n");

    // ── 준비: RIO 플래그 리슨 소켓 (상속 테스트 대상) ──
    SOCKET listenSock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                                  WSA_FLAG_REGISTERED_IO | WSA_FLAG_OVERLAPPED);
    if (listenSock == INVALID_SOCKET)
    {
        PrintWsaErr("WSASocket(listen, WSA_FLAG_REGISTERED_IO)");
        printf("[FATAL] cannot even create a registered-IO socket\n");
        return 1;
    }
    Track(listenSock);

    // ── [1] RIO 함수 테이블 (RioApi.h — 본서버와 동일 로더를 여기서 검증) ──
    printf("[1] RIO function table via CRioApi::Load...\n");
    if (CRioApi::Load(listenSock))
    {
        gate1 = true;
        printf("    PASS\n");
    }
    else
    {
        PrintWsaErr("CRioApi::Load");
        printf("    FAIL -- RIO unavailable on this system\n");
        WSACleanup();
        return 1;
    }

    // ── 루프백 자기연결: bind(포트 0 자동) → listen → connect → accept ──
    // peer(일반 I/O 상대)는 무플래그 소켓이어야 한다 (파일 상단 주의 참조)
    SOCKADDR_IN addr = {};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = 0;
    if (bind(listenSock, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(listenSock, 1) == SOCKET_ERROR)
    {
        PrintWsaErr("bind/listen");
        return 1;
    }
    int addrLen = sizeof(addr);
    getsockname(listenSock, (SOCKADDR*)&addr, &addrLen);   // 할당된 포트 회수

    SOCKET cliSock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                               WSA_FLAG_OVERLAPPED);       // 무플래그 = 일반 I/O 가능
    if (cliSock == INVALID_SOCKET ||
        connect(cliSock, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        PrintWsaErr("connect");
        return 1;
    }
    Track(cliSock);
    SOCKET srvSock = accept(listenSock, NULL, NULL);
    if (srvSock == INVALID_SOCKET)
    {
        PrintWsaErr("accept");
        return 1;
    }
    Track(srvSock);

    // ── CQ 생성 (이벤트 통지 — 본설계와 동일한 auto-reset + NotifyReset) ──
    HANDLE cqEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    RIO_NOTIFICATION_COMPLETION nc = {};
    nc.Type = RIO_EVENT_COMPLETION;
    nc.Event.EventHandle = cqEvent;
    nc.Event.NotifyReset = TRUE;
    RIO_CQ cq = CRioApi::Rio().RIOCreateCompletionQueue(64, &nc);
    if (cq == RIO_INVALID_CQ)
    {
        PrintWsaErr("RIOCreateCompletionQueue");
        return 1;
    }

    // ── [2] accept 소켓의 REGISTERED_IO 상속 — RQ 생성 성공 여부로 판정 ──
    printf("[2] RIOCreateRequestQueue on accept()-ed socket...\n");
    SOCKET rioSock = INVALID_SOCKET;   // RIO 제출을 수행할 쪽
    SOCKET peerSock = INVALID_SOCKET;  // 일반 send/recv 상대 (반드시 무플래그)
    SOCKET closedOne = INVALID_SOCKET; // [3b]에서 닫은 소켓 (정리 시 이중 close 방지)
    RIO_RQ rq = CRioApi::Rio().RIOCreateRequestQueue(srvSock, 1, 1, 1, 1, cq, cq, (void*)1);
    if (rq != RIO_INVALID_RQ)
    {
        gate2 = true;
        rioSock = srvSock;
        peerSock = cliSock;
        printf("    PASS -- accept() inherits WSA_FLAG_REGISTERED_IO\n");
    }
    else
    {
        PrintWsaErr("RIOCreateRequestQueue(accepted)");
        printf("    FAIL -- accept() does NOT inherit the flag. Main design needs AcceptEx.\n");

        // [3] 속행 폴백: 무플래그 리슨 + RIO 플래그 아웃바운드 접속
        //     (아웃바운드가 RIO쪽, accept된 쪽이 일반 peer)
        SOCKET listen2 = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                                   WSA_FLAG_OVERLAPPED);
        SOCKET rioOut = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                                  WSA_FLAG_REGISTERED_IO | WSA_FLAG_OVERLAPPED);
        SOCKADDR_IN addr2 = {};
        addr2.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &addr2.sin_addr);
        addr2.sin_port = 0;
        if (listen2 == INVALID_SOCKET || rioOut == INVALID_SOCKET ||
            bind(listen2, (SOCKADDR*)&addr2, sizeof(addr2)) == SOCKET_ERROR ||
            listen(listen2, 1) == SOCKET_ERROR)
        {
            PrintWsaErr("fallback listen setup");
            return 1;
        }
        Track(listen2);
        Track(rioOut);
        int a2len = sizeof(addr2);
        getsockname(listen2, (SOCKADDR*)&addr2, &a2len);
        if (connect(rioOut, (SOCKADDR*)&addr2, sizeof(addr2)) == SOCKET_ERROR)
        {
            PrintWsaErr("fallback connect");
            return 1;
        }
        SOCKET peer2 = accept(listen2, NULL, NULL);
        if (peer2 == INVALID_SOCKET)
        {
            PrintWsaErr("fallback accept");
            return 1;
        }
        Track(peer2);

        rq = CRioApi::Rio().RIOCreateRequestQueue(rioOut, 1, 1, 1, 1, cq, cq, (void*)2);
        if (rq == RIO_INVALID_RQ)
        {
            PrintWsaErr("RIOCreateRequestQueue(outbound fallback)");
            printf("[FATAL] RQ creation failed on both paths\n");
            return 1;
        }
        rioSock = rioOut;
        peerSock = peer2;
    }

    // ── 등록 버퍼: CRioSlab (본서버와 동일 — 단일 슬랩 1회 등록 + 오프셋 슬라이스) ──
    const size_t SLAB_SIZE = 128 * 1024;     // recv 64K + send 64K
    const ULONG RECV_OFF = 0;
    const ULONG SEND_OFF = 64 * 1024;
    CRioSlab slabObj;
    if (!slabObj.Init(SLAB_SIZE))
    {
        PrintWsaErr("CRioSlab::Init");
        return 1;
    }
    char* slab = slabObj.Base();             // 이하 코드는 기존 slab/bufId 이름 그대로 사용
    RIO_BUFFERID bufId = slabObj.BufferId();

    // ── [3a] 에코 왕복: peer --PING--> rioSock --PONG--> peer + RIONotify ──
    printf("[3a] registered-buffer echo + RIONotify event...\n");
    do
    {
        RIO_BUF recvBuf = { bufId, RECV_OFF, 4096 };
        if (!CRioApi::Rio().RIOReceive(rq, &recvBuf, 1, 0, (void*)0x0EC1)) { PrintWsaErr("RIOReceive"); break; }

        if (send(peerSock, "PING", 4, 0) != 4) { PrintWsaErr("send(peer)"); break; }

        // RIONotify 무장 — CQ에 이미 완료가 있으면 즉시 시그널되는 게 정상 동작
        INT nr1 = CRioApi::Rio().RIONotify(cq);
        INT nr2 = CRioApi::Rio().RIONotify(cq);   // 정보: 중복 무장 반환값 (본구현 idle 루프 참고용)
        printf("    RIONotify x2 returned: %d, %d\n", nr1, nr2);

        DWORD w = WaitForSingleObject(cqEvent, 3000);
        printf("    cqEvent wait -> %s\n", (w == WAIT_OBJECT_0) ? "signaled" : "TIMEOUT");
        if (w != WAIT_OBJECT_0) break;

        RIORESULT results[8] = {};
        ULONG n = PollDequeue(cq, results, 8, 1000);
        if (n == 0) { printf("    no completion dequeued\n"); break; }
        if (results[0].Status != 0 || results[0].BytesTransferred != 4 ||
            memcmp(slab + RECV_OFF, "PING", 4) != 0)
        {
            printf("    unexpected recv completion: status=%ld bytes=%lu\n",
                   results[0].Status, results[0].BytesTransferred);
            break;
        }

        memcpy(slab + SEND_OFF, "PONG", 4);
        RIO_BUF sendBuf = { bufId, SEND_OFF, 4 };
        if (!CRioApi::Rio().RIOSend(rq, &sendBuf, 1, 0, (void*)0x5E4D)) { PrintWsaErr("RIOSend"); break; }

        char echo[8] = {};
        int r = recv(peerSock, echo, 4, 0);
        if (r != 4 || memcmp(echo, "PONG", 4) != 0)
        {
            printf("    peer recv mismatch (r=%d)\n", r);
            break;
        }

        // send 완료도 걷어서 CQ를 비워둔다 ([3b] 판정 오염 방지)
        ULONG n2 = PollDequeue(cq, results, 8, 1000);
        if (n2 == 0) { printf("    send completion missing\n"); break; }

        gate3a = true;
        printf("    PASS\n");
    } while (false);

    // ── [3b] 드레인: pending recv 상태에서 closesocket → 에러 완료 도착? ──
    printf("[3b] closesocket drains pending RIO request into CQ...\n");
    if (gate3a)
    {
        RIO_BUF recvBuf2 = { bufId, RECV_OFF, 4096 };
        if (!CRioApi::Rio().RIOReceive(rq, &recvBuf2, 1, 0, (void*)0xDEAD))
        {
            PrintWsaErr("RIOReceive(pending for close)");
        }
        else
        {
            INT nr = CRioApi::Rio().RIONotify(cq);
            (void)nr;
            closedOne = rioSock;
            closesocket(rioSock);
            rioSock = INVALID_SOCKET;

            DWORD w = WaitForSingleObject(cqEvent, 2000);
            printf("    cqEvent wait after closesocket -> %s\n",
                   (w == WAIT_OBJECT_0) ? "signaled" : "TIMEOUT");

            RIORESULT results[8] = {};
            ULONG n = PollDequeue(cq, results, 8, 2000);
            if (n > 0)
            {
                gate3b = true;
                printf("    PASS -- completion arrived: status=%ld bytes=%lu ctx=0x%llX\n",
                       results[0].Status, results[0].BytesTransferred,
                       (unsigned long long)results[0].RequestContext);
            }
            else
            {
                printf("    FAIL -- pending request did NOT complete after closesocket\n");
            }
        }
    }
    else
    {
        printf("    SKIP (3a failed)\n");
    }

    // ── 정리 — [3b]에서 닫은 소켓만 빼고 일괄 close → CQ → 버퍼 → 슬랩 ──
    for (int i = 0; i < g_nAll; ++i)
    {
        if (g_all[i] != INVALID_SOCKET && g_all[i] != closedOne)
            closesocket(g_all[i]);
    }
    CRioApi::Rio().RIOCloseCompletionQueue(cq);
    slabObj.Release();   // 등록해제 + VirtualFree — WSACleanup 전에 명시 호출 (본서버도 동일 순서)
    CloseHandle(cqEvent);
    WSACleanup();

    passCount = (gate1 ? 1 : 0) + (gate2 ? 1 : 0) + (gate3a ? 1 : 0) + (gate3b ? 1 : 0);
    printf("=== GATE RESULT: [1]%s [2]%s [3a]%s [3b]%s -- %d/4 ===\n",
           gate1 ? "PASS" : "FAIL", gate2 ? "PASS" : "FAIL",
           gate3a ? "PASS" : "FAIL", gate3b ? "PASS" : "FAIL", passCount);
    return (passCount == 4) ? 0 : 2;
}
