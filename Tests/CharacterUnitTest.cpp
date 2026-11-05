// 캐릭터 서버 부품 시험 — 규칙 · 장부 · 외형 표는 서버 없이 돌린다. 시각은 시험이 넘긴다
//   인자 <ini> <db> 를 주면 시험 DB 로 경쟁까지 본다. 실제 gamedb 에 돌리지 말 것 — 행을 못 지운다
#include <Config/DbPassword.h>
#include <Config/IniFile.h>
#include <Db/DbConnection.h>
#include <Identity/SecureRandom.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "CharSessionTable.h"
#include "CharacterDb.h"
#include "CharacterRules.h"
#include "LookTable.h"
#include "TestHelp.h"
#include "ZoneDirectory.h"

using TestHelp::Check;

namespace
{
    using Name = char16_t[CHAR_NAME_MAX_LEN];

    bool NameOk(const std::u16string& text)
    {
        Name name{};
        for (size_t i = 0; i < text.size() && i < CHAR_NAME_MAX_LEN; ++i)
            name[i] = text[i];
        return CharacterName::IsValid(name);
    }

    void TestNames()
    {
        Check(NameOk(u"가나"), "2글자 한글");
        Check(NameOk(u"가나다라마바"), "6글자 한글");
        Check(NameOk(u"abAB12"), "영문 · 숫자");
        Check(NameOk(u"힣가a1"), "섞임 · 범위 끝 글자");
        Check(!NameOk(u"가"), "1글자 → 거절");
        Check(!NameOk(u""), "빈 이름 → 거절");
        Check(!NameOk(u"가 나"), "공백 → 거절");
        Check(!NameOk(u"ab!"), "특수문자 → 거절");
        Check(!NameOk(u"ㄱㄴ"), "자모 낱자 → 거절");
        Check(!NameOk(u"é1"), "라틴 확장 → 거절(비교 규칙이 e 와 같다고 본다)");

        Name gap{ u'a', u'b', 0, u'c', 0, 0 };
        Check(!CharacterName::IsValid(gap), "0 뒤에 값 → 거절");

        Name mixed{ u'a', 0xAC00, u'1', 0xD7A3, 0, 0 };
        Check(CharacterName::IsValid(mixed), "변환 시험 재료");
        const std::string utf8 = CharacterName::ToUtf8(mixed);
        Check(utf8.size() == 1 + 3 + 1 + 3, "UTF-8 길이: " + std::to_string(utf8.size()));
        Name back{};
        Check(CharacterName::FromUtf8(utf8.data(), utf8.size(), back), "되읽기");
        Check(std::memcmp(mixed, back, sizeof(mixed)) == 0, "왕복 같음");

        Name out{ u'x', u'x', u'x', u'x', u'x', u'x' };
        const std::string seven = "abcdefg";
        Check(!CharacterName::FromUtf8(seven.data(), seven.size(), out) && out[0] == 0, "7글자 행 → false · 칸 비움");
        const std::string oneChar = "a";
        Check(!CharacterName::FromUtf8(oneChar.data(), oneChar.size(), out) && out[0] == 0, "규칙 밖 행 → false");
        const char broken[] = { static_cast<char>(0xEA), static_cast<char>(0xB0) };
        Check(!CharacterName::FromUtf8(broken, sizeof(broken), out), "끝이 잘린 UTF-8 → false");
        const char badTail[] = { static_cast<char>(0xEA), 'a', 'b' };
        Check(!CharacterName::FromUtf8(badTail, sizeof(badTail), out), "이어지는 바이트가 틀림 → false");
    }

