// ==========================================================================
// PartySystem — 파티 규칙 (USE_PARTY)
//
// [책임]
//  - 파티 맺기·가입·나가기·내보내기·파티장 넘기기·해산과 요청 표(초대·가입 신청·추천 — 시간 지남)의 규칙
//  - 파티원 상태(체력·최대 체력·레벨·맵)를 지난번 보낸 사본과 견줘 바뀐 줄을 알린다(0.5초마다)
//  - 그 과정에서 정한 것을 지표(CGameMonitor)로 센다 — CBuffSystem 과 같은 꼴
//  - 보내기는 하지 않는다 — CPartyHandler 가 알림(IPartyEvents)을 받아 S2C_PARTY_* 로 보낸다
//
// [규칙 — 10-16~17 사용자 결정, Protocol.h 파티 절 머리말]
//  최대 8명 · 받는 사람은 늘 수락해야 들어간다 · 파티원이 부른 사람은 파티장 승인을 먼저 거친다(추천) ·
//  파티 밖 사람의 가입 신청은 파티장이 수락하면 바로 들어간다 · 파티 없는 사람이 초대해 수락되면 초대한 사람이 파티장인 새 파티 ·
//  파티장이 빠지면 가입 순서 다음 사람이 파티장 · 한 명만 남으면 해산 · 끊기면 바로 빠진다 · 저장하지 않는다.
//  요청은 받는 쪽 하나·보내는 쪽 하나씩 — 받는 사람이 답하는 중이면 TARGET_BUSY, 내가 보낸 것이 아직이면 SELF_BUSY.
//
// [요청이 무효가 되는 규칙 하나] 처지가 바뀐 사람(파티에 듦·빠짐·파티장이 되거나 그만둠)이 낀 요청은 전부 무효(CANCELLED)다.
//  자리마다 「이 요청이 아직 말이 되나」를 따로 따지면 한 갈래를 빠뜨려도 화면이 멀쩡하다 — 처지를 바꾸는 자리가 이 규칙 하나를 부른다.
//  수락 순간에는 인원 상한만 다시 본다(인원은 처지가 아니라 다른 사람의 가입으로 바뀐다).
//
// [왜 따로인가] 파티는 존을 넘는다 — 목록은 게임 서버에 하나(효과 목록 CBuffSystem 과 같은 꼴). 존마다 두면 존을 옮길 때 옮겨 담아야 한다.
//
// [목록은 틱 너머로 주소를 든다] 파티원·요청의 CPlayer* — 사람이 사라지는 길은 퇴장 하나다(CPlayerPlacement::ReleasePlayer → 「떠남」 알림 CGameServer::OnLeavingGame → Forget).
//  죽음·존 이동은 같은 CPlayer 가 그대로라 빼지 않는다. 빠뜨리면 다음 틱이 해제된 메모리를 밟는다.
//
// [계층 경계] CPlayer·CGameMonitor 와 존 번호 → 맵 번호 풀기(CMapManager 정적 함수)만 안다. 세션·패킷·섹터는 모른다 —
//  누른 사람을 주변 9섹터에서 찾는 일은 CPartyHandler::FindPlayerAround 가 하고 찾은 사람(없으면 nullptr)을 넘긴다.
//  객체는 CGameServer 가 멤버로 든다 — _gameMonitor · 알림 받는 _partyHandler 뒤에 선언한다(그 참조를 받는다 — 멤버는 선언 순서로 세워진다).
//
// [와이어 enum 을 그대로 쓴다] 알림·요청 종류(Protocol.h PartyNotice · PartyAskKind)는 정하는 쪽과 와이어가 1:1 이라
//  두 벌을 두면 옮겨 적는 표만 늘어난다(줍기 거절 PickupFail 과 같은 꼴 — GameMonitor.h [사유 enum 의 집]).
//  와이어에 안 실리는 것(빠진 까닭 · 요청이 끝난 까닭)만 여기서 정한다.
// ==========================================================================
#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>

