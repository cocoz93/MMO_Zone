// 서버 간 연결 시험 — CLinkConnector · CLinkPeers · CLinkRequests 를 코어 위에서 돌린다
//   CIOCPServer 는 프로세스에 하나라서 한 서버가 자기 서버 간 포트에 접속한다(아래층 역할 = LinkUp · 위층 역할 = LinkDown).
//   인자 없음 = 빠른 갈래 / long = 하트비트로 60초 넘게 유지 + 하트비트가 없으면 끊기는 대조군까지(약 2분 40초)
#include "IOCPServer.h"
#include "NetIoModel.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <Link/LinkConnector.h>
#include <Link/LinkPeers.h>
#include <Link/LinkRequests.h>
#include <Link/SessionTag.h>
#include <Lobby/LobbyLoop.h>
#include <Lobby/PacketSend.h>
#include <Lobby/PacketTable.h>
#include <Process/ShutdownWait.h>
#include <Protocol/ServerLinkProtocol.h>

#include "TestHelp.h"

using TestHelp::Check;
using TestHelp::WaitUntil;

namespace
{
    constexpr int PORT_WEB = 16500;
    constexpr int PORT_LINK = 16501;
    constexpr int PORT_CLOSED = 16502;      // 아무도 안 여는 포트
    constexpr int PORT_LATE = 16503;        // 시험 중간에 여는 포트
    constexpr int NO_HEARTBEAT_SEC = 3600;  // 시험 시간 안에 한 번도 안 나가는 주기

    struct Harness
    {
        CMonitorManager monitor;
        std::unique_ptr<CIOCPServer> network;
        std::unique_ptr<CLobbyLoop> loop;
        std::unique_ptr<CLinkConnector> connector;
        std::unique_ptr<CLinkPeers> peers;
        std::unique_ptr<CLinkRequests> requests;

        CPacketTable upTable{ LINK_PACKET_FIRST, LINK_PACKET_LAST };     // 아래층 쪽이 받는 표
        CPacketTable downTable{ LINK_PACKET_FIRST, LINK_PACKET_LAST };   // 위층 쪽이 받는 표

        std::atomic<int> upHeartbeats{ 0 };
        std::atomic<int> downHeartbeats{ 0 };
        std::atomic<int> registered{ 0 };          // 연결될 때 부르는 등록 함수
        std::atomic<int> upDown{ 0 };              // 아래층이 본 끊김
        std::atomic<int> peerDown{ 0 };            // 위층이 본 끊김
        std::atomic<int64_t> lastPeerSession{ 0 };
        std::atomic<bool> answerTokens{ true };

        std::mutex logicMutex;
        std::vector<std::function<void()>> logicWork;

        // 로직 스레드 전용 함수를 시험 스레드에서 부른다 — 로직 스레드가 돌려 주고 끝날 때까지 막힌다
        template <typename Fn>
        auto OnLogic(Fn fn) -> decltype(fn())
        {
            std::packaged_task<decltype(fn())()> task(std::move(fn));
            auto result = task.get_future();
            {
                std::lock_guard<std::mutex> lk(logicMutex);
                logicWork.push_back([&task] { task(); });
            }
            return result.get();
        }

        void DrainLogicWork()
        {
            std::vector<std::function<void()>> work;
            {
                std::lock_guard<std::mutex> lk(logicMutex);
                work.swap(logicWork);
            }
            for (auto& fn : work)
                fn();
        }

        bool BothUp()
        {
            return OnLogic([this] { return connector->IsUp() && peers->Count() == 1; });
        }

        bool IsUp()
        {
            return OnLogic([this] { return connector->IsUp(); });
        }

        int64_t UpSession()
        {
            return OnLogic([this] { return ToInt(connector->Session()); });
        }

        // 토큰 확인 한 번 — 끝나면 reply 를 인자로 done 을 부른다
        void SendTokenCheck(std::function<void(const void*)> done)
        {
            OnLogic([this, &done]
            {
                const uint64_t id = requests->Begin(LinkRequestKind::TokenCheck, CLobbyLoop::NowMs(), std::move(done));
                LINK_TOKEN_CHECK_REQ req{};
                req.requestId = id;
                connector->Send(req);
            });
        }
    };

