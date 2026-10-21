#include "PlayerPlacement.h"
#include "SessionHub.h"
#include "SelfNotifier.h"
#include "SightBroadcaster.h"
#include "CharacterStore.h"   // USE_DB_WORKER — 놓기 전 최종 저장
#include "MapManager.h"
#include "Zone.h"
#include "Player.h"
#include "GameMonitor.h"
#include "SerialBuffer.h"
#include <Protocol/Protocol.h>
#include <cassert>
#include "PacketBuild.h" // 머리 쓰기·닫기 · 등장 상태(WriteSnapshot — 내 캐릭터 생성이 같은 조각을 쓴다) — 게임 서버 나누기 0-②

using namespace PacketBuild;   // 아래 조립 함수들이 머리 쓰기·닫기를 이름만으로 부른다

// ==========================================================================
// 배치 S2C 조립 — 이 조각만 부른다(GameServer.cpp 에서 옮김 · 몸·주석 그대로).
//   쓰는 법(스칼라는 << · 고정배열·문자열은 SetData(raw) · pack(1))은 PacketBuild.h 머리의 [규칙] 주석과 같다.
// ==========================================================================
namespace
{
    CSerialBuffer* MakeZoneInfo(CZone* zone)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ZONE_INFO::TYPE);
        *buf << static_cast<int>(zone->GetMapId());
        *buf << static_cast<int>(CMapManager::GetChannelIndexFromZoneId(zone->GetZoneId()));
        *buf << static_cast<int>(zone->GetMapWidth());
        *buf << static_cast<int>(zone->GetMapHeight());
        *buf << static_cast<int>(zone->GetSectorManager().GetSectorSize());
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ZONE_INFO));
        return buf;
    }

    CSerialBuffer* MakeCreateMyPlayer(CPlayer* target)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_CREATE_MY_PLAYER::TYPE);
        *buf << static_cast<int>(target->_actorId);
        *buf << static_cast<uint8_t>(target->_direction);
        *buf << static_cast<uint8_t>(target->_displayChar);
        *buf << static_cast<uint8_t>(target->_colorIndex);
        *buf << target->_x;
        *buf << target->_y;
        *buf << static_cast<int>(target->_speed);
        WriteSnapshot(buf, target);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_CREATE_MY_PLAYER));
        return buf;
    }

    CSerialBuffer* MakeZoneChangeOk(CPlayer* target, int32_t mapId, int32_t channelIndex)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ZONE_CHANGE_OK::TYPE);
        *buf << static_cast<int>(mapId);
        *buf << static_cast<int>(channelIndex);
        *buf << static_cast<int>(target->_actorId);
        *buf << static_cast<uint8_t>(target->_displayChar);
        *buf << static_cast<uint8_t>(target->_colorIndex);
        *buf << static_cast<uint8_t>(target->_direction);
        *buf << target->_x;
        *buf << target->_y;
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ZONE_CHANGE_OK));
        return buf;
    }

    CSerialBuffer* MakeZoneChangeFail(uint8_t reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ZONE_CHANGE_FAIL::TYPE);
        *buf << static_cast<uint8_t>(reason);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ZONE_CHANGE_FAIL));
        return buf;
    }

}

// 떠나보내기 — 게임 중인 플레이어를 세계에서 내보내는 한 곳: 존에서 빼기(존을 찾으면) → 놓기(ReleasePlayer).
//   끊김(OnDisconnected)과 같은 계정 재접속(KickDuplicateAccount)이 같이 부른다 — 정리 코드를 두 벌 두지 않는다(게임 서버 나누기 0-③).
//   ★ 부른 뒤에는 player 를 쓰지 말 것 — ReleasePlayer 가 해제했다.
void CPlayerPlacement::LeaveGame(CPlayer* player)
{
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone != nullptr)
    {
        // 주변 DELETE 브로드캐스트 + 섹터/존 해제
        RemoveFromZone(zone, player);
    }

    // 로그아웃 최종 저장 → 「떠남」 알림(대기열 회수) → 슬롯 비우기(해제) — 놓는 길은 하나다
    ReleasePlayer(player);
}

