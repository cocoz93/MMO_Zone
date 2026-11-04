#pragma once

// ==========================================================================
// PacketBuild — 여러 모듈이 같이 쓰는 S2C 조립 함수
//
// [책임] 패킷 머리 쓰기·닫기(BeginPacket · FinalizePacket), 여러 조립 함수가 몸통에 끼워 쓰는 부분 쓰기 함수
//        (등장 상태 · 남의 캐릭터 한 명분 · 파티 사람 · 파티원 한 줄), 둘 이상의 모듈이 부르는 완성 패킷 둘(부활 · 체력 바뀜).
//        한 클래스만 부르는 조립 함수는 여기 두지 않는다 — 그 클래스의 파일에 둔다(게임 서버 나누기 플랜, 10-18).
// [규칙] S2C 직렬화 — 모듈마다 파일 안에 둔 조립 함수도 이 규칙을 따른다.
//        와이어 = payload 영역 바이트 그대로 (WSASend가 GetReadBufferPtr()+GetDataSize()만 전송).
//        pack(1)이라 패딩이 없으므로 필드를 선언 순서/폭대로 쓰면 구조체와 바이트 동일.
//        스칼라는 <<, 고정배열/문자열은 SetData(raw) — << 문자열 연산자는 길이접두(2B)가 끼어
//        와이어가 깨지므로 사용 금지.
//        (게임 서버 나누기 14 단계에 GameServer.cpp 머리의 「S2C 직렬화 헬퍼」에서 옮김 — 그 파일에 조립 함수가 하나도 안 남아서)
// [소유권] 완성 패킷을 돌려주는 함수는 RefCount=1(소유권 1개)인 버퍼를 돌려주고, 송신 함수가 이를 소비한다
//        (송신 함수 = 세션 허브 SendPacket · CSightBroadcaster::BroadcastAroundSector 의 CSerialBuffer* 오버로드).
// ==========================================================================

#include <Identity/GameIds.h>
#include <cassert>
#include <cstdint>

#include "BuildConfig.h"   // USE_COMBAT · USE_PARTY — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include <Protocol/Protocol.h>
#include "SerialBuffer.h"
#include "Character.h"     // WriteSnapshot · WriteCreateEntry 가 캐릭터 칸을 읽는다
#include "CharacterComponents.h" // WriteSnapshot — 체력 컴포넌트(없으면 0/0)
#if USE_PARTY
#include "Player.h"        // 파티원 정보 쓰기 — 레벨(_level)은 플레이어 칸
#include "MapManager.h"    // 파티원 한 줄 — 존 번호를 맵 번호로 푼다
#endif
#if USE_EQUIP
#include "Player.h"        // 겉모습 — 끼운 무기는 플레이어 가방의 착용 칸
#include "Inventory.h"     // WornIndexOf
#include "ItemTable.h"     // MAX_ITEM_ID — 와이어 칸(u16)에 들어가나
static_assert(CItemTable::MAX_ITEM_ID <= 0xFFFF, "아이템 번호가 겉모습 칸(ActorLook::weaponItemId u16)에 안 들어간다");
#endif

// 강제 인라인 — 아래 CREATE 항목 쓰기 둘에만 쓴다. 소스는 한 벌, 기계어는 부르는 자리마다 펼친다.
//   그냥 inline 은 권고라 Release(MSVC)가 따로 떨어진 함수로 남겨, 묶음 루프가 엔트리마다 호출했다
//   (10-09 기계어 관측 — 한 명분을 합치기 전엔 묶음 장마다 1번. WriteCreateEntry 만 강제하면 이번엔 WriteSnapshot 이 남았다).
#if defined(_MSC_VER)
    #define CREATE_FORCEINLINE __forceinline
#else
    #define CREATE_FORCEINLINE inline __attribute__((always_inline))
#endif

