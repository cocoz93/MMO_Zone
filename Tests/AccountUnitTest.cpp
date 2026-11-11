// 계정 서버 부품 시험 — 규칙 · 장부는 서버 없이 돌린다. 시각은 시험이 넘긴다(실제로 2분을 안 기다린다)
//   인자 <ini> <db> 를 주면 시험 DB 로 같은 새 이름 경쟁까지 본다. 실제 accountdb 에 돌리지 말 것 — 행을 못 지운다
#include <Identity/SecureRandom.h>
#include <Config/DbPassword.h>
#include <Config/IniFile.h>
#include <Db/DbConnection.h>
#include <atomic>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "AccountDb.h"
#include "AccountRules.h"
#include "AuthTokenLedger.h"
#include "TestHelp.h"

using TestHelp::Check;

namespace
{
    constexpr int TTL_SEC = 120;
    constexpr int64_t TTL_MS = int64_t{ TTL_SEC } * 1000;
    constexpr int64_t KEEP_MS = int64_t{ CAuthTokenLedger::EXPIRED_KEEP_SEC } * 1000;
    const AccountId ACC_A{ 11 };
    const AccountId ACC_B{ 22 };

    bool NameOk(const std::string& text)
    {
        char field[ACCOUNT_LOGIN_NAME_BYTES]{};
        std::memcpy(field, text.data(), text.size() < sizeof(field) ? text.size() : sizeof(field));
        return CAccountRules::IsValidLoginName(field);
    }

    void TestNameRules()
    {
        std::printf("-- 이름 형식\n");
        Check(NameOk("abcd"), "4자 통과");
        Check(NameOk("abcdefghijklmnop"), "16자 통과");
        Check(!NameOk(""), "빈 칸 거절");
        Check(!NameOk("abc"), "3자 거절");
        Check(!NameOk("abcdefghijklmnopq"), "칸을 다 채움(NUL 없음) 거절");
        Check(NameOk("ABCD") && NameOk("Ab12") && NameOk("0009"), "대문자 · 숫자 통과");

        // 영문·숫자 앞뒤 경계 글자 — 범위 비교가 한 칸 어긋나면 여기서 걸린다
        for (const char c : { '/', ':', '@', '[', '`', '{' })
            Check(!NameOk(std::string("abc") + c), std::string("경계 글자 거절 ") + c);
        for (const char c : { '0', '9', 'A', 'Z', 'a', 'z' })
            Check(NameOk(std::string("abc") + c), std::string("경계 글자 통과 ") + c);

        Check(!NameOk("ab cd"), "공백 거절");
        Check(!NameOk("abc\xEA\xB0\x80"), "한글 바이트 거절(음수 char)");
        Check(!NameOk("ab'--"), "따옴표 거절");

        char tail[ACCOUNT_LOGIN_NAME_BYTES]{};
        std::memcpy(tail, "abcd", 4);
        tail[5] = '!';
        Check(CAccountRules::IsValidLoginName(tail), "NUL 뒤 바이트는 안 봄");

        Check(CAccountRules::LowerName("AbZ9z") == "abz9z", "소문자로 맞춤");
    }

    void TestNames()
    {
        std::printf("-- 지표 라벨 이름\n");
        std::set<std::string> names;
        for (int i = 0; i < static_cast<int>(AccountLoginResult::COUNT); ++i)
            names.insert(CAccountRules::LoginResultName(static_cast<AccountLoginResult>(i)));
        Check(names.size() == 5 && names.count("unknown") == 0, "로그인 결과 5갈래 이름이 전부 다름");

        std::set<std::string> tokens;
        for (int i = 0; i < TOKEN_CHECK_RESULT_COUNT; ++i)
            tokens.insert(CAccountRules::TokenResultName(static_cast<TokenCheckResult>(i)));
        Check(tokens.size() == 4 && tokens.count("unknown") == 0, "토큰 결과 4갈래 이름이 전부 다름");

        // 계정 우편 지표는 종류 × 결과 48칸이다 — 이름이 겹치면 두 칸이 한 줄로 합쳐져 수가 틀린다
        std::set<std::string> mailOps;
        for (int i = 0; i < ACC_MAIL_OP_COUNT; ++i)
            mailOps.insert(CAccountRules::MailOpName(static_cast<AccMailOp>(i)));
        Check(mailOps.size() == 8 && mailOps.count("unknown") == 0, "계정 우편 요청 8종류 이름이 전부 다름");

        std::set<std::string> mailResults;
        for (int i = 0; i < LINK_MAIL_RESULT_COUNT; ++i)
            mailResults.insert(CAccountRules::MailResultName(static_cast<LinkMailResult>(i)));
        Check(mailResults.size() == 7 && mailResults.count("unknown") == 0, "계정 우편 결과 7갈래(월드 서버만 만드는 offline 포함) 이름이 전부 다름");

        std::set<std::string> dbOps;
        for (int i = 0; i < CAccountDb::OP_COUNT; ++i)
            dbOps.insert(CAccountDb::OpName(static_cast<CAccountDb::Op>(i)));
        Check(dbOps.size() == 10 && dbOps.count("unknown") == 0, "DB 문장 10개(로그인 + 우편 아홉) 이름이 전부 다름");
    }