void CPlayerPlacement::ReleasePlayer(CPlayer* player)
{
#if USE_DB_WORKER
    // 최종 저장 (스냅샷은 값 복사라 직후 해제 안전)
    //   ★ 아래 Forget 보다 「앞」이다 — Forget 이 버프 칸을 비우므로 뒤에서 찍으면 걸려 있던 버프가 저장에 안 실린다
    //     (나갔다 들어오면 버프가 늘 사라진다 — 컴파일도 화면도 멀쩡하고 DB 의 buffs 열만 비어 있다).
    if (_characterStore.IsRunning())
        _characterStore.SaveOnLeave(player);   // 떠난 기록으로 — 확정될 때까지 저장소가 들고 다시 싣는다
#endif

    // 「떠남」 — 틱 너머 목록(부활 대기열 · 지속 효과 · 파티)을 든 쪽이 뺀다(받는 쪽 몸·까닭 주석은 CGameServer::OnLeavingGame).
    //   최종 저장 뒤 · 칸 비우기 앞 — 옮기기 전 이 자리에 몸이 있을 때와 같은 차례(게임 서버 나누기 5 단계 · 0-③ ⑶).
    _events.OnLeavingGame(player);

    // 슬롯 비우기 — 주인이 슬롯이라 여기서 해제된다(맨 끝이어야 한다)
    _sessionHub.RemovePlayer(player);
}

void CPlayerPlacement::SendZoneInfo(CPlayer* target, CZone* zone)
{
    _sessionHub.SendPacket(target, MakeZoneInfo(zone));
}

void CPlayerPlacement::SendCreateMyPlayer(CPlayer* target)
{
    _sessionHub.SendPacket(target, MakeCreateMyPlayer(target));   // 본인 체력도 이 안에 실린다(ActorSnapshot)
}

// 존에서 존으로 옮기기 한 곳(PlayerPlacement.h 선언 주석).
void CPlayerPlacement::TransferPlayer(CPlayer* player, CZone* oldZone, CZone* newZone, ArrivalKind kind, const SpawnAt* at)
{
    // 같은 존으로 옮기기는 부르는 쪽이 막았다 — 통과시키면 자리만 다시 뽑혀 순간이동이 된다(PickZoneChangeTarget 의 SAME_ZONE 주석)
    assert(oldZone != newZone);

    // ── 현재 존에서 퇴장 ──
    RemoveFromZone(oldZone, player);

    // ── 새 존에 입장 (player 객체 재활용, playerId 유지) ──
    //   들어갈 수 있는지는 부르는 쪽이 나가기 "전에" 다 봤다 — 입장은 실패가 없다(CZone::EnterZone 주석).
    //   그래서 나간 뒤 들어가다 실패해 원래 맵으로 되돌리는 길이 없다(10-09 에 없앰 — 한 번도 안 도는 길이었다).
    if (at != nullptr)
        newZone->EnterZoneAt(player, at->x, at->y);
    else
        newZone->EnterZone(player);

    _gameMonitor._sector._zoneChangeCount.Inc();

#if USE_DB_WORKER
    player->_dbDirty = true;   // 존/맵 이동 → 위치·map_id 변경, 저장 대상
#endif

    // 델타 동기화 기준 좌표 초기화
    player->_lastSyncX = player->_x;
    player->_lastSyncY = player->_y;

    // 존 정보 + 존 이동 성공 통보 + 숫자 → 주변과 서로 생성 (NotifyArrival 한 곳)
    NotifyArrival(player, newZone, kind);
}

void CPlayerPlacement::SendZoneChangeOk(CPlayer* target, int32_t mapId, int32_t channelIndex)
{
    _sessionHub.SendPacket(target, MakeZoneChangeOk(target, mapId, channelIndex));
}

void CPlayerPlacement::SendZoneChangeFail(CPlayer* target, uint8_t reason)
{
    _sessionHub.SendPacket(target, MakeZoneChangeFail(reason));
}

// ==========================================================================
// 존 입장/퇴장 브로드캐스트
// ==========================================================================

