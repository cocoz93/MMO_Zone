#include "MonsterHandler.h"

#if USE_MONSTER

#include "SessionHub.h"
#include "SightBroadcaster.h"
#include "PlayerPlacement.h"
#include "CombatSystem.h"     // USE_COMBAT — 묶음에 싣는다
#include "BuffSystem.h"       // USE_COMBAT — 사라짐 때 효과 목록에서 빼기
#include "SkillSystem.h"      // USE_SKILL — 묶음에 싣는다
#include "MapManager.h"
#include "Zone.h"
#include "Monster.h"
#include "MonsterTable.h"     // MonsterDef — 등장 때 정의 번호(_def.monsterId)
#include "GameMonitor.h"
#include <Protocol/Protocol.h> // SpawnReason

// ==========================================================================
// 몬스터 (USE_MONSTER)
//   몬스터 코드(깔기·판단·벽 앞 재판단·죽은 몬스터 지우기)는 존마다 든 CMonsterManager 에 있다(MonsterManager.h).
//   여기는 틱마다 그걸 부르고, 알림을 받아 송신·번호 발급만 한다. 죽은 몬스터를 존에서 빼고 지우라 부르는 것은 전투 조각의 OnDeath 끝(CCombatHandler).
// ==========================================================================

MonsterContext CMonsterHandler::MakeMonsterContext()
{
    return MonsterContext{ *this, _sightBroadcaster, _gameMonitor   // 걷기·멈춤(IZoneEvents)은 시야 조각이 받는다
#if USE_COMBAT
                         , _combatSystem
#endif
#if USE_SKILL
                         , _skillSystem
#endif
    };
}

// 게임 루프 1-1) — 존마다 관리자 Tick. 리젠 주기가 된 구역을 채우고(처음 깔기 포함), 몬스터마다 판단한다.
void CMonsterHandler::TickMonsters()
{
    const MonsterContext ctx = MakeMonsterContext();
    _mapManager.ForEachZone([&](CZone* zone)
    {
        zone->GetMonsterManager().Tick(ctx, _frameCount);
    });
}

// ── 몬스터 알림과 번호 부탁 (IMonsterEvents) ── 보내기는 시야 조각(BroadcastSpawn) · 존에서 빼기는 배치(RemoveFromZone).

int32_t CMonsterHandler::AllocActorId()
{
    return _sessionHub.AllocActorId();   // 단일 증가 카운터라 플레이어와 충돌하지 않는다
}

void CMonsterHandler::OnMonsterSpawn(CZone* zone, CMonster* monster)
{
    // 표시 문자 · 정의 번호 — 클라가 사람 대신 몬스터로 그리고(부류 DISPLAY_MONSTER) 어느 몬스터인지 고르는(defId = monsterId) 패킷 값.
    //   플레이어의 CalcDisplayChar 처럼 게임 서버 쪽(이 몬스터 조각)이 넣는다 — 존 층(몬스터 생성자)은 이 값을 모른다(10-06).
    //   깔 때 존 입장 직후 이 콜백을 곧바로 부르므로, 아래 CREATE 보다 늦는 일이 없다.
    //   그림 이름은 서버가 모른다 — 웹이 같은 몬스터 표(MonsterTable.csv sprite 칸)를 읽어 defId 로 찾는다.
    static_assert(CMonsterTable::MAX_MONSTER_ID <= UINT16_MAX, "몬스터 번호가 defId(u16)에 안 들어간다");
    monster->_displayChar = DISPLAY_MONSTER;
    monster->_defId       = static_cast<uint16_t>(monster->_def.monsterId);

    // 이미 그 자리를 보고 있던 플레이어에게 CREATE(SPAWN) 가 간다 — 가득 찬 체력을 싣는다(ActorSnapshot).
    //   보는 사람이 없으면 아무에게도 안 가고, 나중에 걸어온 사람은 섹터 진입 때 받는다.
    _sightBroadcaster.BroadcastSpawn(zone, monster, SpawnReason::SPAWN);

    // 델타 동기화 기준점을 지금 자리로 — 방금 보낸 CREATE 가 이 좌표를 실었으니 「클라가 아는 자리」가 여기다.
    //   플레이어의 입장·존 이동 때와 같은 꼴(SpawnPlayer·RecvZoneChange). 존 층은 이 값을 모른다 — 10-06 몬스터 관리자에서 옮김.
    //   안 맞추면 옛 자리와의 차이로 보정 패킷(MOVE_SYNC)이 한 번 더 나갈 뿐, 위치가 틀리지는 않는다.
    monster->_lastSyncX = monster->_x;
    monster->_lastSyncY = monster->_y;
}

// 사라짐 — 방 클리어로 남은 몬스터를 지울 때 하나마다(CMonsterManager::VanishAll). 죽은 몬스터 지우기 앞(전투 조각의 OnDeath 3·8)과 같은 두 일:
//   효과 목록에서 빼고(이 뒤에 관리자가 해제한다 — 목록에 주소가 남으면 안 된다) · 존에서 뺀다(주변 DELETE · 같은 틱 이동자 · 대기열 — 플레이어 퇴장과 같은 길).
//   죽음(DEATH)은 안 보낸다 — 죽은 게 아니라 사라진 것이다.
void CMonsterHandler::OnMonsterVanish(CZone* zone, CMonster* monster)
{
#if USE_COMBAT
    _buffSystem.ClearAll(monster);
#endif
    _placement.RemoveFromZone(zone, monster);
#if USE_PARTY
    _gameMonitor._room._minionsCleared.Inc();   // 클리어는 방 갈래만 건다(OnDeath) — 사라짐은 곧 방 클리어로 지운 수다
#endif
}

#endif // USE_MONSTER
