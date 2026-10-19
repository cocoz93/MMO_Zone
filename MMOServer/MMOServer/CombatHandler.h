// ==========================================================================
// CCombatHandler — 전투: 공격·스킬 요청과 공격·시전·효과 알림, 부활 대기열 (게임 서버 나누기 9 단계 · C3 · USE_COMBAT)
//
// [책임]
//  - 받은 패킷 둘의 처리기와 그 등록(RegisterRoutes — 공격 · 스킬(USE_SKILL)) — 컨텐츠 조각이 자기 줄을 등록한다
//  - 공격 알림(ICombatEvents — 휘두름 · 맞음 · 죽음) · 시전 알림(ISkillEvents — 시전 · 되살려라) · 효과 알림(IBuffEvents)을 받아 세계에 적용
//    (주변 방송 · 본인 알림 · 죽은 목록 등록 · 처치 보상 부르기 · 죽은 몬스터 지우기) · 스킬 거절 사유를 와이어 값으로
//  - 부활 대기열(_deadActors) — 죽은 플레이어 담기 · 틱마다 부활(제자리 방송 또는 마을로 옮기기) · 퇴장 때 빼기(RemoveFromDeadActors)
//
// [경계]
//  판정은 규칙에 맡긴다 — 공격 흐름은 CCombatSystem, 스킬은 CSkillSystem, 지속 효과는 CBuffSystem, 쿨타임·피해·부활 시각은 namespace Combat.
//  보상은 처치 보상 조각(CRewardHandler), 옮기기·존에서 빼기는 배치(CPlayerPlacement), 보내기는 세 보내기 조각.
//  규칙 객체 셋과 효과 틱(CBuffSystem::Tick)은 게임 서버가 든다 — 이 조각은 그 참조를 빌려 부르고, 그 알림을 받는다.
//  퇴장 정리는 게임 서버가 배치의 「존에서 빠짐 · 떠남」 알림을 받아 차례대로 나눠 부른다(이 조각엔 부활 대기열 빼기 하나).
//
// [모르는 것]
//  파티·가방·입장 절차. 세션 번호. 패킷 분기 표는 자기 줄을 등록할 때(RegisterRoutes)만 쓴다.
// ==========================================================================
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "BuildConfig.h"     // USE_COMBAT · USE_SKILL · USE_MONSTER · USE_EXP · USE_DROP · USE_PARTY — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_COMBAT

#include "CombatSystem.h"    // ICombatEvents
#include "BuffSystem.h"      // IBuffEvents · BuffKind
#include "SkillSystem.h"     // USE_SKILL — ISkillEvents · SkillDef (헤더 안에서 스스로 토글을 본다)

class CZone;
class CCharacter;
class CPlayer;
class CSerialBuffer;
class CPacketRoutes;
class CSessionHub;
class CSelfNotifier;
class CSightBroadcaster;
class CPlayerPlacement;
class CMapManager;
class CGameMonitor;
#if USE_MONSTER
class CRewardHandler;
#endif

// 알림 셋은 공개 상속 — 게임 서버가 규칙 셋(_combatSystem · _skillSystem · _buffSystem)에 이것을 알림 받는 쪽으로 꽂는다
//   (시야 조각의 IZoneEvents 와 같은 꼴). 알림 함수 자체는 비공개 — 규칙이 인터페이스로만 부른다.
class CCombatHandler
    : public ICombatEvents
    , public IBuffEvents
#if USE_SKILL
    , public ISkillEvents
