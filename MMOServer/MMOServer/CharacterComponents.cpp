#include "CharacterComponents.h"

#include <cassert>

// ── 체력 컴포넌트 ──

bool CHealthComponent::TakeDamage(int32_t damage, uint64_t frame)
{
    _hp -= damage;
    if (_hp < 0)
        _hp = 0;   // 와이어로 음수 HP가 나가면 클라 막대가 뒤집힌다

    if (_hp != 0)
        return false;

    _isDead     = true;
    _deathFrame = frame;
    return true;
}

void CHealthComponent::SpendHp(int32_t cost)
{
    assert(_hp - cost >= 1 && "자기 스킬로는 안 죽는다 — 부르는 쪽 앞선 검사(LOW_HP)가 막았어야 한다");
    _hp -= cost;
}

void CHealthComponent::Heal(int32_t amount)
{
    const int32_t healed = _hp + amount;
    _hp = (healed < _maxHp) ? healed : _maxHp;
}

void CHealthComponent::FillToMax()
{
    _hp = _maxHp;
}

void CHealthComponent::ClampToMax()
{
    if (_hp > _maxHp)
        _hp = _maxHp;
}

void CHealthComponent::RestoreHp(int32_t saved)
{
    _hp = (saved <= 0 || saved > _maxHp) ? _maxHp : saved;
}

void CHealthComponent::Revive()
{
    _hp         = _maxHp;
    _isDead     = false;
    _deathFrame = 0;
}

// ── 공격 컴포넌트 — 마력 ──
#if USE_SKILL

void CAttackComponent::SpendMp(int32_t cost)
{
    assert(_mp >= cost && "마력이 모자란 시전 — 부르는 쪽 앞선 검사(LOW_MP)가 막았어야 한다");
    _mp -= cost;
}

void CAttackComponent::GainMp(int32_t amount)
{
    const int32_t filled = _mp + amount;
    _mp = (filled < _maxMp) ? filled : _maxMp;
}

void CAttackComponent::FillMpToMax()
{
    _mp = _maxMp;
}

void CAttackComponent::ClampMpToMax()
{
    if (_mp > _maxMp)
        _mp = _maxMp;
}

void CAttackComponent::RestoreMp(int32_t saved)
{
    _mp = (saved < 0 || saved > _maxMp) ? _maxMp : saved;
}

#endif // USE_SKILL

// ── 효과 컴포넌트 ──
#if USE_COMBAT

bool CBuffComponent::AnyActive() const
{
    for (const BuffSlot& slot : _slots)
    {
        if (slot.IsActive())
            return true;
    }
    return false;
}

#endif // USE_COMBAT