    void TestLedgerConsume()
    {
        std::printf("-- 장부: 일회용\n");
        CAuthTokenLedger ledger(TTL_SEC);
        char token[CAuthTokenLedger::TOKEN_BYTES];
        ledger.Issue(ACC_A, 1000, token);
        Check(ledger.LiveCount() == 1 && ledger.StoredCount() == 1, "발급 뒤 1개");

        AccountId out = NO_ACCOUNT;
        Check(ledger.Consume(token, 1000 + 1, out) == TokenCheckResult::Ok, "첫 확인 Ok");
        Check(out == ACC_A, "계정 번호가 맞음");
        Check(ledger.LiveCount() == 0 && ledger.StoredCount() == 0, "장부에서 지움");

        out = ACC_B;
        Check(ledger.Consume(token, 1000 + 2, out) == TokenCheckResult::NotFound, "같은 토큰 두 번 → NotFound");
        Check(out == ACC_B, "실패하면 번호를 안 건드림");
    }

    void TestLedgerShape()
    {
        std::printf("-- 장부: 위조\n");
        CAuthTokenLedger ledger(TTL_SEC);
        char issued[CAuthTokenLedger::TOKEN_BYTES];
        ledger.Issue(ACC_A, 0, issued);

        char other[CAuthTokenLedger::TOKEN_BYTES]{};
        std::memset(other, 'x', SecureRandom::CODE_RANDOM_BYTES);
        AccountId out = NO_ACCOUNT;
        Check(ledger.Consume(other, 1, out) == TokenCheckResult::NotFound, "없는 난수 → NotFound");

        char empty[CAuthTokenLedger::TOKEN_BYTES]{};
        Check(ledger.Consume(empty, 1, out) == TokenCheckResult::BadSize, "전부 0 → BadSize");

        char longer[CAuthTokenLedger::TOKEN_BYTES];
        std::memcpy(longer, issued, sizeof(longer));
        longer[SecureRandom::CODE_RANDOM_BYTES] = 'x';
        Check(ledger.Consume(longer, 1, out) == TokenCheckResult::BadSize, "난수 뒤 칸에 값 → BadSize");
        Check(out == NO_ACCOUNT, "위조로는 번호가 안 나옴");

        Check(ledger.LiveCount() == 1, "위조는 진짜 토큰을 안 지움");
        Check(ledger.Consume(issued, 1, out) == TokenCheckResult::Ok, "진짜 토큰은 그대로 Ok");
    }

    void TestLedgerExpiry()
    {
        std::printf("-- 장부: 만료\n");
        CAuthTokenLedger ledger(TTL_SEC);
        char early[CAuthTokenLedger::TOKEN_BYTES];
        char late[CAuthTokenLedger::TOKEN_BYTES];
        ledger.Issue(ACC_A, 0, early);
        ledger.Issue(ACC_A, 0, late);
        Check(ledger.LiveCount() == 2, "한 계정이 여러 개 받음");

        AccountId out = NO_ACCOUNT;
        Check(ledger.Consume(early, TTL_MS - 1, out) == TokenCheckResult::Ok, "만료 1ms 전 Ok");

        Check(ledger.Consume(late, TTL_MS, out) == TokenCheckResult::Expired, "만료 시각 → Expired");
        Check(ledger.Consume(late, TTL_MS + 1, out) == TokenCheckResult::Expired, "다시 내도 Expired — 안 지움");

        ledger.Sweep(TTL_MS - 1);
        Check(ledger.LiveCount() == 1, "만료 전 정리는 그대로");
        ledger.Sweep(TTL_MS);
        Check(ledger.LiveCount() == 0 && ledger.StoredCount() == 1, "만료되면 대기 수에서 빠지고 보관");
        ledger.Sweep(TTL_MS + 1);
        Check(ledger.LiveCount() == 0, "거듭 정리해도 음수 안 됨");

        Check(ledger.Consume(late, TTL_MS + KEEP_MS - 1, out) == TokenCheckResult::Expired, "보관 중 Expired");
        ledger.Sweep(TTL_MS + KEEP_MS - 1);
        Check(ledger.StoredCount() == 1, "보관 끝나기 1ms 전 그대로");
        ledger.Sweep(TTL_MS + KEEP_MS);
        Check(ledger.StoredCount() == 0, "보관 끝 → 지움");
        Check(ledger.Consume(late, TTL_MS + KEEP_MS, out) == TokenCheckResult::NotFound, "보관 뒤 NotFound");
    }

    void TestLedgerUnique()
    {
        std::printf("-- 장부: 토큰이 전부 다름\n");
        CAuthTokenLedger ledger(TTL_SEC);
        std::set<std::string> seen;
        for (int i = 0; i < 2000; ++i)
        {
            char token[CAuthTokenLedger::TOKEN_BYTES];
            ledger.Issue(ACC_A, 0, token);
            seen.insert(std::string(token, sizeof(token)));
            if (SecureRandom::ClassifyCode(token) != SecureRandom::CodeShape::Valid)
                Check(false, "토큰 꼴이 틀림");
        }
        Check(seen.size() == 2000, "2000개가 전부 다름");
        Check(ledger.StoredCount() == 2000, "장부에 2000개");
    }

