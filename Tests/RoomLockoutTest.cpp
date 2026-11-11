// 보스방 잠금 열(characters.room_lockouts)의 바이트 형식 시험 — 왕복 · 빈 값 · 불량
#include <cstdint>
#include <cstring>
#include <vector>

#include "RoomLockoutBlob.h"
#include "TestHelp.h"

using TestHelp::Check;
using namespace RoomLockoutBlob;

namespace
{
    constexpr size_t BYTES_MAX = HEADER_BYTES + ENTRY_BYTES * MAX_SLOTS;

    void TestRoundTrip()
    {
        Lockout items[MAX_SLOTS];
        for (int i = 0; i < MAX_SLOTS; ++i)
            items[i] = Lockout{ 100 + i, int64_t{ 1794375083000 } + i };

        uint8_t bytes[BYTES_MAX];
        const size_t written = PackLockouts(items, MAX_SLOTS, bytes, sizeof(bytes));
        Check(written == BYTES_MAX, "칸 16개 = 194바이트");
        Check(BYTES_MAX == 194, "상한이 열 크기 안");

        Lockout out[MAX_SLOTS];
        int n = 0;
        Check(UnpackLockouts(bytes, written, out, MAX_SLOTS, n) == Unpack::OK, "왕복 → OK");
        Check(n == MAX_SLOTS, "왕복 칸 수");
        bool same = true;
        for (int i = 0; i < MAX_SLOTS; ++i)
            same = same && out[i].mapId == items[i].mapId && out[i].untilMs == items[i].untilMs;
        Check(same, "왕복 값이 같다");
    }

    void TestLayout()
    {
        const Lockout one{ 3, int64_t{ 0x0102030405060708 } };
        uint8_t bytes[BYTES_MAX];
        const size_t written = PackLockouts(&one, 1, bytes, sizeof(bytes));
        const uint8_t expected[] = { 1, 1, 3, 0, 0, 0, 8, 7, 6, 5, 4, 3, 2, 1 };
        Check(written == sizeof(expected) && std::memcmp(bytes, expected, sizeof(expected)) == 0, "바이트 배치 = 판 · 칸 수 · 맵 LE · 시각 LE");
    }

    void TestEmpty()
    {
        uint8_t bytes[BYTES_MAX];
        Check(PackLockouts(nullptr, 0, bytes, sizeof(bytes)) == 0, "빈 목록 → 길이 0");
        Lockout out[MAX_SLOTS];
        int n = 7;
        Check(UnpackLockouts(bytes, 0, out, MAX_SLOTS, n) == Unpack::EMPTY, "길이 0 → EMPTY");
        Check(n == 0, "EMPTY 면 칸 수 0");
    }

    void TestBad()
    {
        const Lockout one{ 3, 1000 };
        uint8_t bytes[BYTES_MAX];
        const size_t written = PackLockouts(&one, 1, bytes, sizeof(bytes));
        Lockout out[MAX_SLOTS];
        int n = 0;

        std::vector<uint8_t> copy(bytes, bytes + written);
        copy[0] = 2;
        Check(UnpackLockouts(copy.data(), copy.size(), out, MAX_SLOTS, n) == Unpack::BAD, "판 번호 틀림 → BAD");

        copy.assign(bytes, bytes + written);
        copy[1] = 0;
        Check(UnpackLockouts(copy.data(), HEADER_BYTES, out, MAX_SLOTS, n) == Unpack::BAD, "머리만 있고 칸 수 0 → BAD");

        Check(UnpackLockouts(bytes, written - 1, out, MAX_SLOTS, n) == Unpack::BAD, "길이 모자람 → BAD");
        copy.assign(bytes, bytes + written);
        copy.push_back(0);
        Check(UnpackLockouts(copy.data(), copy.size(), out, MAX_SLOTS, n) == Unpack::BAD, "길이 넘침 → BAD");
        Check(UnpackLockouts(bytes, 1, out, MAX_SLOTS, n) == Unpack::BAD, "머리 반쪽 → BAD");

        Lockout oneSlot[1];
        Lockout two[2] = { Lockout{ 3, 1 }, Lockout{ 4, 2 } };
        const size_t twoLen = PackLockouts(two, 2, bytes, sizeof(bytes));
        Check(UnpackLockouts(bytes, twoLen, oneSlot, 1, n) == Unpack::BAD, "칸 수가 받을 자리를 넘음 → BAD");
    }

    void TestCap()
    {
        const Lockout one{ 3, 1000 };
        uint8_t bytes[HEADER_BYTES + ENTRY_BYTES];
        Check(PackLockouts(&one, 1, bytes, sizeof(bytes) - 1) == 0, "자리가 모자라면 0");
        Check(PackLockouts(&one, 1, bytes, sizeof(bytes)) == sizeof(bytes), "딱 맞으면 쓴다");
    }
}

int main()
{
    TestRoundTrip();
    TestLayout();
    TestEmpty();
    TestBad();
    TestCap();
    return TestHelp::Finish("room_lockout_test");
}
