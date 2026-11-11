#pragma once
// ==========================================================================
// 보이는 채널 번호 ↔ (존 서버 · 채널) — 존 서버가 쓰고 단위 시험이 같은 식을 본다
//
//   보이는 번호 = (존 서버 번호 − 1) × 구간 + 채널 번호 + 1 (world-server-plan.md 2절 13번)
//   구간(ChannelBlock)은 존 서버 한 대가 쓰는 번호 수다. 모든 존 서버가 같은 값이어야 번호가 안 겹친다.
//   방 맵은 번호를 안 붙인다 — 파티마다 채널 하나라 번호에 뜻이 없다.
// ==========================================================================

#include <cstdint>

namespace ChannelNumber
{
    // 상한 200 — 맵 하나의 채널 줄이 서버 간 패킷 한 장에 들어야 한다(LINK_ZONE_CHANNELS_MAX)
    inline constexpr int32_t BLOCK_MIN = 1;
    inline constexpr int32_t BLOCK_MAX = 200;

    struct Place
    {
        int32_t zoneServerId = 0;
        int32_t channelIndex = 0;
    };

    inline int32_t ToVisible(int32_t zoneServerId, int32_t block, int32_t channelIndex)
    {
        return (zoneServerId - 1) * block + channelIndex + 1;
    }

    // 0 이하이거나 구간이 틀렸으면 false 이고 out 은 그대로다
    inline bool FromVisible(int32_t visible, int32_t block, Place& out)
    {
        if (visible <= 0 || block < BLOCK_MIN)
            return false;
        out.zoneServerId = (visible - 1) / block + 1;
        out.channelIndex = (visible - 1) % block;
        return true;
    }
}
