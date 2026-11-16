// 접속 보탬 시험 — 생성자 포트 0(듣기 없음) · 접속 알림의 주소 · 끊긴 까닭 · 거절 알림
//   CIOCPServer 는 프로세스에 하나라서(Start 가 공용 메시지 풀을 덮어쓴다) 판마다 프로세스를 따로 띄운다.
//   인자 없이 부르면 자기 자신을 판 이름마다 다시 불러 모은다(회귀 목록이 인자 없이 부른다).
//     none = 듣는 포트가 하나도 없는 서버 — 붙기(ConnectTo)만
//     a    = 듣기는 모두 AddListener(포트 0 = OS 가 고름) — 주소 · 끊긴 까닭 · 거절
//     b    = 보낸 뒤 끊기 — 링에 64KB 가까이 쌓인 채 불러도 끝까지 받고 FIN 으로 끝남
//     c    = 살아 있음 확인 켬 · 붙기 한도 · 작은 링(받기 4,096 · 보내기 2,048)
//     d    = 받기 링이 자르기 부품 최대를 못 담으면 Start 가 false
//     e    = 큰 링(4MB) 보낸 뒤 끊기 — 커널이 다 못 받아 보내기가 걸린 채로 부른다
//   상대는 시험이 직접 연 맨 소켓이다(받는 쪽 · 거는 쪽 둘 다).
//   외부 라이브러리 없음, 실패하면 종료 값 1.
#include "TestHelp.h"   // WinSock2 — Windows.h 보다 먼저 와야 해서 맨 앞

#include "IOCPServer.h"
#include "RecvSink.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <dirent.h>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/select.h>
#endif

#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE 3      // 윈도우 ws2ipdef.h 의 TCP_KEEPALIVE(초) — 헤더 판에 따라 이 이름이 없을 수 있다
#endif
#ifndef TCP_KEEPINTVL
#define TCP_KEEPINTVL 17
#endif

void SignalProcessShutdown() {}   // 코어가 부르는 종료 알림 — 시험에는 기다리는 쪽이 없다

using TestHelp::Check;

namespace
{
    // 고른 값: 구분 값은 0 과 서로 다른 아무 수 · 세션 표 4 = 표를 채워 거절을 일으키기 쉬운 크기
    constexpr int TAG_IN   = 5;   // 구분 값 있는 듣기
    constexpr int TAG_OUT  = 9;   // ConnectTo 로 건 접속
    constexpr int MAX_CLIENTS = 4;
    constexpr int IDLE_SEC = 3;   // 쉬는 접속 끊기 — 다른 까닭을 일으키는 세션은 1초 안에 끊겨 안 걸린다

    const char* ReasonName(DisconnectReason r)
    {
        switch (r)
        {
        case DisconnectReason::IoError:      return "IoError";
        case DisconnectReason::PeerClosed:   return "PeerClosed";
        case DisconnectReason::LocalRequest: return "LocalRequest";
        case DisconnectReason::IdleTimeout:  return "IdleTimeout";
        case DisconnectReason::BadMessage:   return "BadMessage";
        case DisconnectReason::SendOverflow: return "SendOverflow";
        case DisconnectReason::AfterSend:    return "AfterSend";
        }
        return "?";
    }

    const char* RefuseName(RefuseReason r)
    {
        switch (r)
        {
        case RefuseReason::SessionTableFull: return "SessionTableFull";
        case RefuseReason::UntaggedLimit:    return "UntaggedLimit";
        case RefuseReason::EventQueueBusy:   return "EventQueueBusy";
        }
        return "?";
    }

    // 알림 넷을 그대로 적는다
    class CRecordingSink final : public CRecvSink
    {
    public:
        void OnConnected(const SinkConnected& info) override
        {
            std::lock_guard<std::mutex> lock(_lock);
            _connected.push_back(info);
            _order.push_back({ info.sessionId, 'C' });
        }
        void OnReceived(const SinkReceived& info) override
        {
            {
                std::lock_guard<std::mutex> lock(_lock);
                _received.push_back({ info.sessionId, std::string(info.msg->GetReadBufferPtr(), static_cast<size_t>(info.msg->GetDataSize())) });
                _order.push_back({ info.sessionId, 'R' });
            }
            info.msg->SubRef();
        }
        void OnDisconnected(const SinkDisconnected& info) override
        {
            std::lock_guard<std::mutex> lock(_lock);
            _disconnected.push_back(info);
            _order.push_back({ info.sessionId, 'D' });
        }
        void OnRefused(const SinkRefused& info) override
        {
            std::lock_guard<std::mutex> lock(_lock);
            _refused.push_back(info);
        }

        std::vector<SinkConnected>    Connected()    { std::lock_guard<std::mutex> l(_lock); return _connected; }
        std::vector<SinkDisconnected> Disconnected() { std::lock_guard<std::mutex> l(_lock); return _disconnected; }
        std::vector<SinkRefused>      Refused()      { std::lock_guard<std::mutex> l(_lock); return _refused; }
        std::vector<std::pair<int64_t, std::string>> Received() { std::lock_guard<std::mutex> l(_lock); return _received; }

        // 이 세션의 알림 차례('C' · 'R' · 'D')
        std::string OrderOf(int64_t sessionId)
        {
            std::lock_guard<std::mutex> l(_lock);
            std::string out;
            for (const auto& e : _order)
                if (e.first == sessionId)
                    out += e.second;
            return out;
        }

        // 상대 포트로 받은 접속을 찾는다(받은 접속의 상대 포트 = 시험 소켓의 이쪽 포트). 없으면 0
        bool FindByPeerPort(uint16_t port, SinkConnected& out)
        {
            std::lock_guard<std::mutex> l(_lock);
            for (const SinkConnected& c : _connected)
                if (c.peerPort == port)
                {
                    out = c;
                    return true;
                }
            return false;
        }

        bool FindDisconnect(int64_t sessionId, SinkDisconnected& out)
        {
            std::lock_guard<std::mutex> l(_lock);
            for (const SinkDisconnected& d : _disconnected)
                if (d.sessionId == sessionId)
                {
                    out = d;
                    return true;
                }
            return false;
        }

    private:
        std::mutex _lock;
        std::vector<SinkConnected> _connected;
        std::vector<SinkDisconnected> _disconnected;
        std::vector<SinkRefused> _refused;
        std::vector<std::pair<int64_t, std::string>> _received;
        std::vector<std::pair<int64_t, char>> _order;
    };

    uint32_t LoopbackNet()
    {
        in_addr a{};
        inet_pton(AF_INET, "127.0.0.1", &a);
        return a.s_addr;
    }

