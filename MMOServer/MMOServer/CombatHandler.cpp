#include "CombatHandler.h"

#if USE_COMBAT

#include "SessionHub.h"
#include "SelfNotifier.h"
#include "SightBroadcaster.h"
#include "PlayerPlacement.h"
#include "RewardHandler.h"    // USE_MONSTER — 경험치 · 드랍 · 방 보상
#include "Combat.h"           // 부활 시각 · 부활 · 부활 자리 규칙(RespawnsToTown)
#include "MapManager.h"
#include "Zone.h"
#include "Player.h"
#include "Monster.h"
#include "MonsterManager.h"   // USE_MONSTER — 되살리기 요청 · 방 클리어 · 죽은 몬스터 지우기
#include "MonsterTable.h"     // MonsterDef — 처치 알림이 경험치(_def.exp)·보스 표식(_def.boss)을 읽는다
#include "SkillTable.h"       // USE_SKILL — SkillDef(시전 알림이 비용·번호를 읽는다)
#include "GameMonitor.h"
#include "PacketRoutes.h"
#include "SerialBuffer.h"
#include <Protocol/Protocol.h>
#include <Common/ErrorLog.h>
#include <algorithm>          // std::remove — 부활 대기열 빼기
#include <cassert>
#include "PacketBuild.h"      // 머리 쓰기·닫기 · 부활(MakeRespawn) · 체력 바뀜(MakeActorStats) — 게임 서버 나누기 0-②

using namespace PacketBuild;   // 아래 조립 함수들이 머리 쓰기·닫기를 이름만으로 부른다

// ==========================================================================
// 전투·스킬 S2C 조립과 스킬 거절 사유 → 와이어 값 — 이 조각만 부른다(GameServer.cpp 에서 옮김 · 몸·주석 그대로).
//   쓰는 법(스칼라는 << · 고정배열·문자열은 SetData(raw) · pack(1))은 PacketBuild.h 머리의 [규칙] 주석과 같다.
// ==========================================================================
namespace
{
    // ── 전투 (USE_COMBAT) ──
    //   필드 순서는 Protocol.h의 구조체와 글자 그대로 같아야 한다. 아래 assert가 크기만 보므로
    //   순서가 뒤바뀌어도 크기가 같으면 안 걸린다 — 고칠 때 구조체를 나란히 열고 볼 것.

    CSerialBuffer* MakeAttack(int32_t attackerId, uint8_t direction)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ATTACK::TYPE);
        *buf << static_cast<int>(attackerId);
        *buf << static_cast<uint8_t>(direction);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ATTACK));
        return buf;
    }

    CSerialBuffer* MakeDamage(int32_t targetId, int32_t attackerId, int32_t damage, int32_t remainHp)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_DAMAGE::TYPE);
        *buf << static_cast<int>(targetId);
        *buf << static_cast<int>(attackerId);
        *buf << static_cast<int>(damage);
        *buf << static_cast<int>(remainHp);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_DAMAGE));
        return buf;
    }

    CSerialBuffer* MakeDeath(int32_t targetId)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_DEATH::TYPE);
        *buf << static_cast<int>(targetId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_DEATH));
        return buf;
    }

