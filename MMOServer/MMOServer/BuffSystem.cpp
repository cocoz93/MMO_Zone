#include "BuffSystem.h"

#if USE_COMBAT

#include <algorithm>   // remove — 목록에서 빼기
#include <cassert>

#include "Character.h"     // 효과 컴포넌트 자리(Buffs)
#include "CharacterComponents.h" // 효과 컴포넌트(CBuffComponent) — 걸린 버프 목록
#include "StatSystem.h"    // 바뀐 효과를 스탯에 반영
#include "GameMonitor.h"   // 걸림·끝남·게이지 지표
#include "GameDataFiles.h" // 버프 표 — 지속과 지표 칸

CBuffSystem::CBuffSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor, CStatSystem& statSystem, IBuffEvents& events)
    : _tables(tables)
    , _gameMonitor(gameMonitor)
    , _statSystem(statSystem)
    , _events(events)
{
}

namespace
{
    // 효과 컴포넌트 꺼내기 — 효과가 걸리는 것은 사람·몬스터뿐이라 늘 있다(거는 길 = 스킬 효과의 시전자·대상 · 입장 복원 · 죽음 · 퇴장).
    //   효과 목록(_buffed)에 든 것도 Apply·Restore 가 넣은 것이라 같다. 컴포넌트 없는 것(NPC)이 여기 오면 거는 쪽의 잘못이다.
    CBuffComponent& BuffsOf(CCharacter* c)
    {
        assert(c->Buffs() != nullptr && "효과 컴포넌트가 없는 것에 효과를 다룸 — 거는 쪽이 대상을 잘못 골랐다");
        return *c->Buffs();
    }
}

// 순서가 뜻을 갖지 않는 목록이라 remove 로 당긴다. 없으면 아무 일 없음 — 플래그와 목록이 어긋나는 길이 나중에 생겨도 조용히 새지 않게
//   (RemoveFromDeadActors 와 같은 약속).
void CBuffSystem::RemoveFromList(CCharacter* c)
{
    if (_buffed.empty())
        return;
    _buffed.erase(std::remove(_buffed.begin(), _buffed.end(), c), _buffed.end());
}

void CBuffSystem::Apply(CCharacter* target, int32_t buffId, uint64_t frame)
{
    const BuffDef* def = _tables.Buffs().Find(buffId);
    assert(def != nullptr && "표에 없는 버프 — 거는 쪽의 표 읽기(스킬 표 ⑸)가 번호를 대조했다");
    if (def == nullptr)
        return;

    CBuffComponent& buffs = BuffsOf(target);
    auto& m = _gameMonitor._buff;
    const uint64_t expireFrame = frame + def->durationFrames;

    if (ActiveBuff* active = buffs.Find(buffId))
    {
        m._refreshed[def->index].Inc();   // 되감기 — 같은 번호를 다시 걸면 시간만 다시
        active->expireFrame = expireFrame;
    }
    else
    {
        if (!buffs.AnyActive())   // 첫 버프가 걸린다 — 효과 목록에 들어온다(걸린 버프 목록에 넣기 전에 봐야 한다)
            _buffed.push_back(target);
        m._applied[def->index].Inc();
        m._active.Inc();
        buffs._activeBuffs.push_back(ActiveBuff{ buffId, expireFrame });
    }

    RecalcAndNotifyMotion(target);   // 바탕 + 장비 뒤에 수정 항목 — 체력·마력은 최대치만 맞춘다
    _events.OnBuffChanged(target, buffId);
}

void CBuffSystem::RecalcAndNotifyMotion(CCharacter* target)
{
    const int32_t speedBefore  = target->_speed;
    const bool    rootedBefore = target->_rooted;
    _statSystem.RecalcStats(target, false);
    if (target->_speed != speedBefore)
        _events.OnSpeedChanged(target);
    if (target->_rooted && !rootedBefore)
        _events.OnRooted(target);
}

