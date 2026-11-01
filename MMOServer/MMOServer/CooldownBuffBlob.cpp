#include "CooldownBuffBlob.h"

namespace CooldownBuffBlob
{
namespace
{
    // little-endian 고정 — 메모리 복사(memcpy)로 쓰면 빌드 대상의 바이트 순서를 따라가 DB 값의 뜻이 기계마다 달라진다.
    void PutU16(uint8_t* p, uint16_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
    }

    void PutU64(uint8_t* p, uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            p[i] = static_cast<uint8_t>(v >> (8 * i));
    }

    uint16_t GetU16(const uint8_t* p)
    {
        return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
    }

    uint64_t GetU64(const uint8_t* p)
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(p[i]) << (8 * i);
        return v;
    }

    // 헤더 검사 — 둘 다 같은 규칙. 통과하면 개수를 돌려준다.
    //   길이 0(EMPTY)과 버전은 부르는 쪽이 먼저 본다 — 판마다 항목 길이가 달라서 버전을 알아야 entryBytes 가 정해진다.
    //   헤더가 있는데 개수가 0 이면 BAD — 쓰는 쪽(Pack)은 비면 헤더도 안 쓰므로 이 꼴은 나올 수 없다.
    Unpack CheckHeader(const uint8_t* data, size_t len, size_t entryBytes, int maxN, int& n)
    {
        n = 0;
        if (len < HEADER_BYTES)
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

    out[0] = COOLDOWN_VERSION;
    out[1] = static_cast<uint8_t>(n);
    uint8_t* p = out + HEADER_BYTES;
    for (int i = 0; i < n; ++i, p += COOLDOWN_BYTES)
    {
        PutU16(p, items[i].skillId);
        PutU64(p + 2, static_cast<uint64_t>(items[i].readyAtMs));
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

    out[0] = BUFF_VERSION;
    out[1] = static_cast<uint8_t>(n);
    uint8_t* p = out + HEADER_BYTES;
    for (int i = 0; i < n; ++i, p += BUFF_BYTES)
    {
        PutU16(p, items[i].buffId);
        PutU64(p + 2, static_cast<uint64_t>(items[i].expireAtMs));
    }
    return need;
}

Unpack UnpackCooldowns(const uint8_t* data, size_t len, Cooldown* out, int maxN, int& n, bool& outOldFormat)
{
    n = 0;
    outOldFormat = false;
    if (len == 0)
        return Unpack::EMPTY;

    // 판마다 항목 길이·번호 칸 크기만 다르다 — 시각은 둘 다 번호 뒤 8바이트
    size_t idBytes = 0;
    switch (data[0])
    {
    case COOLDOWN_VERSION:    idBytes = 2;                        break;
    case COOLDOWN_VERSION_V1: idBytes = 1; outOldFormat = true;   break;
    default:                  return Unpack::BAD;                 // 모르는 버전
    }
    const size_t entryBytes = idBytes + 8;

    const Unpack r = CheckHeader(data, len, entryBytes, maxN, n);
    if (r != Unpack::OK)
        return r;

    const uint8_t* p = data + HEADER_BYTES;
    for (int i = 0; i < n; ++i, p += entryBytes)
    {
        out[i].skillId   = (idBytes == 2) ? GetU16(p) : p[0];
        out[i].readyAtMs = static_cast<int64_t>(GetU64(p + idBytes));
    }
    return Unpack::OK;
}

Unpack UnpackBuffs(const uint8_t* data, size_t len, Buff* out, int maxN, int& n, bool& outOldFormat)
{
    n = 0;
    outOldFormat = false;
    if (len == 0)
        return Unpack::EMPTY;

    // 판마다 항목 길이가 다르다 — 값이 둘이고 나머지는 모르는 버전
    size_t entryBytes = 0;
    switch (data[0])
    {
    case BUFF_VERSION:    entryBytes = BUFF_BYTES;                             break;
    case BUFF_VERSION_V1: entryBytes = BUFF_BYTES_V1; outOldFormat = true;     break;
    default:              return Unpack::BAD;                                  // 모르는 버전
    }

    const Unpack r = CheckHeader(data, len, entryBytes, maxN, n);
    if (r != Unpack::OK)
        return r;

    const uint8_t* p = data + HEADER_BYTES;
    for (int i = 0; i < n; ++i, p += entryBytes)
    {
        if (outOldFormat)
        {
            // 옛 판 종류 0(공격력 배수)만 있었다 — 그 밖은 표에 없는 번호 0 으로 돌려줘 호출부가 건너뛰게 한다. 값(p+1)은 표 몫이라 버린다
            out[i].buffId     = (p[0] == 0) ? BUFF_ID_OF_V1_ATTACK_MUL : static_cast<uint16_t>(0);
            out[i].expireAtMs = static_cast<int64_t>(GetU64(p + 5));
        }
        else
        {
            out[i].buffId     = GetU16(p);
            out[i].expireAtMs = static_cast<int64_t>(GetU64(p + 2));
        }
    }
    return Unpack::OK;
}
}
