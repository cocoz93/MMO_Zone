#include "RoomLockoutBlob.h"

#include <chrono>

namespace RoomLockoutBlob
{
namespace
{
    void PutU32(uint8_t* p, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            p[i] = static_cast<uint8_t>(v >> (8 * i));
    }

    void PutU64(uint8_t* p, uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            p[i] = static_cast<uint8_t>(v >> (8 * i));
    }

    uint32_t GetU32(const uint8_t* p)
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(p[i]) << (8 * i);
        return v;
    }

    uint64_t GetU64(const uint8_t* p)
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(p[i]) << (8 * i);
        return v;
    }
}

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

size_t PackLockouts(const Lockout* items, int n, uint8_t* out, size_t cap)
{
    if (n <= 0)
        return 0;
    const size_t need = HEADER_BYTES + static_cast<size_t>(n) * ENTRY_BYTES;
    if (need > cap || n > 255)
        return 0;
    out[0] = VERSION;
    out[1] = static_cast<uint8_t>(n);
    uint8_t* p = out + HEADER_BYTES;
    for (int i = 0; i < n; ++i, p += ENTRY_BYTES)
    {
        PutU32(p, static_cast<uint32_t>(items[i].mapId));
        PutU64(p + 4, static_cast<uint64_t>(items[i].untilMs));
    }
    return need;
}

Unpack UnpackLockouts(const uint8_t* data, size_t len, Lockout* out, int maxN, int& n)
{
    n = 0;
    if (len == 0)
        return Unpack::EMPTY;
    if (len < HEADER_BYTES || data[0] != VERSION)
        return Unpack::BAD;
    const int count = data[1];
    if (count == 0 || count > maxN || len != HEADER_BYTES + static_cast<size_t>(count) * ENTRY_BYTES)
        return Unpack::BAD;
    const uint8_t* p = data + HEADER_BYTES;
    for (int i = 0; i < count; ++i, p += ENTRY_BYTES)
    {
        out[i].mapId   = static_cast<int32_t>(GetU32(p));
        out[i].untilMs = static_cast<int64_t>(GetU64(p + 4));
    }
    n = count;
    return Unpack::OK;
}
}
