// ==========================================================================
// CMonsterHandler — 몬스터: 존마다 몬스터 틱과 몬스터 알림(등장 · 사라짐 · 번호 부탁) (게임 서버 나누기 10 단계 · C4 · USE_MONSTER)
//
// [책임]
//  - 틱 1-1 — 존마다 몬스터 관리자의 Tick(리젠 · 판단)을 부르고, 그때 넘길 묶음(MonsterContext)을 만든다
//  - 몬스터 알림(IMonsterEvents) — 번호 발급(세션 허브의 발급기로) · 등장(표시 문자 · 주변 CREATE · 동기화 기준점) · 사라짐(효과 목록 · 존에서 빼기)
//
// [경계]
//  몬스터 코드(깔기 · 판단 · 벽 앞 재판단 · 지우기)는 존마다 든 CMonsterManager. 공격·스킬은 플레이어와 같은 규칙(CCombatSystem · CSkillSystem)을 쓴다.
//  보내기는 CSightBroadcaster, 존에서 빼기는 배치. 죽은 몬스터 지우기는 CCombatHandler::OnDeath(처치 알림) 끝이다.
//
// [모르는 것]
//  보상 · 파티 · 가방 · 입장. 세션과 패킷 분기 표.
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"     // USE_MONSTER · USE_COMBAT · USE_SKILL · USE_PARTY — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_MONSTER

#include "MonsterManager.h"  // IMonsterEvents · MonsterContext

class CZone;
class CMonster;
class CSessionHub;
class CSightBroadcaster;
class CPlayerPlacement;
class CMapManager;
class CGameMonitor;
#if USE_COMBAT
class CCombatSystem;
class CBuffSystem;
#endif
#if USE_SKILL
class CSkillSystem;
#endif

// 몬스터 알림은 비공개 상속 — 꽂는 것은 이 클래스 자신뿐이다(MakeMonsterContext 가 *this 를 싣는다).
class CMonsterHandler : private IMonsterEvents
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(세션 허브 · CSightBroadcaster · 배치 · 규칙 셋 · 맵 관리자 · 게임 지표 · 틱 번호)보다
    //   뒤에 이것을 선언해야 한다(멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    CMonsterHandler(CSessionHub& sessionHub, CSightBroadcaster& sightBroadcaster, CPlayerPlacement& placement,
#if USE_COMBAT
                    CCombatSystem& combatSystem, CBuffSystem& buffSystem,
#endif
#if USE_SKILL
                    CSkillSystem& skillSystem,
#endif
                    CMapManager& mapManager, CGameMonitor& gameMonitor, const uint64_t& frameCount)
        : _sessionHub(sessionHub), _sightBroadcaster(sightBroadcaster), _placement(placement)
#if USE_COMBAT
        , _combatSystem(combatSystem), _buffSystem(buffSystem)
#endif
#if USE_SKILL
        , _skillSystem(skillSystem)
#endif
        , _mapManager(mapManager), _gameMonitor(gameMonitor), _frameCount(frameCount) {}

    // ── 몬스터 (USE_MONSTER) ──
    // 몬스터 코드(깔기·판단·벽 앞 재판단)는 존마다 든 CMonsterManager 에 있다.
    //   여기는 틱마다 그걸 부르고, 알림을 받아 송신·번호 발급만 한다.

    // 게임 루프 1-1) — 좌표 갱신(2) 바로 앞. 존마다 관리자 Tick(처음이면 깔고, 몬스터마다 판단).
    //   순서가 뒤면 AI 가 정한 방향이 한 틱씩 밀린다.
    void TickMonsters();

private:
    // 관리자에 넘길 묶음 — 부를 때마다 만든다. 담긴 것이 전부 이 객체가 든 것의 참조라 만드는 값이 싸다.
    //   (예전엔 설정값 둘 — 존당 마릿수·뱀 속도 — 이 Start() 전에 바뀌어 틱마다 만들었다. 둘 다 몬스터 표·스폰 표로 갔다)
    MonsterContext MakeMonsterContext();

    // 몬스터 알림과 번호 부탁 (IMonsterEvents) — 뜻은 MonsterManager.h.
    int32_t AllocActorId() override;                                         // = 세션 허브의 발급기 — 번호 공간이 플레이어와 하나
    void OnMonsterSpawn(CZone* zone, CMonster* monster) override;            // 주변에 CREATE(SPAWN) · 동기화 기준점을 지금 자리로
    void OnMonsterVanish(CZone* zone, CMonster* monster) override;           // 방 클리어 — 효과 목록에서 빼고 존에서 뺀다(주변 DELETE) · 죽은 몬스터 지우기 앞과 같은 길

    CSessionHub&       _sessionHub;         // 번호 발급기(플레이어와 같은 번호 공간)
    CSightBroadcaster& _sightBroadcaster;   // 등장 CREATE · 걷기·멈춤 알림(IZoneEvents — 묶음에 싣는다)
    CPlayerPlacement&  _placement;          // 사라짐 — 존에서 빼기
#if USE_COMBAT
    CCombatSystem&     _combatSystem;       // 공격 흐름 — 빌린 것(게임 서버가 든다) · 묶음에 싣는다
    CBuffSystem&       _buffSystem;         // 사라짐 — 효과 목록에서 빼기
#endif
#if USE_SKILL
    CSkillSystem&      _skillSystem;        // 스킬 규칙 — 빌린 것 · 묶음에 싣는다
#endif
    CMapManager&       _mapManager;         // 존마다 돌기
    CGameMonitor&      _gameMonitor;        // 몬스터 · 방 지표(묶음에 싣는다)
    const uint64_t&    _frameCount;         // 게임 루프의 틱 번호 — 빌린 것, 읽기만
};

#endif // USE_MONSTER
