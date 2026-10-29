#include "NpcManager.h"

#include "Zone.h"            // 나를 든 존 — 입장·맵 번호
#include "Npc.h"             // unique_ptr<CNpc> 를 지우려면 완전한 타입이 필요하다
#include "NpcTable.h"        // NpcDef — 깔 때 NPC 표의 줄을 넘긴다
#include "GameDataFiles.h"   // 쥔 기획 데이터에서 NPC 표를 꺼낸다
#include <Common/ErrorLog.h> // 깐 로그 한 줄

CNpcManager::CNpcManager(CZone& zone, const CGameDataFiles& gameData)
    : _zone(zone)
    , _gameData(gameData)
{
}

CNpcManager::~CNpcManager() = default;

void CNpcManager::Tick(INpcEvents& events)
{
    if (_spawned)
        return;
    _spawned = true;

    for (const NpcDef& def : _gameData.Npcs().All())
    {
        if (def.mapId != _zone.GetMapId())
            continue;

        auto owned = std::make_unique<CNpc>(def);
        CNpc* npc = owned.get();                  // 빌려 쓰는 포인터 — 소유는 아래에서 목록으로 넘어간다
        npc->_actorId = events.AllocActorId();    // 단일 증가 카운터라 플레이어·몬스터와 충돌하지 않는다

        // 자리는 표 그대로(표가 읽을 때 맵 안인지 봤다). 방향·정지 상태·섹터 등록·주민 목록 추가는 EnterZoneAt 이 전부 한다.
        _zone.EnterZoneAt(npc, static_cast<float>(def.x), static_cast<float>(def.y));   // 실패가 없다 — CZone::EnterZone 주석
        _npcs.push_back(std::move(owned));

        // 주변에 알린다 — 첫 틱이라 대개 아무도 안 보고 있고, 나중에 걸어온 사람은 섹터 진입 때 받는다(셋째 목록도 시야 경로가 돈다).
        events.OnNpcSpawn(&_zone, npc);
    }

    if (!_npcs.empty())
        SLOG_INFO("[Npc] zone {} (맵 {}) — NPC {}명", _zone.GetZoneId(), _zone.GetMapId(), _npcs.size());
}
