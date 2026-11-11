// 월드 서버 파티 규칙 시험 — 오프라인 규칙 표 · 복원 합치기. 서버를 안 띄운다
#include <algorithm>
#include <cstring>
#include <vector>

#include "PartySystem.h"
#include "TestHelp.h"
#include "WorldMetrics.h"
#include "WorldPresence.h"

using TestHelp::Check;

namespace
{
    constexpr int64_t SETTLE_MS = 5000;
    const SessionId ZONE{ 11 };

    struct Noticed { CharacterId to; PartyNotice code; int64_t who; };
    struct Asked { CharacterId to; uint32_t id; PartyAskKind kind; };

    // 알림을 받아 적기만 한다 — 시험은 개수와 번호를 본다
    class CRecorder : public IPartyEvents
    {
    public:
        std::vector<Noticed>     notices;
        std::vector<Asked>       asks;
        std::vector<CharacterId> askEnds;
        std::vector<CharacterId> nones;
        int infos = 0;
        int members = 0;

        void Clear() { notices.clear(); asks.clear(); askEnds.clear(); nones.clear(); infos = 0; members = 0; }
        int Count(PartyNotice code) const
        {
            return static_cast<int>(std::count_if(notices.begin(), notices.end(), [code](const Noticed& n) { return n.code == code; }));
        }
        int CountTo(CharacterId to, PartyNotice code) const
        {
            return static_cast<int>(std::count_if(notices.begin(), notices.end(), [=](const Noticed& n) { return n.to == to && n.code == code; }));
        }

        void OnPartyAsk(CharacterId to, uint32_t id, PartyAskKind kind, const LinkPartyPerson&, const LinkPartyPerson&, uint32_t) override { asks.push_back({ to, id, kind }); }
        void OnPartyAskEnd(CharacterId to, uint32_t, PartyNotice) override { askEnds.push_back(to); }
        void OnPartyNotice(CharacterId to, PartyNotice code, const LinkPartyPerson& who) override { notices.push_back({ to, code, who.characterId }); }
        void OnPartyInfo(const Party&) override { ++infos; }
        void OnPartyNone(CharacterId to) override { nones.push_back(to); }
        void OnPartyMember(const Party&, const LinkPartyMemberRow&) override { ++members; }
    };

    struct Bench
    {
        CWorldPresence presence;
        CWorldMetrics  metrics;
        CRecorder      events;
        CPartySystem   party{ presence, metrics, events, SETTLE_MS };

        Bench() { presence.Register(ZONE, ZoneServerId{ 1 }, 100, "ws"); }

        CharacterId Connect(int64_t raw)
        {
            LinkOnlineEntry entry{};
            entry.characterId = raw;
            entry.level = 1;
            entry.name[0] = static_cast<char16_t>(u'가' + raw);
            presence.Enter(ZONE, entry);
            party.Online(CharacterId{ raw });
            return CharacterId{ raw };
        }
        void Disconnect(CharacterId id)
        {
            presence.Leave(ZONE, id);
            party.Offline(id);
        }
        // me 가 부르고 받는 사람이 수락한다 — 답할 요청은 가장 최근 것
        void Invite(CharacterId me, CharacterId target)
        {
            party.Request(me, target, 0);
            party.Answer(target, events.asks.back().id, true, 0);
        }
        // 파티장 first 와 나머지를 맺는다
        void Make(std::vector<CharacterId> people)
        {
            for (size_t i = 1; i < people.size(); ++i)
                Invite(people[0], people[i]);
            events.Clear();
        }
        bool Online(CharacterId who) const
        {
            const Party* p = party.FindParty(who);
            if (p == nullptr)
                return false;
            for (const LinkPartyMemberRow& row : p->members)
            {
                if (row.who.characterId == ToInt(who))
                    return row.online != 0;
            }
            return false;
        }
    };

    const CharacterId A{ 1 }, B{ 2 }, C{ 3 }, D{ 4 };

