#include "SelfNotifier.h"
#include "SessionHub.h"
#include "Player.h"
#include "GameMonitor.h"
#include "GameDataFiles.h"
#include "SerialBuffer.h"
#include "NetIoModel.h"   // MAX_PACKET_SIZE — 가방 전체 한 장 검사(static_assert)
#include <Protocol/Protocol.h>
#include <cassert>
#include <cstddef>  // offsetof
#include <cstring>  // memcpy — 알림 문구를 고정 배열에 옮겨 담는다
#include "PacketBuild.h" // 머리 쓰기·닫기 · 부활(MakeRespawn — 전투의 부활 방송과 같이 쓴다)

using namespace PacketBuild;   // 아래 조립 함수들이 머리 쓰기·닫기를 이름만으로 부른다

// ==========================================================================
// 본인 전용 S2C 조립 — 이 조각만 부른다(GameServer.cpp 에서 옮김 · 몸·주석 그대로).
//   쓰는 법(스칼라는 << · 고정배열·문자열은 SetData(raw) · pack(1))은 PacketBuild.h 머리의 [규칙] 주석과 같다.
// ==========================================================================
namespace
{
#if USE_EXP
    CSerialBuffer* MakeExp(int32_t level, int64_t exp, int64_t levelBaseExp, int64_t nextLevelExp)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_EXP::TYPE);
        *buf << static_cast<int>(level);
        *buf << exp;
        *buf << levelBaseExp;
        *buf << nextLevelExp;
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_EXP));
        return buf;
    }
#endif

#if USE_COMBAT
    // 내 최종 공격력·방어력 — 본인 전용(SendMyStats 만 부른다)
    CSerialBuffer* MakeMyStats(int32_t attack, int32_t defense)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_MY_STATS::TYPE);
        *buf << static_cast<int>(attack);
        *buf << static_cast<int>(defense);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_MY_STATS));
        return buf;
    }

    // 지속 효과 통보 — 본인에게만(Protocol.h MSG_S2C_ACTOR_BUFF). remainMs 0 = 풀림
    CSerialBuffer* MakeActorBuff(int32_t actorId, uint8_t kind, int32_t value, uint32_t remainMs)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ACTOR_BUFF::TYPE);
        *buf << static_cast<int>(actorId);
        *buf << static_cast<uint8_t>(kind);
        *buf << static_cast<int>(value);
        *buf << static_cast<unsigned int>(remainMs);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ACTOR_BUFF));
        return buf;
    }
#endif // USE_COMBAT

    // 본인에게 알림 한 줄 — 고정 256바이트 char 배열이라 SetData(raw)로 통째로 싣는다.
    //   넘치면 자른다(마지막 한 바이트는 NUL 자리). 답 문구는 짧아 UTF-8 글자 가운데서 잘릴 일이 없다.
    //   ※ 토글 밖 — 부르는 곳이 채팅 명령 답(USE_INVENTORY)과 방 보상(RewardRoom · USE_PARTY && USE_MONSTER) 둘이다.
    //     전에는 「채팅 명령 답뿐」이라 보고 USE_INVENTORY 안에 둬서, 방 보상이 들어온 뒤 가방을 끈 빌드가 방 보상의 이 호출에서
    //     컴파일이 안 됐다(10-19 게임 서버 나누기 2 단계의 토글 끈 빌드에서 발견 — 나누기 전 커밋에서도 같은 오류).
    //     부르는 쪽(SendSystemMessage)이 늘 컴파일되는 멤버라 둘 다 꺼도 안 쓰는 함수가 되지 않는다.
    CSerialBuffer* MakeError(const std::string& utf8)
    {
        char text[sizeof(MSG_S2C_ERROR::message)] = {};
        const size_t n = (utf8.size() < sizeof(text) - 1) ? utf8.size() : sizeof(text) - 1;
        memcpy(text, utf8.data(), n);

        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ERROR::TYPE);
        buf->SetData(text, static_cast<int>(sizeof(text)));
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ERROR));
        return buf;
    }

