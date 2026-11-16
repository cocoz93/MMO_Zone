// 자르기 부품 시험 — 전체 길이 머리 · 몸 길이 머리 · 줄 단위 · 큰 버퍼 풀
//   ①~③ 은 서버 없이 부품만 본다. 링은 진짜 CRingBufferST 를 써서 ParsePackets 와 같은 차례(두 구간 보이기 →
//   자르기 → 앞부분 복사 → 링에서 지우기)로 돌린다.
//   ④ 는 서버를 띄워 줄 단위로 받고, 기본 버퍼를 넘는 줄이 큰 풀에서 나와 큰 풀로 돌아가는지 본다.
//   CIOCPServer 는 프로세스에 하나라서(Start 가 공용 메시지 풀을 덮어쓴다) 서버는 ④ 에서 하나만 띄운다.
//   외부 라이브러리 없음, 실패하면 종료 값 1.
#include "TestHelp.h"   // WinSock2 — Windows.h 보다 먼저 와야 해서 맨 앞

#include "IOCPServer.h"
#include "MessageFramer.h"
#include "RingBuffer.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/select.h>
#endif

void SignalProcessShutdown() {}   // 코어가 부르는 종료 알림 — 시험에는 기다리는 쪽이 없다

using TestHelp::Check;

namespace
{
    struct Outcome
    {
        std::vector<std::string> messages;
        bool   invalid  = false;
        size_t badSize  = 0;
        size_t left     = 0;     // 끝나고 링에 남은 바이트
        bool   ringFull = false; // 시험 입력이 링에 다 안 들어감(시험 짜임 잘못)
    };

    // ParsePackets 와 같은 차례로 조각들을 차례로 넣으며 자른다.
    //   startOffset 만큼 링을 미리 돌려 두면 읽기 자리가 링 끝 가까이에서 시작한다(감김 시험)
    Outcome Run(const CMessageFramer& framer, const std::vector<std::string>& chunks,
                size_t ringCapacity = 65535, size_t startOffset = 0)
    {
        CRingBufferST ring(ringCapacity);
        if (startOffset != 0)
        {
            const std::string pad(startOffset, 'x');
            ring.Enqueue(pad.data(), pad.size());
            ring.Consume(pad.size());
        }

        Outcome out;
        for (const std::string& chunk : chunks)
        {
            if (!chunk.empty() && ring.Enqueue(chunk.data(), chunk.size()) != chunk.size())
            {
                out.ringFull = true;
                return out;
            }
            while (true)
            {
                const size_t dataSize = ring.GetDataSize();
                if (dataSize == 0)
                    break;
                const size_t directSize = ring.GetDirectReadSize();
                const RecvSpan data{ ring.GetReadPtr(), directSize, ring.GetBufferStartPtr(), dataSize - directSize };
                const FrameResult frame = framer.Next(data);
                if (frame.status == FrameStatus::NeedMore)
                    break;
                if (frame.status == FrameStatus::Invalid)
                {
                    out.invalid = true;
                    out.badSize = frame.badSize;
                    out.left = ring.GetDataSize();
                    return out;
                }
                std::string message(frame.deliver, '\0');
                data.CopyFront(message.data(), frame.deliver);
                ring.Consume(frame.consume);
                out.messages.push_back(message);
            }
        }
        out.left = ring.GetDataSize();
        return out;
    }

    // 한 바이트씩 쪼갠다
    std::vector<std::string> Bytes(const std::string& all)
    {
        std::vector<std::string> chunks;
        for (char c : all)
            chunks.push_back(std::string(1, c));
        return chunks;
    }

    std::string U16(size_t v)
    {
        std::string s(2, '\0');
        s[0] = static_cast<char>(v & 0xFF);
        s[1] = static_cast<char>((v >> 8) & 0xFF);
        return s;
    }

    // 전체 길이 머리 메시지 — 길이 = 머리 4 + 몸
    std::string Total(uint16_t type, const std::string& body) { return U16(4 + body.size()) + U16(type) + body; }

    // 몸 길이 머리 메시지 — 길이 = 몸만
    std::string Body(const std::string& body) { return U16(body.size()) + body; }

    std::string Fill(size_t n, char base)
    {
        std::string s(n, '\0');
        for (size_t i = 0; i < n; ++i)
            s[i] = static_cast<char>(base + (i % 23));
        return s;
    }