    void TestBlob()
    {
        CharacterLookBlob::Look look;
        look.gender = 2; look.skin = 3; look.hair = 5; look.top = 4; look.bottom = 1;

        uint8_t column[CharacterLookBlob::COLUMN_BYTES];
        const int written = CharacterLookBlob::Write(look, column);
        Check(written == 6, "판 1 은 6바이트");
        Check(column[0] == 1, "첫 바이트 = 판 번호");

        CharacterLookBlob::Look read;
        Check(CharacterLookBlob::Read(column, written, read), "DB 길이로 읽기");
        Check(read.gender == 2 && read.skin == 3 && read.hair == 5 && read.top == 4 && read.bottom == 1, "다섯 칸이 제자리");
        Check(CharacterLookBlob::Read(column, CharacterLookBlob::COLUMN_BYTES, read), "와이어 칸 전체로 읽기");

        Check(!CharacterLookBlob::Read(column, 5, read), "짧음 → false");
        Check(!CharacterLookBlob::Read(column, 33, read), "칸보다 김 → false");

        uint8_t unknown[CharacterLookBlob::COLUMN_BYTES];
        std::memcpy(unknown, column, sizeof(unknown));
        unknown[0] = 2;
        Check(!CharacterLookBlob::Read(unknown, sizeof(unknown), read), "모르는 판 → false");
        unknown[0] = 0;
        Check(!CharacterLookBlob::Read(unknown, sizeof(unknown), read), "판 번호 0 → false");

        uint8_t tail[CharacterLookBlob::COLUMN_BYTES];
        std::memcpy(tail, column, sizeof(tail));
        tail[6] = 1;
        Check(!CharacterLookBlob::Read(tail, sizeof(tail), read), "형식 뒤에 값 → false");
    }

    std::string TempPath(const std::string& name)
    {
        return (std::filesystem::temp_directory_path() / ("character_unit_" + name)).string();
    }

    std::string WriteTemp(const std::string& name, const std::string& text)
    {
        const std::string path = TempPath(name);
        std::ofstream file(path, std::ios::binary);
        file << text;
        return path;
    }

    void TestLookTable(const std::string& binDir)
    {
        const std::string good =
            "# comment\n"
            "1,skin,1,b,hd\n1,hair,1,h,\n1,top,1,t\n1,bottom,1,l,\n"
            "2,skin,2,b,hd\n2,hair,2,h,\n2,top,2,t,\n2,bottom,2,l,\n";

        CLookTable table;
        Check(table.Load(WriteTemp("look_good.csv", good)), "맞는 표 읽기(끝 빈 칸 · 안 적음 둘 다)");
        CharacterLookBlob::Look look;
        look.gender = 1; look.skin = 1; look.hair = 1; look.top = 1; look.bottom = 1;
        Check(table.IsAllowed(look), "있는 번호 → 허용");
        CharacterLookBlob::Look wrong = look;
        wrong.gender = 2;
        Check(!table.IsAllowed(wrong), "다른 성별의 번호 → 거절");
        wrong = look; wrong.skin = 2;
        Check(!table.IsAllowed(wrong), "없는 피부 → 거절");
        wrong = look; wrong.hair = 2;
        Check(!table.IsAllowed(wrong), "없는 머리 → 거절");
        wrong = look; wrong.top = 2;
        Check(!table.IsAllowed(wrong), "없는 상의 → 거절");
        wrong = look; wrong.bottom = 2;
        Check(!table.IsAllowed(wrong), "없는 하의 → 거절");
        wrong = look; wrong.gender = 0;
        Check(!table.IsAllowed(wrong), "성별 0 → 거절");
        wrong = look; wrong.gender = 3;
        Check(!table.IsAllowed(wrong), "성별 3 → 거절");
        wrong = look; wrong.gender = 255;
        Check(!table.IsAllowed(wrong), "성별 255 → 범위 밖 안 읽음");

        const auto rejects = [](const std::string& label, const std::string& text)
        {
            CLookTable bad;
            Check(!bad.Load(WriteTemp("look_bad.csv", text)), label);
        };
        const std::string rest = "1,hair,1,h,\n1,top,1,t,\n1,bottom,1,l,\n2,skin,2,b,hd\n2,hair,2,h,\n2,top,2,t,\n2,bottom,2,l,\n";
        rejects("같은 번호 두 번 → 거절", "1,skin,1,b,hd\n1,skin,1,c,hd\n" + rest);
        rejects("모르는 부위 → 거절", "1,skin,1,b,hd\n1,wing,1,w,\n" + rest);
        rejects("성별 3 → 거절", "1,skin,1,b,hd\n3,skin,1,w,hd\n" + rest);
        rejects("번호 0 → 거절", "1,skin,1,b,hd\n1,hair,0,w,\n" + rest);
        rejects("번호 256 → 거절", "1,skin,1,b,hd\n1,hair,256,w,\n" + rest);
        rejects("피부에 머리 그림 없음 → 거절", "1,skin,1,b,\n" + rest);
        rejects("머리 줄에 머리 그림 있음 → 거절", "1,skin,1,b,hd\n1,hair,2,h,hd\n" + rest);
        rejects("그림 이름 빈칸 → 거절", "1,skin,1,b,hd\n1,hair,2,,\n" + rest);
        rejects("칸 모자람 → 거절", "1,skin,1\n" + rest);
        rejects("칸 넘침 → 거절", "1,skin,1,b,hd,x\n" + rest);
        rejects("성별 하나의 부위가 빠짐 → 거절", "1,skin,1,b,hd\n1,hair,1,h,\n1,top,1,t,\n1,bottom,1,l,\n2,skin,2,b,hd\n");
        rejects("빈 파일 → 거절", "# only comment\n");
        CLookTable missing;
        Check(!missing.Load(TempPath("look_not_exists.csv")), "파일 없음 → 거절");

        if (binDir.empty())
            return;
        CLookTable real;
        Check(real.Load(binDir + "LookTable.csv"), "진짜 LookTable.csv 읽기");
        using Part = CLookTable::Part;
        Check(real.Count(1, Part::Skin) == 5 && real.Count(1, Part::Hair) == 8 && real.Count(1, Part::Top) == 6 && real.Count(1, Part::Bottom) == 6, "남 5·8·6·6");
        Check(real.Count(2, Part::Skin) == 4 && real.Count(2, Part::Hair) == 5 && real.Count(2, Part::Top) == 4 && real.Count(2, Part::Bottom) == 4, "여 4·5·4·4");
    }

