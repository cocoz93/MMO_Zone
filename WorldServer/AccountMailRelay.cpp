#include "AccountMailRelay.h"

#include <Common/ErrorLog.h>
#include <Lobby/LobbyLoop.h>
#include <Lobby/PacketSend.h>
#include <Protocol/ServerLinkProtocol.h>
#include <cstring>

namespace
{
    // 답이 있는 요청 여섯 — 계정 서버가 끊기면 이 종류의 대기를 전부 지운다
    constexpr LinkRequestKind MAIL_KINDS[] = {
        LinkRequestKind::AccMailCount, LinkRequestKind::AccMailList, LinkRequestKind::AccMailRead,
        LinkRequestKind::AccMailClaim, LinkRequestKind::AccMailSend, LinkRequestKind::AccMailErase,
    };
}

CAccountMailRelay::CAccountMailRelay(CLinkPeers& zones, CLinkConnector& account, CMonitorManager& monitor, const CWorldPresence& presence, int replyTimeoutSec)
    : _zones(zones), _account(account), _monitor(monitor), _presence(presence), _requests(replyTimeoutSec)
{
}

bool CAccountMailRelay::RegisterRoutes(CPacketTable& zoneTable, CPacketTable& accountTable)
{
    bool ok = zoneTable.Register<LINK_ACCMAIL_COUNT_REQ>([this](SessionId id, CSerialBuffer* m)
        { Forward<LINK_ACCMAIL_COUNT_REQ, LINK_ACCMAIL_COUNT_ACK>(id, m, LinkRequestKind::AccMailCount); });
    ok = zoneTable.Register<LINK_ACCMAIL_LIST_REQ>([this](SessionId id, CSerialBuffer* m)
        { Forward<LINK_ACCMAIL_LIST_REQ, LINK_ACCMAIL_LIST_ACK>(id, m, LinkRequestKind::AccMailList); }) && ok;
    ok = zoneTable.Register<LINK_ACCMAIL_READ_REQ>([this](SessionId id, CSerialBuffer* m)
        { Forward<LINK_ACCMAIL_READ_REQ, LINK_ACCMAIL_READ_ACK>(id, m, LinkRequestKind::AccMailRead); }) && ok;
    ok = zoneTable.Register<LINK_ACCMAIL_CLAIM_REQ>([this](SessionId id, CSerialBuffer* m)
        { Forward<LINK_ACCMAIL_CLAIM_REQ, LINK_ACCMAIL_CLAIM_ACK>(id, m, LinkRequestKind::AccMailClaim); }) && ok;
    ok = zoneTable.Register<LINK_ACCMAIL_SEND_REQ>([this](SessionId id, CSerialBuffer* m)
        { Forward<LINK_ACCMAIL_SEND_REQ, LINK_ACCMAIL_SEND_ACK>(id, m, LinkRequestKind::AccMailSend); }) && ok;
    ok = zoneTable.Register<LINK_ACCMAIL_ERASE_REQ>([this](SessionId id, CSerialBuffer* m)
        { Forward<LINK_ACCMAIL_ERASE_REQ, LINK_ACCMAIL_ERASE_ACK>(id, m, LinkRequestKind::AccMailErase); }) && ok;
    ok = zoneTable.Register<LINK_ACCMAIL_DONE>([this](SessionId id, CSerialBuffer* m) { ForwardOneWay<LINK_ACCMAIL_DONE>(id, m, "받음"); }) && ok;
    ok = zoneTable.Register<LINK_ACCMAIL_UNCLAIM>([this](SessionId id, CSerialBuffer* m) { ForwardOneWay<LINK_ACCMAIL_UNCLAIM>(id, m, "받는 중 풀기"); }) && ok;

    ok = accountTable.Register<LINK_ACCMAIL_COUNT_ACK>([this](SessionId, CSerialBuffer* m) { Reply<LINK_ACCMAIL_COUNT_ACK>(m); }) && ok;
    ok = accountTable.Register<LINK_ACCMAIL_LIST_ACK>([this](SessionId, CSerialBuffer* m) { Reply<LINK_ACCMAIL_LIST_ACK>(m); }) && ok;
    ok = accountTable.Register<LINK_ACCMAIL_READ_ACK>([this](SessionId, CSerialBuffer* m) { Reply<LINK_ACCMAIL_READ_ACK>(m); }) && ok;
    ok = accountTable.Register<LINK_ACCMAIL_CLAIM_ACK>([this](SessionId, CSerialBuffer* m) { Reply<LINK_ACCMAIL_CLAIM_ACK>(m); }) && ok;
    ok = accountTable.Register<LINK_ACCMAIL_SEND_ACK>([this](SessionId, CSerialBuffer* m) { Reply<LINK_ACCMAIL_SEND_ACK>(m); }) && ok;
    ok = accountTable.Register<LINK_ACCMAIL_ERASE_ACK>([this](SessionId, CSerialBuffer* m) { Reply<LINK_ACCMAIL_ERASE_ACK>(m); }) && ok;
    return ok;
}

