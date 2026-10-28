// ==========================================================================
// BuffSystem — 지속 효과(버프) 규칙 (USE_COMBAT)
//
// [책임]
//  - 걸기(비어 있으면 새로, 차 있으면 값·시간 덮기 = 되감기) · 만료 틱 · 죽음(풀림) · 퇴장(목록에서만 뺌) · 되살리기(입장 — DB 저장값)
//  - 바뀌면 스탯을 다시 세운다(CStatSystem::RecalcStats — 바탕 + 장비 뒤에 배수) → 알림(IBuffEvents)
//  - 그 과정에서 정한 것(걸림·되감김·끝난 사유·살아 있는 칸 수)을 지표(CGameMonitor)로 센다 — CCombatSystem 과 같은 꼴
//  - 보내기는 하지 않는다 — 게임 서버가 알림을 받아 S2C_ACTOR_BUFF(본인)·S2C_MY_STATS 로 보낸다
//
// [왜 따로인가]
//  세우기(CStatSystem)와 「시간이 지나 풀림」은 다른 일이고, 거는 쪽이 스킬·아이템 둘이라 어느 한쪽 것이 아니다(10-12).
//  목록은 게임 서버에 하나 — 존마다 두면 존을 옮길 때 옮겨 담아야 한다(효과는 존을 따라간다). 부활 대기열(_deadActors)과 같은 꼴.
//
// [목록은 틱 너머로 주소를 든다] 캐릭터가 사라지는 **세** 길이 반드시 뺀다 — 퇴장 CGameServer::OnLeavingGame(Forget — 배치의 놓기 CPlayerPlacement::ReleasePlayer 가 「떠남」 알림으로 부른다) ·
//  죽음 CCombatHandler::OnDeath(ClearAll — 몬스터는 그 끝의 Despawn 해제 "앞") · 존 삭제 CGameServer::OnErasingZone(ForgetZone — 빈 채널 정리가 지우기 직전에 부른다 · 존이 지워지면 그 존의
//  몬스터도 같이 해제된다). 하나라도 빠지면 다음 틱이 해제된 메모리를 밟는다.
//  셋째 길은 몬스터에 버프가 걸리는 날(보스 격노, 10-14) 처음 생겼다 — 그 전엔 몬스터가 목록에 들어올 일이 없어 잠복해 있었다.
//  수정 전 실서버로 본 것: 버프 걸린 보스가 있는 동적 채널이 지워진 뒤 mmo_buff_active 가 1 로 남았다(서버는 안 죽었다).
//  Tick 이 이 목록의 주소를 매 틱 읽으므로(코드) 그 주소는 해제된 메모리다 — 정의되지 않은 동작이라 안 죽은 것은 우연이다.
//  RemoveFromZone(존에서 빼기)에서는 빼지 않는다 — 존 이동도 그 길을 지나는데 효과는 존을 따라가야 한다.
//
// [계층 경계]
//  CCharacter·CStatSystem·CGameMonitor 를 안다. 세션·패킷·CGameServer·존·스킬 표는 모른다(값·지속은 거는 쪽이 넘긴다).
//  객체는 CGameServer 가 멤버로 든다 — _statSystem 뒤에 선언한다(그 참조를 받는다 — 멤버는 선언 순서로 세워진다).
// ==========================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "BuildConfig.h"   // USE_COMBAT — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include "Buff.h"          // BuffKind · BuffSlot · BuffEnd

#if USE_COMBAT

class CCharacter;
class CStatSystem;
class CGameMonitor;

// 효과가 바뀌었다 — 전투 조각 CCombatHandler 가 구현한다(패킷 조립·송신 — 게임 서버 나누기 9 단계). ICombatEvents 와 같은 꼴.
//   스탯은 이미 다시 세운 뒤에 온다 — 받는 쪽은 칸(효과 컴포넌트 CBuffComponent::_slots[kind])을 그대로 읽어 보내면 된다(비었으면 풀림 = remainMs 0).
//   ★ 순수 가상이라 빠뜨리면 컴파일이 안 된다.
class IBuffEvents
{
public:
    virtual ~IBuffEvents() = default;
    virtual void OnBuffChanged(CCharacter* target, BuffKind kind) = 0;
};