    void TestRules()
    {
        Check(!CCharacterRules::IsValidSlot(-1, 4), "자리 -1 → 거절");
        Check(CCharacterRules::IsValidSlot(0, 4) && CCharacterRules::IsValidSlot(3, 4), "자리 0 · 3");
        Check(!CCharacterRules::IsValidSlot(4, 4), "자리 4 → 거절");

        using State = CCharacterRules::DeleteState;
        const int64_t week = 7 * CCharacterRules::SECONDS_PER_DAY;
        Check(CCharacterRules::ClassifyDelete(false, 0, 7) == State::Alive, "지움 없음 → 살아 있음");
        Check(CCharacterRules::ClassifyDelete(true, 0, 7) == State::Pending, "방금 지움 → 대기");
        Check(CCharacterRules::ClassifyDelete(true, week - 1, 7) == State::Pending, "1초 전 → 대기");
        Check(CCharacterRules::ClassifyDelete(true, week, 7) == State::Expired, "딱 7일 → 지남");

        Check(CCharacterRules::GraceLeftSec(0, 7) == week, "방금 지움 = 7일 남음");
        Check(CCharacterRules::GraceLeftSec(week - 1, 7) == 1, "1초 남음");
        Check(CCharacterRules::GraceLeftSec(week + 5, 7) == 1, "지났어도 0 은 안 됨(0 = 대기 아님)");

        using Key = CCharacterRules::DuplicateKey;
        Check(CCharacterRules::ClassifyDuplicate("Duplicate entry 'abc' for key 'character_roster.uq_roster_name'") == Key::Name, "이름 키");
        Check(CCharacterRules::ClassifyDuplicate("Duplicate entry '1-0' for key 'character_roster.uq_roster_slot'") == Key::Slot, "자리 키");
        Check(CCharacterRules::ClassifyDuplicate("Duplicate entry '1' for key 'PRIMARY'") == Key::Unknown, "다른 키");
        Check(CCharacterRules::ClassifyDuplicate(nullptr) == Key::Unknown, "문구 없음");

        // 갈래 전수 — 이름이 전부 다르고 unknown 이 없다
        const auto names = [](auto kindCount, auto cast)
        {
            std::set<std::string> seen;
            for (int i = 0; i < kindCount; ++i)
                seen.insert(CCharacterRules::ResultName(cast(i)));
            return seen;
        };
        const auto exact = [](const std::set<std::string>& seen, int count) { return static_cast<int>(seen.size()) == count && seen.count("unknown") == 0; };
        Check(exact(names(static_cast<int>(CharLoginResult::COUNT), [](int i) { return static_cast<CharLoginResult>(i); }), 6), "로그인 결과 이름 6");
        Check(exact(names(static_cast<int>(CharCreateResult::COUNT), [](int i) { return static_cast<CharCreateResult>(i); }), 8), "만들기 결과 이름 8");
        Check(exact(names(static_cast<int>(CharDeleteResult::COUNT), [](int i) { return static_cast<CharDeleteResult>(i); }), 5), "삭제 결과 이름 5");
        Check(exact(names(static_cast<int>(CharRestoreResult::COUNT), [](int i) { return static_cast<CharRestoreResult>(i); }), 5), "복구 결과 이름 5");
        Check(exact(names(static_cast<int>(CharSelectResult::COUNT), [](int i) { return static_cast<CharSelectResult>(i); }), 7), "고르기 결과 이름 7");
    }

