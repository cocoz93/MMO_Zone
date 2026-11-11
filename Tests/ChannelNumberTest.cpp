// 보이는 채널 번호 변환 · 월드 서버 채널 표 시험 — 서버를 안 띄운다
#include <Identity/ChannelNumber.h>
#include <vector>

#include "ChannelTable.h"
#include "TestHelp.h"

using TestHelp::Check;

namespace
{
    void TestNumber()
    {
        using namespace ChannelNumber;
        Check(ToVisible(1, 100, 0) == 1, "존 1 채널 0 = 1");
        Check(ToVisible(1, 100, 99) == 100, "존 1 채널 99 = 100");
        Check(ToVisible(2, 100, 0) == 101, "존 2 채널 0 = 101");
        Check(ToVisible(3, 50, 7) == 108, "구간 50 존 3 채널 7 = 108");

        Place place;
        Check(FromVisible(1, 100, place) && place.zoneServerId == 1 && place.channelIndex == 0, "1 → 존 1 채널 0");
        Check(FromVisible(100, 100, place) && place.zoneServerId == 1 && place.channelIndex == 99, "100 → 존 1 채널 99");
        Check(FromVisible(101, 100, place) && place.zoneServerId == 2 && place.channelIndex == 0, "101 → 존 2 채널 0");

        Place untouched{ 7, 8 };
        Check(!FromVisible(0, 100, untouched) && untouched.zoneServerId == 7 && untouched.channelIndex == 8, "0 은 거절 · 그대로");
        Check(!FromVisible(-5, 100, untouched) && untouched.zoneServerId == 7, "음수 거절");
        Check(!FromVisible(5, 0, untouched) && untouched.zoneServerId == 7, "구간 0 거절");

        // 왕복 전수 — 구간 셋 × 존 서버 넷 × 구간 안 채널 전부
        int bad = 0, total = 0;
        for (int block : { 1, 100, 200 })
            for (int zone = 1; zone <= 4; ++zone)
                for (int channel = 0; channel < block; ++channel)
                {
                    Place out;
                    ++total;
                    if (!FromVisible(ToVisible(zone, block, channel), block, out) || out.zoneServerId != zone || out.channelIndex != channel)
                        ++bad;
                }
        Check(bad == 0 && total == 4 * (1 + 100 + 200), "왕복 전수 — 틀린 수 0 · 돈 수 1204");
    }

    std::vector<LinkZoneChannelRow> Rows(std::initializer_list<std::pair<int, int>> list)
    {
        std::vector<LinkZoneChannelRow> rows;
        for (const auto& [channel, players] : list)
        {
            LinkZoneChannelRow row{};
            row.channel = channel;
            row.players = static_cast<uint16_t>(players);
            rows.push_back(row);
        }
        return rows;
    }

    void TestTable()
    {
        CChannelTable table;
        LinkChannelListRow out[8]{};
        Check(table.RowCount() == 0 && table.ListOf(1, out, 8) == 0, "처음엔 비어 있음");

        auto a = Rows({ { 1, 5 }, { 0, 3 } });
        auto b = Rows({ { 0, 9 } });
        table.Update(ZoneServerId{ 2 }, 1, b.data(), static_cast<int>(b.size()));
        table.Update(ZoneServerId{ 1 }, 1, a.data(), static_cast<int>(a.size()));
        Check(table.RowCount() == 3, "줄 수 3");
        Check(table.ListOf(1, out, 8) == 3, "목록 3줄");
        Check(out[0].zoneServerId == 1 && out[0].channel == 0 && out[0].players == 3, "첫 줄 = 존 1 채널 0");
        Check(out[1].zoneServerId == 1 && out[1].channel == 1 && out[1].players == 5, "둘째 = 존 1 채널 1");
        Check(out[2].zoneServerId == 2 && out[2].channel == 0 && out[2].players == 9, "셋째 = 존 2 채널 0");
        Check(table.ListOf(1, out, 2) == 2, "상한까지만");
        Check(table.ListOf(7, out, 8) == 0, "다른 맵은 0줄");

        auto c = Rows({ { 0, 1 } });
        table.Update(ZoneServerId{ 1 }, 1, c.data(), static_cast<int>(c.size()));
        Check(table.RowCount() == 2 && table.ListOf(1, out, 8) == 2 && out[0].players == 1, "갈아 끼우면 앞 줄이 사라짐");

        auto d = Rows({ { 0, 4 } });
        table.Update(ZoneServerId{ 1 }, 2, d.data(), static_cast<int>(d.size()));
        Check(table.RowCount() == 3, "다른 맵 줄이 더해짐");
        table.RemoveZone(ZoneServerId{ 1 });
        Check(table.RowCount() == 1 && table.ListOf(2, out, 8) == 0, "존 서버를 지우면 모든 맵에서 빠짐");
        Check(table.ListOf(1, out, 8) == 1 && out[0].zoneServerId == 2, "남은 존 서버 줄은 그대로");
        table.RemoveZone(ZoneServerId{ 9 });
        Check(table.RowCount() == 1, "없는 존 서버를 지워도 그대로");
    }
}

int main()
{
    TestNumber();
    TestTable();
    return TestHelp::Finish("channel_test");
}
