#include "MailSignalRelay.h"

#include <Lobby/PacketSend.h>
#include <Monitoring/MetricsText.h>
#include <Protocol/ServerLinkProtocol.h>

namespace
{
    void Add(std::atomic<int64_t>& value) { value.fetch_add(1, std::memory_order_relaxed); }
}

CMailSignalRelay::CMailSignalRelay(CLinkPeers& zones, CMonitorManager& monitor, const CWorldPresence& presence)
    : _zones(zones), _monitor(monitor), _presence(presence)
{
}

bool CMailSignalRelay::RegisterRoutes(CPacketTable& zoneTable, CPacketTable& accountTable)
{
    bool ok = zoneTable.Register<LINK_MAIL_ARRIVED>([this](SessionId id, CSerialBuffer* m) { OnMailArrived(id, m); });
    ok = accountTable.Register<LINK_ACCMAIL_ARRIVED>([this](SessionId, CSerialBuffer* m) { OnAccMailArrived(m); }) && ok;
    return ok;
}

void CMailSignalRelay::OnMailArrived(SessionId zone, CSerialBuffer* msg)
{
    // 등록 전 연결은 버린다 — 파티 · 채널 · 계정 우편과 같은 문(CWorldServer::CheckRegistered)
    if (!_presence.IsRegistered(zone))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    LINK_MAIL_ARRIVED signal;
    if (!PacketSend::Read(msg, signal))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    Add(_received[CHARACTER]);

    const CWorldPresence::Person* to = _presence.Find(CharacterId{ signal.characterId });
    if (to == nullptr)
    {
        Add(_offline);   // 접속 중이 아니다 — 입장 때 수를 새로 받는다
        return;
    }
    // 받는 사람이 보낸 사람과 같은 존 서버여도 월드를 거쳐 돌아간다(5.6 — 찾기는 월드 서버의 장부 한 곳)
    Add(_zones.Send(to->zoneSession, signal) ? _forwarded[CHARACTER] : _sendFailed[CHARACTER]);
}

void CMailSignalRelay::OnAccMailArrived(CSerialBuffer* msg)
{
    LINK_ACCMAIL_ARRIVED signal;
    if (!PacketSend::Read(msg, signal))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    Add(_received[ACCOUNT]);

    // 계정 번호로 존 서버를 고르지 않는다 — 존 서버가 _accountId 로 사람을 찾는다(5.6)
    _presence.ForEachZone([&](const CWorldPresence::Zone& z)
    {
        Add(_zones.Send(z.session, signal) ? _forwarded[ACCOUNT] : _sendFailed[ACCOUNT]);
    });
}

void CMailSignalRelay::WriteMetrics(std::ostringstream& ss) const
{
    static constexpr const char* NAMES[SIGNAL_COUNT] = { "character", "account" };
    const auto load = [](const std::atomic<int64_t>& value) { return value.load(std::memory_order_relaxed); };
    const auto labeled = [&](const char* name, const char* help, const std::atomic<int64_t> (&values)[SIGNAL_COUNT])
    {
        MetricsText::WriteHead(ss, name, help, "counter");
        for (int i = 0; i < SIGNAL_COUNT; ++i)
            ss << name << "{signal=\"" << NAMES[i] << "\"} " << load(values[i]) << "\n";
        ss << "\n";
    };
    labeled("mmo_world_mail_signal_received_total", "Mail arrival signals received (character from a zone server, account from the account server)", _received);
    labeled("mmo_world_mail_signal_forwarded_total", "Mail arrival signals forwarded to zone servers, one per zone packet", _forwarded);
    labeled("mmo_world_mail_signal_send_failed_total", "Mail arrival signals that could not be sent to a zone server (disconnected meanwhile)", _sendFailed);
    MetricsText::WriteCounter(ss, "mmo_world_mail_signal_offline_total", "Character mail signals dropped because the receiver is not in the presence ledger", load(_offline));
}
