// ==========================================================================
// CCommandHandler — 채팅 명령: '/' 로 시작하는 채팅을 받아 명령 표 · 운영자 권한 · 거절 지표 · 명령 둘(/item · /gm)
//   (10-26 채팅·운영 조각 CChatHandler 에서 떼어 냄 — 명령이 늘 때마다 채팅 조각이 받는 참조가 늘고, /die 가 전투를 부르면 「전투는 모른다」가 깨져서)
//
// [책임]
//  - 명령 틀 — 낱말 나누기 · 이름 → 처리 함수 표 · 운영자 권한 · 거절 지표(_chat — 지표 이름은 떼기 전 그대로)
//  - /item(가방에 바로 넣기 — USE_INVENTORY) · /gm(GM 모드 켜고 끄기 — USE_COMBAT)
//
// [경계]
//  '/' 가로채기(방송 안 함)는 채팅 조각(CChatHandler::RecvChat)이 하고 여기로 넘긴다. 운영자 로그인(_isAdmin 을 세우는 것)도 채팅 조각이다 —
//  여기는 읽기만 한다. 가방 넣기 판정은 가방(CInventory::TryAdd), 바뀐 칸 내보내기는 가방·땅 조각, 스탯 다시 세우기는 스탯 규칙,
//  알림 한 줄·내 스탯은 본인 알림 조각, 주변에 보내기는 시야 조각.
//
// [모르는 것]
//  받은 패킷 · 파티 · 입장 · 저장. 어떤 명령을 더 갖출지 · 권한 단계 · 명령 기록은 아직 안 정했다(운영자 명령 설계는 따로 — 10-26).
//  명령 틀은 토글 없이 늘 있고, 명령 줄만 그 명령이 기대는 토글을 본다. 명령이 하나도 없는 빌드(전투를 끈 빌드 — 가방도 전투가 있어야 켜진다)에서도
//  '/' 채팅은 「모르는 명령」으로 답한다.
// ==========================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BuildConfig.h"     // USE_INVENTORY · USE_COMBAT — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include <Protocol/Protocol.h>   // ChatChar — 명령 낱말 나누기가 받는 글자 형

class CPlayer;
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

class CCommandHandler
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(시야 조각 · 본인 알림 조각 · 게임 지표 · 맵 관리자와, 명령이 쓰는 스탯 규칙 ·
    //   가방·땅 조각 · 기획 데이터)보다 뒤에 이것을 선언해야 한다(멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    //   명령 하나만 쓰는 것은 그 명령의 토글 안에서 받는다 — /gm 의 스탯 규칙(USE_COMBAT) · /item 의 가방·땅 조각과 기획 데이터(USE_INVENTORY).
    CCommandHandler(CSightBroadcaster& sightBroadcaster, CSelfNotifier& selfNotifier, CGameMonitor& gameMonitor,
#if USE_COMBAT
                    CStatSystem& statSystem,
#endif
#if USE_INVENTORY
                    CItemHandler& itemHandler, const CGameDataFiles& gameDataFiles,
#endif
                    CMapManager& mapManager)
        : _sightBroadcaster(sightBroadcaster), _selfNotifier(selfNotifier), _gameMonitor(gameMonitor),
#if USE_COMBAT
          _statSystem(statSystem),
#endif
#if USE_INVENTORY
          _itemHandler(itemHandler), _gameDataFiles(gameDataFiles),
#endif
          _mapManager(mapManager) {}

    // '/' 로 시작하는 채팅 — 채팅 조각(RecvChat)이 방송하지 않고 넘긴다. 답은 본인에게 S2C_ERROR 한 줄.
    //   명령은 이름 → 처리 함수 표(CommandHandler.cpp Handle). 지금 둘 다 운영자 전용이다. 표는 늘 있고 줄만 토글을 본다.
    //   msg 는 NUL 로 끝맺은 채팅 글자(len = 글자 수 — 채팅 조각이 받은 크기로 역산한 값).
    void Handle(CPlayer* player, const ChatChar* msg, uint16_t len);

private:
#if USE_INVENTORY
    void RunItemCommand(CPlayer* player, const std::vector<std::string>& words);   // /item <아이템번호> [개수] — 가방에 바로 넣기
#endif
#if USE_COMBAT
    void RunGmCommand(CPlayer* player, const std::vector<std::string>& words);     // /gm — GM 모드 켜고 끄기(Player.h GM 칸)
#endif

    CSightBroadcaster&     _sightBroadcaster;   // /gm 의 체력·속도 알림(본인 + 주변)
    CSelfNotifier&         _selfNotifier;       // 명령 답(알림 한 줄) · /gm 의 내 스탯·마력
    CGameMonitor&          _gameMonitor;        // 명령 거절 · /gm 켜고 끈 수(_chat) · /item 넣은 수(_inventory)
#if USE_COMBAT
    CStatSystem&           _statSystem;         // /gm 의 스탯 다시 세우기
#endif
#if USE_INVENTORY
    CItemHandler&          _itemHandler;        // /item 뒤 바뀐 칸 내보내기(칸 패킷 + DB)
    const CGameDataFiles&  _gameDataFiles;      // 아이템 표(/item 번호 찾기)
#endif
    CMapManager&           _mapManager;         // 존 찾기(/gm)
};
