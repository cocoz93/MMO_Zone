// 입장 대기 명단 시험 — 게임 없이 표만 돌린다. 시각은 시험이 넘긴다(실제로 2분을 안 기다린다)
#include <Identity/SecureRandom.h>
#include <cstring>
#include <string>

#include "EnterGate.h"
#include "TestHelp.h"

using TestHelp::Check;

namespace
{
    constexpr int TTL_SEC = 120;
    constexpr int64_t TTL_MS = int64_t{ TTL_SEC } * 1000;
    constexpr int64_t KEEP_MS = int64_t{ CEnterGate::EXPIRED_KEEP_SEC } * 1000;
    constexpr int MAX_CLIENTS = 10;

    struct Code
    {
        char bytes[CEnterGate::CODE_BYTES]{};
    };

    Code MakeCode(char first)
    {
        Code code;
        code.bytes[0] = first;
        code.bytes[15] = first;
        return code;
    }

    CEnterGate MakeGate()
    {
        CEnterGate gate;
        gate.SetTtlSec(TTL_SEC);
        return gate;
    }

    const CharacterId CHAR_A{ 1000000000001 };
    const CharacterId CHAR_B{ 1000000000002 };
    const AccountId   ACC_A{ 11 };
    const AccountId   ACC_B{ 22 };

    void TestConsume()
    {
        std::printf("-- 입장: 일회용\n");
        CEnterGate gate = MakeGate();
        const Code code = MakeCode('a');
        Check(gate.AddNotice(code.bytes, CHAR_A, ACC_A, 0, MAX_CLIENTS, 1000), "예고를 적음");
        Check(gate.LiveCount() == 1, "살아 있는 번호 1");

        CEnterGate::Ticket ticket;
        Check(gate.Consume(code.bytes, 1000, ticket) == EnterCodeResult::Ok, "처음 → Ok");
        Check(ticket.characterId == CHAR_A && ticket.accountId == ACC_A, "캐릭터·계정 번호가 예고 그대로");
        Check(gate.LiveCount() == 0 && gate.StoredCount() == 0, "입장하면 지워짐");

        CEnterGate::Ticket second;
        Check(gate.Consume(code.bytes, 1000, second) == EnterCodeResult::NotFound, "같은 번호 두 번 → NotFound");
        Check(second.characterId == NO_CHARACTER, "NotFound 면 결과를 안 채움");
    }

    void TestShape()
    {
        std::printf("-- 칸 모양\n");
        CEnterGate gate = MakeGate();
        CEnterGate::Ticket ticket;

        Code empty;
        Check(CEnterGate::IsEmptyCode(empty.bytes), "전부 0 → 빈 번호");
        Check(gate.Consume(empty.bytes, 0, ticket) == EnterCodeResult::BadSize, "빈 번호 → BadSize(받아 줄지는 부르는 쪽)");

        Code tail = MakeCode('a');
        tail.bytes[16] = 1;
        Check(!CEnterGate::IsEmptyCode(tail.bytes), "뒤 칸에 값 → 빈 번호 아님");
        Check(gate.Consume(tail.bytes, 0, ticket) == EnterCodeResult::BadSize, "16바이트 뒤에 값 → BadSize");

        Code tailOnly;
        tailOnly.bytes[63] = 1;
        Check(gate.Consume(tailOnly.bytes, 0, ticket) == EnterCodeResult::BadSize, "앞이 비고 뒤만 값 → BadSize");

        const Code unknown = MakeCode('z');
        Check(gate.Consume(unknown.bytes, 0, ticket) == EnterCodeResult::NotFound, "모양은 맞는데 없는 번호 → NotFound");
    }

    void TestExpire()
    {
        std::printf("-- 만료·보관\n");
        CEnterGate gate = MakeGate();
        const Code code = MakeCode('a');
        CEnterGate::Ticket ticket;
        gate.AddNotice(code.bytes, CHAR_A, ACC_A, 0, MAX_CLIENTS, 0);

        gate.Sweep(TTL_MS - 1);
        Check(gate.LiveCount() == 1, "만료 1ms 전 → 아직 살아 있음");

        gate.Sweep(TTL_MS);
        Check(gate.LiveCount() == 0, "만료 순간 → 살아 있는 수에서 빠짐");
        Check(gate.StoredCount() == 1, "만료 순간 → 보관 중");
        Check(gate.Consume(code.bytes, TTL_MS, ticket) == EnterCodeResult::Expired, "만료 뒤 → Expired");
        Check(gate.Consume(code.bytes, TTL_MS + 5, ticket) == EnterCodeResult::Expired, "Expired 는 지우지 않아 또 Expired");
        Check(ticket.characterId == NO_CHARACTER, "Expired 면 결과를 안 채움");

        gate.Sweep(TTL_MS + KEEP_MS - 1);
        Check(gate.StoredCount() == 1, "보관 끝 1ms 전 → 아직 있음");
        gate.Sweep(TTL_MS + KEEP_MS);
        Check(gate.StoredCount() == 0, "보관 끝 → 지워짐");
        Check(gate.Consume(code.bytes, TTL_MS + KEEP_MS, ticket) == EnterCodeResult::NotFound, "보관 뒤 → NotFound");

        // Sweep 을 안 돌았어도 시간이 판정한다 — 게이지 주기 사이에 온 입장
        CEnterGate noSweep = MakeGate();
        noSweep.AddNotice(code.bytes, CHAR_A, ACC_A, 0, MAX_CLIENTS, 0);
        Check(noSweep.Consume(code.bytes, TTL_MS, ticket) == EnterCodeResult::Expired, "쓸어내기 전이어도 시간이 지났으면 Expired");
    }

