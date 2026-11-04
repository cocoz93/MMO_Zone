// 공용 코드 시험 — 난수 · 패킷 표 · 시간 구간 · 응답 대기 · DB 줄기 · 로직 고리 · 지표 문
//   인자: [DB 접속 INI] — 주면 CDbPipeline 시험까지 돈다([DB] 섹션을 읽는다). 안 주면 그 부분은 안 돌렸다고 찍는다.
#include "IOCPServer.h"
#include "NetIoModel.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <Config/IniFile.h>
#include <Db/DbPipeline.h>
#include <Identity/SecureRandom.h>
#include <Link/LinkRequests.h>
#include <Link/SessionTag.h>
#include <Lobby/LobbyLoop.h>
#include <Lobby/PacketSend.h>
#include <Lobby/PacketTable.h>
#include <Monitoring/CoreMetrics.h>
#include <Monitoring/LatencyHistogram.h>
#include <Monitoring/MetricsEndpoint.h>
#include <Process/ShutdownWait.h>
#include <Protocol/ServerLinkProtocol.h>

#include "TestHelp.h"

#ifdef _WIN32
#include <mysql.h>
#else
#include <mysql/mysql.h>
#endif

using TestHelp::Check;
using TestHelp::WaitUntil;

namespace
{
    // 시험 패킷 번호대 — 쓰이지 않는 곳에서 고른 값
    constexpr uint16_t TEST_FIRST = 4000;
    constexpr uint16_t TEST_LAST = 4009;

#pragma pack(push, 1)
    struct PKT_A
    {
        static constexpr uint16_t TYPE = 4001;
        MsgHeader header;
        int32_t value;
    };
    struct PKT_B   // 표에 없는 번호
    {
        static constexpr uint16_t TYPE = 4002;
        MsgHeader header;
    };
    struct PKT_OUT   // 번호대 밖
    {
        static constexpr uint16_t TYPE = 4100;
        MsgHeader header;
    };
    struct PKT_SHORT   // PKT_A 번호인데 짧다
    {
        static constexpr uint16_t TYPE = 4001;
        MsgHeader header;
    };
#pragma pack(pop)

    CSerialBuffer* MakeBuffer(uint16_t type, uint16_t declaredSize, int actualBytes)
    {
        CSerialBuffer* msg = CSerialBuffer::Alloc();
        MsgHeader header;
        header.size = declaredSize;
        header.type = static_cast<MsgType>(type);
        std::memcpy(msg->GetWriteBufferPtr(), &header, sizeof(header));
        msg->MoveWritePos(static_cast<int>(sizeof(header)));
        for (int i = static_cast<int>(sizeof(header)); i < actualBytes; ++i)
        {
            *msg->GetWriteBufferPtr() = 0;
            msg->MoveWritePos(1);
        }
        return msg;
    }

    bool Contains(const std::string& text, const std::string& what)
    {
        return text.find(what) != std::string::npos;
    }

    void TestSecureRandom()
    {
        std::printf("-- SecureRandom\n");
        std::set<std::string> seen;
        bool allShapesValid = true;
        bool allOk = true;
        constexpr int DRAWS = 1000;
        for (int i = 0; i < DRAWS; ++i)
        {
            char code[SecureRandom::CODE_WIRE_BYTES];
            std::memset(code, 0x7F, sizeof(code));   // MakeCode 가 뒤를 비우는지 보려고 먼저 더럽힌다
            allOk = SecureRandom::MakeCode(code) && allOk;
            allShapesValid = allShapesValid && SecureRandom::ClassifyCode(code) == SecureRandom::CodeShape::Valid;
            seen.insert(std::string(code, SecureRandom::CODE_RANDOM_BYTES));
        }
        Check(allOk, "난수: 매번 성공");
        Check(allShapesValid, "난수: 앞 16바이트 값 · 뒤 48바이트 0");
        Check(static_cast<int>(seen.size()) == DRAWS, "난수: 1000번 뽑아 겹침 없음(값이 매번 다름)");

        char code[SecureRandom::CODE_WIRE_BYTES] = {};
        Check(SecureRandom::ClassifyCode(code) == SecureRandom::CodeShape::Empty, "칸 모양: 전부 0 = Empty");
        code[SecureRandom::CODE_RANDOM_BYTES - 1] = 1;
        Check(SecureRandom::ClassifyCode(code) == SecureRandom::CodeShape::Valid, "칸 모양: 16번째 바이트만 값 = Valid");
        code[SecureRandom::CODE_RANDOM_BYTES] = 1;
        Check(SecureRandom::ClassifyCode(code) == SecureRandom::CodeShape::BadSize, "칸 모양: 17번째 바이트 값 = BadSize");
        code[SecureRandom::CODE_RANDOM_BYTES - 1] = 0;
        Check(SecureRandom::ClassifyCode(code) == SecureRandom::CodeShape::BadSize, "칸 모양: 앞이 비고 뒤만 값 = BadSize");
    }