    bool Same(const std::vector<std::string>& a, const std::vector<std::string>& b) { return a == b; }

    // 줄 단위 기준 — 줄 규약의 기존 구현과 같은 규칙을 문자열로 그대로 쓴다.
    //   LF 를 찾아 그 앞을 한 줄로 · 끝의 CR 하나만 뗀다 · 빈 줄도 낸다 ·
    //   줄을 다 뽑은 뒤 남은 것이 4,096 을 넘으면 끊는다
    struct RefOutcome
    {
        std::vector<std::string> lines;
        bool dropped = false;
    };
    RefOutcome RefSplit(const std::vector<std::string>& chunks)
    {
        RefOutcome out;
        std::string in;
        for (const std::string& chunk : chunks)
        {
            in += chunk;
            size_t nl;
            while ((nl = in.find('\n')) != std::string::npos)
            {
                std::string line = in.substr(0, nl);
                in.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                out.lines.push_back(line);
            }
            if (in.size() > 4096)
            {
                out.dropped = true;
                return out;
            }
        }
        return out;
    }

    // ── ① 전체 길이 머리 ──
    void TotalLengthTests()
    {
        const CTotalLengthFramer f;
        const std::string a = Total(1, "abc"), b = Total(2, ""), c = Total(3, Fill(100, 'A'));
        const std::vector<std::string> want{ a, b, c };

        Outcome o = Run(f, { a + b + c });
        Check(Same(o.messages, want) && !o.invalid && o.left == 0, "① 한 번에 셋 → 셋 · 머리째 · 남은 것 0");

        o = Run(f, Bytes(a + b + c));
        Check(Same(o.messages, want) && !o.invalid && o.left == 0, "① 한 바이트씩 쪼개 와도 같은 셋");

        // 링 64 · 읽기 자리 63 → 첫 구간 1바이트(길이 머리가 둘로 갈린다)
        o = Run(f, { c.substr(0, 40) }, 64, 63);
        Check(o.messages.empty() && o.left == 40, "① 감김 — 머리만 오고 몸이 덜 오면 기다린다");
        const std::string d = Total(4, Fill(30, 'k'));   // 34바이트 — 링 64 안에서 감긴다
        o = Run(f, { d }, 64, 63);
        Check(o.messages.size() == 1 && o.messages[0] == d, "① 감김 — 길이 머리가 링 끝에서 갈려도 그대로");

        const std::string full = Total(5, Fill(LENGTH_FRAME_MAX_SIZE - 4, 'm'));
        o = Run(f, { full });
        Check(full.size() == 1458 && o.messages.size() == 1 && o.messages[0].size() == 1458, "① 최대 딱(1,458) 받음");

        o = Run(f, { Total(6, Fill(LENGTH_FRAME_MAX_SIZE - 3, 'm')) });
        Check(o.invalid && o.badSize == 1459 && o.messages.empty(), "① 최대 + 1(1,459) 틀림 · 크기 1,459 보고");

        o = Run(f, { U16(3) + U16(7) });
        Check(o.invalid && o.badSize == 3, "① 머리(4)보다 작은 길이 3 틀림");
        o = Run(f, { U16(0) + U16(7) });
        Check(o.invalid && o.badSize == 0, "① 길이 0 틀림");

        o = Run(f, { a.substr(0, 3) });
        Check(o.messages.empty() && !o.invalid && o.left == 3, "① 머리 덜 옴(3바이트) → 기다린다");
    }

