#include "StatSync.h"

#if USE_COMBAT

#include <algorithm>

#include "Character.h"         // As<CPlayer>
#include "Player.h"            // _statDirty · 체력 컴포넌트 · 존 번호
#include "SelfNotifier.h"      // MY_STATS · ACTOR_MP(본인)
#include "SightBroadcaster.h"  // ACTOR_STATS 주변 방송
#include "MapManager.h"        // 존 찾기
#include "Zone.h"
#include "PacketBuild.h"       // MakeActorStats
#include "SerialBuffer.h"

using namespace PacketBuild;

void CStatSync::OnStatsChanged(CCharacter* character, uint8_t changed)
{
    if (CPlayer* player = As<CPlayer>(character))
        Mark(player, changed);
}

void CStatSync::Mark(CPlayer* player, uint8_t changed)
{
    if (changed == 0)
        return;
    if (player->_statDirty == 0)
        _dirty.push_back(player);   // Clear 로 0 이 된 뒤 다시 표시되면 두 번 들어간다 — 두 번째는 Flush 가 비트 0 으로 건너뛴다
    player->_statDirty |= changed;
}

void CStatSync::Clear(CPlayer* player, uint8_t sent)
{
    player->_statDirty &= static_cast<uint8_t>(~sent);
}

void CStatSync::Forget(CPlayer* player)
{
    player->_statDirty = 0;
    _dirty.erase(std::remove(_dirty.begin(), _dirty.end(), player), _dirty.end());   // 두 번 들어간 경우까지 전부
}

void CStatSync::Flush()
{
    for (CPlayer* player : _dirty)
    {
        const uint8_t bits = player->_statDirty;
        if (bits == 0)
            continue;   // 입장·존 이동이 이미 보냈다(Clear) · 두 번 들어간 두 번째
        player->_statDirty = 0;

        if (bits & StatChange::AROUND_HP)
        {
            // 존이 없으면(떠나는 중 — 같은 틱이면 Forget 이 이미 뺐다) 주변 몫만 건너뛴다. excludeSelf=false — 본인 막대도 같은 장으로
            if (CZone* zone = _mapManager.GetZone(player->_zoneId))
                _sightBroadcaster.BroadcastAroundSector(zone, player,
                    MakeActorStats(player->_actorId, player->_health.Hp(), player->_health.MaxHp()), false);
        }
        if (bits & StatChange::SELF_STATS)
            _selfNotifier.SendMyStats(player);
#if USE_SKILL
        if (bits & StatChange::SELF_MP)
            _selfNotifier.SendActorMp(player);
#endif
    }
    _dirty.clear();
}

#endif // USE_COMBAT
