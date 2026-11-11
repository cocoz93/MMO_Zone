#include "ChannelListFlow.h"

#include <Identity/ChannelNumber.h>
#include <algorithm>
#include <cstring>

#include "EnterGate.h"
#include "MapManager.h"
#include "PacketBuild.h"
#include "PacketRoutes.h"
#include "Player.h"
#include "SerialBuffer.h"
#include "SessionHub.h"
#include "WorldLink.h"
#include "Zone.h"

namespace
{
    constexpr int64_t REPORT_PERIOD_MS = 1000;   // 플랜 2절 14번 — 목록은 사람이 펼쳐 보는 값이라 1초면 된다

    static_assert(sizeof(MSG_S2C_CHANNEL_LIST) <= MAX_PACKET_SIZE, "꽉 찬 채널 목록이 한 패킷에 안 들어간다");
    static_assert(CHANNEL_LIST_MAX == LINK_CHANNEL_LIST_MAX, "웹 목록과 서버 간 목록의 줄 상한이 어긋났다");
    static_assert(CHANNEL_WS_ADDR_BYTES == LINK_WS_ADDR_BYTES, "웹 주소 칸 크기가 어긋났다");
    static_assert(ChannelNumber::BLOCK_MAX <= LINK_ZONE_CHANNELS_MAX, "맵 하나의 채널 줄이 패킷 한 장에 안 들어간다");
}

struct CChannelListFlow::ListReply
{
    const LINK_CHANNEL_LIST_ACK* head;
    const char* rows;
};

void CChannelListFlow::Configure(int zoneServerId, int channelBlock)
{
    _zoneServerId = zoneServerId;
    _channelBlock = channelBlock;
}

bool CChannelListFlow::RegisterRoutes(CPacketRoutes& routes)
{
    return routes.OnPlayer(MsgType::C2S_CHANNEL_LIST_REQ, sizeof(MSG_C2S_CHANNEL_LIST_REQ),
                           [this](CPlayer* p, CSerialBuffer* m) { RecvListReq(p, m); });
}

void CChannelListFlow::Tick(int64_t nowMs)
{
    if (nowMs < _nextReportMs)
        return;
    _nextReportMs = nowMs + REPORT_PERIOD_MS;
    ReportChanged();
}

void CChannelListFlow::OnWorldUp()
{
    _lastSent.clear();
    _nextReportMs = 0;   // 다음 틱에 전부 — 끊긴 사이 월드 서버가 이 존 서버의 줄을 지웠다
}

void CChannelListFlow::OnWorldDown()
{
    _requests.FailAll(LinkRequestKind::ChannelList);   // 시한까지 기다리지 않고 자기 채널로 답한다
}

void CChannelListFlow::ReportChanged()
{
    if (!_worldLink.IsUp())
        return;

    _mapManager.ForEachFieldMap([this](MapId mapId, CFieldMap& fieldMap)
    {
        _rows.clear();
        fieldMap.ForEachChannel([this](int32_t channelIndex, CZone* zone)
        {
            if (channelIndex >= _channelBlock)
                return;   // 구간 밖이면 보이는 번호가 다음 존 서버와 겹친다
            LinkZoneChannelRow row{};
            row.channel = channelIndex;
            row.players = static_cast<uint16_t>(zone->GetPlayerCount());
            _rows.push_back(row);
        });
        std::sort(_rows.begin(), _rows.end(),
                  [](const LinkZoneChannelRow& a, const LinkZoneChannelRow& b) { return a.channel < b.channel; });

        const auto [last, isNew] = _lastSent.try_emplace(ToInt(mapId));
        const bool same = !isNew && last->second.size() == _rows.size()
            && (_rows.empty() || std::memcmp(last->second.data(), _rows.data(), _rows.size() * sizeof(LinkZoneChannelRow)) == 0);
        if (same)
            return;
        if (_worldLink.SendChannels(mapId, _rows.data(), static_cast<int>(_rows.size())))
            last->second = _rows;
        else
            _lastSent.erase(last);   // 못 보냈다 — 다음 번에 다시
    });
}