#endif
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(세 보내기 조각 · 배치 · 처치 보상 조각 · 규칙 셋 · 맵 관리자 · 게임 지표 · 틱 번호)보다
    //   뒤에 이것을 선언해야 한다(멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    //   거꾸로 규칙 셋은 이것보다 앞에 세워지며 이것의 참조를 묶기만 한다(알림 받는 쪽 — 게임 서버가 *this 를 넘기던 것과 같은 꼴).
    //   기본 맵 번호(마을 부활)는 맵 관리자에게 묻는다(CMapManager::GetDefaultMapId — 게임 서버 나누기 14 단계 전에는 게임 서버가 들고 빌려 줬다).
    CCombatHandler(CSessionHub& sessionHub, CSelfNotifier& selfNotifier, CSightBroadcaster& sightBroadcaster, CPlayerPlacement& placement,
#if USE_MONSTER
                   CRewardHandler& rewardHandler,
#endif
                   CCombatSystem& combatSystem, CBuffSystem& buffSystem,
#if USE_SKILL
                   CSkillSystem& skillSystem,
#endif
                   CMapManager& mapManager, CGameMonitor& gameMonitor, const uint64_t& frameCount)
        : _sessionHub(sessionHub), _selfNotifier(selfNotifier), _sightBroadcaster(sightBroadcaster), _placement(placement)
#if USE_MONSTER
        , _rewardHandler(rewardHandler)
#endif
        , _combatSystem(combatSystem), _buffSystem(buffSystem)
#if USE_SKILL
        , _skillSystem(skillSystem)
#endif
        , _mapManager(mapManager), _gameMonitor(gameMonitor), _frameCount(frameCount) {}

    // 받은 패킷 등록 — 공격 · 스킬(USE_SKILL). 게임 서버의 등록(RegisterRoutes)이 부른다. false = 등록 실패(기동 거절).
    bool RegisterRoutes(CPacketRoutes& routes);

    // 부활 대기자 훑기 — 틱마다 1회. 존 전체를 돌지 않으려고 죽은 것만 따로 들고 있는다.
    //   플레이어만 — 몬스터는 되살리지 않는다(죽는 순간 지우고, 스폰이 새로 채운다).
    //   마을(기본 맵)이 아닌 맵에서 죽었으면 마을 시작 구역으로 옮겨 되살린다(배치의 TransferPlayer, ArrivalKind::RESPAWN) — 대기열을 다 돈 뒤에.
    void TickRespawn();

    // 부활 대기열에서 빼기 — 퇴장 경로가 반드시 부른다(배치의 「존에서 빠짐 · 떠남」 알림을 받는 게임 서버의 OnLeavingZone · OnLeavingGame). 안 부르면 다음 틱이 해제된 포인터를 밟는다.
    void RemoveFromDeadActors(CCharacter* player);

private:
    // ── 전투 (USE_COMBAT) ──
    // 공격 흐름은 CCombatSystem(_combatSystem)이 하고, 여기서는 그 알림을 받아 세계에 적용한다
    //   (송신 · 죽은 목록 등록 · 보상).
    void RecvAttack(CPlayer* player, CSerialBuffer* pMsg);

    // 공격 알림 셋 (ICombatEvents) — 뜻과 순서는 CombatSystem.h. 셋 다 공격자 주변 9섹터에 방송한다.
    void OnSwing(CZone* zone, CCharacter* attacker) override;
    void OnHit(CZone* zone, CCharacter* attacker, CCharacter* target, int32_t damage, int32_t remainHp) override;
    void OnDeath(CZone* zone, CCharacter* attacker, CCharacter* target) override;   // 죽은 목록 → DEATH → 경험치 → 드랍 → 몬스터면 지움

    // ── 지속 효과 (USE_COMBAT) ──
    // 걸기·만료·정리는 CBuffSystem(_buffSystem) — 여기는 알림을 받아 본인 알림 조각에 넘기기만. 틱 3-2b 가 만료를 훑고,
    //   사라지는 두 길(놓기의 「떠남」 알림 — 게임 서버의 OnLeavingGame → Forget · OnDeath → ClearAll)이 목록에서 뺀다(BuffSystem.h [목록은 틱 너머로 주소를 든다]).
    // 효과 알림 (IBuffEvents) — 스탯은 이미 다시 선 뒤다. 사람이면 S2C_ACTOR_BUFF(본인) → S2C_MY_STATS(바뀐 공격력). 몬스터면 보낼 세션이 없다.
    void OnBuffChanged(CCharacter* target, BuffKind kind) override;