    void TestSessionTable()
    {
        using Stage = CCharSessionTable::Stage;
        const SessionId s1{ 101 };
        const SessionId s2{ 102 };
        const AccountId acc{ 7 };

        CCharSessionTable table;
        table.Add(s1);
        Check(table.Find(s1) != nullptr && table.Find(s1)->stage == Stage::Fresh, "접속 = Fresh");
        Check(table.Find(s2) == nullptr, "없는 세션");
        Check(!table.BeginLogin(s2), "없는 세션 로그인 → false");
        Check(table.BeginLogin(s1) && table.Find(s1)->stage == Stage::Verifying, "로그인 시작");
        Check(!table.BeginLogin(s1), "확인 중 다시 → false");

        table.FailLogin(s1);
        Check(table.Find(s1)->stage == Stage::Fresh, "실패 → Fresh 로");
        Check(table.BeginLogin(s1), "같은 연결로 다시");

        Check(table.CompleteLogin(s1, acc) == NO_SESSION, "옛 세션 없음");
        Check(table.Find(s1)->stage == Stage::LoggedIn && table.Find(s1)->account == acc, "로그인 끝 · 계정 묶임");
        Check(!table.BeginLogin(s1), "로그인 뒤 다시 → false");
        table.FailLogin(s1);
        Check(table.Find(s1)->stage == Stage::LoggedIn, "로그인 뒤 실패 표시 무시");

        Check(table.BeginRequest(s1), "처리 시작");
        Check(!table.BeginRequest(s1), "처리 중 → false(BUSY)");
        table.EndRequest(s1);
        Check(table.BeginRequest(s1), "끝나면 다시");
        table.EndRequest(s1);
        Check(!table.BeginRequest(s2), "없는 세션 처리 → false");
        table.EndRequest(s2);   // 터지지 않는다

        table.Add(s2);
        Check(table.BeginLogin(s2), "새 창 로그인");
        Check(table.CompleteLogin(s2, acc) == s1, "같은 계정 → 옛 세션을 돌려줌");
        Check(table.Find(s1)->stage == Stage::Replaced, "옛 세션 = 밀려남");
        Check(table.AccountCount() == 1, "계정 칸 하나");

        table.Remove(s1);
        Check(table.AccountCount() == 1, "옛 세션이 나가도 계정 칸은 새 세션 것");
        table.Add(s1);
        Check(table.BeginLogin(s1), "같은 번호로 다른 창이 로그인 시작");
        Check(table.CompleteLogin(s1, AccountId{ 8 }) == NO_SESSION, "계정이 다르면 안 밀림(반대 방향)");
        Check(table.Find(s2)->stage == Stage::LoggedIn, "다른 계정은 그대로");

        table.Remove(s2);
        Check(table.AccountCount() == 1 && table.Count() == 1, "나가면 계정 칸도 지움");
        table.Remove(s2);   // 두 번 불러도 터지지 않는다
        Check(table.Count() == 1, "두 번 지움 = 그대로");
    }

