// UDP 부품(CUdpEndpoint) 시험 — 루프백 끝점 둘로 받기 · 보내기 · 깨움 · 큰 것 버림 · 주소 되돌림을 팔마다 같은 대본으로 돌린다
//   Windows 는 보통 · RIO 두 팔을 돌려 결과(받은 바이트 · 차례 · 셈)가 같은지 대조한다. 리눅스는 epoll 팔 하나 + rio 거절.
//   외부 라이브러리 없음, 실패하면 종료 값 1.
#include "TestHelp.h"   // WinSock2 — Windows.h 보다 먼저 와야 해서 맨 앞

#include "UdpEndpoint.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <poll.h>
#endif

void SignalProcessShutdown() {}   // 코어가 부르는 종료 알림 — 시험에는 기다리는 쪽이 없다

using TestHelp::Check;

namespace
{
    using Clock = std::chrono::steady_clock;
    using WaitResult = CUdpEndpoint::WaitResult;

    constexpr uint32_t LOOPBACK = 0x7F000001;   // 127.0.0.1 — 호스트 바이트 차례
    constexpr uint32_t MAX_DG   = 2048;         // 받기 한 칸 — 부품 기본값 그대로
    constexpr uint32_t SLOTS    = 8;            // 보내기 칸 · 한 Receive 상한 — 작게 둬 「칸이 차면 먼저 내보냄」 · 「한 Receive 상한」 길을 여러 번 지나가게
    constexpr int      BATCH    = 50;           // 묶음 건수 — 보내기 칸(8)의 여러 배
    // 받기 칸 — 묶음(50)이 Receive 사이에 한꺼번에 몰려도 다 걸려 있게.
    //   RIO 팔은 걸어 둔 칸보다 많이 몰리면 나머지를 커널이 버린다(⑩ 이 못박음 · 11-17 첫 판: 칸 8 에 50건 → 8건)
    constexpr uint32_t RECV_SLOTS = 64;

    CUdpEndpoint::Config MakeConfig(bool rio)
    {
        CUdpEndpoint::Config c;
        c.port = 0;   // OS 가 고름 — 시험끼리 포트가 안 부딪힌다
        c.bindIp = LOOPBACK;
        c.maxDatagram = MAX_DG;
        c.recvSlots = RECV_SLOTS;
        c.sendSlots = SLOTS;
        c.rio = rio;
        return c;
    }

    const char* NameOf(WaitResult r)
    {
        switch (r)
        {
        case WaitResult::Timeout: return "Timeout";
        case WaitResult::Ready:   return "Ready";
        case WaitResult::Woken:   return "Woken";
        }
        return "?";
    }