namespace PacketBuild
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

    // 등장 상태 구조체(Protocol.h ActorSnapshot) — MakeCreateMyPlayer(맨 끝) 와 WriteCreateEntry(CREATE_OTHER·묶음 — defId 앞) 둘이 부른다.
    //   이 구조체에 필드를 늘리면 여기 한 줄 · proto.js readSnap 한 줄(Protocol.h 의 크기 단언이 알려 준다).
    //   전투가 꺼진 빌드는 0 을 싣는다 — 체력 컴포넌트는 꺼져도 있지만 쓰는 곳이 없어 의미가 없다.
    CREATE_FORCEINLINE void WriteSnapshot(CSerialBuffer* buf, const CCharacter* ch)
    {
#if USE_COMBAT
        const CHealthComponent* health = ch->Health();   // 체력 컴포넌트가 없으면 0/0 — 클라는 최대 체력 0 이면 막대를 안 그린다(Protocol.h ActorSnapshot)
        *buf << static_cast<int>(health != nullptr ? health->Hp() : 0);
        *buf << static_cast<int>(health != nullptr ? health->MaxHp() : 0);
#else
        (void)ch;
        *buf << static_cast<int>(0);
        *buf << static_cast<int>(0);
#endif
    }

    // 남의 캐릭터 한 명분(Protocol.h CreatePlayerEntry) — CREATE_OTHER 의 몸통이자 묶음 엔트리 한 칸.
    //   MakeCreateOtherPlayer 와 MakeCreatePlayerBatch 가 둘 다 이것을 부른다 — 칸을 늘리면 여기 한 줄.
    //   강제 인라인 — 위 CREATE_FORCEINLINE 주석.
    CREATE_FORCEINLINE void WriteCreateEntry(CSerialBuffer* buf, const CCharacter* ch, SpawnReason reason)
    {
        *buf << static_cast<int>(ch->_actorId);
        *buf << static_cast<uint8_t>(ch->_direction);
        *buf << static_cast<uint8_t>(ch->_moveState);
        *buf << static_cast<uint8_t>(ch->_displayChar);
        *buf << static_cast<uint8_t>(ch->_colorIndex);
        *buf << static_cast<uint8_t>(reason);
        *buf << ch->_x;
        *buf << ch->_y;
        *buf << static_cast<int>(ch->_speed);
        WriteSnapshot(buf, ch);
        *buf << static_cast<uint16_t>(ch->_defId);   // 정의 번호 — 맨 끝(Protocol.h CreatePlayerEntry)
    }

    // 겉모습(Protocol.h ActorLook) — 남의 화면에 그릴 것. 값은 쓸 때 그 자리에서 읽는다(따로 들고 있지 않는다 —
    //   끼운 칸과 두 벌이 되면 어긋난다 · 공격력도 같은 칸을 그때 읽는다 StatSystem.cpp RecalcStats). 사람만 · 몬스터·NPC 는 늘 맨몸.
    //   칸을 늘리면 LookOf · WriteLook 두 함수와 proto.js readLook(Protocol.h 크기 단언이 알려 준다).
    inline BodyLook BodyLookOf(const CharacterLookBlob::Look& body)
    {
        BodyLook out{};
        out.gender = body.gender;
        out.skin = body.skin;
        out.hair = body.hair;
        out.top = body.top;
        out.bottom = body.bottom;
        return out;
    }
    inline ActorLook LookOf(const CCharacter* ch)
    {
        ActorLook look{};
        if (const CPlayer* p = As<CPlayer>(ch))
        {
#if USE_EQUIP
            look.weaponItemId = static_cast<uint16_t>(p->_inventory.At(WornIndexOf(EquipSlot::WEAPON)).itemId);
#endif
            look.body = BodyLookOf(p->_body);
        }
        return look;
    }
    // 맨몸(모든 칸 0)이면 겉모습 패킷을 안 보낸다 — 무기를 안 든 부하 더미 5,000명의 등장 바이트가 그대로다(Protocol.h 겉모습 절)
    inline bool HasLook(const ActorLook& look) { return look.weaponItemId != 0 || look.body.gender != 0; }

    // 몸 겉모습 칸 쓰기 — ACTOR_LOOK 의 뒤쪽 · MY_LOOK 의 몸통
    inline void WriteBodyLook(CSerialBuffer* buf, const BodyLook& body)
    {
        *buf << static_cast<uint8_t>(body.gender);
        *buf << static_cast<uint8_t>(body.skin);
        *buf << static_cast<uint8_t>(body.hair);
        *buf << static_cast<uint8_t>(body.top);
        *buf << static_cast<uint8_t>(body.bottom);
        *buf << static_cast<uint8_t>(body.reserved);
    }

    // 이름 칸 쓰기 — 6칸 전부(짧은 이름은 뒤가 0). 와이어는 u16 그대로(ChatChar).
    inline void WriteNameCells(CSerialBuffer* buf, const char16_t (&name)[CHAR_NAME_MAX_LEN])
    {
        for (int i = 0; i < CHAR_NAME_MAX_LEN; ++i)
            *buf << static_cast<uint16_t>(name[i]);
    }

    // 이름이 있는 사람이면 그 사람, 아니면 nullptr — 몬스터·NPC·옛 행·더미는 이름 패킷이 안 나간다
    inline const CPlayer* NamedPlayerOf(const CCharacter* ch)
    {
        const CPlayer* p = As<CPlayer>(ch);
        return (p != nullptr && p->_name[0] != 0) ? p : nullptr;
    }

    // 겉모습 한 장 — 남이 화면에 나타날 때(CSightBroadcaster 의 보이기 함수들) · 끼우기·빼기 뒤 주변(CItemHandler) 둘이 부른다
    inline CSerialBuffer* MakeActorLook(const CCharacter* ch, const ActorLook& look)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ACTOR_LOOK::TYPE);
        *buf << static_cast<int>(ch->_actorId);
        *buf << static_cast<uint16_t>(look.weaponItemId);
        WriteBodyLook(buf, look.body);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ACTOR_LOOK));
        return buf;
    }

    // 남의 이름 한 장 — 보이기 함수들이 겉모습 바로 뒤에 넣는다(Protocol.h 이름 절)
    inline CSerialBuffer* MakeActorName(const CPlayer* p)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ACTOR_NAME::TYPE);
        *buf << static_cast<int>(p->_actorId);
        WriteNameCells(buf, p->_name);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ACTOR_NAME));
        return buf;
    }

