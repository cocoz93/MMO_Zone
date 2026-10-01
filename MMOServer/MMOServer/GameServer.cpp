#include "GameServer.h"
#include "Player.h"
#include "Monster.h"     // USE_MONSTER — 헤더 안에서 스스로 토글을 본다
#include "MonsterAI.h"   // 몬스터 판단 (Combat 과 같은 자리의 순수 판정 모듈)
#include "GameDataCsv.h" // USE_INVENTORY — 채팅 명령 인자를 숫자로 읽는다(표 파서와 같은 규칙)
#include <Protocol/Protocol.h>
#include "SerialBuffer.h"
#include <Common/ErrorLog.h>
#include "CoreAffinity.h"
#include <iostream>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstddef>  // offsetof
#include <cassert>  // 직렬화 필드 누락/폭 검증
#include <cstring>  // memcpy — 알림 문구를 고정 배열에 옮겨 담는다
#include <string>
#include <random>   // USE_MONSTER — 스폰 구역 안 자리 뽑기

// ==========================================================================
// S2C 직렬화 헬퍼 (파일 로컬)
//
// 와이어 = payload 영역 바이트 그대로 (WSASend가 GetReadBufferPtr()+GetDataSize()만 전송).
// pack(1)이라 패딩이 없으므로 필드를 선언 순서/폭대로 쓰면 구조체와 바이트 동일.
// 스칼라는 <<, 고정배열/문자열은 SetData(raw) — << 문자열 연산자는 길이접두(2B)가 끼어
// 와이어가 깨지므로 사용 금지.
//
// [소유권 계약] 빌더는 RefCount=1(소유권 1개)인 버퍼를 반환하고,
//   송신 함수(BroadcastAroundSector/SendPacket의 CSerialBuffer* 오버로드)가 이를 소비한다.
// ==========================================================================
namespace
{
    // 헤더 시작: size placeholder(아래서 백패치) + type
    inline void BeginPacket(CSerialBuffer* buf, MsgType type)
    {
        *buf << static_cast<uint16_t>(0);                 // header.size (placeholder)
        *buf << static_cast<uint16_t>(type);              // header.type
    }

    // 헤더 종료: size 백패치(= 전체 payload 바이트) + Seal
    // 소유권 1은 Alloc()/Clear()에서 이미 확보됨(RefCount=1) → 여기서 AddRef 불필요
    inline void FinalizePacket(CSerialBuffer* buf)
    {
        *reinterpret_cast<uint16_t*>(buf->GetPayloadBufferPtr()) =
            static_cast<uint16_t>(buf->GetDataSize());
        buf->Seal();
    }

    // 등장 상태 조각(Protocol.h ActorSnapshot) — CREATE 빌더 셋이 맨 끝에 부른다.
    //   조각에 필드를 늘리면 여기 한 줄 · proto.js readSnap 한 줄(Protocol.h 의 크기 단언이 알려 준다).
    //   전투가 꺼진 빌드는 0 을 싣는다 — _hp 는 꺼져도 있지만(CCharacter) 쓰는 곳이 없어 의미가 없다.
    inline void WriteSnapshot(CSerialBuffer* buf, const CCharacter* ch)
    {
#if USE_COMBAT
        *buf << static_cast<int>(ch->_hp);
        *buf << static_cast<int>(ch->_maxHp);
#else
        (void)ch;
        *buf << static_cast<int>(0);
        *buf << static_cast<int>(0);
#endif
    }

    // MOVE_START / MOVE_STOP은 레이아웃 동일 — 3곳/2곳에서 재사용되므로 빌더로 DRY 처리
    CSerialBuffer* MakeMoveStart(int32_t playerId, uint8_t direction, float x, float y)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_MOVE_START::TYPE);
        *buf << static_cast<int>(playerId);
        *buf << static_cast<uint8_t>(direction);
        *buf << x;
        *buf << y;
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_MOVE_START));
        return buf;
    }

    CSerialBuffer* MakeMoveStop(int32_t playerId, uint8_t direction, float x, float y)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_MOVE_STOP::TYPE);
        *buf << static_cast<int>(playerId);
        *buf << static_cast<uint8_t>(direction);
        *buf << x;
        *buf << y;
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_MOVE_STOP));
        return buf;
    }

    CSerialBuffer* MakeSyncPosition(int32_t playerId, float x, float y)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_SYNC_POSITION::TYPE);
        *buf << static_cast<int>(playerId);
        *buf << x;
        *buf << y;
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_SYNC_POSITION));
        return buf;
    }

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

    CSerialBuffer* MakeCreateOtherPlayer(CCharacter* player, SpawnReason reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_CREATE_OTHER_PLAYER::TYPE);
        *buf << static_cast<int>(player->_actorId);
        *buf << static_cast<uint8_t>(player->_direction);
        *buf << static_cast<uint8_t>(player->_moveState);
        *buf << static_cast<uint8_t>(player->_displayChar);
        *buf << static_cast<uint8_t>(player->_colorIndex);
        *buf << static_cast<uint8_t>(reason);
        *buf << player->_x;
        *buf << player->_y;
        *buf << static_cast<int>(player->_speed);
        WriteSnapshot(buf, player);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_CREATE_OTHER_PLAYER));
        return buf;
    }

    CSerialBuffer* MakeDeletePlayer(int32_t playerId)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_DELETE_PLAYER::TYPE);
        *buf << static_cast<int>(playerId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_DELETE_PLAYER));
        return buf;
    }

#if USE_COMBAT
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

    CSerialBuffer* MakeRespawn(int32_t targetId, float x, float y, int32_t hp)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_RESPAWN::TYPE);
        *buf << static_cast<int>(targetId);
        *buf << x;
        *buf << y;
        *buf << static_cast<int>(hp);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_RESPAWN));
        return buf;
    }

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

    CSerialBuffer* MakeActorStats(int32_t actorId, int32_t hp, int32_t maxHp)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ACTOR_STATS::TYPE);
        *buf << static_cast<int>(actorId);
        *buf << static_cast<int>(hp);
        *buf << static_cast<int>(maxHp);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ACTOR_STATS));
        return buf;
    }

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
#endif // USE_COMBAT

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

    CSerialBuffer* MakeAdminLoginOk()
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ADMIN_LOGIN_OK::TYPE);    // 페이로드 없음 (헤더만)
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ADMIN_LOGIN_OK));
        return buf;
    }

    CSerialBuffer* MakeAdminLoginFail()
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ADMIN_LOGIN_FAIL::TYPE);  // 페이로드 없음 (헤더만)
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ADMIN_LOGIN_FAIL));
        return buf;
    }

    // 가변 길이 — message는 고정배열/문자열이라 SetData(raw). msgLen은 null 제외 글자 수.
    CSerialBuffer* MakeChat(int32_t playerId, uint8_t displayChar, uint8_t colorIndex,
                             ChatChar* message, uint16_t msgLen)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_CHAT::TYPE);
        *buf << static_cast<int>(playerId);
        *buf << static_cast<uint8_t>(displayChar);
        *buf << static_cast<uint8_t>(colorIndex);
        buf->SetData(reinterpret_cast<char*>(message),
            (msgLen + 1) * sizeof(ChatChar));              // message (가변, null 포함, raw)
        FinalizePacket(buf);
        assert(buf->GetDataSize() ==
            offsetof(MSG_S2C_CHAT, message) + (msgLen + 1) * sizeof(ChatChar));
        return buf;
    }

#if USE_INVENTORY
    // 본인에게 알림 한 줄 — 고정 256바이트 char 배열이라 SetData(raw)로 통째로 싣는다.
    //   넘치면 자른다(마지막 한 바이트는 NUL 자리). 답 문구는 짧아 UTF-8 글자 가운데서 잘릴 일이 없다.
    //   ※ 지금 부르는 곳이 채팅 명령 답뿐이라 USE_INVENTORY 안에 둔다(밖에 두면 끈 빌드에서 안 쓰는 함수가 된다).
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

    // ── 인벤토리 (USE_INVENTORY) ──
    //   필드 순서는 Protocol.h 의 InventoryEntry·MSG_S2C_INVENTORY_SLOT 와 글자 그대로 같아야 한다.

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

    CSerialBuffer* MakeInventorySlot(uint16_t slot, int32_t itemId, int32_t count)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_INVENTORY_SLOT::TYPE);
        *buf << static_cast<uint16_t>(slot);
        *buf << static_cast<int>(itemId);
        *buf << static_cast<int>(count);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_INVENTORY_SLOT));
        return buf;
    }

    CSerialBuffer* MakeUseItemFail(uint16_t slot, UseItemFail reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_USE_ITEM_FAIL::TYPE);
        *buf << static_cast<uint16_t>(slot);
        *buf << static_cast<uint8_t>(reason);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_USE_ITEM_FAIL));
        return buf;
    }

#if USE_EQUIP
    CSerialBuffer* MakeEquipFail(uint16_t slot, EquipFail reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_EQUIP_FAIL::TYPE);
        *buf << static_cast<uint16_t>(slot);
        *buf << static_cast<uint8_t>(reason);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_EQUIP_FAIL));
        return buf;
    }
#endif
#endif // USE_INVENTORY

#if USE_DROP
    CSerialBuffer* MakeGroundItemCreate(const CGroundItem& g, GroundItemAppear reason)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_GROUND_ITEM_CREATE::TYPE);
        *buf << static_cast<int32_t>(g.groundId);
        *buf << static_cast<int32_t>(g.item.itemId);
        *buf << static_cast<int32_t>(g.item.count);
        *buf << g.x;
        *buf << g.y;
        *buf << static_cast<uint8_t>(reason);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_GROUND_ITEM_CREATE));
        return buf;
    }

    CSerialBuffer* MakeGroundItemDelete(int32_t groundId, GroundItemGone reason, int32_t pickerId)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_GROUND_ITEM_DELETE::TYPE);
        *buf << static_cast<int32_t>(groundId);
        *buf << static_cast<uint8_t>(reason);
        *buf << static_cast<int32_t>(pickerId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_GROUND_ITEM_DELETE));
        return buf;
    }

    CSerialBuffer* MakePickupFail(PickupFail reason, int32_t itemId)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_PICKUP_FAIL::TYPE);
        *buf << static_cast<uint8_t>(reason);
        *buf << static_cast<int32_t>(itemId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_PICKUP_FAIL));
        return buf;
    }
#endif // USE_DROP

#if USE_SECTOR_AGGREGATION
    // 섹터 묶음 — dirty 플레이어 [0,count)의 최종 상태(위치·방향·이동상태)를 직렬화.
    // 스칼라 시퀀스(<<)라 SetData 불필요. count는 선기록(백패치 불필요).
    // 좌표는 float 그대로가 아니라 눈금(QuantizePos)으로 싣는다 — 필드 순서는 SectorUpdateEntry와 같아야 한다.
    CSerialBuffer* MakeSectorUpdates(CCharacter** players, int count)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_SECTOR_UPDATES::TYPE);
        *buf << static_cast<uint16_t>(count);
        for (int i = 0; i < count; ++i)
        {
            CCharacter* p = players[i];
            *buf << static_cast<int>(p->_actorId);
            *buf << static_cast<uint16_t>(QuantizePos(p->_x));
            *buf << static_cast<uint16_t>(QuantizePos(p->_y));
            *buf << static_cast<uint8_t>(PackDirState(static_cast<uint8_t>(p->_direction),
                                                      static_cast<uint8_t>(p->_moveState)));
        }
        FinalizePacket(buf);
        // (정정) GetDataSize는 BeginPacket이 쓴 MsgHeader 4B를 포함 — offsetof 기준이 맞는 식
        assert(buf->GetDataSize() ==
            static_cast<int>(offsetof(MSG_S2C_SECTOR_UPDATES, entries) + count * sizeof(SectorUpdateEntry)));
        return buf;
    }
#endif

#if USE_MEMBERSHIP_INBOUND_BUNDLE
    // 가득 찬 묶음(상한 × 엔트리)도 한 패킷에 들어가야 한다 — 엔트리를 늘리면 상한을 같이 줄일 것(Protocol.h 계산 주석).
    //   여기 두는 이유는 위 가방 단언과 같다(MAX_PACKET_SIZE 가 코어 헤더에 있다).
    static_assert(sizeof(MSG_S2C_CREATE_PLAYER_BATCH) <= MAX_PACKET_SIZE,
                  "가득 찬 CREATE 묶음이 한 패킷(MAX_PACKET_SIZE)에 안 들어간다 — CREATE_PLAYER_BATCH_MAX_ENTRIES 를 줄일 것");

    // [Phase 2] 인바운드 멤버십 묶음 — 상대 [0,count)의 생성 정보를 배치 1패킷으로 직렬화.
    // 필드 순서는 MakeCreateOtherPlayer 바디와 동일 (클라가 엔트리를 기존 CREATE 이벤트로 분해).
    CSerialBuffer* MakeCreatePlayerBatch(CCharacter* const* players, int count)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_CREATE_PLAYER_BATCH::TYPE);
        *buf << static_cast<uint16_t>(count);
        for (int i = 0; i < count; ++i)
        {
            CCharacter* p = players[i];
            *buf << static_cast<int>(p->_actorId);
            *buf << static_cast<uint8_t>(p->_direction);
            *buf << static_cast<uint8_t>(p->_moveState);
            *buf << static_cast<uint8_t>(p->_displayChar);
            *buf << static_cast<uint8_t>(p->_colorIndex);
            *buf << static_cast<uint8_t>(SpawnReason::NORMAL);   // 인바운드(걸어서 시야 진입) 경로 고정 — OFF의 기본 인자와 동일
            *buf << p->_x;
            *buf << p->_y;
            *buf << static_cast<int>(p->_speed);
            WriteSnapshot(buf, p);
        }
        FinalizePacket(buf);
        assert(buf->GetDataSize() ==
            static_cast<int>(offsetof(MSG_S2C_CREATE_PLAYER_BATCH, entries) + count * sizeof(CreatePlayerBatchEntry)));
        return buf;
    }

    // [Phase 2] 인바운드 멤버십 묶음 — 상대 [0,count)의 playerId를 배치 1패킷으로 직렬화.
    CSerialBuffer* MakeDeletePlayerBatch(CCharacter* const* players, int count)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_DELETE_PLAYER_BATCH::TYPE);
        *buf << static_cast<uint16_t>(count);
        for (int i = 0; i < count; ++i)
            *buf << static_cast<int>(players[i]->_actorId);
        FinalizePacket(buf);
        assert(buf->GetDataSize() ==
            static_cast<int>(offsetof(MSG_S2C_DELETE_PLAYER_BATCH, entries) + count * sizeof(DeletePlayerBatchEntry)));
        return buf;
    }
#endif
}

CGameServer::CGameServer(CMonitorManager& monitor, CGameMonitor& gameMonitor)
    : _mode(ServerMode::GameServer)
    , _monitor(monitor)
    , _gameMonitor(gameMonitor)
    , _running(false)
{
}

CGameServer::~CGameServer()
{
    Stop();
}

bool CGameServer::Init(ServerMode mode, int port, int maxClients)
{
    return Init(mode, port, maxClients, nullptr, 0);
}

bool CGameServer::Init(ServerMode mode, int port, int maxClients,
                        const MapConfig* maps, int32_t mapCount, int workerThreads, int sendWorkers,
                        int rioWorkers, int completionBatch, int sendDepth)
{
    _mode = mode;

    _network = std::make_unique<NetIoModel>(port, maxClients, _mode, _monitor,
                                            workerThreads, sendWorkers, rioWorkers, completionBatch, sendDepth);

    // 게임 서버 모드일 때만 맵 등록
    if (_mode == ServerMode::GameServer)
    {
        if (maps == nullptr || mapCount <= 0)
            return false;

        for (int32_t i = 0; i < mapCount; ++i)
        {
            if (!_mapManager.RegisterMap(maps[i]))
                return false;
        }

        // 첫 번째 맵을 기본 접속 맵으로 설정
        _defaultMapId = maps[0].mapId;

        // 프레임 재사용 컨테이너 reserve (워밍업 realloc 방지)
        // 전체 서버 범위 버퍼도 maxPerChannel 기준 — 초과 시 자연 증가 후 capacity 유지
        int32_t maxPerChannel = maps[0].maxPlayersPerChannel;
        _sessionSlots.assign(maxClients, nullptr);   // 인덱스 직접 접근용 슬롯 (sessionId 인덱스로 접근)
#if USE_DB_WORKER
        _pendingEnter.assign(maxClients, PendingEnter{});   // 대기표 — 위 슬롯과 같은 크기·같은 색인 (불변식 공유)
#endif
        _broadcastBuffer.reserve(maxPerChannel);       // 주변 9섹터 단위
        _eventAroundBuffer.reserve(maxPerChannel);     // 주변 9섹터 단위
        _pendingSectorChanges.reserve(maxPerChannel);  // 전체 서버 프레임 이벤트
        _tickSectorChanges.reserve(maxPerChannel);     // 전체 서버 TickAll 결과
        _tickClampedPlayers.reserve(maxPerChannel);    // 전체 서버 TickAll 결과
        _sectorChangedSet.reserve(maxPerChannel);      // 전체 서버 dedup
        _sectorOutbox.dirtyMovers.reserve(maxClients);              // 전체 서버 dirty (이동/sync 묶음 대상)
    }

    return true;
}

#if USE_GAME_DATA_FILES
bool CGameServer::LoadGameDataFiles(const std::string& dir)
{
    if (!_gameDataFiles.LoadAll(dir))
        return false;

#if USE_MONSTER
    // 스폰 구역은 맵 크기와 대조해야 해서 표만으로는 못 본다 — 맵은 Init 이 먼저 받았다(main.cpp).
    std::vector<CSpawnRegionTable::MapSize> maps;
    _mapManager.ForEachMapConfig([&maps](const MapConfig& mc)
    {
        maps.push_back({ mc.mapId, mc.mapWidth, mc.mapHeight });
    });
    if (!_gameDataFiles.SpawnRegions().CheckMaps(maps))
        return false;
#endif
    return true;
}
#endif

bool CGameServer::Start()
{
    if (!_network->Start())
        return false;

    // 게임 서버 모드일 때만 게임 루프 스레드 시작
    // 에코 테스트는 CIOCPServer 내부에서 자체 처리
    if (_mode == ServerMode::GameServer)
    {
        _running = true;
        _gameThread = std::thread(&CGameServer::GameLoopThread, this);
    }

    const char* modeName = "Unknown";
    switch (_mode)
    {
    case ServerMode::GameCodiEchoTest:    modeName = "GameCodiEchoTest";    break;
    case ServerMode::NetWorkLib_EchoTest: modeName = "NetWorkLib_EchoTest"; break;
    case ServerMode::GameServer:          modeName = "GameServer";          break;
    }
    SLOG_INFO("[GameServer] Started - Mode: {}", modeName);

    return true;
}

void CGameServer::Stop()
{
    _running = false;

    if (_gameThread.joinable())
        _gameThread.join();

#if USE_DB_WORKER
    // 게임 스레드 종료 후(단일 스레드 시점) 전원 최종 저장 → 드레인 → 워커 종료
    if (_dbWorker)
    {
        SaveAllPlayers();
        _dbWorker->Shutdown(0);
        _dbWorker.reset();
    }
#endif

    if (_network)
    {
        _network->ShutdownServer();

        // 미처리 이벤트의 CSerialBuffer 정리
        NetworkEvent event(NetworkEvent::Type::CONNECTED, 0);
        while (_network->PopNetworkEvent(event))
        {
            if (event.pMsg != nullptr)
                event.pMsg->SubRef();
        }
    }

    // 잔여 플레이어 정리 (CGameServer가 생명주기 소유)
    for (CPlayer* player : _sessionSlots)   // nullptr는 delete 무해
    {
        delete player;
    }
    _sessionSlots.clear();

    // 몬스터는 여기서 안 치운다 — 소유가 존이라 _mapManager 가 소멸할 때 존과 같이 해제된다.
    //   게임 스레드가 이미 끝나(위 join) 그 사이에 몬스터를 읽는 코드가 없다.
}

#if USE_DB_WORKER
// ==========================================================================
// DB 저장 파이프라인 (dirty flag 기반 비동기 위치 저장)
// ==========================================================================

bool CGameServer::InitDB(const DBConfig& dbConfig, int savePeriodSec, int workerCount, int queueMax)
{
    _dbWorker = std::make_unique<CDBWorker>(_monitor);
#if USE_INVENTORY
    _dbWorker->SetItemSaveCounters(&_gameMonitor._dbItem);   // Start 전에 — 워커 스레드가 뜨기 전에 자리를 알려 둔다
#endif
    if (!_dbWorker->Start(dbConfig, workerCount, queueMax))
    {
        SLOG_ERROR("[DB] InitDB failed - check MySQL connection/config");
        _dbWorker.reset();
        return false;
    }
    _dbSavePeriodFrames = (savePeriodSec > 0 ? savePeriodSec : 10) * FRAME_PER_SEC;
    _dbSaveFrameCounter = 0;
    return true;
}

bool CGameServer::ShouldSave(const CPlayer* player) const
{
    // 저장 대상 판정(주기·종료 공통):
    //   MOVING   = 위치가 계속 변하는 중 → 저장 후 dirty가 리셋돼도 매 주기 재저장 필요
    //   _dbDirty = 정지/접속/존변경/클램프로 생긴 미저장 변경(일회성, 저장 후 리셋)
    return player->_moveState == MoveState::MOVING || player->_dbDirty;
}

DBSaveJob CGameServer::MakeSaveJob(const CPlayer* player) const
{
    DBSaveJob job;
    job.accountId = player->_accountId;
    job.x         = player->_x;
    job.y         = player->_y;
    job.mapId     = CMapManager::GetMapIdFromZoneId(player->_zoneId);
    job.level     = player->_level;
    job.exp       = player->_exp;
    return job;
}

void CGameServer::EnqueuePlayerSave(CPlayer* player)
{
    // 수용된 경우에만 dirty 해제 — 백프레셔로 드롭됐는데 지워버리면 그 변경은 복원 통로가 없다.
    if (_dbWorker->Enqueue(MakeSaveJob(player)))
        player->_dbDirty = false;
}

void CGameServer::TickPeriodicSave()
{
    if (++_dbSaveFrameCounter < _dbSavePeriodFrames)
        return;
    _dbSaveFrameCounter = 0;

    // 서버 메모리 = 진실. 저장 대상(ShouldSave)만 배치에 모아 워커에 1회 핸드오프.
    //   thread_local 재사용으로 매 주기 할당 회피 (게임 스레드 단독 접근).
    thread_local std::vector<DBSaveJob> batch;
    thread_local std::vector<CPlayer*>  owners;    // batch와 나란한 소유자 (드롭 시 dirty 복원용)
    thread_local std::vector<size_t>    dropped;
    batch.clear();
    owners.clear();
    for (CPlayer* player : _sessionSlots)
    {
        if (player == nullptr)
            continue;
        if (ShouldSave(player))
        {
            batch.push_back(MakeSaveJob(player));
            owners.push_back(player);
            player->_dbDirty = false;   // 저장 요청 후 리셋 (드롭되면 아래에서 되돌림)
        }
    }
    _dbWorker->EnqueueBatch(batch, &dropped);   // lock 1회 + notify 1회

    // 백프레셔로 버려진 잡은 dirty를 되살려 다음 주기에 재시도한다.
    //   되돌리지 않으면 정지(IDLE) 상태 플레이어의 변경은 ShouldSave 대상에서 빠져 영영 유실된다.
    //   호출은 전부 게임 스레드 단독이라 owners 포인터는 이 시점에 유효하다.
    for (size_t idx : dropped)
        owners[idx]->_dbDirty = true;
}

void CGameServer::SaveAllPlayers()
{
    // 종료는 유실 방지 우선 — dirty/MOVING 무관하게 전원 최종 저장(안전망).
    // 주기 저장(ShouldSave 필터)과 목적이 다름: 여기선 dirty 추적을 신뢰하지 않고 무조건 다 씀.
    std::vector<DBSaveJob> batch;
    batch.reserve(_sessionSlots.size());
    for (CPlayer* player : _sessionSlots)
    {
        if (player == nullptr)
            continue;
        batch.push_back(MakeSaveJob(player));
        player->_dbDirty = false;
    }
    _dbWorker->EnqueueBatch(batch);   // 배치 1회 핸드오프
}

// ==========================================================================
// DB 왕복 — 캐릭터 불러오기
//   가는 길: RequestCharacterLoad → 워커 큐(저장과 같은 큐 — 계정별 순서 보장)
//   오는 길: 워커 → 회신 큐 → ProcessDbResults(틱마다 통째로 꺼냄) → OnCharacterLoaded
// ==========================================================================

static int64_t SteadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool CGameServer::RequestCharacterLoad(int64_t sessionId, int64_t accountId)
{
    DBLoadJob job;
    job.accountId = accountId;
    job.sessionId = sessionId;       // 결과가 돌아왔을 때 주인을 찾는 열쇠 — 세션 번호 "전체"
    job.requestNs = SteadyNowNs();

    // 거절되면 결과가 오지 않는다 → requested 에 넣지 않고 따로 센다(등식이 안 흐려지게).
    //   호출부는 false 를 받으면 그 세션을 끊어야 한다 — 매달아 두면 영영 안 들어온다.
    if (_dbWorker->EnqueueLoad(job))
    {
        _gameMonitor._enter._requested.Inc();
        return true;
    }
    _gameMonitor._enter._rejected.Inc();
    return false;
}

// 상태별 수와 게이지를 한 자리에서만 움직인다 — 세는 자리가 갈라지면 한 곳만 빠져도 영영 안 맞는다.
void CGameServer::AdjustPendingCount(EnterState state, int delta)
{
    if (state == EnterState::LOADING)
    {
        _pendingLoadingCount += delta;
        _gameMonitor._enter._pending.Store(_pendingLoadingCount);
    }
    else
    {
        _pendingWaitingCount += delta;
        _gameMonitor._enter._waiting.Store(_pendingWaitingCount);
    }
}