    void Build(Harness& h, int connectPort, int heartbeatSec, int retrySec)
    {
        h.network = std::make_unique<CIOCPServer>(PORT_WEB, 16, ServerMode::GameServer, h.monitor, 2, 2, 2, 0, 1);
        h.network->AddListener("127.0.0.1", PORT_LINK, static_cast<int>(SessionTag::LinkDown));
        h.network->SetUntaggedSessionLimit(8);

        CLinkConnector::Config config;
        config.peerName = "character";
        config.ip = "127.0.0.1";
        config.port = connectPort;
        config.retrySec = retrySec;
        config.heartbeatSec = heartbeatSec;
        h.connector = std::make_unique<CLinkConnector>(*h.network, h.monitor, config);
        h.peers = std::make_unique<CLinkPeers>(*h.network, h.monitor, heartbeatSec);
        h.requests = std::make_unique<CLinkRequests>(3);
        h.loop = std::make_unique<CLobbyLoop>(*h.network, h.monitor, 2);

        h.connector->SetHandlers([&h](SessionId) { ++h.registered; }, [&h]() { ++h.upDown; });
        h.peers->SetHandlers([&h](SessionId id) { h.lastPeerSession = ToInt(id); }, [&h](SessionId) { ++h.peerDown; });

        h.upTable.Register<LINK_HEARTBEAT>([&h](SessionId, CSerialBuffer*) { ++h.upHeartbeats; });
        h.downTable.Register<LINK_HEARTBEAT>([&h](SessionId, CSerialBuffer*) { ++h.downHeartbeats; });

        // 위층: 토큰 확인을 받아 답한다
        h.downTable.Register<LINK_TOKEN_CHECK_REQ>([&h](SessionId id, CSerialBuffer* msg)
        {
            if (!h.answerTokens)
                return;
            LINK_TOKEN_CHECK_REQ req{};
            PacketSend::Read(msg, req);
            LINK_TOKEN_CHECK_ACK ack{};
            ack.requestId = req.requestId;
            ack.result = static_cast<uint8_t>(TokenCheckResult::Ok);
            ack.accountId = 777;
            h.peers->Send(id, ack);
        });

        // 아래층: 답을 받아 대기와 짝을 맞춘다
        h.upTable.Register<LINK_TOKEN_CHECK_ACK>([&h](SessionId, CSerialBuffer* msg)
        {
            LINK_TOKEN_CHECK_ACK ack{};
            PacketSend::Read(msg, ack);
            h.requests->Complete(ack.requestId, CLobbyLoop::NowMs(), &ack);
        });

        h.loop->SetRoute(SessionTag::LinkUp, &h.upTable,
            [&h](SessionId id) { h.connector->OnConnected(id); },
            [&h](SessionId id) { h.connector->OnDisconnected(id); h.requests->FailAll(LinkRequestKind::TokenCheck); });
        h.loop->SetRoute(SessionTag::LinkDown, &h.downTable,
            [&h](SessionId id) { h.peers->OnConnected(id); },
            [&h](SessionId id) { h.peers->OnDisconnected(id); });
        h.loop->SetPeriodic([&h](int64_t nowMs)
        {
            h.DrainLogicWork();
            h.connector->Tick(nowMs);
            h.peers->Tick(nowMs);
            h.requests->ExpireDue(nowMs);
        });
    }

    void Begin(Harness& h)
    {
        Check(h.network->Start(), "코어 Start");
        h.loop->Start();
        h.connector->Start();
    }

    void Shutdown(Harness& h)
    {
        h.connector->Stop();   // 코어 약속 — ConnectTo 를 부르는 스레드를 먼저 멈춘다
        h.loop->Stop();
        h.network->ShutdownServer();
    }