    void TestPacketTable()
    {
        std::printf("-- CPacketTable\n");
        CPacketTable table(TEST_FIRST, TEST_LAST);
        int called = 0;
        SessionId calledSession = NO_SESSION;
        auto handler = [&](SessionId id, CSerialBuffer*) { ++called; calledSession = id; };

        Check(table.Register<PKT_A>(handler), "등록: 번호대 안");
        Check(!table.Register<PKT_A>(handler), "등록: 같은 번호 두 번 거절");
        Check(!table.Register(TEST_FIRST - 1, 4, handler), "등록: 번호대 아래 거절");
        Check(!table.Register(TEST_LAST + 1, 4, handler), "등록: 번호대 위 거절");
        Check(table.Register(TEST_FIRST, 4, handler), "등록: 번호대 첫 번호");
        Check(table.Register(TEST_LAST, 4, handler), "등록: 번호대 끝 번호");
        Check(!table.Register(TEST_FIRST + 5, 4, CPacketTable::Handler{}), "등록: 빈 함수 거절");

        const uint16_t fullSize = static_cast<uint16_t>(sizeof(PKT_A));

        CSerialBuffer* ok = MakeBuffer(PKT_A::TYPE, fullSize, fullSize);
        Check(table.Route(SessionId{ 7 }, ok) == CPacketTable::Dispatch::Handled && called == 1 && calledSession == SessionId{ 7 }, "길: 맞는 패킷 처리 · 세션 번호 전달");
        ok->SubRef();

        CSerialBuffer* tooSmall = MakeBuffer(PKT_A::TYPE, static_cast<uint16_t>(fullSize - 1), fullSize - 1);
        Check(table.Route(SessionId{ 7 }, tooSmall) == CPacketTable::Dispatch::TooSmall && called == 1, "길: 최소 크기 한 칸 모자람 → TooSmall · 안 부름");
        tooSmall->SubRef();

        CSerialBuffer* unknown = MakeBuffer(PKT_B::TYPE, 4, 4);
        Check(table.Route(SessionId{ 7 }, unknown) == CPacketTable::Dispatch::UnknownType && called == 1, "길: 번호대 안의 빈 칸 → UnknownType");
        unknown->SubRef();

        CSerialBuffer* outside = MakeBuffer(PKT_OUT::TYPE, 4, 4);
        Check(table.Route(SessionId{ 7 }, outside) == CPacketTable::Dispatch::UnknownType && called == 1, "길: 번호대 밖 → UnknownType");
        outside->SubRef();

        CSerialBuffer* empty = CSerialBuffer::Alloc();
        Check(table.Route(SessionId{ 7 }, empty) == CPacketTable::Dispatch::NoHeader && called == 1, "길: 머리 없는 버퍼 → NoHeader");
        empty->SubRef();
    }