void CGameServer::SetPendingEnter(int64_t sessionId, int64_t accountId, int32_t actorId, EnterState state)
{
    const uint16_t idx = CSession::ExtractIndex(sessionId);
    assert(idx < _pendingEnter.size());   // FindPlayer 와 같은 불변식 — 두 배열은 Init 에서 같은 크기로 잡힌다
    PendingEnter& slot = _pendingEnter[idx];

    if (slot.sessionId != -1)
    {
        // 있을 수 없다 — 같은 자리의 끊김 이벤트는 다음 접속보다 먼저 온다(_sessionSlots 도 같은 순서에 기댄다).
        //   그래도 덮어쓰면 스스로 아문다: 옛 조회의 결과는 세션 번호가 안 맞아 orphaned 로 세어진다.
        SLOG_ERROR("[DB] pending enter overwritten (slot={}, old session={}, new session={})",
                   idx, slot.sessionId, sessionId);
        AdjustPendingCount(slot.state, -1);
    }

    slot.sessionId = sessionId;
    slot.accountId = accountId;
    slot.actorId   = actorId;
    slot.state     = state;
    AdjustPendingCount(state, +1);
}

// [입장 대기] → [불러오는 중]. 번호 둘은 이때 정해지므로 여기서 채운다.
//   상태 검사는 RecvEnterGame 앞 문턱과 겹친다. 일부러 남긴 둘째 겹이고 역할이 다르다 —
//   앞 문턱은 "번호를 뽑고 조회를 던지는 일 자체를 하지 말 것"이고, 여기는 "게이지 둘을 움직이기 전에
//   정말 [입장 대기]였나"를 보는 이 함수의 전제다. 이 줄이 없으면 [불러오는 중]에 또 불렸을 때
//   입장 대기 수가 음수로 내려간다(돌연변이로 확인: 요청 셋에 입장 대기 -2 · 불러오는 중 2).
//   ※ 지금은 앞 문턱이 먼저 막아서 이 검사에 도달하는 길이 없다 — 시험으로는 못 태운다.
bool CGameServer::PromotePendingEnter(int64_t sessionId, int64_t accountId, int32_t actorId)
{
    const uint16_t idx = CSession::ExtractIndex(sessionId);
    assert(idx < _pendingEnter.size());
    PendingEnter& slot = _pendingEnter[idx];

    if (slot.sessionId != sessionId || slot.state != EnterState::WAITING_ENTER)
        return false;

    AdjustPendingCount(EnterState::WAITING_ENTER, -1);
    slot.accountId = accountId;
    slot.actorId   = actorId;
    slot.state     = EnterState::LOADING;
    AdjustPendingCount(EnterState::LOADING, +1);
    return true;
}

bool CGameServer::TakePendingEnter(int64_t sessionId, PendingEnter& out)
{
    const uint16_t idx = CSession::ExtractIndex(sessionId);
    assert(idx < _pendingEnter.size());
    PendingEnter& slot = _pendingEnter[idx];

    // 빈 자리(-1)이거나 같은 자리에 들어온 다른 세션 — 세션 번호 "전체"가 같아야 주인이다.
    if (slot.sessionId != sessionId)
        return false;

    out  = slot;
    AdjustPendingCount(slot.state, -1);
    slot = PendingEnter{};
    return true;
}