#if USE_COMBAT
    // 부활 — 도착 때 본인에게(배치) · 부활을 주변에 방송(전투) 둘이 부른다
    inline CSerialBuffer* MakeRespawn(ActorId targetId, float x, float y, int32_t hp)
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

    // 속도 바뀜 — 본인 + 주변(BroadcastAroundSector, excludeSelf=false — Protocol.h MSG_S2C_ACTOR_SPEED). 운영 명령 /gm · 속도 버프
    inline CSerialBuffer* MakeActorSpeed(ActorId actorId, int32_t speed)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ACTOR_SPEED::TYPE);
        *buf << static_cast<int>(actorId);
        *buf << static_cast<int>(speed);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ACTOR_SPEED));
        return buf;
    }

    // 체력 바뀜 — 전투 · 처치 보상 · 가방(물약) · 운영 명령이 주변에 방송한다
    inline CSerialBuffer* MakeActorStats(ActorId actorId, int32_t hp, int32_t maxHp)
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
#endif // USE_COMBAT

#if USE_PARTY
    // 파티원 정보 구조체(Protocol.h PartyPerson 22B) — p 가 nullptr 이면 0 으로 채운다(ASK 의 about 이 없을 때).
    inline void WritePartyPerson(CSerialBuffer* buf, const CPlayer* p)
    {
        *buf << static_cast<int>(p != nullptr ? p->_actorId : NO_ACTOR);
        *buf << static_cast<uint8_t>(p != nullptr ? p->_displayChar : 0);
        *buf << static_cast<uint8_t>(p != nullptr ? p->_colorIndex : 0);
        *buf << static_cast<int>(p != nullptr ? p->_level : 0);
        for (int i = 0; i < CHAR_NAME_MAX_LEN; ++i)
            *buf << static_cast<uint16_t>(p != nullptr ? p->_name[i] : 0);
    }

    // 파티원 한 줄(Protocol.h PartyMemberEntry 34B) — INFO 의 줄이자 MEMBER 의 몸통. 줄을 늘리면 여기 한 곳 · proto.js readPartyMember.
    //   체력은 등장 상태 쓰기(WriteSnapshot)와 같은 규칙 — 전투가 꺼진 빌드는 0. 맵은 존 번호를 풀어 싣는다(채널은 안 싣는다).
    inline void WritePartyMember(CSerialBuffer* buf, const CPlayer* p)
    {
        WritePartyPerson(buf, p);
#if USE_COMBAT
        *buf << static_cast<int>(p->_health.Hp());
        *buf << static_cast<int>(p->_health.MaxHp());
#else
        *buf << static_cast<int>(0);
        *buf << static_cast<int>(0);
#endif
        *buf << static_cast<int>(CMapManager::GetMapIdFromZoneId(p->_zoneId));
    }
#endif // USE_PARTY
}