#if USE_SKILL
    // 스킬 거절 사유 → 와이어 값. 사유를 정하는 건 CSkillSystem(SkillSystem.h), 패킷 값으로 바꾸는 건 여기 —
    //   ItemHandler.cpp 의 ToUseItemFail 과 같은 꼴. 통과(COUNT)는 실패 패킷이 아니다.
    SkillFail ToSkillFail(SkillReject reject)
    {
        switch (reject)
        {
        case SkillReject::BAD_SKILL: return SkillFail::BAD_SKILL;
        case SkillReject::NOT_FOR_CASTER: return SkillFail::BAD_SKILL;   // 와이어 값을 늘리지 않는다 — 사람 눈에는 「없는 스킬」(몬스터 스킬은 사람 막대에도 안 나온다)
        case SkillReject::DEAD:      return SkillFail::DEAD;
        case SkillReject::COOLDOWN:  return SkillFail::COOLDOWN;
        case SkillReject::LOW_HP:    return SkillFail::LOW_HP;
        case SkillReject::LOW_MP:    return SkillFail::LOW_MP;
        case SkillReject::NO_TARGET: return SkillFail::NO_TARGET;
        case SkillReject::COUNT:     break;
        }
        assert(false && "거절이 아닌 결과를 실패 패킷으로 바꾸려 했다");
        return SkillFail::BAD_SKILL;
    }

    // 필드 순서는 Protocol.h 의 구조체와 글자 그대로 — 위 전투 패킷과 같은 주의(assert 는 크기만 본다)
    CSerialBuffer* MakeSkillCast(int32_t casterId, uint8_t skillId, uint8_t direction)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_SKILL_CAST::TYPE);
        *buf << static_cast<int>(casterId);
        *buf << static_cast<uint8_t>(skillId);
        *buf << static_cast<uint8_t>(direction);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_SKILL_CAST));
        return buf;
    }

    CSerialBuffer* MakeSkillFail(uint8_t skillId, SkillFail reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_SKILL_FAIL::TYPE);
        *buf << static_cast<uint8_t>(skillId);
        *buf << static_cast<uint8_t>(reason);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_SKILL_FAIL));
        return buf;
    }
#endif // USE_SKILL
}

// 받은 패킷 등록 — 옛 등록의 함정 주석을 그대로 옮겼다(게임 서버 나누기 0-④ 의 등록에서 이 조각 몫 둘 · 9 단계).
bool CCombatHandler::RegisterRoutes(CPacketRoutes& routes)
{
    bool ok = true;
    // 이 줄을 빠뜨리면 등록 안 된 번호가 되어 OnReceived가 패킷을 조용히 버린다(로그 없음, _packetErrors만 증가).
    ok &= routes.OnPlayer(MsgType::C2S_ATTACK, sizeof(MSG_C2S_ATTACK), [this](CPlayer* p, CSerialBuffer* m) { RecvAttack(p, m); });
#if USE_SKILL
    // 이 줄이 없으면 스킬 요청이 조용히 버려진다(위 공격 줄과 같은 함정 — 거절 패킷도 안 간다)
    ok &= routes.OnPlayer(MsgType::C2S_USE_SKILL, sizeof(MSG_C2S_USE_SKILL), [this](CPlayer* p, CSerialBuffer* m) { RecvUseSkill(p, m); });
#endif
    return ok;
}

// ==========================================================================
// 전투 (USE_COMBAT)
//
// 공격 흐름(쿨타임 → 대상 찾기 → 피해 → 죽음 · 지표)은 CCombatSystem(CombatSystem.h)이 하고,
//   여기서는 그 알림(ICombatEvents)을 받아 세계에 적용한다 — 송신 · 죽은 목록 등록 · 보상.
//   판정 함수(쿨타임·사거리·피해)는 namespace Combat 이 하고, 그쪽은 존·섹터·송신을 모른다.
//
// [수신자 범위의 한계] 전투 패킷은 전부 "공격자 주변 9섹터"에 뿌린다. 사거리가 짧아(1.5타일 + 두 몸 반지름 — 상한 4 로 최대 9.5)
//   맞은 쪽은 반드시 그 안에 있다(섹터 크기가 그 최대 사거리 이상일 때 — 지금 20. 범위 스킬은 칸이 더 멀어 칸 상한 5 · 대상 반지름 4 면
//   맞는 쪽 중심이 약 13.4 — 섹터를 그보다 작게 줄이면 이 가정부터 다시 볼 것). 다만 맞은 쪽 주변 구경꾼 중 공격자 9섹터 밖인 사람은 못 본다 —
//   섹터가 타일 여러 개라 실제로 겹치지 않는 경우가 드물고, 어긋나도 다음 CREATE 때
//   HP가 다시 맞춰진다(CREATE 가 체력을 싣는다 — ActorSnapshot). 이번 범위에서는 여기까지만 한다.
// ==========================================================================

// 세션 핸들러 — 세션이 있어야 성립하는 것만 보고 본체로 넘긴다.
//   몬스터는 패킷을 안 보내므로 이 경로를 못 탄다. 그래서 본체가 따로 있다.
void CCombatHandler::RecvAttack(CPlayer* player, CSerialBuffer* pMsg)
{
    (void)pMsg;   // C2S_ATTACK은 바디가 없다 — 방향은 서버가 들고 있고, 대상도 서버가 고른다

    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    _combatSystem.PerformAttack(zone, player, _frameCount);
}