void CGameServer::ProcessDbResults()
{
    if (!_dbWorker)
        return;

    // _dbResults 는 직전 틱에 모두 소비되어 비어 있다 — 빈 벡터를 워커 쪽과 맞바꾼다(락 1회).
    _dbWorker->SwapOutResults(_dbResults);
    if (_dbResults.empty())
        return;

    const int64_t nowNs = SteadyNowNs();

    static_assert(DB_JOB_KIND_COUNT == (USE_INVENTORY ? 3 : 2), "DBJobKind 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
    for (const DBResult& result : _dbResults)
    {
        switch (result.kind)
        {
        case DBJobKind::LOAD_CHARACTER:
            OnCharacterLoaded(result, nowNs);
            break;

        case DBJobKind::SAVE_POSITION:   // 저장은 편도 — 결과를 돌려주지 않는다. 여기 오면 워커 쪽 결함이다
#if USE_INVENTORY
        case DBJobKind::SAVE_ITEM_SLOT:  // 칸 저장도 편도 — 성공·실패는 워커가 _dbItem 에 센다
#endif
        case DBJobKind::COUNT:
            SLOG_ERROR("[DB] unexpected result kind {}", static_cast<int>(result.kind));
            break;
        }
    }
    _dbResults.clear();
}

void CGameServer::OnCharacterLoaded(const DBResult& result, int64_t nowNs)
{
    CGameMonitor::EnterCounters& enter = _gameMonitor._enter;

    // 지연은 주인이 있든 없든 잰다 — "꺼낸 결과 전부"가 분모라야 아래 갈래 합과 맞대 볼 수 있다.
    enter.RecordLatencyUs((nowNs - result.requestNs) / 1000);

    // 주인 확인 — 대기표의 세션 번호 "전체"가 같아야 한다(같은 자리에 다른 사람이 들어왔으면 안 맞는다).
    //   불러오는 사이에 나갔으면 OnDisconnected 가 대기표를 이미 비웠다 → 결과는 버린다.
    //   코드(찾음·없음·실패)는 보지 않고 이 갈래 하나로 센다.
    PendingEnter pending;
    if (!TakePendingEnter(result.sessionId, pending))
    {
        enter._orphaned.Inc();
        return;
    }

    // [2단계] 결과를 "어느 존에 · 어느 자리에 · 저장할까"로 바꾼 뒤 스폰한다.
    //   존 조회(FindOrCreateChannel)는 찾음·없음 갈래에서만 한다 — 채널을 만들 수도 있는 함수라
    //   들여보내지 않을 갈래(주인 없음·실패)에서는 부르면 안 된다.
    CZone* zone = nullptr;
    EnterPlacement place;   // 기본값 = 무작위 자리 · 다음 주기에 저장

    static_assert(static_cast<int>(DBResultCode::COUNT) == 3, "DBResultCode 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
    switch (result.code)
    {
    case DBResultCode::FOUND:
        enter._found.Inc();

        // 레벨·경험치는 자리(좌표·맵) 판정과 무관하게 행의 값을 그대로 쓴다.
        //   좌표가 못 쓸 값이어도 레벨까지 버릴 이유는 없다 — 다른 사실이다.
        //   ★ 표 범위 검사와 레벨 정산은 여기가 아니라 SpawnPlayer 가 한다 — 새 캐릭터·못 읽음 갈래도 거기를 지난다.
        place.level = result.level;
        place.exp   = result.exp;
#if USE_INVENTORY
        place.loaded = &result;   // 가방 줄 — 검사와 적재는 SpawnPlayer 가 한다(이 함수 안에서 동기로 불리므로 가리켜도 안전)
#endif

        // 존 먼저, 좌표는 그 존의 크기로 본다. 저장된 맵이 없으면 zone 이 nullptr 로 남아 아래에서 기본 맵을 구한다.
        zone = _mapManager.FindOrCreateChannel(result.mapId);
        if (zone != nullptr && IsValidSpawnPos(result.x, result.y, zone->GetMapWidth(), zone->GetMapHeight()))
        {
            place.hasPos  = true;
            place.x       = result.x;
            place.y       = result.y;
            place.dbDirty = false;   // DB 와 같은 값으로 들어간다 — 움직이기 전엔 다시 쓸 것이 없다
        }
        else
        {
            // 맵이 없어졌거나 좌표가 맵 밖·숫자 아님 → 무작위 자리. 그 자리는 DB 와 다르므로 dbDirty 는 기본값(true) 그대로.
            enter._fallback.Inc();
            SLOG_WARN("[DB] saved position unusable -> random spawn (account={}, map={}, x={}, y={})",
                      result.accountId, result.mapId, result.x, result.y);
        }
        break;

    case DBResultCode::NOT_FOUND:
        enter._newCharacter.Inc();   // 새 캐릭터 — 기본 맵 무작위 자리. 행은 첫 저장(UPSERT)이 만든다
#if USE_INVENTORY
        // ★ 캐릭터 행이 없어도 가방 줄은 있을 수 있다(첫 저장 전에 서버가 죽은 계정) — 워커가 읽어 왔다. 그대로 넣는다.
        place.loaded = &result;
#endif
        break;

    case DBResultCode::FAILED:
        // "없음"과 절대 섞지 않는다 — 들여보내면 새 캐릭터로 스폰되고 다음 저장이 진짜 행을 덮는다.
        //   재시도는 하지 않는다: 워커가 이미 재연결하고 한 번 더 해 봤다. 클라가 다시 접속하는 것이 재시도다.
        enter._failed.Inc();
        SLOG_ERROR("[DB] character load failed -> disconnect (account={}, session={})", result.accountId, result.sessionId);
        _network->RequestDisconnectSession(result.sessionId);
        return;

    case DBResultCode::COUNT:
        // 값이 아니라 개수 표시 — 올 수 없다. 대기표는 이미 비웠으니 세션을 매달아 두지 않고 끊는다.
        SLOG_ERROR("[DB] unexpected load result code (session={})", result.sessionId);
        _network->RequestDisconnectSession(result.sessionId);
        return;
    }

    if (zone == nullptr)
        zone = _mapManager.FindOrCreateChannel(_defaultMapId);

    if (SpawnPlayer(result.sessionId, pending.actorId, pending.accountId, zone, place) == nullptr)
    {
        // 갈래 계수기는 위에서 이미 올랐다. 여기 오는 길은 채널 생성 실패뿐이라 계수기를 따로 두지 않는다(로그만).
        SLOG_ERROR("[DB] spawn failed after load -> disconnect (account={}, session={})", result.accountId, result.sessionId);
        _network->RequestDisconnectSession(result.sessionId);
    }
}
#else
bool CGameServer::InitDB(const DBConfig&, int, int, int) { return true; }   // 토글 OFF: no-op
#endif

// ==========================================================================
// 게임 루프
// ==========================================================================

void CGameServer::GameLoopThread()
{
    using Clock = std::chrono::steady_clock;

    CoreAffinity::PinGameThread();   // 게임루프 전용 코어 고정 (격리 off면 no-op)

    // CPU 점유율 측정용: 자기 실핸들을 복제해 모니터에 등록 (진단정리 6 사각지대 보강)
    // GetCurrentThread()는 의사핸들(호출 스레드 기준)이라 HTTP 스레드에서 못 씀 → 실핸들로 복제
    {
        _monitor._gameLoopThreadHandle = Platform::CaptureCurrentThreadCpu();
    }

    auto prevTime = Clock::now();

    while (_running)
    {
        auto frameStart = Clock::now();

        // deltaTime 계산
        float deltaTime = std::chrono::duration<float>(frameStart - prevTime).count();
        prevTime = frameStart;
        ++_frameCount;   // 이동 예산의 경과 시간 산출 기준 (틱당 FRAME_INTERVAL_MS)

        // 1) 네트워크 이벤트 전부 소비
        _monitor._gameLoop._eventQueueSize.Store(static_cast<int64_t>(_network->GetEventQueueSize()));
        auto phaseT1 = Clock::now();
        ProcessNetworkEvents();

#if USE_DB_WORKER
        // 1-0) DB 회신 소비 — 워커가 돌려준 결과(캐릭터 불러오기)를 통째로 꺼낸다 (락 1회).
        //   네트워크 이벤트 "뒤"에 두는 이유: 같은 틱에 그 세션의 끊김과 결과가 같이 오면 끊김을 먼저
        //   처리해 결과가 주인 없는 것으로 버려지게 한다. 거꾸로면 들여보냈다가 곧바로 지우게 된다.
        //   네트워크 구간(phaseT1~T2) "안"에 두는 이유: 접속 처리(스폰) 비용이 원래 이 구간에 있었다.
        //   결과를 받아 스폰하게 바뀐 뒤에도 _phaseNetworkUs 가 같은 것을 재도록 자리를 맞춘다.
        ProcessDbResults();
#endif
        auto phaseT2 = Clock::now();

#if USE_MONSTER
        // 1-1) 몬스터 — 새 존에 스폰(2단계). 3단계에서 AI 판단이 여기 붙는다.
        //   좌표 갱신(2) 앞에 두는 이유: AI 가 정한 방향이 같은 틱에 반영돼야 한 틱씩 밀리지 않는다.
        TickMonsters();
#endif

        // 2) 게임 로직 갱신 (좌표 이동 + 섹터 변경 감지 + 경계 클램핑)
        _tickSectorChanges.clear();
        _tickClampedPlayers.clear();
        _mapManager.TickAll(deltaTime, _tickSectorChanges, _tickClampedPlayers);

        // 3) 섹터 변경 배치 처리 (RecvMoveStop + TickAll 병합 → 삽입 시 중복 차단)
        for (const auto& change : _tickSectorChanges)
        {
            PushSectorChange(change.character, change.oldSectorX, change.oldSectorY);
        }

        // [계측] 멤버십(섹터이동 CREATE/DELETE) 송신 시간 — 구간 1쌍으로만 측정.
        //   per-call 금지: 멤버십은 ~1.04M/s라 건당 now()를 넣으면 측정 오버헤드가 대상을 오염시킴.
        auto membT0 = Clock::now();
        for (const auto& change : _pendingSectorChanges)
        {
            // 출발 섹터 == 현재 섹터이면 원위치 복귀 → 브로드캐스트 불필요
            if (change.oldSectorX == change.character->_sectorX &&
                change.oldSectorY == change.character->_sectorY)
                continue;

            CZone* zone = _mapManager.GetZone(change.character->_zoneId);
            if (zone != nullptr)
            {
                ProcessSectorChange(zone, change.character,
                                    change.oldSectorX, change.oldSectorY);
            }
        }
        // 같은 틱에 함께 이동한 쌍은 위 루프가 서로를 놓친다(상대는 이미 새 섹터로 옮겨간 뒤라
        // 이탈/진입 섹터 조회에 안 걸림) → 누락된 CREATE/DELETE만 보충. 보정 비용도 멤버십 구간에 포함.
        FixSameTickMoverPairs();

        _tickMembershipUs += std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - membT0).count();
        _pendingSectorChanges.clear();
        _sectorChangedSet.clear();

        // 3-1) 맵 경계 클램핑으로 정지된 플레이어에게 MOVE_STOP 브로드캐스트
        for (CCharacter* player : _tickClampedPlayers)
        {
#if USE_DB_WORKER
            if (CPlayer* p = AsPlayer(player))
                p->_dbDirty = true;   // 경계 클램프로 위치 변경 → 저장 대상
#endif
#if USE_MONSTER
            // 벽에 닿은 몬스터는 다음 틱에 바로 다시 판단하게 한다.
            //   AI 는 맵 경계를 모른다(MonsterAI 는 맵 크기를 안 받는다). 그래서 배회 방향이
            //   바깥을 향하면 클램프로 멈춘 채 다음 판단(최대 3초)까지 벽에 붙어 있게 된다.
            //   실측으로 뱀의 50%가 가장자리 띠에 몰렸다(면적 비율은 36%). 여기서 판단을
            //   앞당기면 40ms 만에 새 방향을 고른다 — 4방향 중 대부분이 안쪽이라 곧 벗어난다.
            if (player->_kind == CharacterKind::MONSTER)
                static_cast<CMonster*>(player)->_nextThinkFrame = 0;
#endif
            CZone* zone = _mapManager.GetZone(player->_zoneId);
            if (zone != nullptr)
            {
                NotifyMoveStop(zone, player, false);  // 본인 포함
            }
        }

#if USE_COMBAT
        // 3-2) 부활 대기자 훑기 (게임 로직 페이즈에 계상 — 브로드캐스트를 동반한다)
        TickRespawn();
#endif

#if USE_DROP
        // 3-3) 땅의 아이템 사라지기 — 섹터 변경 대기열을 비운 뒤라 "같은 틱 이동자" 보정이 필요 없다
        TickGroundItems();
#endif

        auto phaseT3 = Clock::now();

        // 4) 주기적 위치 동기화 (MOVING 플레이어 → 주변 브로드캐스트)
        ++_syncFrameCount;
        if (_syncFrameCount >= SYNC_INTERVAL_FRAMES)
        {
            _syncFrameCount = 0;
            int64_t inGame = 0;   // 게임 중인 플레이어 수 — 이미 도는 이 순회에 얹는다(존마다 덧셈 한 번)
            _mapManager.ForEachZone([&](CZone* zone)
            {
                inGame += zone->GetPlayerCount();
                for (CCharacter* player : zone->GetCharacters())
                {
                    if (player->_moveState != MoveState::MOVING)
                        continue;

                    // 델타 동기화: 마지막 동기화 이후 충분히 이동한 경우만 전송
                    float dx = player->_x - player->_lastSyncX;
                    float dy = player->_y - player->_lastSyncY;
                    if (dx * dx + dy * dy < SYNC_DISTANCE_THRESHOLD_SQ)
                        continue;

                    player->_lastSyncX = player->_x;
                    player->_lastSyncY = player->_y;

                    NotifyMoveSync(zone, player);  // 본인 포함 (이동 중 클라-서버 좌표 드리프트 보정)
                }
            });
            _gameMonitor._enter._inGame.Store(inGame);   // 게이지 — 동접 = 게임 중 + 불러오는 중 (GameMonitor.h)
        }

#if USE_BROADCAST_BUNDLE
        // [digest] 이번 틱 보류물(이동 번들+채팅)을 수신섹터 기준 연접으로 배포 (아래 flush가 묶어 보냄)
        FlushSectorSends();
#elif USE_SECTOR_AGGREGATION
        // [묶음] 이번 틱 dirty를 섹터별 묶음으로 송신 (RequestSendMsg는 Deferred → 아래 flush가 묶어 보냄)
        FlushSectorUpdates();
#endif

        // [coalescing] 이번 틱에 쌓인 송신을 세션당 1회 WSASend로 flush (broadcast_sync 페이즈에 계상)
        // [계측] flush 구간을 별도 측정 → 비용종류별 "송신(WSASend)" 분리. _phaseBroadcastSyncUs는 그대로 둠(이 값을 포함).
        auto flushT0 = Clock::now();
        _network->FlushPendingSends();
        auto flushT1 = Clock::now();

        auto phaseT4 = Clock::now();

        // 구간별 시간 기록 (마이크로초 누적)
        _monitor._gameLoop._phaseNetworkUs.Add(std::chrono::duration_cast<std::chrono::microseconds>(phaseT2 - phaseT1).count());
        _monitor._gameLoop._phaseGameLogicUs.Add(std::chrono::duration_cast<std::chrono::microseconds>(phaseT3 - phaseT2).count());
        _monitor._gameLoop._phaseBroadcastSyncUs.Add(std::chrono::duration_cast<std::chrono::microseconds>(phaseT4 - phaseT3).count());

        // [계측] 비용종류별 — 틱 내 누적분을 1회씩 원자 반영 후 리셋 (송신은 flush 구간 직접 측정)
        _monitor._gameLoop._broadcastGatherUs.Add(_tickBroadcastGatherUs);
        _monitor._gameLoop._broadcastEnqueueUs.Add(_tickBroadcastEnqueueUs);
        _monitor._gameLoop._flushSendUs.Add(std::chrono::duration_cast<std::chrono::microseconds>(flushT1 - flushT0).count());
        _monitor._gameLoop._membershipSends.Add(_tickMembershipSends);      // 멤버십 변경 복사량(횟수)
        _monitor._gameLoop._membershipCostUs.Add(_tickMembershipUs);        // 멤버십 변경 송신 시간
        _monitor._gameLoop._membershipPairFixes.Add(_tickPairFixes);        // 같은 틱 이동자 쌍 보정 건수
        _monitor._gameLoop._moveBudgetRejects.Add(_tickMoveBudgetRejects);  // 이동 예산 거부 건수
        _tickBroadcastGatherUs = 0;
        _tickBroadcastEnqueueUs = 0;
        _tickMembershipSends = 0;
        _tickMembershipUs = 0;
        _tickPairFixes = 0;
        _tickMoveBudgetRejects = 0;

        // [계측 오버헤드 절감] handle-latency 지역 누적분 → 전역 1회 반영
        _monitor._gameLoop.FlushHandleLatency();

        // 5) 빈 동적 채널 정리
        ++_cleanupFrameCount;
        if (_cleanupFrameCount >= CLEANUP_INTERVAL_FRAMES)
        {
            _cleanupFrameCount = 0;
#if USE_MONSTER
            // 존을 지우기 직전에 그 존의 몬스터를 대기열에서 뺀다 — 안 빼면 존과 함께 해제된 뒤 대기열에 죽은 주소가 남는다.
            _mapManager.CleanupEmptyChannels([this](CZone* zone)
            {
#if USE_DROP
                ForgetGroundItems(zone);   // 땅의 아이템은 세기만 — 벡터는 존과 함께 사라진다(알릴 사람이 없다)
#endif
                DespawnMonsters(zone);
            });
#else
            _mapManager.CleanupEmptyChannels(nullptr);
#endif
        }

#if USE_DB_WORKER
        // 5-1) DB 주기 저장 — dirty(또는 MOVING) 플레이어만 워커에 enqueue (틱 임계경로 밖 I/O)
        TickPeriodicSave();
#endif

        // 6) Tick 시간 기록 + 프레임 제한
        auto frameEnd = Clock::now();
        double tickMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
        _monitor._gameLoop.RecordTickTime(tickMs);

        int sleepMs = FRAME_INTERVAL_MS - static_cast<int>(tickMs);
        if (sleepMs > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }
}

// ==========================================================================
// 이벤트 디스패치
// ==========================================================================

void CGameServer::ProcessNetworkEvents()
{
    // 프레임 진입 시점 스냅샷을 통째로 스왑 (락 1회). 처리 중 도착분은 다음 프레임으로 이월
    // _localEvents는 직전 프레임에 모두 소비되어 비어 있음 (멤버 재사용으로 deque 블록 보존)
    _network->SwapOutNetworkEvents(_localEvents);

    while (!_localEvents.empty())
    {
        NetworkEvent& event = _localEvents.front();
        switch (event.type)
        {
        case NetworkEvent::Type::CONNECTED:
            OnConnected(event.sessionId);
            break;

        case NetworkEvent::Type::DISCONNECTED:
            OnDisconnected(event.sessionId);
            break;

        case NetworkEvent::Type::RECEIVED:
            OnReceived(event.sessionId, event.pMsg);
            // handle-latency: enqueue(recv) → 처리완료(응답 송신 포함) 시간.
            // 틱 게이트 대기 + 핸들러 비용 = RTT의 서버 기여분. RECEIVED만 측정.
            if (event.enqueueTimeNs != 0)
            {
                int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                _monitor._gameLoop.RecordHandleLatencyLocal(
                    static_cast<double>(nowNs - event.enqueueTimeNs) / 1.0e6);
            }
            break;
        }
        _localEvents.pop();
    }
}

bool CGameServer::IsValidSpawnPos(float x, float y, int32_t mapWidth, int32_t mapHeight)
{
    // 이동 쪽 클램프(CZone::Tick)와 같은 범위 — 0 <= 좌표 < 맵 크기.
    //   식이 다르면 "입장은 되는데 첫 틱에 클램프되는" 자리가 생긴다.
    //   NaN·무한대를 먼저 거른다 — NaN 은 비교가 전부 거짓이라 범위 검사만으로는 뜻이 안 읽힌다.
    return std::isfinite(x) && std::isfinite(y)
        && x >= 0.0f && x < static_cast<float>(mapWidth)
        && y >= 0.0f && y < static_cast<float>(mapHeight);
}

CPlayer* CGameServer::SpawnPlayer(int64_t sessionId, int32_t actorId, int64_t accountId,
                                  CZone* zone, const EnterPlacement& place)
{
    if (zone == nullptr)
        return nullptr;

    // 플레이어 생성 + ID/표시 속성 부여 (CGameServer가 생명주기 소유)
    //   번호는 호출부가 준다 — 접속 때 이미 뽑았다(조회 열쇠로 먼저 필요했다).
    CPlayer* player = new CPlayer();
    player->_actorId = actorId;
    player->_displayChar = CalcDisplayChar(player->_actorId);
    player->_colorIndex = CalcColorIndex(player->_actorId);
    if (_playerSpeed > 0)
        player->_speed = _playerSpeed;   // 게임용 속도 — 0 이면 생성자의 25~50 무작위(부하 시험 기준) 그대로

    // 저장된 자리가 있으면 그 자리에, 없으면 무작위 자리에
    const bool entered = place.hasPos ? zone->EnterZoneAt(player, place.x, place.y)
                                      : zone->EnterZone(player);
    if (!entered)
    {
        delete player;
        return nullptr;
    }

    // 경계 매핑 등록
    player->_sessionId = sessionId;
    _sessionSlots[CSession::ExtractIndex(sessionId)] = player;

    // 델타 동기화 기준 좌표 초기화 (스폰 직후 불필요한 동기화 방지)
    player->_lastSyncX = player->_x;
    player->_lastSyncY = player->_y;

    // 레벨·경험치 — 패킷을 보내기 "전"에 세운다. DB 토글 밖에 두는 이유는 토글을 끈 빌드에서도
    //   값이 서 있어야 하기 때문이다(그 빌드에선 늘 기본값 1·0 이고 메모리에서만 돈다).
    player->_level = place.level;
    player->_exp   = place.exp;

#if USE_DB_WORKER
    // 1단계 임시: playerId를 계정 식별자로 사용 (2단계는 접속 핸드셰이크로 수신)
    player->_accountId = accountId;
    // 새 캐릭터·무작위 자리면 true(첫 주기에 저장 → 신규 행 INSERT 유도), DB 에서 불러온 자리 그대로면 false
    player->_dbDirty   = place.dbDirty;
#else
    (void)accountId;
#endif

#if USE_INVENTORY && USE_DB_WORKER
    // ── 불러온 가방 — 스탯을 세우기 "전"에 채운다 ──
    //   ★ 무기 칸(900)도 여기서 들어온다. 아래 레벨 정산·스탯 세우기(RecalcStats)가 무기 몫을 더하므로,
    //     이 줄이 그보다 뒤에 있으면 무기를 끼운 채 나갔다 들어온 사람이 맨손 공격력으로 입장한다
    //     (컴파일도 되고 화면도 멀쩡하다 — 입장 직후 S2C_MY_STATS 의 공격력만 틀린다).
    //   가방과 계수기만 만지는 일이라 앞당겨도 다른 것이 안 바뀐다. 전체 목록(SendInventory)은 그대로 아래에서 보낸다.
    if (place.loaded != nullptr)
        LoadInventoryRows(player, *place.loaded);
#endif

#if USE_EXP
    // ── 레벨 정산 — 스탯을 표에서 다시 세우고, 불러온 값이 표와 어긋나면 바로잡는다 ──
    //   ★ SendZoneInfo 보다 앞이다. SendCreateMyPlayer 가 곧바로 최대 체력을 실어 보내므로
    //     여기서 안 세우면 첫 화면의 막대가 생성자 기본값(100)으로 나간다.
    //   갈래 둘 — ⑴ 레벨이 표 밖(표를 줄인 패치) ⑵ 경험치가 이미 다음 레벨 몫을 넘음
    //     (표의 필요치를 낮춘 패치, 또는 만렙 상한에 걸려 안 오른 채 저장된 행).
    //     둘 다 "행은 멀쩡한데 표가 바뀐" 경우라 평소엔 한 번도 안 도는 갈래다.
    const bool clamped = ClampLevelToTable(player);
    if (clamped)
        _gameMonitor._exp._levelClamped.Inc();

    // 0 을 줘서 레벨만 정산한다 — 경험치는 한 톨도 안 늘고, 올릴 수 있으면 올린다.
    const bool settled = GainExp(player, 0);

#if USE_DB_WORKER
    // 바로잡은 값은 DB 와 다르다 — 안 켜면 다음 주기 저장이 건너뛰어 영영 틀린 채로 남는다.
    if (clamped || settled)
        player->_dbDirty = true;
#endif
#endif

#if USE_COMBAT
    // ── 스탯 세우기 — 레벨 정산 뒤 ──
    //   GainExp 는 레벨이 올랐을 때만 스탯을 세운다. 안 올랐으면 생성자 기본값 그대로라 여기서 세운다.
    //   지금은 스폰이 늘 만피라 refillHp=true — 죽은 채로 들어오는 경로가 생기면 그때 가른다.
    //   ★ SendZoneInfo 보다 앞이다(위 레벨 정산 주석과 같은 이유 — CREATE 가 최대 체력을 싣는다).
    //   USE_EXP 밖인 이유는 RecalcStats 선언 주석(GameServer.h).
    RecalcStats(player, true);
#endif

    // 1) 존 메타 정보 전송
    SendZoneInfo(player, zone);

    // 2) 본인에게 내 캐릭터 생성
    SendCreateMyPlayer(player);

#if USE_EXP
    // 3) 본인에게 레벨·경험치 — CREATE 뒤에 보낸다(클라가 내 액터를 알고 나서 숫자를 채우게)
    SendExp(player);
#endif

#if USE_INVENTORY
    // 3-0) 불러온 가방은 위에서 이미 채웠다(스탯 세우기 앞 — 무기 몫 때문). 여기서는 보내기만 한다.
    // 3-1) 본인에게 가방 전체 — 빈 가방이어도 한 장. 존 이동 때는 안 보낸다(가방이 안 바뀐다).
    //   ★ 부하 더미가 이 패킷을 받는 유일한 길이 여기다(더미는 아이템을 안 받는다). 그래서 더미 시험과
    //     "등록을 빼면 떨어지나" 돌연변이가 이 한 장에 매달려 있다 — "비었으면 안 보내자"로 바꾸면 둘 다
    //     조용히 아무것도 안 보게 된다.
    SendInventory(player);
#endif

#if USE_COMBAT
    // 3-2) 본인에게 최종 공격력·방어력 — 가방 전체 뒤(클라가 무기 칸을 알고 나서 숫자를 채우게).
    //   ★ 부하 더미도 이 한 장을 반드시 받는다 — 가방 전체와 같은 함정(등록을 빼면 전원 떨어진다).
    SendMyStats(player);
#endif

    // 4) 주변 상호 CREATE 브로드캐스트
    BroadcastEnterZone(zone, player, SpawnReason::CONNECT);

    return player;
}

void CGameServer::OnConnected(int64_t sessionId)
{
    // 이미 등록된 세션이면 무시 (중복 접속 방어)
    if (FindPlayer(sessionId) != nullptr)
    {
        _network->RequestDisconnectSession(sessionId);
        return;
    }

#if USE_LOGIN
    // [입장 요청] 접속만으로는 아무것도 안 만든다 — 캐릭터 번호도 계정 번호도 조회도
    //   입장 요청(C2S_ENTER_GAME)을 받아야 정해진다. 여기서는 대기표에 자리만 잡는다.
    //   ★ 자리를 안 잡으면 이 세션이 어느 표에도 없어서, 유령 플레이어를 잡는 유일한 장치인
    //     "동접 = 게임 중 + 불러오는 중 + 입장 대기" 등식이 늘 틀린 채로 울린다.
    //   ※ 입장 요청을 끝내 안 보내는 세션은 여기 남는다. 네트워크 계층의 유휴 타임아웃은
    //     하트비트만 보내도 갱신되므로 안 끊는다 — 그래서 게이지(mmo_enter_waiting)로 지켜본다.
    //     값이 올라가서 안 내려오면 그때 쓸어내기를 넣는다(지금 넣으면 한 번도 안 도는 갈래다).
    SetPendingEnter(sessionId, 0, 0, EnterState::WAITING_ENTER);
#else
    // 번호는 접속 때 뽑는다 — 지금은 계정 번호 = 캐릭터 번호(임시)라 조회 열쇠로 먼저 필요하다.
    //   못 들어간 접속은 번호 하나를 버린다(단조 증가 카운터라 무해 — 몬스터도 같은 카운터를 쓴다).
    const int32_t actorId = AllocPlayerId();

#if USE_DB_WORKER
    if (_dbWorker)
    {
        // [DB 왕복 2단계] 접속 → 불러오는 중 → 게임 중. 여기서는 대기표만 남기고 조회를 던진다.
        //   플레이어도 존 입장도 패킷도 없다 — 스폰은 결과가 와야 한다(OnCharacterLoaded).
        //   그 사이에 온 패킷은 OnReceived 가 조용히 버린다(FindPlayer == nullptr).
        const int64_t accountId = actorId;
        if (!RequestCharacterLoad(sessionId, accountId))
        {
            // 거절 = 결과가 안 온다. 매달아 두면 영영 안 들어오므로 끊는다(rejected 는 위에서 셌다).
            _network->RequestDisconnectSession(sessionId);
            return;
        }
        SetPendingEnter(sessionId, accountId, actorId, EnterState::LOADING);
        return;
    }
#endif

    // DB 워커가 없으면(토글 OFF·미기동) 예전 그대로 곧바로 스폰 — 기본 맵의 여유 채널, 무작위 자리.
    CZone* zone = _mapManager.FindOrCreateChannel(_defaultMapId);
    if (SpawnPlayer(sessionId, actorId, actorId, zone, EnterPlacement{}) == nullptr)
        _network->RequestDisconnectSession(sessionId);
#endif
}

#if USE_LOGIN
// 같은 계정으로 이미 들어와 있는 세션을 걷어낸다. 걷을 곳이 둘이다 — 게임 중, 그리고 불러오는 중.
//   ★ 맵(계정 → 플레이어)을 안 만들고 훑는 이유: 넣고 빼야 할 표가 둘이라 한쪽이 어긋나면
//     해제된 주소를 들게 된다. 훑기는 어긋날 표가 없다. 그리고 번호를 안 들고 온 접속은 이 길을
//     아예 안 타므로(부하 더미가 전부 그렇다) 훑는 횟수가 사람 수준에 머문다.
//   ※ 모든 계정이 지정 번호로 들어오는 날(로그인 서버가 붙는 날, 또는 부하 더미를 고정 계정으로
//     바꾸는 날)에는 접속마다 전체를 훑게 된다. 그때 재 보고 계정 → 플레이어 맵으로 바꾼다.
void CGameServer::KickDuplicateAccount(int64_t accountId, int64_t exceptSessionId)
{
    // ⑴ 게임 중 — 정리 코드를 새로 쓰지 않고 평소 끊김 처리를 그대로 부른다. 그 안에 퇴장 방송·
    //    슬롯 해제·부활 대기열 회수·최종 저장 투입이 전부 있고, 무엇보다 **틱 중간 대기열에서 빼는
    //    일**이 들어 있다(BroadcastLeaveZone 이 _sectorChangedSet·_pendingSectorChanges 에서 제거,
    //    :2569~2573). 이 함수는 네트워크 이벤트 구간 한가운데서 플레이어를 지우는데, 같은 틱에
    //    그 플레이어가 MOVE_STOP 으로 그 대기열에 들어가 있을 수 있다 — 새로 짰으면 빠뜨렸을 자리다.
    //    뒤늦게 오는 진짜 끊김 이벤트는 플레이어가 이미 없어 FindPlayer 가 nullptr 이라 무해하다.
    for (CPlayer* other : _sessionSlots)
    {
        if (other == nullptr || other->_accountId != accountId || other->_sessionId == exceptSessionId)
            continue;

        const int64_t oldSession = other->_sessionId;
        SLOG_WARN("[LOGIN] duplicate account -> kick old session (account={}, old={}, new={})",
                  accountId, oldSession, exceptSessionId);
        OnDisconnected(oldSession);                        // 최종 저장이 이 뒤의 조회보다 큐에서 앞선다
        _network->RequestDisconnectSession(oldSession);
        _gameMonitor._enter._duplicate.Inc();
        break;   // 같은 계정이 둘 이상 게임 중일 수는 없다 — 여기서 막고 있기 때문이다
    }

    // ⑵ 불러오는 중 — 아직 플레이어가 없으니 저장도 방송도 없다. 대기표만 지우고 끊는다.
    //    탭 둘을 거의 동시에 열면 둘째가 첫째를 이 상태에서 걷어낸다. 뒤늦게 오는 그 조회 결과는
    //    대기표에서 주인을 못 찾아 orphaned 로 세어진다.
    //    [입장 대기] 자리는 계정 번호가 아직 0 이라 여기 안 걸린다(이 함수는 0 으로 안 불린다).
    for (const PendingEnter& slot : _pendingEnter)
    {
        if (slot.sessionId == -1 || slot.state != EnterState::LOADING
            || slot.accountId != accountId || slot.sessionId == exceptSessionId)
            continue;

        const int64_t oldSession = slot.sessionId;   // TakePendingEnter 가 slot 을 비우므로 먼저 복사한다
        SLOG_WARN("[LOGIN] duplicate account while loading -> drop (account={}, old={})", accountId, oldSession);
        PendingEnter gone;
        TakePendingEnter(oldSession, gone);
        _network->RequestDisconnectSession(oldSession);
        _gameMonitor._enter._duplicate.Inc();
        break;
    }
}

void CGameServer::RecvEnterGame(int64_t sessionId, CSerialBuffer* pMsg)
{
    MSG_C2S_ENTER_GAME recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // ⑴ 대기표가 [입장 대기]인가. 이미 요청했거나(불러오는 중) 이미 게임 중이면 조용히 무시한다 —
    //    두 번째 요청으로 캐릭터를 하나 더 만들지 않게 막는 자리다.
    {
        const uint16_t idx = CSession::ExtractIndex(sessionId);
        assert(idx < _pendingEnter.size());
        const PendingEnter& slot = _pendingEnter[idx];
        if (slot.sessionId != sessionId || slot.state != EnterState::WAITING_ENTER)
            return;
    }

    // ⑴-2 계정 번호 범위 — 0(미지정)이거나 ACCOUNT_ID_MIN 이상이어야 한다. 걸리면 캐릭터 번호도 안 뽑고 끊는다.
    //    지정 번호가 자동 번호 구간(int32 카운터, 2^31 미만)으로 내려오면 0 으로 들어온 남의 접속과 같은 행을
    //    서로 덮어쓰고, 아래 ⑶ 중복 정리가 그 접속을 끊는다. 음수(-1 등)도 여기서 걸린다.
    //    ※ 범위 검사일 뿐 본인 확인이 아니다 — 범위 안의 남의 번호는 인증이 붙기 전까지 못 거른다.
    if (recvMsg.accountId != 0 && recvMsg.accountId < ACCOUNT_ID_MIN)
    {
        SLOG_WARN("[LOGIN] enter rejected: account out of range -> disconnect (account={}, session={})",
                  recvMsg.accountId, sessionId);
        PendingEnter gone;
        TakePendingEnter(sessionId, gone);
        _network->RequestDisconnectSession(sessionId);
        return;
    }

    // ⑵ 번호 둘. 캐릭터 번호는 언제나 새로 뽑고, 계정 번호는 들고 온 값이 있으면 그것, 없으면(0)
    //    방금 뽑은 캐릭터 번호를 그대로 쓴다 — 그래야 번호를 안 들고 온 접속이 지금까지와 똑같이 돈다.
    //    자동과 지정은 구간이 타입으로 갈린다: AllocPlayerId 는 int32 카운터라 2^31 위로 못 올라온다.
    const int32_t actorId   = AllocPlayerId();
    const int64_t accountId = (recvMsg.accountId != 0) ? recvMsg.accountId : static_cast<int64_t>(actorId);

    // [불변식] 여기 오는 동안 _dbWorker 는 널이 아니다 — InitDB 가 실패하면 main 이 서버를 아예
    //   안 띄우고(main.cpp:109~113), 내리는 쪽은 게임 스레드를 join 한 "뒤"다(Stop, :428~437).
    //   USE_LOGIN 은 USE_DB_WORKER 를 요구하므로 "DB 없는 빌드"도 여기 못 온다.
    //   ※ 널 대비 분기를 두지 않는다 — 한 번도 안 타는 갈래이고, 혹시 탄다면 불러오기 없이 스폰해
    //     다음 저장이 남의 행을 덮는 쪽이라 조용한 오동작이 된다. 차라리 여기서 멈추는 게 낫다.
    assert(_dbWorker != nullptr);

    // ⑶ 같은 계정 정리 — 반드시 조회를 던지기 "전"이다. 정리가 넣는 최종 저장과 이 조회는 같은
    //    워커 큐를 타고(계정 번호 % 워커 수) 앞에서부터 실행되므로, 순서가 뒤집히면 방금 저장한
    //    자리를 못 읽는다. 번호를 안 들고 온 접속은 새 번호라 겹칠 수가 없어 아예 안 훑는다.
    if (recvMsg.accountId != 0)
        KickDuplicateAccount(accountId, sessionId);

    // ⑷ 조회를 던진다. 거절되면 결과가 안 오므로 매달아 두지 않고 끊는다(rejected 는 안에서 셌다).
    if (!RequestCharacterLoad(sessionId, accountId))
    {
        PendingEnter gone;
        TakePendingEnter(sessionId, gone);
        _network->RequestDisconnectSession(sessionId);
        return;
    }

    // ⑸ 대기표를 [불러오는 중]으로. 조회 뒤에 해도 안전하다 — 결과는 같은 스레드가 네트워크 이벤트를
    //    다 돈 "뒤에야" 꺼내므로(ProcessNetworkEvents → ProcessDbResults), 그 전에 주인을 찾을 일이 없다.
    //    [불변식] 여기서 반드시 성공한다 — ⑴ 에서 [입장 대기]인 것을 봤고, 그 사이 이 자리를 건드리는
    //      것은 ⑶ 뿐인데 거기서 내 세션을 명시적으로 제외한다. 실패하면 조회는 던져 놓고 대기표가 없어
    //      결과가 주인을 못 찾고, 그 세션은 스폰도 끊김도 없이 영영 매달린다(클라가 하트비트를 보내
    //      유휴 타임아웃도 안 걸린다). 그래서 조용히 넘기지 않고 단언으로 잡는다.
    const bool promoted = PromotePendingEnter(sessionId, accountId, actorId);
    assert(promoted);
    (void)promoted;   // 릴리즈(NDEBUG) 미사용 경고 억제
}
#endif

void CGameServer::OnDisconnected(int64_t sessionId)
{
    CPlayer* player = FindPlayer(sessionId);
    if (player == nullptr)
    {
#if USE_DB_WORKER
        // 불러오는 중에 나간 세션 — 플레이어가 아직 없으니 저장도 방송도 없다. 대기표만 비운다.
        //   뒤늦게 오는 결과는 대기표에서 주인을 못 찾아 orphaned 로 세어진다.
        PendingEnter gone;
        TakePendingEnter(sessionId, gone);
#endif
        return;
    }
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone != nullptr)
    {
        // 주변 DELETE 브로드캐스트 + 섹터/존 해제
        BroadcastLeaveZone(zone, player);
    }

    // 경계 매핑 해제 (zone 유무와 무관하게 반드시 수행)
    _sessionSlots[CSession::ExtractIndex(sessionId)] = nullptr;

#if USE_COMBAT
    // 부활 대기열 회수 — 위 BroadcastLeaveZone이 이미 하지만, zone == nullptr 분기는 그걸 건너뛴다.
    //   그 상태로 죽어 있을 수는 없어도, 아래 delete 뒤에 대기열이 그 주소를 들고 있으면
    //   다음 틱이 해제된 메모리를 밟는다. 두 번 불러도 무해하므로 zone 유무와 무관하게 여기서도 뺀다.
    RemoveFromDeadActors(player);
#endif

#if USE_DB_WORKER
    // 로그아웃 최종 저장 (스냅샷은 값 복사라 직후 delete 안전)
    if (_dbWorker)
        EnqueuePlayerSave(player);
#endif

    // 플레이어 삭제 (CGameServer가 생명주기 소유)
    delete player;
}

void CGameServer::OnReceived(int64_t sessionId, CSerialBuffer* pMsg)
{
    // [불변식] RECEIVED 생산자는 ParsePackets 한 곳뿐이고, 그 버퍼는 직전에 역참조된 Alloc 결과다.
    assert(pMsg != nullptr);

    // 헤더에서 패킷 타입 읽기
    //   [불변식] PeekData는 항상 성공한다 — RECEIVED 이벤트를 만드는 곳은 ParsePackets 한 군데뿐이고
    //   (IOCPServer.cpp의 PushNetworkEvent(RECEIVED)), 거기 닿으려면 totalPacketSize >= sizeof(MsgHeader)
    //   검증과 그 크기만큼의 적재 성공을 모두 통과해야 하므로 _DataSize >= sizeof(MsgHeader)가 보장된다.
    //   ※ 호출을 assert 안에 넣지 말 것 — header를 채우는 부작용이라, 릴리즈에서 식째로 사라지면
    //     header가 스택 잔여값인 채로 아래 switch를 타 엉뚱한 핸들러가 돈다(원래 결함 그 자체).
    MsgHeader header{};
    const int peekedSize = pMsg->PeekData(reinterpret_cast<char*>(&header), sizeof(header));
    assert(peekedSize == static_cast<int>(sizeof(header)));
    (void)peekedSize;   // 릴리즈(NDEBUG) 미사용 경고 억제

    // 타입별 최소 패킷 크기 검증
    uint16_t expectedSize = GetExpectedSize(header.type);
    if (expectedSize == 0 || header.size < expectedSize)
    {
        _monitor._packetErrors.Inc();
        pMsg->SubRef();
        return;
    }

#if USE_LOGIN
    // 입장 요청만 아래 문턱 "앞"에서 가른다 — 이 패킷이 올 때는 아직 플레이어가 없어서,
    //   문턱 뒤에 두면 FindPlayer 가 nullptr 을 돌려 영영 핸들러에 못 닿는다.
    //   나머지 패킷은 지금까지와 똑같이 플레이어를 찾은 뒤에 간다.
    if (header.type == MsgType::C2S_ENTER_GAME)
    {
        RecvEnterGame(sessionId, pMsg);
        pMsg->SubRef();
        return;
    }
#endif

    // 경계 계층: sessionId → CPlayer* 변환은 여기 한 번뿐 — 이후 컨텐츠 로직은 세션 ID 를 다시 쓰지 않는다.
    //   핸들러(RecvXxx)는 CPlayer* 를 받고, 브로드캐스트·섹터 쪽은 CCharacter* 로 받는다(몬스터도 같은 길).
    CPlayer* player = FindPlayer(sessionId);
    if (player == nullptr)
    {
        pMsg->SubRef();
        return;
    }

    switch (header.type)
    {
    case MsgType::C2S_MOVE_START:
        RecvMoveStart(player, pMsg);
        break;

    case MsgType::C2S_MOVE_STOP:
        RecvMoveStop(player, pMsg);
        break;

    case MsgType::C2S_CHAT:
        RecvChat(player, pMsg);
        break;

    case MsgType::C2S_ZONE_CHANGE:
        RecvZoneChange(player, pMsg);
        break;

    case MsgType::C2S_ADMIN_LOGIN:
        RecvAdminLogin(player, pMsg);
        break;

#if USE_COMBAT
    case MsgType::C2S_ATTACK:
        RecvAttack(player, pMsg);
        break;
#endif

#if USE_INVENTORY
    case MsgType::C2S_USE_ITEM:
        RecvUseItem(player, pMsg);
        break;
#endif

#if USE_EQUIP
    case MsgType::C2S_EQUIP:
        RecvEquip(player, pMsg);
        break;

    case MsgType::C2S_UNEQUIP:
        RecvUnequip(player, pMsg);
        break;
#endif

#if USE_DROP
    case MsgType::C2S_PICKUP:
        RecvPickup(player, pMsg);
        break;
#endif

    default:
        // C2S_HEARTBEAT 등 — 게임 로직 처리 불필요 (타임아웃 갱신은 IOCPServer 수신 시점에서 처리)
        break;
    }

    // SerialBuffer 해제 (소유권 1 반환 — RefCount=0이면 프리리스트로)
    pMsg->SubRef();
}

uint16_t CGameServer::GetExpectedSize(MsgType type)
{
    switch (type)
    {
    case MsgType::C2S_MOVE_START:   return sizeof(MSG_C2S_MOVE_START);
    case MsgType::C2S_MOVE_STOP:    return sizeof(MSG_C2S_MOVE_STOP);
    case MsgType::C2S_CHAT:         return sizeof(MsgHeader) + sizeof(ChatChar); // 가변 길이: 최소 1글자
    case MsgType::C2S_ZONE_CHANGE:  return sizeof(MSG_C2S_ZONE_CHANGE);
    case MsgType::C2S_HEARTBEAT:    return sizeof(MSG_C2S_HEARTBEAT);
#if USE_COMBAT
    // 이 줄을 빠뜨리면 0이 돌아가 위 OnReceived가 패킷을 조용히 버린다(로그 없음, _packetErrors만 증가).
    case MsgType::C2S_ATTACK:       return sizeof(MSG_C2S_ATTACK);
#endif
    case MsgType::C2S_ADMIN_LOGIN:  return sizeof(MSG_C2S_ADMIN_LOGIN);
    // 이 줄이 없으면 입장 요청이 0 을 받아 파싱 오류로 샌다 — 로그도 안 남고 _packetErrors 만 오른다.
    //   1단계에서는 크기만 등록하고 핸들러는 없다(아래 OnReceived 의 default 로 빠지거나,
    //   플레이어가 아직 없으면 그 앞 문턱에서 버려진다). 서버 동작은 그대로다.
    case MsgType::C2S_ENTER_GAME:   return sizeof(MSG_C2S_ENTER_GAME);
#if USE_INVENTORY
    // 이 줄이 없으면 물약 쓰기가 조용히 버려진다(위 C2S_ATTACK 과 같은 함정 — 답도 로그도 없다).
    case MsgType::C2S_USE_ITEM:     return sizeof(MSG_C2S_USE_ITEM);
#endif
#if USE_EQUIP
    // 이 두 줄이 없으면 끼우기·빼기가 조용히 버려진다(위 물약과 같은 함정 — 거절 패킷도 안 간다).
    case MsgType::C2S_EQUIP:        return sizeof(MSG_C2S_EQUIP);
    case MsgType::C2S_UNEQUIP:      return sizeof(MSG_C2S_UNEQUIP);
#endif
#if USE_DROP
    case MsgType::C2S_PICKUP:       return sizeof(MSG_C2S_PICKUP);   // 몸 없음 — 헤더뿐
#endif
    default:                        return 0;
    }
}

// ==========================================================================
// 패킷 핸들러
// ==========================================================================

void CGameServer::RecvMoveStart(CPlayer* player, CSerialBuffer* pMsg)
{
#if USE_COMBAT
    if (player->_isDead)
        return;   // 시체는 안 움직인다 — 부활 전까지 조작을 막는다
#endif

    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    MSG_C2S_MOVE_START recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // Direction 범위 검증 (4방향)
    if (recvMsg.direction < static_cast<uint8_t>(Direction::UP) ||
        recvMsg.direction > static_cast<uint8_t>(Direction::RIGHT))
        return;

    Direction dir = static_cast<Direction>(recvMsg.direction);

    // 이동 중 방향 전환: 같은 방향이면 무시, 다른 방향이면 갱신 + 재브로드캐스트
    // ※ 좌표 수용보다 먼저 검사 — MOVING 중 반복 전송으로 서버 좌표를 밀어내는 치트 방지
    if (player->_moveState == MoveState::MOVING)
    {
        if (dir == player->_direction)
            return;

        // 벽 방향 검증: 벽 위치에서 벽 쪽으로 방향 전환 시 정지 처리
        if (IsBlockedByWall(zone, player, dir))
        {
            player->_moveState = MoveState::IDLE;
            player->_direction = dir;

#if USE_DB_WORKER
            player->_dbDirty = true;   // MOVING → IDLE 전이(주기저장 MOVING 조건에서 빠지므로 최종위치 마킹)
#endif

            NotifyMoveStop(zone, player, false);  // 본인 포함
            return;
        }

        player->_direction = dir;
        player->_lastSyncX = player->_x;
        player->_lastSyncY = player->_y;

        NotifyMoveStart(zone, player);
        return;
    }

    // 클라이언트 예측 좌표 수용 (IDLE → MOVING 전환 시에만):
    // 서버 좌표와의 오차가 허용 범위 내이고, 초당 이동량 예산이 남아 있으면 채택.
    //   거리 임계값만으로는 1회 한도밖에 못 막는다 — STOP/START를 연타하면 전이마다 다시 8타일이
    //   수용되므로, 시간에 비례해 적립되는 예산을 함께 봐야 자기 속도 이상으로 못 움직인다.
    {
        float dx = recvMsg.x - player->_x;
        float dy = recvMsg.y - player->_y;
        const float distSq = dx * dx + dy * dy;
        if (distSq <= MOVE_START_ACCEPT_DIST_SQ && ConsumeMoveBudget(player, distSq))
        {
            // 맵 경계 클램핑 후 수용
            float acceptX = recvMsg.x;
            float acceptY = recvMsg.y;
            float mapW = static_cast<float>(zone->GetMapWidth());
            float mapH = static_cast<float>(zone->GetMapHeight());
            if (acceptX < 0.0f)    acceptX = 0.0f;
            if (acceptX >= mapW)   acceptX = mapW - 1.0f;
            if (acceptY < 0.0f)    acceptY = 0.0f;
            if (acceptY >= mapH)   acceptY = mapH - 1.0f;
            player->_x = acceptX;
            player->_y = acceptY;

#if USE_DB_WORKER
            player->_dbDirty = true;   // 클라 예측 좌표 채택 → 위치 변경, 저장 대상
#endif

            // 좌표 수용으로 섹터가 변경되었을 수 있으므로 재계산
            int32_t newSectorX = zone->GetSectorManager().CalcSectorX(player->_x);
            int32_t newSectorY = zone->GetSectorManager().CalcSectorY(player->_y);
            if (newSectorX != player->_sectorX || newSectorY != player->_sectorY)
            {
                int32_t oldSectorX = player->_sectorX;
                int32_t oldSectorY = player->_sectorY;

                zone->GetSectorManager().RemoveCharacter(player, oldSectorX, oldSectorY);
                player->_sectorX = newSectorX;
                player->_sectorY = newSectorY;
                zone->GetSectorManager().AddCharacter(player, newSectorX, newSectorY);

                PushSectorChange(player, oldSectorX, oldSectorY);
            }
        }
        // 범위 초과 또는 예산 부족 시 서버 좌표 유지 (치트 방지)
    }

    // 벽 방향 검증: 벽 위치에서 벽 쪽으로 이동 시도 시 차단
    if (IsBlockedByWall(zone, player, dir))
    {
        SendSyncPosition(player);        // 본인 — 서버 권위 좌표로 되돌림
        NotifyMoveSync(zone, player);    // 주변 — 위에서 채택한 좌표를 전파.
                                         //   이게 없으면 상태가 IDLE이라 주기 동기화(MOVING 대상)에도
                                         //   틱 끝 묶음(dirty 대상)에도 안 실려, 주변은 최대 8타일 떨어진
                                         //   옛 위치를 계속 렌더한다.
        return;
    }

    // 플레이어 상태 갱신
    player->_direction = dir;
    player->_moveState = MoveState::MOVING;

    // 델타 동기화 기준 좌표 갱신 (이동 시작 직후 즉시 동기화 방지)
    player->_lastSyncX = player->_x;
    player->_lastSyncY = player->_y;

    // 주변에 MOVE_START 브로드캐스트 (묶음 모드: dirty 마킹만)
    NotifyMoveStart(zone, player);
}

void CGameServer::RecvMoveStop(CPlayer* player, CSerialBuffer* pMsg)
{
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    MSG_C2S_MOVE_STOP recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // 이동 중이 아니면 무시 (서버 벽 클램핑으로 이미 IDLE 처리됨)
    if (player->_moveState != MoveState::MOVING)
        return;

    // Direction 범위 검증 (4방향)
    if (recvMsg.direction < static_cast<uint8_t>(Direction::UP) ||
        recvMsg.direction > static_cast<uint8_t>(Direction::RIGHT))
        return;

    // 서버 권위 모델: 서버 좌표 유지, 상태만 변경
    player->_direction = static_cast<Direction>(recvMsg.direction);
    player->_moveState = MoveState::IDLE;

#if USE_DB_WORKER
    player->_dbDirty = true;   // 정지 → IDLE 전이(주기저장 MOVING 조건에서 빠지므로 최종위치 마킹)
#endif

    // 섹터 이동 판정
    int32_t newSectorX = zone->GetSectorManager().CalcSectorX(player->_x);
    int32_t newSectorY = zone->GetSectorManager().CalcSectorY(player->_y);

    if (newSectorX != player->_sectorX || newSectorY != player->_sectorY)
    {
        int32_t oldSectorX = player->_sectorX;
        int32_t oldSectorY = player->_sectorY;

        // 섹터 데이터 갱신
        zone->GetSectorManager().RemoveCharacter(player, oldSectorX, oldSectorY);
        player->_sectorX = newSectorX;
        player->_sectorY = newSectorY;
        zone->GetSectorManager().AddCharacter(player, newSectorX, newSectorY);

        // 시야 진입/이탈 — 대기열에 추가 (틱 끝에 배치 처리)
        PushSectorChange(player, oldSectorX, oldSectorY);
    }

    // 주변에 MOVE_STOP 브로드캐스트 (묶음 모드: dirty 마킹만)
    NotifyMoveStop(zone, player, true);
}

void CGameServer::RecvChat(CPlayer* player, CSerialBuffer* pMsg)
{
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    // 가변 길이 수신: header.size 기반으로 실제 메시지 길이 역산
    MsgHeader header{};
    pMsg->PeekData(reinterpret_cast<char*>(&header), sizeof(header));

    uint16_t recvSize = header.size;
    if (recvSize > sizeof(MSG_C2S_CHAT))
        recvSize = sizeof(MSG_C2S_CHAT);  // 최대 크기 제한

    MSG_C2S_CHAT recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), recvSize);

    // 메시지 길이 계산 (바이트 → wchar_t 글자 수)
    uint16_t msgBytes = recvSize - sizeof(MsgHeader);
    msgBytes -= msgBytes % sizeof(ChatChar);  // 문자 경계 정렬 (홀수 바이트 방어)
    uint16_t msgLen = msgBytes / sizeof(ChatChar);
    if (msgLen > CHAT_MSG_MAX_LEN - 1)
        msgLen = CHAT_MSG_MAX_LEN - 1;
    recvMsg.message[msgLen] = L'\0';  // null 종단 보장