void CAccountMailRelay::Tick(int64_t nowMs)
{
    _requests.ExpireDue(nowMs);
}

void CAccountMailRelay::OnAccountDown()
{
    int failed = 0;
    for (const LinkRequestKind kind : MAIL_KINDS)
        failed += _requests.FailAll(kind);
    if (failed > 0)
        SLOG_WARN("[AccountMailRelay] 계정 서버 끊김 — 답을 기다리던 계정 우편 요청 {}건을 지움. 존 서버의 시한이 끝낸다", failed);
}

template <typename TRequest, typename TAck>
void CAccountMailRelay::Forward(SessionId zone, CSerialBuffer* msg, LinkRequestKind kind)
{
    // 등록 전 연결은 버린다 — 파티 · 채널과 같은 문(CWorldServer::CheckRegistered)
    if (!_presence.IsRegistered(zone))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    TRequest request;
    if (!PacketSend::Read(msg, request))
    {
        _monitor._packetErrors.Inc();
        return;
    }

    const uint64_t zoneRequestId = request.requestId;
    const uint64_t requestId = _requests.Begin(kind, CLobbyLoop::NowMs(),
        [this, zone, zoneRequestId](const void* reply)
        {
            if (reply == nullptr)
                return;   // 시한 · 계정 서버 끊김 — 존 서버가 같은 시한으로 끝낸다
            TAck ack;
            std::memcpy(&ack, reply, sizeof(ack));
            ack.requestId = zoneRequestId;
            _zones.Send(zone, ack);   // 그새 존 서버가 끊겼으면 보낼 곳이 없어 버려진다
        });
    request.requestId = requestId;
    if (!_account.Send(request))
    {
        _requests.Cancel(requestId);
        TAck ack{};
        ack.requestId = zoneRequestId;
        ack.result = static_cast<uint8_t>(LinkMailResult::OFFLINE);
        _zones.Send(zone, ack);
    }
}

template <typename TPacket>
void CAccountMailRelay::ForwardOneWay(SessionId zone, CSerialBuffer* msg, const char* what)
{
    if (!_presence.IsRegistered(zone))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    TPacket packet;
    if (!PacketSend::Read(msg, packet))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    if (!_account.Send(packet))
    {
        SLOG_WARN("[AccountMailRelay] 계정 서버에 못 넘김(끊김) — {} · 계정 {} 우편 {} · 캐릭터 {}. 받는 중으로 남아 그 캐릭터의 입장 때 이어진다",
                  what, static_cast<int64_t>(packet.accountId), static_cast<int64_t>(packet.mailId), static_cast<int64_t>(packet.characterId));
    }
}

template <typename TAck>
void CAccountMailRelay::Reply(CSerialBuffer* msg)
{
    TAck ack;
    if (!PacketSend::Read(msg, ack))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    _requests.Complete(ack.requestId, CLobbyLoop::NowMs(), &ack);   // 시한 뒤에 온 답은 짝이 없어 버려진다
}
