// ==========================================================================
// CChatHandler — 채팅·운영: 채팅 · 운영자 로그인 · 채팅 명령(/item · /gm) (게임 서버 나누기 13 단계 · C8 · 토글 없음 — 명령 줄만 토글: /item USE_INVENTORY · /gm USE_COMBAT)
//
// [책임]
//  - 받은 패킷 둘의 처리기와 그 등록(RegisterRoutes — 채팅 · 운영자 로그인) — 컨텐츠 조각이 자기 줄을 등록한다
//  - 채팅 — 받은 크기로 글자 수를 역산해 끝을 맺고 주변에 방송(수신섹터 digest 면 틱 끝 묶음에 보류 등록). '/' 로 시작하면 방송 없이 명령으로
//  - 운영자 로그인 — 고정 열쇠 대조 → 운영자 표시(_isAdmin) · 성공·실패 답
//  - 채팅 명령 — 이름 → 처리 함수 표 · 운영자 권한 · 거절 지표 · /item(가방에 바로 넣기 — USE_INVENTORY) · /gm(GM 모드 켜고 끄기 — USE_COMBAT)
//
// [경계]
//  가방 넣기 판정은 가방(CInventory::TryAdd), 바뀐 칸 내보내기는 가방·땅 조각, 스탯 다시 세우기는 스탯 규칙, 알림 한 줄·내 스탯은 본인 알림 조각,
//  주변에 보내기는 시야 조각. 운영자 표시로 열리는 것(채널 정원 우회 · 다른 맵 지정 이동)은 맵 관리자가 본다.
//
// [모르는 것]
//  전투·파티·입장·저장.
//  명령 틀('/' 가로채기 · 표 · 권한 · 거절)은 토글 없이 늘 있고, 명령 줄만 그 명령이 기대는 토글을 본다. 명령이 하나도 없는 빌드(전투를 끈 빌드)에서도
//  '/' 채팅은 방송하지 않고 「모르는 명령」으로 답한다. 게임 서버 나누기 14 단계 전에는 표 전체가 USE_INVENTORY 안이라 가방을 끈 빌드엔 /gm 이 없었다.
// ==========================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BuildConfig.h"     // USE_INVENTORY · USE_COMBAT — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include <Protocol/Protocol.h>   // ChatChar — 명령 낱말 나누기가 받는 글자 형

class CPlayer;
class CSerialBuffer;
class CPacketRoutes;
class CSessionHub;
class CSightBroadcaster;
class CSelfNotifier;
class CGameMonitor;
class CMapManager;
#if USE_COMBAT
class CStatSystem;
#endif
#if USE_INVENTORY
class CItemHandler;
class CGameDataFiles;
#endif

class CChatHandler
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(세션 허브 · 시야 조각 · 본인 알림 조각 · 게임 지표 · 맵 관리자와, 명령이 쓰는 스탯 규칙 ·
    //   가방·땅 조각 · 기획 데이터)보다 뒤에 이것을 선언해야 한다(멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    //   명령 하나만 쓰는 것은 그 명령의 토글 안에서 받는다 — /gm 의 스탯 규칙(USE_COMBAT) · /item 의 가방·땅 조각과 기획 데이터(USE_INVENTORY).
    CChatHandler(CSessionHub& sessionHub, CSightBroadcaster& sightBroadcaster, CSelfNotifier& selfNotifier, CGameMonitor& gameMonitor,
#if USE_COMBAT
                 CStatSystem& statSystem,
#endif
#if USE_INVENTORY
                 CItemHandler& itemHandler, const CGameDataFiles& gameDataFiles,
#endif
                 CMapManager& mapManager)
        : _sessionHub(sessionHub), _sightBroadcaster(sightBroadcaster), _selfNotifier(selfNotifier), _gameMonitor(gameMonitor),
#if USE_COMBAT
          _statSystem(statSystem),
#endif
#if USE_INVENTORY
          _itemHandler(itemHandler), _gameDataFiles(gameDataFiles),
#endif
          _mapManager(mapManager) {}

    // 받은 패킷 등록 — 채팅 · 운영자 로그인. 게임 서버의 등록(RegisterRoutes)이 부른다. false = 등록 실패(기동 거절).
    bool RegisterRoutes(CPacketRoutes& routes);

private:
    // 채팅(주변 방송 — 본인 포함 · '/' 로 시작하면 명령) · 운영자 로그인(고정 열쇠 대조)
    void RecvChat(CPlayer* player, CSerialBuffer* pMsg);
    void RecvAdminLogin(CPlayer* player, CSerialBuffer* pMsg);

    // '/' 로 시작하는 채팅 — 방송하지 않고 명령으로 처리한다. 답은 본인에게 S2C_ERROR 한 줄.
    //   명령은 이름 → 처리 함수 표(ChatHandler.cpp HandleChatCommand). 지금 둘 다 운영자 전용이다. 표는 늘 있고 줄만 토글을 본다.
    void HandleChatCommand(CPlayer* player, const ChatChar* msg, uint16_t len);
#if USE_INVENTORY
    void RunItemCommand(CPlayer* player, const std::vector<std::string>& words);   // /item <아이템번호> [개수] — 가방에 바로 넣기
#endif
#if USE_COMBAT
    void RunGmCommand(CPlayer* player, const std::vector<std::string>& words);     // /gm — GM 모드 켜고 끄기(Player.h GM 칸)
#endif

    CSessionHub&           _sessionHub;         // 운영자 로그인 답
    CSightBroadcaster&     _sightBroadcaster;   // 채팅 방송 · /gm 의 체력·속도 알림(본인 + 주변)
    CSelfNotifier&         _selfNotifier;       // 명령 답(알림 한 줄) · /gm 의 내 스탯·마력
    CGameMonitor&          _gameMonitor;        // 명령 거절 · /gm 켜고 끈 수(_chat) · /item 넣은 수(_inventory)
#if USE_COMBAT
    CStatSystem&           _statSystem;         // /gm 의 스탯 다시 세우기
#endif
#if USE_INVENTORY
    CItemHandler&          _itemHandler;        // /item 뒤 바뀐 칸 내보내기(칸 패킷 + DB)
    const CGameDataFiles&  _gameDataFiles;      // 아이템 표(/item 번호 찾기)
#endif
    CMapManager&           _mapManager;         // 존 찾기(채팅 · /gm)
};
