// 우편 와이어의 바이트 규칙 시험 — 글자 변환(TextUtf.h) · 첨부 바이트(MailAttach.h). 서버를 안 띄운다
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

#include <Text/TextUtf.h>

#include "MailAttach.h"
#include "TestHelp.h"

using TestHelp::Check;

namespace
{
    std::string Bytes(std::initializer_list<int> list)
    {
        std::string s;
        for (int b : list)
            s += static_cast<char>(b);
        return s;
    }

    bool AllZero(const char16_t* cells, int capacity)
    {
        for (int i = 0; i < capacity; ++i)
        {
            if (cells[i] != 0)
                return false;
        }
        return true;
    }

    // UTF-8 → 칸 → UTF-8 이 처음 바이트와 같은가
    bool RoundTrip(const std::string& utf8, int capacity, int expectCells)
    {
        char16_t cells[MAIL_BODY_MAX]{};
        if (!TextUtf::FromUtf8(utf8.data(), utf8.size(), cells, capacity))
            return false;
        const int length = TextUtf::CellLength(cells, capacity);
        std::string back;
        return length == expectCells && TextUtf::ToUtf8(cells, length, back) && back == utf8;
    }

    void TestRoundTrip()
    {
        Check(RoundTrip("Hello", MAIL_TITLE_MAX, 5), "영문 왕복 · 5칸");
        Check(RoundTrip("\xEC\x9A\xB0\xED\x8E\xB8\xED\x95\xA8", MAIL_TITLE_MAX, 3), "한글 3자(9바이트) 왕복 · 3칸");
        Check(RoundTrip("\xC3\xA9", MAIL_TITLE_MAX, 1), "2바이트 글자(é) 왕복 · 1칸");
        Check(RoundTrip("\xF0\x9F\x98\x80", MAIL_TITLE_MAX, 2), "이모지(U+1F600) 왕복 · 대리쌍 2칸");
        Check(RoundTrip("a\nb", MAIL_BODY_MAX, 3), "줄바꿈 든 본문 왕복 · 줄바꿈 1칸");
        Check(RoundTrip("", MAIL_BODY_MAX, 0), "빈 글자 왕복");

        char16_t cells[MAIL_TITLE_MAX]{};
        const std::string emoji = "\xF0\x9F\x98\x80";
        Check(TextUtf::FromUtf8(emoji.data(), emoji.size(), cells, MAIL_TITLE_MAX) && cells[0] == 0xD83D && cells[1] == 0xDE00,
            "U+1F600 = D83D DE00");
    }

    void TestLoneSurrogate()
    {
        std::string out = "x";
        const char16_t high[] = { 0xD83D };
        const char16_t low[] = { 0xDE00 };
        const char16_t reversed[] = { 0xDE00, 0xD83D };
        const char16_t highThenA[] = { 0xD83D, u'A' };
        const char16_t pair[] = { u'A', 0xD83D, 0xDE00 };

        Check(!TextUtf::ToUtf8(high, 1, out) && out.empty(), "홀로 남은 높은 칸 → false · 비운다");
        Check(!TextUtf::ToUtf8(low, 1, out) && out.empty(), "홀로 남은 낮은 칸 → false");
        Check(!TextUtf::ToUtf8(reversed, 2, out), "뒤집힌 짝 → false");
        Check(!TextUtf::ToUtf8(highThenA, 2, out), "높은 칸 뒤에 보통 글자 → false");
        Check(TextUtf::ToUtf8(pair, 3, out) && out == "A\xF0\x9F\x98\x80", "짝 맞은 대리쌍 → 4바이트");

        Check(!TextUtf::PairsOk(high, 1), "PairsOk — 홀로 높은 칸");
        Check(!TextUtf::PairsOk(low, 1), "PairsOk — 홀로 낮은 칸");
        Check(!TextUtf::PairsOk(reversed, 2), "PairsOk — 뒤집힌 짝");
        Check(TextUtf::PairsOk(pair, 3), "PairsOk — 짝 맞음");
        // 대리쌍이 칸 끝에서 잘린 경우 — 웹 maxlength 가 20칸째에서 짝을 자른 꼴
        char16_t cut[MAIL_TITLE_MAX]{};
        for (int i = 0; i < MAIL_TITLE_MAX - 1; ++i)
            cut[i] = u'a';
        cut[MAIL_TITLE_MAX - 1] = 0xD83D;
        Check(!TextUtf::PairsOk(cut, TextUtf::CellLength(cut, MAIL_TITLE_MAX)), "PairsOk — 끝 칸에서 잘린 짝");
    }

