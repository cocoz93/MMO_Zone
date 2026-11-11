// ==========================================================================
// PartyCopy — 존 서버가 드는 파티 사본 (USE_PARTY)
//
// [책임]
//  - 월드 서버가 보낸 명단을 파티 번호로 들고, 그 명단의 이 존 서버 사람에게 CPlayer 주소와 파티 번호(_partyId)를 채운다
//  - 캐릭터 번호 → 이 존 서버의 사람 찾기
//
// [왜 사본을 드나] 파티의 정본은 월드 서버(WorldServer/PartySystem.h)다. 존 서버는 칠 때마다 파티원 거르기(_partyId)와
//  보스방 판정(명단)을 읽는데, 이 둘이 서버 간 왕복을 기다리면 틱이 멈춘다. 사본은 읽기 전용이고 바꾸는 길은 월드 서버의 패킷뿐이다.
//  월드 서버가 끊겨도 사본은 그대로 둔다 — 파티원 거르기와 보스방은 계속 돌고, 바꾸는 요청만 거절된다.
//
// [목록은 틱 너머로 주소를 든다] 사람이 사라지는 길은 퇴장 하나다(CPlayerPlacement::ReleasePlayer → CGameServer::OnLeavingGame → Forget).
//  빠뜨리면 다음 틱이 해제된 메모리를 밟는다. 존 이동 · 죽음은 같은 CPlayer 가 그대로라 안 지운다.
//
// [이 존 서버에 사람이 없는 파티는 안 든다] 볼 일이 없다 — 마지막 사람이 나가면 사본을 지운다.
// ==========================================================================
#pragma once

#include <Identity/GameIds.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "BuildConfig.h"          // USE_PARTY — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include <Protocol/Protocol.h>    // PartyNotice · PARTY_MAX_MEMBERS
#include <Protocol/ServerLinkProtocol.h>

#if USE_PARTY

class CPlayer;

struct Party
{
    struct Member
    {
        // 월드 서버가 아는 값 — 이 존 서버의 사람은 틱의 상태 비교가 지금 값과 견준다.
        //   비교로 보는 까닭: 체력이 바뀌는 자리가 여럿이다(피해 · 물약 · 부활 · 레벨업 · 스킬 비용). 자리마다 알리면 하나를 빠뜨려도
        //   컴파일 · 화면이 멀쩡하다(Player.h _dbSaved 와 같은 까닭).
        LinkPartyMemberRow row{};
        CPlayer*           player = nullptr;   // 이 존 서버에 있으면 주소, 아니면 nullptr
    };

    int32_t             id     = 0;
    CharacterId         leader = NO_CHARACTER;
    std::vector<Member> members;   // 가입 순서(파티장 포함)

    const Member* Find(CharacterId characterId) const;
    bool LeaderOnline() const;
};

class CPartyCopy
{
public:
    void     AddPlayer(CPlayer* player);
    CPlayer* FindPlayer(CharacterId characterId) const;
    Party*   FindParty(const CPlayer* player);

    // 명단으로 사본을 고친다 — 이 존 서버 사람의 파티 번호를 채우고, 명단에서 빠진 사람은 0 으로. 이 존 서버에 사람이 없으면 사본을 지우고 nullptr
    Party* ApplyInfo(int32_t partyId, CharacterId leader, const LinkPartyMemberRow* rows, int count);
    // 파티가 없어진 한 사람
    void ApplyNone(CPlayer* player);
    Party* FindById(int32_t partyId);

    // 퇴장 — 주소를 지운다. 이 존 서버에 사람이 남은 파티면 돌려주고, 아니면 사본을 지우고 nullptr. 두 번 불러도 무해
    Party* Forget(CPlayer* player);
    // 파티가 없어진 사람의 줄을 뺀다(ApplyNone)
    static void RemoveRow(Party& party, CharacterId characterId);
    // 월드 서버가 끊긴 동안의 퇴장 — 줄은 두고 오프라인으로만 표시한다(플랜 5.5 「사본을 그대로 둔다」).
    //   줄을 빼면 가입 순서(줄의 자리)가 당겨져, 복원 때 다른 존 서버가 보낸 줄과 순서가 어긋날 수 있다
    static void MarkOffline(Party& party, CharacterId characterId);
    // 월드 서버가 끊긴 동안 다시 들어온 사람 — 사본에 줄이 남아 있으면 다시 잇고 그 파티를 돌려준다(없으면 nullptr)
    Party* Relink(CPlayer* player);

    std::unordered_map<int32_t, Party>& Parties() { return _parties; }

private:
    void EraseIfNoLocal(int32_t partyId);

    std::unordered_map<int32_t, Party>    _parties;   // 파티 번호 → 사본(노드 기반이라 다른 파티를 넣고 빼도 주소가 그대로다)
    std::unordered_map<int64_t, CPlayer*> _players;   // 캐릭터 번호 → 이 존 서버의 사람
};

#endif // USE_PARTY