    uint16_t LocalPort(SOCKET s)
    {
        sockaddr_in a{};
        socklen_t len = sizeof(a);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0)
            return 0;
        return ntohs(a.sin_port);
    }

    uint16_t PeerPort(SOCKET s)
    {
        sockaddr_in a{};
        socklen_t len = sizeof(a);
        if (getpeername(s, reinterpret_cast<sockaddr*>(&a), &len) != 0)
            return 0;
        return ntohs(a.sin_port);
    }

    // 시험이 직접 여는 듣기 — 127.0.0.1 의 빈 포트
    SOCKET RawListen(uint16_t& port)
    {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            return INVALID_SOCKET;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = LoopbackNet();
        a.sin_port = 0;
        if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(s, 16) != 0)
        {
            Platform::CloseSocket(s);
            return INVALID_SOCKET;
        }
        port = LocalPort(s);
        return s;
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

    SOCKET AcceptWithin(SOCKET listener, int timeoutMs)
    {
        if (!Readable(listener, timeoutMs))
            return INVALID_SOCKET;
        return ::accept(listener, nullptr, nullptr);
    }

    // 상대가 닫았나(받기가 0 이나 실패) — 시한 안에
    bool ClosedWithin(SOCKET s, int timeoutMs)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        char buf[4096];
        while (std::chrono::steady_clock::now() < until)
        {
            if (!Readable(s, 50))
                continue;
            if (::recv(s, buf, static_cast<int>(sizeof(buf)), 0) <= 0)
                return true;
        }
        return false;
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

    // 강제 끊기(RST) — LINGER 를 켜고 0초로 닫는다
    void CloseWithReset(SOCKET s)
    {
        linger lg{};
        lg.l_onoff = 1;
        lg.l_linger = 0;
        ::setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
        Platform::CloseSocket(s);
    }

    // 전체 길이 머리(4바이트: 전체 크기 2 · 종류 2) + 몸
    std::string Packet(uint16_t type, const std::string& body)
    {
        const uint16_t size = static_cast<uint16_t>(sizeof(MsgHeader) + body.size());
        std::string out(sizeof(MsgHeader), '\0');
        out[0] = static_cast<char>(size & 0xFF);
        out[1] = static_cast<char>(size >> 8);
        out[2] = static_cast<char>(type & 0xFF);
        out[3] = static_cast<char>(type >> 8);
        return out + body;
    }

    // 이 프로세스가 쥔 듣기(LISTEN) 소켓 수 — 생성자 포트를 정말 안 열었는지 운영체제 표로 센다. 못 읽으면 -1
    int CountOwnListeningSockets()
    {
#ifdef _WIN32
        ULONG size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
        std::vector<unsigned char> buf(size + 4096);
        size = static_cast<ULONG>(buf.size());
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
            return -1;
        const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
        const DWORD pid = GetCurrentProcessId();
        int count = 0;
        for (DWORD i = 0; i < table->dwNumEntries; ++i)
            if (table->table[i].dwOwningPid == pid)
                ++count;
        return count;
#else
        // 내 fd 중 소켓의 inode → /proc/net/tcp 에서 상태 0A(LISTEN) 인 줄과 맞춘다
        std::set<unsigned long> inodes;
        DIR* dir = opendir("/proc/self/fd");
        if (dir == nullptr)
            return -1;
        while (dirent* e = readdir(dir))
        {
            char path[512];
            char target[512];
            std::snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
            const ssize_t n = readlink(path, target, sizeof(target) - 1);
            if (n <= 0)
                continue;
            target[n] = '\0';
            unsigned long inode = 0;
            if (std::sscanf(target, "socket:[%lu]", &inode) == 1)
                inodes.insert(inode);
        }
        closedir(dir);
        std::ifstream tcp("/proc/net/tcp");
        if (!tcp)
            return -1;
        std::string line;
        std::getline(tcp, line);   // 머리 줄
        int count = 0;
        while (std::getline(tcp, line))
        {
            std::istringstream ss(line);
            std::string sl, local, remote, st, txrx, trtm, retr, uid, timeout;
            unsigned long inode = 0;
            ss >> sl >> local >> remote >> st >> txrx >> trtm >> retr >> uid >> timeout >> inode;
            if (st == "0A" && inodes.count(inode) != 0)
                ++count;
        }
        return count;
#endif
    }

    // 이 프로세스가 쥔 소켓 가운데 이쪽 포트 · 상대 포트가 맞는 것 — 서버가 쥔 세션 소켓의 옵션을 시험이 직접 읽으려고. 없으면 INVALID_SOCKET
    //   윈도우는 소켓도 프로세스 핸들이라 작은 핸들 값을 차례로 물어본다(소켓이 아니면 getsockname 이 실패) · 리눅스는 /proc/self/fd
    SOCKET FindOwnSocket(uint16_t localPort, uint16_t peerPort)
    {
        auto matches = [&](SOCKET s)
        {
            sockaddr_in a{};
            socklen_t len = sizeof(a);
            if (getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0 || ntohs(a.sin_port) != localPort)
                return false;
            sockaddr_in b{};
            len = sizeof(b);
            return getpeername(s, reinterpret_cast<sockaddr*>(&b), &len) == 0 && ntohs(b.sin_port) == peerPort;
        };
#ifdef _WIN32
        for (uintptr_t h = 4; h < 0x10000; h += 4)
            if (matches(static_cast<SOCKET>(h)))
                return static_cast<SOCKET>(h);
        return INVALID_SOCKET;
#else
        DIR* dir = opendir("/proc/self/fd");
        if (dir == nullptr)
            return INVALID_SOCKET;
        SOCKET found = INVALID_SOCKET;
        while (dirent* e = readdir(dir))
        {
            const int fd = std::atoi(e->d_name);
            if (fd > 2 && matches(fd))
            {
                found = fd;
                break;
            }
        }
        closedir(dir);
        return found;
#endif
    }

    // 소켓 옵션 하나(int) — 못 읽으면 -1
    //   윈도우는 참거짓 옵션(SO_KEEPALIVE 등)을 1바이트만 쓰고 길이 1 을 돌려준다 — 그때는 그 1바이트를 읽는다
    int SockOptInt(SOCKET s, int level, int name)
    {
        unsigned char bytes[sizeof(int)] = {};
        socklen_t len = sizeof(bytes);
        if (getsockopt(s, level, name, reinterpret_cast<char*>(bytes), &len) != 0)
            return -1;
        if (len == 1)
            return bytes[0];
        int v = 0;
        std::memcpy(&v, bytes, sizeof(v));
        return v;
    }

    // 묶기만 하고 안 듣는 포트 — 붙으면 거절이 온다
    SOCKET BindOnly(uint16_t& port)
    {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            return INVALID_SOCKET;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = LoopbackNet();
        if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
        {
            Platform::CloseSocket(s);
            return INVALID_SOCKET;
        }
        port = LocalPort(s);
        return s;
    }

    long long ElapsedMs(std::chrono::steady_clock::time_point from)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count();
    }

    size_t CountFrom(CRecordingSink* sink, int64_t sessionId)
    {
        size_t n = 0;
        for (const auto& r : sink->Received())
            if (r.first == sessionId)
                ++n;
        return n;
    }

    // 직렬화 버퍼 한 통(전체 길이 머리 + 몸)을 RequestSendMsg 로 — 링 길(SendOverflow 의 다른 자리)을 보려고
    bool SendMsg(CIOCPServer& server, int64_t sessionId, const std::string& body, SendFlush flush)
    {
        CSerialBuffer* msg = CSerialBuffer::Alloc();
        MsgHeader header;
        header.size = static_cast<uint16_t>(sizeof(MsgHeader) + body.size());
        header.type = static_cast<MsgType>(3000);
        std::memcpy(msg->GetWriteBufferPtr(), &header, sizeof(header));
        msg->MoveWritePos(static_cast<int>(sizeof(header)));
        std::memcpy(msg->GetWriteBufferPtr(), body.data(), body.size());
        msg->MoveWritePos(static_cast<int>(body.size()));
        return server.RequestSendMsg(sessionId, msg, flush);   // ref 하나를 넘긴다(실패해도 거기서 돌려준다)
    }

    // ── 판 none — 듣는 포트가 없는 서버. 붙기만 한다 ──
    void CaseNone()
    {
        CMonitorManager monitor;
        auto sinkOwned = std::make_unique<CRecordingSink>();
        CRecordingSink* sink = sinkOwned.get();

        CIOCPServer server(0, 2, "ConnExtrasTest", monitor, 2, 2, 2, 0, 1);
        Check(server.SetRecvSink(std::move(sinkOwned)), "none · SetRecvSink");
        Check(server.SetIdleTimeoutSec(0), "none · 쉬는 접속 끊기 끔");
        if (!server.Start())
        {
            Check(false, "none · ① 생성자 포트 0 · AddListener 없음으로 Start");
            return;
        }
        Check(true, "none · ① 생성자 포트 0 · AddListener 없음으로 Start");
        const int listening = CountOwnListeningSockets();
        Check(listening == 0, "none · ① 이 프로세스의 듣기 소켓 0 (" + std::to_string(listening) + ")");
        Check(server.GetListenerPort(0) == 0, "none · ① GetListenerPort(0) = 0 (AddListener 없음)");

        uint16_t lport = 0;
        SOCKET l = RawListen(lport);
        Check(l != INVALID_SOCKET && lport != 0, "none · 상대 듣기(시험 소켓) 열기");
        const int64_t sid = server.ConnectTo("127.0.0.1", lport, TAG_OUT);
        Check(sid != 0, "none · ① ConnectTo 성공 — 듣기 없이 붙기만 됨");
        SOCKET peer = AcceptWithin(l, 2000);
        Check(peer != INVALID_SOCKET, "none · 상대가 접속을 받음");

        std::vector<SinkConnected> cs = sink->Connected();
        Check(cs.size() == 1 && cs[0].sessionId == sid && cs[0].outbound && cs[0].tag == TAG_OUT,
              "none · ② 접속 알림 하나 — 건 접속(outbound) · 구분 값 9");

        const std::string p = Packet(3, "hello");
        Check(SendAll(peer, p), "none · 상대가 메시지 보냄");
        TestHelp::WaitUntil([&] { return !sink->Received().empty(); }, 3000);
        const auto rs = sink->Received();
        Check(rs.size() == 1 && rs[0].first == sid && rs[0].second == p, "none · 받음 하나 — 그대로");

        Check(server.RequestDisconnectSession(sid), "none · 이쪽에서 끊기");
        Check(ClosedWithin(peer, 3000), "none · 상대 쪽에서 끊김을 봄");
        TestHelp::WaitUntil([&] { return !sink->Disconnected().empty(); }, 3000);
        SinkDisconnected d;
        Check(sink->FindDisconnect(sid, d) && d.reason == DisconnectReason::LocalRequest && d.tag == TAG_OUT,
              std::string("none · ⑥ 끊긴 까닭 LocalRequest (") + (sink->FindDisconnect(sid, d) ? ReasonName(d.reason) : "없음") + ")");
        Check(sink->OrderOf(sid) == "CRD", "none · 알림 차례 접속 · 받음 · 끊김 (" + sink->OrderOf(sid) + ")");

        Platform::CloseSocket(peer);
        Platform::CloseSocket(l);
        server.ShutdownServer();
    }

    // 받은 접속 하나를 열고 그 세션 알림을 기다린다 — 실패하면 sessionId 0
    struct Opened
    {
        SOCKET        sock = INVALID_SOCKET;
        SinkConnected info;
    };

    Opened OpenAccepted(CRecordingSink* sink, int port)
    {
        Opened o;
        o.sock = TestHelp::ConnectRaw("127.0.0.1", port);
        if (o.sock == INVALID_SOCKET)
            return o;
        const uint16_t mine = LocalPort(o.sock);
        TestHelp::WaitUntil([&] { return sink->FindByPeerPort(mine, o.info); }, 3000);
        return o;
    }

    // ── 판 a — 듣기는 모두 AddListener. 주소 · 끊긴 까닭 · 거절 ──
    void CaseA()
    {
        CMonitorManager monitor;
        auto sinkOwned = std::make_unique<CRecordingSink>();
        CRecordingSink* sink = sinkOwned.get();

        CIOCPServer server(0, MAX_CLIENTS, "ConnExtrasTest", monitor, 2, 2, 2, 0, 1);
        Check(server.AddListener("127.0.0.1", 0, TAG_IN), "a · AddListener 127.0.0.1:0 구분 값 5");
        Check(server.AddListener("127.0.0.1", 0, 0), "a · AddListener 127.0.0.1:0 구분 값 0");
        server.SetUntaggedSessionLimit(1);
        server.SetEventQueueAcceptLimit(1);   // 큐 길이는 시험이 지표를 직접 올려 일으킨다(게임 루프가 없다)
        Check(server.SetIdleTimeoutSec(IDLE_SEC), "a · 쉬는 접속 끊기 3초");
        Check(server.SetRecvSink(std::move(sinkOwned)), "a · SetRecvSink");
        if (!server.Start())
        {
            Check(false, "a · Start");
            return;
        }
        const int portIn = server.GetListenerPort(0);
        const int port0 = server.GetListenerPort(1);
        Check(portIn != 0 && port0 != 0 && portIn != port0 && server.GetListenerPort(2) == 0,
              "a · 포트 0 으로 연 듣기 둘의 실제 포트 (" + std::to_string(portIn) + " · " + std::to_string(port0) + ") · 셋째 0");
        const int listening = CountOwnListeningSockets();
        Check(listening == 2, "a · ① 듣기 소켓 = AddListener 둘뿐 (" + std::to_string(listening) + ") — 생성자 포트는 안 엶");

        const uint32_t lo = LoopbackNet();

        // ── ② 받은 접속 둘(구분 값 5 · 0) · 건 접속 하나 — 주소 칸 ──
        Opened c1 = OpenAccepted(sink, portIn);
        Check(c1.info.sessionId != 0 && !c1.info.outbound && c1.info.tag == TAG_IN
              && c1.info.peerIp == lo && c1.info.peerPort == LocalPort(c1.sock)
              && c1.info.localIp == lo && c1.info.localPort == portIn,
              "a · ② 받은 접속(구분 값 5) — 상대 = 시험 소켓 주소 · 이쪽 = 127.0.0.1:" + std::to_string(portIn)
              + " (상대 포트 " + std::to_string(c1.info.peerPort) + " · 이쪽 포트 " + std::to_string(c1.info.localPort) + ")");
        Opened c0 = OpenAccepted(sink, port0);
        Check(c0.info.sessionId != 0 && !c0.info.outbound && c0.info.tag == 0
              && c0.info.peerIp == lo && c0.info.peerPort == LocalPort(c0.sock) && c0.info.localPort == port0,
              "a · ② 받은 접속(구분 값 0) — 주소 · 구분 값");

        // ── ④ 끔(기본) — 켠 수 0 · 서버가 쥔 받은 소켓의 SO_KEEPALIVE 0 ──
        {
            const SOCKET serverSide = FindOwnSocket(static_cast<uint16_t>(portIn), LocalPort(c1.sock));
            const int ka = (serverSide != INVALID_SOCKET) ? SockOptInt(serverSide, SOL_SOCKET, SO_KEEPALIVE) : -9;
            Check(server.GetKeepAliveOnCount() == 0 && ka == 0,
                  "a · ④ 살아 있음 확인 끔(기본) — 켠 수 " + std::to_string(server.GetKeepAliveOnCount()) + " · 서버 쪽 소켓 SO_KEEPALIVE " + std::to_string(ka));
        }

        uint16_t lport = 0;
        SOCKET l = RawListen(lport);
        const int64_t s9 = server.ConnectTo("127.0.0.1", lport, TAG_OUT);
        SOCKET p9 = AcceptWithin(l, 2000);
        SinkConnected o9{};
        for (const SinkConnected& c : sink->Connected())
            if (c.sessionId == s9)
                o9 = c;
        Check(s9 != 0 && p9 != INVALID_SOCKET && o9.sessionId == s9 && o9.outbound && o9.tag == TAG_OUT
              && o9.peerIp == lo && o9.peerPort == lport && o9.localIp == lo && o9.localPort == PeerPort(p9),
              "a · ② 건 접속 — 상대 = 상대 듣기 포트 " + std::to_string(lport) + " · 이쪽 = 상대가 본 포트 "
              + std::to_string(p9 != INVALID_SOCKET ? PeerPort(p9) : 0) + " (알림 " + std::to_string(o9.peerPort) + " · " + std::to_string(o9.localPort) + ")");

        // ── ⑥ 끊긴 까닭 — 갈래마다 하나씩 ──
        Platform::CloseSocket(c1.sock);                 // 정상 닫기(FIN) → PeerClosed
        CloseWithReset(c0.sock);                        // 강제 끊기(RST) → IoError
        Check(server.RequestDisconnectSession(s9), "a · ⑥ 이쪽에서 끊기 부름");   // → LocalRequest
        Check(ClosedWithin(p9, 3000), "a · ⑥ 이쪽에서 끊은 접속을 상대가 봄");

        Opened c2 = OpenAccepted(sink, portIn);
        Check(c2.info.sessionId != 0 && SendAll(c2.sock, std::string("\x02\x00\x01\x00", 4)),
              "a · ⑥ 틀린 머리(전체 크기 2 < 머리 4) 보냄");   // → BadMessage
        Check(ClosedWithin(c2.sock, 3000), "a · ⑥ 틀린 머리를 보낸 접속이 끊김");

        Opened c3 = OpenAccepted(sink, portIn);
        const std::string big(70000, 'z');   // 보내기 링(65,535 · RIO 65,536)보다 크다
        Check(c3.info.sessionId != 0 && !server.RequestSendRaw(c3.info.sessionId, big.data(), static_cast<int>(big.size())),
              "a · ⑥ 링보다 큰 보내기 → 거절");   // → SendOverflow
        Check(ClosedWithin(c3.sock, 3000), "a · ⑥ 보내기가 넘친 접속이 끊김");

        const auto idleStart = std::chrono::steady_clock::now();
        Opened c4 = OpenAccepted(sink, portIn);         // 아무것도 안 보냄 → IdleTimeout
        const bool idleClosed = ClosedWithin(c4.sock, (IDLE_SEC + 3) * 1000);
        const double idleSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - idleStart).count();
        char idleText[32];
        std::snprintf(idleText, sizeof(idleText), "%.2f", idleSec);
        Check(idleClosed && idleSec >= IDLE_SEC - 0.5, std::string("a · ⑥ 조용한 접속이 쉬는 접속 끊기로 끊김 (") + idleText + "초)");

        TestHelp::WaitUntil([&] { return sink->Disconnected().size() >= 6; }, 3000);
        struct Want { const char* who; int64_t sid; DisconnectReason reason; int tag; };
        const Want wants[] = {
            { "정상 닫기",   c1.info.sessionId, DisconnectReason::PeerClosed,   TAG_IN },
            { "강제 끊기",   c0.info.sessionId, DisconnectReason::IoError,      0 },
            { "이쪽 끊기",   s9,                DisconnectReason::LocalRequest, TAG_OUT },
            { "틀린 머리",   c2.info.sessionId, DisconnectReason::BadMessage,   TAG_IN },
            { "보내기 넘침", c3.info.sessionId, DisconnectReason::SendOverflow, TAG_IN },
            { "조용함",      c4.info.sessionId, DisconnectReason::IdleTimeout,  TAG_IN },
        };
        for (const Want& w : wants)
        {
            SinkDisconnected d;
            const bool found = sink->FindDisconnect(w.sid, d);
            Check(found && d.reason == w.reason && d.tag == w.tag,
                  std::string("a · ⑥ ") + w.who + " → " + ReasonName(w.reason) + " (" + (found ? ReasonName(d.reason) : "끊김 알림 없음")
                  + " · 구분 값 " + std::to_string(found ? d.tag : -1) + ")");
        }
        // 갈래마다 개수 — 같은 까닭이 둘 나오거나 빠진 갈래가 없나
        int counts[7] = { 0, 0, 0, 0, 0, 0, 0 };
        const std::vector<SinkDisconnected> ds = sink->Disconnected();
        for (const SinkDisconnected& d : ds)
            ++counts[static_cast<int>(d.reason)];
        std::string countText;
        for (int i = 0; i < 7; ++i)
            countText += std::string(i ? " · " : "") + ReasonName(static_cast<DisconnectReason>(i)) + " " + std::to_string(counts[i]);
        Check(ds.size() == 6 && counts[0] == 1 && counts[1] == 1 && counts[2] == 1 && counts[3] == 1 && counts[4] == 1 && counts[5] == 1 && counts[6] == 0,
              "a · ⑥ 끊김 6 · 까닭마다 1 · AfterSend 0 (" + countText + ")");
        for (const Want& w : wants)
        {
            const std::string order = sink->OrderOf(w.sid);
            if (order != "CD" && order != "CRD")
                Check(false, std::string("a · 알림 차례 — ") + w.who + " " + order);
        }
        Platform::CloseSocket(c2.sock);
        Platform::CloseSocket(c3.sock);
        Platform::CloseSocket(c4.sock);
        Platform::CloseSocket(p9);

        // ── ⑧ 거절 — 세션 표가 참(구분 값 5) · 구분 값 0 상한 · 이벤트 큐 과부하 ──
        const int64_t acceptFailed0 = monitor._acceptFailed.Load();
        const int64_t rejectedQueue0 = monitor._acceptRejectedByQueue.Load();
        std::vector<Opened> fill;
        for (int i = 0; i < MAX_CLIENTS; ++i)
            fill.push_back(OpenAccepted(sink, portIn));
        bool filled = true;
        for (const Opened& o : fill)
            filled = filled && o.info.sessionId != 0;
        Check(filled, "a · ⑧ 세션 표 4 칸을 구분 값 5 접속으로 채움");
        SOCKET over = TestHelp::ConnectRaw("127.0.0.1", portIn);
        const uint16_t overPort = (over != INVALID_SOCKET) ? LocalPort(over) : 0;
        Check(over != INVALID_SOCKET && ClosedWithin(over, 3000), "a · ⑧ 다섯째 접속이 바로 닫힘");
        TestHelp::WaitUntil([&] { return sink->Refused().size() >= 1; }, 2000);

        // 건 접속은 거절 알림이 아니라 ConnectTo 의 0 으로 안다
        const int64_t sFull = server.ConnectTo("127.0.0.1", lport, TAG_OUT);
        SOCKET pFull = AcceptWithin(l, 1000);
        Check(sFull == 0, "a · ⑧ 표가 찼을 때 ConnectTo → 0");
        if (pFull != INVALID_SOCKET)
            Platform::CloseSocket(pFull);

        for (Opened& o : fill)
            Platform::CloseSocket(o.sock);
        TestHelp::WaitUntil([&] { return sink->Disconnected().size() >= 6 + static_cast<size_t>(MAX_CLIENTS); }, 3000);

        Opened u1 = OpenAccepted(sink, port0);
        Check(u1.info.sessionId != 0, "a · ⑧ 구분 값 0 첫 접속은 받음(상한 1)");
        SOCKET u2 = TestHelp::ConnectRaw("127.0.0.1", port0);
        const uint16_t u2Port = (u2 != INVALID_SOCKET) ? LocalPort(u2) : 0;
        Check(u2 != INVALID_SOCKET && ClosedWithin(u2, 3000), "a · ⑧ 구분 값 0 둘째 접속이 바로 닫힘(상한)");
        TestHelp::WaitUntil([&] { return sink->Refused().size() >= 2; }, 2000);

        monitor._gameLoop._eventQueueSize.Store(1);   // 큐 과부하 — 구분 값 0 만 막는다
        SOCKET q0 = TestHelp::ConnectRaw("127.0.0.1", port0);
        const uint16_t q0Port = (q0 != INVALID_SOCKET) ? LocalPort(q0) : 0;
        Check(q0 != INVALID_SOCKET && ClosedWithin(q0, 3000), "a · ⑧ 큐 과부하 중 구분 값 0 접속이 바로 닫힘");
        TestHelp::WaitUntil([&] { return sink->Refused().size() >= 3; }, 2000);
        Opened q5 = OpenAccepted(sink, portIn);
        Check(q5.info.sessionId != 0, "a · ⑧ 큐 과부하 중에도 구분 값 5 접속은 받음");
        monitor._gameLoop._eventQueueSize.Store(0);

        const std::vector<SinkRefused> rf = sink->Refused();
        Check(rf.size() == 3, "a · ⑧ 거절 알림 3 (" + std::to_string(rf.size()) + ") — 건 접속 거절은 안 옴");
        struct WantRefuse { const char* who; RefuseReason reason; int tag; uint16_t port; };
        const WantRefuse wr[] = {
            { "표가 참",     RefuseReason::SessionTableFull, TAG_IN, overPort },
            { "구분 값 0 상한", RefuseReason::UntaggedLimit, 0,      u2Port },
            { "큐 과부하",   RefuseReason::EventQueueBusy,   0,      q0Port },
        };
        for (size_t i = 0; i < 3; ++i)
        {
            const bool ok = i < rf.size() && rf[i].reason == wr[i].reason && rf[i].tag == wr[i].tag
                            && rf[i].peerIp == lo && rf[i].peerPort == wr[i].port;
            Check(ok, std::string("a · ⑧ ") + wr[i].who + " → " + RefuseName(wr[i].reason) + " · 구분 값 " + std::to_string(wr[i].tag)
                      + " · 상대 포트 " + std::to_string(wr[i].port)
                      + (i < rf.size() ? std::string(" (알림 ") + RefuseName(rf[i].reason) + " · " + std::to_string(rf[i].tag) + " · " + std::to_string(rf[i].peerPort) + ")" : std::string(" (없음)")));
        }
        Check(monitor._acceptFailed.Load() - acceptFailed0 == 3 && monitor._acceptRejectedByQueue.Load() - rejectedQueue0 == 1,
              "a · ⑧ 코어 셈 — 받기 실패 3(표 · 상한 · 건 접속) · 큐 거절 1 ("
              + std::to_string(monitor._acceptFailed.Load() - acceptFailed0) + " · " + std::to_string(monitor._acceptRejectedByQueue.Load() - rejectedQueue0) + ")");

        Platform::CloseSocket(over);
        Platform::CloseSocket(u2);
        Platform::CloseSocket(q0);
        Platform::CloseSocket(u1.sock);
        Platform::CloseSocket(q5.sock);
        Platform::CloseSocket(l);
        server.ShutdownServer();
    }

    // 받기 버퍼를 작게 잡고 붙는다 — 서버 커널에 못 보낸 것이 남은 채 닫히게 해, 강제 끊기(RST)면 꼬리를 잃게 한다
    SOCKET ConnectSmallRecv(int port, int recvBuf)
    {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            return INVALID_SOCKET;
        ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvBuf), sizeof(recvBuf));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = LoopbackNet();
        a.sin_port = htons(static_cast<uint16_t>(port));
        if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
        {
            Platform::CloseSocket(s);
            return INVALID_SOCKET;
        }
        return s;
    }

    // 끝까지 읽는다 — 끝 = 받기 0(FIN) 이면 lastRc 0 · 실패(RST 등)면 -1 · 시한이면 1
    std::string ReadToEnd(SOCKET s, int timeoutMs, int& lastRc)
    {
        std::string out;
        lastRc = 1;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        char buf[4096];
        while (std::chrono::steady_clock::now() < until)
        {
            if (!Readable(s, 50))
                continue;
            const int n = static_cast<int>(::recv(s, buf, static_cast<int>(sizeof(buf)), 0));
            if (n > 0)
            {
                out.append(buf, static_cast<size_t>(n));
                continue;
            }
            lastRc = (n == 0) ? 0 : -1;
            break;
        }
        return out;
    }

    // ── 판 b — 보낸 뒤 끊기 ──
    void CaseB()
    {
        CMonitorManager monitor;
        auto sinkOwned = std::make_unique<CRecordingSink>();
        CRecordingSink* sink = sinkOwned.get();

        CIOCPServer server(0, MAX_CLIENTS, "ConnExtrasTest", monitor, 2, 2, 2, 0, 1);
        Check(server.AddListener("127.0.0.1", 0, TAG_IN), "b · AddListener 127.0.0.1:0");
        Check(server.SetIdleTimeoutSec(0), "b · 쉬는 접속 끊기 끔");
        Check(server.SetRecvSink(std::move(sinkOwned)), "b · SetRecvSink");
        if (!server.Start())
        {
            Check(false, "b · Start");
            return;
        }
        const int port = server.GetListenerPort(0);

        // ③-1 링에 60,000 바이트(65,535 아래 · 64KB 가까이)를 쌓아 두고 부른다
        //   RequestSendRaw 는 틱 끝 묶음 보내기라 FlushPendingSends 전에는 링에 머문다 — 보내기는 이 함수가 일으킨다
        SOCKET c = ConnectSmallRecv(port, 4096);
        SinkConnected ci{};
        const uint16_t cport = (c != INVALID_SOCKET) ? LocalPort(c) : 0;
        TestHelp::WaitUntil([&] { return sink->FindByPeerPort(cport, ci); }, 3000);
        Check(c != INVALID_SOCKET && ci.sessionId != 0, "b · ③ 접속(받기 버퍼 4,096)");
        std::string payload(60000, '\0');
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>((i * 31 + 7) & 0xFF);
        Check(server.RequestSendRaw(ci.sessionId, payload.data(), static_cast<int>(payload.size())), "b · ③ 링에 60,000 바이트 넣음(아직 안 보냄)");
        Check(server.RequestDisconnectAfterSend(ci.sessionId), "b · ③ 보낸 뒤 끊기 부름");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // 상대는 아직 안 읽는다 — 서버 쪽에 못 보낸 것이 남게
        int lastRc = 1;
        const std::string got = ReadToEnd(c, 5000, lastRc);
        Check(got.size() == payload.size() && got == payload,
              "b · ③ 끝까지 받음 — " + std::to_string(got.size()) + " / " + std::to_string(payload.size()) + " 바이트 · 내용 같음 " + (got == payload ? "예" : "아니오"));
        Check(lastRc == 0, std::string("b · ③ 끝이 FIN(받기 0) — 강제 끊기(RST) 아님 (") + (lastRc == 0 ? "FIN" : lastRc < 0 ? "실패" : "시한") + ")");
        TestHelp::WaitUntil([&] { return !sink->Disconnected().empty(); }, 3000);
        SinkDisconnected d;
        Check(sink->FindDisconnect(ci.sessionId, d) && d.reason == DisconnectReason::AfterSend && d.tag == TAG_IN,
              std::string("b · ③ 끊긴 까닭 AfterSend (") + (sink->FindDisconnect(ci.sessionId, d) ? ReasonName(d.reason) : "없음") + ")");
        Check(!server.RequestDisconnectAfterSend(ci.sessionId), "b · ③ 끊긴 세션에 다시 부르면 false");
        Platform::CloseSocket(c);

        // ③-2 보낼 것이 없을 때 — 바로 끊긴다
        SOCKET e = TestHelp::ConnectRaw("127.0.0.1", port);
        SinkConnected ei{};
        const uint16_t eport = (e != INVALID_SOCKET) ? LocalPort(e) : 0;
        TestHelp::WaitUntil([&] { return sink->FindByPeerPort(eport, ei); }, 3000);
        Check(ei.sessionId != 0 && server.RequestDisconnectAfterSend(ei.sessionId), "b · ③ 빈 링에서 부름");
        int lastE = 1;
        const std::string gotE = ReadToEnd(e, 3000, lastE);
        Check(gotE.empty() && lastE == 0, "b · ③ 빈 링 — 받은 것 0 · 끝이 FIN (" + std::to_string(gotE.size()) + " · " + std::to_string(lastE) + ")");
        TestHelp::WaitUntil([&] { return sink->Disconnected().size() >= 2; }, 3000);
        Check(sink->FindDisconnect(ei.sessionId, d) && d.reason == DisconnectReason::AfterSend, "b · ③ 빈 링 — 까닭 AfterSend");
        Platform::CloseSocket(e);

        // ③-3 없는 세션
        Check(!server.RequestDisconnectAfterSend(0x0000123456789), "b · ③ 없는 세션 번호 → false");

        // 끊김은 둘뿐 — 한 세션에 두 번 안 옴
        Check(sink->Disconnected().size() == 2, "b · ③ 끊김 알림 2 (" + std::to_string(sink->Disconnected().size()) + ")");
        server.ShutdownServer();
    }

    // ── 판 c — 살아 있음 확인 켬 · 붙기 한도 · 작은 링 ──
    void CaseC()
    {
        CMonitorManager monitor;
        auto sinkOwned = std::make_unique<CRecordingSink>();
        CRecordingSink* sink = sinkOwned.get();

        CIOCPServer server(0, MAX_CLIENTS, "ConnExtrasTest", monitor, 2, 2, 2, 0, 1);
        Check(server.AddListener("127.0.0.1", 0, TAG_IN), "c · AddListener 127.0.0.1:0");
        Check(server.SetIdleTimeoutSec(0), "c · 쉬는 접속 끊기 끔");
        Check(!server.SetKeepAlive(1000, 0), "c · ④ 한쪽만 0 이면 false");
        Check(server.SetKeepAlive(30000, 5000), "c · ④ 살아 있음 확인 30초 · 5초(받은 접속만)");
        Check(!server.SetSessionBufferSizes(0, 2048), "c · ⑦ 0 이면 false");
        Check(server.SetSessionBufferSizes(4096, 2048), "c · ⑦ 받기 4,096 · 보내기 2,048");
        Check(server.SetRecvSink(std::move(sinkOwned)), "c · SetRecvSink");
        if (!server.Start())
        {
            Check(false, "c · Start");
            return;
        }
        Check(!server.SetKeepAlive(1000, 1000) && !server.SetSessionBufferSizes(8192, 8192), "c · Start 뒤 설정은 false");
        const int port = server.GetListenerPort(0);

        // ── ④ 받은 접속 둘 — 켠 수 2 · 서버 쪽 소켓 옵션 ──
        Opened k1 = OpenAccepted(sink, port);
        Opened k2 = OpenAccepted(sink, port);
        Check(k1.info.sessionId != 0 && k2.info.sessionId != 0 && server.GetKeepAliveOnCount() == 2,
              "c · ④ 받은 접속 2 → 켠 수 " + std::to_string(server.GetKeepAliveOnCount()));
        const SOCKET k1Server = (k1.sock != INVALID_SOCKET) ? FindOwnSocket(static_cast<uint16_t>(port), LocalPort(k1.sock)) : INVALID_SOCKET;
        const int kaOn = (k1Server != INVALID_SOCKET) ? SockOptInt(k1Server, SOL_SOCKET, SO_KEEPALIVE) : -9;
        Check(kaOn == 1, "c · ④ 서버 쪽 소켓 SO_KEEPALIVE " + std::to_string(kaOn));
        const int kaIdle = (k1Server != INVALID_SOCKET) ? SockOptInt(k1Server, IPPROTO_TCP, TCP_KEEPIDLE) : -9;
        const int kaGap = (k1Server != INVALID_SOCKET) ? SockOptInt(k1Server, IPPROTO_TCP, TCP_KEEPINTVL) : -9;
        Check(kaIdle == 30 && kaGap == 5,
              "c · ④ 서버 쪽 소켓 값 — 쉬는 초 " + std::to_string(kaIdle) + " · 간격 초 " + std::to_string(kaGap) + " (기대 30 · 5)");

        // ── ⑤ 붙기 한도 ──
        uint16_t lport = 0;
        SOCKET l = RawListen(lport);
        const auto t0 = std::chrono::steady_clock::now();
        const int64_t out1 = server.ConnectTo("127.0.0.1", lport, TAG_OUT, 1000);
        const long long okMs = ElapsedMs(t0);
        SOCKET p1 = AcceptWithin(l, 2000);
        Check(out1 != 0 && p1 != INVALID_SOCKET && okMs < 1000, "c · ⑤ 듣는 주소 · 한도 1초 → 붙음 (" + std::to_string(okMs) + "ms)");
        Check(p1 != INVALID_SOCKET && SendAll(p1, Packet(77, "hello")), "c · ⑤ 상대가 보냄");
        TestHelp::WaitUntil([&] { return CountFrom(sink, out1) >= 1; }, 3000);
        Check(out1 != 0 && CountFrom(sink, out1) == 1, "c · ⑤ 한도 길로 붙은 세션이 받음");
        Check(server.GetKeepAliveOnCount() == 2, "c · ④ 건 접속은 안 켬(outboundToo 끔) — 켠 수 " + std::to_string(server.GetKeepAliveOnCount()));
        const SOCKET outServer = (p1 != INVALID_SOCKET) ? FindOwnSocket(PeerPort(p1), lport) : INVALID_SOCKET;
        const int outKa = (outServer != INVALID_SOCKET) ? SockOptInt(outServer, SOL_SOCKET, SO_KEEPALIVE) : -9;
        Check(outKa == 0, "c · ④ 건 접속 소켓 SO_KEEPALIVE " + std::to_string(outKa));

        // 답 없는 주소 — 문서용 주소 TEST-NET-1(RFC 5737). 아무도 답하지 않는 망을 전제로 한다(바로 거절이 오는 망이면 시간 칸이 깨진다)
        const auto t1 = std::chrono::steady_clock::now();
        const int64_t out2 = server.ConnectTo("192.0.2.1", 9, TAG_OUT, 300);
        const long long holeMs = ElapsedMs(t1);
        Check(out2 == 0 && holeMs >= 250 && holeMs < 1500, "c · ⑤ 답 없는 주소 · 한도 300ms → 0 (" + std::to_string(holeMs) + "ms)");

        // 닫힌 포트(묶기만 하고 안 들음) — 한도 안에 0
        uint16_t closedPort = 0;
        SOCKET bound = BindOnly(closedPort);
        const auto t2 = std::chrono::steady_clock::now();
        const int64_t out3 = server.ConnectTo("127.0.0.1", closedPort, TAG_OUT, 300);
        const long long refusedMs = ElapsedMs(t2);
        Check(bound != INVALID_SOCKET && out3 == 0 && refusedMs < 1500, "c · ⑤ 닫힌 포트 · 한도 300ms → 0 (" + std::to_string(refusedMs) + "ms)");

        // ── ⑦ 작은 링 ──
        // 받기 4,096 — 자르기 부품 최대(1,458) 메시지 둘을 한 번에 보내도 받는다
        Opened r1 = OpenAccepted(sink, port);
        const std::string big(LENGTH_FRAME_MAX_SIZE - sizeof(MsgHeader), 'r');
        Check(r1.info.sessionId != 0 && SendAll(r1.sock, Packet(78, big) + Packet(79, big)), "c · ⑦ 최대 메시지 둘(2,916 바이트)을 한 번에 보냄");
        TestHelp::WaitUntil([&] { return CountFrom(sink, r1.info.sessionId) >= 2; }, 3000);
        Check(CountFrom(sink, r1.info.sessionId) == 2, "c · ⑦ 받기 링 4,096 으로 둘 다 받음 (" + std::to_string(CountFrom(sink, r1.info.sessionId)) + ")");
        // 보내기 2,048 — 링은 2,047 까지 담는다. 1,408 바이트 하나는 들어가고 둘째는 넘친다(틱 끝 보내기라 링에 머문다)
        const std::string body(1404, 's');
        const bool first = SendMsg(server, r1.info.sessionId, body, SendFlush::Deferred);
        const bool second = SendMsg(server, r1.info.sessionId, body, SendFlush::Deferred);
#if USE_RIO_TRANSPORT
        // RIO 는 링 크기 설정을 안 쓴다 — 세션 버퍼는 슬랩 상수(65,536)가 정한다(`Transport_Rio.cpp` 첫 세션 알림). 그래서 둘 다 들어가고 안 끊긴다
        Check(first && second, std::string("c · ⑦ RIO — 보내기 링 설정 안 씀 · 1,408 바이트 둘 다 들어감 (둘째 ") + (second ? "들어감" : "넘침") + ")");
#else
        Check(first && !second, std::string("c · ⑦ 보내기 링 2,048 — 1,408 바이트 첫째 ") + (first ? "들어감" : "실패") + " · 둘째 " + (second ? "들어감" : "넘침"));
        TestHelp::WaitUntil([&] { SinkDisconnected x; return sink->FindDisconnect(r1.info.sessionId, x); }, 3000);
        SinkDisconnected d;
        const bool found = sink->FindDisconnect(r1.info.sessionId, d);
        Check(found && d.reason == DisconnectReason::SendOverflow,
              std::string("c · ⑦ 넘친 접속은 끊김 · 까닭 SendOverflow (") + (found ? ReasonName(d.reason) : "끊김 알림 없음") + ")");
#endif

        for (SOCKET s : { k1.sock, k2.sock, r1.sock, p1, l, bound })
            if (s != INVALID_SOCKET)
                Platform::CloseSocket(s);
        server.ShutdownServer();
    }

    // ── 판 d — 받기 링이 자르기 부품 최대를 못 담으면 Start 가 false ──
    void CaseD()
    {
        CMonitorManager monitor;
        CIOCPServer server(0, MAX_CLIENTS, "ConnExtrasTest", monitor, 2, 2, 2, 0, 1);
        Check(server.AddListener("127.0.0.1", 0, TAG_IN), "d · AddListener 127.0.0.1:0");
        Check(server.SetSessionBufferSizes(LENGTH_FRAME_MAX_SIZE, 65535), "d · ⑦ 받기 링 = 자르기 부품 최대(1,458)");
        const bool started = server.Start();
        Check(!started, "d · ⑦ Start false — 링은 크기 − 1 까지 담아 최대 메시지가 안 들어간다");
        if (started)
            server.ShutdownServer();
    }

    // ── 판 e — 큰 링 보낸 뒤 끊기 ──
    //   안 읽는 상대 몫으로 커널이 받아 두는 양은 윈도우 수십 KB · 리눅스 수 MB 다(루프백 실측) — 판 b 의 60,000 은 커널이 다 받아
    //   「다 빠질 때까지 기다림」을 빼도 안 걸린다. 3MB 를 넣어 보내기가 걸린 채로 부르게 한다
    void CaseE()
    {
        CMonitorManager monitor;
        auto sinkOwned = std::make_unique<CRecordingSink>();
        CRecordingSink* sink = sinkOwned.get();

        CIOCPServer server(0, MAX_CLIENTS, "ConnExtrasTest", monitor, 2, 2, 2, 0, 1);
        Check(server.AddListener("127.0.0.1", 0, TAG_IN), "e · AddListener 127.0.0.1:0");
        Check(server.SetIdleTimeoutSec(0), "e · 쉬는 접속 끊기 끔");
        Check(server.SetSessionBufferSizes(65535, 4 * 1024 * 1024), "e · ⑦ 보내기 링 4MB");
        Check(server.SetRecvSink(std::move(sinkOwned)), "e · SetRecvSink");
        if (!server.Start())
        {
            Check(false, "e · Start");
            return;
        }
        const int port = server.GetListenerPort(0);

        SOCKET c = ConnectSmallRecv(port, 4096);
        SinkConnected ci{};
        const uint16_t cport = (c != INVALID_SOCKET) ? LocalPort(c) : 0;
        TestHelp::WaitUntil([&] { return sink->FindByPeerPort(cport, ci); }, 3000);
        Check(c != INVALID_SOCKET && ci.sessionId != 0, "e · 접속(받기 버퍼 4,096)");
        std::string payload(3000000, '\0');
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>((i * 131 + (i >> 16)) & 0xFF);
#if USE_RIO_TRANSPORT
        // RIO 는 링 크기 설정을 안 쓴다(슬랩 65,536) — 3MB 는 못 담고, 그보다 작은 양은 커널이 다 받아 「다 빠질 때까지 기다림」을 못 건드린다.
        //   그래서 RIO 에서는 설정이 안 먹는 것만 본다. 다 빠질 때까지 기다림은 IOCP · 리눅스 판 e 가 본다
        Check(!server.RequestSendRaw(ci.sessionId, payload.data(), static_cast<int>(payload.size())),
              "e · RIO — 보내기 링 4MB 설정 안 씀 · 3,000,000 바이트는 슬랩(65,536)에 안 들어감");
        Platform::CloseSocket(c);
        server.ShutdownServer();
#else
        Check(server.RequestSendRaw(ci.sessionId, payload.data(), static_cast<int>(payload.size())), "e · ③ 링에 3,000,000 바이트 넣음");
        Check(server.RequestDisconnectAfterSend(ci.sessionId), "e · ③ 보낸 뒤 끊기 부름");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        int lastRc = 1;
        const std::string got = ReadToEnd(c, 20000, lastRc);
        Check(got.size() == payload.size() && got == payload,
              "e · ③ 끝까지 받음 — " + std::to_string(got.size()) + " / " + std::to_string(payload.size()) + " 바이트 · 내용 같음 " + (got == payload ? "예" : "아니오"));
        Check(lastRc == 0, std::string("e · ③ 끝이 FIN (") + (lastRc == 0 ? "FIN" : lastRc < 0 ? "실패" : "시한") + ")");
        TestHelp::WaitUntil([&] { return !sink->Disconnected().empty(); }, 3000);
        SinkDisconnected d;
        const bool found = sink->FindDisconnect(ci.sessionId, d);
        Check(found && d.reason == DisconnectReason::AfterSend,
              std::string("e · ③ 끊긴 까닭 AfterSend (") + (found ? ReasonName(d.reason) : "없음") + ")");
        Platform::CloseSocket(c);
        server.ShutdownServer();
#endif
    }
}

