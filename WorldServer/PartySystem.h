#pragma once
// ==========================================================================
// CPartySystem — 파티 규칙 (월드 서버)
//
// [책임]
//  - 파티 맺기 · 가입 · 나가기 · 내보내기 · 파티장 넘기기 · 해산과 요청 표(초대 · 가입 신청 · 추천 — 시간 지남)의 규칙
//  - 오프라인 규칙 · 끊겼다 다시 붙은 존 서버의 소속 복원
//  - 정한 것을 지표(CWorldMetrics)로 센다
//  - 보내기는 하지 않는다 — CPartyFlow 가 알림(IPartyEvents)을 받아 서버 간 패킷으로 보낸다
//
// [규칙 — 10-16~17 · 11-11 사용자 결정]
//  최대 8명 · 받는 사람은 늘 수락해야 들어간다 · 파티원이 부른 사람은 파티장 승인을 먼저 거친다(추천) ·
//  파티 밖 사람의 가입 신청은 파티장이 수락하면 바로 들어간다 · 파티 없는 사람이 초대해 수락되면 초대한 사람이 파티장인 새 파티 ·
//  파티장이 빠지면 가입 순서 다음 사람이 파티장 · 한 명만 남으면 해산 · 저장하지 않는다.
//  요청은 받는 쪽 하나 · 보내는 쪽 하나씩 — 받는 사람이 답하는 중이면 TARGET_BUSY, 내가 보낸 것이 아직이면 SELF_BUSY.
//
// [끊겨도 파티에 남는다 — 11-11] 파티는 캐릭터 번호를 든다. 접속이 끊기면 오프라인으로 표시할 뿐 빼지 않는다.
//  채널 이동도 잠깐 오프라인이 되는 것일 뿐이라 전용 코드와 타이머가 없다. 파티가 줄어드는 길은 직접 나가기 · 내보내기 · 해산 · 전원 오프라인뿐이다.
//  오프라인도 정원 8명과 「한 명만 남으면 해산」의 인원에 든다.
//
// [요청이 무효가 되는 규칙 하나] 처지가 바뀐 사람(파티에 듦 · 빠짐 · 파티장이 되거나 그만둠 · 오프라인이 됨)이 낀 요청은 전부 무효(CANCELLED)다.
//  자리마다 「이 요청이 아직 말이 되나」를 따로 따지면 한 분기를 빠뜨려도 화면이 멀쩡하다 — 처지를 바꾸는 자리가 이 규칙 하나를 부른다.
//  수락 순간에는 인원 상한만 다시 본다(인원은 처지가 아니라 다른 사람의 가입으로 바뀐다).
//
// [복원] 파티는 DB 에 없다. 월드 서버가 재시작하거나 존 서버 연결이 끊겨 파티가 지워졌으면, 다시 붙은 존 서버들이 자기 사람의 소속을 증언하고
//  월드 서버가 파티 번호로 모은다. 존 서버가 몇 대인지 모르므로 마지막 증언 뒤 restoreSettleMs 를 기다려 확정한다.
//  모으는 동안 그 사람의 요청은 NO_WORLD 로 거절한다 — 파티를 모르는 채 처리하면 없는 사람으로 치고 새 파티를 맺어 버린다.
//
// [계층 경계] 장부(CWorldPresence)와 지표만 안다. 네트워크 · 패킷은 모른다. 로직 스레드 전용이라 락이 없다.
// ==========================================================================

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Identity/GameIds.h>
#include <Protocol/Protocol.h>             // PartyNotice · PartyAskKind · PARTY_MAX_MEMBERS
#include <Protocol/ServerLinkProtocol.h>

#include "WorldMetrics.h"

class CWorldPresence;

// 파티 하나 — 알림이 명단을 읽어 패킷을 만든다. 바꾸는 쪽은 CPartySystem 뿐이다
struct Party
{
    int32_t     id = 0;
    CharacterId leader = NO_CHARACTER;
    std::vector<LinkPartyMemberRow> members;   // 가입 순서(파티장 포함) — 파티장이 빠지면 이 순서로 넘긴다
};

// 파티가 정한 것을 내보낸다 — CPartyFlow 가 구현한다. 순수 가상이라 빠뜨리면 컴파일이 안 된다
class IPartyEvents
{
public:
    virtual ~IPartyEvents() = default;

