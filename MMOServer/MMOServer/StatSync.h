// ==========================================================================
// CStatSync — 스탯 알림: 바뀐 사람을 표시해 두고 틱 끝에 한 번 보낸다 (USE_COMBAT)
//
// [책임]
//  - 표시 — 스탯이 바뀐 사람과 바뀐 몫(StatChange 비트, CPlayer::_statDirty)을 목록에 모은다. 한 틱에 여러 번 바뀌어도 목록엔 한 번
//  - 보내기 — 틱 끝(Flush)에 표시된 사람마다 비트 하나당 패킷 한 장:
//      AROUND_HP  → S2C_ACTOR_STATS (본인 + 주변 — 남의 머리 위 막대)
//      SELF_STATS → S2C_MY_STATS    (본인만)
//      SELF_MP    → S2C_ACTOR_MP    (본인만 · USE_SKILL)
//  - 표시하는 쪽 둘 — ⑴ CStatSystem::RecalcStats 가 전후를 견줘 알린다(IStatEvents — 레벨업 · 장착 · GM · 효과 걸림·풀림)
//                     ⑵ 지금 값만 바꾸는 핸들러가 직접 Mark(물약 · 스킬 비용 — RecalcStats 를 안 지난다)
//
// [왜 따로인가]
//  전에는 스탯이 바뀌는 자리마다 부르는 쪽이 어떤 패킷을 보낼지 골라 바로 보냈다(전투 · 명령 · 아이템 · 보상 네 핸들러).
//  그래서 한 자리가 하나를 빠뜨려도 컴파일·화면이 멀쩡했다 — 효과 걸림은 MY_STATS 만 보내 최대 체력이 바뀌면 주변이 모를 구조였다.
//  이제 「무엇이 바뀌었나」는 값을 세우는 한 곳이 견주고, 「무엇을 보내나」는 여기 한 곳이 비트로 정한다.
//
// [언제 보내나]
//  게임 루프 3-4(땅 아이템 뒤 · FlushSends 앞, GameServer.cpp). 표시하는 자리(네트워크 · DB 회신 · 몬스터 · 효과 만료)가 전부 앞이라
//  같은 틱 안에 나간다. 그래서 같은 틱의 다른 패킷보다 뒤에 실린다 — 예: 시전 때 「시전 → 맞은 피해 → 시전자 체력·마력」(같은 묶음이라 화면은 같은 순간).
//
// [사람만 담는다]
//  몬스터는 받을 세션이 없고, 주변에 보일 체력이 바뀌는 길(스킬 비용)도 없다(몬스터 스킬은 비용 0 만 허용 — MonsterSkillTable.cpp).
//  격노는 공격력만 바꾼다(본인 전용 값). 그래서 몬스터 알림은 버린다 — 같은 틱에 죽어 해제된 몬스터 주소를 목록이 밟는 일도 이걸로 없다.
//  몬스터 체력·최대 체력을 바꾸는 길이 생기면 그때 담고, 「존 지우기 직전」(OnErasingZone)에 그 존 몫을 빼야 한다.
//
// [수명]
//  목록은 주소를 든다 — 틱 끝에 비우지만, 표시 뒤 같은 틱에 떠나는 사람은 「떠남」(CGameServer::OnLeavingGame)이 Forget 으로 뺀다.
//  입장·존 이동(CPlayerPlacement::NotifyArrival)은 자기 몫을 직접 보내므로 그 뒤 보낸 비트만 지운다(Clear) — 겹쳐 보내지 않게.
//
// [모르는 것]
//  스탯 규칙(CStatSystem) · 패킷 조립(PacketBuild) · DB. 보내기는 본인 알림(CSelfNotifier)과 시야 방송(CSightBroadcaster)에 맡긴다.
// ==========================================================================
#pragma once

#include "BuildConfig.h"   // USE_COMBAT · USE_SKILL — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_COMBAT

#include <cstdint>
#include <vector>

#include "StatSystem.h"    // IStatEvents · StatChange

class CCharacter;
class CPlayer;
class CSelfNotifier;
class CSightBroadcaster;
class CMapManager;

class CStatSync final : public IStatEvents
{
public:
    // ★ 참조만 든다 — 게임 서버는 넘기는 셋보다 뒤에 이것을 선언한다(멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    CStatSync(CSelfNotifier& selfNotifier, CSightBroadcaster& sightBroadcaster, CMapManager& mapManager)
        : _selfNotifier(selfNotifier), _sightBroadcaster(sightBroadcaster), _mapManager(mapManager) {}

    // IStatEvents — 스탯을 세운 뒤 바뀐 몫. 사람이면 Mark, 몬스터는 버린다(헤더 [사람만 담는다]).
    void OnStatsChanged(CCharacter* character, uint8_t changed) override;

    // 바뀐 몫 표시 — 처음 표시될 때만 목록에 넣는다(비트가 0 → 0 아님).
    void Mark(CPlayer* player, uint8_t changed);

    // 이미 보낸 몫 지우기 — 입장·존 이동이 자기 차례에 보낸 뒤 부른다. 목록에는 남아도 된다(Flush 가 비트 0 을 건너뛴다).
    void Clear(CPlayer* player, uint8_t sent);

    // 떠남 — 목록에서 뺀다(같은 틱에 해제되는 주소를 Flush 가 밟지 않게). 목록에 없으면 아무 일 없다.
    void Forget(CPlayer* player);

    // 틱 끝 — 표시된 사람마다 비트대로 보내고 목록을 비운다. 순서는 체력 → 공격력·방어력 → 마력(옛 레벨업·GM 의 순서).
    void Flush();

private:
    CSelfNotifier&     _selfNotifier;       // MY_STATS · ACTOR_MP(본인)
    CSightBroadcaster& _sightBroadcaster;   // ACTOR_STATS(본인 + 주변)
    CMapManager&       _mapManager;         // 주변 방송의 존 찾기

    std::vector<CPlayer*> _dirty;           // 이번 틱에 표시된 사람 — 틱마다 비운다(용량은 남겨 재할당 없음)
};

#endif // USE_COMBAT
