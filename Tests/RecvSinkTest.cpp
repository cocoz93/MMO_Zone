// 받은 것 넘길 곳 시험 — 부품을 주면 접속 · 받음 · 끊김이 큐를 안 거치고 부품으로 가는지
//   서버를 하나 띄운다(CIOCPServer 는 프로세스에 하나 — Start 가 공용 메시지 풀을 덮어쓴다. 그래서 framer_test 와 따로 둔다).
//   부품은 받은 메시지를 그대로 되돌려 보낸다(에코) — ref 를 받아 RequestSendMsg 로 넘기는 길까지 본다.
//   같은 서버로 쉬는 접속 끊기(SetIdleTimeoutSec 2초)도 본다. 끄기(0)는 framer_test ④ 서버가 본다.
//   외부 라이브러리 없음, 실패하면 종료 값 1.
#include "TestHelp.h"   // WinSock2 — Windows.h 보다 먼저 와야 해서 맨 앞

#include "IOCPServer.h"
#include "RecvSink.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/select.h>
#endif

void SignalProcessShutdown() {}   // 코어가 부르는 종료 알림 — 시험에는 기다리는 쪽이 없다

using TestHelp::Check;

namespace
{
    // 고른 값: 쓰는 곳이 없는 높은 번호(코어 시험 16300 · 16301 · 자르기 시험 16310)
    constexpr int PORT_MAIN   = 16320;
    constexpr int PORT_TAGGED = 16321;
    constexpr int TAG         = 7;
    constexpr int IDLE_SEC    = 2;   // 쉬는 접속 끊기 — 짧게 둬 시험이 몇 초에 끝나게(앞 시험들은 1초 안에 끝나 안 걸린다)

    // 한 세션에 일어난 일을 차례대로 적는다 — 'C' 접속 · 'R' 받음 · 'D' 끊김
    struct Trace
    {
        int64_t     sessionId = 0;
        int         tag       = -1;
        std::string order;
        std::vector<std::string> received;
    };

    class CRecordingSink final : public CRecvSink
    {
    public:
        explicit CRecordingSink(CIOCPServer*& network) : _network(network) {}

        void OnConnected(const SinkConnected& info) override
        {
            std::lock_guard<std::mutex> lock(_lock);
            Trace& t = Find(info.sessionId);
            t.tag = info.tag;
            t.order += 'C';
            if (std::this_thread::get_id() == _mainThread)
                _onMainThread = true;
        }

        void OnReceived(const SinkReceived& info) override
        {
            {
                std::lock_guard<std::mutex> lock(_lock);
                Trace& t = Find(info.sessionId);
                t.order += 'R';
                if (info.tag != t.tag)
                    _tagMismatch = true;
                t.received.emplace_back(info.msg->GetReadBufferPtr(), static_cast<size_t>(info.msg->GetDataSize()));
                if (std::this_thread::get_id() == _mainThread)
                    _onMainThread = true;
            }
            _network->RequestSendMsg(info.sessionId, info.msg);   // ref 하나를 넘긴다(실패해도 거기서 돌려준다)
        }

        void OnDisconnected(const SinkDisconnected& info) override
        {
            std::lock_guard<std::mutex> lock(_lock);
            Trace& t = Find(info.sessionId);
            t.order += 'D';
            if (info.tag != t.tag)
                _tagMismatch = true;
        }

        std::vector<Trace> Snapshot()
        {
            std::lock_guard<std::mutex> lock(_lock);
            return _traces;
        }

        bool TagMismatch()  { std::lock_guard<std::mutex> lock(_lock); return _tagMismatch; }
        bool OnMainThread() { std::lock_guard<std::mutex> lock(_lock); return _onMainThread; }

    private:
        Trace& Find(int64_t sessionId)
        {
            for (Trace& t : _traces)
                if (t.sessionId == sessionId)
                    return t;
            _traces.push_back(Trace{});
            _traces.back().sessionId = sessionId;
            return _traces.back();
        }