    virtual void OnPartyAsk(CharacterId to, uint32_t requestId, PartyAskKind kind, const LinkPartyPerson& from, const LinkPartyPerson& about, uint32_t remainMs) = 0;
    virtual void OnPartyAskEnd(CharacterId to, uint32_t requestId, PartyNotice why) = 0;
    virtual void OnPartyNotice(CharacterId to, PartyNotice code, const LinkPartyPerson& who) = 0;
    virtual void OnPartyInfo(const Party& party) = 0;                                      // 접속 중인 파티원 전원에게
    virtual void OnPartyNone(CharacterId to) = 0;                                          // 파티가 없어진 한 사람에게
    virtual void OnPartyMember(const Party& party, const LinkPartyMemberRow& row) = 0;     // 접속 중인 파티원 전원에게
};

// 복원 합치기의 결과 — 값을 돌려주는 함수라 갈래를 전수로 시험한다
struct RestoreDecision
{
    bool disband = false;                 // 남은 사람이 한 명 이하
    int  leaderIndex = -1;                // 합친 뒤 순서의 파티장 자리
    std::vector<LinkPartyRestoreRow> rows;   // 접속 중인 사람만, 가입 순서로
};

class CPartySystem
{
public:
    // 요청을 기다리는 시간 — 30초(10-16 사용자 「30초」 기본값 승인)
    static constexpr int64_t ASK_TIMEOUT_MS = 30'000;

    // restoreSettleMs = 복원 줄이 멈춘 뒤 기다릴 시간. 재접속 주기보다 길어야 늦게 붙는 존 서버의 사람이 안 빠진다
    CPartySystem(const CWorldPresence& presence, CWorldMetrics& metrics, IPartyEvents& events, int64_t restoreSettleMs);

    // 사람을 파티에 부른다 — 두 사람의 처지로 초대 · 신청 · 추천 중 하나. target 이 접속 중이 아니면 NO_TARGET
    void Request(CharacterId me, CharacterId target, int64_t nowMs);
    void Answer(CharacterId me, uint32_t requestId, bool accept, int64_t nowMs);
    void Leave(CharacterId me);
    void Kick(CharacterId me, CharacterId target);
    void SetLeader(CharacterId me, CharacterId target);
    void Disband(CharacterId me);
    void TakeLeader(CharacterId me);   // 파티장이 오프라인일 때만 바꾼다

    // 장부가 바뀐 뒤에 부른다
    void Online(CharacterId id);    // 3007 · 3004 — 소속이 있으면 온라인으로 돌리고 명단을 보낸다
    void Offline(CharacterId id);   // 3008 · 존 서버 연결 끊김 — 낀 요청 취소 · 오프라인 표시 · 전원 오프라인이면 파티를 지운다

    void ApplyState(const LinkPartyStateRow& state);   // 파티에 있으면 값을 받아 전원에게 알린다
    void Restore(const LinkPartyRestoreRow& row, int64_t nowMs);

    void Tick(int64_t nowMs);   // 요청 시간 지남 · 복원 확정

    const Party* FindParty(CharacterId id) const;
    bool IsRestoring(CharacterId id) const { return _restoringPeople.count(ToInt(id)) != 0; }
    int  OnlineMembers() const;

    // 복원 합치기 — online(캐릭터 번호)이 true 인 줄만 남긴다. 순수 함수
    template <typename IsOnline>
    static RestoreDecision DecideRestore(std::vector<LinkPartyRestoreRow> rows, IsOnline online);

private:
    struct PendingRequest
    {
        uint32_t        id = 0;
        PartyAskKind    kind = PartyAskKind::INVITE;
        LinkPartyPerson origin{};   // 화면에서 누른 사람 — 「보낸 요청은 한 번에 하나」를 이 칸으로 센다(추천에서 이어진 초대도 추천한 사람)
        LinkPartyPerson from{};     // INVITE = 초대한 사람(파티장 · 파티 없는 사람) · APPLY = 신청한 사람 · SUGGEST = 추천한 파티원
        LinkPartyPerson to{};       // 답할 사람 — INVITE = 초대받은 사람 · APPLY · SUGGEST = 파티장
        LinkPartyPerson about{};    // SUGGEST = 초대하자는 사람 · INVITE = 추천한 파티원(없으면 0)
        int64_t         expireMs = 0;
    };

    struct Restoring
    {
        std::vector<LinkPartyRestoreRow> rows;
        int64_t settleAtMs = 0;
    };