void CBuffSystem::Tick(uint64_t frame)
{
    if (_buffed.empty())
        return;

    for (size_t i = 0; i < _buffed.size(); )
    {
        CCharacter* c = _buffed[i];
        CBuffComponent& buffs = BuffsOf(c);
        std::vector<ActiveBuff>& list = buffs._activeBuffs;
        assert(list.size() <= static_cast<size_t>(MAX_BUFF_ROWS) && "표에 있는 번호만 하나씩 걸린다(Buff.h MAX_BUFF_ROWS)");
        int32_t expired[MAX_BUFF_ROWS];
        int nExpired = 0;

        // 뒤엣것과 바꿔치기로 제거 — 순서가 뜻을 갖지 않는 목록이다. k 는 안 올린다(방금 당겨온 것을 아직 안 봤다)
        for (size_t k = 0; k < list.size(); )
        {
            if (frame < list[k].expireFrame)
            {
                ++k;
                continue;
            }
            expired[nExpired++] = list[k].buffId;
            list[k] = list.back();
            list.pop_back();
            _gameMonitor._buff._ended[static_cast<int>(BuffEnd::EXPIRED)].Inc();
            _gameMonitor._buff._active.Add(-1);
        }

        if (nExpired == 0)
        {
            ++i;
            continue;
        }

        // 스탯을 먼저 다시 세운 뒤 알린다 — 받는 쪽이 보내는 S2C_MY_STATS 가 풀린 값이어야 한다
        RecalcAndNotifyMotion(c);
        for (int k = 0; k < nExpired; ++k)
            _events.OnBuffChanged(c, expired[k]);

        // 마지막 버프가 풀렸으면 목록에서 — 뒤엣것과 바꿔치기로 O(1) 제거, i 는 안 올린다(방금 당겨온 것을 아직 안 봤다). TickRespawn 꼴
        if (!buffs.AnyActive())
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
    CBuffComponent& buffs = BuffsOf(target);
    std::vector<ActiveBuff>& list = buffs._activeBuffs;
    assert(list.size() <= static_cast<size_t>(MAX_BUFF_ROWS) && "표에 있는 번호만 하나씩 걸린다(Buff.h MAX_BUFF_ROWS)");
    int32_t cleared[MAX_BUFF_ROWS];
    int nCleared = 0;
    for (const ActiveBuff& active : list)
    {
        cleared[nCleared++] = active.buffId;
        _gameMonitor._buff._ended[static_cast<int>(BuffEnd::DEATH)].Inc();
        _gameMonitor._buff._active.Add(-1);
    }
    list.clear();

    RemoveFromList(target);   // 걸린 것이 없었어도 목록에 남아 있을 길을 막는다(위 RemoveFromList 주석)
    if (nCleared == 0)
        return;

    RecalcAndNotifyMotion(target);
    for (int k = 0; k < nCleared; ++k)
        _events.OnBuffChanged(target, cleared[k]);
}

bool CBuffSystem::Restore(CCharacter* target, const BuffDef& def, uint64_t remainFrames, uint64_t frame)
{
    assert(remainFrames > 0 && "남은 틱 0 은 이미 끝난 항목이다 — 호출부가 지난 항목을 거른다");
    CBuffComponent& buffs = BuffsOf(target);
    if (buffs.Find(def.buffId) != nullptr)
        return false;

    if (!buffs.AnyActive())   // 첫 버프가 걸린다 — 효과 목록에 들어온다(걸린 버프 목록에 넣기 전에 봐야 한다 — Apply 와 같은 순서)
        _buffed.push_back(target);
    _gameMonitor._buff._restored[def.index].Inc();
    _gameMonitor._buff._active.Inc();

    buffs._activeBuffs.push_back(ActiveBuff{ def.buffId, frame + remainFrames });
    return true;   // 스탯·알림은 호출부 몫(헤더 주석 ⑴)
}

void CBuffSystem::EndAllAsLeave(CCharacter* c)
{
    CBuffComponent& buffs = BuffsOf(c);
    for (size_t k = 0; k < buffs._activeBuffs.size(); ++k)
    {
        _gameMonitor._buff._ended[static_cast<int>(BuffEnd::LEAVE)].Inc();
        _gameMonitor._buff._active.Add(-1);
    }
    buffs._activeBuffs.clear();
}

void CBuffSystem::Forget(CCharacter* target)
{
    EndAllAsLeave(target);
    RemoveFromList(target);   // 알림·재계산 없음 — 곧 해제되고 받을 세션도 없다
}

void CBuffSystem::ForgetZone(int32_t zoneId)
{
    // 뒤엣것과 바꿔치기로 O(1) 제거 — 바꿔 온 것을 아직 안 봤으니 i 를 안 올린다(Tick 과 같은 꼴). 목록이 비어 있으면(보통) 바로 끝난다.
    for (size_t i = 0; i < _buffed.size(); )
    {
        CCharacter* c = _buffed[i];
        if (c->_zoneId != zoneId)
        {
            ++i;
            continue;
        }
        EndAllAsLeave(c);
        _buffed[i] = _buffed.back();
        _buffed.pop_back();
    }

#ifndef NDEBUG
    for (const CCharacter* c : _buffed)
        assert(c->_zoneId != zoneId && "존을 지운 뒤 효과 목록에 그 존의 캐릭터가 남았다 — 다음 틱이 해제된 메모리를 밟는다");
#endif
}

#endif // USE_COMBAT