// ── 공격 알림 셋 (ICombatEvents) ──
//   공격 흐름(쿨타임 → 대상 찾기 → 피해 → 죽음 · 지표)은 CCombatSystem 이 하고, 여기는 받은 일을 세계에 적용한다.
//   방송은 셋 다 공격자 주변 9섹터 · 본인 포함(excludeSelf=false) — 위 [수신자 범위의 한계] 참조.

// 휘둘렀다 — 대상 유무와 무관하게 온다(헛스윙도 그려야 한다)
void CCombatHandler::OnSwing(CZone* zone, CCharacter* attacker)
{
    _sightBroadcaster.BroadcastAroundSector(zone, attacker,
        MakeAttack(attacker->_actorId, static_cast<uint8_t>(attacker->_direction)), false);
}

// 맞혔다 — damage·remainHp 는 CCombatSystem 이 한 번 센 값 그대로다(깎인 체력과 화면의 숫자가 갈리지 않게)
void CCombatHandler::OnHit(CZone* zone, CCharacter* attacker, CCharacter* target, int32_t damage, int32_t remainHp)
{
    _sightBroadcaster.BroadcastAroundSector(zone, attacker,
        MakeDamage(target->_actorId, attacker->_actorId, damage, remainHp), false);
}

// 죽었다 — 플레이어 죽은 목록 → 죽음 방송 → 경험치 → 드랍, 이 순서다.
//   ★ 순서를 바꾸면 레벨업 체력(ACTOR_STATS)이 죽음(DEATH)보다 먼저 가 화면이 뒤집힌다.
//   죽음 표시(체력 컴포넌트 TakeDamage — 생사·죽은 프레임)·걷기 멈춤(_moveState)과 죽음 계수는 CCombatSystem 이 이미 적었다.
//   죽은 몬스터는 시체로 남는다(되살리지 않는다) — 죽은 목록은 플레이어 것만 든다.
void CCombatHandler::OnDeath(CZone* zone, CCharacter* attacker, CCharacter* target)
{
    // 플레이어 죽은 목록 — 부활 자리·시간이 임시 사양(제자리 3초)이라 존이 아니라 이 조각이 든다(10-05 결정 — 그때는 게임 서버).
    //   몬스터를 여기 넣으면 존이 지워질 때 밖에서 빼 줘야 하는 약속이 다시 생긴다(MonsterManager.h [왜 떼어냈나]).
    if (As<CPlayer>(target) != nullptr)
        _deadActors.push_back(target);

    _sightBroadcaster.BroadcastAroundSector(zone, attacker, MakeDeath(target->_actorId), false);

    // 죽으면 지속 효과가 풀린다(10-01) — 사람·몬스터 공통. 와이어 = DEATH → ACTOR_BUFF(0) → MY_STATS(바탕 공격력), 아래 레벨업 MY_STATS 보다 앞.
    //   ★ 몬스터는 맨 끝 8) Despawn 이 해제하므로 그 "앞"인 여기서 목록에서 빼야 한다 — 뒤에 두면 해제된 주소가 목록에 남는다.
    _buffSystem.ClearAll(target);

#if USE_PARTY && USE_MONSTER
    // ── 6·7 의 갈림 — 방 맵의 보스(몬스터 표 boss)면 방 클리어다: 보상이 막타 한 명이 아니라 방 안 사람 각자(RewardRoom)이고,
    //   남은 몬스터는 사라진다(RequestClear — 지우기는 다음 몬스터 틱. 여기는 범위 스킬의 대상 버퍼를 도는 중일 수 있다 — MonsterManager.h).
    //   보스가 아닌 방 몬스터(가짜 뱀왕)는 필드와 같은 막타 길로 간다 — 경험치 0 · 드랍 줄 없음이면 아무것도 안 생긴다(10-18 · snakeking-plan.md D5).
    //   (10-17 엔 보스 표식이 없어 방의 몬스터면 종류를 안 가렸다 — room-plan.md 「내가 정한 기본값」 4)
    CMonster* const roomMonster = _mapManager.IsRoomMap(zone->GetMapId()) ? As<CMonster>(target) : nullptr;
    const bool roomReward = (roomMonster != nullptr) && roomMonster->_def.boss;
    if (roomReward)
    {
        _rewardHandler.RewardRoom(zone, roomMonster);
        zone->GetMonsterManager().RequestClear();
        _gameMonitor._room._cleared.Inc();
    }
#else
    const bool roomReward = false;
#endif

#if USE_EXP && USE_MONSTER
    // ── 6) 경험치 — 사람이 몬스터를 잡았을 때만 ──
    //   ★ 막타 한 명에게만 준다. 나눠 주려면 누가 얼마나 때렸는지를 몬스터가 들고 있어야 하고,
    //     그건 퇴장 경로가 훑어야 할 대기열이 하나 더 느는 일이다(Monster.h [대상] 과 같은 이유).
    //   때린 쪽은 CCharacter* 라 As<CPlayer> 로 확인 후 내려 바꾼다.
    //   PvP 는 안 준다: 당한 쪽이 몬스터일 때만이다. 방 맵은 위 RewardRoom 이 이미 줬다.
    CPlayer* killer = As<CPlayer>(attacker);
    if (!roomReward && killer != nullptr && target->_kind == CharacterKind::MONSTER)
        _rewardHandler.GrantMonsterExp(zone, killer, As<CMonster>(target)->_def.exp);   // 잡힌 몬스터 종류의 경험치(몬스터 표)
#endif

#if USE_DROP
    // ── 7) 드랍 — 사람이 몬스터를 잡았을 때만(경험치와 같은 조건) ──
    //   막타 한 명이 주인이다(경험치와 같은 이유 — 누가 얼마나 때렸는지 몬스터가 들고 있지 않다). 방 맵은 위 RewardRoom 이 이미 줬다.
    CPlayer*  looter = As<CPlayer>(attacker);
    CMonster* prey   = As<CMonster>(target);
    if (!roomReward && looter != nullptr && prey != nullptr)
        _rewardHandler.DropLoot(zone, looter, prey);
#endif
    (void)roomReward;   // 경험치·드랍을 다 끈 빌드에서도 쓰인 값으로 둔다

#if USE_MONSTER
    // ── 8) 죽은 몬스터 지우기 — 보상까지 끝낸 맨 끝(이 아래로, 그리고 부른 공격 흐름도 target 을 안 쓴다) ──
    //   시체는 클라 연출이다(웹 클라는 죽어 있는 상대의 DELETE 를 받으면 흐려지며 지운다). 서버가 들고 있으면
    //   나중에 다가온 사람 화면에 시체가 다시 나타나고, 스폰이 새로 채우니 객체가 쌓인다(10-09).
    //   존에서 빼는 길은 플레이어 퇴장과 같다 — 주변 DELETE · 옛 섹터 시청자 · 같은 틱 이동자 · 이동 묶음 빼기 · LeaveZone.
    //   객체를 놓고 그 점을 비우는 것은 존의 몬스터 관리자다 — 빈 점은 그 구역 다음 리젠 때 찬다.
    if (CMonster* dead = As<CMonster>(target))
    {
        _placement.RemoveFromZone(zone, dead);
        zone->GetMonsterManager().Despawn(dead);   // ← 여기서 해제된다
    }
#endif
}

