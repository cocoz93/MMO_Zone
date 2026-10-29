#include "NpcHandler.h"

#include "SessionHub.h"
#include "SightBroadcaster.h"
#include "MapManager.h"
#include "Zone.h"
#include "Npc.h"
#include "NpcTable.h"         // NpcDef — 등장 때 정의 번호(_def.npcId)
#include "GameMonitor.h"
#include <Protocol/Protocol.h> // DISPLAY_NPC · SpawnReason

// ==========================================================================
// NPC (토글 없음)
//   NPC 객체·깔기는 존마다 든 CNpcManager 에 있다(NpcManager.h). 여기는 틱마다 그걸 부르고, 알림을 받아 송신·번호 발급만 한다.
// ==========================================================================

// 게임 루프 1-1b) — 존마다 관리자 Tick. 처음 도는 존이면 NPC 를 깐다.
void CNpcHandler::TickNpcs()
{
    _mapManager.ForEachZone([&](CZone* zone)
    {
        zone->GetNpcManager().Tick(*this);
    });
}

int32_t CNpcHandler::AllocActorId()
{
    return _sessionHub.AllocActorId();   // 단일 증가 카운터라 플레이어·몬스터와 충돌하지 않는다
}

void CNpcHandler::OnNpcSpawn(CZone* zone, CNpc* npc)
{
    // 표시 문자 · 정의 번호 — 클라가 사람 대신 NPC 로 그리고(부류 DISPLAY_NPC) 어느 NPC 인지 고르는(defId = npcId) 패킷 값.
    //   존 층(NPC 생성자)은 이 값을 모른다 — CMonsterHandler::OnMonsterSpawn 과 같은 자리.
    //   이름·생김새·인사는 서버가 안 싣는다 — 웹이 같은 NPC 표(NpcTable.csv)를 읽어 번호로 찾는다.
    static_assert(CNpcTable::MAX_NPC_ID <= UINT16_MAX, "NPC 번호가 defId(u16)에 안 들어간다");
    npc->_displayChar = DISPLAY_NPC;
    npc->_defId       = static_cast<uint16_t>(npc->_def.npcId);

    // 이미 그 자리를 보고 있던 플레이어에게 CREATE(SPAWN) — 체력 컴포넌트가 없어 체력 0/0 이 실린다(클라는 막대를 안 그린다).
    _sightBroadcaster.BroadcastSpawn(zone, npc, SpawnReason::SPAWN);

    // 델타 동기화 기준점을 지금 자리로 — 몬스터와 같은 꼴(NPC 는 안 움직여 이 뒤로 바뀌지 않는다)
    npc->_lastSyncX = npc->_x;
    npc->_lastSyncY = npc->_y;

    _gameMonitor._npc._spawned.Inc();
}