int main(int argc, char** argv)
{
    if (argc >= 2)
    {
        const std::string which = argv[1];
        if (which == "none")
            CaseNone();
        else if (which == "a")
            CaseA();
        else if (which == "b")
            CaseB();
        else if (which == "c")
            CaseC();
        else if (which == "d")
            CaseD();
        else if (which == "e")
            CaseE();
        else
        {
            std::printf("모르는 판 '%s' — none · a · b · c · d · e\n", which.c_str());
            return 2;
        }
        return TestHelp::Finish(("conn_extras_test " + which).c_str());
    }

    // 인자 없음 — 판마다 자기 자신을 따로 띄운다(서버는 프로세스에 하나)
    //   제 경로는 운영체제에서 읽는다 — argv[0] 은 부른 셸에 따라 「./이름」 꼴이라 cmd 가 못 띄운다
    std::string self = argv[0];
#ifdef _WIN32
    char selfBuf[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, selfBuf, MAX_PATH) > 0)
        self = selfBuf;
#else
    char selfBuf[4096] = {};
    const ssize_t selfLen = readlink("/proc/self/exe", selfBuf, sizeof(selfBuf) - 1);
    if (selfLen > 0)
        self.assign(selfBuf, static_cast<size_t>(selfLen));
#endif
    const char* cases[] = { "none", "a", "b", "c", "d", "e" };
    for (const char* c : cases)
    {
        const std::string cmd = std::string("\"") + self + "\" " + c;
        std::fflush(stdout);
        const int rc = std::system(cmd.c_str());
        Check(rc == 0, std::string("판 ") + c + " (종료 값 " + std::to_string(rc) + ")");
    }
    return TestHelp::Finish("conn_extras_test");
}