    // ── ② 몸 길이 머리 ──
    void BodyLengthTests()
    {
        const CBodyLengthFramer f;
        const std::string a = Body("abcd"), b = Body(""), c = Body(Fill(200, 'B'));
        const std::vector<std::string> want{ a, b, c };

        Outcome o = Run(f, { a + b + c });
        Check(Same(o.messages, want) && !o.invalid && o.left == 0, "② 한 번에 셋(빈 몸 포함) → 셋 · 머리째");

        o = Run(f, Bytes(a + b + c));
        Check(Same(o.messages, want) && !o.invalid && o.left == 0, "② 한 바이트씩 쪼개 와도 같은 셋");

        const std::string d = Body(Fill(40, 'k'));   // 42바이트
        o = Run(f, { d }, 64, 63);
        Check(o.messages.size() == 1 && o.messages[0] == d, "② 감김 — 길이 머리가 링 끝에서 갈려도 그대로");

        const std::string full = Body(Fill(LENGTH_FRAME_MAX_SIZE - 2, 'm'));
        o = Run(f, { full });
        Check(full.size() == 1458 && o.messages.size() == 1 && o.messages[0].size() == 1458, "② 최대 딱(몸 1,456 · 전체 1,458) 받음");

        o = Run(f, { Body(Fill(LENGTH_FRAME_MAX_SIZE - 1, 'm')) });
        Check(o.invalid && o.badSize == 1459, "② 최대 + 1(몸 1,457 · 전체 1,459) 틀림");

        o = Run(f, { std::string(1, '\x05') });
        Check(o.messages.empty() && !o.invalid && o.left == 1, "② 머리(2)보다 작게 옴(1바이트) → 기다린다");

        // 몸 길이에 머리를 더하는지 — 몸 3 이면 전체 5. 머리를 안 더하면 3바이트에서 끊어 다음 메시지가 어긋난다
        o = Run(f, { Body("xyz") + Body("q") });
        Check(o.messages.size() == 2 && o.messages[0] == Body("xyz") && o.messages[1] == Body("q"), "② 몸 길이 + 머리 = 전체(다음 메시지가 안 어긋남)");
    }

    // ── ③ 줄 단위 ──
    void LineTests()
    {
        const CLineFramer f;

        Outcome o = Run(f, { "a\nbb\r\nccc\n" });
        Check(Same(o.messages, { "a", "bb", "ccc" }) && o.left == 0, "③ 한 번에 셋 → CR LF 뺀 셋");

        o = Run(f, Bytes("a\nbb\r\nccc\n"));
        Check(Same(o.messages, { "a", "bb", "ccc" }) && o.left == 0, "③ 한 바이트씩 쪼개 와도 같은 셋");

        // 링 64 · 읽기 자리 58 → 첫 구간 6바이트 = "hello\r" · 둘째 구간 = "\n" (CR 과 LF 가 링 끝에서 갈린다)
        o = Run(f, { "hello\r\n" }, 64, 58);
        Check(Same(o.messages, { "hello" }) && o.left == 0, "③ 감김 — CR 이 링 끝 · LF 가 링 처음이어도 CR 뗌");
        o = Run(f, { "abc", "defgh\n" }, 64, 60);
        Check(Same(o.messages, { "abcdefgh" }), "③ 감김 — LF 가 둘째 구간에만 있어도 찾음");

        o = Run(f, { "\n" });
        Check(Same(o.messages, { "" }), "③ LF 만 → 빈 줄 하나");
        o = Run(f, { "\r\n" });
        Check(Same(o.messages, { "" }), "③ CR LF 만 → 빈 줄 하나");
        o = Run(f, { "x\n" });
        Check(Same(o.messages, { "x" }), "③ LF 만으로 끝난 줄도 받음");
        o = Run(f, { "a\rb\n" });
        Check(Same(o.messages, { "a\rb" }), "③ 줄 가운데 CR 은 그대로");
        o = Run(f, { "a\r\r\n" });
        Check(Same(o.messages, { "a\r" }), "③ 끝의 CR 은 하나만 뗀다");

        const std::string full(CLineFramer::MAX_LINE, 'L');
        o = Run(f, { full + "\r\n" });
        Check(o.messages.size() == 1 && o.messages[0].size() == 4096 && !o.invalid, "③ 최대 딱(4,096) + CR LF 받음");
        o = Run(f, { full + "\n" });
        Check(o.messages.size() == 1 && o.messages[0].size() == 4096 && !o.invalid, "③ 최대 딱(4,096) + LF 받음");
        o = Run(f, { full });
        Check(o.messages.empty() && !o.invalid && o.left == 4096, "③ LF 없이 4,096 쌓임 → 기다린다");

        o = Run(f, { full + "L\n" });
        Check(o.invalid && o.badSize == 4097, "③ 최대 + 1(4,097) + LF 틀림");
        o = Run(f, { full + "L" });
        Check(o.invalid && o.badSize == 4097, "③ LF 없이 4,097 쌓임 → 틀림");
        o = Run(f, { full, "L" });
        Check(o.invalid && o.badSize == 4097, "③ 4,096 뒤 한 바이트 더 와서 4,097 → 틀림");

        // 줄 하나를 낸 뒤 남은 것이 넘치면 — 앞 줄은 내고 끊는다
        o = Run(f, { "ok\n" + full + "LL" });
        Check(Same(o.messages, { "ok" }) && o.invalid, "③ 앞 줄은 내고 그 뒤 넘친 나머지에서 틀림");
    }

