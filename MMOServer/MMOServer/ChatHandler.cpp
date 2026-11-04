#include "ChatHandler.h"

#include "SightBroadcaster.h"
#include "MapManager.h"
#include "Zone.h"
#include "Player.h"           // CPlayer · 운영자 표시
#include "PacketRoutes.h"
#include "PacketBuild.h"      // 머리 쓰기·닫기
#include "SerialBuffer.h"
#include <Protocol/Protocol.h>
#include <cassert>            // 직렬화 크기 검증
#include <cstddef>            // offsetof — 채팅 크기 검증
#include "CommandHandler.h"   // '/' 로 시작하는 채팅을 넘기는 곳

using namespace PacketBuild;   // 아래 조립 함수들이 머리 쓰기·닫기를 이름만으로 부른다

// 채팅·운영 S2C 조립 — 이 클래스만 부른다(GameServer.cpp 에서 옮김 · 몸·주석 그대로).
//   쓰는 법(스칼라는 << · 고정배열·문자열은 SetData(raw) · pack(1))은 PacketBuild.h 머리의 [규칙] 주석과 같다.

namespace
{
    // 가변 길이 — message는 고정배열/문자열이라 SetData(raw). msgLen은 null 제외 글자 수.
    CSerialBuffer* MakeChat(ActorId playerId, uint8_t displayChar, uint8_t colorIndex,
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
}

// 받은 패킷 등록 — 게임 서버 등록의 채팅 줄 그대로 — 운영자 로그인 줄은 고정 열쇠와 함께 없앴다(11-04 · /test 는 CCommandHandler)(게임 서버 나누기 0-④ 의 등록에서 이 클래스 몫 둘 · 13 단계).
bool CChatHandler::RegisterRoutes(CPacketRoutes& routes)
{
    bool ok = true;
    ok &= routes.OnPlayer(MsgType::C2S_CHAT,       sizeof(MsgHeader) + sizeof(ChatChar),   // 가변 길이: 최소 1글자
                          [this](CPlayer* p, CSerialBuffer* m) { RecvChat(p, m); });
    return ok;
}

void CChatHandler::RecvChat(CPlayer* player, CSerialBuffer* pMsg)
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

    // '/' 로 시작하면 명령이다 — 방송하지 않는다. 운영자가 아닌 사람의 /item 이 채팅으로 새지 않게,
    //   권한 검사보다 "방송 안 함"이 먼저다. 부하 더미의 채팅 문구에는 '/' 로 시작하는 것이 없다.
    //   토글이 없다 — 명령이 하나도 없는 빌드에서도 가로챈다(명령 줄만 토글을 본다 · 게임 서버 나누기 14 단계 전에는 USE_INVENTORY 안).
    if (msgLen > 0 && recvMsg.message[0] == u'/')
    {
        _commandHandler.Handle(player, recvMsg.message, msgLen);   // CCommandHandler(CommandHandler.h)
        return;
    }

#if USE_BROADCAST_BUNDLE
    // [digest] 즉시 브로드캐스트 대신 보류 등록 — 틱 끝 digest에 합류 (실송신 시점은 기존과 동일한 틱 끝 flush).
    // 소스 섹터는 수신 시점 좌표로 고정 → 기존(즉시 배포)의 AOI 의미 보존.
    _sightBroadcaster.RegisterSectorItem(zone, player->_zoneId, player->_sectorX, player->_sectorY,
        MakeChat(player->_actorId, player->_displayChar, player->_colorIndex,
            recvMsg.message, msgLen));
#else
    // 본인 포함 브로드캐스트 (송신 함수가 소유권 소비)
    _sightBroadcaster.BroadcastAroundSector(zone, player,
        MakeChat(player->_actorId, player->_displayChar, player->_colorIndex,
            recvMsg.message, msgLen), false);
#endif
}