        CIOCPServer*&      _network;   // 서버가 부품을 쥐므로 부품 생성 때는 아직 주소만 — 시험이 Start 전에 채운다
        std::mutex         _lock;
        std::vector<Trace> _traces;
        bool               _tagMismatch  = false;
        bool               _onMainThread = false;
        const std::thread::id _mainThread = std::this_thread::get_id();
    };

    // 전체 길이 머리(4바이트: 전체 크기 2 · 종류 2) + 몸
    std::string Packet(uint16_t type, const std::string& body)
    {
        constexpr size_t HEADER = sizeof(MsgHeader);   // 4 — 직렬 버퍼 머리 HEADER_SIZE(2) 와 다르다
        const uint16_t size = static_cast<uint16_t>(HEADER + body.size());
        std::string out(HEADER, '\0');
        out[0] = static_cast<char>(size & 0xFF);
        out[1] = static_cast<char>(size >> 8);
        out[2] = static_cast<char>(type & 0xFF);
        out[3] = static_cast<char>(type >> 8);
        return out + body;
    }

    bool Readable(SOCKET s, int timeoutMs)
    {
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(s, &reads);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        return ::select(static_cast<int>(s) + 1, &reads, nullptr, nullptr, &tv) > 0;
    }

    std::string RecvExactly(SOCKET s, size_t size, int timeoutMs)
    {
        std::string out;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        char buf[4096];
        while (out.size() < size && std::chrono::steady_clock::now() < until)
        {
            if (!Readable(s, 50))
                continue;
            const int n = ::recv(s, buf, static_cast<int>(sizeof(buf)), 0);
            if (n <= 0)
                break;
            out.append(buf, static_cast<size_t>(n));
        }
        return out;
    }