#include "BuildConfig.h"          // USE_PARTY — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include <Protocol/Protocol.h>    // PartyNotice · PartyAskKind · PARTY_MAX_MEMBERS

#if USE_PARTY

class CPlayer;
class CGameMonitor;

// 파티원이 빠진 까닭 — 지표 칸(mmo_party_member_removed_total{why})
enum class PartyLeave : uint8_t
{
    LEAVE,          // 스스로 나감(C2S_PARTY_LEAVE)
    KICKED,         // 파티장이 내보냄
    DISCONNECTED,   // 끊김(ReleasePlayer → Forget)
    DISBANDED,      // 해산(파티장이 해산 · 한 명만 남음)
    COUNT
};

// 요청이 끝난 까닭 — 지표 칸(mmo_party_requests_ended_total{why}). 등식: Σ 요청 = Σ 끝남 + 지금 요청 표 크기
enum class PartyRequestEnd : uint8_t
{
    ACCEPTED,    // 받은 사람이 수락(가입이 됐든 인원 상한에 막혔든 — 답이 온 것)
    DECLINED,    // 받은 사람이 거절
    EXPIRED,     // 시간 지남(ASK_TIMEOUT_FRAMES)
    CANCELLED,   // 무효 — 낀 사람의 처지가 바뀜(위 [요청이 무효가 되는 규칙 하나])
    COUNT
};

// 파티 하나 — 알림(OnPartyInfo)이 파티원 목록을 읽어 패킷을 만든다. 바꾸는 쪽은 CPartySystem 뿐이다.
struct Party
{
    // 지난번 S2C_PARTY_INFO·S2C_PARTY_MEMBER 로 보낸 값 — 틱의 상태 비교(MEMBER_SCAN_FRAMES)가 지금 값과 견준다.
    //   비교로 보는 까닭: 체력이 바뀌는 자리가 여럿이다(피해·물약·부활·레벨업·스킬 비용). 자리마다 알리면 하나를 빠뜨려도
    //   컴파일·화면이 멀쩡하다(Player.h _dbSaved 와 같은 까닭).
    struct Sent
    {
        int32_t hp    = 0;
        int32_t maxHp = 0;
        int32_t level = 0;
        int32_t mapId = -1;
    };
    struct Member
    {
        CPlayer* player = nullptr;
        Sent     sent;
    };

    int32_t             id     = 0;
    CPlayer*            leader = nullptr;
    std::vector<Member> members;   // 가입 순서(파티장 포함) — 파티장이 빠지면 이 순서로 넘긴다
};

// 파티가 정한 것을 내보낸다 — CPartyHandler 가 구현한다(패킷 조립·송신 — 게임 서버 나누기 7 단계). IBuffEvents 와 같은 꼴. ★ 순수 가상이라 빠뜨리면 컴파일이 안 된다.
//   받는 사람(to)은 늘 CPlayer — 몬스터는 파티에 없다.
class IPartyEvents
{
public:
    virtual ~IPartyEvents() = default;

    // 답할 요청이 왔다(S2C_PARTY_ASK) — about 은 없으면 nullptr
    virtual void OnPartyAsk(CPlayer* to, uint32_t requestId, PartyAskKind kind, const CPlayer* from, const CPlayer* about, uint32_t remainMs) = 0;
    // 그 요청이 무효가 됐다(S2C_PARTY_ASK_END) — why = EXPIRED · CANCELLED
    virtual void OnPartyAskEnd(CPlayer* to, uint32_t requestId, PartyNotice why) = 0;
    // 알림 한 줄(S2C_PARTY_NOTICE) — actorId = 관련 사람(없으면 0)
    virtual void OnPartyNotice(CPlayer* to, PartyNotice code, int32_t actorId) = 0;
    // 파티 전체(S2C_PARTY_INFO) — party 가 nullptr 이면 파티 없음(빠진 사람에게)
    virtual void OnPartyInfo(CPlayer* to, const Party* party) = 0;
    // 파티원 한 명의 줄(S2C_PARTY_MEMBER) — member 의 지금 값을 읽어 보낸다
    virtual void OnPartyMember(CPlayer* to, const CPlayer* member) = 0;
};