    Party* PartyOf(CharacterId id);
    static LinkPartyMemberRow* FindRow(Party& party, CharacterId id);
    static bool IsOnline(const Party& party, CharacterId id);
    LinkPartyPerson PersonOf(CharacterId id) const;   // 장부 → 없으면 파티 줄 → 없으면 번호만

    bool HasReceived(CharacterId id) const;   // 답할 요청이 있나(받는 사람 하나씩)
    bool HasSent(CharacterId id) const;       // 내가 누른 요청이 아직 있나(보내는 사람 하나씩)

    void Notice(CharacterId to, PartyNotice code, const LinkPartyPerson& who);
    void Ask(const LinkPartyPerson& to, PartyAskKind kind, const LinkPartyPerson& origin, const LinkPartyPerson& from, const LinkPartyPerson& about, int64_t nowMs);

    // 요청 표에서 i 번째를 뺀다 — 끝난 까닭을 센다. 알림은 부르는 쪽 몫
    PendingRequest TakeRequest(size_t i, PartyRequestEnd why);
    // 처지가 바뀐 사람이 낀 요청을 전부 무효로 — skip(접속이 끊긴 사람)에게는 아무것도 안 보낸다
    void CancelRequestsOf(const LinkPartyPerson& changed, CharacterId skip);

    // 파티장의 일을 시도한 사람이 파티장인가 — 파티장이 오프라인이면 그 사람으로 바꾸고 true
    bool EnsureLeader(Party& party, CharacterId me);

    LinkPartyMemberRow NewRow(CharacterId id) const;
    void Join(Party& party, CharacterId id);
    void Create(CharacterId leader, CharacterId first);
    void RemoveMember(Party& party, CharacterId id, PartyLeave why);
    void DisbandParty(Party& party);
    void EraseQuietly(Party& party);   // 전원 오프라인 — 받을 사람이 없어 알림 없이 지운다
    void ChangeLeader(Party& party, CharacterId newLeader);
    void BroadcastNotice(const Party& party, PartyNotice code, const LinkPartyPerson& who);

    void Expire(int64_t nowMs);
    void SettleRestores(int64_t nowMs);

    const CWorldPresence& _presence;
    CWorldMetrics&        _metrics;
    IPartyEvents&         _events;
    int64_t               _restoreSettleMs;

    std::unordered_map<int32_t, Party>   _parties;   // 파티 번호 → 파티(노드 기반이라 다른 파티를 넣고 빼도 주소가 그대로다)
    std::unordered_map<int64_t, int32_t> _memberOf;  // 캐릭터 번호 → 파티 번호
    std::vector<PendingRequest>          _requests;  // 요청 표 — 사람마다 보낸 것 하나 · 받은 것 하나라 접속자 수로 묶인다
    std::unordered_map<int32_t, Restoring> _restoring;       // 복원 중인 파티 번호 → 모인 줄
    std::unordered_set<int64_t>            _restoringPeople; // 복원 중인 캐릭터 번호
    int32_t  _nextPartyId   = 1;
    uint32_t _nextRequestId = 1;   // 0 은 안 쓴다(돌아 감기면 1 부터)
};

template <typename IsOnlineFn>
RestoreDecision CPartySystem::DecideRestore(std::vector<LinkPartyRestoreRow> rows, IsOnlineFn online)
{
    RestoreDecision out;
    for (const LinkPartyRestoreRow& row : rows)
    {
        if (online(CharacterId{ row.state.characterId }))
            out.rows.push_back(row);
    }
    // 가입 순서 — 같은 파티의 줄은 존 서버마다 따로 오지만 순서 칸은 같은 사본에서 나왔다
    for (size_t i = 1; i < out.rows.size(); ++i)
    {
        const LinkPartyRestoreRow current = out.rows[i];
        size_t j = i;
        while (j > 0 && out.rows[j - 1].joinOrder > current.joinOrder)
        {
            out.rows[j] = out.rows[j - 1];
            --j;
        }
        out.rows[j] = current;
    }

    if (out.rows.size() <= 1)
    {
        out.disband = true;
        return out;
    }
    out.leaderIndex = 0;   // 파티장이 빠졌으면 가입 순서 다음 사람
    for (size_t i = 0; i < out.rows.size(); ++i)
    {
        if (out.rows[i].isLeader != 0)
        {
            out.leaderIndex = static_cast<int>(i);
            break;
        }
    }
    return out;
}
