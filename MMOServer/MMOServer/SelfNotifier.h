// ==========================================================================
// CSelfNotifier — 본인 알림: 그 사람 자신의 상태를 그 사람에게만 보낸다 (게임 서버 나누기 2 단계 · B5)
//
// [책임]
//  - 본인에게 가는 패킷(경험치 · 스탯 · 마력 · 지속 효과 · 스킬 쿨타임 · 가방 전체 · 시스템 한 줄 · 되살아남)을 조립해 보낸다
//  - 그 패킷의 보낸 수를 지표(CGameMonitor)로 센다 — 세는 곳은 패킷마다 여기 한 곳이다
//  - 절대 틱(쿨타임·효과가 끝나는 프레임)을 남은 시간(ms)으로 바꾼다 — 클라는 받은 순간부터 깎는다
//
// [경계]
//  보내기는 세션 허브(CSessionHub::SendPacket) 하나로만 한다. 주변 방송은 하지 않는다 — 여기서 보내는 것은 전부 본인에게만 간다
//  (마력·효과·쿨타임은 Protocol.h 의 해당 구조체 주석이 「방송 함수로 보내지 말 것」이라 못 박는다 — 방송하면 남의 값이 새고 주변 전원에게 한 장씩 간다).
//  값을 세우는 규칙(스탯 · 레벨 · 효과 · 쿨타임)은 각 시스템 몫이고, 여기는 이미 선 값을 읽어 싣기만 한다.
//  언제 보내는지(입장 차례 · 레벨업 · 효과 바뀜)는 부르는 쪽이 정한다 — 도착 차례는 CPlayerPlacement::NotifyArrival.
//
// [모르는 것]
//  존·섹터·DB·입장 절차·다른 사람. 받는 쪽은 언제나 CPlayer* 한 명이다(몬스터에겐 세션이 없다 — Player.h [세션]).
// ==========================================================================
#pragma once

#include <cstdint>
#include <string>

#include "BuildConfig.h"          // USE_EXP · USE_COMBAT · USE_INVENTORY · USE_SKILL — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include <Protocol/Protocol.h>    // BuffKind — 와이어 enum(지속 효과 종류)

class CPlayer;
class CSessionHub;
class CGameMonitor;
class CGameDataFiles;

class CSelfNotifier
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 넷(세션 허브 · 지표 · 기획 데이터 · 틱 번호)보다 뒤에 이것을 선언해야 한다
    //   (멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    //   frameCount = 게임 루프의 틱 번호(CGameServer::_frameCount) — 읽기만 한다. 남은 시간 셈의 「지금」이다.
    CSelfNotifier(CSessionHub& sessionHub, CGameMonitor& gameMonitor, const CGameDataFiles& gameDataFiles, const uint64_t& frameCount)
        : _sessionHub(sessionHub), _gameMonitor(gameMonitor), _gameDataFiles(gameDataFiles), _frameCount(frameCount) {}

#if USE_EXP
    // ── 경험치·레벨 ──
    //   규칙(경험치 더하기·레벨 올리기·표 밖 보정)은 CStatSystem(_statSystem) — 여기는 결과를 보내기만 한다.
    // 본인에게 레벨·경험치를 알린다. 만렙이면 nextLevelExp 를 0 으로 보낸다.
    void SendExp(CPlayer* player);
#endif

#if USE_COMBAT
    // 스탯 세우기는 CStatSystem::RecalcStats(_statSystem) — 여기는 결과를 보내기만 한다.
    // 본인에게 최종 공격력·방어력(S2C_MY_STATS) — 입장·레벨업·끼우기·빼기·지속 효과 걸림·풀림(OnBuffChanged). 세는 곳도 여기 한 곳.
    //   존 이동 때는 안 보낸다 — 값이 안 바뀌고, 웹은 판을 안 비우며, 콘솔은 이 값을 안 그린다.
    void SendMyStats(CPlayer* player);

    // 본인에게 효과 칸 하나(S2C_ACTOR_BUFF) — 비었으면 remainMs 0(풀림). 세는 곳도 여기 한 곳.
    //   ★ 본인 전용이다 — BroadcastAroundSector 로 보내지 말 것(Protocol.h MSG_S2C_ACTOR_BUFF).
    void SendActorBuff(CPlayer* player, BuffKind kind);

    // 켜져 있는 효과를 전부 본인에게 — 입장·존 이동(NotifyArrival). 입장 때는 DB 저장값으로 되살린 효과만 간다(없으면 0장 — RestoreCooldownsAndBuffs).
    void SendActiveBuffs(CPlayer* player);

    // 본인에게 되살아남(S2C_RESPAWN) — 마을 부활의 도착 차례(NotifyArrival 2-1)만 부른다. 지금 자리·체력을 싣는다.
    //   주변에 보내는 부활(같은 패킷 — 제자리 부활)은 전투 쪽이 BroadcastAroundSector 로 한다(PacketBuild.h MakeRespawn 을 같이 쓴다).
    void SendRespawn(CPlayer* player);
#endif

#if USE_INVENTORY
    // 가방 전체 — 입장 때 한 번(빈 가방이어도 한 장).
    void SendInventory(CPlayer* player);
#endif

    // 본인에게 알림 한 줄 — 기존 S2C_ERROR 를 쓴다(콘솔은 이미 UTF-8 로 풀어 보여 준다).
    //   토글 밖 — 부르는 곳이 채팅 명령 답(USE_INVENTORY)과 방 보상(USE_PARTY && USE_MONSTER) 둘이다(SelfNotifier.cpp MakeError 주석).
    void SendSystemMessage(CPlayer* player, const std::string& utf8);

#if USE_SKILL
    // 본인에게 마력(S2C_ACTOR_MP) — 입장·존 이동·레벨업·스킬 마력 소모·마력 물약. 세는 곳도 여기 한 곳.
    //   ★ 본인 전용이다 — BroadcastAroundSector 로 보내지 말 것(Protocol.h MSG_S2C_ACTOR_MP).
    void SendActorMp(CPlayer* player);

    // 아직 돌고 있는 스킬 쿨타임을 전부 본인에게(S2C_SKILL_COOLDOWN_REMAIN) — 입장·존 이동(NotifyArrival, SendActiveBuffs 옆). 칸마다 한 장, 없으면 0장.
    //   ★ 본인 전용이다 — BroadcastAroundSector 로 보내지 말 것(Protocol.h MSG_S2C_SKILL_COOLDOWN_REMAIN). 세는 곳도 여기 한 곳.
    void SendActiveSkillCooldowns(CPlayer* player);
#endif

private:
    CSessionHub&          _sessionHub;      // 보내기 — 한 사람에게 한 패킷
    CGameMonitor&         _gameMonitor;     // 보낸 수
    const CGameDataFiles& _gameDataFiles;   // 레벨 표(SendExp 의 누적 필요치)
    const uint64_t&       _frameCount;      // 게임 루프의 틱 번호 — 빌린 것, 읽기만
};