class CPartySystem
{
public:
    // 요청을 기다리는 시간 — 30초. ★ 임시값(10-16 사용자 「30초」 기본값 승인 — 수치는 사람 몫). Protocol.h PartyNotice::EXPIRED 가 이 이름을 가리킨다.
    static constexpr uint64_t ASK_TIMEOUT_FRAMES = 30ull * FRAME_PER_SEC;
    // 파티원 상태 비교 주기 — 12틱(약 0.5초). 파티가 없으면 비교가 바로 끝난다(부하 시험엔 파티가 없다).
    static constexpr uint64_t MEMBER_SCAN_FRAMES = FRAME_PER_SEC / 2;

    CPartySystem(CGameMonitor& gameMonitor, IPartyEvents& events);

    // 화면에서 누른 사람과 파티 — target 은 CPartyHandler 가 누른 사람 주변 9섹터에서 찾은 사람(없으면 nullptr → NO_TARGET).
    //   두 사람의 처지로 초대·가입 신청·추천 중 하나를 요청 표에 넣고 받는 사람에게 묻는다(흐름 = MyDocs/claude-plans/party-plan.md 「흐름」 표).
    void Request(CPlayer* me, CPlayer* target, uint64_t frame);

    // 받은 요청에 답한다 — 내가 받은 요청이 아니면 NO_REQUEST.
    void Answer(CPlayer* me, uint32_t requestId, bool accept, uint64_t frame);

    // 나가기 · 내보내기 · 파티장 넘기기 · 해산 — 뒤 셋은 파티장만. 대상은 우리 파티원 중 actorId 로 찾는다(맵이 달라도 된다).
    void Leave(CPlayer* me);
    void Kick(CPlayer* me, int32_t targetId);
    void SetLeader(CPlayer* me, int32_t targetId);
    void Disband(CPlayer* me);

    // 틱마다 1회(게임 루프 3-2c) — 요청 시간 지남 · 파티원 상태 비교(MEMBER_SCAN_FRAMES 마다). 둘 다 비면 바로 끝난다.
    void Tick(uint64_t frame);

    // 퇴장 — 파티에서 빼고(남은 사람에게 알림) 낀 요청을 지운다. 이 사람에게는 아무것도 안 보낸다(세션이 없다). 두 번 불러도 무해.
    void Forget(CPlayer* player);

    // 알림 한 줄 — 번호별로 세고(_party._notices) 내보낸다. 방 들어가기 거절(파티 없음 · 파티장 아님 · 방 거절 셋)도 CPartyHandler::SendRoomReject 가 이 길로 보낸다 —
    //   보내는 자리가 둘로 갈려도 세는 자리는 여기 하나다
    void Notice(CPlayer* to, PartyNotice code, int32_t actorId);

    // 읽기 — 이 사람의 파티(없으면 nullptr). 방 들어가기 판정(Room.h CheckRoomEntry)이 파티장·파티원 목록을 읽는다.
    //   바꾸는 길은 위 공개 함수뿐이다 — 돌려준 주소는 다음 파티 변화까지만 믿을 것(같은 처리 안에서 읽고 끝낸다)
    const Party* FindParty(const CPlayer* p) const;

private:
    struct PendingRequest
    {
        uint32_t     id          = 0;
        PartyAskKind kind        = PartyAskKind::INVITE;
        CPlayer*     origin      = nullptr;   // 화면에서 누른 사람 — 「보낸 요청은 한 번에 하나」를 이 칸으로 센다(추천에서 이어진 초대도 추천한 사람)
        CPlayer*     from        = nullptr;   // INVITE = 초대한 사람(파티장 · 파티 없는 사람) · APPLY = 신청한 사람 · SUGGEST = 추천한 파티원
        CPlayer*     to          = nullptr;   // 답할 사람 — INVITE = 초대받은 사람 · APPLY·SUGGEST = 파티장
        CPlayer*     about       = nullptr;   // SUGGEST = 초대하자는 사람 · INVITE = 추천한 파티원(없으면 nullptr)
        uint64_t     expireFrame = 0;
    };

