#include "StatSystem.h"

#if USE_COMBAT

#include "Player.h"          // 레벨·경험치·스탯 필드 · 착용 칸(가방)
#include "Monster.h"         // USE_MONSTER — 몬스터 바탕은 표 행(_def). 헤더 안에서 스스로 토글을 본다
#include "MonsterTable.h"    // USE_MONSTER — MonsterDef 의 칸(maxHp·attack·defense)
#include "GameDataFiles.h"   // 레벨 표 · 아이템 표(무기 몫)
#include "GameMonitor.h"     // 레벨업·보정 지표
#include <Common/ErrorLog.h>
#include <cassert>           // 몬스터를 끈 빌드에 몬스터 종류가 오는 일은 없다

CStatSystem::CStatSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor)
    : _tables(tables)
    , _gameMonitor(gameMonitor)
{
}

// ==========================================================================
// 스탯 — 최대 체력·공격력·방어력을 세우는 한 곳
//   공격력·방어력은 최종값을 필드에 둔다(Character.h [전투 스탯]). 때리는 경로는 필드만 읽는다.
//   순서 = 바탕(종류로 갈림) → 장비(플레이어) → 지속 효과 배수(공통). 배수를 맨 뒤에 두는 까닭 — 장착이 이 함수를 다시 부르는데,
//   배수가 앞에 있으면 무기를 바꾸는 순간 효과가 사라진다.
//   플레이어 바탕은 레벨 표 하나다 — 표는 전투(USE_COMBAT)가 켜지면 늘 읽는다(경험치를 꺼도 1레벨 줄은 바탕 스탯이라 필요).
//   코드 상수(CHARACTER_DEFAULT_*)는 10-06 에 없앴다 — 표와 두 벌이었고, 전투를 끈 빌드는 스탯을 안 보낸다(WriteSnapshot 0).
//   몬스터 바탕은 몬스터 표 행(_def) — 생성자(Monster.cpp)가 넣는 것과 같은 복사다(식이 아니라 복사라 두 곳에 둔다, 10-12).
//   효과가 풀릴 때 돌아갈 「바탕」이 여기서 다시 선다 — 생성자 값을 필드에 두고 거기서 곱하면 풀 때 나눌 수밖에 없고, 그건 반올림이 남는다.
// ==========================================================================
void CStatSystem::RecalcStats(CCharacter* character, bool refill)
{
    // ── 1) 바탕 — 종류마다 표가 다르다. 갈래 둘이라 switch 다(하나가 나머지를 삼키지 않게) ──
    switch (character->_kind)
    {
    case CharacterKind::PLAYER:
    {
        CPlayer* player = As<CPlayer>(character);
        const CLevelTable& table = _tables.Level();
        CHealthPart& health = player->_health;
        CAttackPart& attack = player->_attack;
        health.SetMaxHp(table.MaxHp(player->_level));
        attack._attackPower = table.AttackPower(player->_level);
        health.SetDefense(table.Defense(player->_level));
        // GM 모드(운영자 /gm) — 최대 체력·공격력의 바탕만 GM 값으로 바꾼다. 방어력·마력은 레벨 표 그대로,
        //   장비 몫·효과 배수는 아래에서 그 위에 그대로 얹는다(끼우거나 효과가 걸려도 GM 이 안 풀린다).
        if (player->_gmMode)
        {
            health.SetMaxHp(CPlayer::GM_MAX_HP);
            attack._attackPower = CPlayer::GM_ATTACK;
        }
#if USE_SKILL
        attack.SetMaxMp(table.MaxMp(player->_level));   // 장비 몫은 없다 — 마력을 올리는 아이템이 아직 없다
#endif

#if USE_EQUIP
        // 착용 칸의 몫 — 칸마다 아이템 표의 attack 을 더한다(지금은 무기 칸 하나).
        //   표에서 사라진 번호(패치로 지운 아이템)는 더하지 않는다 — 불러오기가 이미 건너뛰므로 평소엔 안 닿는다.
        for (int w = 0; w < INVENTORY_WORN_SLOTS; ++w)
        {
            const Item& worn = player->_inventory.At(WornIndexOf(static_cast<EquipSlot>(w)));
            if (worn.itemId == 0)
                continue;
            if (const ItemDef* def = _tables.Item().Find(worn.itemId))
                attack._attackPower += def->attack;
        }
#endif
        break;
    }

    case CharacterKind::MONSTER:
    {
#if USE_MONSTER
        // 몬스터 표 행 — 생성자가 넣은 것과 같은 값(Monster.cpp 생성자와 서로 가리킨다). 마력은 안 건드린다(몬스터는 마력 0 — 몬스터 스킬은 비용 0 만 허용한다, MonsterSkillTable.h).
        //   보스의 자기 버프(격노)가 이 갈래를 탄다 — 효과가 걸리면 이 바탕에서 다시 세우고 배수를 곱한다(공격력 10 → 20 → 풀리면 10, 10-14 실서버로 확인).
        CMonster* monster = As<CMonster>(character);
        const MonsterDef& def = monster->_def;
        monster->_health.SetMaxHp(def.maxHp);
        monster->_attack._attackPower = def.attack;
        monster->_health.SetDefense(def.defense);
#else
        assert(false && "몬스터를 끈 빌드에 몬스터가 있을 수 없다");
#endif
        break;
    }
    }

    // 공통 꼬리의 부품 — 스탯을 세우는 것은 사람·몬스터뿐이라 부품 셋이 늘 있다(부르는 쪽 = 입장 · 레벨업 · 장비 · GM · 효과 걸림·풀림)
    assert(character->Health() != nullptr && character->Attack() != nullptr && "부품 없는 것의 스탯을 세움");
    CHealthPart& health = *character->Health();
    CAttackPart& attack = *character->Attack();

    // ── 2) 지속 효과 배수 — 바탕 + 장비 "뒤". 칸은 CBuffSystem 만 켜고 끈다(CharacterParts.h CBuffPart) ──
    {
        assert(character->Buffs() != nullptr && "효과 부품 없는 것의 스탯을 세움");
        const BuffSlot& mul = character->Buffs()->_slots[static_cast<int>(BuffKind::ATTACK_MUL)];
        if (mul.IsActive())
            attack._attackPower *= mul.value;
    }

    // ── 3) 체력·마력 맞추기 ──
    // 레벨이 올랐을 때만 채운다. 입장은 false 로 최대치만 세우고, DB 가 들고 있던 체력·마력은 RestoreHpMp 가 넣는다.
    //   효과 걸림·풀림도 false(최대치만 맞춘다 — 효과가 최대 체력을 안 바꿔 지금은 아무 일 없다).
    if (refill)
        health.FillToMax();
    else
        health.ClampToMax();   // 표를 낮춘 패치 뒤엔 들고 있던 체력이 최대를 넘을 수 있다

#if USE_SKILL
    // 마력도 같은 줄 — 레벨업만 채우고, 입장·끼우기·빼기는 최대치만 맞춘다(입장 값은 RestoreHpMp). 몬스터는 둘 다 0 이라 아무 일 없다
    if (refill)
        attack.FillMpToMax();
    else
        attack.ClampMpToMax();
#endif
}

