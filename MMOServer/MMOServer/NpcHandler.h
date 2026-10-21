// ==========================================================================
// CNpcHandler — NPC: 존마다 NPC 틱과 NPC 알림(등장 · 번호 부탁) (NPC·상점 플랜 4판 · 조각 C9 · 토글 없음)
//
// [책임]
//  - 틱 1-1b — 존마다 NPC 관리자의 Tick(첫 틱이면 그 맵의 NPC 를 깐다)을 부른다
//  - NPC 알림(INpcEvents) — 번호 발급(세션 허브의 발급기로) · 등장(부류 값 DISPLAY_NPC · 정의 번호 = npcId · 주변 CREATE · 지표)
//
// [경계]
//  NPC 객체·깔기는 존마다 든 CNpcManager. 보내기는 시야 조각. 사고팔기 요청은 상점 조각(USE_SHOP) — 이 조각은 상점을 모른다.
//  토글이 없다 — NPC 는 맵·포털처럼 세계 데이터라, 상점을 끈 빌드에도 서 있다(말 걸기만 된다).
//
// [모르는 것]
//  전투 · 보상 · 가방 · 상점 · 입장. 세션과 패킷 분기 표.
// ==========================================================================
#pragma once

#include <cstdint>

#include "NpcManager.h"   // INpcEvents

class CZone;
class CNpc;
class CSessionHub;
class CSightBroadcaster;
class CMapManager;
class CGameMonitor;

// NPC 알림은 비공개 상속 — 꽂는 것은 이 클래스 자신뿐이다(TickNpcs 가 *this 를 넘긴다).
class CNpcHandler : private INpcEvents
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(세션 허브 · 시야 조각 · 맵 관리자 · 게임 지표)보다 뒤에 이것을 선언해야 한다.
    CNpcHandler(CSessionHub& sessionHub, CSightBroadcaster& sightBroadcaster, CMapManager& mapManager, CGameMonitor& gameMonitor)
        : _sessionHub(sessionHub), _sightBroadcaster(sightBroadcaster), _mapManager(mapManager), _gameMonitor(gameMonitor) {}

    // 게임 루프 1-1b) — 존마다 NPC 관리자 Tick. 처음 도는 존이면 NPC 를 깔고, 그 뒤로는 곧바로 끝난다(존마다 bool 하나).
    void TickNpcs();

private:
    // NPC 알림과 번호 부탁 (INpcEvents) — 뜻은 NpcManager.h.
    int32_t AllocActorId() override;                       // = 세션 허브의 발급기 — 번호 공간이 플레이어·몬스터와 하나
    void OnNpcSpawn(CZone* zone, CNpc* npc) override;      // 부류 값 · 정의 번호 · 주변에 CREATE(SPAWN) · 지표

    CSessionHub&       _sessionHub;         // 번호 발급기
    CSightBroadcaster& _sightBroadcaster;   // 등장 CREATE
    CMapManager&       _mapManager;         // 존마다 돌기
    CGameMonitor&      _gameMonitor;        // NPC 지표
};