#if USE_SKILL
    // ── 스킬 (USE_SKILL) ──
    // 판정·비용·대상·효과는 CSkillSystem(_skillSystem) — 여기는 받기와, 거절이면 사유를 본인에게(와이어 값으로 바꿔) 보내기만.
    //   성공의 결과는 알림으로 돌아온다 — 시전(OnCast: 시전 방송 · 시전자 체력 · 시전자 마력) → 피해·죽음·보상(ICombatEvents, 일반 공격과 같은 함수).
    void RecvUseSkill(CPlayer* player, CSerialBuffer* pMsg);

    // 시전 알림 (ISkillEvents) — 뜻과 순서는 SkillSystem.h. 시전자 주변 9섹터에 방송한다(본인 포함). 마력은 본인에게만.
    void OnCast(CZone* zone, CCharacter* caster, const SkillDef& def) override;

    // 되살려라 (ISkillEvents) — 존의 몬스터 관리자에 요청만 넘긴다(RequestRevive — 깔기는 다음 틱 리젠 · 등장 알림은 OnMonsterSpawn 그대로).
    void OnRevive(CZone* zone, CCharacter* caster, int32_t monsterId) override;
#endif

    CSessionHub&       _sessionHub;         // 스킬 거절(한 사람에게)
    CSelfNotifier&     _selfNotifier;       // 시전자 마력 · 효과 칸 · 스탯(본인에게)
    CSightBroadcaster& _sightBroadcaster;   // 휘두름 · 피해 · 죽음 · 시전 · 체력 · 제자리 부활을 주변에
    CPlayerPlacement&  _placement;          // 마을 부활 옮기기 · 죽은 몬스터 존에서 빼기
#if USE_MONSTER
    CRewardHandler&    _rewardHandler;      // 경험치 · 드랍 · 방 보상
#endif
    CCombatSystem&     _combatSystem;       // 공격 흐름 — 빌린 것(게임 서버가 든다)
    CBuffSystem&       _buffSystem;         // 죽으면 효과 풀기 — 빌린 것
#if USE_SKILL
    CSkillSystem&      _skillSystem;        // 스킬 규칙 — 빌린 것
#endif
    CMapManager&       _mapManager;         // 존 찾기 · 방 맵 판별 · 마을 채널 고르기(기본 맵 번호 GetDefaultMapId)
    CGameMonitor&      _gameMonitor;        // 부활 · 방 클리어 지표
    const uint64_t&    _frameCount;         // 게임 루프의 틱 번호 — 빌린 것, 읽기만(쿨타임 · 부활 시각)

    // 죽어서 부활을 기다리는 플레이어. 여기만 틱 너머로 포인터를 든다 —
    //   퇴장 경로(배치의 「존에서 빠짐 · 떠남」 알림을 받는 게임 서버의 OnLeavingZone · OnLeavingGame)가 반드시 회수해야 한다.
    //   몬스터는 여기 안 들어온다 — 죽는 순간 지운다(OnDeath 끝 → 존의 몬스터 관리자 Despawn).
    //   같은 규율이 이미 dirtyMovers(시야 조각) 와 존의 섹터 이동 대기열(CZone::LeaveZone 이 뺀다)에 있다.
    std::vector<CCharacter*> _deadActors;
    // 마을 부활로 옮길 사람과 목적지 채널 — TickRespawn 이 대기열을 다 돈 뒤에 옮긴다(도는 중에 옮기면 퇴장이 대기열을 지운다).
    //   틱 너머로 안 든다(그 함수 안에서 채우고 비운다) — 멤버인 까닭은 틱마다 할당하지 않으려는 것뿐(시야 조각의 _eventAroundBuffer 와 같은 꼴)
    std::vector<std::pair<CPlayer*, CZone*>> _respawnTransferBuffer;
};

#endif // USE_COMBAT