// ==========================================================================
// 입장 — 저장된 체력·마력 넣기
//   최대치는 저장하지 않는다 — 레벨 표·장비로 다시 세우는 값이라, 표를 낮춘 패치·장비 변화 뒤엔 저장값이 최대를 넘을 수 있다.
//   그래서 RecalcStats 가 최대치를 세운 "뒤"에 불러 넘은 몫을 깎는다(EntryHandler.cpp SpawnPlayer 의 순서).
// ==========================================================================
void CStatSystem::RestoreHpMp(CHealthPart& health, CAttackPart& attack, int32_t hp, int32_t mp)
{
    // 체력 — 0 이하는 가득. 음수는 「저장된 적 없음」, 0(죽은 채 저장)은 부르는 쪽이 부활 규칙으로 이미 바꿨다(헤더 주석).
    health.RestoreHp(hp);

#if USE_SKILL
    // 마력 — 음수만 가득. 0 은 다 쓰고 나간 값이라 그대로 둔다(체력과 다르다 — 마력 0 은 살아 있다).
    attack.RestoreMp(mp);
#else
    (void)attack;
    (void)mp;
#endif
}

#if USE_EXP
// ==========================================================================
// 경험치·레벨
//   표(CLevelTable)는 기획 데이터라 파일에서 왔고, 여기서는 읽기만 한다.
//   ★ 누적 기준 — _exp 는 "지금까지 얻은 총합"이고 표의 필요치도 같은 기준이다.
//     레벨 안에서 다시 0부터 세지 않아서 레벨업이 한 줄이고 넘친 경험치가 안 사라진다.
// ==========================================================================

bool CStatSystem::ClampLevelToTable(CPlayer* player)
{
    const CLevelTable& table = _tables.Level();
    const int32_t maxLevel = table.MaxLevel();

    if (player->_level >= 1 && player->_level <= maxLevel)
        return false;

    SLOG_WARN("[EXP] level out of table -> clamp (account={}, level={}, table max={})",
              player->_accountId, player->_level, maxLevel);
    player->_level = (player->_level < 1) ? 1 : maxLevel;
    _gameMonitor._exp._levelClamped.Inc();
    return true;
}

bool CStatSystem::GainExp(CPlayer* player, int64_t amount)
{
    const CLevelTable& table = _tables.Level();
    const int32_t maxLevel = table.MaxLevel();

    // 만렙이면 더 안 쌓는다 — 쌓아 봐야 쓸 데가 없고, 숫자만 끝없이 커진다.
    if (player->_level >= maxLevel)
        return false;

    if (amount > 0)
        player->_exp += amount;

    // ★ 앞의 level < maxLevel 이 없으면 만렙에서 table[maxLevel + 1] 을 읽어 배열 밖이다.
    //   한 번에 여러 레벨이 오를 수 있으므로 if 가 아니라 while 이다.
    const int32_t before = player->_level;
    while (player->_level < maxLevel && player->_exp >= table.RequiredExp(player->_level + 1))
        ++player->_level;

    if (player->_level == before)
        return false;

    // 오르면 체력·마력을 가득 채운다 — 쓰러져 있으면 안 채운다. 방 보상(CRewardHandler::RewardRoom)은 부활을 기다리는 사람에게도 가는데,
    //   채우면 「쓰러졌는데 체력이 있다」가 되고 그 채로 저장되면 「죽은 채 저장 = 체력 0」 규칙(입장 때 부활 판정)이 깨진다. 부활이 채운다
    RecalcStats(player, !player->_health.IsDead());
    _gameMonitor._exp._levelUps.Inc();
    return true;
}
#endif // USE_EXP

#endif // USE_COMBAT