    bool SendAll(SOCKET s, const std::string& bytes)
    {
        size_t sent = 0;
        while (sent < bytes.size())
        {
            const int n = ::send(s, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
            if (n <= 0)
                return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    const Trace* FindTrace(const std::vector<Trace>& traces, int tag)
    {
        for (const Trace& t : traces)
            if (t.tag == tag)
                return &t;
        return nullptr;
    }

    void SinkTests()
    {
        CMonitorManager monitor;
        CIOCPServer* networkPtr = nullptr;
        auto sinkOwned = std::make_unique<CRecordingSink>(networkPtr);
        CRecordingSink* sink = sinkOwned.get();

        CIOCPServer server(PORT_MAIN, 8, "RecvSinkTest", monitor, 2, 2, 2, 0, 1);
        networkPtr = &server;
        Check(server.AddListener("127.0.0.1", PORT_TAGGED, TAG), "AddListener — 구분 값 7 포트");
        Check(server.SetRecvSink(std::move(sinkOwned)), "SetRecvSink — Start 전에는 받음");
        Check(!server.SetIdleTimeoutSec(-1), "SetIdleTimeoutSec — 음수 거절");
        Check(!server.SetIdleTimeoutSec(CIOCPServer::MAX_IDLE_TIMEOUT_SEC + 1), "SetIdleTimeoutSec — 상한 + 1 거절");
        Check(server.SetIdleTimeoutSec(CIOCPServer::MAX_IDLE_TIMEOUT_SEC), "SetIdleTimeoutSec — 상한 딱은 받음");
        Check(server.SetIdleTimeoutSec(IDLE_SEC), "SetIdleTimeoutSec — 2초");
        if (!server.Start())
        {
            Check(false, "Start");
            return;
        }
        Check(!server.SetRecvSink(nullptr), "SetRecvSink — Start 뒤에는 거절");
        Check(!server.SetIdleTimeoutSec(0), "SetIdleTimeoutSec — Start 뒤에는 거절");

        // ── 생성자 포트(구분 값 0) — 셋 보내고 셋 돌려받기 ──
        SOCKET a = TestHelp::ConnectRaw("127.0.0.1", PORT_MAIN);
        Check(a != INVALID_SOCKET, "접속 — 생성자 포트");
        const std::string p1 = Packet(1, "one");
        const std::string p2 = Packet(2, std::string(1000, 'x'));
        const std::string p3 = Packet(3, "");
        Check(SendAll(a, p1 + p2 + p3), "메시지 셋 보냄(한 번에 붙여서)");
        Check(RecvExactly(a, p1.size() + p2.size() + p3.size(), 3000) == p1 + p2 + p3,
              "부품이 받은 메시지를 그대로 되돌려 보냄 — 셋 그대로 · 차례 그대로");

        std::vector<Trace> traces = sink->Snapshot();
        const Trace* ta = FindTrace(traces, 0);
        Check(ta != nullptr && ta->order == "CRRR", "생성자 포트 세션 — 접속 1 · 받음 3 (" + (ta ? ta->order : std::string("없음")) + ")");
        Check(ta != nullptr && ta->received.size() == 3 && ta->received[0] == p1 && ta->received[1] == p2 && ta->received[2] == p3,
              "받음 셋 — 머리째 그대로");

        // ── 세션별 큐를 정해도 부품이 먼저다 ──
        ThreadSafeQueue<NetworkEvent> sessionQueue;
        Check(ta != nullptr && server.SetSessionRecvQueue(ta->sessionId, &sessionQueue), "세션별 큐 정함");
        const std::string p4 = Packet(4, "after-queue");
        Check(SendAll(a, p4) && RecvExactly(a, p4.size(), 3000) == p4, "세션별 큐를 정한 뒤에도 부품이 되돌려 보냄");
        Check(sessionQueue.GetSize() == 0, "세션별 큐는 비어 있음(" + std::to_string(sessionQueue.GetSize()) + ")");

        // ── 구분 값 7 포트 ──
        SOCKET b = TestHelp::ConnectRaw("127.0.0.1", PORT_TAGGED);
        Check(b != INVALID_SOCKET, "접속 — 구분 값 7 포트");
        const std::string q1 = Packet(9, "tagged");
        Check(SendAll(b, q1) && RecvExactly(b, q1.size(), 3000) == q1, "구분 값 7 세션도 되돌려 받음");
        traces = sink->Snapshot();
        const Trace* tb = FindTrace(traces, TAG);
        Check(tb != nullptr && tb->order == "CR", "구분 값 7 세션 — 접속 1 · 받음 1 (" + (tb ? tb->order : std::string("없음")) + ")");

        // ── 끊김 — 둘 다 받음 뒤에 한 번 ──
        Platform::CloseSocket(a);
        Platform::CloseSocket(b);
        TestHelp::WaitUntil([&]
        {
            const std::vector<Trace> now = sink->Snapshot();
            const Trace* x = FindTrace(now, 0);
            const Trace* y = FindTrace(now, TAG);
            return x && y && !x->order.empty() && x->order.back() == 'D' && !y->order.empty() && y->order.back() == 'D';
        });
        traces = sink->Snapshot();
        ta = FindTrace(traces, 0);
        tb = FindTrace(traces, TAG);
        Check(ta != nullptr && ta->order == "CRRRRD", "생성자 포트 세션 — 끝 차례 접속 · 받음 4 · 끊김 (" + (ta ? ta->order : std::string("없음")) + ")");
        Check(tb != nullptr && tb->order == "CRD", "구분 값 7 세션 — 끝 차례 접속 · 받음 · 끊김 (" + (tb ? tb->order : std::string("없음")) + ")");
        Check(!sink->TagMismatch(), "받음 · 끊김의 구분 값이 접속 때와 같음");
        Check(!sink->OnMainThread(), "부품은 시험 스레드가 아닌 서버 스레드에서 불림");

        // ── 이벤트 큐에는 아무것도 없다 ──
        NetworkEvent event(NetworkEvent::Type::CONNECTED, 0);
        int popped = 0;
        while (server.PopNetworkEvent(event))
        {
            ++popped;
            if (event.pMsg != nullptr)
                event.pMsg->SubRef();
            event.pMsg = nullptr;
        }
        Check(popped == 0, "이벤트 큐 — 꺼낸 것 0 (" + std::to_string(popped) + ")");

        // ── 쉬는 접속 끊기(2초) — 아무것도 안 보내는 세션은 끊기고, 0.5초마다 보내는 세션은 남는다 ──
        //   끊기는 때 = 2 ~ 3초(1초 단위 · TimingWheel.h Init). 시험 기계가 바쁠 때를 봐 위로 1초 더 둔다
        const int64_t timedOut0 = monitor._sessionTimedOut.Load();
        const auto idleStart = std::chrono::steady_clock::now();
        SOCKET idle = TestHelp::ConnectRaw("127.0.0.1", PORT_MAIN);
        SOCKET busy = TestHelp::ConnectRaw("127.0.0.1", PORT_MAIN);
        Check(idle != INVALID_SOCKET && busy != INVALID_SOCKET, "쉬는 세션 · 바쁜 세션 접속");
        bool idleClosed = false;
        bool busyEchoOk = true;
        double idleClosedSec = 0.0;
        const std::string ping = Packet(5, "ping");
        while (std::chrono::steady_clock::now() - idleStart < std::chrono::milliseconds(5000))
        {
            if (!SendAll(busy, ping) || RecvExactly(busy, ping.size(), 1000) != ping)
                busyEchoOk = false;
            if (!idleClosed && Readable(idle, 500))
            {
                char buf[16];
                if (::recv(idle, buf, static_cast<int>(sizeof(buf)), 0) <= 0)
                {
                    idleClosed = true;
                    idleClosedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - idleStart).count();
                }
            }
            else if (idleClosed)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        char secText[32];
        std::snprintf(secText, sizeof(secText), "%.2f", idleClosedSec);
        Check(idleClosed && idleClosedSec >= IDLE_SEC && idleClosedSec <= IDLE_SEC + 2.0,
              std::string("쉬는 세션 — 2 ~ 4초 안에 서버가 끊음 (") + (idleClosed ? secText : "안 끊김") + "초)");
        Check(busyEchoOk, "바쁜 세션 — 5초 내내 0.5초마다 보내고 돌려받음");
        Check(monitor._sessionTimedOut.Load() - timedOut0 == 1,
              "쉬어서 끊긴 수 1 (" + std::to_string(monitor._sessionTimedOut.Load() - timedOut0) + ")");
        Platform::CloseSocket(idle);
        Platform::CloseSocket(busy);
        TestHelp::WaitUntil([&]
        {
            const std::vector<Trace> now = sink->Snapshot();
            return now.size() == 4 && !now[3].order.empty() && now[3].order.back() == 'D';
        });

        // ── 서버 종료 중에는 끊김을 안 부른다 — 붙어 있는 세션 하나를 남기고 끈다 ──
        SOCKET c = TestHelp::ConnectRaw("127.0.0.1", PORT_MAIN);
        Check(c != INVALID_SOCKET, "종료 전 접속 하나");
        TestHelp::WaitUntil([&] { return sink->Snapshot().size() == 5; });
        const size_t sessionsBefore = sink->Snapshot().size();
        server.ShutdownServer();
        traces = sink->Snapshot();
        Check(sessionsBefore == 5 && traces.size() == 5 && traces[4].order == "C",
              "종료 중 — 남은 세션에 끊김을 안 부름 (" + (traces.size() == 5 ? traces[4].order : std::string("세션 수 ") + std::to_string(traces.size())) + ")");
        Platform::CloseSocket(c);
    }
}

int main()
{
    SinkTests();
    return TestHelp::Finish("recv_sink_test");
}