    void TestOffline()
    {
        Bench b;
        b.Connect(1); b.Connect(2); b.Connect(3);
        b.Make({ A, B, C });
        b.Disconnect(C);
        Check(b.party.FindParty(C) != nullptr && !b.Online(C), "끊겨도 파티에 남고 오프라인으로 표시된다");
        Check(b.events.infos == 1, "끊기면 명단을 한 번 보낸다");
        Check(b.events.Count(PartyNotice::LEFT) == 0, "끊김은 떠남 알림을 안 낸다");
        Check(b.party.FindParty(A)->members.size() == 3, "오프라인도 인원에 든다");

        b.events.Clear();
        b.Connect(3);
        Check(b.Online(C), "다시 들어오면 온라인으로 돌아온다");
        Check(b.events.infos == 1, "돌아오면 명단을 한 번 보낸다");
    }

    void TestCancel()
    {
        Bench b;
        b.Connect(1); b.Connect(2);
        b.party.Request(A, B, 0);
        b.Disconnect(B);
        Check(b.events.CountTo(A, PartyNotice::CANCELLED) == 1, "받을 사람이 끊기면 누른 사람에게 취소를 알린다");
        Check(b.events.askEnds.empty(), "끊긴 사람에게는 창 닫기를 안 보낸다");
        b.events.Clear();
        b.Connect(2);
        b.party.Request(B, A, 0);
        b.Disconnect(B);
        Check(b.events.askEnds.size() == 1 && b.events.askEnds[0] == A, "누른 사람이 끊기면 받는 사람의 창을 닫는다");
    }

    void TestLeaderOfflineTakeover()
    {
        // 파티장의 일을 시도한 파티원이 파티장을 맡는다 — 초대 · 내보내기 · 넘기기 · 해산 전수
        {
            Bench b;
            b.Connect(1); b.Connect(2); b.Connect(3); b.Connect(4);
            b.Make({ A, B, C });
            b.Disconnect(A);
            b.events.Clear();
            b.party.Request(B, D, 0);
            Check(b.party.FindParty(B)->leader == B, "초대 — 파티장을 맡는다");
            Check(b.events.Count(PartyNotice::LEADER) == 2, "초대 — 접속 중인 파티원 둘에게 파티장 알림");
            Check(b.events.asks.size() == 1 && b.events.asks[0].to == D && b.events.asks[0].kind == PartyAskKind::INVITE, "초대 — 추천이 아니라 초대가 간다");
        }
        {
            Bench b;
            b.Connect(1); b.Connect(2); b.Connect(3);
            b.Make({ A, B, C });
            b.Disconnect(A);
            b.events.Clear();
            b.party.Kick(B, C);
            Check(b.party.FindParty(C) == nullptr, "내보내기 — 처리된다");
            Check(b.events.Count(PartyNotice::LEADER) >= 1, "내보내기 — 파티장 알림");
        }
        {
            Bench b;
            b.Connect(1); b.Connect(2); b.Connect(3);
            b.Make({ A, B, C });
            b.Disconnect(A);
            b.party.SetLeader(B, C);
            Check(b.party.FindParty(B)->leader == C, "넘기기 — 맡은 뒤 넘긴다");
        }
        {
            Bench b;
            b.Connect(1); b.Connect(2); b.Connect(3);
            b.Make({ A, B, C });
            b.Disconnect(A);
            b.party.Disband(B);
            Check(b.party.FindParty(B) == nullptr && b.party.FindParty(C) == nullptr, "해산 — 처리된다");
        }
        {
            Bench b;   // 파티장이 접속 중이면 못 맡는다
            b.Connect(1); b.Connect(2); b.Connect(3);
            b.Make({ A, B, C });
            b.party.Kick(B, C);
            Check(b.events.CountTo(B, PartyNotice::NOT_LEADER) == 1 && b.party.FindParty(A)->leader == A, "파티장이 접속 중이면 NOT_LEADER");
            b.events.Clear();
            b.party.Request(B, CharacterId{ 9 }, 0);
            Check(b.events.CountTo(B, PartyNotice::NO_TARGET) == 1, "없는 사람은 NO_TARGET");
        }
        {
            Bench b;   // 파티장이 접속 중이면 파티원의 초대는 추천이다
            b.Connect(1); b.Connect(2); b.Connect(4);
            b.Make({ A, B });
            b.party.Request(B, D, 0);
            Check(b.events.asks.size() == 1 && b.events.asks[0].to == A && b.events.asks[0].kind == PartyAskKind::SUGGEST, "접속 중인 파티장에게 추천이 간다");
        }
    }

