// 코어 시험 — AddListener · ConnectTo · 구분 값 · 구분 값 0 상한 · 따로 세기
//   공개 코어 저장소가 아니라 쓰는 쪽(이 저장소)에 둔다. 외부 라이브러리 없음, 실패하면 종료 값 1.
//   CIOCPServer는 프로세스에 하나라서(Start가 공용 메시지 풀을 덮어쓴다) 한 서버가 자기 포트에 접속해 왕복을 본다.
#include "IOCPServer.h"
#include "NetIoModel.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#include <sys/select.h>
#endif

void SignalProcessShutdown() {}   // 코어가 부르는 종료 알림 — 시험에는 기다리는 쪽이 없다

namespace
{
    // 고른 값: 쓰는 곳이 없는 높은 번호. 정원은 시험이 금방 채울 만큼 작게.
    constexpr int PORT_WEB = 16300;
    constexpr int PORT_LINK = 16301;
    constexpr int WEB_LIMIT = 4;
    constexpr int RESERVE = 6;
    constexpr int TAG_UP = 1;     // 내가 건 연결
    constexpr int TAG_DOWN = 2;   // 내가 받은 연결
    constexpr int QUEUE_LIMIT = 1000;

    int g_failed = 0;
    int g_checks = 0;

    void Check(bool ok, const std::string& what)
    {
        ++g_checks;
        if (!ok)
            ++g_failed;
        std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
        std::fflush(stdout);
    }

    struct Seen
    {
        NetworkEvent::Type type;
        int tag;
        int64_t sessionId;
        std::string payload;   // RECEIVED만 — 머리 뒤 내용
    };

    class CEvents
    {
    public:
        explicit CEvents(CIOCPServer& server, CMonitorManager& monitor) : _server(server), _monitor(monitor) {}

        // 조건에 맞는 이벤트를 찾아 꺼낸다. 시한까지 없으면 false
        template <typename Pred>
        bool Take(Pred pred, Seen* out = nullptr, int timeoutMs = 3000)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            for (;;)
            {
                Pump();
                for (auto it = _seen.begin(); it != _seen.end(); ++it)
                {
                    if (pred(*it))
                    {
                        if (out != nullptr)
                            *out = *it;
                        _seen.erase(it);
                        return true;
                    }
                }
                if (std::chrono::steady_clock::now() >= until)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        bool TakeOf(NetworkEvent::Type type, int tag, Seen* out = nullptr, int timeoutMs = 3000)
        {
            return Take([&](const Seen& s) { return s.type == type && s.tag == tag; }, out, timeoutMs);
        }

        bool TakeSession(NetworkEvent::Type type, int64_t sessionId, Seen* out = nullptr, int timeoutMs = 3000)
        {
            return Take([&](const Seen& s) { return s.type == type && s.sessionId == sessionId; }, out, timeoutMs);
        }

        size_t PendingCount() { Pump(); return _seen.size(); }
        std::string Dump()
        {
            Pump();
            std::string text;
            for (const Seen& s : _seen)
                text += "(" + std::to_string(static_cast<int>(s.type)) + " tag=" + std::to_string(s.tag) + ") ";
            return text;
        }
        bool SawNegative() const { return _sawNegative; }

    private:
        void Pump()
        {
            if (_monitor._sessionCount.Load() < 0 || _monitor._taggedSessionCount.Load() < 0)
                _sawNegative = true;

            NetworkEvent event(NetworkEvent::Type::CONNECTED, 0);
            while (_server.PopNetworkEvent(event))
            {
                Seen s{ event.type, event.tag, event.sessionId, {} };
                if (event.pMsg != nullptr)
                {
                    const int size = event.pMsg->GetDataSize();
                    if (size > static_cast<int>(sizeof(MsgHeader)))
                        s.payload.assign(event.pMsg->GetReadBufferPtr() + sizeof(MsgHeader), size - sizeof(MsgHeader));
                    event.pMsg->SubRef();
                    event.pMsg = nullptr;
                }
                _seen.push_back(std::move(s));
            }
        }

        CIOCPServer& _server;
        CMonitorManager& _monitor;
        std::deque<Seen> _seen;
        bool _sawNegative = false;
    };