    // ── ③ 기준(기존 줄 규약 구현의 규칙)과 무작위 대조 ──
    void LineReferenceTests()
    {
        const CLineFramer f;
        std::mt19937 rng(20261117);   // 고정 씨앗 — 다시 돌려도 같은 입력
        const char alphabet[] = { 'a', 'b', '\r', '\n' };

        int same = 0, total = 0;
        for (int round = 0; round < 2000; ++round)
        {
            std::string all;
            const int len = static_cast<int>(rng() % 200);
            for (int i = 0; i < len; ++i)
                all.push_back(alphabet[rng() % 4]);

            std::vector<std::string> chunks;
            size_t pos = 0;
            while (pos < all.size())
            {
                const size_t n = 1 + rng() % 17;
                chunks.push_back(all.substr(pos, n));
                pos += n;
            }

            const size_t offset = rng() % 64;   // 읽기 자리를 링 끝 64바이트 안에 둔다 — 감김이 자주 난다
            const Outcome mine = Run(f, chunks, 4096 * 2 + 64, 4096 * 2 + 64 - 1 - offset);
            const RefOutcome ref = RefSplit(chunks);
            ++total;
            if (!mine.ringFull && mine.messages == ref.lines && mine.invalid == ref.dropped)
                ++same;
        }
        Check(same == total, "③ 무작위 2,000판 — 기준과 같은 줄 · 같은 끊음 (" + std::to_string(same) + "/" + std::to_string(total) + ")");

        // 넘침 쪽도 기준과 같은지 — 4,096 근처 길이로
        int sameBig = 0, totalBig = 0;
        for (size_t n : { size_t(4095), size_t(4096), size_t(4097), size_t(5000) })
        {
            for (const std::string tail : { std::string(""), std::string("\n"), std::string("\r\n") })
            {
                const std::vector<std::string> chunks{ std::string(n, 'z') + tail };
                const Outcome mine = Run(f, chunks);
                const RefOutcome ref = RefSplit(chunks);
                ++totalBig;
                // 기준은 한 조각에 LF 까지 온 긴 줄을 낸다(남은 것만 본다) — 이 부품은 넘길 버퍼 크기가 있어 끊는다.
                //   그래서 4,097 이상 + LF 는 「다름」이 맞다. 같아야 하는 것은 그 밖 전부
                const bool expectDiff = (n > 4096 && !tail.empty());
                const bool equal = (mine.messages == ref.lines && mine.invalid == ref.dropped);
                if (equal != expectDiff)
                    ++sameBig;
            }
        }
        Check(sameBig == totalBig, "③ 4,096 근처 12판 — 기준과 같고, 다른 곳은 「LF 까지 온 4,097 이상」뿐 (" + std::to_string(sameBig) + "/" + std::to_string(totalBig) + ")");
    }

    void MaxSizeTests()
    {
        Check(CTotalLengthFramer().MaxMessageSize() == 1458, "최대 — 전체 길이 머리 1,458");
        Check(CBodyLengthFramer().MaxMessageSize() == 1458, "최대 — 몸 길이 머리 1,458");
        Check(CLineFramer().MaxMessageSize() == 4096, "최대 — 줄 4,096");
    }

    // ── ④ 큰 버퍼 풀 — 서버를 띄워 줄 단위로 받는다 ──
    // 고른 값: 쓰는 곳이 없는 높은 번호(코어 시험은 16300 · 16301)
    constexpr int PORT_LINE = 16310;

    // 최대가 큰 버퍼(4,096)보다 하나 큰 부품 — Start 가 거절해야 한다
    class CTooBigFramer final : public CMessageFramer
    {
    public:
        FrameResult Next(const RecvSpan&) const override { return FrameResult::More(); }
        size_t MaxMessageSize() const override { return MAX_LARGE_PACKET_SIZE + 1; }
        const char* Name() const override { return "TooBig"; }
    };