#if USE_INVENTORY
    // ── 인벤토리 (USE_INVENTORY) ──
    //   필드 순서는 Protocol.h 의 InventoryEntry 와 글자 그대로 같아야 한다(칸 패킷 MakeInventorySlot 은 ItemHandler.cpp — 같은 주의).

    // 가방 전체는 한 장으로 간다(나눠 보내는 길이 없다) — 가득 찬 가방도 한 패킷에 들어가야 한다.
    //   지금 616B(헤더 4 + 개수 2 + 10 × 61 — 가방 60 + 착용 1). 탭·착용 칸을 늘려 칸이 145개를 넘으면 여기서 컴파일이 멈춘다.
    //   ※ Protocol.h 가 아니라 여기 두는 이유 — 상한(MAX_PACKET_SIZE)이 코어 헤더에 있어 거기선 안 보인다.
    static_assert(sizeof(MSG_S2C_INVENTORY) <= MAX_PACKET_SIZE,
                  "가득 찬 가방 전체 목록이 한 패킷(MAX_PACKET_SIZE)에 안 들어간다 — 나눠 보내는 길을 만들 것");

    // 가방 전체 — 가변 길이, 빈 칸은 안 싣는다. count 를 먼저 세어 앞에 쓴다(MakeSectorUpdates 와 같은 꼴).
    CSerialBuffer* MakeInventory(const CInventory& inv)
    {
        const int count = inv.CountNonEmpty();

        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_INVENTORY::TYPE);
        *buf << static_cast<uint16_t>(count);
        for (int i = 0; i < INVENTORY_ALL_SLOTS; ++i)   // 착용 칸까지 — 클라가 무기 칸도 이 한 장으로 안다
        {
            const Item& s = inv.At(i);
            if (s.itemId == 0)
                continue;
            *buf << static_cast<uint16_t>(CInventory::SlotNumberOf(i));
            *buf << static_cast<int>(s.itemId);
            *buf << static_cast<int>(s.count);
        }
        FinalizePacket(buf);
        assert(buf->GetDataSize() ==
            offsetof(MSG_S2C_INVENTORY, entries) + static_cast<size_t>(count) * sizeof(InventoryEntry));
        return buf;
    }
#endif // USE_INVENTORY

#if USE_SKILL
    // 남은 스킬 쿨타임 — 본인에게만(Protocol.h MSG_S2C_SKILL_COOLDOWN_REMAIN). remainMs 는 1 이상(다 돈 칸은 안 보낸다)
    CSerialBuffer* MakeSkillCooldownRemain(int32_t actorId, uint8_t skillId, uint32_t remainMs)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_SKILL_COOLDOWN_REMAIN::TYPE);
        *buf << static_cast<int>(actorId);
        *buf << static_cast<uint8_t>(skillId);
        *buf << static_cast<unsigned int>(remainMs);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_SKILL_COOLDOWN_REMAIN));
        return buf;
    }

    // 마력 — 본인 전용(SendActorMp 만 부른다). 모양은 ACTOR_STATS 꼴(대상 번호 먼저)
    CSerialBuffer* MakeActorMp(int32_t actorId, int32_t mp, int32_t maxMp)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ACTOR_MP::TYPE);
        *buf << static_cast<int>(actorId);
        *buf << static_cast<int>(mp);
        *buf << static_cast<int>(maxMp);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ACTOR_MP));
        return buf;
    }
#endif // USE_SKILL
}

// ==========================================================================
// 스탯·경험치 보내기 — 세우는 규칙은 CStatSystem(StatSystem.cpp), 여기는 결과를 패킷으로만
// ==========================================================================

#if USE_EXP
// 레벨·경험치 — 표(CLevelTable)의 누적 필요치를 그대로 싣는다(누적 기준은 StatSystem.cpp 경험치 머리 주석).
void CSelfNotifier::SendExp(CPlayer* player)
{
    const CLevelTable& table = _gameDataFiles.Level();
    const int32_t maxLevel = table.MaxLevel();

    // 만렙이면 다음 필요치가 0 — 클라는 0 을 보면 나누지 말고 MAX 로 그린다.
    const int64_t nextExp = (player->_level < maxLevel) ? table.RequiredExp(player->_level + 1) : 0;

    _sessionHub.SendPacket(player, MakeExp(player->_level, player->_exp,
                               table.RequiredExp(player->_level), nextExp));
}
#endif // USE_EXP

#if USE_COMBAT
void CSelfNotifier::SendMyStats(CPlayer* player)
{
    _sessionHub.SendPacket(player, MakeMyStats(player->_attack._attackPower, player->_health.Defense()));
    _gameMonitor._combat._myStatsSent.Inc();
}

// ==========================================================================
// 지속 효과 — 걸기·만료·정리는 CBuffSystem(BuffSystem.cpp), 여기는 본인에게 보내기만
// ==========================================================================