#if USE_INVENTORY
    // '/' 로 시작하면 명령이다 — 방송하지 않는다. 운영자가 아닌 사람의 /item 이 채팅으로 새지 않게,
    //   권한 검사보다 "방송 안 함"이 먼저다. 부하 더미의 채팅 문구에는 '/' 로 시작하는 것이 없다.
    if (msgLen > 0 && recvMsg.message[0] == u'/')
    {
        HandleChatCommand(player, recvMsg.message, msgLen);
        return;
    }
#endif

#if USE_BROADCAST_BUNDLE
    // [digest] 즉시 브로드캐스트 대신 보류 등록 — 틱 끝 digest에 합류 (실송신 시점은 기존과 동일한 틱 끝 flush).
    // 소스 섹터는 수신 시점 좌표로 고정 → 기존(즉시 배포)의 AOI 의미 보존.
    RegisterSectorItem(zone, player->_zoneId, player->_sectorX, player->_sectorY,
        MakeChat(player->_actorId, player->_displayChar, player->_colorIndex,
            recvMsg.message, msgLen));
#else
    // 본인 포함 브로드캐스트 (송신 함수가 소유권 소비)
    BroadcastAroundSector(zone, player,
        MakeChat(player->_actorId, player->_displayChar, player->_colorIndex,
            recvMsg.message, msgLen), false);
#endif
}

#if USE_INVENTORY
// ==========================================================================
// 채팅 명령 · 인벤토리 (USE_INVENTORY)
// ==========================================================================

namespace
{
    // 채팅(UTF-16) → 낱말들. 명령과 인자가 ASCII 라 그 밖의 글자는 '?' 로 바꾼다
    //   (한글 인자를 받는 명령이 생기면 그때 UTF-8 로 옮긴다).
    //   ★ NUL 에서 멈춘다 — 웹·콘솔 클라는 끝에 NUL 을 붙여 보내고(proto.js chat()), RecvChat 의
    //     글자 수는 헤더 크기로 역산해 그 NUL 까지 센다. 안 멈추면 마지막 낱말이 "5\0" 이 되어 숫자로 안 읽힌다.
    void SplitCommandWords(const ChatChar* msg, uint16_t len, std::vector<std::string>& out)
    {
        out.clear();
        std::string cur;
        for (uint16_t i = 0; i < len; ++i)
        {
            const ChatChar c = msg[i];
            if (c == u'\0')
                break;
            if (c == u' ' || c == u'\t')
            {
                if (!cur.empty())
                {
                    out.push_back(cur);
                    cur.clear();
                }
                continue;
            }
            cur += (c < 0x80) ? static_cast<char>(c) : '?';
        }
        if (!cur.empty())
            out.push_back(cur);
    }
}

void CGameServer::HandleChatCommand(CPlayer* player, const ChatChar* msg, uint16_t len)
{
    CGameMonitor::InventoryCounters& counters = _gameMonitor._inventory;

    // 명령은 사람이 치는 것이라 드물다 — 할당을 아끼지 않는다
    std::vector<std::string> words;
    SplitCommandWords(msg, len, words);
    const std::string cmd = words.empty() ? std::string("/") : words[0];   // 첫 글자가 '/' 라 비지 않는다

    // 명령이 하나뿐이라 갈래가 하나다. 둘째 명령이 생기면 "이름 → 처리 함수" 표로 바꾼다(if 사슬 금지).
    if (cmd != "/item")
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::UNKNOWN_COMMAND)].Inc();
        SendSystemMessage(player, "모르는 명령입니다: " + cmd);
        return;
    }

    if (!player->_isAdmin)
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::NOT_ADMIN)].Inc();
        SendSystemMessage(player, "운영자만 쓸 수 있는 명령입니다: /item");
        return;
    }

    // /item <아이템번호> [개수]  — 개수를 안 적으면 1
    int64_t itemId = 0;
    int64_t count  = 1;
    const bool okId    = words.size() >= 2 && GameDataCsv::ParseInt(words[1], itemId);
    const bool okCount = words.size() < 3 || GameDataCsv::ParseInt(words[2], count);
    if (!okId || !okCount || words.size() > 3 || count < 1 || count > 9999)
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::BAD_ARGS)].Inc();
        SendSystemMessage(player, "쓰는 법: /item <아이템번호> [개수 1~9999]");
        return;
    }

    // 번호 범위를 먼저 본다 — int32 로 줄이기 전에 걸러야 큰 값이 엉뚱한 번호로 접히지 않는다
    const ItemDef* def = (itemId > 0 && itemId <= CItemTable::MAX_ITEM_ID)
                       ? _gameDataFiles.Item().Find(static_cast<int32_t>(itemId))
                       : nullptr;
    if (def == nullptr)
    {
        counters._addRejected[static_cast<int>(InvAddResult::UNKNOWN_ITEM)].Inc();
        SendSystemMessage(player, "없는 아이템 번호입니다: " + words[1]);
        return;
    }

    const InvAddResult result = player->_inventory.TryAdd(*def, static_cast<int32_t>(count));
    switch (result)
    {
    case InvAddResult::OK:
        counters._itemsAdded.Add(count);
        SLOG_INFO("[ITEM] /item (account={}, player={}) -> {} x{} (item {})",
                  player->_accountId, player->_actorId, def->name, count, def->itemId);
        FlushInventory(player);
        return;

    case InvAddResult::TAB_FULL:
        SendSystemMessage(player, std::string("가방에 자리가 없습니다 — ") + InvTabDisplayName(def->tab)
                                  + " 탭에 " + def->name + " " + std::to_string(count) + "개를 다 둘 수 없습니다");
        break;

    case InvAddResult::UNKNOWN_ITEM:   // TryAdd 는 이 둘을 안 돌려준다 — 위에서 이미 걸렀다
    case InvAddResult::BAD_COUNT:
        SendSystemMessage(player, std::string("넣지 못했습니다: ") + InvAddResultName(result));
        break;

    case InvAddResult::COUNT:          // 값이 아니라 개수 표시 — 아래 배열 색인으로 쓰면 범위 밖이다
        assert(false && "TryAdd 가 COUNT 를 돌려줄 수는 없다");
        return;
    }
    counters._addRejected[static_cast<int>(result)].Inc();
}

void CGameServer::FlushInventory(CPlayer* player)
{
    CInventory& inv = player->_inventory;
    inv.TakeChanges([&](int index)
    {
        // 칸의 "최종값"을 보낸다 — 증감이 아니라서 같은 칸이 두 번 가도 결과가 같다
        const Item& s = inv.At(index);
        const uint16_t slotNo = CInventory::SlotNumberOf(index);
        SendPacket(player, MakeInventorySlot(slotNo, s.itemId, s.count));
        _gameMonitor._inventory._slotUpdatesSent.Inc();

#if USE_DB_WORKER
        // 같은 최종값을 곧바로 DB 로 — 60초 주기를 안 기다린다(아이템은 잃으면 되돌릴 길이 없다).
        //   빈 칸은 행을 지우지 않고 itemId 0 으로 덮는다(DB 계정에 DELETE 권한이 없다).
        //   같은 계정은 같은 워커 큐라, 다시 들어올 때 불러오기가 이 저장들 뒤에 줄 선다.
        if (_dbWorker)
        {
            const DBItemSlotJob job{ player->_accountId, slotNo, s.itemId, s.count };
            if (_dbWorker->EnqueueItemSlot(job))
                _gameMonitor._inventory._itemSavesQueued.Inc();
            else
            {
                // 아이템 저장 상한 — 정상 상태에선 안 닿는다(닿았다면 DB 가 멈춘 것). 되살릴 길은 이번 범위 밖이라 세고 남긴다.
                _gameMonitor._inventory._itemSaveRejected.Inc();
                SLOG_ERROR("[DB] item slot save REJECTED — queue ceiling (account={}, slot={}, item={}, count={})",
                           player->_accountId, slotNo, s.itemId, s.count);
            }
        }
#endif
    });
}

#if USE_DB_WORKER
static_assert(DB_INV_MAX_ROWS >= INVENTORY_ALL_SLOTS, "결과에 실을 가방 줄 수가 가방·착용 칸 수보다 적다");

void CGameServer::LoadInventoryRows(CPlayer* player, const DBResult& result)
{
    CGameMonitor::InventoryCounters& counters = _gameMonitor._inventory;
    auto skip = [&](InvLoadSkip why, const DBInvRow& r)
    {
        counters._loadSkipped[static_cast<int>(why)].Inc();
        SLOG_WARN("[INV] load skip {} (account={}, slot={}, item={}, count={})",
                  InvLoadSkipName(why), player->_accountId, r.slot, r.itemId, r.count);
    };

    // 사유는 가장 기본적인 것부터 본다 — 칸이 틀리면 아이템을 볼 것도 없다
    for (int i = 0; i < result.invCount; ++i)
    {
        const DBInvRow& r = result.inv[i];

        InvTab tab;
        int index = -1;
        if (r.slot < 0 || r.slot > 0xFFFF || !FindInvSlot(static_cast<uint16_t>(r.slot), tab, index))
        {
            skip(InvLoadSkip::BAD_SLOT, r);   // uint16 로 줄이기 전에 범위부터 — 큰 값이 엉뚱한 칸으로 접히지 않게
            continue;
        }
        const ItemDef* def = _gameDataFiles.Item().Find(r.itemId);
        if (def == nullptr)
        {
            skip(InvLoadSkip::UNKNOWN_ITEM, r);
            continue;
        }
        // 무기 칸(900)이면 FindInvSlot 이 탭을 EQUIP 으로 준다 — 무기 칸에 물약·재료가 든 줄도 여기서 걸린다
        if (def->tab != tab)
        {
            skip(InvLoadSkip::WRONG_TAB, r);
            continue;
        }
        if (r.count <= 0 || r.count > def->maxStack)
        {
            skip(InvLoadSkip::BAD_COUNT, r);   // 깎아 넣지 않는다 — 깎으면 사라진 걸 아무도 모른다
            continue;
        }
        player->_inventory.LoadSlot(index, r.itemId, r.count);
        counters._loadedSlots.Inc();
    }

    if (result.invOverflow > 0)
    {
        counters._loadSkipped[static_cast<int>(InvLoadSkip::TOO_MANY_ROWS)].Add(result.invOverflow);
        SLOG_WARN("[INV] load skip too_many_rows x{} (account={}) — 결과 자리 {} 를 넘은 줄",
                  result.invOverflow, player->_accountId, DB_INV_MAX_ROWS);
    }
}
#endif

void CGameServer::SendInventory(CPlayer* player)
{
    SendPacket(player, MakeInventory(player->_inventory));
    _gameMonitor._inventory._snapshotsSent.Inc();
}

void CGameServer::SendSystemMessage(CPlayer* player, const std::string& utf8)
{
    SendPacket(player, MakeError(utf8));
}

UseItemFail CGameServer::CheckUseItem(const CPlayer* player, uint16_t slot, int& outIndex, const ItemDef*& outDef) const
{
    // UseItemFail 의 순서 그대로 본다 — 여럿이 겹치면 먼저 걸린 하나로 답한다(열거 순서가 곧 확인 순서).
    InvTab tab;
    if (!FindInvSlot(slot, tab, outIndex))
        return UseItemFail::BAD_SLOT;

    const Item& s = player->_inventory.At(outIndex);
    if (s.itemId == 0)
        return UseItemFail::EMPTY;

    // 표에서 사라진 번호(패치로 지운 아이템)도 여기서 "못 쓴다"로 떨어진다
    outDef = _gameDataFiles.Item().Find(s.itemId);
    if (outDef == nullptr || outDef->tab != InvTab::USE || outDef->healHp <= 0)
        return UseItemFail::NOT_USABLE;

    if (player->_isDead)
        return UseItemFail::DEAD;

    if (_frameCount < player->_itemUseReadyFrame)
        return UseItemFail::COOLDOWN;

    // 체력이 가득이면 안 줄인다 — 물약만 날리게 두지 않는다
    if (player->_hp >= player->_maxHp)
        return UseItemFail::FULL_HP;

    return UseItemFail::COUNT;   // 통과
}

void CGameServer::RecvUseItem(CPlayer* player, CSerialBuffer* pMsg)
{
    static_assert(ITEM_USE_COOLDOWN_FRAMES == FRAME_PER_SEC, "물약 대기시간은 1초 — 틱 주기를 바꿨으면 같이 맞출 것");

    MSG_C2S_USE_ITEM recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    int index = -1;
    const ItemDef* def = nullptr;
    const UseItemFail fail = CheckUseItem(player, recvMsg.slot, index, def);
    if (fail != UseItemFail::COUNT)
    {
        _gameMonitor._inventory._useRejected[static_cast<int>(fail)].Inc();
        SendPacket(player, MakeUseItemFail(recvMsg.slot, fail));
        return;
    }

    // ── 성공 — 체력을 채우고(최대치에서 멈춘다) 한 개 줄이고 대기시간을 건다 ──
    const int32_t healed = player->_hp + def->healHp;   // std::min 은 윈도 헤더의 min 매크로에 먹힌다
    player->_hp = (healed < player->_maxHp) ? healed : player->_maxHp;
    player->_inventory.ConsumeOne(index);
    player->_itemUseReadyFrame = _frameCount + ITEM_USE_COOLDOWN_FRAMES;   // 성공했을 때만 — 거절은 대기시간을 안 건다
    _gameMonitor._inventory._itemsUsed.Inc();

    // 칸 갱신(본인) → 체력 통보(본인 + 주변 — 남의 머리 위 막대도 차야 한다). 레벨업 때 체력 방송과 같은 꼴.
    //   ★ 순서가 곧 와이어 순서다 — 칸 패킷이 체력 패킷보다 앞선다(시험이 이 순서로 "이 체력이 물약 몫"을 가린다).
    FlushInventory(player);
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone != nullptr)
        BroadcastAroundSector(zone, player, MakeActorStats(player->_actorId, player->_hp, player->_maxHp), false);
}

#if USE_EQUIP
// ==========================================================================
// 장착 — 끼우기·빼기는 "칸 둘 맞바꾸기" 하나다(CInventory::SwapSlots).
//   저장은 지금 가방 길 그대로(FlushInventory — 칸마다 DB 한 줄). ★ 두 줄은 따로 쓴다 — 두 문장 사이에
//   서버가 죽으면 무기 하나를 잃거나 복제될 수 있다(여러 칸 넣기와 같은 틈, 플랜 ㉯ — 거래 때 트랜잭션 레인으로).
// ==========================================================================
EquipFail CGameServer::CheckEquip(const CPlayer* player, uint16_t slot, bool unequip, int& outFrom, int& outTo) const
{
    // EquipFail 의 순서 그대로 본다 — 여럿이 겹치면 먼저 걸린 하나로 답한다(열거 순서가 곧 확인 순서).
    const CInventory& inv = player->_inventory;

    // BAD_SLOT — 끼우기는 "가방 칸", 빼기는 "착용 칸" 이어야 한다
    InvTab tab;
    if (!FindInvSlot(slot, tab, outFrom) || IsWornIndex(outFrom) != unequip)
        return EquipFail::BAD_SLOT;

    // EMPTY
    const Item& s = inv.At(outFrom);
    if (s.itemId == 0)
        return EquipFail::EMPTY;

    // NOT_EQUIPPABLE — 끼우기만. 물약 칸을 보내도 여기서 걸린다(가방 칸이면 탭을 안 가린다 — 선언 주석)
    //   표에서 사라진 번호도 여기로 떨어진다. 무기 칸의 것은 불러오기가 이미 장비만 들였다.
    if (!unequip)
    {
        const ItemDef* def = _gameDataFiles.Item().Find(s.itemId);
        if (def == nullptr || def->tab != InvTab::EQUIP)
            return EquipFail::NOT_EQUIPPABLE;
    }

    if (player->_isDead)
        return EquipFail::DEAD;

    if (_frameCount < player->_equipReadyFrame)
        return EquipFail::COOLDOWN;

    // 맞바꿀 상대 — 끼우기는 무기 칸(비었든 찼든), 빼기는 장비 탭의 첫 빈 칸
    if (!unequip)
    {
        outTo = WornIndexOf(EquipSlot::WEAPON);   // 부위가 무기 하나뿐 — 둘째 부위가 생기면 아이템 표의 부위 칸으로 고른다
        return EquipFail::COUNT;
    }
    outTo = inv.FirstEmptyInTab(InvTab::EQUIP);
    if (outTo < 0)
        return EquipFail::TAB_FULL;   // 아무것도 안 바꾼다 — 무기는 그대로 끼워져 있다
    return EquipFail::COUNT;
}

void CGameServer::RecvEquip(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_EQUIP recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    ApplyEquip(player, recvMsg.slot, false);
}

void CGameServer::RecvUnequip(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_UNEQUIP recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
    ApplyEquip(player, recvMsg.slot, true);
}

void CGameServer::ApplyEquip(CPlayer* player, uint16_t slot, bool unequip)
{
    static_assert(EQUIP_COOLDOWN_FRAMES == FRAME_PER_SEC, "장착 대기시간은 1초 — 틱 주기를 바꿨으면 같이 맞출 것");

    int from = -1, to = -1;
    const EquipFail fail = CheckEquip(player, slot, unequip, from, to);
    if (fail != EquipFail::COUNT)
    {
        _gameMonitor._equip._rejected[static_cast<int>(fail)].Inc();
        SendPacket(player, MakeEquipFail(slot, fail));
        return;
    }

    // ── 성공 — 칸 둘을 맞바꾸고 대기시간을 건다(성공했을 때만 — 거절은 안 건다) ──
    player->_inventory.SwapSlots(from, to);
    player->_equipReadyFrame = _frameCount + EQUIP_COOLDOWN_FRAMES;
    if (unequip) _gameMonitor._equip._unequipped.Inc();
    else         _gameMonitor._equip._equipped.Inc();

    // 칸 패킷 둘 + DB 두 줄 → 공격력 다시 세우기 → 본인에게 새 공격력.
    //   ★ 순서가 곧 와이어 순서다 — 칸 패킷이 MY_STATS 보다 앞선다(클라가 무기 칸을 먼저 알고 숫자를 채운다).
    //   체력은 그대로다(refillHp=false) — 무기는 공격력만 바꾼다.
    FlushInventory(player);
    RecalcStats(player, false);
    SendMyStats(player);
}
#endif // USE_EQUIP
#endif // USE_INVENTORY

// ==========================================================================
// 패킷 전송 추상화
// ==========================================================================

// ==========================================================================
// 계층 경계 헬퍼
// ==========================================================================

int32_t CGameServer::AllocPlayerId()
{
    return _nextPlayerId++;
}