    long long MsSince(Clock::time_point t)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
    }

    // 첫 4바이트 = 순번(작은 끝) · 나머지 = 순번과 자리로 정한 바이트
    std::string Payload(int seq, size_t size)
    {
        std::string s(size, '\0');
        for (size_t j = 0; j < size; ++j)
            s[j] = static_cast<char>((seq * 31 + static_cast<int>(j) * 7) & 0xFF);
        for (size_t j = 0; j < 4 && j < size; ++j)
            s[j] = static_cast<char>((seq >> (8 * j)) & 0xFF);
        return s;
    }

    int SeqOf(const std::string& s)
    {
        if (s.size() < 4)
            return -1;
        return static_cast<int>(static_cast<uint8_t>(s[0]) | (static_cast<uint8_t>(s[1]) << 8)
                                | (static_cast<uint8_t>(s[2]) << 16) | (static_cast<uint8_t>(s[3]) << 24));
    }

    uint64_t Fnv1a(const std::string& s)
    {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : s)
        {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }

    bool SendStr(CUdpEndpoint& ep, const UdpAddr& to, const std::string& s)
    {
        return ep.Send(to, reinterpret_cast<const uint8_t*>(s.data()), static_cast<uint32_t>(s.size()));
    }

    using Got = std::vector<std::pair<UdpAddr, std::string>>;

    // count 건이 모일 때까지 Wait · Receive — 시한을 넘으면 모인 만큼. 끝엔 늘 Receive 가 0 을 돌려준 뒤다(다 비움)
    Got Collect(CUdpEndpoint& ep, size_t count, int timeoutMs = 2000)
    {
        Got got;
        UdpDatagram buf[SLOTS];
        const auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true)
        {
            size_t n;
            while ((n = ep.Receive(buf, SLOTS)) > 0)
            {
                for (size_t i = 0; i < n; ++i)
                    got.emplace_back(buf[i].from, std::string(reinterpret_cast<const char*>(buf[i].data), buf[i].size));
            }
            if (got.size() >= count)
                break;
            const auto now = Clock::now();
            if (now >= until)
                break;
            ep.Wait(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count()) + 1);
        }
        return got;
    }

    bool WaitOutside(CUdpEndpoint& ep, int ms)
    {
#ifdef _WIN32
        return WaitForSingleObject(static_cast<HANDLE>(ep.WaitHandle()), static_cast<DWORD>(ms)) == WAIT_OBJECT_0;
#else
        pollfd p{ ep.WaitHandle(), POLLIN, 0 };
        return ::poll(&p, 1, ms) == 1;
#endif
    }

    // 팔 하나에 대본 전부. 돌려주는 글 = 두 팔 대조용(받은 차례 · 크기 · 바이트 해시 · 보낸 쪽 · 셈 — 팔마다 다른 Flush 수는 뺌)
    std::string RunArm(const std::string& name, bool rio)
    {
        std::string transcript;
        auto note = [&](const std::string& s) { transcript += s; transcript += '\n'; };
        auto label = [&](const UdpAddr& from, uint16_t aPort, uint16_t bPort) {
            if (from.ip != LOOPBACK)
                return std::string("?ip");
            return from.port == aPort ? std::string("A") : from.port == bPort ? std::string("B") : std::string("?port");
        };

        CUdpEndpoint a, b;
        const bool openA = a.Open(MakeConfig(rio));
        const bool openB = b.Open(MakeConfig(rio));
        Check(openA && openB, name + " — 끝점 둘 열림");
        if (!openA || !openB)
            return {};
        const uint16_t aPort = a.LocalPort(), bPort = b.LocalPort();
        Check(aPort != 0 && bPort != 0 && aPort != bPort, name + " — 포트 0 이면 OS 가 고름(" + std::to_string(aPort) + " · " + std::to_string(bPort) + ")");
        const UdpAddr toA{ LOOPBACK, aPort }, toB{ LOOPBACK, bPort };
#ifdef _WIN32
        Check(b.WaitHandle() != nullptr, name + " — 기다림 손잡이 있음");
#else
        Check(b.WaitHandle() >= 0, name + " — 기다림 손잡이 있음");
#endif

        // ⓪ 아무것도 없을 때
        UdpDatagram d[SLOTS];
        Check(b.Receive(d, SLOTS) == 0, name + " — 빈 받기 0");
        auto t0 = Clock::now();
        const WaitResult idle = b.Wait(50);
        Check(idle == WaitResult::Timeout && MsSince(t0) >= 40, name + " — 받을 것 · 깨움이 없으면 시간 끝(" + std::to_string(MsSince(t0)) + "ms)");

        // ① 한 건
        SendStr(a, toB, "hello");
        a.Flush();
        const WaitResult first = b.Wait(2000);
        Check(first == WaitResult::Ready, name + " — 한 건: Wait 가 받을 것으로 깸");
        const size_t n1 = b.Receive(d, SLOTS);
        const std::string s1 = n1 == 1 ? std::string(reinterpret_cast<const char*>(d[0].data), d[0].size) : std::string();
        Check(n1 == 1 && s1 == "hello", name + " — 한 건: 바이트 같음");
        Check(n1 == 1 && d[0].from.ip == LOOPBACK && d[0].from.port == aPort, name + " — 한 건: 보낸 쪽 주소(127.0.0.1 · A 포트)");
        note("one " + s1 + " " + (n1 == 1 ? label(d[0].from, aPort, bPort) : "-"));
        Check(b.Receive(d, SLOTS) == 0, name + " — 한 건: 다 받은 뒤 0");

        // ② 묶음 — 크기 1 ~ 1,000 여러 가지 · 칸 8 이라 보내는 쪽은 여러 번 먼저 내보내고 받는 쪽은 Receive 를 여러 번
        const uint64_t flushBefore = a.GetStats().flushes.load();
        for (int i = 0; i < BATCH; ++i)
            SendStr(a, toB, Payload(i, 4 + static_cast<size_t>(i * 37) % 997));
        a.Flush();
        const Got batch = Collect(b, BATCH);
        bool order = batch.size() == BATCH, bytes = batch.size() == BATCH, from = batch.size() == BATCH;
        for (size_t i = 0; i < batch.size(); ++i)
        {
            if (SeqOf(batch[i].second) != static_cast<int>(i))
                order = false;
            if (batch[i].second != Payload(static_cast<int>(i), 4 + (i * 37) % 997))
                bytes = false;
            if (label(batch[i].first, aPort, bPort) != "A")
                from = false;
            note("batch " + std::to_string(batch[i].second.size()) + " " + std::to_string(Fnv1a(batch[i].second)) + " " + label(batch[i].first, aPort, bPort));
        }
        Check(batch.size() == BATCH, name + " — 묶음: " + std::to_string(BATCH) + "건 다 옴(" + std::to_string(batch.size()) + ")");
        Check(order, name + " — 묶음: 차례 그대로");
        Check(bytes, name + " — 묶음: 바이트 그대로");
        Check(from, name + " — 묶음: 보낸 쪽 주소 그대로");
        const uint64_t flushes = a.GetStats().flushes.load() - flushBefore;
#ifdef _WIN32
        if (!rio)
            Check(flushes == 0, name + " — 보통 팔은 Flush 로 낼 것이 없음(" + std::to_string(flushes) + ")");
        else
#endif
            Check(flushes >= BATCH / SLOTS, name + " — 칸이 차면 먼저 내보냄(Flush " + std::to_string(flushes) + "번)");

        // ③ 받기 칸 딱 · 하나 넘침 · 그 뒤 것
        SendStr(a, toB, Payload(100, MAX_DG));
        SendStr(a, toB, Payload(101, MAX_DG + 1));   // 보내는 쪽도 칸(2,048)보다 큰 길 — RIO 큰 칸 · 리눅스 바로 보내기
        SendStr(a, toB, "after");
        a.Flush();
        const Got edge = Collect(b, 2);
        Check(edge.size() == 2 && edge[0].second == Payload(100, MAX_DG), name + " — 딱 2,048 은 받음");
        Check(edge.size() == 2 && edge[1].second == "after", name + " — 2,049 는 버리고 다음 것을 받음");
        Check(b.GetStats().oversizeDropped.load() == 1, name + " — 큰 것 버림 셈 1(" + std::to_string(b.GetStats().oversizeDropped.load()) + ")");
        for (const auto& g : edge)
            note("edge " + std::to_string(g.second.size()) + " " + std::to_string(Fnv1a(g.second)));

        // ④ 보내기 상한(maxSendDatagram 기본 65,507) 넘음 — 거절하고 셈
        const std::string huge(CUdpEndpoint::MAX_UDP_PAYLOAD + 1, 'x');
        Check(!SendStr(a, toB, huge), name + " — 상한 넘는 보내기 거절");
        Check(a.GetStats().sendFail.load() == 1, name + " — 보내기 실패 셈 1");

        // ⑤ 다른 스레드의 Wake — 앞에서 받을 것을 비우는 사이에 선 헛 신호를 먼저 거둔다(헤더 「깸은 헛것일 수 있다」)
        for (int i = 0; i < 10 && b.Wait(0) != WaitResult::Timeout; ++i)
        {
        }
        std::thread waker([&b] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            b.Wake();
        });
        t0 = Clock::now();
        const WaitResult woken = b.Wait(3000);
        const long long wokenMs = MsSince(t0);
        waker.join();
        Check(woken == WaitResult::Woken && wokenMs >= 30 && wokenMs < 2000,
              name + " — 다른 스레드의 Wake 로 깸(" + NameOf(woken) + " · " + std::to_string(wokenMs) + "ms)");
        Check(b.Receive(d, SLOTS) == 0, name + " — 깸 뒤 받을 것 없음");
        b.Wake();
        const WaitResult pre = b.Wait(0);
        Check(pre == WaitResult::Woken, name + " — 먼저 깨워 두면 Wait(0) 이 깨움(" + NameOf(pre) + ")");
        const WaitResult after = b.Wait(30);
        Check(after == WaitResult::Timeout, name + " — 깨움을 거둔 뒤엔 시간 끝(" + NameOf(after) + ")");
        Check(b.GetStats().wakes.load() == 2, name + " — 깨움 셈 2");

        // ⑥ 주소 되돌림 — 받은 from 으로 보내면 처음 쪽이 받는다
        SendStr(a, toB, "ping");
        a.Flush();
        const Got ping = Collect(b, 1);
        bool back = false;
        if (ping.size() == 1)
        {
            SendStr(b, ping[0].first, "pong");
            b.Flush();
            const Got pong = Collect(a, 1);
            back = pong.size() == 1 && pong[0].second == "pong" && label(pong[0].first, aPort, bPort) == "B";
            note("pong " + (pong.empty() ? std::string("-") : pong[0].second + " " + label(pong[0].first, aPort, bPort)));
        }
        Check(back, name + " — 받은 주소로 되돌려 보내면 처음 쪽이 받음");

        // ⑦ 바깥에서 WaitHandle 로 기다리기 — 깨면 Wait(0) 으로 거두고 Receive 를 비운다
        SendStr(a, toB, "outside");
        a.Flush();
        const bool outside = WaitOutside(b, 2000);
        b.Wait(0);
        const Got ext = Collect(b, 1);
        Check(outside && ext.size() == 1 && ext[0].second == "outside", name + " — 바깥에서 기다려도 받을 것에 깸");

        // ⑧ 셈 — 위에서 한 일과 같은지
        const auto& sa = a.GetStats();
        const auto& sb = b.GetStats();
        const std::string counts = "A sent " + std::to_string(sa.sent.load()) + " recv " + std::to_string(sa.received.load())
            + " sendFail " + std::to_string(sa.sendFail.load()) + " recvFail " + std::to_string(sa.recvFail.load())
            + " | B sent " + std::to_string(sb.sent.load()) + " recv " + std::to_string(sb.received.load())
            + " oversize " + std::to_string(sb.oversizeDropped.load()) + " wakes " + std::to_string(sb.wakes.load())
            + " recvFail " + std::to_string(sb.recvFail.load());
        note(counts);
        // A 보냄 = 한 건 1 + 묶음 50 + 가장자리 3 + ping 1 + outside 1 = 56 (상한 넘은 1 은 실패로) · B 받음 = 1 + 50 + 2 + 1 + 1 = 55
        Check(sa.sent.load() == 56 && sa.received.load() == 1 && sb.sent.load() == 1 && sb.received.load() == 55
                  && sa.recvFail.load() == 0 && sb.recvFail.load() == 0,
              name + " — 셈이 한 일과 같음: " + counts);

        // ⑨ 거절 — 두 번 열기 · 같은 포트 · 틀린 설정
        Check(!a.Open(MakeConfig(rio)), name + " — 열린 끝점에 Open 거절");
        {
            CUdpEndpoint c;
            CUdpEndpoint::Config cfg = MakeConfig(rio);
            cfg.port = aPort;
            Check(!c.Open(cfg), name + " — 같은 포트 둘째 끝점 Open 실패");
            cfg = MakeConfig(rio);
            cfg.recvSlots = 0;
            Check(!c.Open(cfg), name + " — 받기 칸 0 거절");
            cfg = MakeConfig(rio);
            cfg.maxDatagram = 0;
            Check(!c.Open(cfg), name + " — 받기 칸 크기 0 거절");
            cfg = MakeConfig(rio);
            cfg.maxSendDatagram = MAX_DG - 1;
            Check(!c.Open(cfg), name + " — 보내기 상한 < 받기 칸 거절");
            Check(!c.IsOpen(), name + " — 거절된 끝점은 닫힌 채");
        }

        // ⑩ 받기 칸보다 많이 몰릴 때 — 팔마다 다르다(대조 글에 안 넣음). 받는 쪽이 Receive 를 안 부르는 동안 20건을 몰아 보낸다
        {
            CUdpEndpoint c;
            CUdpEndpoint::Config cfg = MakeConfig(rio);
            cfg.recvSlots = SLOTS;
            const bool openC = c.Open(cfg);
            const UdpAddr toC{ LOOPBACK, c.LocalPort() };
            constexpr int BURST = 20;
            for (int i = 0; openC && i < BURST; ++i)
                SendStr(a, toC, Payload(200 + i, 100));
            a.Flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));   // 다 도착하거나 버려질 때까지
            const Got burst = Collect(c, BURST, 300);
            bool head = true;
            for (size_t i = 0; i < burst.size(); ++i)
                if (SeqOf(burst[i].second) != 200 + static_cast<int>(i))
                    head = false;
            if (rio)
                Check(openC && burst.size() == SLOTS && head,
                      name + " — 걸어 둔 칸(8)보다 몰리면 나머지는 커널이 버림: 20건 중 앞 " + std::to_string(burst.size()) + "건");
            else
                Check(openC && burst.size() == BURST && head,
                      name + " — 칸(8)보다 몰려도 커널 버퍼에 쌓여 다 옴: 20건 중 " + std::to_string(burst.size()) + "건");

            // 칸 수를 넘겨 계속 받기 — 8건씩 세 번(합 24 > 칸 8). RIO 팔은 거둔 칸을 다음 Receive 가 다시 걸어야 이어 받는다
            bool rounds = openC;
            for (int round = 0; openC && round < 3; ++round)
            {
                for (int i = 0; i < static_cast<int>(SLOTS); ++i)
                    SendStr(a, toC, Payload(300 + round * 10 + i, 64));
                a.Flush();
                const Got part = Collect(c, SLOTS);
                if (part.size() != SLOTS)
                    rounds = false;
                for (size_t i = 0; i < part.size(); ++i)
                    if (SeqOf(part[i].second) != 300 + round * 10 + static_cast<int>(i))
                        rounds = false;
            }
            Check(rounds, name + " — 칸 수(8)를 넘겨 8건씩 세 번 이어 받음");
        }

        // ⑪ 닫은 뒤 다시 열기 — 셈은 0 부터
        b.Close();
        Check(!b.IsOpen() && b.Receive(d, SLOTS) == 0 && !SendStr(b, toA, "x"), name + " — 닫은 끝점은 받기 0 · 보내기 거절");
        Check(b.Open(MakeConfig(rio)) && b.GetStats().received.load() == 0 && b.GetStats().wakes.load() == 0, name + " — 닫은 뒤 다시 열면 셈 0");
        return transcript;
    }
}

int main()
{
#ifdef _WIN32
    const std::string plain = RunArm("보통", false);
    const std::string rio = RunArm("RIO", true);
    Check(!plain.empty() && plain == rio, "두 팔이 같은 결과(받은 차례 · 크기 · 바이트 · 보낸 쪽 · 셈)");
    if (plain != rio)
        std::printf("--- 보통\n%s--- RIO\n%s", plain.c_str(), rio.c_str());
#else
    const std::string epoll = RunArm("epoll", false);
    Check(!epoll.empty(), "epoll — 대본 끝까지");
    CUdpEndpoint r;
    CUdpEndpoint::Config cfg = MakeConfig(true);
    Check(!r.Open(cfg), "리눅스에서 rio = true 는 Open 실패(말없이 바꾸지 않음)");
#endif
    return TestHelp::Finish("udp_endpoint_test");
}
