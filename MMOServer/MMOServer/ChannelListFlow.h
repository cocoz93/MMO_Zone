// ==========================================================================
// CChannelListFlow — 채널 목록: 월드 서버에 채널 줄을 알리고, 플레이어의 목록 요청에 답한다
//
// [책임]
//  - 1초마다 필드 맵의 채널 줄을 지난번 보낸 값과 견줘 바뀐 맵만 알린다(3034)
//  - 월드 서버에 다시 붙으면 지난번 값을 비워 전부 다시 알린다 — 월드 서버는 끊긴 존 서버의 줄을 지웠다
//  - C2S_CHANNEL_LIST_REQ 를 월드 서버에 묻고 답을 보이는 번호로 바꿔 보낸다
//
// [월드 서버가 없을 때] 끊겼거나 3초 안에 답이 없으면 이 존 서버의 채널만 답한다
// [방 맵] 번호에 뜻이 없어 0줄로 답한다(world-server-plan.md 2절 13번)
// [스레드] Configure 는 메인 스레드(Start 전). 나머지는 게임 스레드 단독
// ==========================================================================
#pragma once

#include "NetIoModel.h"   // 맨 앞 — 소켓 헤더가 Windows.h 보다 먼저 와야 한다

#include <Identity/GameIds.h>
#include <Link/LinkRequests.h>
#include <Protocol/Protocol.h>
#include <Protocol/ServerLinkProtocol.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

class CMapManager;
class CPacketRoutes;
class CPlayer;
class CSerialBuffer;
class CSessionHub;
class CWorldLink;

class CChannelListFlow
{
public:
    CChannelListFlow(CSessionHub& sessionHub, CMapManager& mapManager, CWorldLink& worldLink, CLinkRequests& requests)
        : _sessionHub(sessionHub), _mapManager(mapManager), _worldLink(worldLink), _requests(requests) {}

    void Configure(int zoneServerId, int channelBlock);
    bool RegisterRoutes(CPacketRoutes& routes);

    void Tick(int64_t nowMs);
    void OnWorldUp();
    void OnWorldDown();
    void OnListAck(const LINK_CHANNEL_LIST_ACK& head, const char* rows, int64_t nowMs);

private:
    struct ListReply;

    void RecvListReq(CPlayer* player, CSerialBuffer* msg);
    void OnListDone(SessionId sessionId, MapId mapId, const ListReply* reply);
    void ReportChanged();
    void SendOwnChannels(CPlayer* player, MapId mapId);
    void SendEntries(CPlayer* player);

    CSessionHub&   _sessionHub;
    CMapManager&   _mapManager;
    CWorldLink&    _worldLink;
    CLinkRequests& _requests;

    int     _zoneServerId = 1;
    int     _channelBlock = 100;
    int64_t _nextReportMs = 0;

    std::unordered_map<int32_t, std::vector<LinkZoneChannelRow>> _lastSent;   // 맵 번호 → 지난번 보낸 줄
    std::vector<LinkZoneChannelRow> _rows;       // 틱마다 다시 쓰는 임시 칸
    std::vector<ChannelListEntry>   _entries;    // 답을 만드는 임시 칸
};