    std::string MakePayload(int size, int seed)
    {
        std::string text(static_cast<size_t>(size), '\0');
        for (int i = 0; i < size; ++i)
            text[static_cast<size_t>(i)] = static_cast<char>((seed + i * 31) & 0xFF);
        return text;
    }

    bool Send(CIOCPServer& server, int64_t sessionId, const std::string& payload, SendFlush flush = SendFlush::Immediate)
    {
        CSerialBuffer* msg = CSerialBuffer::Alloc();
        MsgHeader header;
        header.size = static_cast<uint16_t>(sizeof(MsgHeader) + payload.size());
        header.type = static_cast<MsgType>(3000);
        std::memcpy(msg->GetWriteBufferPtr(), &header, sizeof(header));
        msg->MoveWritePos(static_cast<int>(sizeof(header)));
        if (!payload.empty())
        {
            std::memcpy(msg->GetWriteBufferPtr(), payload.data(), payload.size());
            msg->MoveWritePos(static_cast<int>(payload.size()));
        }
        return server.RequestSendMsg(sessionId, msg, flush);
    }

    SOCKET ConnectRaw(const char* ip, int port)
    {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            return INVALID_SOCKET;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, ip, &addr.sin_addr);
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            Platform::CloseSocket(s);
            return INVALID_SOCKET;
        }
        return s;
    }

    // 서버가 끊었는지 — 시한 안에 읽기 쪽이 닫히면 true
    bool ClosedByPeer(SOCKET s, int timeoutMs)
    {
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(s, &reads);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        const int ready = ::select(static_cast<int>(s) + 1, &reads, nullptr, nullptr, &tv);
        if (ready <= 0)
            return false;
        char buf[16];
        return ::recv(s, buf, sizeof(buf), 0) <= 0;
    }

    // 이 PC의 루프백이 아닌 주소 — UDP connect는 패킷을 안 보내고 나갈 주소만 정한다
    std::string FindOtherAddress()
    {
        SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
            return {};
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
        std::string result;
        if (::connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0)
        {
            sockaddr_in local{};
#ifdef _WIN32
            int len = sizeof(local);
#else
            socklen_t len = sizeof(local);
#endif
            if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0)
            {
                char text[64] = {};
                inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
                result = text;
            }
        }
        Platform::CloseSocket(s);
        return (result == "127.0.0.1" || result == "0.0.0.0") ? std::string{} : result;
    }

    void WaitCounter(const Counter& counter, int64_t expected, int timeoutMs = 3000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (counter.Load() != expected && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int main()
{
    std::printf("transport = %s\n", kNetIoModelName);

    CMonitorManager monitor;
    CIOCPServer server(PORT_WEB, WEB_LIMIT + RESERVE, "CoreLinkTest", monitor, 2, 2, 2, 0, 1);

    // 시작 전에는 틀린 주소를 거절하고 맞는 주소를 받는다
    Check(!server.AddListener("not-an-ip", PORT_LINK, TAG_DOWN), "AddListener: 틀린 주소 거절");
    Check(server.AddListener("127.0.0.1", PORT_LINK, TAG_DOWN), "AddListener: 맞는 주소 받음");
    server.SetUntaggedSessionLimit(WEB_LIMIT);
    server.SetEventQueueAcceptLimit(QUEUE_LIMIT);

    if (!server.Start())
    {
        Check(false, "Start");
        return 1;
    }
    Check(!server.AddListener("127.0.0.1", PORT_LINK + 1, TAG_DOWN), "AddListener: Start 뒤에는 거절");

    CEvents events(server, monitor);
    using Type = NetworkEvent::Type;

    // ── 1. 지정 주소 밖 접속 거절 ─────────────────────────────────────
    {
        const std::string other = FindOtherAddress();
        std::printf("다른 주소 = %s\n", other.empty() ? "(없음)" : other.c_str());
        Check(!other.empty(), "이 PC의 다른 주소를 찾음 (없으면 거절 시험을 못 본다)");
        if (!other.empty())
        {
            SOCKET denied = ConnectRaw(other.c_str(), PORT_LINK);
            Check(denied == INVALID_SOCKET, "서버 간 포트: 다른 주소로 온 접속 거절");
            if (denied != INVALID_SOCKET)
                Platform::CloseSocket(denied);

            // 반대 방향 — 모든 주소를 듣는 생성자 포트는 같은 주소로 붙는다
            SOCKET allowed = ConnectRaw(other.c_str(), PORT_WEB);
            Check(allowed != INVALID_SOCKET, "웹 포트: 다른 주소로 온 접속 받음(대조군)");
            Seen seen;
            Check(events.TakeOf(Type::CONNECTED, 0, &seen), "웹 포트 접속 이벤트의 구분 값 0");
            if (allowed != INVALID_SOCKET)
                Platform::CloseSocket(allowed);
            Check(events.TakeOf(Type::DISCONNECTED, 0), "웹 포트 끊김 이벤트의 구분 값 0");
        }

        SOCKET loopback = ConnectRaw("127.0.0.1", PORT_LINK);
        Check(loopback != INVALID_SOCKET, "서버 간 포트: 지정 주소로 온 접속 받음(반대 방향)");
        Seen seen;
        Check(events.TakeOf(Type::CONNECTED, TAG_DOWN, &seen), "서버 간 포트 접속 이벤트의 구분 값");
        if (loopback != INVALID_SOCKET)
            Platform::CloseSocket(loopback);
        Check(events.TakeOf(Type::DISCONNECTED, TAG_DOWN), "서버 간 포트 끊김 이벤트의 구분 값");
    }

    // ── 2. 없는 포트로 ConnectTo → 0, 세션 안 샘 ────────────────────
    {
        const int64_t before = monitor._sessionCreated.Load();
        Check(server.ConnectTo("127.0.0.1", PORT_LINK + 7, TAG_UP) == 0, "ConnectTo: 없는 포트면 0");
        Check(server.ConnectTo("not-an-ip", PORT_LINK, TAG_UP) == 0, "ConnectTo: 틀린 주소면 0");
        Check(monitor._sessionCreated.Load() == before, "ConnectTo 실패는 세션을 안 만든다");
    }

    // ── 3. ConnectTo 왕복 · 따로 세기 ───────────────────────────────
    int64_t upId = 0;
    int64_t downId = 0;
    {
        upId = server.ConnectTo("127.0.0.1", PORT_LINK, TAG_UP);
        Check(upId != 0, "ConnectTo: 세션 번호를 돌려줌");
        Seen seenUp, seenDown;
        Check(events.TakeOf(Type::CONNECTED, TAG_UP, &seenUp), "건 쪽 접속 이벤트의 구분 값");
        Check(seenUp.sessionId == upId, "건 쪽 접속 이벤트의 세션 번호가 돌려준 번호와 같음");
        Check(events.TakeOf(Type::CONNECTED, TAG_DOWN, &seenDown), "받은 쪽 접속 이벤트의 구분 값");
        downId = seenDown.sessionId;

        WaitCounter(monitor._taggedSessionCount, 2);
        Check(monitor._taggedSessionCount.Load() == 2, "구분 값 세션 수 2");
        Check(monitor._sessionCount.Load() == 0, "서버 간 연결이 동접 수에 안 잡힘");

        // 크기를 바꿔 가며 양방향 왕복 — 상한 1458B까지. RIO는 링 경계를 넘기는 데까지 간다
        const int sizes[] = { 0, 1, 16, 100, 700, 1454 };   // 1454 + 머리 4 = 패킷 상한 1458
        int rounds = 0;
        bool sameBytes = true;
        for (int repeat = 0; repeat < 40 && sameBytes; ++repeat)
        {
            for (int size : sizes)
            {
                const std::string payload = MakePayload(size, repeat + size);
                if (!Send(server, upId, payload))
                {
                    sameBytes = false;
                    break;
                }
                Seen got;
                if (!events.TakeSession(Type::RECEIVED, downId, &got) || got.tag != TAG_DOWN || got.payload != payload)
                {
                    sameBytes = false;
                    break;
                }
                if (!Send(server, downId, payload))
                {
                    sameBytes = false;
                    break;
                }
                if (!events.TakeSession(Type::RECEIVED, upId, &got) || got.tag != TAG_UP || got.payload != payload)
                {
                    sameBytes = false;
                    break;
                }
                rounds += 2;
            }
        }
        Check(sameBytes, "양방향 왕복 " + std::to_string(rounds) + "회 — 내용·구분 값 일치");

        // 틱 끝에 묶어 보내는 길도 같은 세션에서 돈다
        const std::string deferred = MakePayload(64, 77);
        Check(Send(server, upId, deferred, SendFlush::Deferred), "묶어 보내기 적재");
        server.FlushPendingSends();
        Seen got;
        Check(events.TakeSession(Type::RECEIVED, downId, &got) && got.payload == deferred, "묶어 보내기 왕복");
    }

    // ── 4. 웹 정원이 차도 서버 간 연결이 붙음 ──────────────────────
    std::vector<SOCKET> webSockets;
    {
        for (int i = 0; i < WEB_LIMIT; ++i)
        {
            SOCKET s = ConnectRaw("127.0.0.1", PORT_WEB);
            Check(s != INVALID_SOCKET, "웹 접속 " + std::to_string(i + 1));
            webSockets.push_back(s);
            Check(events.TakeOf(Type::CONNECTED, 0), "웹 접속 이벤트 구분 값 0");
        }
        WaitCounter(monitor._sessionCount, WEB_LIMIT);
        Check(monitor._sessionCount.Load() == WEB_LIMIT, "동접 수 = 웹 정원");

        const int64_t failedBefore = monitor._acceptFailed.Load();
        SOCKET over = ConnectRaw("127.0.0.1", PORT_WEB);
        Check(over != INVALID_SOCKET && ClosedByPeer(over, 2000), "웹 정원 초과 접속은 서버가 끊음");
        if (over != INVALID_SOCKET)
            Platform::CloseSocket(over);
        WaitCounter(monitor._acceptFailed, failedBefore + 1);
        Check(monitor._acceptFailed.Load() == failedBefore + 1, "거절 지표 +1");
        Check(monitor._sessionCount.Load() == WEB_LIMIT, "거절 뒤 동접 수 그대로");

        const int64_t second = server.ConnectTo("127.0.0.1", PORT_LINK, TAG_UP);
        Check(second != 0, "웹 정원이 차 있어도 서버 간 연결이 붙음");
        Seen a, b;
        Check(events.TakeOf(Type::CONNECTED, TAG_UP, &a), "둘째 연결 건 쪽 이벤트");
        Check(events.TakeOf(Type::CONNECTED, TAG_DOWN, &b), "둘째 연결 받은 쪽 이벤트");
        WaitCounter(monitor._taggedSessionCount, 4);
        Check(monitor._taggedSessionCount.Load() == 4, "구분 값 세션 수 4");
        Check(monitor._sessionCount.Load() == WEB_LIMIT, "서버 간 연결을 더 맺어도 동접 수 그대로");

        // 끊으면 둘 다 원래대로
        Check(server.RequestDisconnectSession(second), "둘째 연결 끊기 요청");
        Check(events.TakeSession(Type::DISCONNECTED, second), "둘째 연결 건 쪽 끊김 이벤트");
        Check(events.TakeSession(Type::DISCONNECTED, b.sessionId), "둘째 연결 받은 쪽 끊김 이벤트");
        WaitCounter(monitor._taggedSessionCount, 2);
        Check(monitor._taggedSessionCount.Load() == 2, "끊은 뒤 구분 값 세션 수 2");
        Check(monitor._sessionCount.Load() == WEB_LIMIT, "끊은 뒤 동접 수 그대로");
    }

    // ── 5. 과부하 거절은 구분 값 0에만 ─────────────────────────────
    {
        for (SOCKET s : webSockets)
            Platform::CloseSocket(s);
        webSockets.clear();
        for (int i = 0; i < WEB_LIMIT; ++i)
            Check(events.TakeOf(Type::DISCONNECTED, 0), "웹 끊김 이벤트 구분 값 0");
        WaitCounter(monitor._sessionCount, 0);
        Check(monitor._sessionCount.Load() == 0, "웹을 다 끊으면 동접 수 0");

        monitor._gameLoop._eventQueueSize.Store(QUEUE_LIMIT + 1);
        const int64_t rejectedBefore = monitor._acceptRejectedByQueue.Load();
        SOCKET web = ConnectRaw("127.0.0.1", PORT_WEB);
        Check(web != INVALID_SOCKET && ClosedByPeer(web, 2000), "과부하: 웹 접속 끊음");
        if (web != INVALID_SOCKET)
            Platform::CloseSocket(web);
        WaitCounter(monitor._acceptRejectedByQueue, rejectedBefore + 1);
        Check(monitor._acceptRejectedByQueue.Load() == rejectedBefore + 1, "과부하 거절 지표 +1");

        const int64_t third = server.ConnectTo("127.0.0.1", PORT_LINK, TAG_UP);
        Check(third != 0, "과부하: 서버 간 연결은 붙음");
        Seen b;
        Check(events.TakeOf(Type::CONNECTED, TAG_UP), "과부하 중 건 쪽 이벤트");
        Check(events.TakeOf(Type::CONNECTED, TAG_DOWN, &b), "과부하 중 받은 쪽 이벤트");
        monitor._gameLoop._eventQueueSize.Store(0);

        Check(server.RequestDisconnectSession(third), "과부하 중 연결 끊기 요청");
        Check(events.TakeSession(Type::DISCONNECTED, third), "과부하 중 연결 끊김(건 쪽)");
        Check(events.TakeSession(Type::DISCONNECTED, b.sessionId), "과부하 중 연결 끊김(받은 쪽)");
    }

    // ── 6. 자리 재사용 — 구분 값이 있던 자리에 웹이 와도 0으로 나간다 ──
    {
        Check(server.RequestDisconnectSession(upId), "첫 연결 끊기 요청");
        Check(events.TakeSession(Type::DISCONNECTED, upId), "첫 연결 끊김(건 쪽)");
        Check(events.TakeSession(Type::DISCONNECTED, downId), "첫 연결 끊김(받은 쪽)");
        WaitCounter(monitor._taggedSessionCount, 0);
        Check(monitor._taggedSessionCount.Load() == 0, "구분 값 세션 수 0");

        bool allZero = true;
        for (int i = 0; i < WEB_LIMIT + RESERVE; ++i)
        {
            SOCKET s = ConnectRaw("127.0.0.1", PORT_WEB);
            Seen seen;
            if (s == INVALID_SOCKET || !events.TakeOf(Type::CONNECTED, 0, &seen))
                allZero = false;
            if (s != INVALID_SOCKET)
                Platform::CloseSocket(s);
            if (!events.TakeOf(Type::DISCONNECTED, 0))
                allZero = false;
        }
        Check(allZero, "자리를 한 바퀴 돌려도 웹 세션의 구분 값은 0");
        WaitCounter(monitor._sessionCount, 0);
        Check(monitor._sessionCount.Load() == 0, "동접 수 0");
        Check(monitor._taggedSessionCount.Load() == 0, "구분 값 세션 수 0");
    }

    Check(!events.SawNegative(), "세는 값이 음수로 간 적 없음");
    Check(events.PendingCount() == 0, "남은 이벤트 없음 " + events.Dump());

    server.ShutdownServer();

    std::printf("\n%s — 확인 %d건 · 실패 %d건\n", g_failed == 0 ? "통과" : "실패", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