    void TestBadUtf8()
    {
        struct Case { std::string bytes; const char* what; };
        const Case cases[] = {
            { Bytes({ 0xC0, 0x80 }), "지나치게 긴 2바이트(NUL)" },
            { Bytes({ 0xC1, 0xBF }), "지나치게 긴 2바이트(0x7F)" },
            { Bytes({ 0xE0, 0x80, 0x80 }), "지나치게 긴 3바이트" },
            { Bytes({ 0xF0, 0x80, 0x80, 0x80 }), "지나치게 긴 4바이트" },
            { Bytes({ 0xED, 0xA0, 0x80 }), "대리쌍 영역 U+D800 을 직접 인코딩" },
            { Bytes({ 0xED, 0xBF, 0xBF }), "대리쌍 영역 U+DFFF 를 직접 인코딩" },
            { Bytes({ 0xF4, 0x90, 0x80, 0x80 }), "U+10FFFF 초과" },
            { Bytes({ 0xEA }), "잘린 3바이트(첫 바이트만)" },
            { Bytes({ 0xEA, 0xB0 }), "잘린 3바이트(두 바이트)" },
            { Bytes({ 0xEA, 0x41, 0x80 }), "이어지는 바이트가 아님" },
            { Bytes({ 0x80 }), "이어지는 바이트로 시작" },
            { Bytes({ 0xF8, 0x88, 0x80, 0x80, 0x80 }), "0xF8 첫 바이트" },
            { Bytes({ 0xFF }), "0xFF" },
            { Bytes({ 'a', 0x00, 'b' }), "글자 0(NUL)이 낌" },
        };
        for (const Case& c : cases)
        {
            char16_t cells[MAIL_TITLE_MAX];
            for (char16_t& cell : cells)
                cell = u'?';
            const bool ok = TextUtf::FromUtf8(c.bytes.data(), c.bytes.size(), cells, MAIL_TITLE_MAX);
            Check(!ok && AllZero(cells, MAIL_TITLE_MAX), std::string("깨진 UTF-8 → false · 칸 전부 0 — ") + c.what);
        }
        // 4바이트 경계 바로 안쪽은 받는다
        const std::string maxCp = Bytes({ 0xF4, 0x8F, 0xBF, 0xBF });
        Check(RoundTrip(maxCp, MAIL_TITLE_MAX, 2), "U+10FFFF 는 받는다");
        const std::string beforeSur = Bytes({ 0xED, 0x9F, 0xBF });
        Check(RoundTrip(beforeSur, MAIL_TITLE_MAX, 1), "U+D7FF 는 받는다");
    }

    void TestCapacity()
    {
        char16_t cells[MAIL_TITLE_MAX]{};
        const std::string twenty(MAIL_TITLE_MAX, 'a');
        const std::string twentyOne(MAIL_TITLE_MAX + 1, 'a');
        Check(TextUtf::FromUtf8(twenty.data(), twenty.size(), cells, MAIL_TITLE_MAX) && TextUtf::CellLength(cells, MAIL_TITLE_MAX) == MAIL_TITLE_MAX,
            "칸 수만큼(20자)은 들어간다 · 끝 0 없이 20칸");
        Check(!TextUtf::FromUtf8(twentyOne.data(), twentyOne.size(), cells, MAIL_TITLE_MAX) && AllZero(cells, MAIL_TITLE_MAX), "21자 → false · 칸 전부 0");
        const std::string edge = std::string(MAIL_TITLE_MAX - 1, 'a') + "\xF0\x9F\x98\x80";
        Check(!TextUtf::FromUtf8(edge.data(), edge.size(), cells, MAIL_TITLE_MAX), "19자 + 대리쌍(2칸) → 칸 모자람 false");
        const std::string fits = std::string(MAIL_TITLE_MAX - 2, 'a') + "\xF0\x9F\x98\x80";
        Check(TextUtf::FromUtf8(fits.data(), fits.size(), cells, MAIL_TITLE_MAX) && TextUtf::CellLength(cells, MAIL_TITLE_MAX) == MAIL_TITLE_MAX,
            "18자 + 대리쌍 = 20칸 꼭 맞음");
    }