    Party* PartyOf(const CPlayer* p);
    static bool IsMember(const Party& party, const CPlayer* p);
    CPlayer* FindMember(Party& party, int32_t actorId);
    bool HasReceived(const CPlayer* p) const;   // 답할 요청이 있나(받는 사람 하나씩)
    bool HasSent(const CPlayer* p) const;       // 내가 누른 요청이 아직 있나(보내는 사람 하나씩)

    void Ask(CPlayer* to, PartyAskKind kind, CPlayer* origin, CPlayer* from, CPlayer* about, uint64_t frame);

    // 요청 표에서 i 번째를 뺀다 — 끝난 까닭을 센다. 알림은 부르는 쪽 몫.
    PendingRequest TakeRequest(size_t i, PartyRequestEnd why);
    // 처지가 바뀐 사람이 낀 요청을 전부 무효로 — 낀 사람 각자에게: 받는 사람은 ASK_END(CANCELLED), 나머지는 NOTICE(CANCELLED, changed).
    //   skip(사라지는 사람)에게는 아무것도 안 보낸다.
    void CancelRequestsOf(CPlayer* changed, const CPlayer* skip);

    // 가입 — 인원은 부르는 쪽이 봤다. 처지가 바뀐 사람의 요청 무효 → 사본 채움 → 전원에게 INFO · JOINED.
    void Join(Party& party, CPlayer* p);
    // 새 파티 — leader 와 first 둘. 둘 다 처지가 바뀐다.
    void Create(CPlayer* leader, CPlayer* first);
    // 빼기 한 곳 — 나감·내보냄·끊김이 같이 쓴다. 한 명 남으면 해산, 파티장이 빠졌으면 넘김. notifyRemoved = 빠진 사람에게 보낼까(끊김이면 false).
    void RemoveMember(Party& party, CPlayer* p, PartyLeave why, bool notifyRemoved);
    // 해산 — 남은 전원에게 INFO(없음) · DISBANDED. 파티를 지운다(party 는 이 뒤 못 쓴다).
    void DisbandParty(Party& party);
    // 파티장 바꾸기 — 두 사람의 처지가 바뀐다(요청 무효) · 전원에게 INFO · LEADER.
    void ChangeLeader(Party& party, CPlayer* newLeader);
    // 전원에게 INFO — 사본(sent)을 지금 값으로 맞춘다(바로 뒤 상태 비교가 같은 값을 또 보내지 않게).
    void BroadcastInfo(Party& party);
    void BroadcastNotice(const Party& party, PartyNotice code, int32_t actorId);

    void Expire(uint64_t frame);
    void ScanMembers();

    static Party::Sent SnapshotOf(const CPlayer* p);

    CGameMonitor& _gameMonitor;
    IPartyEvents& _events;

    std::unordered_map<int32_t, Party> _parties;   // 파티 번호 → 파티(노드 기반이라 다른 파티를 넣고 빼도 주소가 그대로다)
    std::vector<PendingRequest>        _requests;  // 요청 표 — 사람마다 보낸 것 하나·받은 것 하나라 접속자 수로 묶인다
    int32_t  _nextPartyId   = 1;
    uint32_t _nextRequestId = 1;                   // 0 은 안 쓴다(돌아 감기면 1 부터)
    uint64_t _lastScanFrame = 0;
};

#endif // USE_PARTY
