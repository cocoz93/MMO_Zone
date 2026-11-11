#pragma once
// ==========================================================================
// CChannelTable — 월드 서버의 채널 표: 맵별 · 존 서버별 채널 줄
//
//   존 서버가 바뀐 맵을 통째로 알리면(3034) 그 맵 · 그 존 서버 줄을 갈아 끼운다.
//   존 서버 연결이 끊기면 그 존 서버 줄을 전부 지운다 — 꺼진 서버의 채널을 목록에 내지 않게.
//   플레이어를 모른다. 로직 스레드 단독이라 락이 없다.
// ==========================================================================

#include <cstdint>
#include <map>
#include <vector>

#include <Identity/GameIds.h>
#include <Protocol/ServerLinkProtocol.h>

class CChannelTable
{
public:
    void Update(ZoneServerId zone, int32_t mapId, const LinkZoneChannelRow* rows, int count);
    void RemoveZone(ZoneServerId zone);

    // 그 맵의 줄을 존 서버 번호 → 채널 번호 차례로 채운다. 채운 수를 돌려준다
    int ListOf(int32_t mapId, LinkChannelListRow* out, int maxRows) const;

    int RowCount() const { return _rowCount; }

private:
    using ZoneRows = std::map<int32_t, std::vector<LinkZoneChannelRow>>;   // 존 서버 번호 → 줄

    std::map<int32_t, ZoneRows> _maps;   // 맵 번호 → 존 서버별 줄
    int _rowCount = 0;
};