    void TestTakeLeader()
    {
        Bench b;
        b.Connect(1); b.Connect(2); b.Connect(3);
        b.Make({ A, B, C });
        b.party.TakeLeader(B);
        Check(b.party.FindParty(B)->leader == A, "파티장이 접속 중이면 못 맡는다");
        b.Disconnect(A);
        b.party.TakeLeader(B);
        Check(b.party.FindParty(B)->leader == B, "파티장이 오프라인이면 맡는다");
    }

    void TestApplyToOfflineLeader()
    {
        Bench b;
        b.Connect(1); b.Connect(2); b.Connect(3);
        b.Make({ A, B });
        b.Disconnect(A);
        b.events.Clear();
        b.party.Request(C, B, 0);
        Check(b.events.CountTo(C, PartyNotice::TARGET_BUSY) == 1, "파티장이 오프라인이면 가입 신청은 TARGET_BUSY");
        Check(b.events.asks.empty(), "요청이 만들어지지 않는다");
    }

    void TestOfflineTargets()
    {
        {
            Bench b;
            b.Connect(1); b.Connect(2); b.Connect(3);
            b.Make({ A, B, C });
            b.Disconnect(C);
            b.party.Kick(A, C);
            Check(b.party.FindParty(C) == nullptr, "오프라인 파티원을 내보낼 수 있다");
            Check(b.party.FindParty(A) != nullptr && b.party.FindParty(A)->members.size() == 2, "남은 사람은 그대로");
        }
        {
            Bench b;
            b.Connect(1); b.Connect(2); b.Connect(3);
            b.Make({ A, B, C });
            b.Disconnect(C);
            b.events.Clear();
            b.party.SetLeader(A, C);
            Check(b.events.CountTo(A, PartyNotice::TARGET_BUSY) == 1 && b.party.FindParty(A)->leader == A, "오프라인 파티원에게는 못 넘긴다");
        }
    }

    void TestAllOffline()
    {
        Bench b;
        b.Connect(1); b.Connect(2);
        b.Make({ A, B });
        b.Disconnect(A);
        Check(b.party.FindParty(B) != nullptr, "한 명이 접속 중이면 파티가 남는다");
        b.events.Clear();
        b.Disconnect(B);
        Check(b.party.FindParty(A) == nullptr && b.party.FindParty(B) == nullptr, "전원 오프라인이면 파티를 지운다");
        Check(b.events.notices.empty() && b.events.nones.empty() && b.events.infos == 0, "받을 사람이 없어 아무것도 안 보낸다");
        Check(b.metrics._party.active.load() == 0 && b.metrics._party.members.load() == 0, "지표 게이지가 0 으로 돌아온다");
    }

    void TestCapacity()
    {
        Bench b;
        std::vector<CharacterId> people;
        for (int64_t i = 1; i <= 8; ++i)
            people.push_back(b.Connect(i));
        b.Make(people);
        b.Disconnect(people[7]);
        b.Connect(20);
        b.events.Clear();
        b.party.Request(A, CharacterId{ 20 }, 0);
        Check(b.events.CountTo(A, PartyNotice::FULL) == 1, "오프라인도 정원에 든다");
    }