class CBuffSystem
{
public:
    CBuffSystem(CGameMonitor& gameMonitor, CStatSystem& statSystem, IBuffEvents& events);

    // 걸기 — 비어 있으면 새로(목록에 넣음), 차 있으면 값·시간을 덮는다(되감기). 스탯 다시 세우기 → 알림.
    //   durationFrames 는 1 이상(표 읽기가 막는다) — 0 이면 칸이 「비어 있다」와 구별이 안 된다.
    void Apply(CCharacter* target, BuffKind kind, int32_t value, uint64_t durationFrames, uint64_t frame);

    // 만료 — 틱마다 1회(게임 루프 3-2b). 목록만 훑고, 비면 바로 끝난다. 풀린 종류마다 알림.
    void Tick(uint64_t frame);

    // 죽음 — 살아 있는 칸 전부 풀고(스탯 다시 세우기 · 종류마다 알림) 목록에서 뺀다. 몬스터면 해제(Despawn) 앞에 부를 것.
    void ClearAll(CCharacter* target);

    // 퇴장 — 칸을 비우고 목록에서만 뺀다. 알림·재계산 없음(곧 해제된다 · 세션이 없다). 두 번 불러도 무해.
    void Forget(CCharacter* target);

    // 존 삭제 — 이 존 번호의 캐릭터를 목록에서 전부 뺀다(Forget 과 같은 규칙: 칸 비움 · 끝난 사유 LEAVE · 알림·재계산 없음).
    //   빈 채널을 지우기 직전에 부른다 — 사람은 이미 다 나가 Forget 으로 빠졌으니 남은 것은 존이 든 몬스터뿐이다. 목록을 밖에 내주지 않고
    //   여기서 훑는다(MonsterManager.h 「목록을 밖에 내주는 함수는 두지 않는다」와 같은 까닭). 없으면 아무 일 없음.
    void ForgetZone(int32_t zoneId);

    // 되살리기 — 입장 때 DB 저장값으로 빈 칸을 채운다(CCharacterStore::RestoreCooldownsAndBuffs). 반환 false = 칸이 이미 켜져 있어 안 채움
    //   (입장 직후라 늘 비어 있다 — 한 묶음에 같은 종류가 둘인 불량 행이면 첫 것만 쓴다).
    //   Apply 와 다른 둘 —
    //   ⑴ 스탯을 다시 세우지 않고 알리지도 않는다. 호출부가 곧 RecalcStats 로 배수까지 한 번에 세우고,
    //      NotifyArrival 의 SendActiveBuffs 가 CREATE_MY_PLAYER 뒤에 보낸다(이 자리는 클라가 내 액터를 알기 전이다).
    //   ⑵ 「걸림(_applied)」이 아니라 「되살림(_restored)」으로 센다 — 등식은 Σ applied + Σ restored = Σ ended + active.
    //   remainFrames 는 1 이상(호출부가 지난 항목을 거른다) — 0 이면 칸이 「비어 있다」와 구별이 안 된다(Apply 와 같은 까닭).
    bool Restore(CCharacter* target, BuffKind kind, int32_t value, uint64_t remainFrames, uint64_t frame);

private:
    void RemoveFromList(CCharacter* c);
    // 살아 있는 칸을 전부 비우고 「끝남(LEAVE)」으로 센다 — Forget · ForgetZone 이 같이 쓴다(끝나는 자리를 한 곳에서 세야 등식이 안 어긋난다)
    void EndSlotsAsLeave(CCharacter* c);

    CGameMonitor& _gameMonitor;
    CStatSystem&  _statSystem;
    IBuffEvents&  _events;

    // 살아 있는 칸이 하나라도 있는 캐릭터 — 틱이 이것만 훑는다(전원을 매 틱 돌면 동접만큼 게임 루프에 얹힌다).
    //   들어오는 자리 = Apply · Restore(첫 칸이 켜질 때) · 나가는 자리 = Tick(마지막 칸이 꺼질 때) · ClearAll · Forget
    std::vector<CCharacter*> _buffed;
};

#endif // USE_COMBAT