// playerId → 고유 표시 문자 (A-Z, a-z, 0-9 = 62종)
uint8_t CGameServer::CalcDisplayChar(int32_t playerId)
{
    static constexpr char CHARS[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    return static_cast<uint8_t>(CHARS[static_cast<uint32_t>(playerId) % 62]);
}

// playerId → 고유 색상 인덱스 (0-6, 7종)
uint8_t CGameServer::CalcColorIndex(int32_t playerId)
{
    return static_cast<uint8_t>((static_cast<uint32_t>(playerId) / 62) % 7);
}

// ── 패킷 전송 추상화 ──
// 단일 플레이어에게 패킷 전송 (CSerialBuffer — 소유권 1을 소비)
// 빌더가 반환한 RefCount=1 버퍼를 넘기면, 송신 후 SubRef로 소유권을 회수한다.
//   받는 쪽은 CPlayer* 뿐이다. 세션 -1 검사는 둔다 — 스폰 도중처럼 세션이 잠시 없는 상태가 생기면 이 한 줄이 막는다.
void CGameServer::SendPacket(CPlayer* target, CSerialBuffer* pMsg)
{
    if (target->_sessionId != -1)
    {
        pMsg->AddRef();   // 송신용 소유권 — RequestSendMsg(CSerialBuffer*)가 소비
        // 게임루프 송신은 묶어 보낼 수 있음을 표시(Deferred) — 실제 지연 여부는 USE_SEND_COALESCING이 결정
        const int dataSize = pMsg->GetDataSize();
        if (_network->RequestSendMsg(target->_sessionId, pMsg, SendFlush::Deferred))
        {
            _monitor._sendPackets.Inc();
            _monitor._sendEnqueuedBytes.Add(static_cast<int64_t>(dataSize));
        }
    }
    pMsg->SubRef();       // 빌더가 넘긴 소유권 1 회수 (세션 무효여도 안전 회수)
}

// 주변 브로드캐스트 (CSerialBuffer — 빌더 RefCount=1 버퍼를 소비)
void CGameServer::BroadcastAroundSector(CZone* zone, CCharacter* player, CSerialBuffer* pMsg, bool excludeSelf)
{
    // [계측] 비용종류별 — gather(주변 모으기) / enqueue(수신자별 복사) 구간을 호출당 2~3회 now()로 분리.
    //   per-call 측정이라 오버헤드가 수신자 수가 아닌 호출 수에 비례. 누적은 틱 끝 1회만 원자반영.
    auto _measGatherT0 = std::chrono::steady_clock::now();

    _broadcastBuffer.clear();
    const CCharacter* exclude = excludeSelf ? player : nullptr;
    zone->GetSectorManager().GetAroundPlayers(
        player->_sectorX, player->_sectorY, _broadcastBuffer, exclude);

    auto _measGatherT1 = std::chrono::steady_clock::now();
    _tickBroadcastGatherUs += std::chrono::duration_cast<std::chrono::microseconds>(
        _measGatherT1 - _measGatherT0).count();

    _monitor._gameLoop._broadcastCalls.Inc();
    _monitor._gameLoop._broadcastTargets.Add(static_cast<int64_t>(_broadcastBuffer.size()));   // 플레이어만 센다 — 뱀까지 세던 옛 값과 단위가 다르다

    // 타겟별 AddRef를 1회 배치 AddRef로 압축 (원자연산 N→1) — 목록이 플레이어뿐이라 선카운트가 곧 크기다
    const size_t validCount = _broadcastBuffer.size();
    if (validCount > 0)
        pMsg->AddRef(static_cast<int64_t>(validCount));   // 타겟별 소유권 일괄 확보

    // 송신 메트릭은 타겟별 원자증가 대신 성공분을 지역 누적 후 1회 반영 (원자연산 N→1)
    const int dataSize = pMsg->GetDataSize();
    size_t sentPkts = 0;
    for (CPlayer* other : _broadcastBuffer)
    {
        if (_network->RequestSendMsg(other->_sessionId, pMsg, SendFlush::Deferred))
            ++sentPkts;
    }
    if (sentPkts > 0)
    {
        _monitor._sendPackets.Add(static_cast<int64_t>(sentPkts));
        _monitor._sendEnqueuedBytes.Add(static_cast<int64_t>(sentPkts) * dataSize);
    }
    pMsg->SubRef();   // 빌더가 넘긴 소유권 1 회수 (타겟 0명이어도 안전 회수)

    // [계측] enqueue 구간 = gather 직후 ~ 여기 (precount + 배치 AddRef + 수신자별 RequestSendMsg 복사 + 메트릭).
    //   복사가 곧 USE_LOCKFREE_SENDQ 토글이 없애려는 비용 → A/B로 이 값의 변화를 본다.
    auto _measEnqueueT1 = std::chrono::steady_clock::now();
    _tickBroadcastEnqueueUs += std::chrono::duration_cast<std::chrono::microseconds>(
        _measEnqueueT1 - _measGatherT1).count();
}

// ── 이동 알림 헬퍼 ──
// 묶음 모드(USE_SECTOR_AGGREGATION)면 dirty 마킹만, 아니면 즉시 주변 브로드캐스트.
// 호출부의 #if/#else 산재를 이 3개로 흡수 (OFF 경로 바이트·동작 불변).
void CGameServer::NotifyMoveStart(CZone* zone, CCharacter* player)
{
#if USE_SECTOR_AGGREGATION
    MarkMoveDirty(player);
#else
    BroadcastAroundSector(zone, player,
        MakeMoveStart(player->_actorId, static_cast<uint8_t>(player->_direction),
            player->_x, player->_y));
#endif
}

void CGameServer::NotifyMoveStop(CZone* zone, CCharacter* player, bool excludeSelf)
{
#if USE_SECTOR_AGGREGATION
    MarkMoveDirty(player);
#else
    BroadcastAroundSector(zone, player,
        MakeMoveStop(player->_actorId, static_cast<uint8_t>(player->_direction),
            player->_x, player->_y), excludeSelf);
#endif
}

void CGameServer::NotifyMoveSync(CZone* zone, CCharacter* player)
{
#if USE_SECTOR_AGGREGATION
    MarkMoveDirty(player);
#else
    BroadcastAroundSector(zone, player,
        MakeSyncPosition(player->_actorId, player->_x, player->_y), false);
#endif
}

void CGameServer::SendZoneInfo(CPlayer* target, CZone* zone)
{
    SendPacket(target, MakeZoneInfo(zone));
}

void CGameServer::SendCreateMyPlayer(CPlayer* target)
{
    SendPacket(target, MakeCreateMyPlayer(target));   // 본인 체력도 이 안에 실린다(ActorSnapshot)
}

void CGameServer::SendCreateOtherPlayer(CPlayer* target, CCharacter* who, SpawnReason reason)
{
    ++_tickMembershipSends;   // 멤버십 변경 복사 집계 (BroadcastAroundSector 밖 경로)
    SendPacket(target, MakeCreateOtherPlayer(who, reason));
}

void CGameServer::SendDeletePlayer(CPlayer* target, CCharacter* who)
{
    ++_tickMembershipSends;   // 멤버십 변경 복사 집계 (BroadcastAroundSector 밖 경로)
    SendPacket(target, MakeDeletePlayer(who->_actorId));
}

#if USE_MEMBERSHIP_INBOUND_BUNDLE
// [Phase 2] 인바운드 멤버십 배치 송신 — 상대 목록을 배치 상한씩 잘라 mover 1명에게 송신.
// _membershipSends는 OFF의 개별 SendXxx 호출당 1과 동일하게 엔트리당 1 집계 (A/B 통제지표 불변).
// count=0이면 아무것도 안 보냄 — OFF의 빈 루프와 동일.
void CGameServer::SendCreatePlayerBatch(CPlayer* target, CCharacter* const* others, int count)
{
    _tickMembershipSends += count;
    for (int i = 0; i < count; i += CREATE_PLAYER_BATCH_MAX_ENTRIES)
    {
        int chunk = (std::min)(count - i, CREATE_PLAYER_BATCH_MAX_ENTRIES);
        SendPacket(target, MakeCreatePlayerBatch(others + i, chunk));
    }
}

void CGameServer::SendDeletePlayerBatch(CPlayer* target, CCharacter* const* others, int count)
{
    _tickMembershipSends += count;
    for (int i = 0; i < count; i += DELETE_PLAYER_BATCH_MAX_ENTRIES)
    {
        int chunk = (std::min)(count - i, DELETE_PLAYER_BATCH_MAX_ENTRIES);
        SendPacket(target, MakeDeletePlayerBatch(others + i, chunk));
    }
}
#endif // USE_MEMBERSHIP_INBOUND_BUNDLE

void CGameServer::SendSyncPosition(CPlayer* target)
{
    SendPacket(target, MakeSyncPosition(target->_actorId, target->_x, target->_y));
}

void CGameServer::RecvZoneChange(CPlayer* player, CSerialBuffer* pMsg)
{
#if USE_COMBAT
    if (player->_isDead)
        return;   // 죽은 채로 존을 옮기면 부활 대기열이 옛 존을 가리킨 채 남는다
#endif

    int64_t sessionId = player->_sessionId;
    if (sessionId == -1)
        return;

    CZone* oldZone = _mapManager.GetZone(player->_zoneId);
    if (oldZone == nullptr)
        return;

    MSG_C2S_ZONE_CHANGE recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    int32_t targetMapId = recvMsg.targetMapId;
    int32_t targetChannelIndex = recvMsg.targetChannelIndex;

    CZone* newZone = nullptr;

    if (targetChannelIndex >= 0)
    {
        // ── 채널 지정 이동 (같은 맵 내) ──
        int32_t currentMapId = CMapManager::GetMapIdFromZoneId(oldZone->GetZoneId());

        newZone = _mapManager.FindChannel(currentMapId, targetChannelIndex);
        if (newZone == nullptr)
        {
            SendZoneChangeFail(player, 0);  // 채널 없음
            return;
        }
        if (newZone->GetZoneId() == oldZone->GetZoneId())
        {
            SendZoneChangeFail(player, 2);  // 이미 해당 채널
            return;
        }

        // 인원 제한 체크 (admin은 스킵)
        if (!player->_isAdmin)
        {
            int32_t maxPlayers = _mapManager.GetMaxPlayersPerChannel(currentMapId);
            if (maxPlayers > 0 && newZone->GetPlayerCount() >= maxPlayers)
            {
                SendZoneChangeFail(player, 1);  // 채널 가득 참
                return;
            }
        }

        targetMapId = currentMapId;
    }
    else
    {
        // ── 기존 맵 이동 (자동 채널 배정) ──

        // 랜덤 맵 이동 요청 처리
        if (targetMapId == -1)
        {
            int32_t currentMapId = CMapManager::GetMapIdFromZoneId(oldZone->GetZoneId());
            targetMapId = _mapManager.GetRandomMapId(currentMapId);
            if (targetMapId == -1)
            {
                SendZoneChangeFail(player, 0);
                return;
            }
        }

        newZone = _mapManager.FindOrCreateChannel(targetMapId, player->_isAdmin);
        if (newZone == nullptr)
        {
            SendZoneChangeFail(player, 0);
            return;
        }

        // 자동배정이 "지금 있는 존"을 그대로 돌려준 경우 — 채널 지정 분기(reason 2)와 동일하게 거부.
        //   통과시키면 아래 LeaveZone→EnterZone의 CalcSpawnPos가 좌표를 맵 전역 난수로 재추첨해,
        //   요청만으로 순간이동이 된다(MOVE_START 이동량 예산 우회).
        if (newZone->GetZoneId() == oldZone->GetZoneId())
        {
            SendZoneChangeFail(player, 2);  // 이미 해당 채널
            return;
        }
    }

    // ── 현재 존에서 퇴장 ──
    BroadcastLeaveZone(oldZone, player);

    // ── 새 존에 입장 (player 객체 재활용, playerId 유지) ──

    if (!newZone->EnterZone(player))
    {
        // 입장 실패 시 원래 맵의 여유 채널로 복귀 시도
        CZone* fallback = _mapManager.FindOrCreateChannel(CMapManager::GetMapIdFromZoneId(oldZone->GetZoneId()));
        if (fallback != nullptr && fallback->EnterZone(player))
        {
            SendZoneChangeFail(player, 1);

            // 델타 동기화 기준 좌표 초기화
            player->_lastSyncX = player->_x;
            player->_lastSyncY = player->_y;

            // 복귀한 존에서 본인 + 주변 상호 통보
            SendZoneInfo(player, fallback);
            SendCreateMyPlayer(player);
#if USE_EXP
            SendExp(player);   // 아래 성공 경로와 같은 이유 — 클라가 존을 넘을 때 판을 비운다
#endif
            BroadcastEnterZone(fallback, player, SpawnReason::ZONE_TRANSFER);

            return;
        }

        // 복귀도 실패 → 좀비 방지를 위해 연결 해제
        _sessionSlots[CSession::ExtractIndex(sessionId)] = nullptr;
        delete player;
        _network->RequestDisconnectSession(sessionId);
        return;
    }

    _monitor._gameLoop._zoneChangeCount.Inc();

#if USE_DB_WORKER
    player->_dbDirty = true;   // 존/맵 이동 → 위치·map_id 변경, 저장 대상
#endif

    // 델타 동기화 기준 좌표 초기화
    player->_lastSyncX = player->_x;
    player->_lastSyncY = player->_y;

    // 존 메타 정보 + 존 이동 성공 통보
    SendZoneInfo(player, newZone);
    int32_t channelIndex = CMapManager::GetChannelIndexFromZoneId(newZone->GetZoneId());
    SendZoneChangeOk(player, targetMapId, channelIndex);

#if USE_EXP
    // ★ 레벨·경험치를 다시 보낸다 — 이 경로는 SendCreateMyPlayer 를 안 거친다.
    //   콘솔 클라는 존을 넘을 때 내 정보를 통째로 비우고 다시 세운다(speed 만 따로 옮겨 담는다,
    //   GameInstance.cpp 의 ZONE_CHANGE_OK). 안 보내면 거기서 화면이 1 레벨로 돌아가고
    //   다음 경험치가 들어올 때까지 그대로다. 웹 클라는 안 비우므로 이 장이 덧쓰기가 될 뿐 무해하다.
    SendExp(player);
#endif

    // 주변 상호 CREATE 브로드캐스트
    BroadcastEnterZone(newZone, player, SpawnReason::ZONE_TRANSFER);
}

// ── 운영자 인증 ──

// 부하 테스트 도구용 고정 키 — 실서비스 인증 아님.
// 와이어에 평문으로 흐르고 해시·시도 제한도 없다. admin 권한 효과는 채널 인원 제한 우회뿐(CMapInstance::FindOrCreateChannel).
// 실계정 연동 단계에서 계정 DB 조회 기반 인증으로 교체한다.
static constexpr char ADMIN_KEY[] = "admin1234";

void CGameServer::RecvAdminLogin(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_ADMIN_LOGIN recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // null-terminate 보장
    recvMsg.key[ADMIN_KEY_MAX_LEN - 1] = '\0';

    if (strcmp(recvMsg.key, ADMIN_KEY) == 0)
    {
        player->_isAdmin = true;
        SendPacket(player, MakeAdminLoginOk());
    }
    else
    {
        SendPacket(player, MakeAdminLoginFail());
    }
}

#if USE_COMBAT
// ==========================================================================
// 전투 (USE_COMBAT)
//
// 공격 흐름(쿨타임 → 대상 찾기 → 피해 → 죽음 · 지표)은 CCombatSystem(CombatSystem.h)이 하고,
//   여기서는 그 알림(ICombatEvents)을 받아 세계에 적용한다 — 송신 · 죽은 목록 등록 · 보상.
//   판정 함수(쿨타임·사거리·피해)는 namespace Combat 이 하고, 그쪽은 존·섹터·송신을 모른다.
//
// [수신자 범위의 한계] 전투 패킷은 전부 "공격자 주변 9섹터"에 뿌린다. 사거리가 1.5타일이라
//   맞은 쪽은 반드시 그 안에 있다. 다만 맞은 쪽 주변 구경꾼 중 공격자 9섹터 밖인 사람은 못 본다 —
//   섹터가 타일 여러 개라 실제로 겹치지 않는 경우가 드물고, 어긋나도 다음 CREATE 때
//   HP가 다시 맞춰진다(CREATE 가 체력을 싣는다 — ActorSnapshot). 이번 범위에서는 여기까지만 한다.
// ==========================================================================

// 세션 핸들러 — 세션이 있어야 성립하는 것만 보고 본체로 넘긴다.
//   몬스터는 패킷을 안 보내므로 이 경로를 못 탄다. 그래서 본체가 따로 있다.
void CGameServer::RecvAttack(CPlayer* player, CSerialBuffer* pMsg)
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
void CGameServer::OnSwing(CZone* zone, CCharacter* attacker)
{
    BroadcastAroundSector(zone, attacker,
        MakeAttack(attacker->_actorId, static_cast<uint8_t>(attacker->_direction)), false);
}

// 맞혔다 — damage·remainHp 는 CCombatSystem 이 한 번 센 값 그대로다(깎인 체력과 화면의 숫자가 갈리지 않게)
void CGameServer::OnHit(CZone* zone, CCharacter* attacker, CCharacter* target, int32_t damage, int32_t remainHp)
{
    BroadcastAroundSector(zone, attacker,
        MakeDamage(target->_actorId, attacker->_actorId, damage, remainHp), false);
}

// 죽었다 — 죽은 목록 → 죽음 방송 → 경험치 → 드랍, 이 순서다.
//   ★ 순서를 바꾸면 레벨업 체력(ACTOR_STATS)이 죽음(DEATH)보다 먼저 가 화면이 뒤집힌다.
//   죽음 필드(_isDead·_deathFrame·_moveState)와 죽음 계수는 CCombatSystem 이 이미 적었다.
void CGameServer::OnDeath(CZone* zone, CCharacter* attacker, CCharacter* target)
{
    _deadActors.push_back(target);

    BroadcastAroundSector(zone, attacker, MakeDeath(target->_actorId), false);

#if USE_EXP && USE_MONSTER
    // ── 6) 경험치 — 사람이 몬스터를 잡았을 때만 ──
    //   ★ 막타 한 명에게만 준다. 나눠 주려면 누가 얼마나 때렸는지를 몬스터가 들고 있어야 하고,
    //     그건 퇴장 경로가 훑어야 할 대기열이 하나 더 느는 일이다(Monster.h [대상] 과 같은 이유).
    //   때린 쪽은 CCharacter* 라 AsPlayer 로 확인 후 내려 바꾼다 — _tickClampedPlayers 순회와 같은 꼴.
    //   PvP 는 안 준다: 당한 쪽이 몬스터일 때만이다.
    CPlayer* killer = AsPlayer(attacker);
    if (killer != nullptr && target->_kind == CharacterKind::MONSTER)
    {
        const int32_t reward = SNAKE_EXP_REWARD;   // 종류가 하나뿐 — 늘어나면 종류별 표에서 뽑는다

        // ★ 준 뒤에 센다 — GainExp 는 만렙이면 한 톨도 안 쌓는다.
        //   먼저 세면 만렙 이후 잡은 수만큼 "줬다"가 늘어, 계수기가 실제와 갈린다.
        const int64_t beforeExp = killer->_exp;
        const bool leveled = GainExp(killer, reward);
        const bool gained  = (killer->_exp != beforeExp);

        if (gained)
        {
            _gameMonitor._exp._granted.Inc();
            _gameMonitor._exp._grantedAmount.Add(killer->_exp - beforeExp);

            SendExp(killer);

            // ★ 경험치가 늘었으면 저장 대상이다 — 안 켜면 레벨업이 주기 저장에 안 실린다.
            //   불러온 캐릭터는 dirty 가 아닌 채로 들어오므로(값이 DB 와 같다) 여기서 안 켜면 영영 안 써진다.
#if USE_DB_WORKER
            killer->_dbDirty = true;
#endif
        }

        if (leveled)
        {
            // 늘어난 체력을 본인과 주변에 다시 알린다 — 막대가 옛 최대치로 남지 않게.
            //   excludeSelf=false 라 본인에게도 간다(부활 방송과 같은 꼴).
            BroadcastAroundSector(zone, killer,
                MakeActorStats(killer->_actorId, killer->_hp, killer->_maxHp), false);

            // 오른 공격력·방어력은 본인에게만 — 남의 공격력은 화면에 안 쓴다(S2C_MY_STATS 는 본인 전용)
            SendMyStats(killer);
        }
    }
#endif

#if USE_DROP
    // ── 7) 드랍 — 사람이 몬스터를 잡았을 때만(경험치와 같은 조건) ──
    //   막타 한 명이 주인이다(경험치와 같은 이유 — 누가 얼마나 때렸는지 몬스터가 들고 있지 않다).
    if (CPlayer* looter = AsPlayer(attacker); looter != nullptr && target->_kind == CharacterKind::MONSTER)
        DropLoot(zone, looter, static_cast<CMonster*>(target));
#endif
}

// ==========================================================================
// 스탯 — 최대 체력·공격력·방어력을 세우는 한 곳
//   공격력·방어력은 최종값을 필드에 둔다(Character.h [전투 스탯]). 때리는 경로는 필드만 읽는다.
//   ★ USE_EXP 밖이다 — 표를 끈 빌드는 기본값을 바탕으로 쓴다(CHARACTER_DEFAULT_* = 표의 1레벨 칸).
// ==========================================================================
void CGameServer::RecalcStats(CPlayer* player, bool refillHp)
{
#if USE_EXP
    const CLevelTable& table = _gameDataFiles.Level();
    player->_maxHp       = table.MaxHp(player->_level);
    player->_attackPower = table.AttackPower(player->_level);
    player->_defense     = table.Defense(player->_level);
#else
    player->_maxHp       = CHARACTER_DEFAULT_MAX_HP;
    player->_attackPower = CHARACTER_DEFAULT_ATTACK_POWER;
    player->_defense     = CHARACTER_DEFAULT_DEFENSE;
#endif

#if USE_EQUIP
    // 착용 칸의 몫 — 칸마다 아이템 표의 attack 을 더한다(지금은 무기 칸 하나).
    //   표에서 사라진 번호(패치로 지운 아이템)는 더하지 않는다 — 불러오기가 이미 건너뛰므로 평소엔 안 닿는다.
    for (int w = 0; w < INVENTORY_WORN_SLOTS; ++w)
    {
        const Item& worn = player->_inventory.At(WornIndexOf(static_cast<EquipSlot>(w)));
        if (worn.itemId == 0)
            continue;
        if (const ItemDef* def = _gameDataFiles.Item().Find(worn.itemId))
            player->_attackPower += def->attack;
    }
#endif

    // 레벨이 올랐을 때만 채운다. 입장 때는 DB 가 들고 있던 체력이 아직 없으므로(스탯만 맞춘다)
    //   호출부가 정한다 — 지금은 스폰도 만피라 둘 다 true 로 들어온다.
    if (refillHp)
        player->_hp = player->_maxHp;
    else if (player->_hp > player->_maxHp)
        player->_hp = player->_maxHp;   // 표를 낮춘 패치 뒤엔 들고 있던 체력이 최대를 넘을 수 있다
}

void CGameServer::SendMyStats(CPlayer* player)
{
    SendPacket(player, MakeMyStats(player->_attackPower, player->_defense));
    _gameMonitor._combat._myStatsSent.Inc();
}

#if USE_EXP
// ==========================================================================
// 경험치·레벨
//   표(CLevelTable)는 기획 데이터라 파일에서 왔고, 여기서는 읽기만 한다.
//   ★ 누적 기준 — _exp 는 "지금까지 얻은 총합"이고 표의 필요치도 같은 기준이다.
//     레벨 안에서 다시 0부터 세지 않아서 레벨업이 한 줄이고 넘친 경험치가 안 사라진다.
// ==========================================================================

bool CGameServer::ClampLevelToTable(CPlayer* player)
{
    const CLevelTable& table = _gameDataFiles.Level();
    const int32_t maxLevel = table.MaxLevel();

    if (player->_level >= 1 && player->_level <= maxLevel)
        return false;

    SLOG_WARN("[EXP] level out of table -> clamp (account={}, level={}, table max={})",
              player->_accountId, player->_level, maxLevel);
    player->_level = (player->_level < 1) ? 1 : maxLevel;
    return true;
}

bool CGameServer::GainExp(CPlayer* player, int64_t amount)
{
    const CLevelTable& table = _gameDataFiles.Level();
    const int32_t maxLevel = table.MaxLevel();

    // 만렙이면 더 안 쌓는다 — 쌓아 봐야 쓸 데가 없고, 숫자만 끝없이 커진다.
    if (player->_level >= maxLevel)
        return false;

    if (amount > 0)
        player->_exp += amount;

    // ★ 앞의 level < maxLevel 이 없으면 만렙에서 table[maxLevel + 1] 을 읽어 배열 밖이다.
    //   한 번에 여러 레벨이 오를 수 있으므로 if 가 아니라 while 이다.
    const int32_t before = player->_level;
    while (player->_level < maxLevel && player->_exp >= table.RequiredExp(player->_level + 1))
        ++player->_level;

    if (player->_level == before)
        return false;

    RecalcStats(player, true);   // 오르면 체력을 가득 채운다
    _gameMonitor._exp._levelUps.Inc();
    return true;
}

void CGameServer::SendExp(CPlayer* player)
{
    const CLevelTable& table = _gameDataFiles.Level();
    const int32_t maxLevel = table.MaxLevel();

    // 만렙이면 다음 필요치가 0 — 클라는 0 을 보면 나누지 말고 MAX 로 그린다.
    const int64_t nextExp = (player->_level < maxLevel) ? table.RequiredExp(player->_level + 1) : 0;

    SendPacket(player, MakeExp(player->_level, player->_exp,
                               table.RequiredExp(player->_level), nextExp));
}
#endif // USE_EXP

// 부활 대기자 훑기 — 틱마다 1회. 죽은 것만 따로 들고 있어 존 전체를 돌지 않는다.
void CGameServer::TickRespawn()
{
    if (_deadActors.empty())
        return;

    for (size_t i = 0; i < _deadActors.size(); )
    {
        CCharacter* dead = _deadActors[i];

        if (!Combat::IsRespawnDue(*dead, _frameCount))
        {
            ++i;
            continue;
        }

        Combat::Respawn(*dead);

        // 부활도 "되살아난 쪽"으로 가른다 (죽음과 같은 기준 — GameMonitor.h 참조)
#if USE_MONSTER
        if (dead->_kind == CharacterKind::MONSTER) _gameMonitor._monster._respawnsTotal.Inc();
        else                                       _gameMonitor._combat._respawnsTotal.Inc();
#else
        _gameMonitor._combat._respawnsTotal.Inc();
#endif

        CZone* zone = _mapManager.GetZone(dead->_zoneId);
        if (zone != nullptr)
        {
#if USE_MONSTER
            // 몬스터는 같은 구역 안 다른 자리로 옮겨 되살아난다 — 제자리 부활이면 잡은 자리에 3초마다 다시 선다.
            //   플레이어는 지금 그대로 제자리다(부활 지점·마을은 따로 정할 일).
            if (dead->_kind == CharacterKind::MONSTER)
                RespawnMonsterElsewhere(zone, static_cast<CMonster*>(dead));
            else
#endif
            {
                // 본인 포함 — 자기 부활을 자기가 알아야 클라가 조작을 다시 연다
                BroadcastAroundSector(zone, dead,
                    MakeRespawn(dead->_actorId, dead->_x, dead->_y, dead->_hp), false);
            }
        }

        // 순서가 뜻을 갖지 않는 대기열이라 뒤엣것과 바꿔치기로 O(1) 제거.
        //   i를 올리지 않는다 — 방금 당겨온 것을 아직 안 봤다.
        _deadActors[i] = _deadActors.back();
        _deadActors.pop_back();
    }
}

// 부활 대기열에서 빼기 — 퇴장 경로가 부른다.
//   _isDead로 미리 거르지 않는다. 플래그와 대기열이 어긋나는 경로가 나중에 생겨도
//   여기서 조용히 새지 않게 하려는 것이고, 대기열 길이는 "지금 죽어 있는 수"라 훑는 비용이 없다.
void CGameServer::RemoveFromDeadActors(CCharacter* player)
{
    if (_deadActors.empty())
        return;

    _deadActors.erase(
        std::remove(_deadActors.begin(), _deadActors.end(), player),
        _deadActors.end());
}
#endif // USE_COMBAT

#if USE_MONSTER
// ==========================================================================
// 몬스터 (USE_MONSTER)
// ==========================================================================

// 게임 루프 1-1) — 아직 몬스터를 안 깐 존을 찾아 스폰한다.
//   "존이 만들어질 때 부른다"가 아니라 루프가 훑는 이유 — 존은 CMapInstance::CreateChannel
//   (private)에서만 생기고 그 호출처가 전부 MapManager 안이다. CGameServer 는
//   FindOrCreateChannel 의 반환값만 받으므로 그게 새 존인지 원래 있던 존인지 알 길이 없다.
//   받는 자리 셋에 "스폰했나"를 흩으면 하나를 빠뜨린 날 몬스터 없는 채널이 생긴다.
//   비용은 틱마다 존 수만큼의 플래그 비교이고, 스폰은 존마다 딱 한 번만 돈다.
void CGameServer::TickMonsters()
{
    _mapManager.ForEachZone([this](CZone* zone)
    {
        if (!zone->IsMonsterSpawned())
            SpawnMonsters(zone);

        for (const auto& monster : zone->GetMonsters())
            ThinkMonster(zone, monster.get());
    });
}

// 몬스터 하나의 판단 — 대상 찾기와 송신은 여기가 하고, "무엇을 할까"는 MonsterAI 가 정한다.
void CGameServer::ThinkMonster(CZone* zone, CMonster* monster)
{
    // 매 틱 돌지 않는다 — 몬스터마다 0.4초에 한 번. 스폰 때 프레임을 흩어 한 틱에 안 몰린다.
    //   게임 루프가 단일 코어라 여기서 아끼는 것이 그대로 동접 여유가 된다.
    if (_frameCount < monster->_nextThinkFrame)
        return;

    _gameMonitor._monster._thinkCalls.Inc();

    CCharacter* target = FindAITarget(zone, monster);
    const MonsterAI::Decision d = MonsterAI::Think(*monster, target, _frameCount);

    // 상태·대상·다음 판단 시점은 그대로 적는다 (와이어에 안 나가는 서버 내부 값)
    if (d.nextState != monster->_aiState)
        _gameMonitor._monster._stateTransitions.Inc();
    monster->_aiState        = d.nextState;
    monster->_targetActorId  = d.targetActorId;
    monster->_nextThinkFrame = d.nextThinkFrame;
    monster->_stateEndFrame  = d.stateEndFrame;   // 배회 기간 — 다음 판단 시점과 따로 들고 간다

    // ── 방향·이동 상태는 바뀐 것만 브로드캐스트한다 ──
    //   안 바뀐 것까지 보내면 뱀 한 마리가 0.4초마다 주변에 패킷을 뿌린다.
    const bool dirChanged  = (d.direction != monster->_direction);
    const bool moveChanged = (d.moveState != monster->_moveState);

    monster->_direction = d.direction;
    monster->_moveState = d.moveState;

    if (moveChanged || (dirChanged && d.moveState == MoveState::MOVING))
    {
        // 걷기 시작(또는 걷는 중에 방향을 틀었다) → MOVE_START 로 새 방향을 알린다.
        //   멈췄다면 MOVE_STOP. 좌표는 이미 서버 것이 권위라 그대로 실린다.
        if (d.moveState == MoveState::MOVING)
            NotifyMoveStart(zone, monster);
        else
            NotifyMoveStop(zone, monster, false);   // 본인 포함 — 몬스터는 세션이 없어 어차피 안 간다
    }

    // ── 공격 ──
    //   ★ USE_COMBAT 안에 둔다 — 공격 본체(CCombatSystem::PerformAttack)가 전투 토글 소유다.
    //     전투를 끄면 몬스터는 배회·추적까지만 하고 물지 않는다. 그게 맞는 동작이고,
    //     이 가드를 빼면 USE_COMBAT=0 + USE_MONSTER=1 조합이 컴파일되지 않는다.
#if USE_COMBAT
    if (d.attack)
        _combatSystem.PerformAttack(zone, monster, _frameCount);
#endif
}

// AI 가 쫓을 대상 하나를 고른다 — 시야 안에서 가장 가까운 "산 플레이어".
//   ★ 플레이어 목록만 돈다. 섞인 목록에서 종류로 거르던 시절엔 안 거르면 뱀이 옆의 뱀을 쫓아가 물었다.
//     플레이어 목록만 도는 자리가 둘이다 — 여기(누구를 쫓을까)와 CCombatSystem::PerformAttack(실제로 누구를 무나).
//     여기만 그러면 플레이어를 쫓아간 뱀이 정면에 선 다른 뱀을 문다(실측으로 확인).
//     CCombatSystem::PerformAttack 쪽은 "때리는 쪽이 몬스터일 때만" 플레이어 목록이므로 플레이어는 뱀을 그대로 때린다.
CCharacter* CGameServer::FindAITarget(CZone* zone, CMonster* monster)
{
    // 시야(6타일)가 걸치는 섹터만 참조로 훑는다 — GetAroundPlayers(3×3, 60×60타일)를
    //   쓰지 않는 이유는 공격 본체와 같다(그건 브로드캐스트용이라 후보를 벡터에 전부 복사한다).
    const CSectorManager& sectors = zone->GetSectorManager();
    const int32_t sx0 = sectors.CalcSectorX(monster->_x - MonsterAI::SIGHT_RANGE);
    const int32_t sx1 = sectors.CalcSectorX(monster->_x + MonsterAI::SIGHT_RANGE);
    const int32_t sy0 = sectors.CalcSectorY(monster->_y - MonsterAI::SIGHT_RANGE);
    const int32_t sy1 = sectors.CalcSectorY(monster->_y + MonsterAI::SIGHT_RANGE);

    CCharacter* best = nullptr;
    float bestDistSq = 0.0f;

    for (int32_t sy = sy0; sy <= sy1; ++sy)
    {
        for (int32_t sx = sx0; sx <= sx1; ++sx)
        {
            for (CPlayer* candidate : sectors.GetSectorPlayers(sx, sy))   // 플레이어만 — 몬스터끼리 싸우지 않고, 자기 자신도 여기 없다
            {
                if (candidate->_isDead)
                    continue;   // 시체는 쫓지 않는다

                float distSq = 0.0f;
                if (!MonsterAI::IsInSight(*monster, *candidate, distSq))
                    continue;

                if (best == nullptr || distSq < bestDistSq)
                {
                    best = candidate;
                    bestDistSq = distSq;
                }
            }
        }
    }

    return best;
}

void CGameServer::SpawnMonsters(CZone* zone)
{
    zone->MarkMonsterSpawned();   // 실패하더라도 매 틱 재시도하지 않게 먼저 세운다

    const int32_t spawnCount = (_monsterPerZone > 0) ? _monsterPerZone : MONSTERS_PER_ZONE;

    // 구역마다 몇 마리 — 총수(INI)를 스폰 구역 표의 비율대로 정수로 딱 나눈다.
    //   이 맵에 구역이 없는 일은 없다 — 기동 때 CheckMaps 가 설정된 맵마다 구역 ≥ 1 을 보고, 아니면 서버를 안 띄웠다.
    std::vector<int32_t> perRegion;
    _gameDataFiles.SpawnRegions().SplitCount(zone->GetMapId(), spawnCount, perRegion);

    int32_t i = 0;   // 존 안 순번 — 첫 판단 시점을 흩는 데 쓴다
    for (size_t region = 0; region < perRegion.size(); ++region)
    {
        for (int32_t k = 0; k < perRegion[region]; ++k, ++i)
        {
            auto owned = std::make_unique<CMonster>();
            CMonster* monster = owned.get();       // 빌려 쓰는 포인터 — 소유는 아래 AddMonster 에서 존으로 넘어간다
            monster->_actorId = AllocPlayerId();   // 단일 증가 카운터라 플레이어와 충돌하지 않는다
            if (_snakeSpeed > 0)
                monster->_speed = _snakeSpeed;     // 게임용 속도 — 0 이면 SNAKE_SPEED(15) 그대로

            // 자리는 배정받은 구역 안 무작위(사람 근처면 다시 뽑음). 방향·정지 상태·섹터 등록·주민 목록 추가는
            //   EnterZoneAt 이 전부 한다.
            float x = 0.0f;
            float y = 0.0f;
            PickSpawnPos(zone, static_cast<int32_t>(region), x, y);
            if (zone->AddMonster(std::move(owned), x, y) == nullptr)
                continue;   // 입장 실패 — 존이 그 자리에서 해제했다(monster 는 이제 쓰면 안 된다)

            monster->_spawnRegion = static_cast<int32_t>(region);   // 부활할 때 같은 구역 안으로 간다
            monster->_spawnX = monster->_x;   // 귀환을 넣을 때 쓴다
            monster->_spawnY = monster->_y;

            // 첫 판단 시점을 흩는다 — 다 같은 프레임에 깨어나면 한 존의 몬스터가 한 틱에 몰려
            //   그 틱만 튄다. 판단 주기 안에서 고르게 퍼뜨린다.
            monster->_nextThinkFrame = _frameCount + (i % MonsterAI::THINK_INTERVAL_FRAMES);

            // 주변에 알린다 — 이미 그 자리를 보고 있던 플레이어에게 CREATE 가 간다.
            //   보는 사람이 없으면 아무에게도 안 가고, 나중에 걸어온 사람은 섹터 진입 때 받는다.
            BroadcastSpawn(zone, monster, SpawnReason::SPAWN);
        }
    }

    // 구역별 마릿수 — 표를 바꾼 뒤 비율이 뜻대로 나뉘었는지 로그 한 줄로 본다(시험도 이 줄을 읽는다)
    std::string split;
    for (size_t region = 0; region < perRegion.size(); ++region)
    {
        if (region > 0)
            split += " / ";
        split += std::to_string(perRegion[region]);
    }
    SLOG_INFO("[Monster] zone {} spawned {} (regions {})", zone->GetZoneId(), i, split);
}

// 태어날 자리 — 구역 안 균등 무작위. 산 플레이어가 가까우면 다시 뽑는다(헤더 주석).
void CGameServer::PickSpawnPos(CZone* zone, int32_t regionIndex, float& outX, float& outY)
{
    static_assert(SPAWN_CLEAR_RANGE == MonsterAI::SIGHT_RANGE,
                  "태어나자마자 누군가를 알아채지 않게 — 비우는 반경은 뱀 시야와 같아야 한다");

    const std::vector<SpawnRegion>& regions = _gameDataFiles.SpawnRegions().RegionsOf(zone->GetMapId());
    // [불변식] 색인은 SpawnMonsters 가 같은 목록에서 준 것이고, 목록은 기동 때 CheckMaps 가 비어 있지 않음을 봤다
    assert(regionIndex >= 0 && regionIndex < static_cast<int32_t>(regions.size()));
    const SpawnRegion& r = regions[static_cast<size_t>(regionIndex)];

    // 게임 스레드 전용 — 서버의 다른 난수 자리(Zone.cpp CalcSpawnPos · MonsterAI.cpp)와 같은 꼴
    thread_local std::mt19937 rng{ std::random_device{}() };
    std::uniform_real_distribution<float> distX(static_cast<float>(r.x0), static_cast<float>(r.x1));
    std::uniform_real_distribution<float> distY(static_cast<float>(r.y0), static_cast<float>(r.y1));

    for (int32_t attempt = 1; ; ++attempt)
    {
        outX = distX(rng);
        outY = distY(rng);
        if (!IsLivePlayerNear(zone, outX, outY, SPAWN_CLEAR_RANGE))
            return;

        if (attempt >= SPAWN_MAX_TRIES)
        {
            _gameMonitor._monster._spawnRerollExhausted.Inc();   // 다 걸렸다 — 마지막 자리를 그대로 쓴다
            return;
        }
        _gameMonitor._monster._spawnRerolls.Inc();
    }
}

// (x, y) 에서 range 안에 산 플레이어가 있나 — 반경이 걸치는 섹터의 플레이어 목록만 참조로 훑는다(복사 0)
bool CGameServer::IsLivePlayerNear(CZone* zone, float x, float y, float range) const
{
    const CSectorManager& sectors = zone->GetSectorManager();
    const int32_t sx0 = sectors.CalcSectorX(x - range);
    const int32_t sx1 = sectors.CalcSectorX(x + range);
    const int32_t sy0 = sectors.CalcSectorY(y - range);
    const int32_t sy1 = sectors.CalcSectorY(y + range);
    const float rangeSq = range * range;

    for (int32_t sy = sy0; sy <= sy1; ++sy)
    {
        for (int32_t sx = sx0; sx <= sx1; ++sx)
        {
            for (const CPlayer* p : sectors.GetSectorPlayers(sx, sy))
            {
                if (p->_isDead)
                    continue;   // 시체 옆에 태어나는 건 괜찮다 — 알아챌 대상이 아니다
                const float dx = p->_x - x;
                const float dy = p->_y - y;
                if (dx * dx + dy * dy < rangeSq)
                    return true;
            }
        }
    }
    return false;
}

#if USE_COMBAT
// 몬스터 부활 — 같은 구역 안 다른 자리로 옮긴다. 불리는 자리·금지 사항은 헤더 주석.
void CGameServer::RespawnMonsterElsewhere(CZone* zone, CMonster* monster)
{
    // 틱 3-2 — 섹터 변경 대기열을 처리하고 비운 뒤다(GameLoopThread). 누가 TickRespawn 을 앞으로 옮기면 여기서 걸린다.
    assert(_pendingSectorChanges.empty());

    CSectorManager& sectors = zone->GetSectorManager();

    // ⑴ 옛 자리 — 시체가 사라진다. 그 자리를 보던 플레이어에게만 간다
    _eventAroundBuffer.clear();
    sectors.GetAroundPlayers(monster->_sectorX, monster->_sectorY, _eventAroundBuffer, monster);
    for (CPlayer* other : _eventAroundBuffer)
        SendDeletePlayer(other, monster);

    // ⑵ 새 자리 — 같은 구역 안. 섹터를 옮기는 세 줄은 RecvMoveStop 과 같은 꼴이다
    float x = 0.0f;
    float y = 0.0f;
    PickSpawnPos(zone, monster->_spawnRegion, x, y);
    monster->_x = x;
    monster->_y = y;
    monster->_lastSyncX = x;   // 델타 동기화 기준도 새 자리로 — 옛 자리와의 차이로 동기화가 한 번 튀지 않게
    monster->_lastSyncY = y;
    monster->_spawnX = x;
    monster->_spawnY = y;

    const int32_t newSectorX = sectors.CalcSectorX(x);
    const int32_t newSectorY = sectors.CalcSectorY(y);
    if (newSectorX != monster->_sectorX || newSectorY != monster->_sectorY)
    {
        sectors.RemoveCharacter(monster, monster->_sectorX, monster->_sectorY);
        monster->_sectorX = newSectorX;
        monster->_sectorY = newSectorY;
        sectors.AddCharacter(monster, newSectorX, newSectorY);
    }

    // ⑶ 새 자리 주변에 등장 — CREATE(SPAWN) 가 가득 찬 체력을 싣는다(ActorSnapshot).
    //   옛·새 자리를 둘 다 보는 사람은 ⑴ 의 DELETE 다음 이 CREATE 를 받는다 — 둘 다 곧바로 보내는 경로라 순서가 안 뒤집힌다.
    BroadcastSpawn(zone, monster, SpawnReason::SPAWN);
}
#endif // USE_COMBAT

// 존이 사라지기 전에 몬스터를 대기열에서 뺀다 — 해제는 존이 한다(ClearMonsters).
void CGameServer::DespawnMonsters(CZone* zone)
{
    for (const auto& monster : zone->GetMonsters())
    {
        // 섹터 변경·부활 대기열에서 빼는 것이 핵심이다. 안 빼고 해제하면
        //   다음 틱이 해제된 주소를 밟는다(BroadcastLeaveZone 이 그 셋을 전부 훑는다).
        BroadcastLeaveZone(zone, monster.get());
    }

    // 순회가 끝난 뒤 한 번에 해제한다 — 도는 중에 LeaveZone 이 이 목록을 건드리지 않는 이유(Zone.h)
    zone->ClearMonsters();
}
#endif // USE_MONSTER

#if USE_DROP
// ==========================================================================
// 드랍 (USE_DROP) — 땅의 아이템
//   땅의 아이템은 섹터의 셋째 목록에 값으로 담긴다(SectorManager.h [셋째 목록]).
// ==========================================================================

void CGameServer::DropLoot(CZone* zone, CPlayer* killer, CMonster* monster)
{
    _gameMonitor._drop._rolls.Inc();   // 아무것도 안 떨어져도 1 — "잡은 수"와 같아야 한다

    // 게임 스레드 전용 — 서버의 다른 난수 자리와 같은 꼴. 굴림 자체는 순수 함수(DropTable.h RollDrops)
    thread_local std::mt19937 rng{ std::random_device{}() };
    RollDrops(_gameDataFiles.Drop().RowsOf(monster->_kindId), rng, _lootBuffer);
    if (_lootBuffer.empty())
        return;

    // 놓는 자리 — 첫 아이템은 죽은 자리, 다음부터 옆 칸. 한 점에 포개면 웹이 그림 하나만 그린다.
    static constexpr float OFFSETS[][2] = {
        { 0, 0 }, { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 },
    };
    constexpr size_t OFFSET_COUNT = sizeof(OFFSETS) / sizeof(OFFSETS[0]);
    const float maxX = static_cast<float>(zone->GetMapWidth()) - 1.0f;    // 이동 쪽 클램프(CZone::Tick)와 같은 범위
    const float maxY = static_cast<float>(zone->GetMapHeight()) - 1.0f;

    CSectorManager& sectors = zone->GetSectorManager();
    for (size_t i = 0; i < _lootBuffer.size(); ++i)
    {
        CGroundItem g;
        g.groundId        = _nextGroundId++;
        g.item            = _lootBuffer[i];
        const float fx    = monster->_x + OFFSETS[i % OFFSET_COUNT][0];
        const float fy    = monster->_y + OFFSETS[i % OFFSET_COUNT][1];
        g.x               = fx < 0.0f ? 0.0f : (fx > maxX ? maxX : fx);
        g.y               = fy < 0.0f ? 0.0f : (fy > maxY ? maxY : fy);
        g.ownerAccountId  = killer->_accountId;
        g.ownerUntilFrame = _frameCount + LOOT_LOCK_FRAMES;
        g.expireFrame     = _frameCount + GROUND_ITEM_LIFE_FRAMES;

        // 아이템마다 자기 섹터 — 경계에 떨어지면 한 번에 떨어진 것들의 섹터가 서로 다를 수 있다
        const int32_t sx = sectors.CalcSectorX(g.x);
        const int32_t sy = sectors.CalcSectorY(g.y);
        sectors.AddGroundItem(g, sx, sy);
        ++_groundItemCount;
        _gameMonitor._drop._dropped.Inc();

        _eventAroundBuffer.clear();
        sectors.GetAroundPlayers(sx, sy, _eventAroundBuffer);
        for (CPlayer* other : _eventAroundBuffer)
            SendGroundItemCreate(other, g, GroundItemAppear::DROP);
    }
    _gameMonitor._drop._groundItems.Store(_groundItemCount);
}

void CGameServer::SendGroundItemCreate(CPlayer* target, const CGroundItem& item, GroundItemAppear reason)
{
    SendPacket(target, MakeGroundItemCreate(item, reason));
}

void CGameServer::SendGroundItemDelete(CPlayer* target, int32_t groundId, GroundItemGone reason, int32_t pickerId)
{
    SendPacket(target, MakeGroundItemDelete(groundId, reason, pickerId));
}

void CGameServer::RemoveGroundItem(CZone* zone, int32_t sectorX, int32_t sectorY, size_t index,
                                   GroundItemRemoval why, int32_t pickerId)
{
    CSectorManager& sectors = zone->GetSectorManager();
    const int32_t groundId = sectors.GetSectorItems(sectorX, sectorY)[index].groundId;   // 빼기 전에 읽는다

    // 와이어의 사유 — 빼는 까닭 셋 중 알릴 사람이 있는 것은 둘뿐이다(CLEANED 는 ForgetGroundItems)
    GroundItemGone wire = GroundItemGone::EXPIRED;
    switch (why)
    {
    case GroundItemRemoval::PICKED:  wire = GroundItemGone::PICKED;  break;
    case GroundItemRemoval::EXPIRED: wire = GroundItemGone::EXPIRED; break;
    case GroundItemRemoval::CLEANED:
    case GroundItemRemoval::COUNT:
        assert(false && "CLEANED 는 알릴 사람이 없다 — ForgetGroundItems 가 센다");
        return;
    }

    _eventAroundBuffer.clear();
    sectors.GetAroundPlayers(sectorX, sectorY, _eventAroundBuffer);
    for (CPlayer* other : _eventAroundBuffer)
        SendGroundItemDelete(other, groundId, wire, pickerId);

    // 같은 틱에 섹터를 넘은 사람 — 틱 1단계(줍기)에서만 대기열이 차 있다. 사라지기(3-3)는 비운 뒤라 여기서 끝난다.
    if (!_pendingSectorChanges.empty())
        SendGroundItemDeleteToLateMovers(zone, sectorX, sectorY, groundId, wire, pickerId);

    sectors.RemoveGroundItemAt(sectorX, sectorY, index);
    _gameMonitor._drop._removed[static_cast<int>(why)].Inc();
    --_groundItemCount;
    _gameMonitor._drop._groundItems.Store(_groundItemCount);
}

void CGameServer::TickGroundItems()
{
    if (++_groundItemScanCounter < GROUND_ITEM_SCAN_FRAMES)
        return;
    _groundItemScanCounter = 0;

    if (_groundItemCount == 0)
        return;   // 땅에 아무것도 없다 — 존·섹터를 훑지 않는다(부하 시험은 더미가 안 때려 늘 여기서 끝난다)

    _mapManager.ForEachZone([this](CZone* zone)
    {
        const CSectorManager& sectors = zone->GetSectorManager();
        for (int32_t sy = 0; sy < sectors.GetSectorCountY(); ++sy)
        {
            for (int32_t sx = 0; sx < sectors.GetSectorCountX(); ++sx)
            {
                const std::vector<CGroundItem>& list = sectors.GetSectorItems(sx, sy);
                for (size_t i = 0; i < list.size(); )
                {
                    if (_frameCount >= list[i].expireFrame)
                        RemoveGroundItem(zone, sx, sy, i, GroundItemRemoval::EXPIRED, 0);   // 뒤엣것이 i 로 온다 — 올리지 않는다
                    else
                        ++i;
                }
            }
        }
    });
}

void CGameServer::ForgetGroundItems(CZone* zone)
{
    const CSectorManager& sectors = zone->GetSectorManager();
    int64_t n = 0;
    for (int32_t sy = 0; sy < sectors.GetSectorCountY(); ++sy)
    {
        for (int32_t sx = 0; sx < sectors.GetSectorCountX(); ++sx)
            n += static_cast<int64_t>(sectors.GetSectorItems(sx, sy).size());
    }
    if (n == 0)
        return;

    _gameMonitor._drop._removed[static_cast<int>(GroundItemRemoval::CLEANED)].Add(n);
    _groundItemCount -= n;
    _gameMonitor._drop._groundItems.Store(_groundItemCount);
}

PickupFail CGameServer::CheckPickup(const CPlayer* player, CZone* zone, PickupTarget& out) const
{
    if (player->_isDead)
        return PickupFail::DEAD;

    // 반경이 걸치는 섹터만 참조로 훑는다(공격 대상 찾기와 같은 꼴 — 복사 0)
    const CSectorManager& sectors = zone->GetSectorManager();
    const int32_t sx0 = sectors.CalcSectorX(player->_x - PICKUP_RANGE);
    const int32_t sx1 = sectors.CalcSectorX(player->_x + PICKUP_RANGE);
    const int32_t sy0 = sectors.CalcSectorY(player->_y - PICKUP_RANGE);
    const int32_t sy1 = sectors.CalcSectorY(player->_y + PICKUP_RANGE);
    const float rangeSq = PICKUP_RANGE * PICKUP_RANGE;

    bool  anyInRange = false;   // 반경 안에 뭔가 있긴 했다 — "아무것도 없음(NO_ITEM)"과 "남의 것뿐(NOT_OWNER)"을 가른다
    bool  found      = false;
    float bestSq     = 0.0f;
    for (int32_t sy = sy0; sy <= sy1; ++sy)
    {
        for (int32_t sx = sx0; sx <= sx1; ++sx)
        {
            const std::vector<CGroundItem>& list = sectors.GetSectorItems(sx, sy);
            for (size_t i = 0; i < list.size(); ++i)
            {
                const CGroundItem& g = list[i];
                const float dx = g.x - player->_x;
                const float dy = g.y - player->_y;
                const float dSq = dx * dx + dy * dy;
                if (dSq > rangeSq)
                    continue;
                anyInRange = true;

                // 내 것이거나(막타 친 계정) 우선 시간이 끝났으면 주울 수 있다
                const bool mine = g.ownerAccountId == 0 || g.ownerAccountId == player->_accountId ||
                                  _frameCount >= g.ownerUntilFrame;
                if (!mine)
                    continue;

                if (!found || dSq < bestSq)
                {
                    found  = true;
                    bestSq = dSq;
                    out.sectorX = sx;
                    out.sectorY = sy;
                    out.index   = i;
                }
            }
        }
    }

    if (found)
        return PickupFail::COUNT;
    return anyInRange ? PickupFail::NOT_OWNER : PickupFail::NO_ITEM;
}

void CGameServer::RecvPickup(CPlayer* player, CSerialBuffer* /*pMsg*/)   // 몸이 없다 — 헤더뿐
{
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    PickupTarget at;
    PickupFail fail = CheckPickup(player, zone, at);
    int32_t failItemId = 0;

    if (fail == PickupFail::COUNT)
    {
        const CGroundItem& g = zone->GetSectorManager().GetSectorItems(at.sectorX, at.sectorY)[at.index];
        const ItemDef* def = _gameDataFiles.Item().Find(g.item.itemId);
        if (def == nullptr)
        {
            assert(false && "드랍 표 로드가 아이템 표에 없는 번호를 막는다");
            return;
        }

        // ★ 가방 넣기가 먼저, 땅에서 빼기가 나중 — 반대면 가득 찬 가방에서 아이템이 사라진다
        const InvAddResult r = player->_inventory.TryAdd(*def, g.item.count);
        if (r == InvAddResult::OK)
        {
            _gameMonitor._inventory._itemsAdded.Add(g.item.count);   // /item 과 같은 계수기 — 가방에 들어간 개수
            RemoveGroundItem(zone, at.sectorX, at.sectorY, at.index, GroundItemRemoval::PICKED, player->_actorId);
            FlushInventory(player);   // 칸 패킷 + 곧바로 DB 저장(지금 경로 그대로)
            return;
        }

        // 드랍 아이템은 표에 있고 개수 1 이상이라 TryAdd 가 돌려줄 거절은 가득(TAB_FULL)뿐이다
        _gameMonitor._inventory._addRejected[static_cast<int>(r)].Inc();
        fail = PickupFail::TAB_FULL;
        failItemId = g.item.itemId;   // 클라가 어느 탭이 찼는지 안다
    }

    _gameMonitor._drop._pickupRejected[static_cast<int>(fail)].Inc();
    SendPacket(player, MakePickupFail(fail, failItemId));
}
#endif // USE_DROP

void CGameServer::SendZoneChangeOk(CPlayer* target, int32_t mapId, int32_t channelIndex)
{
    SendPacket(target, MakeZoneChangeOk(target, mapId, channelIndex));
}

void CGameServer::SendZoneChangeFail(CPlayer* target, uint8_t reason)
{
    SendPacket(target, MakeZoneChangeFail(reason));
}

// ==========================================================================
// 벽 방향 검증 — 경계 위치에서 벽 쪽 이동 차단 (클라이언트 IsBlockedByWall과 동일)
// ==========================================================================

bool CGameServer::IsBlockedByWall(CZone* zone, CCharacter* player, Direction dir)
{
    float maxX = static_cast<float>(zone->GetMapWidth()) - 1.0f;
    float maxY = static_cast<float>(zone->GetMapHeight()) - 1.0f;

    bool atLeft   = (player->_x <= 0.0f);
    bool atRight  = (player->_x >= maxX);
    bool atTop    = (player->_y <= 0.0f);
    bool atBottom = (player->_y >= maxY);

    switch (dir)
    {
    case Direction::LEFT:  return atLeft;
    case Direction::RIGHT: return atRight;
    case Direction::UP:    return atTop;
    case Direction::DOWN:  return atBottom;
    default: return false;
    }
}

// C2S_MOVE_START 좌표 수용의 이동량 예산 — 경과분을 적립하고, 통과하면 소비까지 수행.
//   적립: 마지막 검사 이후 흐른 프레임 × 자기 속도 × 여유계수. 상한이 1회 수용 한도(8타일)와 같아
//         정상 클라는 예산이 늘 만충이다(서버가 이동을 직접 계산하므로 예측 오차 소모가 거의 없음).
//   소비: 실제 수용한 거리만큼. 연타해도 장기 평균은 적립 속도(자기 속도 × 여유계수)로 수렴한다.
bool CGameServer::ConsumeMoveBudget(CPlayer* player, float distSq)
{
    const uint64_t elapsedFrames = _frameCount - player->_moveBudgetFrame;
    if (elapsedFrames > 0)
    {
        player->_moveBudgetFrame = _frameCount;
        const float elapsedSec = static_cast<float>(elapsedFrames) * (FRAME_INTERVAL_MS / 1000.0f);
        player->_moveBudget += static_cast<float>(player->_speed) * MOVE_BUDGET_SLACK * elapsedSec;
        if (player->_moveBudget > MOVE_BUDGET_CAP)
            player->_moveBudget = MOVE_BUDGET_CAP;
    }

    const float dist = std::sqrt(distSq);
    if (dist > player->_moveBudget)
    {
        ++_tickMoveBudgetRejects;
        return false;
    }

    player->_moveBudget -= dist;
    return true;
}

// 두 섹터가 서로의 시야(주변 9섹터) 안인가 — 체비쇼프 거리 ≤ 1.
//   GetAroundSectorList(SectorManager.cpp)가 dx,dy ∈ [-1,1]을 모으는 것과 같은 판정을
//   섹터 목록을 만들지 않고 좌표만으로 계산한다(맵 밖 섹터는 애초에 주민이 없어 결과 동일).
//   ※ 섹터 좌표는 존마다 별개 격자이므로, 호출부에서 반드시 같은 존인지 먼저 확인할 것.
namespace
{
    inline bool IsSectorAdjacent(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
    {
        int32_t dx = x1 - x2;
        int32_t dy = y1 - y2;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        return dx <= 1 && dy <= 1;
    }
}

#if USE_DROP
// 같은 틱에 섹터를 넘은 사람에게 땅의 아이템 DELETE — 헤더 주석. 여기(IsSectorAdjacent 뒤)에 정의하는 이유는 그 도우미가 이 위에 있어서다.
//   판정은 BroadcastLeaveZone 셋째 덩이와 같고, 아이템은 안 움직이니 "퇴장자의 옛 섹터 = 지금 섹터"인 경우다.
void CGameServer::SendGroundItemDeleteToLateMovers(CZone* zone, int32_t sectorX, int32_t sectorY,
                                                   int32_t groundId, GroundItemGone wire, int32_t pickerId)
{
    for (const auto& change : _pendingSectorChanges)
    {
        CPlayer* mover = AsPlayer(change.character);   // 대기열은 섞여 있다(몬스터도 섹터를 넘는다) — 받는 쪽만
        if (mover == nullptr || mover->_zoneId != zone->GetZoneId())
            continue;   // 대기열은 서버 전역 — 섹터 좌표는 존마다 별개 격자라 같은 존만 비교
        if (change.oldSectorX == mover->_sectorX && change.oldSectorY == mover->_sectorY)
            continue;   // 원위치 복귀 — 화면과 등록이 같다

        // 옛 섹터로는 보였고(화면에 있다) 새 섹터로는 안 보인다(방금 보낸 주변 DELETE 에서 빠졌다)
        if (IsSectorAdjacent(change.oldSectorX, change.oldSectorY, sectorX, sectorY) &&
            !IsSectorAdjacent(mover->_sectorX, mover->_sectorY, sectorX, sectorY))
        {
            SendGroundItemDelete(mover, groundId, wire, pickerId);
            _gameMonitor._drop._lateMoverDeletes.Inc();
        }
    }
}
#endif

// ==========================================================================
// 존 입장/퇴장 브로드캐스트
// ==========================================================================

void CGameServer::BroadcastEnterZone(CZone* zone, CPlayer* player, SpawnReason reason)
{
    const CSectorManager& sectors = zone->GetSectorManager();

    // 주변 플레이어 → 나를 본다 (아웃바운드) — 받는 쪽은 플레이어 목록뿐이다
    _eventAroundBuffer.clear();
    sectors.GetAroundPlayers(player->_sectorX, player->_sectorY, _eventAroundBuffer, player);
    for (CPlayer* other : _eventAroundBuffer)
        SendCreateOtherPlayer(other, player, reason);   // 기존 플레이어에게 신규 플레이어 생성

    // 나 → 주변 전부(플레이어·몬스터)를 본다 (인바운드) — 섹터 변경의 인바운드와 같은 도우미로 두 목록을 돈다
    CSectorManager::SectorPos around[CSectorManager::MAX_AROUND_SECTORS];
    int32_t aroundCount = 0;
    sectors.GetAroundSectorList(player->_sectorX, player->_sectorY, around, aroundCount);
    for (int32_t i = 0; i < aroundCount; ++i)
    {
        sectors.ForEachInSector(around[i].x, around[i].y, [&](CCharacter* other)
        {
            if (other != player)
                SendCreateOtherPlayer(player, other);   // 신규 플레이어에게 기존 상대 생성
        });
#if USE_DROP
        // 땅의 아이템도 보인다 — 캐릭터가 아니라 셋째 목록을 따로 돈다(입장·존 이동 공통)
        for (const CGroundItem& g : sectors.GetSectorItems(around[i].x, around[i].y))
            SendGroundItemCreate(player, g, GroundItemAppear::SEEN);
#endif
    }
}

// 세션 없는 것의 등장 — 주변 플레이어에게 알리기만 한다. 받는 쪽이 없으니 "나에게 주변을 생성"은 없다.
void CGameServer::BroadcastSpawn(CZone* zone, CCharacter* who, SpawnReason reason)
{
    _eventAroundBuffer.clear();
    zone->GetSectorManager().GetAroundPlayers(who->_sectorX, who->_sectorY, _eventAroundBuffer, who);

    for (CPlayer* other : _eventAroundBuffer)
        SendCreateOtherPlayer(other, who, reason);
}

void CGameServer::BroadcastLeaveZone(CZone* zone, CCharacter* player)
{
    // 현재 섹터 기준 주변 DELETE — 받는 쪽은 플레이어 목록뿐이다
    _eventAroundBuffer.clear();
    zone->GetSectorManager().GetAroundPlayers(
        player->_sectorX, player->_sectorY, _eventAroundBuffer, player);

    for (CPlayer* other : _eventAroundBuffer)
        SendDeletePlayer(other, player);

    // 미처리 섹터 변경이 있으면 이전 섹터 전용 뷰어에게도 DELETE
    // (같은 프레임에 섹터 변경 + 존 이동/접속해제 시 고스트 방지)
    if (_sectorChangedSet.count(player))
    {
        for (const auto& change : _pendingSectorChanges)
        {
            if (change.character != player)
                continue;

            // 이전 섹터 전용 = old 주변에만 있고 현재 주변에는 없는 섹터
            CSectorManager::SectorPos added[CSectorManager::MAX_AROUND_SECTORS];
            CSectorManager::SectorPos removed[CSectorManager::MAX_AROUND_SECTORS];
            int32_t addedCount = 0;
            int32_t removedCount = 0;

            zone->GetSectorManager().GetSectorDiff(
                player->_sectorX, player->_sectorY,    // 현재 섹터 (이미 갱신된 값)
                change.oldSectorX, change.oldSectorY,   // 이전 섹터
                added, addedCount,                       // old 전용 섹터
                removed, removedCount);

            for (int32_t i = 0; i < addedCount; ++i)
            {
                for (CPlayer* other : zone->GetSectorManager().GetSectorPlayers(added[i].x, added[i].y))
                    SendDeletePlayer(other, player);
            }
            break;
        }
    }

#if USE_SECTOR_AGGREGATION
    // 섹터 묶음 dirty에서도 제거 (틱 끝 FlushSectorUpdates dangling 방지 — 같은 틱 move→퇴장/존이동)
    if (player->_moveDirty)
    {
        player->_moveDirty = false;
        _sectorOutbox.dirtyMovers.erase(std::remove(_sectorOutbox.dirtyMovers.begin(), _sectorOutbox.dirtyMovers.end(), player),
            _sectorOutbox.dirtyMovers.end());
    }
#endif

    // 같은 틱에 먼저 이동해 위 두 조회에서 모두 빠진 이동자에게도 DELETE (치유 불가 유령 방지)
    //   위 조회는 (1) 퇴장자의 현재 섹터 주변, (2) 퇴장자의 이전 섹터 전용 뷰어 — 둘 다 이동자의
    //   "이동 후" 위치로만 걸린다. 퇴장 전엔 퇴장자를 보고 있었지만 같은 틱에 서로 멀어진 이동자는
    //   어느 쪽에도 안 걸리고, 퇴장자는 곧 사라지므로 나중에 치유될 통로조차 없다.
    {
        int32_t leaverOldX = player->_sectorX;
        int32_t leaverOldY = player->_sectorY;
        if (_sectorChangedSet.count(player))
        {
            for (const auto& change : _pendingSectorChanges)
            {
                if (change.character == player)
                {
                    leaverOldX = change.oldSectorX;
                    leaverOldY = change.oldSectorY;
                    break;
                }
            }
        }

        for (const auto& change : _pendingSectorChanges)
        {
            CCharacter* mover = change.character;
            if (mover == player || mover->_zoneId != player->_zoneId)
                continue;   // 대기열은 서버 전역 — 섹터 좌표는 존마다 별개 격자라 같은 존만 비교
            if (change.oldSectorX == mover->_sectorX && change.oldSectorY == mover->_sectorY)
                continue;   // 원위치 복귀 — 시야 변화 없음

            // 이동 전엔 퇴장자를 보고 있었는데(클라가 들고 있음), 위 두 조회 어디에도 안 걸리는 경우만
            if (IsSectorAdjacent(change.oldSectorX, change.oldSectorY, leaverOldX, leaverOldY) &&
                !IsSectorAdjacent(mover->_sectorX, mover->_sectorY, player->_sectorX, player->_sectorY) &&
                !IsSectorAdjacent(mover->_sectorX, mover->_sectorY, leaverOldX, leaverOldY))
            {
                if (CPlayer* m = AsPlayer(mover))
                    SendDeletePlayer(m, player);
                ++_tickPairFixes;
            }
        }
    }

    // 섹터 변경 대기열에서 제거
    _sectorChangedSet.erase(player);
    _pendingSectorChanges.erase(
        std::remove_if(_pendingSectorChanges.begin(), _pendingSectorChanges.end(),
            [player](const SectorChangeInfo& c) { return c.character == player; }),
        _pendingSectorChanges.end());

#if USE_COMBAT
    // 부활 대기열에서도 제거 — 위 대기열들과 같은 이유다(틱 끝 TickRespawn dangling 방지).
    //   죽은 채로 퇴장·존이동하는 경우가 실제 경로다(부활 3초 안에 창을 닫으면 여기로 온다).
    RemoveFromDeadActors(player);
#endif

    zone->LeaveZone(player);
}

// ==========================================================================
// 섹터 변경 대기열 삽입 — 같은 플레이어는 최초 출발 섹터만 기록
// ==========================================================================

void CGameServer::PushSectorChange(CCharacter* player, int32_t oldSectorX, int32_t oldSectorY)
{
    if (!_sectorChangedSet.insert(player).second)
        return;  // 이미 기록됨 → 최초 출발 섹터 유지

    _pendingSectorChanges.push_back({ player, oldSectorX, oldSectorY });
}

// ==========================================================================
// 같은 틱 이동자 쌍 보정
//
// [왜 필요한가]
//   섹터 점유는 이동 즉시 갱신되지만(CZone::Tick 의 섹터 변경 판정, RecvMoveStart/Stop) 통보는 틱 끝에 몰아서
//   한다. 이때 ProcessSectorChange는 이탈/진입 섹터의 "틱 끝 시점" 주민을 읽으므로, 상대도 같은
//   틱에 움직였다면 그 조회에 안 걸린다.
//     · 서로 멀어짐 → 상호 DELETE 누락 → 클라에 유령(멈춘 캐릭터)이 남는다
//     · 서로 같은 섹터로 수렴 → 상호 CREATE 누락 → 바로 옆인데 서로 안 보인다
//   4방향으로 한 칸씩 걷다 같은 틱에 섹터를 넘기만 해도 발생한다. 클라에는 타임아웃 제거도
//   전체 재동기화도 없어 스스로 복구되지 않는다.
//
// [무엇을 하는가]
//   A 패스는 B의 "현재" 섹터로, B 패스는 A의 "현재" 섹터로 판정하므로
//     A 패스 발동 = 인접(A이전, B현재),  B 패스 발동 = 인접(A현재, B이전)
//   이다. 정말 필요한 것은 인접(A이전,B이전) → 인접(A현재,B현재)의 변화이므로,
//   "시야가 바뀌었는데 두 패스 다 발동하지 않는" 쌍만 골라 보충한다. 기존 송신 경로는 그대로
//   두므로 중복은 생기지 않는다(보내던 것을 다시 보내지 않음).
//
// [비용] 이동자 수 M에 대해 O(M²) 정수 비교. 실측 1,000명 M≈62 → 10us/틱(프레임의 0.02%),
//   5,000명 M≈310 → 532us/틱(1.3%). 유의미해지면 이전/현재 섹터로 버킷팅해 후보를 줄일 것.
// ==========================================================================

void CGameServer::FixSameTickMoverPairs()
{
    const size_t count = _pendingSectorChanges.size();
    if (count < 2)
        return;

    for (size_t i = 0; i < count; ++i)
    {
        CCharacter* a = _pendingSectorChanges[i].character;
        const int32_t aOldX = _pendingSectorChanges[i].oldSectorX;
        const int32_t aOldY = _pendingSectorChanges[i].oldSectorY;
        if (aOldX == a->_sectorX && aOldY == a->_sectorY)
            continue;   // 원위치 복귀 — 틱 끝 루프의 skip 조건과 동일

        for (size_t j = i + 1; j < count; ++j)
        {
            CCharacter* b = _pendingSectorChanges[j].character;
            if (a->_zoneId != b->_zoneId)
                continue;   // 대기열은 서버 전역 — 섹터 좌표는 존마다 별개 격자라 같은 존만 비교

            const int32_t bOldX = _pendingSectorChanges[j].oldSectorX;
            const int32_t bOldY = _pendingSectorChanges[j].oldSectorY;
            if (bOldX == b->_sectorX && bOldY == b->_sectorY)
                continue;

            const bool wasVisible = IsSectorAdjacent(aOldX, aOldY, bOldX, bOldY);
            const bool isVisible  = IsSectorAdjacent(a->_sectorX, a->_sectorY, b->_sectorX, b->_sectorY);
            if (wasVisible == isVisible)
                continue;   // 시야 변화 없음 → 통보 자체가 불필요

            const bool aPassHits = IsSectorAdjacent(aOldX, aOldY, b->_sectorX, b->_sectorY);
            const bool bPassHits = IsSectorAdjacent(a->_sectorX, a->_sectorY, bOldX, bOldY);

            if (!isVisible)
            {
                // 보였다가 안 보임 — 두 패스 다 못 잡았을 때만 보충
                if (!aPassHits && !bPassHits)
                {
                    if (CPlayer* pa = AsPlayer(a))
                        SendDeletePlayer(pa, b);
                    if (CPlayer* pb = AsPlayer(b))
                        SendDeletePlayer(pb, a);
                    ++_tickPairFixes;
                }
            }
            else
            {
                // 안 보이다가 보임 — 두 패스 다 못 잡았을 때만 보충
                if (aPassHits && bPassHits)
                {
                    if (CPlayer* pa = AsPlayer(a))
                        SendCreateOtherPlayer(pa, b);
                    if (CPlayer* pb = AsPlayer(b))
                        SendCreateOtherPlayer(pb, a);
                    ++_tickPairFixes;
                }
            }
        }
    }
}

// ==========================================================================
// 섹터 변경 브로드캐스트
// ==========================================================================

void CGameServer::ProcessSectorChange(CZone* zone, CCharacter* player,
                                      int32_t oldSectorX, int32_t oldSectorY)
{
    // 인바운드(상대→나)는 내가 플레이어일 때만 있다 — 몬스터·NPC 는 받는 쪽이 못 되므로
    //   수집·직렬화 자체를 건너뛴다. 아웃바운드(나→상대)는 받는 쪽이 남이라 종류와 무관하게 그대로 간다.
    CPlayer* const me = AsPlayer(player);

    CSectorManager::SectorPos added[CSectorManager::MAX_AROUND_SECTORS];
    CSectorManager::SectorPos removed[CSectorManager::MAX_AROUND_SECTORS];
    int32_t addedCount = 0;
    int32_t removedCount = 0;
    zone->GetSectorManager().GetSectorDiff(
        oldSectorX, oldSectorY,
        player->_sectorX, player->_sectorY,
        added, addedCount, removed, removedCount);

#if USE_MEMBERSHIP_FANOUT_DEDUP
    // [Phase 1] 아웃바운드(나→상대, 수신자마다 내용 동일)는 1회 빌드 후 팬아웃,
    //           인바운드(상대→나, 수신자마다 내용 제각각)는 개별 송신 유지.
    //           수신자별 순서·수신자·횟수는 OFF와 동일 — 재빌드(Alloc+직렬화) 반복만 제거.

    // 이탈 섹터 — 상호 DELETE
    if (removedCount > 0)
    {
#if USE_MEMBERSHIP_DIGEST
        // [Phase 4] 아웃바운드: "나를 삭제" 1개 빌드 → 이탈 섹터들에 직송 등록 (주민 열람 없음, 배포는 틱 끝 digest)
        //           player는 이탈 섹터에 없어(현재 섹터는 새 AOI 소속) exclude 불필요 = 폴백 경로 없음
        RegisterOutboundToSectors(zone, player->_zoneId, removed, removedCount,
                                  MakeDeletePlayer(player->_actorId), nullptr);
#else
        // 아웃바운드: "나를 삭제" 1개 빌드 → 이탈 섹터 전원에게 팬아웃 (player는 이탈 섹터에 없어 exclude 불필요)
        FanoutToSectors(zone, removed, removedCount, MakeDeletePlayer(player->_actorId), nullptr);
#endif

        // 아래는 전부 "나에게" 보내는 것이다 — 내가 플레이어가 아니면 통째로 건너뛴다
        if (me != nullptr)
        {
#if USE_MEMBERSHIP_INBOUND_BUNDLE
            // [Phase 2] 인바운드: 수신자가 mover 1명 → 상대들을 배치 패킷(상한 초과 시 청크 분할)으로 접음.
            //           엔트리 순서 = OFF의 개별 송신 순서(섹터 순회순) 그대로.
            _membershipInboundBuffer.clear();
            for (int32_t i = 0; i < removedCount; ++i)
            {
                zone->GetSectorManager().ForEachInSector(removed[i].x, removed[i].y,
                    [&](CCharacter* other) { _membershipInboundBuffer.push_back(other); });   // 보이는 것 전부(플레이어·몬스터)
            }
            SendDeletePlayerBatch(me, _membershipInboundBuffer.data(),
                                  static_cast<int>(_membershipInboundBuffer.size()));
#else
            // 인바운드: "상대를 삭제"는 상대마다 내용이 달라 개별 송신
            for (int32_t i = 0; i < removedCount; ++i)
            {
                zone->GetSectorManager().ForEachInSector(removed[i].x, removed[i].y,
                    [&](CCharacter* other) { SendDeletePlayer(me, other); });   // 나에게 상대를 삭제 — 보이는 것 전부
            }
#endif
        }
    }

    // 진입 섹터 — 상호 CREATE
    if (addedCount > 0)
    {
#if USE_MEMBERSHIP_DIGEST
        // [Phase 4] 아웃바운드: "나를 생성" 1개 빌드 → 진입 섹터들에 직송 등록.
        //           1칸 이동은 진입 섹터에 자신이 없고, 멀티섹터 점프로 자기 현재 섹터가 끼면 그 섹터만 개별 폴백
        //   ★ 이 경로는 SendCreateOtherPlayer 를 안 거치고 빌더를 직접 쓴다. 예전엔 체력(STATS)을 여기서 따로 실어야 했고,
        //     빠뜨렸더니 "내 HP 막대는 뜨는데 남의 것만 안 뜨는" 증상이 실제로 났다(라이브 캡처로 잡음).
        //     지금은 빌더가 체력을 싣는다(ActorSnapshot) — 여기서 따로 붙일 것이 없다.
        RegisterOutboundToSectors(zone, player->_zoneId, added, addedCount,
                                  MakeCreateOtherPlayer(player, SpawnReason::NORMAL), player);
#else
        // 아웃바운드: "나를 생성" 1개 빌드 → 진입 섹터 전원에게 팬아웃 (자기 자신 제외)
        FanoutToSectors(zone, added, addedCount, MakeCreateOtherPlayer(player, SpawnReason::NORMAL), player);
#endif

        // 아래는 전부 "나에게" 보내는 것이다 — 내가 플레이어가 아니면 통째로 건너뛴다
        if (me != nullptr)
        {
#if USE_MEMBERSHIP_INBOUND_BUNDLE
            // [Phase 2] 인바운드: 상대들을 배치 패킷(상한 초과 시 청크 분할)으로 — 자기 자신 제외는 OFF와 동일.
            _membershipInboundBuffer.clear();
            for (int32_t i = 0; i < addedCount; ++i)
            {
                zone->GetSectorManager().ForEachInSector(added[i].x, added[i].y, [&](CCharacter* other)
                {
                    if (other != player)   // 멀티섹터 점프 시 자기 자신 방지
                        _membershipInboundBuffer.push_back(other);   // 보이는 것 전부(플레이어·몬스터)
                });
            }
            SendCreatePlayerBatch(me, _membershipInboundBuffer.data(),
                                  static_cast<int>(_membershipInboundBuffer.size()));
#else
            // 인바운드: "상대를 생성"은 상대마다 내용이 달라 개별 송신
            for (int32_t i = 0; i < addedCount; ++i)
            {
                zone->GetSectorManager().ForEachInSector(added[i].x, added[i].y, [&](CCharacter* other)
                {
                    if (other != player)   // 멀티섹터 점프 시 자기 자신 방지
                        SendCreateOtherPlayer(me, other);   // 나에게 상대를 생성 — 보이는 것 전부
                });
            }
#endif
        }
    }
#else
    const CSectorManager& sectors = zone->GetSectorManager();

    // 이탈 섹터 — 상호 DELETE
    for (int32_t i = 0; i < removedCount; ++i)
    {
        for (CPlayer* other : sectors.GetSectorPlayers(removed[i].x, removed[i].y))
            SendDeletePlayer(other, player);   // 상대에게 나를 삭제 — 아웃바운드(받는 쪽은 플레이어뿐)
        if (me != nullptr)
            sectors.ForEachInSector(removed[i].x, removed[i].y,
                [&](CCharacter* other) { SendDeletePlayer(me, other); });   // 나에게 상대를 삭제 — 인바운드(보이는 것 전부)
    }

    // 진입 섹터 — 상호 CREATE
    for (int32_t i = 0; i < addedCount; ++i)
    {
        for (CPlayer* other : sectors.GetSectorPlayers(added[i].x, added[i].y))
        {
            if (other != player)   // 멀티섹터 점프 시 자기 자신 방지
                SendCreateOtherPlayer(other, player);   // 상대에게 나를 생성 — 아웃바운드
        }
        if (me != nullptr)
            sectors.ForEachInSector(added[i].x, added[i].y, [&](CCharacter* other)
            {
                if (other != player)
                    SendCreateOtherPlayer(me, other);   // 나에게 상대를 생성 — 인바운드
            });
    }
#endif

#if USE_DROP
    // ── 땅의 아이템 — 인바운드만 있다 ──
    //   아이템은 움직이지 않으니 "남이 나를 본다(아웃바운드)"가 없고, 받는 쪽은 늘 나 하나다.
    //   그래서 위 묶음·digest 토글 덩이들과 상관없이 여기 한 번만 둔다(꺼진 경로를 안 건드린다).
    //   같은 틱에 섹터를 넘으며 방금 떨어진 것을 두 번 받거나, 모르는 것의 삭제를 받을 수 있다 —
    //   클라는 땅 번호로 덮어쓰고 모르는 번호의 삭제는 버린다(Protocol.h MSG_S2C_GROUND_ITEM_CREATE).
    if (me != nullptr)
    {
        const CSectorManager& itemSectors = zone->GetSectorManager();
        for (int32_t i = 0; i < removedCount; ++i)
        {
            for (const CGroundItem& g : itemSectors.GetSectorItems(removed[i].x, removed[i].y))
                SendGroundItemDelete(me, g.groundId, GroundItemGone::OUT_OF_SIGHT, 0);
        }
        for (int32_t i = 0; i < addedCount; ++i)
        {
            for (const CGroundItem& g : itemSectors.GetSectorItems(added[i].x, added[i].y))
                SendGroundItemCreate(me, g, GroundItemAppear::SEEN);
        }
    }
#endif
}

#if USE_MEMBERSHIP_FANOUT_DEDUP
// [Phase 1] 미리 빌드한 버퍼 1개를 여러 섹터의 플레이어에게 팬아웃 (아웃바운드 멤버십 전용).
// BroadcastSectorPacket과 동일한 2패스 배치 AddRef 패턴: 유효 타겟 선카운트 → AddRef(N) → 타겟별 송신 → 최종 SubRef.
// 두 패스 사이 세션 변동 없음(단일 게임루프 스레드) → 카운트 정합 보장. _membershipSends는 OFF와 동일하게 타겟당 1 집계.
void CGameServer::FanoutToSectors(CZone* zone,
                                  const CSectorManager::SectorPos* sectors, int32_t sectorCount,
                                  CSerialBuffer* pMsg, CCharacter* exclude)
{
    // 1패스: 유효 타겟(exclude 제외) 선카운트 — 목록이 플레이어뿐이다
    size_t validCount = 0;
    for (int32_t i = 0; i < sectorCount; ++i)
    {
        for (CPlayer* other : zone->GetSectorManager().GetSectorPlayers(sectors[i].x, sectors[i].y))
        {
            if (other != exclude)
                ++validCount;
        }
    }

    if (validCount > 0)
        pMsg->AddRef(static_cast<int64_t>(validCount));   // 타겟별 소유권 일괄 확보 (원자연산 N→1)

    // 2패스: 팬아웃 — RequestSendMsg가 타겟별 소유권 1 소비 (성공/실패 모든 경로에서 SubRef)
    const int dataSize = pMsg->GetDataSize();
    size_t sentPkts = 0;
    for (int32_t i = 0; i < sectorCount; ++i)
    {
        for (CPlayer* other : zone->GetSectorManager().GetSectorPlayers(sectors[i].x, sectors[i].y))
        {
            if (other == exclude)
                continue;
            ++_tickMembershipSends;   // 멤버십 복사량(횟수) — OFF의 SendXxx 호출당 1과 동일 집계 (플레이어만 — 뱀까지 세던 옛 값과 단위가 다르다)
            if (_network->RequestSendMsg(other->_sessionId, pMsg, SendFlush::Deferred))
                ++sentPkts;
        }
    }

    if (sentPkts > 0)
    {
        _monitor._sendPackets.Add(static_cast<int64_t>(sentPkts));
        _monitor._sendEnqueuedBytes.Add(static_cast<int64_t>(sentPkts) * dataSize);
    }

    pMsg->SubRef();   // 빌더가 넘긴 소유권 1 회수 (타겟 0명이어도 안전)
}

#if USE_MEMBERSHIP_DIGEST
// [Phase 4] 아웃바운드 멤버십을 직송 보류로 등록 — 섹터당 등록 1건(주민 열람 없음), 배포·회수는 틱 끝 digest.
// 소유권: 소비자(직송 등록·폴백 팬아웃)마다 AddRef로 +1씩 공급하고, 끝에서 빌더 몫 1을 회수.
// exclude 처리: 직송은 공유 버퍼라 수신자별 제외가 불가 → exclude가 그 섹터의 주민인 경우
// (멀티섹터 점프로 진입 목록에 자기 현재 섹터가 포함될 때뿐 — 1칸 이동은 진입 섹터에 자신이 없음)만
// 그 1개 섹터를 기존 FanoutToSectors(개별 송신, exclude 지원)로 폴백해 와이어 내용을 보존한다.
void CGameServer::RegisterOutboundToSectors(CZone* zone, int32_t zoneId,
                                            const CSectorManager::SectorPos* sectors, int32_t sectorCount,
                                            CSerialBuffer* pMsg, CCharacter* exclude)
{
    for (int32_t i = 0; i < sectorCount; ++i)
    {
        if (exclude != nullptr &&
            sectors[i].x == exclude->_sectorX && sectors[i].y == exclude->_sectorY)
        {
            // 점프 폴백 — FanoutToSectors가 빌더 몫 1을 소비하므로 AddRef로 별도 소유권 공급
            pMsg->AddRef();
            FanoutToSectors(zone, &sectors[i], 1, pMsg, exclude);
            continue;
        }
        pMsg->AddRef();   // 직송 등록 1건당 소유권 1 — FlushSectorSends 4)에서 회수
        RegisterSectorDirectItem(zone, zoneId, sectors[i].x, sectors[i].y, pMsg);
    }
    pMsg->SubRef();   // 빌더가 넘긴 소유권 1 회수 (등록 0건이어도 안전)
}
#endif // USE_MEMBERSHIP_DIGEST
#endif // USE_MEMBERSHIP_FANOUT_DEDUP

#if USE_SECTOR_AGGREGATION
// ==========================================================================
// 섹터 묶음 (USE_SECTOR_AGGREGATION)
// ==========================================================================

// 즉시 브로드캐스트 대신 dirty 등록 — 같은 플레이어는 틱당 1회만.
// 묶음은 틱 끝 최종 상태를 읽으므로 여기선 등록만 (상태는 호출 직전에 이미 갱신됨).
// → 배출: 틱 끝 FlushSectorUpdates(BUNDLE off) / FlushSectorSends(BUNDLE on)에서 _sectorOutbox.dirtyMovers 소비.
void CGameServer::MarkMoveDirty(CCharacter* player)
{
    if (player->_moveDirty)
        return;
    player->_moveDirty = true;
    _sectorOutbox.dirtyMovers.push_back(player);
}

// 틱 끝: dirty 플레이어를 (zone, sector)별로 묶어 그 섹터 주변에 송신.
// ← 생산: MarkMoveDirty (틱 중 _sectorOutbox.dirtyMovers 적재).
void CGameServer::FlushSectorUpdates()
{
    if (_sectorOutbox.dirtyMovers.empty())
        return;

    // [계측] 묶음 빌드+송신 전체를 enqueue 축에 합산 (baseline의 broadcast_enqueue와 같은 비용종류로 A/B 비교)
    auto measT0 = std::chrono::steady_clock::now();

    // (zoneId, sectorY, sectorX)로 정렬 → 같은 섹터가 연속 구간이 되어 그룹 단위로 처리
    std::sort(_sectorOutbox.dirtyMovers.begin(), _sectorOutbox.dirtyMovers.end(),
        [](CCharacter* a, CCharacter* b)
        {
            if (a->_zoneId != b->_zoneId)   return a->_zoneId < b->_zoneId;
            if (a->_sectorY != b->_sectorY) return a->_sectorY < b->_sectorY;
            return a->_sectorX < b->_sectorX;
        });

    const size_t total = _sectorOutbox.dirtyMovers.size();
    size_t i = 0;
    while (i < total)
    {
        CCharacter* head = _sectorOutbox.dirtyMovers[i];
        const int32_t zoneId = head->_zoneId;
        const int32_t sx = head->_sectorX;
        const int32_t sy = head->_sectorY;

        // head와 같은 섹터가 이어지는 끝(j)을 찾는다 → [i, j)가 한 섹터 묶음
        size_t j = i;
        while (j < total &&  // 배열 끝을 넘지 않는 동안
            _sectorOutbox.dirtyMovers[j]->_zoneId == zoneId &&  // j번째가 기준과 같은 zone이고
            _sectorOutbox.dirtyMovers[j]->_sectorX == sx &&     // 같은 sectorX이고
            _sectorOutbox.dirtyMovers[j]->_sectorY == sy)       // 같은 sectorY이면
            ++j;

        // [i, j) 묶음을 SECTOR_UPDATE_MAX_ENTRIES씩 잘라 패킷화 → 섹터(sx,sy) 주변에 broadcast
        CZone* zone = _mapManager.GetZone(zoneId);
        if (zone != nullptr)
        {
            // 한 섹터를 SECTOR_UPDATE_MAX_ENTRIES씩 끊어 송신 (버퍼 한계 가드 — 균등 부하엔 1청크)
            size_t k = i;
            while (k < j)
            {
                int chunk = static_cast<int>((std::min)(j - k, static_cast<size_t>(SECTOR_UPDATE_MAX_ENTRIES)));
                CSerialBuffer* buf = MakeSectorUpdates(&_sectorOutbox.dirtyMovers[k], chunk);
                BroadcastSectorPacket(zone, sx, sy, buf);
                k += chunk;
            }
        }
        i = j;
    }

    // dirty 리셋 (다음 틱 이월 없음)
    for (CCharacter* p : _sectorOutbox.dirtyMovers)
        p->_moveDirty = false;
    _sectorOutbox.dirtyMovers.clear();

    auto measT1 = std::chrono::steady_clock::now();
    _tickBroadcastEnqueueUs += std::chrono::duration_cast<std::chrono::microseconds>(measT1 - measT0).count();
}

// 섹터 좌표 기준 주변 9섹터에 묶음 패킷 전송.
// BroadcastAroundSector의 소유권(배치 AddRef)·송신 메트릭 패턴을 그대로 복제 (본인 포함).
void CGameServer::BroadcastSectorPacket(CZone* zone, int32_t sectorX, int32_t sectorY, CSerialBuffer* pMsg)
{
    _broadcastBuffer.clear();
    zone->GetSectorManager().GetAroundPlayers(sectorX, sectorY, _broadcastBuffer, nullptr);

    _monitor._gameLoop._broadcastCalls.Inc();
    _monitor._gameLoop._broadcastTargets.Add(static_cast<int64_t>(_broadcastBuffer.size()));   // 플레이어만 센다

    // 배치 AddRef (원자연산 N→1) — 목록이 플레이어뿐이라 선카운트가 곧 크기다
    const size_t validCount = _broadcastBuffer.size();
    if (validCount > 0)
        pMsg->AddRef(static_cast<int64_t>(validCount));

    const int dataSize = pMsg->GetDataSize();
    size_t sentPkts = 0;
    for (CPlayer* other : _broadcastBuffer)
    {
        if (_network->RequestSendMsg(other->_sessionId, pMsg, SendFlush::Deferred))
            ++sentPkts;
    }
    if (sentPkts > 0)
    {
        _monitor._sendPackets.Add(static_cast<int64_t>(sentPkts));
        _monitor._sendEnqueuedBytes.Add(static_cast<int64_t>(sentPkts) * dataSize);
    }
    pMsg->SubRef();   // 빌더가 넘긴 소유권 1 회수 (타겟 0명이어도 안전 회수)
}

#if USE_BROADCAST_BUNDLE
// ==========================================================================
// 수신섹터 digest (USE_BROADCAST_BUNDLE, Phase 3)
//
// 기존(OFF): 소스 아이템(번들 청크·채팅)마다 주변 3×3 주민에게 개별 RequestSendMsg
//            → (아이템 × 수신자)회의 세션 핀(원자연산)+링 뮤텍스+복사 반복이 broadcast_copy의 지배분.
// digest(ON): 같은 섹터 주민은 수신 집합이 완전히 동일(본인 포함 정책)하다는 성질을 이용,
//            수신 섹터별로 이웃 9섹터 보류물을 raw 바이트 1덩어리로 연접 → 주민당 1회 RequestSendRaw.
//            와이어 바이트·논리 패킷 수·순서 의미 불변 (coalescing이 이미 틱 끝 연접 송신이라 클라 무변경).
// ==========================================================================

// 존 최초 등록 — 그리드 크기 확정 (맵 크기는 부팅 후 불변). 방송(items)/직송(directItems)
// 어느 쪽이 먼저 등록해도 1회만 초기화 (countX==0 센티널 — items.empty()는 직송 선행 시 오판).
void CGameServer::EnsureZonePending(CZone* zone, SectorOutbox::ZonePending& zp)
{
    if (zp.countX != 0)
        return;
    zp.countX = zone->GetSectorManager().GetSectorCountX();
    zp.countY = zone->GetSectorManager().GetSectorCountY();
    const size_t total = static_cast<size_t>(zp.countX) * zp.countY;
    zp.items.resize(total);
    zp.recvEpoch.assign(total, 0);
#if USE_MEMBERSHIP_DIGEST
    zp.directItems.resize(total);
#endif
}

// 보류 등록 — (zoneId, 섹터)에 버퍼 적재. 버퍼 소유권 1은 FlushSectorSends 4)에서 회수.
// _broadcastCalls는 소스 아이템 단위로 집계 (기존 per-청크/per-채팅 호출 카운트와 동일 의미 → A/B 비교 가능).
// → 배출: 틱 끝 FlushSectorSends에서 _sectorOutbox.pendingByZone을 수신섹터 digest로 연접·배포.
void CGameServer::RegisterSectorItem(CZone* zone, int32_t zoneId, int32_t sectorX, int32_t sectorY,
                                     CSerialBuffer* pMsg)
{
    auto& zp = _sectorOutbox.pendingByZone[zoneId];
    EnsureZonePending(zone, zp);
    const int32_t idx = sectorY * zp.countX + sectorX;
    if (zp.items[idx].empty())
        _sectorOutbox.touchedSectors.emplace_back(zoneId, idx);
    zp.items[idx].push_back(pMsg);
    _monitor._gameLoop._broadcastCalls.Inc();
}

#if USE_MEMBERSHIP_DIGEST
// [Phase 4] 직송 보류 등록 — (zoneId, 섹터)에 "그 섹터 주민에게만" 전달할 버퍼 적재 (멤버십 CREATE/DELETE 전용).
// 버퍼 소유권 1은 FlushSectorSends 4)에서 회수. _broadcastCalls는 안 늘린다(방송 지표 오염 방지) —
// 멤버십 횟수는 배포 시점에 _tickMembershipSends로 승계 (OFF의 타겟당 1 집계와 동일 산식).
void CGameServer::RegisterSectorDirectItem(CZone* zone, int32_t zoneId, int32_t sectorX, int32_t sectorY,
                                           CSerialBuffer* pMsg)
{
    auto& zp = _sectorOutbox.pendingByZone[zoneId];
    EnsureZonePending(zone, zp);
    const int32_t idx = sectorY * zp.countX + sectorX;
    if (zp.directItems[idx].empty())
        _sectorOutbox.touchedDirectSectors.emplace_back(zoneId, idx);
    zp.directItems[idx].push_back(pMsg);
}
#endif

// 틱 끝: ① 이동 dirty → 섹터별 번들 빌드·보류 ② 수신섹터 후보 수집 ③ 연접·배포 ④ 일괄 해제.
// ← 생산: MarkMoveDirty(_sectorOutbox.dirtyMovers) + RegisterSectorItem(_sectorOutbox.pendingByZone)
//         + RegisterSectorDirectItem(직송, USE_MEMBERSHIP_DIGEST).
void CGameServer::FlushSectorSends()
{
    if (_sectorOutbox.dirtyMovers.empty() && _sectorOutbox.touchedSectors.empty()
#if USE_MEMBERSHIP_DIGEST
        && _sectorOutbox.touchedDirectSectors.empty()
#endif
        )
        return;

    // [계측] 빌드+연접+배포 전체를 enqueue 축에 합산 — OFF의 FlushSectorUpdates·채팅 enqueue와
    //   같은 비용종류이므로 broadcast_copy_ms_per_tick으로 A/B 직접 비교 가능.
    auto measT0 = std::chrono::steady_clock::now();

    // ── 1) 이동 번들 빌드 → 보류 등록 (FlushSectorUpdates의 빌드 로직 그대로, 송신만 보류로 대체) ──
    if (!_sectorOutbox.dirtyMovers.empty())
    {
        std::sort(_sectorOutbox.dirtyMovers.begin(), _sectorOutbox.dirtyMovers.end(),
            [](CCharacter* a, CCharacter* b)
            {
                if (a->_zoneId != b->_zoneId)   return a->_zoneId < b->_zoneId;
                if (a->_sectorY != b->_sectorY) return a->_sectorY < b->_sectorY;
                return a->_sectorX < b->_sectorX;
            });

        const size_t total = _sectorOutbox.dirtyMovers.size();
        size_t i = 0;
        while (i < total)
        {
            CCharacter* head = _sectorOutbox.dirtyMovers[i];
            const int32_t zoneId = head->_zoneId;
            const int32_t sx = head->_sectorX;
            const int32_t sy = head->_sectorY;

            size_t j = i;
            while (j < total &&
                _sectorOutbox.dirtyMovers[j]->_zoneId == zoneId &&
                _sectorOutbox.dirtyMovers[j]->_sectorX == sx &&
                _sectorOutbox.dirtyMovers[j]->_sectorY == sy)
                ++j;

            CZone* zone = _mapManager.GetZone(zoneId);
            if (zone != nullptr)
            {
                size_t k = i;
                while (k < j)
                {
                    int chunk = static_cast<int>((std::min)(j - k, static_cast<size_t>(SECTOR_UPDATE_MAX_ENTRIES)));
                    RegisterSectorItem(zone, zoneId, sx, sy, MakeSectorUpdates(&_sectorOutbox.dirtyMovers[k], chunk));
                    k += chunk;
                }
            }
            i = j;
        }

        for (CCharacter* p : _sectorOutbox.dirtyMovers)
            p->_moveDirty = false;
        _sectorOutbox.dirtyMovers.clear();
    }

    // ── 2) 수신섹터 후보 = 소스(touched)의 3×3 합집합 (epoch 마킹으로 중복 제거, clear 불필요) ──
    ++_sectorOutbox.flushEpoch;
    _sectorOutbox.receiverSectors.clear();
    for (const auto& t : _sectorOutbox.touchedSectors)
    {
        auto& zp = _sectorOutbox.pendingByZone[t.first];
        const int32_t sy = t.second / zp.countX;
        const int32_t sx = t.second % zp.countX;
        const int32_t y0 = (std::max)(sy - 1, 0), y1 = (std::min)(sy + 1, zp.countY - 1);
        const int32_t x0 = (std::max)(sx - 1, 0), x1 = (std::min)(sx + 1, zp.countX - 1);
        for (int32_t ny = y0; ny <= y1; ++ny)
        {
            for (int32_t nx = x0; nx <= x1; ++nx)
            {
                const int32_t ridx = ny * zp.countX + nx;
                if (zp.recvEpoch[ridx] != _sectorOutbox.flushEpoch)
                {
                    zp.recvEpoch[ridx] = _sectorOutbox.flushEpoch;
                    _sectorOutbox.receiverSectors.emplace_back(t.first, ridx);
                }
            }
        }
    }

#if USE_MEMBERSHIP_DIGEST
    // [Phase 4] 직송 섹터 자체도 수신 후보 — removed 섹터는 mover의 새 3×3 밖이라
    // 위 방송 union에 안 잡힌다 (여기 없으면 DELETE가 통째로 증발).
    for (const auto& t : _sectorOutbox.touchedDirectSectors)
    {
        auto& zp = _sectorOutbox.pendingByZone[t.first];
        if (zp.recvEpoch[t.second] != _sectorOutbox.flushEpoch)
        {
            zp.recvEpoch[t.second] = _sectorOutbox.flushEpoch;
            _sectorOutbox.receiverSectors.emplace_back(t.first, t.second);
        }
    }
#endif

    // ── 3) 각 수신섹터: 이웃 9섹터 보류물 연접(digest) → 주민당 RequestSendRaw 1회 ──
    int64_t sentPkts = 0, sentBytes = 0, targets = 0;
    _sectorOutbox.concatBuf.reserve(16384);   // 최초 1회만 실할당 (동접 5000 기준 digest ~5KB)
    for (const auto& r : _sectorOutbox.receiverSectors)
    {
        CZone* zone = _mapManager.GetZone(r.first);
        if (zone == nullptr)
            continue;   // 틱 중 존 소멸 — 보류물 해제는 4)가 책임
        auto& zp = _sectorOutbox.pendingByZone[r.first];
        const int32_t ry = r.second / zp.countX;
        const int32_t rx = r.second % zp.countX;

        const std::vector<CPlayer*>& residents = zone->GetSectorManager().GetSectorPlayers(rx, ry);
        if (residents.empty())
            continue;   // 받을 플레이어가 없으면(뱀만 있는 섹터 포함) 연접 비용도 생략

        _sectorOutbox.concatBuf.clear();
        int32_t itemCount = 0;
#if USE_MEMBERSHIP_DIGEST
        // [Phase 4] 직송 선연접 — CREATE가 같은 digest 안의 이동 번들(방송 구간)보다 앞서도록 순서 보장
        int32_t directCount = 0;
        for (CSerialBuffer* buf : zp.directItems[r.second])
        {
            const char* p = buf->GetReadBufferPtr();
            _sectorOutbox.concatBuf.insert(_sectorOutbox.concatBuf.end(), p, p + buf->GetDataSize());
            ++directCount;
        }
        itemCount += directCount;
#endif
        const int32_t y0 = (std::max)(ry - 1, 0), y1 = (std::min)(ry + 1, zp.countY - 1);
        const int32_t x0 = (std::max)(rx - 1, 0), x1 = (std::min)(rx + 1, zp.countX - 1);
        for (int32_t ny = y0; ny <= y1; ++ny)
        {
            for (int32_t nx = x0; nx <= x1; ++nx)
            {
                for (CSerialBuffer* buf : zp.items[ny * zp.countX + nx])
                {
                    const char* p = buf->GetReadBufferPtr();
                    _sectorOutbox.concatBuf.insert(_sectorOutbox.concatBuf.end(), p, p + buf->GetDataSize());
                    ++itemCount;
                }
            }
        }
        if (itemCount == 0)
            continue;

        const int concatSize = static_cast<int>(_sectorOutbox.concatBuf.size());
        // targets = (아이템 × 주민) 전달 쌍 수 — 기존 Σ(호출별 gather 인원)과 재배열만 다르고 총량 동일. 주민 = 플레이어만
#if USE_MEMBERSHIP_DIGEST
        // [Phase 4] 직송(멤버십)은 방송 지표에서 제외 — OFF의 FanoutToSectors도 _broadcastTargets를 안 늘렸다.
        // 대신 멤버십 횟수를 OFF의 타겟당 1 집계(플레이어만, FanoutToSectors 2패스와 동일 산식)로 승계
        // → membership_sends_rate가 A/B 등가 통제지표로 유지된다.
        targets += static_cast<int64_t>(residents.size()) * (itemCount - directCount);
        _tickMembershipSends += static_cast<int64_t>(residents.size()) * directCount;
#else
        targets += static_cast<int64_t>(residents.size()) * itemCount;
#endif
        for (CPlayer* other : residents)
        {
            if (_network->RequestSendRaw(other->_sessionId, _sectorOutbox.concatBuf.data(), concatSize))
            {
                sentPkts  += itemCount;     // 논리 패킷 수 보존 (digest = 기존 패킷 itemCount개의 연접)
                sentBytes += concatSize;    // Σ아이템 크기와 동일 → avg_pkt_bytes 통제지표 불변
            }
        }
    }

    // ── 4) 보류물 일괄 해제 — 존 소멸·주민 0이어도 무조건 회수 (빌더 소유권 1을 여기서 소비) ──
    for (const auto& t : _sectorOutbox.touchedSectors)
    {
        auto& zp = _sectorOutbox.pendingByZone[t.first];
        for (CSerialBuffer* buf : zp.items[t.second])
            buf->SubRef();
        zp.items[t.second].clear();
    }
    _sectorOutbox.touchedSectors.clear();

#if USE_MEMBERSHIP_DIGEST
    // [Phase 4] 직송 보류물도 동일 규칙으로 일괄 해제 (등록 1건당 소유권 1)
    for (const auto& t : _sectorOutbox.touchedDirectSectors)
    {
        auto& zp = _sectorOutbox.pendingByZone[t.first];
        for (CSerialBuffer* buf : zp.directItems[t.second])
            buf->SubRef();
        zp.directItems[t.second].clear();
    }
    _sectorOutbox.touchedDirectSectors.clear();
#endif

    // 송신 메트릭 배치 반영 (기존 BroadcastSectorPacket의 원자연산 N→1 패턴과 동일)
    if (targets > 0)
        _monitor._gameLoop._broadcastTargets.Add(targets);
    if (sentPkts > 0)
    {
        _monitor._sendPackets.Add(sentPkts);
        _monitor._sendEnqueuedBytes.Add(sentBytes);
    }

    auto measT1 = std::chrono::steady_clock::now();
    _tickBroadcastEnqueueUs += std::chrono::duration_cast<std::chrono::microseconds>(measT1 - measT0).count();
}
#endif // USE_BROADCAST_BUNDLE
#endif // USE_SECTOR_AGGREGATION