    void TestOneLeftCountsOffline()
    {
        {
            Bench b;   // 접속 중 하나 · 오프라인 하나 남으면 둘이라 해산이 아니다
            b.Connect(1); b.Connect(2); b.Connect(3);
            b.Make({ A, B, C });
            b.Disconnect(C);
            b.party.Leave(B);
            Check(b.party.FindParty(A) != nullptr && b.party.FindParty(A)->members.size() == 2, "오프라인을 넣어 둘이면 해산하지 않는다");
        }
        {
            Bench b;   // 나가서 오프라인 한 명만 남으면 해산
            b.Connect(1); b.Connect(2);
            b.Make({ A, B });
            b.Disconnect(B);
            b.party.Leave(A);
            Check(b.party.FindParty(A) == nullptr && b.party.FindParty(B) == nullptr, "한 명만 남으면 해산");
        }
        {
            Bench b;   // 접속 중인 사람이 나가고 오프라인만 둘 남으면 지운다
            b.Connect(1); b.Connect(2); b.Connect(3);
            b.Make({ A, B, C });
            b.Disconnect(B);
            b.Disconnect(C);
            b.party.Leave(A);
            Check(b.party.FindParty(B) == nullptr && b.metrics._party.active.load() == 0, "오프라인만 남으면 파티를 지운다");
        }
    }

    void TestLeaderLeaves()
    {
        Bench b;
        b.Connect(1); b.Connect(2); b.Connect(3);
        b.Make({ A, B, C });
        b.party.Leave(A);
        Check(b.party.FindParty(B)->leader == B, "파티장이 나가면 가입 순서 다음 사람");
        Check(b.events.nones.size() == 1 && b.events.nones[0] == A, "나간 사람에게 파티 없음");
    }

    LinkPartyRestoreRow Row(int32_t partyId, int64_t id, uint8_t order, bool leader)
    {
        LinkPartyRestoreRow row{};
        row.partyId = partyId;
        row.isLeader = leader ? 1 : 0;
        row.joinOrder = order;
        row.state.characterId = id;
        row.state.hp = 10;
        row.state.maxHp = 20;
        row.state.level = 3;
        row.state.mapId = 7;
        row.state.visibleChannel = 2;
        return row;
    }

    void TestDecide()
    {
        const auto all = [](CharacterId) { return true; };
        const auto notTwo = [](CharacterId id) { return ToInt(id) != 2; };

        RestoreDecision d = CPartySystem::DecideRestore({}, all);
        Check(d.disband && d.rows.empty(), "줄이 없으면 해산");

        d = CPartySystem::DecideRestore({ Row(1, 1, 0, true) }, all);
        Check(d.disband && d.rows.size() == 1, "한 명이면 해산");

        d = CPartySystem::DecideRestore({ Row(1, 3, 2, false), Row(1, 1, 0, false), Row(1, 2, 1, true) }, all);
        Check(!d.disband && d.rows.size() == 3, "셋이면 세운다");
        Check(d.rows[0].state.characterId == 1 && d.rows[1].state.characterId == 2 && d.rows[2].state.characterId == 3, "가입 순서로 정렬");
        Check(d.leaderIndex == 1, "파티장 줄의 자리");

        d = CPartySystem::DecideRestore({ Row(1, 1, 0, false), Row(1, 2, 1, true), Row(1, 3, 2, false) }, notTwo);
        Check(d.rows.size() == 2 && d.leaderIndex == 0, "접속 중이 아닌 파티장은 빠지고 첫 사람이 맡는다");

        d = CPartySystem::DecideRestore({ Row(1, 1, 0, true), Row(1, 2, 1, false) }, notTwo);
        Check(d.disband && d.rows.size() == 1, "접속 중인 사람만 세어 한 명이면 해산");
    }