    void TestFastPaths()
    {
        std::printf("-- 연결·재접속·요청\n");
        Harness h;
        Build(h, PORT_LINK, NO_HEARTBEAT_SEC, 1);
        Begin(h);

        Check(WaitUntil([&] { return h.BothUp(); }), "연결: 아래층·위층 둘 다 봄");
        Check(h.registered == 1, "연결: 등록 함수가 한 번 불림");
        Check(h.connector->ReconnectCount() == 0, "연결: 첫 연결은 재접속으로 안 셈");
        Check(h.monitor._taggedSessionCount == 2 && h.monitor._sessionCount == 0, "세션 수: 서버 간 2 · 접속자 0");

        std::ostringstream up;
        h.connector->WriteMetrics(up);
        Check(up.str().find("mmo_link_up{peer=\"character\"} 1") != std::string::npos, "지표: mmo_link_up 1");

        // 요청 왕복 — 번호 짝 맞추기
        std::atomic<int64_t> gotAccount{ 0 };
        std::atomic<int> done{ 0 };
        h.SendTokenCheck([&](const void* reply)
        {
            if (reply != nullptr)
                gotAccount = static_cast<const LINK_TOKEN_CHECK_ACK*>(reply)->accountId;
            ++done;
        });
        Check(WaitUntil([&] { return done == 1; }) && gotAccount == 777, "요청: 위층이 답하고 짝을 찾음");
        Check(h.requests->ReplyCount(LinkRequestKind::TokenCheck) == 1, "요청: 응답 시간 횟수 +1");

        // 위층이 답을 안 하면 시한이 끝낸다 — 3초
        h.answerTokens = false;
        std::atomic<int> timedOut{ 0 };
        h.SendTokenCheck([&](const void* reply)
        {
            if (reply == nullptr)
                ++timedOut;
        });
        Check(!WaitUntil([&] { return timedOut == 1; }, 2500), "시한: 3초 전엔 안 끝남");
        Check(WaitUntil([&] { return timedOut == 1; }, 1500) && h.requests->TimeoutCount(LinkRequestKind::TokenCheck) == 1, "시한: 3초 뒤 끝남 · 지표 +1");
        h.answerTokens = true;

        // 위층이 끊는다 → 아래층이 재접속
        const int64_t firstUp = h.UpSession();
        h.network->RequestDisconnectSession(h.lastPeerSession.load());
        Check(WaitUntil([&] { return h.upDown == 1 && h.peerDown == 1; }), "위층이 끊음: 양쪽이 끊김을 봄");
        Check(WaitUntil([&] { return h.BothUp(); }), "위층이 끊음: 재접속으로 복구");
        Check(h.UpSession() != firstUp, "재접속: 새 세션 번호");
        Check(h.connector->ReconnectCount() == 1, "재접속: 횟수 +1");
        Check(h.registered == 2, "재접속: 등록 함수가 다시 불림");

        // 아래층이 끊는다 → 재접속
        h.network->RequestDisconnectSession(h.UpSession());
        Check(WaitUntil([&] { return h.upDown == 2; }), "아래층이 끊음: 끊김을 봄");
        Check(WaitUntil([&] { return h.BothUp() && h.registered == 3; }), "아래층이 끊음: 재접속으로 복구");
        Check(h.connector->ReconnectCount() == 2, "재접속: 횟수 +1 (2)");
        Check(h.monitor._taggedSessionCount == 2 && h.monitor._sessionCount == 0, "재접속 뒤 세션 수가 그대로(음수·누적 없음)");

        // 대기 중 끊기면 시한까지 안 기다린다
        std::atomic<int> failedFast{ 0 };
        h.answerTokens = false;
        h.SendTokenCheck([&](const void* reply)
        {
            if (reply == nullptr)
                ++failedFast;
        });
        const int64_t timeoutsBefore = h.requests->TimeoutCount(LinkRequestKind::TokenCheck);
        h.network->RequestDisconnectSession(h.UpSession());
        Check(WaitUntil([&] { return failedFast == 1; }, 1000), "끊김: 대기 중 요청이 바로 끝남(시한 안 기다림)");
        Check(h.requests->TimeoutCount(LinkRequestKind::TokenCheck) == timeoutsBefore, "끊김: 시한 초과로 안 셈");

        Shutdown(h);
        Check(h.monitor._taggedSessionCount == 0, "종료 뒤 서버 간 세션 0");
    }

