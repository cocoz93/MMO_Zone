#include "TokenCheckFlow.h"

#include <Lobby/LobbyLoop.h>
#include <Lobby/PacketSend.h>
#include <Protocol/ServerLinkProtocol.h>

#include "AccountMetrics.h"
#include "AuthTokenLedger.h"

void CTokenCheckFlow::OnCheck(SessionId peer, CSerialBuffer* msg)
{
    LINK_TOKEN_CHECK_REQ request;
    if (!PacketSend::Read(msg, request))
    {
        _monitor._packetErrors.Inc();
        return;
    }

    AccountId accountId = NO_ACCOUNT;
    const TokenCheckResult result = _ledger.Consume(request.token, CLobbyLoop::NowMs(), accountId);
    _metrics.CountToken(result);

    LINK_TOKEN_CHECK_ACK ack{};
    ack.requestId = request.requestId;
    ack.result = static_cast<uint8_t>(result);
    ack.accountId = ToInt(accountId);
    _peers.Send(peer, ack);
}