    void TestOnePerAccount()
    {
        std::printf("-- 계정당 하나\n");
        CEnterGate gate = MakeGate();
        const Code first = MakeCode('a'), second = MakeCode('b'), other = MakeCode('c');
        gate.AddNotice(first.bytes, CHAR_A, ACC_A, 0, MAX_CLIENTS, 0);
        gate.AddNotice(other.bytes, CHAR_B, ACC_B, 0, MAX_CLIENTS, 0);
        gate.AddNotice(second.bytes, CHAR_B, ACC_A, 0, MAX_CLIENTS, 100);
        Check(gate.LiveCount() == 2, "같은 계정 새 예고 → 번호 수는 그대로(계정 둘)");

        CEnterGate::Ticket ticket;
        Check(gate.Consume(first.bytes, 200, ticket) == EnterCodeResult::NotFound, "앞 번호 → NotFound");
        Check(gate.Consume(second.bytes, 200, ticket) == EnterCodeResult::Ok, "새 번호 → Ok");
        Check(ticket.characterId == CHAR_B, "새 번호의 캐릭터");

        CEnterGate::Ticket theirs;
        Check(gate.Consume(other.bytes, 200, theirs) == EnterCodeResult::Ok, "다른 계정의 번호는 그대로(반대 방향)");
        Check(theirs.accountId == ACC_B, "다른 계정 번호");
    }

    void TestCapacity()
    {
        std::printf("-- 정원\n");
        CEnterGate gate = MakeGate();
        const Code a = MakeCode('a'), b = MakeCode('b'), c = MakeCode('c');

        Check(!gate.AddNotice(a.bytes, CHAR_A, ACC_A, MAX_CLIENTS, MAX_CLIENTS, 0), "접속자만으로 정원 → 거절");
        Check(gate.LiveCount() == 0, "거절이면 안 적힘");
        Check(gate.AddNotice(a.bytes, CHAR_A, ACC_A, MAX_CLIENTS - 2, MAX_CLIENTS, 0), "두 자리 남음 → 받음");
        Check(gate.AddNotice(b.bytes, CHAR_B, ACC_B, MAX_CLIENTS - 2, MAX_CLIENTS, 0), "한 자리 남음 → 받음");
        Check(!gate.AddNotice(c.bytes, CHAR_B, AccountId{ 33 }, MAX_CLIENTS - 2, MAX_CLIENTS, 0), "접속자 + 명단 = 정원 → 거절");

        // 정원에 닿았어도 같은 계정이 다시 고르면 자리를 바꿀 뿐이다
        Check(gate.AddNotice(c.bytes, CHAR_A, ACC_A, MAX_CLIENTS - 2, MAX_CLIENTS, 10), "같은 계정 새 예고는 정원에 안 걸림");
        Check(gate.LiveCount() == 2, "자리를 바꿔도 수는 그대로");

        // 거절됐으면 앞 번호가 그대로여야 한다
        CEnterGate full = MakeGate();
        CEnterGate::Ticket ticket;
        full.AddNotice(a.bytes, CHAR_A, ACC_A, 0, MAX_CLIENTS, 0);
        Check(!full.AddNotice(b.bytes, CHAR_B, ACC_A, MAX_CLIENTS + 1, MAX_CLIENTS, 0), "넘친 접속자 → 같은 계정이어도 거절");
        Check(full.Consume(a.bytes, 0, ticket) == EnterCodeResult::Ok, "거절이면 앞 번호가 살아 있음");

        // 만료된 번호는 자리를 안 차지한다 — 예고를 적을 때 먼저 쓸어낸다
        CEnterGate old = MakeGate();
        old.AddNotice(a.bytes, CHAR_A, ACC_A, MAX_CLIENTS - 1, MAX_CLIENTS, 0);
        Check(!old.AddNotice(b.bytes, CHAR_B, ACC_B, MAX_CLIENTS - 1, MAX_CLIENTS, TTL_MS - 1), "만료 전 → 자리 차지");
        Check(old.AddNotice(b.bytes, CHAR_B, ACC_B, MAX_CLIENTS - 1, MAX_CLIENTS, TTL_MS), "만료 뒤 → 자리 비움");
    }

    void TestResultNames()
    {
        std::printf("-- 갈래 이름\n");
        // 시험은 값이 아니라 개수를 본다 — 줄이 지표 라벨이다
        std::string joined;
        for (int i = 0; i < static_cast<int>(EnterCodeResult::COUNT); ++i)
            joined += std::string(EnterCodeResultName(static_cast<EnterCodeResult>(i))) + ",";
        Check(joined == "ok,bad_size,not_found,expired,", "갈래 넷의 라벨: " + joined);
    }
}

int main()
{
    TestConsume();
    TestShape();
    TestExpire();
    TestOnePerAccount();
    TestCapacity();
    TestResultNames();
    return TestHelp::Finish("enter_gate_test");
}