    void TestZoneDirectory()
    {
        const SessionId z1{ 1 };
        const SessionId z2{ 2 };
        const SessionId z3{ 3 };
        const ZoneServerId id1{ 1 };
        const ZoneServerId id2{ 2 };
        const CharacterId c1{ 1000 };
        const CharacterId c2{ 2000 };
        const AccountId a1{ 1 };

        CZoneDirectory zones;
        Check(zones.PickZone() == nullptr, "없으면 nullptr");
        Check(!zones.IsRegistered(z1), "등록 전");
        Check(!zones.OnDisconnected(z1), "등록 전 끊김 → false");

        Check(zones.Register(z1, id2, 100, "ws://b") == NO_SESSION, "첫 등록");
        Check(zones.Register(z2, id1, 100, "ws://a") == NO_SESSION, "다른 번호 등록");
        Check(zones.ZoneCount() == 2, "둘");
        Check(zones.PickZone() != nullptr && zones.PickZone()->session == z2, "번호가 가장 작은 것");

        zones.Enter(z1, c1);
        Check(zones.IsInGame(c1, 0), "입장 통지 → 게임 중");
        Check(zones.OnlineCount() == 1, "접속 수 1");
        Check(!zones.IsInGame(c2, 0), "다른 캐릭터는 아님(반대 방향)");
        zones.Leave(z1, c1);
        Check(!zones.IsInGame(c1, 0) && zones.OnlineCount() == 0, "퇴장 → 아님");

        zones.Enter(z3, c1);
        Check(!zones.IsInGame(c1, 0), "등록 안 된 세션의 입장은 안 적음");

        // 입장 대기
        zones.AddEntering(z1, c1, a1, 1000);
        Check(zones.IsInGame(c1, 999), "만료 전 = 삭제 막음");
        Check(!zones.IsInGame(c1, 1000), "만료 시각 = 안 막음");
        Check(zones.OnlineCount() == 0, "입장 대기는 접속 수에 안 셈");
        zones.Sweep(999);
        Check(zones.EnteringCount() == 1, "만료 전 정리는 그대로");
        zones.Sweep(1000);
        Check(zones.EnteringCount() == 0, "만료 뒤 정리");

        zones.AddEntering(z1, c1, a1, 1000);
        zones.AddEntering(z1, c2, a1, 2000);
        Check(zones.EnteringCount() == 1 && !zones.IsInGame(c1, 0) && zones.IsInGame(c2, 0), "같은 계정 새 예고 → 앞 항목 지움");

        zones.Enter(z1, c2);
        Check(zones.EnteringCount() == 0 && zones.IsInGame(c2, 5000) && zones.OnlineCount() == 1, "입장 통지 → 대기에서 접속 중으로");

        // 끊김 — 그 존의 접속 · 대기를 지운다
        zones.AddEntering(z1, c1, a1, 9000);
        Check(zones.OnDisconnected(z1), "끊김 → true");
        Check(!zones.IsInGame(c2, 0) && !zones.IsInGame(c1, 0) && zones.EnteringCount() == 0, "끊기면 장부에서 빠짐");
        Check(zones.ZoneCount() == 1, "남은 존 하나");
        Check(zones.OnlineCount() == 0, "접속 수 0");

        // 재등록 명단 → 복원
        Check(zones.Register(z1, id2, 100, "ws://b") == NO_SESSION, "다시 등록");
        zones.Enter(z1, c2);
        Check(zones.IsInGame(c2, 0), "명단으로 복원");

        // 같은 번호로 새 연결
        zones.AddEntering(z1, c1, a1, 9000);
        Check(zones.Register(z3, id2, 200, "ws://c") == z1, "같은 번호 → 옛 세션을 돌려줌");
        Check(!zones.IsRegistered(z1) && zones.IsRegistered(z3), "새 연결로 바뀜");
        Check(!zones.IsInGame(c2, 0) && zones.EnteringCount() == 0, "옛 연결 장부는 지움");
        Check(!zones.OnDisconnected(z1), "옛 연결의 끊김 이벤트 → false");
        Check(zones.IsRegistered(z3), "새 연결은 그대로");
        Check(zones.Register(z3, id2, 300, "ws://d") == NO_SESSION, "같은 연결이 다시 등록 → 옛 세션 없음");
    }