    void TestUpperAbsent()
    {
        std::printf("-- 위층이 없을 때\n");
        Harness h;
        Build(h, PORT_CLOSED, NO_HEARTBEAT_SEC, 1);
        Begin(h);
        std::this_thread::sleep_for(std::chrono::seconds(4));   // 재시도 몇 번이 돌 시간
        Check(!h.IsUp() && h.registered == 0, "위층이 없으면 연결 안 됨 · 등록 안 불림");
        Check(h.monitor._taggedSessionCount == 0, "실패한 접속은 세션을 안 남김");
        std::ostringstream ss;
        h.connector->WriteMetrics(ss);
        Check(ss.str().find("mmo_link_up{peer=\"character\"} 0") != std::string::npos, "지표: mmo_link_up 0");
        Check(ss.str().find("mmo_link_reconnect_total{peer=\"character\"} 0") != std::string::npos, "지표: 재접속 갈래가 0 으로 나옴");
        Shutdown(h);
        Check(true, "위층이 없어도 Stop 이 끝남");
    }

    // 접속 실패 뒤에도 계속 다시 시도해, 위층이 뜨면 붙는다 — 실패 갈래가 재접속 필요 표시를 되돌리는지 본다
    void TestUpperLate()
    {
        std::printf("-- 위층이 늦게 뜰 때\n");
        Harness h;
        Build(h, PORT_LATE, NO_HEARTBEAT_SEC, 1);
        Begin(h);
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));   // 재시도 몇 번이 실패할 시간
        Check(!h.IsUp() && h.registered == 0, "위층이 뜨기 전: 연결 안 됨");

        // 포트만 여는 위층 — accept 를 안 해도 대기열에 들어가면 connect 는 성공한다
        SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(PORT_LATE));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        const bool listening = ::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && ::listen(listener, 8) == 0;
        Check(listening, "늦게 뜬 위층이 포트를 엶");

        Check(WaitUntil([&] { return h.IsUp(); }), "위층이 뜨면 다시 시도해 붙음");
        Check(h.registered == 1, "등록 함수가 한 번 불림");
        Check(h.connector->ReconnectCount() == 0, "첫 연결이라 재접속으로 안 셈");
        Shutdown(h);
        Platform::CloseSocket(listener);
    }

    void TestKeepAlive(int heartbeatSec, int holdSec, bool expectAlive)
    {
        std::printf("-- 하트비트 %d초 · %d초 유지 → %s\n", heartbeatSec, holdSec, expectAlive ? "끊기지 않음" : "끊김(대조군)");
        Harness h;
        Build(h, PORT_LINK, heartbeatSec, 1);
        Begin(h);
        Check(WaitUntil([&] { return h.BothUp(); }), "연결");
        const int64_t first = h.UpSession();

        std::this_thread::sleep_for(std::chrono::seconds(holdSec));

        if (expectAlive)
        {
            Check(h.upDown == 0 && h.peerDown == 0, "끊김 0");
            Check(h.UpSession() == first, "세션 번호 그대로");
            Check(h.monitor._sessionTimedOut == 0, "세션 시한 초과 0");
            const int expected = holdSec / heartbeatSec;
            Check(h.upHeartbeats >= expected && h.downHeartbeats >= expected,
                  "양쪽이 하트비트를 받음 (" + std::to_string(h.upHeartbeats.load()) + "·" + std::to_string(h.downHeartbeats.load()) + ")");
        }
        else
        {
            Check(h.monitor._sessionTimedOut > 0, "세션 시한 초과 발생 — 하트비트가 없으면 끊긴다");
            Check(h.upDown > 0, "아래층이 끊김을 봄");
        }
        Shutdown(h);
    }
}

int main(int argc, char** argv)
{
    CShutdownWait::Install();   // 코어의 종료 알림 정의(SignalProcessShutdown)가 이 파일과 같이 이어진다

    const bool longRun = argc >= 2 && std::strcmp(argv[1], "long") == 0;
    TestFastPaths();
    TestUpperAbsent();
    TestUpperLate();
    if (longRun)
    {
        TestKeepAlive(20, 70, true);                  // 플랜 값 — HeartbeatSec=20 · 코어 세션 시한 60초
        TestKeepAlive(NO_HEARTBEAT_SEC, 75, false);   // 대조군 — 하트비트가 없으면 끊긴다
    }
    else
    {
        std::printf("-- 60초 유지는 안 돌림 (인자 long)\n");
    }

    return TestHelp::Finish("server_link_test");
}