// 효과 칸 하나를 본인에게 — 비었으면 value 0 · remainMs 0(풀림). 세는 곳도 여기 한 곳(SendActorMp 와 같은 꼴).
//   ★ 켜진 칸에 0 을 보내지 않는다 — 0 은 「풀림」의 뜻이다. 존 이동 패킷이 같은 틱의 만료 훑기(3-2b) 앞에 처리되면 남은 틱이 0 이
//     될 수 있어 그때는 1ms 로 올린다(다음 틱의 만료가 곧 0 을 보낸다).
void CSelfNotifier::SendActorBuff(CPlayer* player, BuffKind kind)
{
    const BuffSlot& slot = player->_buffs._slots[static_cast<int>(kind)];
    uint32_t remainMs = 0;
    if (slot.IsActive())
    {
        const uint64_t leftFrames = (slot.expireFrame > _frameCount) ? (slot.expireFrame - _frameCount) : 0;
        remainMs = static_cast<uint32_t>(leftFrames * FRAME_INTERVAL_MS);
        if (remainMs == 0)
            remainMs = 1;
    }
    _sessionHub.SendPacket(player, MakeActorBuff(player->_actorId, static_cast<uint8_t>(kind), slot.IsActive() ? slot.value : 0, remainMs));
    _gameMonitor._buff._actorBuffSent.Inc();
}

// 켜져 있는 칸을 전부 — 입장·존 이동. 입장엔 DB 저장값으로 되살린 효과가 가고(없으면 0장 — RestoreCooldownsAndBuffs),
//   존 이동엔 그대로 따라온 효과가 다시 간다(웹이 존을 넘으면 내 액터를 새로 만드는 날 비지 않게 — ACTOR_MP 와 같은 까닭, 10-11).
void CSelfNotifier::SendActiveBuffs(CPlayer* player)
{
    for (int k = 0; k < BUFF_KIND_COUNT; ++k)
    {
        if (player->_buffs._slots[k].IsActive())
            SendActorBuff(player, static_cast<BuffKind>(k));
    }
}

// 되살아남을 본인에게 — 마을 부활(도착 차례 2-1). 지금 자리·체력(되살린 뒤 값)을 싣는다.
void CSelfNotifier::SendRespawn(CPlayer* player)
{
    _sessionHub.SendPacket(player, MakeRespawn(player->_actorId, player->_x, player->_y, player->_health.Hp()));
}
#endif // USE_COMBAT

#if USE_INVENTORY
void CSelfNotifier::SendInventory(CPlayer* player)
{
    _sessionHub.SendPacket(player, MakeInventory(player->_inventory));
    _gameMonitor._inventory._snapshotsSent.Inc();
}
#endif // USE_INVENTORY

// 토글 밖 — 채팅 명령 답과 방 보상이 같이 부른다(위 MakeError 주석)
void CSelfNotifier::SendSystemMessage(CPlayer* player, const std::string& utf8)
{
    _sessionHub.SendPacket(player, MakeError(utf8));
}

#if USE_SKILL
// 마력 알림 — 그 사람 자신의 마력을 그 사람에게만. 세는 곳도 여기 한 곳(SendMyStats 와 같은 꼴).
void CSelfNotifier::SendActorMp(CPlayer* player)
{
    _sessionHub.SendPacket(player, MakeActorMp(player->_actorId, player->_attack.Mp(), player->_attack.MaxMp()));
    _gameMonitor._skill._actorMpSent.Inc();
}

// 아직 돌고 있는 쿨타임 칸마다 한 장 — 입장·존 이동(NotifyArrival). 다 돈 칸(ready ≤ 지금)은 안 보낸다(0 은 이 패킷에 없는 값이다).
//   남은 시간은 남은 틱 × 명목 40ms — 클라는 받은 순간부터 깎는다. 시전 순간의 쿨타임은 지금처럼 클라가 SKILL_CAST 로 센다.
void CSelfNotifier::SendActiveSkillCooldowns(CPlayer* player)
{
    for (int id = 1; id <= MAX_SKILL_ID; ++id)
    {
        const uint64_t ready = player->_attack._skillReadyFrame[id];
        if (ready <= _frameCount)
            continue;
        const uint32_t remainMs = static_cast<uint32_t>((ready - _frameCount) * FRAME_INTERVAL_MS);   // 표 상한 1시간 → 360만 ms, u32 안
        _sessionHub.SendPacket(player, MakeSkillCooldownRemain(player->_actorId, static_cast<uint8_t>(id), remainMs));
        _gameMonitor._skill._cooldownRemainSent.Inc();
    }
}
#endif // USE_SKILL