void CChannelListFlow::RecvListReq(CPlayer* player, CSerialBuffer* msg)
{
    MSG_C2S_CHANNEL_LIST_REQ req;
    if (msg->PeekData(reinterpret_cast<char*>(&req), sizeof(req)) != static_cast<int>(sizeof(req)))
        return;
    if (player->_channelListAsking)
        return;   // 앞 질문의 답이 곧 간다 — 연타로 월드 서버에 질문이 쌓이지 않게

    const MapId mapId{ req.mapId };
    if (_mapManager.FindFieldMap(mapId) == nullptr)
    {
        _entries.clear();
        SendEntries(player);
        return;
    }

    const SessionId sessionId = player->_sessionId;
    const uint64_t requestId = _requests.Begin(LinkRequestKind::ChannelList, CEnterGate::NowMs(),
        [this, sessionId, mapId](const void* reply) { OnListDone(sessionId, mapId, static_cast<const ListReply*>(reply)); });
    if (!_worldLink.AskChannels(requestId, mapId))   // 월드 서버 연결이 없다
    {
        _requests.Cancel(requestId);
        SendOwnChannels(player, mapId);
        return;
    }
    player->_channelListAsking = true;
}

void CChannelListFlow::OnListAck(const LINK_CHANNEL_LIST_ACK& head, const char* rows, int64_t nowMs)
{
    const ListReply reply{ &head, rows };
    _requests.Complete(head.requestId, nowMs, &reply);
}

void CChannelListFlow::OnListDone(SessionId sessionId, MapId mapId, const ListReply* reply)
{
    CPlayer* player = _sessionHub.FindPlayer(sessionId);
    if (player == nullptr)
        return;   // 답이 오기 전에 나갔다
    player->_channelListAsking = false;

    if (reply == nullptr)
    {
        SendOwnChannels(player, mapId);
        return;
    }

    _entries.clear();
    for (uint16_t i = 0; i < reply->head->count; ++i)
    {
        LinkChannelListRow row;
        std::memcpy(&row, reply->rows + size_t{ i } * sizeof(LinkChannelListRow), sizeof(row));
        if (row.channel >= _channelBlock)
            continue;
        ChannelListEntry entry{};
        entry.visibleNo = ChannelNumber::ToVisible(row.zoneServerId, _channelBlock, row.channel);
        entry.players = row.players;
        _entries.push_back(entry);
    }
    if (_entries.empty())
    {
        SendOwnChannels(player, mapId);   // 첫 보고가 닿기 전이다
        return;
    }
    SendEntries(player);
}

void CChannelListFlow::SendOwnChannels(CPlayer* player, MapId mapId)
{
    _entries.clear();
    if (CFieldMap* fieldMap = _mapManager.FindFieldMap(mapId))
    {
        fieldMap->ForEachChannel([this](int32_t channelIndex, CZone* zone)
        {
            if (channelIndex >= _channelBlock)
                return;
            ChannelListEntry entry{};
            entry.visibleNo = ChannelNumber::ToVisible(_zoneServerId, _channelBlock, channelIndex);
            entry.players = static_cast<uint16_t>(zone->GetPlayerCount());
            _entries.push_back(entry);
        });
        std::sort(_entries.begin(), _entries.end(),
                  [](const ChannelListEntry& a, const ChannelListEntry& b) { return a.visibleNo < b.visibleNo; });
    }
    SendEntries(player);
}

void CChannelListFlow::SendEntries(CPlayer* player)
{
    const int count = (std::min)(static_cast<int>(_entries.size()), static_cast<int>(CHANNEL_LIST_MAX));

    CSerialBuffer* buf = CSerialBuffer::Alloc();
    PacketBuild::BeginPacket(buf, MSG_S2C_CHANNEL_LIST::TYPE);
    *buf << static_cast<int32_t>(_zoneServerId);
    *buf << static_cast<int32_t>(_channelBlock);
    *buf << static_cast<uint16_t>(count);
    for (int i = 0; i < count; ++i)
    {
        *buf << _entries[i].visibleNo;
        *buf << _entries[i].players;
    }
    PacketBuild::FinalizePacket(buf);
    _sessionHub.SendPacket(player, buf);
}
