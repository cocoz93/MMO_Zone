// ==========================================================================
// CChatHandler — 채팅·운영: 채팅 · 운영자 로그인 · '/' 채팅을 명령 조각으로 넘기기 (게임 서버 나누기 13 단계 · C8 · 토글 없음)
//
// [책임]
//  - 받은 패킷 둘의 처리기와 그 등록(RegisterRoutes — 채팅 · 운영자 로그인) — 컨텐츠 조각이 자기 줄을 등록한다
//  - 채팅 — 받은 크기로 글자 수를 역산해 끝을 맺고 주변에 방송(수신섹터 digest 면 틱 끝 묶음에 보류 등록). '/' 로 시작하면 방송 없이 명령 조각으로
//  - 운영자 로그인 — 고정 열쇠 대조 → 운영자 표시(_isAdmin) · 성공·실패 답
//
// [경계]
//  채팅 명령(표 · 권한 · 거절 · /item · /gm)은 명령 조각(CCommandHandler — 10-26 여기서 떼어 냄)이 한다. 주변에 보내기는 시야 조각.
//  운영자 표시로 열리는 것(채널 정원 우회 · 다른 맵 지정 이동)은 맵 관리자가, 채팅 명령 권한은 명령 조각이 본다.
//
// [모르는 것]
//  전투·가방·파티·입장·저장 — 명령이 쓰는 것들은 명령 조각이 받는다.
//  '/' 가로채기는 토글 없이 늘 있다 — 명령이 하나도 없는 빌드에서도 '/' 채팅은 방송하지 않고 명령 조각이 「모르는 명령」으로 답한다.
// ==========================================================================
#pragma once

#include "BuildConfig.h"     // USE_BROADCAST_BUNDLE — 안 읽으면 ChatHandler.cpp 의 #if 가 조용히 0 이 된다

class CPlayer;
class CSerialBuffer;
class CPacketRoutes;
class CSessionHub;
class CSightBroadcaster;
class CMapManager;
class CCommandHandler;

class CChatHandler
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(세션 허브 · 시야 조각 · 명령 조각 · 맵 관리자)보다 뒤에 이것을 선언해야 한다
    //   (멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    CChatHandler(CSessionHub& sessionHub, CSightBroadcaster& sightBroadcaster, CCommandHandler& commandHandler, CMapManager& mapManager)
        : _sessionHub(sessionHub), _sightBroadcaster(sightBroadcaster), _commandHandler(commandHandler), _mapManager(mapManager) {}

    // 받은 패킷 등록 — 채팅 · 운영자 로그인. 게임 서버의 등록(RegisterRoutes)이 부른다. false = 등록 실패(기동 거절).
    bool RegisterRoutes(CPacketRoutes& routes);

private:
    // 채팅(주변 방송 — 본인 포함 · '/' 로 시작하면 명령) · 운영자 로그인(고정 열쇠 대조)
    void RecvChat(CPlayer* player, CSerialBuffer* pMsg);
    void RecvAdminLogin(CPlayer* player, CSerialBuffer* pMsg);

    CSessionHub&           _sessionHub;         // 운영자 로그인 답
    CSightBroadcaster&     _sightBroadcaster;   // 채팅 방송
    CCommandHandler&       _commandHandler;     // '/' 로 시작하는 채팅을 넘기는 곳(채팅 명령 — CommandHandler.h)
    CMapManager&           _mapManager;         // 존 찾기(채팅)
};
