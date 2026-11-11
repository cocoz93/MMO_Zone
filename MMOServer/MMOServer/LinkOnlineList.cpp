#include "LinkOnlineList.h"

#include <Identity/CharacterName.h>
#include <cstring>

#include "Player.h"
#include "SessionHub.h"

static_assert(LINK_NAME_CHARS == CHAR_NAME_MAX_LEN, "이름 칸 크기가 어긋났다");

void LinkOnlineList::Fill(LinkOnlineEntry& out, const CPlayer& player)
{
    out.characterId = ToInt(player._characterId);
    out.accountId = ToInt(player._accountId);
    out.level = player._level;
    out.job = player._job;
    out.displayChar = player._displayChar;
    out.colorIndex = player._colorIndex;
    std::memcpy(out.name, player._name, sizeof(out.name));
}

int LinkOnlineList::Send(CLinkConnector& connector, const CSessionHub& sessionHub)
{
    // pack 필수 — 없으면 entries 가 8바이트 경계로 밀려 머리와 첫 줄 사이에 빈 2바이트가 낀다
#pragma pack(push, 1)
    struct OnlineListPacket
    {
        LINK_ZONE_ONLINE_LIST head;
        LinkOnlineEntry entries[LINK_ONLINE_LIST_MAX];
    };
#pragma pack(pop)
    static_assert(sizeof(OnlineListPacket) == sizeof(LINK_ZONE_ONLINE_LIST) + sizeof(LinkOnlineEntry) * LINK_ONLINE_LIST_MAX, "명단 패킷에 빈 칸이 끼었다");
    OnlineListPacket packet{};
    int total = 0;

    const auto flush = [&]()
    {
        const int size = static_cast<int>(sizeof(packet.head) + sizeof(LinkOnlineEntry) * packet.head.count);
        packet.head.header.size = static_cast<uint16_t>(size);
        packet.head.header.type = static_cast<MsgType>(LINK_ZONE_ONLINE_LIST::TYPE);
        connector.SendBytes(&packet, size);
        packet.head.count = 0;
    };

    sessionHub.ForEachPlayer([&](CPlayer* player)
    {
        Fill(packet.entries[packet.head.count++], *player);
        ++total;
        if (packet.head.count == LINK_ONLINE_LIST_MAX)
            flush();
    });
    if (packet.head.count > 0)
        flush();
    return total;
}