    void TestCellLength()
    {
        const char16_t two[4] = { u'a', u'b', 0, 0 };
        const char16_t hidden[4] = { u'a', u'b', 0, u'c' };
        const char16_t full[4] = { u'a', u'b', u'c', u'd' };
        const char16_t none[4] = { 0, 0, 0, 0 };
        const char16_t gap[4] = { 0, u'x', 0, 0 };
        Check(TextUtf::CellLength(two, 4) == 2, "CellLength — 0 앞까지 2");
        Check(TextUtf::CellLength(hidden, 4) == -1, "CellLength — 0 뒤에 값 → -1");
        Check(TextUtf::CellLength(full, 4) == 4, "CellLength — 꽉 참(끝 0 없음) → 4");
        Check(TextUtf::CellLength(none, 4) == 0, "CellLength — 비었음 → 0");
        Check(TextUtf::CellLength(gap, 4) == -1, "CellLength — 첫 칸 0 뒤에 값 → -1");
    }

    void TestControl()
    {
        const char16_t newline[] = { u'a', u'\n', u'b' };
        const char16_t carriage[] = { u'a', u'\r' };
        const char16_t tab[] = { u'\t' };
        const char16_t del[] = { 0x7F };
        const char16_t unit[] = { 0x1F };
        const char16_t space[] = { 0x20, 0x7E, 0x80, 0xAC00 };
        Check(!TextUtf::HasControl(newline, 3, true), "본문 — 줄바꿈은 된다");
        Check(TextUtf::HasControl(newline, 3, false), "제목 — 줄바꿈은 안 된다");
        Check(TextUtf::HasControl(carriage, 2, true), "본문 — \\r 은 안 된다");
        Check(TextUtf::HasControl(tab, 1, true), "본문 — 탭은 안 된다");
        Check(TextUtf::HasControl(del, 1, true), "0x7F 는 제어 문자");
        Check(TextUtf::HasControl(unit, 1, true), "0x1F 는 제어 문자");
        Check(!TextUtf::HasControl(space, 4, false), "0x20 · 0x7E · 0x80 · 한글은 제어 문자가 아니다");
    }

    void TestAttachRoundTrip()
    {
        MailAttach::Items none;
        uint8_t bytes[MailAttach::MAX_BYTES];
        Check(MailAttach::Write(none, bytes) == 0, "첨부 없음 → 빈 값(0바이트)");
        MailAttach::Items read;
        read.count = 5;
        Check(MailAttach::Read(nullptr, 0, read) && read.count == 0, "빈 값 → 칸 0 개 · true");

        MailAttach::Items one;
        one.count = 1;
        one.items[0] = MailItemEntry{ 0x01020304, 7 };
        Check(MailAttach::Write(one, bytes) == 10, "한 칸 = 10바이트");
        const uint8_t expect[10] = { 1, 1, 0x04, 0x03, 0x02, 0x01, 7, 0, 0, 0 };
        Check(std::memcmp(bytes, expect, sizeof(expect)) == 0, "바이트 꼴 — 판 1 · 칸 1 · 번호 · 개수(리틀 엔디언)");

        MailAttach::Items full;
        full.count = MAIL_MAX_ITEMS;
        for (int i = 0; i < MAIL_MAX_ITEMS; ++i)
            full.items[i] = MailItemEntry{ 100 + i, 1000 + i };
        const int size = MailAttach::Write(full, bytes);
        Check(size == MailAttach::MAX_BYTES && size == 66, "8칸 = 66바이트");
        Check(MailAttach::Read(bytes, size, read) && read.count == MAIL_MAX_ITEMS, "8칸 왕복 칸 수");
        bool same = true;
        for (int i = 0; i < MAIL_MAX_ITEMS; ++i)
            same = same && read.items[i].itemId == 100 + i && read.items[i].count == 1000 + i;
        Check(same, "8칸 왕복 값이 같다");

        MailAttach::Items twice;
        twice.count = 2;
        twice.items[0] = MailItemEntry{ 1, 3 };
        twice.items[1] = MailItemEntry{ 1, 5 };
        const int twiceSize = MailAttach::Write(twice, bytes);
        Check(MailAttach::Read(bytes, twiceSize, read) && read.count == 2, "같은 아이템 두 칸도 된다");

        MailAttach::Items over;
        over.count = MAIL_MAX_ITEMS + 1;
        Check(MailAttach::Write(over, bytes) == -1, "9칸 쓰기 → -1");
        over.count = -1;
        Check(MailAttach::Write(over, bytes) == -1, "음수 칸 쓰기 → -1");
    }