    struct Got
    {
        std::string bytes;
        int bufferSize = 0;
    };

    // 받은 것 이벤트를 꺼내 모은다. echoLarge 면 큰 버퍼를 SubRef 대신 그대로 되돌려 보낸다(큰 버퍼가 송신 경로를 지나 큰 풀로 가는지)
    void Drain(CIOCPServer& server, std::vector<Got>& got, bool echoLarge)
    {
        NetworkEvent event(NetworkEvent::Type::CONNECTED, 0);
        while (server.PopNetworkEvent(event))
        {
            if (event.pMsg == nullptr)
                continue;
            if (event.type == NetworkEvent::Type::RECEIVED)
            {
                got.push_back(Got{ std::string(event.pMsg->GetReadBufferPtr(), static_cast<size_t>(event.pMsg->GetDataSize())),
                                   event.pMsg->GetBufferSize() });
                if (echoLarge && event.pMsg->GetBufferSize() == MSG_LARGE_SIZE)
                {
                    server.RequestSendMsg(event.sessionId, event.pMsg);   // ref 하나를 넘긴다(실패해도 거기서 돌려준다)
                    event.pMsg = nullptr;
                    continue;
                }
            }
            event.pMsg->SubRef();
            event.pMsg = nullptr;
        }
    }

    // 시한 안에 읽을 것이 오면 true
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

    // size 바이트를 다 받거나 시한이 지날 때까지
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