    bool LoadDb(const std::string& iniPath, const std::string& database, DBConfig& out)
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
        out.connectTimeoutSec = ini.GetInt("DB", "ConnectTimeoutSec", 3);
        out.rwTimeoutSec = ini.GetInt("DB", "RwTimeoutSec", 5);
        return true;
    }

    CCharacterDb::CreateRequest MakeCreate(AccountId account, int slot, const std::string& name)
    {
        CCharacterDb::CreateRequest request;
        request.account = account;
        request.slot = slot;
        request.job = 1;
        for (size_t i = 0; i < name.size() && i < CHAR_NAME_MAX_LEN; ++i)
            request.name[i] = static_cast<char16_t>(static_cast<unsigned char>(name[i]));
        CharacterLookBlob::Look look;
        look.gender = 1; look.skin = 1; look.hair = 1; look.top = 1; look.bottom = 1;
        CharacterLookBlob::Write(look, request.look);
        return request;
    }

    constexpr int GRACE_DAYS = 7;
    constexpr int RACE_THREADS = 8;

    template <typename TOutcome>
    std::vector<TOutcome> RunRace(const DBConfig& config, std::function<TOutcome(CDbConnection&, int)> work)
    {
        std::atomic<int> ready{ 0 };
        std::atomic<bool> go{ false };
        std::vector<TOutcome> rows(RACE_THREADS);
        std::vector<std::thread> threads;
        for (int i = 0; i < RACE_THREADS; ++i)
        {
            threads.emplace_back([&, i]()
            {
                CDbConnection conn(config);
                const bool opened = conn.Open();
                ++ready;
                while (!go.load())
                    std::this_thread::yield();
                if (opened)
                    rows[i] = work(conn, i);
            });
        }
        while (ready.load() < RACE_THREADS)
            std::this_thread::yield();
        go = true;
        for (std::thread& thread : threads)
            thread.join();
        return rows;
    }

    void TestDb(const DBConfig& config)
    {
        std::printf("-- DB: 시험 DB %s\n", config.database.c_str());

        char random[SecureRandom::CODE_WIRE_BYTES];
        SecureRandom::MakeCode(random);
        static const char LETTERS[] = "abcdefghijklmnopqrstuvwxyz";
        std::string unique;
        for (int i = 0; i < 4; ++i)
            unique += LETTERS[static_cast<unsigned char>(random[i]) % 26];
        // 계정 번호도 판마다 다르게 — 표를 안 비운 채 되돌려 돌린다. 시험 DB 는 root 로 언제든 비울 수 있다
        int64_t base = 0;
        std::memcpy(&base, random + 4, 4);
        base = 500000000 + (base & 0xFFFFFF) * 100;
        const AccountId acc{ base + 1 };
        const AccountId other{ base + 2 };
        const AccountId raceAccount{ base + 3 };

        {
            CDbConnection closed(config);
            const CCharacterDb::ListResult none = CCharacterDb::LoadList(closed, acc, GRACE_DAYS);
            Check(!none.ok && closed.TakeJobFailed(), "연결 없음 → ok 아님 · 실패 표시");
            const CCharacterDb::CreateOutcome made = CCharacterDb::Create(closed, MakeCreate(acc, 0, "aa"), GRACE_DAYS);
            Check(made.result == CharCreateResult::DB_FAIL && closed.TakeJobFailed(), "연결 없음 만들기 → DB_FAIL");
            Check(CCharacterDb::CheckSelect(closed, acc, CharacterId{ 1 }) == CharSelectResult::DB_FAIL && closed.TakeJobFailed(), "연결 없음 고르기 → DB_FAIL");
        }

        CDbConnection conn(config);
        Check(conn.Open(), "시험 연결");

        const CCharacterDb::ListResult empty = CCharacterDb::LoadList(conn, acc, GRACE_DAYS);
        Check(empty.ok && empty.rows.empty(), "캐릭터 없음 = ok · 0줄");

        const CCharacterDb::CreateOutcome first = CCharacterDb::Create(conn, MakeCreate(acc, 0, "m" + unique), GRACE_DAYS);
        Check(first.result == CharCreateResult::OK, "만들기 OK");
        Check(ToInt(first.characterId) >= 1000000000000LL, "번호가 1조 구간: " + std::to_string(ToInt(first.characterId)));
        Check(first.list.ok && first.list.rows.size() == 1, "새 목록 1줄");
        if (!first.list.rows.empty())
        {
            const CharListEntry& row = first.list.rows[0];
            Check(row.characterId == ToInt(first.characterId) && row.slot == 0 && row.job == 1 && row.level == 1 && row.deleteLeftSec == 0, "줄 값");
            Check(row.look[0] == 1 && row.look[1] == 1 && row.look[6] == 0, "외형 칸");
            Check(row.name[0] == u'm', "이름 칸");
        }

        const CCharacterDb::CreateOutcome again = CCharacterDb::Create(conn, MakeCreate(acc, 0, "m" + unique), GRACE_DAYS);
        Check(again.result == CharCreateResult::OK && again.characterId == first.characterId, "회신 유실 = 같은 번호로 OK");
        Check(!conn.TakeJobFailed(), "유일 키에 막힌 것은 실패로 안 침");

        std::string upper = "M" + unique;
        const CCharacterDb::CreateOutcome caseOnly = CCharacterDb::Create(conn, MakeCreate(acc, 1, upper), GRACE_DAYS);
        Check(caseOnly.result == CharCreateResult::TAKEN, "대소문자만 다른 이름 → TAKEN");
        const CCharacterDb::CreateOutcome otherName = CCharacterDb::Create(conn, MakeCreate(other, 0, "m" + unique), GRACE_DAYS);
        Check(otherName.result == CharCreateResult::TAKEN, "다른 계정 같은 이름 → TAKEN");
        const CCharacterDb::CreateOutcome slotTaken = CCharacterDb::Create(conn, MakeCreate(acc, 0, "n" + unique), GRACE_DAYS);
        Check(slotTaken.result == CharCreateResult::SLOT_TAKEN, "같은 자리 다른 이름 → SLOT_TAKEN");
        Check(!conn.TakeJobFailed(), "막힌 만들기에 실패 표시 없음");

        const CCharacterDb::CreateOutcome second = CCharacterDb::Create(conn, MakeCreate(acc, 2, "n" + unique), GRACE_DAYS);
        Check(second.result == CharCreateResult::OK && second.list.rows.size() == 2, "다른 자리 OK · 목록 2줄");
        if (second.list.rows.size() == 2)
            Check(second.list.rows[0].slot == 0 && second.list.rows[1].slot == 2, "자리 순서");

        // 고르기 · 소유
        Check(CCharacterDb::CheckSelect(conn, acc, first.characterId) == CharSelectResult::OK, "내 캐릭터 고르기 OK");
        Check(CCharacterDb::CheckSelect(conn, other, first.characterId) == CharSelectResult::NOT_MINE, "남의 캐릭터 → NOT_MINE");
        Check(CCharacterDb::CheckSelect(conn, acc, CharacterId{ 42 }) == CharSelectResult::NOT_MINE, "없는 번호 → NOT_MINE");

        // 삭제
        Check(CCharacterDb::Delete(conn, other, first.characterId, false, GRACE_DAYS).result == CharDeleteResult::NOT_MINE, "남의 캐릭터 삭제 → NOT_MINE");
        Check(CCharacterDb::Delete(conn, other, first.characterId, true, GRACE_DAYS).result == CharDeleteResult::NOT_MINE, "남의 캐릭터가 접속 중이어도 NOT_MINE");
        Check(CCharacterDb::Delete(conn, acc, first.characterId, true, GRACE_DAYS).result == CharDeleteResult::IN_GAME, "접속 중 → IN_GAME");
        Check(CCharacterDb::CheckSelect(conn, acc, first.characterId) == CharSelectResult::OK, "막힌 삭제는 표시 안 함");

        const CCharacterDb::DeleteOutcome removed = CCharacterDb::Delete(conn, acc, first.characterId, false, GRACE_DAYS);
        Check(removed.result == CharDeleteResult::OK, "삭제 OK");
        Check(removed.list.rows.size() == 2, "자리 유지 — 목록 2줄 그대로");
        if (!removed.list.rows.empty())
        {
            const int32_t left = removed.list.rows[0].deleteLeftSec;
            Check(left > 0 && left <= GRACE_DAYS * CCharacterRules::SECONDS_PER_DAY, "남은 초: " + std::to_string(left));
        }
        Check(CCharacterDb::Delete(conn, acc, first.characterId, false, GRACE_DAYS).result == CharDeleteResult::OK, "다시 지움 = OK(회신 유실)");
        Check(CCharacterDb::CheckSelect(conn, acc, first.characterId) == CharSelectResult::DELETED, "삭제 대기 고르기 → DELETED");
        Check(CCharacterDb::Create(conn, MakeCreate(acc, 0, "o" + unique), GRACE_DAYS).result == CharCreateResult::SLOT_TAKEN, "삭제 대기 자리 → SLOT_TAKEN");

        // 복구
        Check(CCharacterDb::Restore(conn, other, first.characterId, GRACE_DAYS).result == CharRestoreResult::NOT_MINE, "남의 캐릭터 복구 → NOT_MINE");
        const CCharacterDb::RestoreOutcome restored = CCharacterDb::Restore(conn, acc, first.characterId, GRACE_DAYS);
        Check(restored.result == CharRestoreResult::OK && restored.list.rows.size() == 2, "복구 OK · 목록");
        if (!restored.list.rows.empty())
            Check(restored.list.rows[0].slot == 0 && restored.list.rows[0].deleteLeftSec == 0, "같은 자리 · 대기 아님");
        Check(CCharacterDb::Restore(conn, acc, first.characterId, GRACE_DAYS).result == CharRestoreResult::OK, "다시 복구 = OK");
        Check(CCharacterDb::CheckSelect(conn, acc, first.characterId) == CharSelectResult::OK, "복구 뒤 고르기 OK");

        // 소유 표와 characters 가 같이 들어갔나 — 목록이 JOIN 이라 없으면 줄이 빠진다
        Check(!conn.TakeJobFailed(), "삭제·복구에 실패 표시 없음");

        // 경쟁 — 같은 자리 다른 이름
        const std::vector<CCharacterDb::CreateOutcome> sameSlot = RunRace<CCharacterDb::CreateOutcome>(config,
            [&](CDbConnection& worker, int i) { return CCharacterDb::Create(worker, MakeCreate(raceAccount, 0, "r" + std::to_string(i) + unique), GRACE_DAYS); });
        int okCount = 0;
        int slotCount = 0;
        for (const CCharacterDb::CreateOutcome& row : sameSlot)
        {
            okCount += row.result == CharCreateResult::OK ? 1 : 0;
            slotCount += row.result == CharCreateResult::SLOT_TAKEN ? 1 : 0;
        }
        Check(okCount == 1, "같은 자리 동시 8 → 하나만 OK: " + std::to_string(okCount));
        Check(slotCount == RACE_THREADS - 1, "나머지는 SLOT_TAKEN: " + std::to_string(slotCount));

        // 경쟁 — 다른 계정 같은 이름
        const std::string raceName = "s" + unique;
        const std::vector<CCharacterDb::CreateOutcome> sameName = RunRace<CCharacterDb::CreateOutcome>(config,
            [&](CDbConnection& worker, int i) { return CCharacterDb::Create(worker, MakeCreate(AccountId{ base + 10 + i }, 0, raceName), GRACE_DAYS); });
        okCount = 0;
        int takenCount = 0;
        for (const CCharacterDb::CreateOutcome& row : sameName)
        {
            okCount += row.result == CharCreateResult::OK ? 1 : 0;
            takenCount += row.result == CharCreateResult::TAKEN ? 1 : 0;
        }
        Check(okCount == 1, "같은 이름 동시 8 → 정확히 하나만 OK: " + std::to_string(okCount));
        Check(takenCount == RACE_THREADS - 1, "나머지는 TAKEN: " + std::to_string(takenCount));

        // 경쟁 — 같은 요청을 동시에(회신 유실로 다시 낸 것) — 전부 같은 번호로 OK
        const std::string resend = "t" + unique;
        const AccountId resendAccount{ base + 40 };
        const std::vector<CCharacterDb::CreateOutcome> sameCreate = RunRace<CCharacterDb::CreateOutcome>(config,
            [&](CDbConnection& worker, int) { return CCharacterDb::Create(worker, MakeCreate(resendAccount, 0, resend), GRACE_DAYS); });
        okCount = 0;
        std::set<int64_t> ids;
        for (const CCharacterDb::CreateOutcome& row : sameCreate)
        {
            okCount += row.result == CharCreateResult::OK ? 1 : 0;
            ids.insert(ToInt(row.characterId));
        }
        Check(okCount == RACE_THREADS && ids.size() == 1, "같은 요청 동시 8 → 전부 같은 번호로 OK: " + std::to_string(okCount));
    }
}

int main(int argc, char** argv)
{
    TestNames();
    TestBlob();
    TestLookTable(argc >= 2 ? [&]() { std::string dir = argv[1]; return dir.substr(0, dir.find_last_of("/\\") + 1); }() : std::string());
    TestRules();
    TestSessionTable();
    TestZoneDirectory();

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
    return TestHelp::Finish("character_unit_test");
}
