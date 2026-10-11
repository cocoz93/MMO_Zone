#include "BuffSystem.h"

#if USE_COMBAT

#include <algorithm>   // remove — 목록에서 빼기
#include <cassert>

#include "Character.h"     // _buffs 칸
#include "StatSystem.h"    // 바뀐 칸을 스탯에 반영
#include "GameMonitor.h"   // 걸림·끝남·게이지 지표

CBuffSystem::CBuffSystem(CGameMonitor& gameMonitor, CStatSystem& statSystem, IBuffEvents& events)
    : _gameMonitor(gameMonitor)
    , _statSystem(statSystem)
    , _events(events)
{
}

bool CBuffSystem::HasAnyActive(const CCharacter* c)
{
    for (const BuffSlot& slot : c->_buffs)
    {
        if (slot.IsActive())
            return true;
    }
    return false;
}

// 순서가 뜻을 갖지 않는 목록이라 remove 로 당긴다. 없으면 아무 일 없음 — 플래그와 목록이 어긋나는 길이 나중에 생겨도 조용히 새지 않게
//   (RemoveFromDeadActors 와 같은 약속).
void CBuffSystem::RemoveFromList(CCharacter* c)
{
    if (_buffed.empty())
        return;
    _buffed.erase(std::remove(_buffed.begin(), _buffed.end(), c), _buffed.end());
}

void CBuffSystem::Apply(CCharacter* target, BuffKind kind, int32_t value, uint64_t durationFrames, uint64_t frame)
{
    assert(durationFrames > 0 && "지속 0 은 비어 있는 칸과 구별이 안 된다 — 표 읽기가 막는다");
    BuffSlot& slot = target->_buffs[static_cast<int>(kind)];
    auto& m = _gameMonitor._buff;

    if (slot.IsActive())
    {
        m._refreshed[static_cast<int>(kind)].Inc();   // 되감기 — 같은 종류는 나중 것이 값·시간을 덮는다
    }
    else
    {
        if (!HasAnyActive(target))   // 첫 칸이 켜진다 — 목록에 들어온다(칸을 채우기 전에 봐야 한다)
            _buffed.push_back(target);
        m._applied[static_cast<int>(kind)].Inc();
        m._active.Inc();
    }

    slot.value       = value;
    slot.expireFrame = frame + durationFrames;

    _statSystem.RecalcStats(target, false);   // 바탕 + 장비 뒤에 배수 — 체력·마력은 최대치만 맞춘다
    _events.OnBuffChanged(target, kind);
}

void CBuffSystem::Tick(uint64_t frame)
{
    if (_buffed.empty())
        return;

    for (size_t i = 0; i < _buffed.size(); )
    {
        CCharacter* c = _buffed[i];
        bool expired[BUFF_KIND_COUNT] = {};
        bool any = false;

        for (int k = 0; k < BUFF_KIND_COUNT; ++k)
        {
            BuffSlot& slot = c->_buffs[k];
            if (!slot.IsActive() || frame < slot.expireFrame)
                continue;
            slot = BuffSlot{};
            _gameMonitor._buff._ended[static_cast<int>(BuffEnd::EXPIRED)].Inc();
            _gameMonitor._buff._active.Add(-1);
            expired[k] = true;
            any = true;
        }

        if (!any)
        {
            ++i;
            continue;
        }

        // 스탯을 먼저 다시 세운 뒤 알린다 — 받는 쪽이 보내는 S2C_MY_STATS 가 풀린 값이어야 한다
        _statSystem.RecalcStats(c, false);
        for (int k = 0; k < BUFF_KIND_COUNT; ++k)
        {
            if (expired[k])
                _events.OnBuffChanged(c, static_cast<BuffKind>(k));
        }

        // 마지막 칸이 꺼졌으면 목록에서 — 뒤엣것과 바꿔치기로 O(1) 제거, i 는 안 올린다(방금 당겨온 것을 아직 안 봤다). TickRespawn 꼴
        if (!HasAnyActive(c))
        {
            _buffed[i] = _buffed.back();
            _buffed.pop_back();
        }
        else
        {
            ++i;
        }
    }
}

void CBuffSystem::ClearAll(CCharacter* target)
{
    bool cleared[BUFF_KIND_COUNT] = {};
    bool any = false;
    for (int k = 0; k < BUFF_KIND_COUNT; ++k)
    {
        BuffSlot& slot = target->_buffs[k];
        if (!slot.IsActive())
            continue;
        slot = BuffSlot{};
        _gameMonitor._buff._ended[static_cast<int>(BuffEnd::DEATH)].Inc();
        _gameMonitor._buff._active.Add(-1);
        cleared[k] = true;
        any = true;
    }

    RemoveFromList(target);   // 칸이 다 비어 있었어도 목록에 남아 있을 길을 막는다(위 RemoveFromList 주석)
    if (!any)
        return;

    _statSystem.RecalcStats(target, false);
    for (int k = 0; k < BUFF_KIND_COUNT; ++k)
    {
        if (cleared[k])
            _events.OnBuffChanged(target, static_cast<BuffKind>(k));
    }
}

void CBuffSystem::Forget(CCharacter* target)
{
    for (int k = 0; k < BUFF_KIND_COUNT; ++k)
    {
        BuffSlot& slot = target->_buffs[k];
        if (!slot.IsActive())
            continue;
        slot = BuffSlot{};
        _gameMonitor._buff._ended[static_cast<int>(BuffEnd::LEAVE)].Inc();
        _gameMonitor._buff._active.Add(-1);
    }
    RemoveFromList(target);   // 알림·재계산 없음 — 곧 해제되고 받을 세션도 없다
}

#endif // USE_COMBAT