    void TestRestore()
    {
        {
            Bench b;   // 모아서 세운다 — 마지막 줄 뒤 대기
            b.Connect(1); b.Connect(2);
            b.party.Restore(Row(5, 1, 0, true), 0);
            b.party.Tick(4000);
            b.party.Restore(Row(5, 2, 1, false), 4000);
            b.party.Tick(SETTLE_MS);
            Check(b.party.FindParty(A) == nullptr, "마지막 줄이 기다림을 늘린다");
            Check(b.party.IsRestoring(A), "모으는 중");

            b.party.Request(A, B, 4100);
            Check(b.events.CountTo(A, PartyNotice::NO_WORLD) == 1, "복원 중인 사람의 요청은 NO_WORLD");
            b.events.Clear();

            b.party.Tick(4000 + SETTLE_MS);
            const Party* party = b.party.FindParty(A);
            Check(party != nullptr && party->id == 5 && party->leader == A && party->members.size() == 2, "같은 번호로 세운다");
            Check(party != nullptr && party->members[1].mapId == 7 && party->members[1].visibleChannel == 2 && party->members[1].hp == 10, "증언한 상태를 쓴다");
            Check(b.events.infos == 1, "세우면 명단을 보낸다");
            Check(!b.party.IsRestoring(A), "복원이 끝난다");
            Check(b.metrics._party.active.load() == 1 && b.metrics._party.members.load() == 2, "지표에 든다");
        }
        {
            Bench b;   // 복원 중인 상대에게는 TARGET_BUSY
            b.Connect(1); b.Connect(2);
            b.party.Restore(Row(5, 2, 0, true), 0);
            b.party.Request(A, B, 0);
            Check(b.events.CountTo(A, PartyNotice::TARGET_BUSY) == 1, "복원 중인 상대는 TARGET_BUSY");
        }
        {
            Bench b;   // 한 명만 증언 — 해산
            b.Connect(1);
            b.party.Restore(Row(5, 1, 0, true), 0);
            b.party.Tick(SETTLE_MS);
            Check(b.party.FindParty(A) == nullptr, "한 명이면 세우지 않는다");
            Check(b.events.nones.size() == 1 && b.events.CountTo(A, PartyNotice::DISBANDED) == 1, "파티 없음과 해산을 보낸다");
            Check(!b.party.IsRestoring(A), "복원이 끝난다");
        }
        {
            Bench b;   // 월드 서버가 이미 아는 사람 — 상태만 받는다
            b.Connect(1); b.Connect(2);
            b.Make({ A, B });
            b.party.Restore(Row(b.party.FindParty(A)->id, 1, 0, true), 0);
            Check(b.events.members == 1 && !b.party.IsRestoring(A), "소속이 있으면 상태만 반영");
        }
        {
            Bench b;   // 파티는 있는데 이 사람이 없다 — 끊긴 사이 내보내졌다
            b.Connect(1); b.Connect(2); b.Connect(3);
            b.Make({ A, B });
            const int32_t id = b.party.FindParty(A)->id;
            b.party.Restore(Row(id, 3, 2, false), 0);
            Check(b.events.nones.size() == 1 && b.events.nones[0] == C, "월드 서버가 정본 — 파티 없음을 보낸다");
            Check(b.party.FindParty(C) == nullptr, "파티에 안 들어간다");
        }
        {
            Bench b;   // 복원한 번호 뒤로 새 번호가 간다
            b.Connect(1); b.Connect(2); b.Connect(3); b.Connect(4);
            b.party.Restore(Row(40, 1, 0, true), 0);
            b.party.Restore(Row(40, 2, 1, false), 0);
            b.party.Tick(SETTLE_MS);
            b.Invite(C, D);
            Check(b.party.FindParty(C)->id == 41, "새 파티 번호는 복원한 번호 다음부터");
        }
    }

    void TestExpire()
    {
        Bench b;
        b.Connect(1); b.Connect(2);
        b.party.Request(A, B, 0);
        b.party.Tick(CPartySystem::ASK_TIMEOUT_MS - 1);
        Check(b.events.askEnds.empty(), "시간 전에는 안 끝난다");
        b.party.Tick(CPartySystem::ASK_TIMEOUT_MS);
        Check(b.events.askEnds.size() == 1 && b.events.CountTo(A, PartyNotice::EXPIRED) == 1, "시간이 지나면 끝난다");
        Check(b.metrics._party.pending.load() == 0, "요청 표가 빈다");
    }
}

int main()
{
    TestOffline();
    TestCancel();
    TestLeaderOfflineTakeover();
    TestTakeLeader();
    TestApplyToOfflineLeader();
    TestOfflineTargets();
    TestAllOffline();
    TestCapacity();
    TestOneLeftCountsOffline();
    TestLeaderLeaves();
    TestDecide();
    TestRestore();
    TestExpire();
    return TestHelp::Finish("world_party_test");
}