#if USE_SKILL
// ==========================================================================
// 스킬 — 판정·비용·대상·효과는 CSkillSystem(SkillSystem.cpp), 여기는 받기와 거절 답만
//   성공의 결과는 알림으로 돌아온다 — 시전(OnCast, 아래) → 피해·죽음·보상(위 OnHit·OnDeath — 일반 공격과 같은 함수)
// ==========================================================================
void CCombatHandler::RecvUseSkill(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_USE_SKILL recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    // 거절이면 아무것도 안 바뀌었다(비용은 모든 검사 뒤에만 뗀다) — 사유만 본인에게
    const SkillReject reject = _skillSystem.UseSkill(zone, player, recvMsg.skillId, _frameCount);
    if (reject != SkillReject::COUNT)
        _sessionHub.SendPacket(player, MakeSkillFail(recvMsg.skillId, ToSkillFail(reject)));
}

// 시전했다 — 비용을 뗀 뒤, 효과 전에 온다. 와이어 순서 = 시전 모양 → 시전자 체력 → 시전자 마력(본인만) → (효과의 DAMAGE …)
void CCombatHandler::OnCast(CZone* zone, CCharacter* caster, const SkillDef& def)
{
    _sightBroadcaster.BroadcastAroundSector(zone, caster,
        MakeSkillCast(caster->_actorId, static_cast<uint8_t>(def.skillId), static_cast<uint8_t>(caster->_direction)), false);

    // 체력을 뗐으면 본인과 주변에 — 남의 머리 위 막대도 줄어야 한다(물약·레벨업 때 체력 방송과 같은 꼴)
    if (def.costHpPct > 0)
        _sightBroadcaster.BroadcastAroundSector(zone, caster, MakeActorStats(caster->_actorId, caster->Health()->Hp(), caster->Health()->MaxHp()), false);

    // 마력을 뗐으면 시전자 본인에게만 — 남의 마력은 화면에 안 그린다(주변 방송 금지, Protocol.h MSG_S2C_ACTOR_MP).
    //   시전자가 사람일 때만이다 — 몬스터는 받을 세션이 없다(몬스터 시전도 이 함수를 지나지만 몬스터 스킬은 마력이 0 이라 여기 안 온다).
    if (def.costMp > 0)
    {
        if (CPlayer* player = As<CPlayer>(caster))
            _selfNotifier.SendActorMp(player);
    }
}