void CPlayerPlacement::NotifyArrival(CPlayer* player, CZone* zone, ArrivalKind kind)
{
    // 길마다 다른 셋 — 내 캐릭터를 CREATE_MY 로 알리나(아니면 ZONE_CHANGE_OK) · 가방·스탯까지 보내나 · 등장 사유.
    //   ArrivalKind 마다 한 줄. 줄 수가 길 수와 다르면 컴파일이 막는다(switch 는 이 빌드들에서 빠진 갈래를 경고하지 않는다).
    //   revive — 마을 부활만. 다른 맵에서 되살아났다는 사실은 ZONE_CHANGE_OK 가 못 싣는다(체력이 없다) — 본인에게 RESPAWN 한 장.
    struct ArrivalRule { bool createMy; bool firstEntry; SpawnReason reason; bool revive; };
    static constexpr ArrivalRule ARRIVAL_RULES[] = {
        { true,  true,  SpawnReason::CONNECT,       false },  // CONNECT
        { false, false, SpawnReason::ZONE_TRANSFER, false },  // ZONE_CHANGE — 내 캐릭터는 ZONE_CHANGE_OK 가 알린다
        { false, false, SpawnReason::ZONE_TRANSFER, true  },  // RESPAWN — 존 이동 + 본인에게 RESPAWN
    };
    static_assert(sizeof(ARRIVAL_RULES) / sizeof(ARRIVAL_RULES[0]) == static_cast<size_t>(ArrivalKind::COUNT),
                  "ARRIVAL_RULES 는 ArrivalKind 마다 한 줄이어야 한다");
    assert(static_cast<size_t>(kind) < static_cast<size_t>(ArrivalKind::COUNT));
    const ArrivalRule& rule = ARRIVAL_RULES[static_cast<size_t>(kind)];

    // 1) 존 메타 정보
    SendZoneInfo(player, zone);

    // 2) 내 캐릭터 — 접속은 CREATE_MY, 존 이동은 성공 통보(ZONE_CHANGE_OK)가 내 자리를 싣는다
    if (rule.createMy)
        SendCreateMyPlayer(player);
    else
        SendZoneChangeOk(player, zone->GetMapId(), CMapManager::GetChannelIndexFromZoneId(zone->GetZoneId()));

#if USE_COMBAT
    // 2-1) 마을 부활 — 내 자리를 안 뒤에 「되살아났다」(체력). 웹·콘솔이 이미 받는 패킷이라 클라는 죽음 풀기를 새로 안 짠다.
    //   본인에게만 — 마을 사람들은 아래 4) 의 CREATE 가 체력 가득한 나를 보여 준다(CREATE 가 체력을 싣는다)
    if (rule.revive)
        _selfNotifier.SendRespawn(player);
#endif

#if USE_EXP
    // 3) 레벨·경험치 — 내 캐릭터 뒤에 보낸다(클라가 내 액터를 알고 나서 숫자를 채우게).
    //   ★ 존 이동도 다시 보낸다 — 그 길은 SendCreateMyPlayer 를 안 거친다.
    //   콘솔 클라는 존을 넘을 때 내 정보를 통째로 비우고 다시 세운다(speed 만 따로 옮겨 담는다,
    //   GameInstance.cpp 의 ZONE_CHANGE_OK). 안 보내면 거기서 화면이 1 레벨로 돌아가고
    //   다음 경험치가 들어올 때까지 그대로다. 웹 클라는 안 비우므로 이 장이 덧쓰기가 될 뿐 무해하다.
    _selfNotifier.SendExp(player);
#endif

#if USE_SKILL
    // 3) 마력도 같은 자리 — 내 캐릭터 뒤(클라가 번호로 내 액터를 찾게). 경험치와 같이 존 이동 때도 다시 보낸다(firstEntry 밖).
    //   ★ 웹은 마력을 내 액터(self)에 담는다 — 웹 주석(live.html 의 me 정의 위)은 존을 넘으면 self 를 새로 만든다고 적어 둔다.
    //     지금은 웹에 존 이동 처리가 없어 덧쓰기가 될 뿐이지만, 그 처리가 생기는 날 마력이 비지 않게 미리 보낸다(10-11).
    //   부하 더미도 입장 때 이 한 장을 반드시 받는다 — 등록을 빼면 전원 떨어진다(가방 전체·MY_STATS 와 같은 함정).
    _selfNotifier.SendActorMp(player);
#endif

#if USE_COMBAT
    // 3-0) 켜져 있는 지속 효과 — 마력과 같은 자리·같은 까닭(존 이동 때 다시, firstEntry 밖). 입장 땐 DB 저장값으로 되살린 효과만 간다(없으면 0장).
    //   부하 더미에겐 올 일이 없다(스킬을 안 쓴다) — 등록은 해 뒀다(DummyClient.cpp).
    _selfNotifier.SendActiveBuffs(player);
#endif

#if USE_SKILL
    // 3-0b) 아직 돌고 있는 스킬 쿨타임 — 효과와 같은 자리·같은 까닭(존 이동 때 다시, firstEntry 밖). 없으면 0장.
    //   다시 접속하면 DB 저장값으로 되살린 쿨타임이 여기서 처음 클라에 간다(RestoreCooldownsAndBuffs) — 안 보내면 웹 스킬 칸이 다 풀린 것처럼 그려진다.
    //   부하 더미에겐 올 일이 없다(스킬을 안 쓴다) — 등록은 해 뒀다(DummyClient.cpp).
    _selfNotifier.SendActiveSkillCooldowns(player);
#endif

    if (rule.firstEntry)
    {
#if USE_INVENTORY
        // 3-1) 가방 전체 — 빈 가방이어도 한 장. 존 이동 때는 안 보낸다(가방이 안 바뀐다).
        //   ★ 부하 더미가 이 패킷을 받는 유일한 길이 여기다(더미는 아이템을 안 받는다). 그래서 더미 시험과
        //     "등록을 빼면 떨어지나" 돌연변이가 이 한 장에 매달려 있다 — "비었으면 안 보내자"로 바꾸면 둘 다
        //     조용히 아무것도 안 보게 된다.
        _selfNotifier.SendInventory(player);

        // 3-1b) 가진 돈 — 가방 전체 바로 뒤 한 장(돈은 가방의 일부 · 존 이동 때는 안 보낸다 — 가방과 같은 갈래).
        //   ★ 부하 더미도 이 한 장을 반드시 받는다 — 가방 전체와 같은 함정(더미 크기 표에서 빠지면 전원 떨어진다).
        _selfNotifier.SendMoney(player);
#endif

#if USE_COMBAT
        // 3-2) 최종 공격력·방어력 — 가방 전체 뒤(클라가 무기 칸을 알고 나서 숫자를 채우게).
        //   ★ 부하 더미도 이 한 장을 반드시 받는다 — 가방 전체와 같은 함정(등록을 빼면 전원 떨어진다).
        _selfNotifier.SendMyStats(player);
#endif
    }

    // 4) 주변 상호 CREATE — 본인 몫을 다 보낸 뒤
    _sightBroadcaster.BroadcastEnterZone(zone, player, rule.reason);
}

// 존에서 빼기 — 시야 몫(시야 조각의 BroadcastLeaveZone) → 「존에서 빠짐」 알림(부활 대기열 정리) → 존에서 빼기, 이 순서
//   (쪼개기 전 BroadcastLeaveZone 한 몸일 때의 순서 그대로 — 게임 서버 나누기 0-③).
//   플레이어 퇴장·존 이동과 죽은 몬스터(OnDeath 끝)·사라지는 몬스터(OnMonsterVanish)가 같이 쓴다 — 종류를 안 가린다.
void CPlayerPlacement::RemoveFromZone(CZone* zone, CCharacter* player)
{
    _sightBroadcaster.BroadcastLeaveZone(zone, player);

    // 「존에서 빠짐」 — 틱 너머 목록(부활 대기열)을 든 쪽이 뺀다(받는 쪽 몸·까닭 주석은 CGameServer::OnLeavingZone).
    //   시야 몫 뒤 · LeaveZone 앞 — 옮기기 전 이 자리에 몸이 있을 때와 같은 차례(게임 서버 나누기 5 단계 · 0-③ ⑴).
    _events.OnLeavingZone(player);

    zone->LeaveZone(player);
}