    // 서버가 끊었는지 — 시한 안에 읽기 쪽이 닫히면 true
    bool ClosedByPeer(SOCKET s, int timeoutMs)
    {
        if (!Readable(s, timeoutMs))
            return false;
        char buf[16];
        return ::recv(s, buf, static_cast<int>(sizeof(buf)), 0) <= 0;
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

    void LargePoolTests()
    {
        std::printf("큰 풀 청크 하나의 슬롯 수 = %d · 슬롯 하나의 버퍼 = %dB\n",
                    LockFree::CExternalTlsFreeList<CLargeSerialBuffer>::CHUNK_SIZE, MSG_LARGE_SIZE);

        CMonitorManager monitor;
        CIOCPServer server(PORT_LINE, 4, ServerMode::GameServer, monitor, 2, 2, 2, 0, 1);

        Check(!server.SetFramer(nullptr), "④ SetFramer — 빈 부품 거절");
        Check(server.SetFramer(std::make_unique<CTooBigFramer>()), "④ SetFramer — Start 전에는 받음");
        Check(!server.Start(), "④ 최대 4,097 부품이면 Start 거절(큰 버퍼 4,096 을 넘는다)");
        Check(server.SetFramer(std::make_unique<CLineFramer>()), "④ 거절 뒤 줄 단위로 다시 고름");
        if (!server.Start())
        {
            Check(false, "④ Start(줄 단위)");
            return;
        }
        Check(!server.SetFramer(std::make_unique<CLineFramer>()), "④ SetFramer — Start 뒤에는 거절");

        const int64_t alloc0 = CSerialBuffer::GetLargeAllocCount();
        const int64_t free0 = CSerialBuffer::GetLargeFreeCount();

        SOCKET s = TestHelp::ConnectRaw("127.0.0.1", PORT_LINE);
        Check(s != INVALID_SOCKET, "④ 접속");
        if (s == INVALID_SOCKET)
        {
            server.ShutdownServer();
            return;
        }

        // 1,458 이하 · 딱 4,096 · 그 사이 — 앞 하나는 기본 버퍼, 뒤 둘은 큰 버퍼
        const std::string big(CLineFramer::MAX_LINE, 'L');
        const std::string mid(2000, 'M');
        Check(SendAll(s, "short\n" + big + "\r\n" + mid + "\n"), "④ 줄 셋 보냄");
        std::vector<Got> got;
        TestHelp::WaitUntil([&] { Drain(server, got, false); return got.size() >= 3; });
        Check(got.size() == 3 && got[0].bytes == "short" && got[1].bytes == big && got[2].bytes == mid,
              "④ 줄 셋 — 5 · 4,096 · 2,000 바이트 그대로");
        Check(got.size() == 3 && got[0].bufferSize == MSG_DEFAULT_SIZE && got[1].bufferSize == MSG_LARGE_SIZE && got[2].bufferSize == MSG_LARGE_SIZE,
              "④ 담은 버퍼 — 1,458 이하는 기본(1,460) · 넘으면 큰 것(4,098)");
        // 큰 버퍼가 기본 풀로 돌아가면 풀이 프로세스를 멈춘다 — 여기까지 살아 있고 수가 맞으면 섞이지 않은 것
        Check(CSerialBuffer::GetLargeAllocCount() - alloc0 == 2 && CSerialBuffer::GetLargeFreeCount() - free0 == 2,
              "④ 큰 풀 — 낸 수 2 · 돌려받은 수 2 (" + std::to_string(CSerialBuffer::GetLargeAllocCount() - alloc0) + " · " +
              std::to_string(CSerialBuffer::GetLargeFreeCount() - free0) + ")");

        // 경계 — 1,458(기본 버퍼 딱) 은 기본, 1,459 는 큰 것
        Check(SendAll(s, std::string(MAX_PACKET_SIZE, 'a') + "\n" + std::string(MAX_PACKET_SIZE + 1, 'b') + "\n"), "④ 1,458 · 1,459 바이트 줄 보냄");
        got.clear();
        TestHelp::WaitUntil([&] { Drain(server, got, false); return got.size() >= 2; });
        Check(got.size() == 2 && got[0].bytes.size() == MAX_PACKET_SIZE && got[0].bufferSize == MSG_DEFAULT_SIZE &&
              got[1].bytes.size() == MAX_PACKET_SIZE + 1 && got[1].bufferSize == MSG_LARGE_SIZE,
              "④ 경계 — 1,458 은 기본 버퍼 · 1,459 는 큰 버퍼");
        Check(CSerialBuffer::GetLargeAllocCount() - alloc0 == 3 && CSerialBuffer::GetLargeFreeCount() - free0 == 3,
              "④ 경계 뒤 — 큰 풀 낸 수 3 · 돌려받은 수 3");

        // 큰 버퍼를 그대로 되돌려 보낸다 — 송신 경로가 큰 버퍼를 다 보내고 다 쓴 뒤 큰 풀로 돌려주는지
        const std::string echo(3000, 'E');
        Check(SendAll(s, echo + "\n"), "④ 3,000 바이트 줄 보냄");
        got.clear();
        TestHelp::WaitUntil([&] { Drain(server, got, true); return got.size() >= 1; });
        Check(RecvExactly(s, echo.size(), 3000) == echo, "④ 큰 버퍼 되돌려 보내기 — 3,000 바이트 그대로 돌아옴");
        TestHelp::WaitUntil([&] { return CSerialBuffer::GetLargeFreeCount() - free0 == 4; });
        Check(CSerialBuffer::GetLargeAllocCount() - alloc0 == 4 && CSerialBuffer::GetLargeFreeCount() - free0 == 4,
              "④ 되돌려 보낸 큰 버퍼도 보낸 뒤 큰 풀로 — 낸 수 4 · 돌려받은 수 4");

        // 한 줄 최대 + 1 → 끊는다
        const int64_t errors0 = monitor._packetErrors.Load();
        Check(SendAll(s, std::string(CLineFramer::MAX_LINE + 1, 'X') + "\n"), "④ 4,097 바이트 줄 보냄");
        Check(ClosedByPeer(s, 3000), "④ 4,097 바이트 줄 → 서버가 끊음");
        Check(monitor._packetErrors.Load() - errors0 == 1, "④ 패킷 오류 1 늘어남");
        Platform::CloseSocket(s);

        got.clear();
        Drain(server, got, false);
        Check(CSerialBuffer::GetLargeAllocCount() - alloc0 == CSerialBuffer::GetLargeFreeCount() - free0,
              "④ 끝 — 큰 풀 낸 수 = 돌려받은 수");
        server.ShutdownServer();
    }
}

int main()
{
    TotalLengthTests();
    BodyLengthTests();
    LineTests();
    LineReferenceTests();
    MaxSizeTests();
    LargePoolTests();
    return TestHelp::Finish("framer_test");
}