    void TestLatencyHistogram()
    {
        std::printf("-- CLatencyHistogram\n");
        CLatencyHistogram histogram;
        histogram.Record(1.0);      // 경계 값은 그 구간에 든다
        histogram.Record(1.5);
        histogram.Record(3000.0);
        histogram.Record(3000.5);   // 마지막 구간 밖

        std::ostringstream labeled;
        histogram.WriteSeries(labeled, "x_seconds", "op=\"a\"");
        const std::string text = labeled.str();
        Check(Contains(text, "x_seconds_bucket{op=\"a\",le=\"0.001\"} 1\n"), "구간: 1ms 는 첫 구간");
        Check(Contains(text, "x_seconds_bucket{op=\"a\",le=\"0.005\"} 2\n"), "구간: 누적 — 1.5ms 는 둘째까지");
        Check(Contains(text, "x_seconds_bucket{op=\"a\",le=\"3\"} 3\n"), "구간: 3000ms 는 마지막 경계 안");
        Check(Contains(text, "x_seconds_bucket{op=\"a\",le=\"+Inf\"} 4\n"), "구간: 경계 밖은 +Inf 에만");
        Check(Contains(text, "x_seconds_count{op=\"a\"} 4\n"), "횟수 = 기록 수");
        Check(Contains(text, "x_seconds_sum{op=\"a\"} 6.003000\n"), "합계(초)");
        Check(histogram.Count() == 4, "Count");

        std::ostringstream bare;
        CLatencyHistogram().WriteSeries(bare, "y_seconds", "");
        Check(Contains(bare.str(), "y_seconds_bucket{le=\"0.001\"} 0\n") && Contains(bare.str(), "y_seconds_count 0\n"), "라벨 없음 · 기록 없음도 0 으로 나옴");
    }

    void TestLinkRequests()
    {
        std::printf("-- CLinkRequests\n");
        CLinkRequests requests(3);
        constexpr int64_t T0 = 1000;

        // 응답 짝 맞추기
        int okCalls = 0;
        const void* gotReply = nullptr;
        const int payload = 42;
        const uint64_t first = requests.Begin(LinkRequestKind::TokenCheck, T0, [&](const void* reply) { ++okCalls; gotReply = reply; });
        const uint64_t second = requests.Begin(LinkRequestKind::TokenCheck, T0, [&](const void*) { ++okCalls; });
        Check(first != 0 && second != 0 && first != second, "요청 번호: 0 아님 · 겹치지 않음");
        Check(requests.Complete(first, T0 + 10, &payload) && okCalls == 1 && gotReply == &payload, "응답: 짝을 찾아 내용을 넘김");
        Check(!requests.Complete(first, T0 + 11, &payload) && okCalls == 1, "응답: 같은 번호 두 번째는 짝 없음");
        Check(!requests.Complete(999999, T0, &payload), "응답: 모르는 번호는 짝 없음");
        Check(requests.ReplyCount(LinkRequestKind::TokenCheck) == 1, "응답 시간 횟수 = 응답 받은 수");

        // 시한 — 경계 바로 앞은 안 끝난다
        Check(requests.ExpireDue(T0 + 2999) == 0 && okCalls == 1, "시한: 3초 전엔 안 끝남");
        Check(requests.ExpireDue(T0 + 3000) == 1 && okCalls == 2, "시한: 3초에 끝남 · 응답 온 것은 다시 안 끝남");
        Check(requests.TimeoutCount(LinkRequestKind::TokenCheck) == 1 && requests.TimeoutCount(LinkRequestKind::EnterNotice) == 0, "시한 지표: 그 종류만 +1");
        Check(!requests.Complete(second, T0 + 3001, &payload), "시한 뒤 늦게 온 응답은 짝 없음");
        Check(requests.ReplyCount(LinkRequestKind::TokenCheck) == 1, "시한 초과는 응답 시간에 안 들어감");

        // 끊김 — 그 종류만 즉시
        int tokenDown = 0;
        int enterDown = 0;
        const void* downReply = &payload;
        requests.Begin(LinkRequestKind::TokenCheck, T0, [&](const void* reply) { ++tokenDown; downReply = reply; });
        requests.Begin(LinkRequestKind::EnterNotice, T0, [&](const void*) { ++enterDown; });
        Check(requests.FailAll(LinkRequestKind::TokenCheck) == 1 && tokenDown == 1 && downReply == nullptr && enterDown == 0, "끊김: 그 종류만 nullptr 로 끝남");
        Check(requests.PendingCount() == 1, "끊김: 다른 종류는 남음");

        // 보내지 못함
        const uint64_t canceled = requests.Begin(LinkRequestKind::TokenCheck, T0, [&](const void*) { ++okCalls; });
        const int before = okCalls;
        requests.Cancel(canceled);
        Check(!requests.Complete(canceled, T0, &payload) && okCalls == before, "취소: 짝이 사라지고 함수도 안 부름");

        // 함수 안에서 새 요청을 시작해도 흔들리지 않는다
        int nested = 0;
        CLinkRequests reentrant(1);
        reentrant.Begin(LinkRequestKind::EnterNotice, T0, [&](const void*)
        {
            reentrant.Begin(LinkRequestKind::EnterNotice, T0, [&](const void*) { ++nested; });
        });
        reentrant.FailAll(LinkRequestKind::EnterNotice);
        Check(reentrant.PendingCount() == 1 && nested == 0, "함수 안에서 새 요청: 끊김 처리 중에 새로 건 것은 살아남음");

        // 지표 — 모든 갈래가 시작부터 0
        CLinkRequests fresh(3);
        std::ostringstream ss;
        fresh.WriteMetrics(ss);
        const std::string text = ss.str();
        Check(Contains(text, "mmo_link_timeout_total{request=\"token_check\"} 0") && Contains(text, "mmo_link_timeout_total{request=\"enter_notice\"} 0"), "지표: 시한 갈래가 0 으로 나옴");
        Check(Contains(text, "mmo_link_reply_seconds_count{request=\"token_check\"} 0") && Contains(text, "mmo_link_reply_seconds_count{request=\"enter_notice\"} 0"), "지표: 응답 시간 갈래가 0 으로 나옴");
    }

