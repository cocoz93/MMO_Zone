#include "CooldownBuffBlob.h"

namespace CooldownBuffBlob
{
namespace
{
    // little-endian 고정 — 메모리 복사(memcpy)로 쓰면 빌드 대상의 바이트 순서를 따라가 DB 값의 뜻이 기계마다 달라진다.
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

    // 헤더 검사 — 둘 다 같은 규칙. 통과하면 개수를 돌려준다.
    //   길이 0 은 EMPTY(정상). 헤더가 있는데 개수가 0 이면 BAD — 쓰는 쪽(Pack)은 비면 헤더도 안 쓰므로 이 꼴은 나올 수 없다.
    Unpack CheckHeader(const uint8_t* data, size_t len, size_t entryBytes, int maxN, int& n)
    {
        n = 0;
        if (len == 0)
            return Unpack::EMPTY;
        if (len < HEADER_BYTES || data[0] != FORMAT_VERSION)
            return Unpack::BAD;
        const int count = data[1];
        if (count == 0 || count > maxN || len != HEADER_BYTES + static_cast<size_t>(count) * entryBytes)
            return Unpack::BAD;
        n = count;
        return Unpack::OK;
    }
}

size_t PackCooldowns(const Cooldown* items, int n, uint8_t* out, size_t cap)
{
    if (n <= 0 || n > 255)
        return 0;
    const size_t need = HEADER_BYTES + static_cast<size_t>(n) * COOLDOWN_BYTES;
    if (need > cap)
        return 0;

    out[0] = FORMAT_VERSION;
    out[1] = static_cast<uint8_t>(n);
    uint8_t* p = out + HEADER_BYTES;
    for (int i = 0; i < n; ++i, p += COOLDOWN_BYTES)
    {
        p[0] = items[i].skillId;
        PutU64(p + 1, static_cast<uint64_t>(items[i].readyAtMs));
    }
    return need;
}

size_t PackBuffs(const Buff* items, int n, uint8_t* out, size_t cap)
{
    if (n <= 0 || n > 255)
        return 0;
    const size_t need = HEADER_BYTES + static_cast<size_t>(n) * BUFF_BYTES;
    if (need > cap)
        return 0;

    out[0] = FORMAT_VERSION;
    out[1] = static_cast<uint8_t>(n);
    uint8_t* p = out + HEADER_BYTES;
    for (int i = 0; i < n; ++i, p += BUFF_BYTES)
    {
        p[0] = items[i].kind;
        PutU32(p + 1, static_cast<uint32_t>(items[i].value));
        PutU64(p + 5, static_cast<uint64_t>(items[i].expireAtMs));
    }
    return need;
}

Unpack UnpackCooldowns(const uint8_t* data, size_t len, Cooldown* out, int maxN, int& n)
{
    const Unpack r = CheckHeader(data, len, COOLDOWN_BYTES, maxN, n);
    if (r != Unpack::OK)
        return r;

    const uint8_t* p = data + HEADER_BYTES;
    for (int i = 0; i < n; ++i, p += COOLDOWN_BYTES)
    {
        out[i].skillId   = p[0];
        out[i].readyAtMs = static_cast<int64_t>(GetU64(p + 1));
    }
    return Unpack::OK;
}

Unpack UnpackBuffs(const uint8_t* data, size_t len, Buff* out, int maxN, int& n)
{
    const Unpack r = CheckHeader(data, len, BUFF_BYTES, maxN, n);
    if (r != Unpack::OK)
        return r;

    const uint8_t* p = data + HEADER_BYTES;
    for (int i = 0; i < n; ++i, p += BUFF_BYTES)
    {
        out[i].kind       = p[0];
        out[i].value      = static_cast<int32_t>(GetU32(p + 1));
        out[i].expireAtMs = static_cast<int64_t>(GetU64(p + 5));
    }
    return Unpack::OK;
}
}
