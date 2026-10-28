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

#if USE_COMBAT
    // 부활 — 도착 때 본인에게(배치) · 부활을 주변에 방송(전투) 둘이 부른다
    inline CSerialBuffer* MakeRespawn(int32_t targetId, float x, float y, int32_t hp)
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

    // 체력 바뀜 — 전투 · 처치 보상 · 가방(물약) · 운영 명령이 주변에 방송한다
    inline CSerialBuffer* MakeActorStats(int32_t actorId, int32_t hp, int32_t maxHp)
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
    // 파티원 정보 구조체(Protocol.h PartyPerson 10B) — p 가 nullptr 이면 0 으로 채운다(ASK 의 about 이 없을 때).
    inline void WritePartyPerson(CSerialBuffer* buf, const CPlayer* p)
    {
        *buf << static_cast<int>(p != nullptr ? p->_actorId : 0);
        *buf << static_cast<uint8_t>(p != nullptr ? p->_displayChar : 0);
        *buf << static_cast<uint8_t>(p != nullptr ? p->_colorIndex : 0);
        *buf << static_cast<int>(p != nullptr ? p->_level : 0);
    }

    // 파티원 한 줄(Protocol.h PartyMemberEntry 22B) — INFO 의 줄이자 MEMBER 의 몸통. 줄을 늘리면 여기 한 곳 · proto.js readPartyMember.
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
