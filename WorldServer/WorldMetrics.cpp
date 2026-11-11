#include "WorldMetrics.h"

#include <Monitoring/MetricsText.h>

#include <Protocol/PartyNoticeName.h>

namespace
{
    const char* PartyLeaveName(PartyLeave r)
    {
        static_assert(static_cast<int>(PartyLeave::COUNT) == 3, "PartyLeave 를 늘렸으면 이름도 붙일 것");
        switch (r)
        {
        case PartyLeave::LEAVE:     return "leave";
        case PartyLeave::KICKED:    return "kicked";
        case PartyLeave::DISBANDED: return "disbanded";
        case PartyLeave::COUNT:     break;
        }
        return "?";
    }

    const char* PartyRequestEndName(PartyRequestEnd r)
    {
        static_assert(static_cast<int>(PartyRequestEnd::COUNT) == 4, "PartyRequestEnd 를 늘렸으면 이름도 붙일 것");
        switch (r)
        {
        case PartyRequestEnd::ACCEPTED:  return "accepted";
        case PartyRequestEnd::DECLINED:  return "declined";
        case PartyRequestEnd::EXPIRED:   return "expired";
        case PartyRequestEnd::CANCELLED: return "cancelled";
        case PartyRequestEnd::COUNT:     break;
        }
        return "?";
    }

    const char* PartyAskKindName(PartyAskKind r)
    {
        static_assert(static_cast<int>(PartyAskKind::COUNT) == 3, "PartyAskKind 를 늘렸으면 이름도 붙일 것");
        switch (r)
        {
        case PartyAskKind::INVITE:  return "invite";
        case PartyAskKind::APPLY:   return "apply";
        case PartyAskKind::SUGGEST: return "suggest";
        case PartyAskKind::COUNT:   break;
        }
        return "?";
    }
}

void CWorldMetrics::Write(std::ostringstream& ss) const
{
    MetricsText::WriteGauge(ss, "mmo_world_zones", "Zone servers registered with the world server", _zones.load(std::memory_order_relaxed));
    MetricsText::WriteGauge(ss, "mmo_world_online", "Characters in the world server presence ledger", _online.load(std::memory_order_relaxed));
    MetricsText::WriteGauge(ss, "mmo_world_channel_rows", "Channel rows in the world server channel table", _channelRows.load(std::memory_order_relaxed));

    const auto load = [](const std::atomic<int64_t>& value) { return value.load(std::memory_order_relaxed); };
    const PartyCounters& pt = _party;
    MetricsText::WriteGauge(ss, "mmo_party_active", "Parties alive right now", load(pt.active));
    MetricsText::WriteGauge(ss, "mmo_party_members", "Party members right now across every party, offline included", load(pt.members));
    MetricsText::WriteGauge(ss, "mmo_party_members_online", "Party members connected right now", load(pt.membersOnline));
    MetricsText::WriteGauge(ss, "mmo_party_pending_requests", "Party requests waiting for an answer right now", load(pt.pending));
    MetricsText::WriteCounter(ss, "mmo_party_created_total", "Parties created (an invite accepted by someone without a party, or restored)", load(pt.created));
    MetricsText::WriteCounter(ss, "mmo_party_ended_total", "Parties gone (disbanded, down to one member, or everyone offline)", load(pt.ended));
    MetricsText::WriteCounter(ss, "mmo_party_member_added_total", "Members added, counting both founders of a new party", load(pt.memberAdded));
    ss << "# HELP mmo_party_member_removed_total Members removed, split by why\n# TYPE mmo_party_member_removed_total counter\n";
    for (int i = 0; i < static_cast<int>(PartyLeave::COUNT); ++i)
        ss << "mmo_party_member_removed_total{why=\"" << PartyLeaveName(static_cast<PartyLeave>(i)) << "\"} " << load(pt.memberRemoved[i]) << "\n";
    ss << "\n# HELP mmo_party_requests_total Party requests put in the table, split by kind (a suggestion approved by the leader adds one invite)\n# TYPE mmo_party_requests_total counter\n";
    for (int i = 0; i < static_cast<int>(PartyAskKind::COUNT); ++i)
        ss << "mmo_party_requests_total{kind=\"" << PartyAskKindName(static_cast<PartyAskKind>(i)) << "\"} " << load(pt.requests[i]) << "\n";
    ss << "\n# HELP mmo_party_requests_ended_total Party requests that left the table, split by why\n# TYPE mmo_party_requests_ended_total counter\n";
    for (int i = 0; i < static_cast<int>(PartyRequestEnd::COUNT); ++i)
        ss << "mmo_party_requests_ended_total{why=\"" << PartyRequestEndName(static_cast<PartyRequestEnd>(i)) << "\"} " << load(pt.requestsEnded[i]) << "\n";
    ss << "\n# HELP mmo_party_notices_total Party notices decided by the world server, split by code\n# TYPE mmo_party_notices_total counter\n";
    for (int i = 0; i < static_cast<int>(PartyNotice::COUNT); ++i)
        ss << "mmo_party_notices_total{code=\"" << PartyNoticeName(static_cast<PartyNotice>(i)) << "\"} " << load(pt.notices[i]) << "\n";
    ss << "\n";
    MetricsText::WriteCounter(ss, "mmo_party_info_sent_total", "Party lists delivered, counted per connected member", load(pt.infoSent));
    MetricsText::WriteCounter(ss, "mmo_party_member_sent_total", "Party member rows delivered, counted per connected member", load(pt.memberSent));
    MetricsText::WriteCounter(ss, "mmo_party_ask_sent_total", "Party asks sent (should equal the sum of mmo_party_requests_total)", load(pt.askSent));
    MetricsText::WriteCounter(ss, "mmo_party_ask_end_sent_total", "Party ask ends sent (expired or cancelled requests)", load(pt.askEndSent));
}