    // ── CDbPipeline ──
    struct SelectOne
    {
        bool ok = false;
        int value = 0;
    };

    SelectOne RunSelectOne(CDbConnection& conn)
    {
        SelectOne result;
        void* stmtPtr = conn.Prepare("SELECT 1");
        if (stmtPtr == nullptr)
            return result;
        MYSQL_STMT* st = static_cast<MYSQL_STMT*>(stmtPtr);
        MYSQL_BIND bind{};
        long long value = 0;
        bind.buffer_type = MYSQL_TYPE_LONGLONG;
        bind.buffer = &value;
        if (mysql_stmt_execute(st) != 0 || mysql_stmt_bind_result(st, &bind) != 0 || mysql_stmt_store_result(st) != 0)
        {
            conn.NoteStmtError(st, "SELECT 1");
            return result;
        }
        const int fetched = mysql_stmt_fetch(st);
        mysql_stmt_free_result(st);
        result.ok = fetched == 0;
        result.value = static_cast<int>(value);
        return result;
    }

    bool DrainUntil(CDbPipeline& pipeline, const std::function<bool()>& pred, int timeoutMs = 5000)
    {
        return WaitUntil([&] { pipeline.DrainReplies(); return pred(); }, timeoutMs);
    }

    void TestDbPipeline(const DBConfig& config)
    {
        std::printf("-- CDbPipeline\n");

        // 못 여는 설정은 Start 가 false
        {
            DBConfig bad = config;
            bad.password = "__wrong_password__";
            CDbPipeline pipeline(bad, 1, 2, { "q" });
            Check(!pipeline.Start(), "틀린 암호: Start 실패");
        }

        CDbPipeline pipeline(config, 2, 2, { "first", "second" });
        Check(pipeline.Start(), "Start");

        // 왕복 — 회신은 DrainReplies 를 부른 스레드에서만 돈다
        {
            std::atomic<int> replies{ 0 };
            int gotValue = 0;
            const std::thread::id logicThread = std::this_thread::get_id();
            bool sameThread = false;
            std::thread::id workThread;
            const bool submitted = pipeline.Run<SelectOne>(1, 0,
                [&workThread](CDbConnection& conn) { workThread = std::this_thread::get_id(); return RunSelectOne(conn); },
                [&](SelectOne& result)
                {
                    gotValue = result.value;
                    sameThread = std::this_thread::get_id() == logicThread;
                    ++replies;
                });
            Check(submitted, "왕복: 큐에 들어감");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            Check(replies == 0, "왕복: DrainReplies 전에는 회신이 안 돈다");
            Check(DrainUntil(pipeline, [&] { return replies == 1; }), "왕복: 회신 받음");
            Check(gotValue == 1 && sameThread && workThread != logicThread, "왕복: 값 전달 · 일은 워커 · 회신은 부른 스레드");
        }

        // 같은 열쇠는 같은 워커 — 차례가 그대로다
        {
            std::vector<int> order;
            constexpr int COUNT = 40;
            int submitted = 0;
            for (int i = 0; i < COUNT;)
            {
                const bool ok = pipeline.Run<int>(5, 1, [i](CDbConnection&) { return i; }, [&order](int& value) { order.push_back(value); });
                if (ok)
                {
                    ++i;
                    ++submitted;
                }
                else
                {
                    pipeline.DrainReplies();   // 큐 상한 2 — 비우며 넣는다
                }
            }
            DrainUntil(pipeline, [&] { return static_cast<int>(order.size()) == COUNT; });
            bool inOrder = static_cast<int>(order.size()) == COUNT;
            for (int i = 0; inOrder && i < COUNT; ++i)
                inOrder = order[i] == i;
            Check(inOrder, "같은 열쇠: 넣은 차례대로 돈다");
        }

        // 큐 상한 — 워커를 막아 두고 넘기면 거절
        {
            std::mutex gate;
            std::unique_lock<std::mutex> hold(gate);
            std::atomic<int> started{ 0 };
            std::atomic<int> replies{ 0 };
            auto blockedWork = [&](CDbConnection&) { ++started; std::lock_guard<std::mutex> lk(gate); return 0; };
            auto countReply = [&](int&) { ++replies; };

            Check(pipeline.Run<int>(2, 0, blockedWork, countReply), "큐 상한: 첫 작업(워커가 잡고 막힘)");
            WaitUntil([&] { return started == 1; });
            Check(pipeline.Run<int>(2, 0, blockedWork, countReply), "큐 상한: 대기 1");
            Check(pipeline.Run<int>(2, 0, blockedWork, countReply), "큐 상한: 대기 2 = 상한");
            Check(!pipeline.Run<int>(2, 0, blockedWork, countReply), "큐 상한: 넘으면 거절");

            std::ostringstream ss;
            pipeline.WriteMetrics(ss);
            Check(Contains(ss.str(), "mmo_db_queue_depth{dbworker=\"0\"} 2"), "지표: 대기 수가 보임");

            Check(pipeline.Run<int>(3, 0, [](CDbConnection&) { return 0; }, countReply), "큐 상한: 다른 워커는 안 막힘");

            hold.unlock();
            DrainUntil(pipeline, [&] { return replies == 4; });
            Check(replies == 4, "큐 상한: 풀리면 전부 회신");
        }

        // 연결이 끊기면 그 작업은 실패 · 다음 작업이 다시 연결
        {
            const int64_t failedBefore = pipeline.FailedCount();

            std::atomic<int> killDone{ 0 };
            pipeline.Run<int>(7, 0, [](CDbConnection& conn)
            {
                // 이 연결을 이 연결에서 끊는다 — 서버가 재시작한 것과 같은 꼴이다
                MYSQL* m = static_cast<MYSQL*>(conn.Handle());
                const std::string kill = "KILL " + std::to_string(mysql_thread_id(m));
                mysql_query(m, kill.c_str());
                return 0;
            }, [&](int&) { ++killDone; });
            DrainUntil(pipeline, [&] { return killDone == 1; });

            SelectOne lost;
            std::atomic<int> lostDone{ 0 };
            pipeline.Run<SelectOne>(7, 0, RunSelectOne, [&](SelectOne& result) { lost = result; ++lostDone; });
            DrainUntil(pipeline, [&] { return lostDone == 1; });
            Check(!lost.ok, "끊김: 끊긴 뒤 첫 작업은 실패");
            Check(pipeline.FailedCount() == failedBefore + 1, "끊김: 실패 지표 +1");

            SelectOne back;
            std::atomic<int> backDone{ 0 };
            pipeline.Run<SelectOne>(7, 0, RunSelectOne, [&](SelectOne& result) { back = result; ++backDone; });
            DrainUntil(pipeline, [&] { return backDone == 1; });
            Check(back.ok && back.value == 1, "끊김: 다음 작업은 다시 연결해 성공");
            Check(pipeline.FailedCount() == failedBefore + 1, "끊김: 성공은 실패 지표를 안 올림");
        }

        // 지표 — op 갈래가 시작부터 나오고 횟수가 맞다
        {
            CDbPipeline fresh(config, 1, 2, { "a", "b" });
            std::ostringstream ss;
            fresh.WriteMetrics(ss);
            Check(Contains(ss.str(), "mmo_db_query_seconds_count{op=\"a\"} 0") && Contains(ss.str(), "mmo_db_query_seconds_count{op=\"b\"} 0"), "지표: op 갈래가 0 으로 나옴");

            std::ostringstream used;
            pipeline.WriteMetrics(used);
            Check(Contains(used.str(), "mmo_db_query_seconds_count{op=\"second\"} 40"), "지표: op 횟수 = 돈 작업 수");
        }

        // Stop — 큐에 남은 작업도 돌고 내려간다
        {
            std::atomic<int> ran{ 0 };
            pipeline.Run<int>(9, 0, [&](CDbConnection&) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); ++ran; return 0; }, [](int&) {});
            pipeline.Run<int>(9, 0, [&](CDbConnection&) { ++ran; return 0; }, [](int&) {});
            pipeline.Stop();
            Check(ran == 2, "Stop: 남은 작업을 다 돌린 뒤 내려감");
            Check(!pipeline.Run<int>(9, 0, [](CDbConnection&) { return 0; }, [](int&) {}), "Stop 뒤 넣기는 거절");
        }
    }

    // ── CLobbyLoop + 코어 + 지표 문 ──
    void TestLobbyLoop()
    {
        std::printf("-- CLobbyLoop · CMetricsEndpoint\n");

        constexpr int PORT_WEB = 16400;
        constexpr int PORT_LINK = 16401;
        constexpr int PORT_METRICS = 16402;

        CMonitorManager monitor;
        CIOCPServer network(PORT_WEB, 16, ServerMode::GameServer, monitor, 2, 2, 2, 0, 1);
        Check(network.AddListener("127.0.0.1", PORT_LINK, static_cast<int>(SessionTag::LinkDown)), "서버 간 포트 추가");
        network.SetUntaggedSessionLimit(8);
        Check(network.Start(), "코어 Start");

        TestPacketTable();   // 패킷 버퍼 풀은 코어 Start 가 만든다 — 그 뒤에 돌린다

        CPacketTable webTable(TEST_FIRST, TEST_LAST);
        CPacketTable linkTable(LINK_PACKET_FIRST, LINK_PACKET_LAST);

        std::atomic<int> webPackets{ 0 };
        std::atomic<int> webValue{ 0 };
        std::atomic<int> linkPackets{ 0 };
        std::atomic<int> webConnected{ 0 };
        std::atomic<int> webDisconnected{ 0 };
        std::atomic<int> linkConnected{ 0 };
        std::atomic<int> periodic{ 0 };

        webTable.Register<PKT_A>([&](SessionId id, CSerialBuffer* msg)
        {
            PKT_A packet{};
            PacketSend::Read(msg, packet);
            webValue = packet.value;
            ++webPackets;

            PKT_A reply{};
            reply.value = packet.value + 1;
            PacketSend::Send(network, monitor, id, reply);
        });
        linkTable.Register<LINK_HEARTBEAT>([&](SessionId, CSerialBuffer*) { ++linkPackets; });

        CLobbyLoop loop(network, monitor, 2);
        loop.SetRoute(SessionTag::Web, &webTable,
            [&](SessionId) { ++webConnected; },
            [&](SessionId) { ++webDisconnected; });
        loop.SetRoute(SessionTag::LinkDown, &linkTable,
            [&](SessionId) { ++linkConnected; },
            [&](SessionId) {});
        loop.SetPeriodic([&](int64_t) { ++periodic; });
        loop.Start();

        CCoreMetrics coreMetrics(monitor);
        CMetricsEndpoint endpoint(PORT_METRICS, [&]
        {
            std::ostringstream ss;
            coreMetrics.Write(ss);
            return ss.str();
        });
        endpoint.Start();

        Check(WaitUntil([&] { return periodic > 3; }), "주기 작업이 돈다");

        SOCKET web = TestHelp::ConnectRaw("127.0.0.1", PORT_WEB);
        Check(web != INVALID_SOCKET, "웹 접속");
        Check(WaitUntil([&] { return webConnected == 1; }), "웹 CONNECTED → 웹 표의 함수");

        // 맞는 패킷 왕복
        PKT_A packet{};
        packet.header.size = sizeof(PKT_A);
        packet.header.type = static_cast<MsgType>(PKT_A::TYPE);
        packet.value = 41;
        ::send(web, reinterpret_cast<const char*>(&packet), sizeof(packet), 0);
        Check(WaitUntil([&] { return webPackets == 1; }), "웹 패킷 처리");
        Check(webValue == 41, "웹 패킷 내용 전달");

        PKT_A answer{};
        const int got = static_cast<int>(::recv(web, reinterpret_cast<char*>(&answer), sizeof(answer), 0));
        Check(got == sizeof(PKT_A) && answer.value == 42 && static_cast<uint16_t>(answer.header.type) == PKT_A::TYPE, "응답 왕복 — 머리·값");

        // 없는 번호·크기 미달은 오류로 세고 버린다
        const int64_t errorsBefore = monitor._packetErrors;
        PKT_B unknown{};
        unknown.header.size = sizeof(PKT_B);
        unknown.header.type = static_cast<MsgType>(PKT_B::TYPE);
        ::send(web, reinterpret_cast<const char*>(&unknown), sizeof(unknown), 0);
        Check(WaitUntil([&] { return monitor._packetErrors == errorsBefore + 1; }), "없는 번호 → 오류 +1");

        PKT_SHORT shortPacket{};
        shortPacket.header.size = sizeof(PKT_SHORT);
        shortPacket.header.type = static_cast<MsgType>(PKT_SHORT::TYPE);
        ::send(web, reinterpret_cast<const char*>(&shortPacket), sizeof(shortPacket), 0);
        Check(WaitUntil([&] { return monitor._packetErrors == errorsBefore + 2; }) && webPackets == 1, "크기 미달 → 오류 +1 · 처리 함수 안 부름");

        // 처리표는 구분 값으로 고른다 — 웹 포트로 온 서버 간 패킷은 처리 안 됨
        LINK_HEARTBEAT heartbeat{};
        heartbeat.header.size = sizeof(LINK_HEARTBEAT);
        heartbeat.header.type = static_cast<MsgType>(LINK_HEARTBEAT::TYPE);
        ::send(web, reinterpret_cast<const char*>(&heartbeat), sizeof(heartbeat), 0);
        Check(WaitUntil([&] { return monitor._packetErrors == errorsBefore + 3; }) && linkPackets == 0, "웹 포트로 온 서버 간 패킷 → 처리 안 됨 · 오류 +1");

        // 반대 방향 — 서버 간 포트로 온 것은 처리됨
        SOCKET link = TestHelp::ConnectRaw("127.0.0.1", PORT_LINK);
        Check(link != INVALID_SOCKET, "서버 간 포트 접속");
        Check(WaitUntil([&] { return linkConnected == 1; }), "서버 간 CONNECTED → 서버 간 표의 함수");
        ::send(link, reinterpret_cast<const char*>(&heartbeat), sizeof(heartbeat), 0);
        Check(WaitUntil([&] { return linkPackets == 1; }), "서버 간 포트로 온 패킷 → 처리됨(반대 방향)");
        Check(monitor._packetErrors == errorsBefore + 3, "서버 간 패킷은 오류를 안 올림");

        // 서버 간 포트에 웹 패킷 → 처리 안 됨
        ::send(link, reinterpret_cast<const char*>(&packet), sizeof(packet), 0);
        Check(WaitUntil([&] { return monitor._packetErrors == errorsBefore + 4; }) && webPackets == 1, "서버 간 포트로 온 웹 패킷 → 처리 안 됨");

        // 지표 문
        std::string metrics;
        Check(WaitUntil([&]
        {
            metrics = TestHelp::HttpGet("127.0.0.1", PORT_METRICS, "/metrics");
            return Contains(metrics, "mmo_session_count 1\n");
        }), "/metrics: 접속자 1(서버 간 연결은 안 셈)");
        Check(Contains(metrics, "mmo_link_session_count 1\n"), "/metrics: 서버 간 연결 1");
        Check(Contains(metrics, "mmo_thread_cpu_ratio{thread=\"gameloop\"}"), "/metrics: 로직 스레드 CPU 라벨이 gameloop");
        Check(Contains(metrics, "mmo_handle_latency_seconds_count "), "/metrics: 처리 지연");
        Check(!Contains(metrics, "mmo_tick_duration_seconds"), "/metrics: 게임 지표는 없음");

        // 처리 지연이 쌓이는지 — 받은 패킷 수만큼
        Check(WaitUntil([&] { return monitor._gameLoop._handleCount >= 6; }), "처리 지연 횟수 = 받은 패킷 수");

        // 끊김
        Platform::CloseSocket(web);
        Check(WaitUntil([&] { return webDisconnected == 1; }), "웹 DISCONNECTED → 웹 표의 함수");
        Platform::CloseSocket(link);

        endpoint.Stop();
        loop.Stop();
        network.ShutdownServer();
        Check(monitor._sessionCount == 0 && monitor._taggedSessionCount == 0, "종료 뒤 세션 수 0");
    }

    bool LoadDbConfig(const char* path, DBConfig& out)
    {
        CIniFile ini;
        if (!ini.Load(path))
            return false;
        out.host = ini.GetString("DB", "Host", "127.0.0.1");
        out.port = ini.GetInt("DB", "Port", 3306);
        out.user = ini.GetString("DB", "User", "");
        out.password = ini.GetString("DB", "Password", "");
        out.database = ini.GetString("DB", "Database", "");
        out.connectTimeoutSec = 3;
        out.rwTimeoutSec = 5;
        return !out.user.empty();
    }
}

int main(int argc, char** argv)
{
    CShutdownWait::Install();   // 코어의 종료 알림 정의(SignalProcessShutdown)가 이 파일과 같이 이어진다

    TestSecureRandom();
    TestLatencyHistogram();
    TestLinkRequests();

    if (argc >= 2)
    {
        DBConfig config;
        if (LoadDbConfig(argv[1], config))
            TestDbPipeline(config);
        else
            Check(false, std::string("DB 설정을 못 읽음: ") + argv[1]);
    }
    else
    {
        std::printf("-- CDbPipeline 안 돌림 (DB 접속 INI 인자 없음)\n");
    }

    TestLobbyLoop();

    return TestHelp::Finish("server_common_test");
}
