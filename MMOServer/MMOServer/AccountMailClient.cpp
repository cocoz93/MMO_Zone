#include "AccountMailClient.h"

#if USE_MAIL

#include <utility>
#include "EnterGate.h"       // NowMs — _linkRequests 의 시각(CGameServer::GameLoopThread 의 ExpireDue 와 같은 시계)
#include "SerialBuffer.h"
#include "WorldLink.h"
#include <Lobby/PacketSend.h>

namespace
{
    // 답이 있는 요청 여섯 — 월드 서버가 끊기면 이 종류의 대기를 전부 끝낸다
    constexpr LinkRequestKind MAIL_KINDS[] = {
        LinkRequestKind::AccMailCount, LinkRequestKind::AccMailList, LinkRequestKind::AccMailRead,
        LinkRequestKind::AccMailClaim, LinkRequestKind::AccMailSend, LinkRequestKind::AccMailErase,
    };
}

bool CAccountMailClient::RegisterRoutes()
{
    CPacketTable& world = _worldLink.Routes();
    bool ok = world.Register<LINK_ACCMAIL_COUNT_ACK>([this](SessionId, CSerialBuffer* m) { OnAck<LINK_ACCMAIL_COUNT_ACK>(m); });
    ok = world.Register<LINK_ACCMAIL_LIST_ACK>([this](SessionId, CSerialBuffer* m) { OnAck<LINK_ACCMAIL_LIST_ACK>(m); }) && ok;
    ok = world.Register<LINK_ACCMAIL_READ_ACK>([this](SessionId, CSerialBuffer* m) { OnAck<LINK_ACCMAIL_READ_ACK>(m); }) && ok;
    ok = world.Register<LINK_ACCMAIL_CLAIM_ACK>([this](SessionId, CSerialBuffer* m) { OnAck<LINK_ACCMAIL_CLAIM_ACK>(m); }) && ok;
    ok = world.Register<LINK_ACCMAIL_SEND_ACK>([this](SessionId, CSerialBuffer* m) { OnAck<LINK_ACCMAIL_SEND_ACK>(m); }) && ok;
    ok = world.Register<LINK_ACCMAIL_ERASE_ACK>([this](SessionId, CSerialBuffer* m) { OnAck<LINK_ACCMAIL_ERASE_ACK>(m); }) && ok;
    return ok;
}

void CAccountMailClient::OnWorldDown()
{
    for (const LinkRequestKind kind : MAIL_KINDS)
        _requests.FailAll(kind);   // 시한까지 기다리지 않는다 — 부르는 쪽이 ACC_TIMEOUT 으로 닫는다
}

bool CAccountMailClient::Count(int64_t account, int64_t character, ReplyFn<LINK_ACCMAIL_COUNT_ACK> done)
{
    LINK_ACCMAIL_COUNT_REQ request{};
    request.accountId = account;
    request.characterId = character;
    return Ask(LinkRequestKind::AccMailCount, request, std::move(done));
}

bool CAccountMailClient::List(int64_t account, int64_t beforeMailId, ReplyFn<LINK_ACCMAIL_LIST_ACK> done)
{
    LINK_ACCMAIL_LIST_REQ request{};
    request.accountId = account;
    request.beforeMailId = beforeMailId;
    return Ask(LinkRequestKind::AccMailList, request, std::move(done));
}

bool CAccountMailClient::Read(int64_t account, int64_t mailId, ReplyFn<LINK_ACCMAIL_READ_ACK> done)
{
    LINK_ACCMAIL_READ_REQ request{};
    request.accountId = account;
    request.mailId = mailId;
    return Ask(LinkRequestKind::AccMailRead, request, std::move(done));
}

bool CAccountMailClient::Claim(int64_t account, int64_t mailId, int64_t character, ReplyFn<LINK_ACCMAIL_CLAIM_ACK> done)
{
    LINK_ACCMAIL_CLAIM_REQ request{};
    request.accountId = account;
    request.mailId = mailId;
    request.characterId = character;
    return Ask(LinkRequestKind::AccMailClaim, request, std::move(done));
}

bool CAccountMailClient::Erase(int64_t account, int64_t mailId, ReplyFn<LINK_ACCMAIL_ERASE_ACK> done)
{
    LINK_ACCMAIL_ERASE_REQ request{};
    request.accountId = account;
    request.mailId = mailId;
    return Ask(LinkRequestKind::AccMailErase, request, std::move(done));
}

bool CAccountMailClient::Send(LINK_ACCMAIL_SEND_REQ& request, ReplyFn<LINK_ACCMAIL_SEND_ACK> done)
{
    return Ask(LinkRequestKind::AccMailSend, request, std::move(done));
}

bool CAccountMailClient::Finish(int64_t account, int64_t mailId, int64_t character)
{
    LINK_ACCMAIL_DONE packet{};
    packet.accountId = account;
    packet.mailId = mailId;
    packet.characterId = character;
    return _worldLink.SendLink(packet);
}

bool CAccountMailClient::Unclaim(int64_t account, int64_t mailId, int64_t character)
{
    LINK_ACCMAIL_UNCLAIM packet{};
    packet.accountId = account;
    packet.mailId = mailId;
    packet.characterId = character;
    return _worldLink.SendLink(packet);
}

template <typename TRequest, typename TAck>
bool CAccountMailClient::Ask(LinkRequestKind kind, TRequest& request, ReplyFn<TAck> done)
{
    if (!_worldLink.IsUp())
        return false;   // 번호를 안 쓴다 — 응답 시간 지표에 안 들어간다
    const uint64_t requestId = _requests.Begin(kind, CEnterGate::NowMs(),
        [done = std::move(done)](const void* reply) { done(static_cast<const TAck*>(reply)); });
    request.requestId = requestId;
    if (!_worldLink.SendLink(request))
    {
        _requests.Cancel(requestId);
        return false;
    }
    return true;
}

template <typename TAck>
void CAccountMailClient::OnAck(CSerialBuffer* msg)
{
    TAck ack;
    if (!PacketSend::Read(msg, ack))
    {
        _worldLink.CountPacketError();
        return;
    }
    _requests.Complete(ack.requestId, CEnterGate::NowMs(), &ack);   // 시한 뒤에 온 답은 짝이 없어 버려진다
}

#endif // USE_MAIL