// 되살려라 — 효과 REVIVE(시전 알림 뒤). 요청만 넘긴다: 깔기는 존의 몬스터 관리자가 다음 틱 리젠에서(빈 점마다 하나 — 등장 알림은 OnMonsterSpawn 그대로).
//   여기서 깔지 않는 이유 — 몬스터의 시전은 그 관리자의 판단 순회 안에서 온다(MonsterManager.h RequestRevive ★).
void CCombatHandler::OnRevive(CZone* zone, CCharacter* caster, int32_t monsterId)
{
    (void)caster;
#if USE_MONSTER
    zone->GetMonsterManager().RequestRevive(monsterId);
#else
    (void)zone;
    (void)monsterId;   // 몬스터를 끈 빌드엔 되살리기를 쓰는 몬스터가 없다(되살리기 줄은 user=monster)
#endif
}

#endif // USE_SKILL

// ==========================================================================
// 지속 효과 — 걸기·만료·정리는 CBuffSystem(BuffSystem.cpp), 여기는 알림을 받아 본인 알림 조각에 넘기기만
// ==========================================================================

// 효과가 바뀌었다(걸림·되감김·만료·죽음) — 스탯은 이미 다시 선 뒤라 MY_STATS 가 새 공격력이다. 와이어 = ACTOR_BUFF → MY_STATS.
//   몬스터면 보낼 세션이 없어 둘 다 안 한다(스탯은 CBuffSystem 이 이미 세웠다 — 보스가 자기에게 건 버프가 이 길을 탄다. 다른 사람 화면엔 시전 방송의 고리만 순간 보이고 지속 표시는 아직 없다).
void CCombatHandler::OnBuffChanged(CCharacter* target, BuffKind kind)
{
    if (CPlayer* player = As<CPlayer>(target))
    {
        _selfNotifier.SendActorBuff(player, kind);
        _selfNotifier.SendMyStats(player);
    }
}