    bool LoadDb(const char* iniPath, const char* database, DBConfig& out)
    {
        CIniFile ini;
        if (!ini.Load(iniPath))
            return false;
        out.host = ini.GetString("DB", "Host", "");
        out.port = ini.GetInt("DB", "Port", 0);
        out.user = ini.GetString("DB", "User", "");
        std::string error;
        if (!DbPassword::Read(ini, iniPath, out.password, error))
        {
            std::printf("%s\n", error.c_str());
            return false;
        }
        out.database = database;
        out.connectTimeoutSec = 3;
        out.rwTimeoutSec = 5;
        return !out.host.empty();
    }

    void TestDb(const DBConfig& config)
    {
        std::printf("-- DB: 시험 DB %s\n", config.database.c_str());

        char suffix[CAuthTokenLedger::TOKEN_BYTES];
        SecureRandom::MakeCode(suffix);
        static const char HEX[] = "0123456789abcdef";
        std::string unique;
        for (int i = 0; i < 6; ++i)
            unique += HEX[static_cast<unsigned char>(suffix[i]) & 0x0F];

        CDbConnection closed(config);
        CAccountDb::LoginRow none = CAccountDb::FindOrCreate(closed, "t" + unique);
        Check(!none.ok && closed.TakeJobFailed(), "연결 없음 → ok 아님 · 실패 표시");

        // 첫 연결은 혼자 연다 — 첫 mysql_init 이 라이브러리 전체를 초기화하는데 스레드에 안전하지 않다.
        //   서버는 CDbPipeline::Start 가 먼저 mysql_library_init 을 불러 이 문제가 없다
        CDbConnection conn(config);
        Check(conn.Open(), "시험 연결");

        // 같은 새 이름을 동시에 — 유일 키가 한쪽을 막고 막힌 쪽은 다시 읽는다
        constexpr int THREADS = 8;
        const std::string raceName = "race" + unique;
        std::atomic<int> ready{ 0 };
        std::atomic<bool> go{ false };
        std::vector<CAccountDb::LoginRow> rows(THREADS);
        std::vector<int> failed(THREADS, 0);
        std::vector<std::thread> threads;
        for (int i = 0; i < THREADS; ++i)
        {
            threads.emplace_back([&, i]()
            {
                CDbConnection conn(config);
                const bool opened = conn.Open();
                ++ready;
                while (!go.load())
                    std::this_thread::yield();
                if (opened)
                    rows[i] = CAccountDb::FindOrCreate(conn, raceName);
                failed[i] = conn.TakeJobFailed() ? 1 : 0;
            });
        }
        while (ready.load() < THREADS)
            std::this_thread::yield();
        go = true;
        for (std::thread& thread : threads)
            thread.join();

        int okCount = 0;
        int createdCount = 0;
        int failedCount = 0;
        std::set<int64_t> ids;
        for (int i = 0; i < THREADS; ++i)
        {
            okCount += rows[i].ok ? 1 : 0;
            createdCount += rows[i].created ? 1 : 0;
            failedCount += failed[i];
            ids.insert(ToInt(rows[i].accountId));
        }
        Check(okCount == THREADS, "8개 전부 성공: " + std::to_string(okCount));
        Check(createdCount == 1, "새로 만든 쪽은 하나: " + std::to_string(createdCount));
        Check(ids.size() == 1, "전부 같은 번호");
        Check(failedCount == 0, "유일 키에 막힌 것은 실패로 안 침: " + std::to_string(failedCount));

        const CAccountDb::LoginRow again = CAccountDb::FindOrCreate(conn, raceName);
        Check(again.ok && !again.created && again.accountId == rows[0].accountId, "다시 로그인 = 같은 번호 · 새로 안 만듦");

        std::string upper = raceName;
        for (char& c : upper)
        {
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        }
        const CAccountDb::LoginRow changed = CAccountDb::FindOrCreate(conn, upper);
        Check(changed.ok && !changed.created && changed.accountId == rows[0].accountId, "대소문자만 다른 이름 = 같은 계정");

        const CAccountDb::LoginRow other = CAccountDb::FindOrCreate(conn, "othr" + unique);
        Check(other.ok && other.created && other.accountId != rows[0].accountId, "다른 이름 = 다른 번호(반대 방향)");
        Check(!conn.TakeJobFailed(), "이 연결에 실패 표시 없음");
    }
}

int main(int argc, char** argv)
{
    TestNameRules();
    TestNames();
    TestLedgerConsume();
    TestLedgerShape();
    TestLedgerExpiry();
    TestLedgerUnique();

    if (argc >= 3)
    {
        DBConfig config;
        if (LoadDb(argv[1], argv[2], config))
            TestDb(config);
        else
            Check(false, std::string("INI 를 못 읽음: ") + argv[1]);
    }
    else
    {
        std::printf("-- DB 시험 건너뜀(인자 <ini> <db> 없음)\n");
    }
    return TestHelp::Finish("account_unit_test");
}
