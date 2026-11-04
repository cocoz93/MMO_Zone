#include "CharSessionTable.h"

void CCharSessionTable::Add(SessionId sessionId)
{
    _entries[ToInt(sessionId)] = Entry{};
}

void CCharSessionTable::Remove(SessionId sessionId)
{
    const auto it = _entries.find(ToInt(sessionId));
    if (it == _entries.end())
        return;

    // 밀려난 세션이면 계정 칸은 이미 새 세션 것이다 — 그쪽을 건드리면 안 된다
    const auto owner = _byAccount.find(ToInt(it->second.account));
    if (owner != _byAccount.end() && owner->second == sessionId)
        _byAccount.erase(owner);
    _entries.erase(it);
}

CCharSessionTable::Entry* CCharSessionTable::Find(SessionId sessionId)
{
    const auto it = _entries.find(ToInt(sessionId));
    return it == _entries.end() ? nullptr : &it->second;
}

bool CCharSessionTable::BeginLogin(SessionId sessionId)
{
    Entry* entry = Find(sessionId);
    if (entry == nullptr || entry->stage != Stage::Fresh)
        return false;
    entry->stage = Stage::Verifying;
    return true;
}

void CCharSessionTable::FailLogin(SessionId sessionId)
{
    Entry* entry = Find(sessionId);
    if (entry != nullptr && entry->stage == Stage::Verifying)
        entry->stage = Stage::Fresh;
}

SessionId CCharSessionTable::CompleteLogin(SessionId sessionId, AccountId accountId)
{
    Entry* entry = Find(sessionId);
    if (entry == nullptr)
        return NO_SESSION;

    SessionId previous = NO_SESSION;
    const auto owner = _byAccount.find(ToInt(accountId));
    if (owner != _byAccount.end() && owner->second != sessionId)
    {
        previous = owner->second;
        Entry* old = Find(previous);
        if (old != nullptr)
            old->stage = Stage::Replaced;
    }

    entry->stage = Stage::LoggedIn;
    entry->account = accountId;
    _byAccount[ToInt(accountId)] = sessionId;
    return previous;
}

bool CCharSessionTable::BeginRequest(SessionId sessionId)
{
    Entry* entry = Find(sessionId);
    if (entry == nullptr || entry->busy)
        return false;
    entry->busy = true;
    return true;
}

void CCharSessionTable::EndRequest(SessionId sessionId)
{
    Entry* entry = Find(sessionId);
    if (entry != nullptr)
        entry->busy = false;
}