// 부활 대기자 훑기 — 틱마다 1회. 죽은 것만 따로 들고 있어 존 전체를 돌지 않는다.
void CCombatHandler::TickRespawn()
{
    if (_deadActors.empty())
        return;

    // 마을로 옮길 사람 — 대기열을 도는 동안엔 옮기지 않는다. 옮기기(TransferPlayer → RemoveFromZone)가
    //   이 대기열을 지우므로(RemoveFromDeadActors) 도는 자리가 어긋난다. 담아 두고 다 돈 뒤 아래에서 옮긴다.
    _respawnTransferBuffer.clear();

    for (size_t i = 0; i < _deadActors.size(); )
    {
        CCharacter* dead = _deadActors[i];
        assert(dead->_kind == CharacterKind::PLAYER);   // 몬스터는 OnDeath 가 안 넣는다 — 되살리지 않는다

        if (!Combat::IsRespawnDue(*dead->Health(), _frameCount))
        {
            ++i;
            continue;
        }

        Combat::Respawn(*dead);

        // 부활도 "되살아난 쪽"으로 가른다 (죽음과 같은 기준 — GameMonitor.h 참조).
        //   이 대기열엔 플레이어만 있다 — 몬스터는 되살리지 않는다.
        _gameMonitor._combat._respawnsTotal.Inc();

        // 어디서 되살아나나 — 죽은 맵이 마을(기본 맵 = 맵 표 첫 줄)이 아니면 마을 시작 구역으로, 마을이면 제자리.
        //   지금은 마을에서 죽는 길이 없다(몬스터 구역 0 · 대인전은 던전에서만 일어난다) — 그 갈래는 예전 그대로 남겨 둔다.
        //   마을 채널은 여기서 고른다(자리 있는 채널 — 존 이동 요청과 같은 규칙). 못 얻으면 제자리로 — 되살리지 못한 채 두지 않는다
        CZone* zone = _mapManager.GetZone(dead->_zoneId);
        CPlayer* player = As<CPlayer>(dead);
        CZone* town = nullptr;
        if (zone != nullptr && player != nullptr && Combat::RespawnsToTown(zone->GetMapId(), _mapManager.GetDefaultMapId()))
            town = _mapManager.FindOrCreateChannel(_mapManager.GetDefaultMapId(), player->_isAdmin);

        if (town != nullptr)
        {
            _gameMonitor._combat._respawnToTown.Inc();   // 부활 자리 — 합 = _respawnsTotal(GameMonitor.h)
            _respawnTransferBuffer.emplace_back(player, town);
        }
        else
        {
            _gameMonitor._combat._respawnInPlace.Inc();
            if (zone != nullptr)
            {
                // 제자리 — 본인 포함. 자기 부활을 자기가 알아야 클라가 조작을 다시 연다
                _sightBroadcaster.BroadcastAroundSector(zone, dead,
                    MakeRespawn(dead->_actorId, dead->_x, dead->_y, dead->Health()->Hp()), false);
            }
        }

        // 순서가 뜻을 갖지 않는 대기열이라 뒤엣것과 바꿔치기로 O(1) 제거.
        //   i를 올리지 않는다 — 방금 당겨온 것을 아직 안 봤다.
        _deadActors[i] = _deadActors.back();
        _deadActors.pop_back();
    }

    // 마을로 — 대기열을 다 돈 뒤(위 주석). 이미 대기열에서 빠졌으니 퇴장의 RemoveFromDeadActors 는 할 일이 없다.
    //   틱 가운데서 옮겨도 되는 까닭 — 이 자리(3-2)는 섹터 이동 대기열을 다 비운 뒤라 같은 틱 이동자 보정이 필요 없다(3-3 주석과 같다).
    //   와이어: 던전 이웃 = DELETE(퇴장) · 본인 = ZONE_INFO → ZONE_CHANGE_OK → RESPAWN → … · 마을 이웃 = CREATE(체력 가득)
    for (const auto& [player, town] : _respawnTransferBuffer)
    {
        CZone* oldZone = _mapManager.GetZone(player->_zoneId);
        assert(oldZone != nullptr && oldZone != town);   // 위에서 「존이 있고 마을이 아님」을 보고 담았다
        _placement.TransferPlayer(player, oldZone, town, CPlayerPlacement::ArrivalKind::RESPAWN, nullptr);
    }
    _respawnTransferBuffer.clear();
}

// 부활 대기열에서 빼기 — 퇴장 경로가 부른다.
//   _isDead로 미리 거르지 않는다. 플래그와 대기열이 어긋나는 경로가 나중에 생겨도
//   여기서 조용히 새지 않게 하려는 것이고, 대기열 길이는 "지금 죽어 있는 수"라 훑는 비용이 없다.
void CCombatHandler::RemoveFromDeadActors(CCharacter* player)
{
    if (_deadActors.empty())
        return;

    _deadActors.erase(
        std::remove(_deadActors.begin(), _deadActors.end(), player),
        _deadActors.end());
}

#endif // USE_COMBAT
