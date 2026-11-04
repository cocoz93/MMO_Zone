#include "AuthTokenLedger.h"

#include <Identity/SecureRandom.h>

static_assert(CAuthTokenLedger::TOKEN_BYTES == SecureRandom::CODE_WIRE_BYTES, "토큰 칸 크기가 어긋났다");

namespace
{
    std::string KeyOf(const char (&token)[CAuthTokenLedger::TOKEN_BYTES])
    {
        return std::string(token, SecureRandom::CODE_RANDOM_BYTES);
    }
}

void CAuthTokenLedger::Issue(AccountId accountId, int64_t nowMs, char (&out)[TOKEN_BYTES])
{
    SecureRandom::MakeCode(out);
    _entries[KeyOf(out)] = Entry{ accountId, nowMs + _ttlMs, true };
    ++_liveCount;
}

TokenCheckResult CAuthTokenLedger::Consume(const char (&token)[TOKEN_BYTES], int64_t nowMs, AccountId& outAccount)
{
    if (SecureRandom::ClassifyCode(token) != SecureRandom::CodeShape::Valid)
        return TokenCheckResult::BadSize;

    const auto it = _entries.find(KeyOf(token));
    if (it == _entries.end())
        return TokenCheckResult::NotFound;

    if (nowMs >= it->second.expireMs)
        return TokenCheckResult::Expired;   // 지우지 않는다 — 보관이 끝날 때까지 몇 번이고 Expired 다

    outAccount = it->second.accountId;
    if (it->second.live)
        --_liveCount;
    _entries.erase(it);
    return TokenCheckResult::Ok;
}

void CAuthTokenLedger::Sweep(int64_t nowMs)
{
    const int64_t keepMs = int64_t{ EXPIRED_KEEP_SEC } * 1000;
    for (auto it = _entries.begin(); it != _entries.end(); )
    {
        Entry& entry = it->second;
        if (entry.live && nowMs >= entry.expireMs)
        {
            entry.live = false;
            --_liveCount;
        }

        if (nowMs >= entry.expireMs + keepMs)
            it = _entries.erase(it);
        else
            ++it;
    }
}
