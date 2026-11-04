#include "EnterGate.h"

#include <Identity/SecureRandom.h>
#include <chrono>

static_assert(CEnterGate::CODE_BYTES == SecureRandom::CODE_WIRE_BYTES, "입장 번호 칸 크기가 어긋났다");

namespace
{
    std::string KeyOf(const char (&code)[CEnterGate::CODE_BYTES])
    {
        return std::string(code, SecureRandom::CODE_RANDOM_BYTES);
    }
}

int64_t CEnterGate::NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool CEnterGate::IsEmptyCode(const char (&code)[CODE_BYTES])
{
    return SecureRandom::ClassifyCode(code) == SecureRandom::CodeShape::Empty;
}

void CEnterGate::Erase(EntryMap::iterator it)
{
    if (it->second.live)
        --_liveCount;
    _codeByAccount.erase(ToInt(it->second.accountId));
    _byCode.erase(it);
}

bool CEnterGate::AddNotice(const char (&code)[CODE_BYTES], CharacterId characterId, AccountId accountId,
                           int onlineCount, int maxClients, int64_t nowMs)
{
    Sweep(nowMs);

    // 같은 계정의 앞 번호 — 정원을 보기 전에 찾아 두고, 받아들일 때만 지운다
    EntryMap::iterator previous = _byCode.end();
    const auto mine = _codeByAccount.find(ToInt(accountId));
    if (mine != _codeByAccount.end())
        previous = _byCode.find(mine->second);

    const int replaced = (previous != _byCode.end() && previous->second.live) ? 1 : 0;
    if (onlineCount + _liveCount - replaced >= maxClients)
        return false;

    if (previous != _byCode.end())
        Erase(previous);

    const std::string key = KeyOf(code);
    _byCode[key] = Entry{ characterId, accountId, nowMs + _ttlMs, true };
    _codeByAccount[ToInt(accountId)] = key;
    ++_liveCount;
    return true;
}

EnterCodeResult CEnterGate::Consume(const char (&code)[CODE_BYTES], int64_t nowMs, Ticket& out)
{
    if (SecureRandom::ClassifyCode(code) != SecureRandom::CodeShape::Valid)
        return EnterCodeResult::BadSize;

    const auto it = _byCode.find(KeyOf(code));
    if (it == _byCode.end())
        return EnterCodeResult::NotFound;

    if (nowMs >= it->second.expireMs)
        return EnterCodeResult::Expired;   // 지우지 않는다 — 보관이 끝날 때까지 몇 번이고 Expired 다

    out.characterId = it->second.characterId;
    out.accountId   = it->second.accountId;
    Erase(it);
    return EnterCodeResult::Ok;
}

void CEnterGate::Sweep(int64_t nowMs)
{
    const int64_t keepMs = int64_t{ EXPIRED_KEEP_SEC } * 1000;
    for (auto it = _byCode.begin(); it != _byCode.end(); )
    {
        Entry& entry = it->second;
        if (entry.live && nowMs >= entry.expireMs)
        {
            entry.live = false;
            --_liveCount;
        }

        if (nowMs >= entry.expireMs + keepMs)
        {
            const auto next = std::next(it);
            Erase(it);
            it = next;
        }
        else
        {
            ++it;
        }
    }
}