    void TestAttachBad()
    {
        MailAttach::Items two;
        two.count = 2;
        two.items[0] = MailItemEntry{ 5, 1 };
        two.items[1] = MailItemEntry{ 6, 2 };
        uint8_t good[MailAttach::MAX_BYTES];
        const int size = MailAttach::Write(two, good);

        struct Case { std::string bytes; const char* what; };
        auto from = [&](int n) { return std::string(reinterpret_cast<const char*>(good), static_cast<size_t>(n)); };
        std::string version = from(size);
        version[0] = 2;
        std::string longer = from(size) + std::string(1, '\0');
        std::string countSays3 = from(size);
        countSays3[1] = 3;
        std::string zeroItems = Bytes({ 1, 0 });
        std::string nineItems = Bytes({ 1, 9 }) + std::string(9 * 8, '\x01');
        std::string zeroCount = from(size);
        zeroCount[2 + 4] = 0;   // 첫 칸 개수 = 0
        std::string negativeCount = from(size);
        negativeCount[2 + 8 + 7] = static_cast<char>(0x80);   // 둘째 칸 개수의 맨 윗바이트 → 음수
        std::string zeroId = from(size);
        zeroId[2] = 0;          // 첫 칸 번호 = 0
        const Case cases[] = {
            { version, "모르는 판(2)" },
            { longer, "칸 수보다 한 바이트 긺" },
            { from(size - 1), "칸 수보다 한 바이트 짧음" },
            { countSays3, "칸 수 3 인데 두 칸뿐" },
            { zeroItems, "칸 0 개짜리 판 — 빈 값만 「없음」" },
            { nineItems, "칸 수 9" },
            { zeroCount, "개수 0" },
            { negativeCount, "개수 음수" },
            { zeroId, "아이템 번호 0" },
            { Bytes({ 1 }), "1바이트" },
            { std::string(MailAttach::MAX_BYTES + 1, '\x01'), "상한 초과 길이" },
        };
        for (const Case& c : cases)
        {
            MailAttach::Items out;
            out.count = 7;
            const bool ok = MailAttach::Read(c.bytes.data(), static_cast<int>(c.bytes.size()), out);
            Check(!ok && out.count == 0, std::string("첨부 불량 → false · 칸 0 개 — ") + c.what);
        }
        MailAttach::Items out;
        Check(MailAttach::Read(good, size, out) && out.count == 2 && out.items[1].itemId == 6 && out.items[1].count == 2, "불량을 만든 원본은 읽힌다(대조군)");
    }
}

int main()
{
    TestRoundTrip();
    TestLoneSurrogate();
    TestBadUtf8();
    TestCapacity();
    TestCellLength();
    TestControl();
    TestAttachRoundTrip();
    TestAttachBad();
    return TestHelp::Finish("mail_wire_test");
}
