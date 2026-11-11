// ==========================================================================
// CChannelMoveFlow — 다른 존 서버로 채널 바꾸기: 거절 검사 → 입력 막기 → 확정 저장 → 입장 번호 → 내보내기
//
// [흐름] world-server-plan.md 5.7
//   C2S_CHANNEL_MOVE_REQ → 거절 검사 → _moveOp = SAVING(입력 막음) → 묶음 회신을 기다림
//   → _moveOp = ASKING → 캐릭터 서버에 3037 → 3038 → S2C_CHANNEL_MOVE → 저장 없이 내보냄
//
// [실패] 저장 실패 · 시한 · Full · 연결 없음이면 S2C_ZONE_CHANGE_FAIL 로 알리고 입력을 푼다
// [세션 닫기] 내보낸 뒤 세션은 약 1초 뒤에 닫는다. 바로 닫으면 코어가 S2C_CHANNEL_MOVE 를 못 보낼 수 있다.
//             웹이 먼저 닫으면 그대로 끝난다
// [끄기] USE_DB_WORKER=0 이면 없다 — 저장 확정을 받을 길이 없다
// [스레드] Configure 는 메인 스레드(Start 전). 나머지는 게임 스레드 단독
// ==========================================================================
#pragma once

#include "NetIoModel.h"   // 맨 앞 — 소켓 헤더가 Windows.h 보다 먼저 와야 한다

#include "BuildConfig.h"   // USE_DB_WORKER · USE_TRADE · USE_COMBAT — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_DB_WORKER

#include <Identity/ChannelNumber.h>
#include <Identity/GameIds.h>
#include <Link/LinkRequests.h>
#include <Protocol/ServerLinkProtocol.h>
#include <cstdint>
#include <vector>

#include "GameMonitor.h"

class CCharacterStore;
class CMapManager;
class CPacketRoutes;
class CPlayer;
class CPlayerPlacement;
class CSerialBuffer;
class CSessionHub;
class CSightBroadcaster;
class CTradeSystem;
class CZoneLink;

class CChannelMoveFlow
{
public:
    CChannelMoveFlow(CSessionHub& sessionHub, CMapManager& mapManager, CCharacterStore& store, CZoneLink& zoneLink,
                     CLinkRequests& requests, CPlayerPlacement& placement, CSightBroadcaster& sight,
#if USE_TRADE
                     CTradeSystem& tradeSystem,
#endif
                     CGameMonitor& gameMonitor)
        : _sessionHub(sessionHub), _mapManager(mapManager), _store(store), _zoneLink(zoneLink), _requests(requests),
          _placement(placement), _sight(sight),
#if USE_TRADE
          _tradeSystem(tradeSystem),
#endif
          _gameMonitor(gameMonitor) {}

    void Configure(int zoneServerId, int channelBlock);
    void SetTimeoutSec(int timeoutSec) { _timeoutMs = int64_t{ timeoutSec } * 1000; }
    bool RegisterRoutes(CPacketRoutes& routes);

    void Tick(int64_t nowMs);
    void OnSaved(CPlayer* player, bool ok);   // 저장소가 넘김 저장의 회신을 알린다
    void OnCharacterDown();                   // 캐릭터 서버 연결 끊김
    void OnHandoffAck(const LINK_HANDOFF_ACK& ack, int64_t nowMs);

private:
    struct Saving
    {
        SessionId           sessionId;
        ChannelNumber::Place dest;
        int64_t             deadlineMs;
    };
    struct Closing
    {
        SessionId sessionId;
        int64_t   dueMs;
    };

    void RecvMoveReq(CPlayer* player, CSerialBuffer* msg);
    ChannelMoveResult Check(const CPlayer* player, int32_t visibleNo, ChannelNumber::Place& out);
    void StopWalking(CPlayer* player);
    void OnAskDone(SessionId sessionId, CharacterId characterId, ChannelNumber::Place dest, const LINK_HANDOFF_ACK* ack);
    void Fail(CPlayer* player, ChannelMoveResult result);
    void SendMove(CPlayer* player, const LINK_HANDOFF_ACK& ack);

    CSessionHub&       _sessionHub;
    CMapManager&       _mapManager;
    CCharacterStore&   _store;
    CZoneLink&         _zoneLink;
    CLinkRequests&     _requests;
    CPlayerPlacement&  _placement;
    CSightBroadcaster& _sight;
#if USE_TRADE
    CTradeSystem&      _tradeSystem;
#endif
    CGameMonitor&      _gameMonitor;

    int     _zoneServerId = 1;
    int     _channelBlock = 100;
    int64_t _timeoutMs = 3000;

    std::vector<Saving>  _saving;    // 확정 저장을 기다리는 사람 — 끊긴 사람은 시한에 빠진다
    std::vector<Closing> _closing;   // 내보낸 뒤 닫을 세션
};

#endif // USE_DB_WORKER
