#include "ChatHandler.h"

#include "SessionHub.h"
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
#include <cstring>            // strcmp — 운영 로그인 열쇠 대조
#include "CommandHandler.h"   // '/' 로 시작하는 채팅을 넘기는 곳

using namespace PacketBuild;   // 아래 조립 함수들이 머리 쓰기·닫기를 이름만으로 부른다

// 채팅·운영 S2C 조립 — 이 조각만 부른다(GameServer.cpp 에서 옮김 · 몸·주석 그대로).
//   쓰는 법(스칼라는 << · 고정배열·문자열은 SetData(raw) · pack(1))은 PacketBuild.h 머리의 [규칙] 주석과 같다.

namespace
{
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
}

// 받은 패킷 등록 — 게임 서버 등록의 채팅 · 운영자 로그인 두 줄 그대로(게임 서버 나누기 0-④ 의 등록에서 이 조각 몫 둘 · 13 단계).
bool CChatHandler::RegisterRoutes(CPacketRoutes& routes)
{
    bool ok = true;
    ok &= routes.OnPlayer(MsgType::C2S_CHAT,       sizeof(MsgHeader) + sizeof(ChatChar),   // 가변 길이: 최소 1글자
                          [this](CPlayer* p, CSerialBuffer* m) { RecvChat(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_ADMIN_LOGIN, sizeof(MSG_C2S_ADMIN_LOGIN), [this](CPlayer* p, CSerialBuffer* m) { RecvAdminLogin(p, m); });
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
        _commandHandler.Handle(player, recvMsg.message, msgLen);   // 채팅 명령 조각(CommandHandler.h)
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

// ── 운영자 인증 ──

// 부하 테스트 도구용 고정 키 — 실서비스 인증 아님.
// 와이어에 평문으로 흐르고 해시·시도 제한도 없다. admin 권한 효과는 채널 인원 제한 우회(CFieldMap::IsChannelFull)와
//   다른 맵 지정 이동(CMapManager::PickZoneChangeTarget — 일반은 포털로만) 둘, 그리고 채팅 운영 명령이다.
// 실계정 연동 단계에서 계정 DB 조회 기반 인증으로 교체한다.
static constexpr char ADMIN_KEY[] = "admin1234";

void CChatHandler::RecvAdminLogin(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_ADMIN_LOGIN recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // null-terminate 보장
    recvMsg.key[ADMIN_KEY_MAX_LEN - 1] = '\0';

    if (strcmp(recvMsg.key, ADMIN_KEY) == 0)
    {
        player->_isAdmin = true;
        _sessionHub.SendPacket(player, MakeAdminLoginOk());
    }
    else
    {
        _sessionHub.SendPacket(player, MakeAdminLoginFail());
    }
}
