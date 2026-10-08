#include "StatSystem.h"

#if USE_COMBAT

#include "Player.h"          // 레벨·경험치·스탯 필드 · 착용 칸(가방)
#include "GameDataFiles.h"   // 레벨 표 · 아이템 표(무기 몫)
#include "GameMonitor.h"     // 레벨업·보정 지표
#include <Common/ErrorLog.h>

CStatSystem::CStatSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor)
    : _tables(tables)
    , _gameMonitor(gameMonitor)
{
}

// ==========================================================================
// 스탯 — 최대 체력·공격력·방어력을 세우는 한 곳
//   공격력·방어력은 최종값을 필드에 둔다(Character.h [전투 스탯]). 때리는 경로는 필드만 읽는다.
//   바탕은 레벨 표 하나다 — 표는 전투(USE_COMBAT)가 켜지면 늘 읽는다(경험치를 꺼도 1레벨 줄은 바탕 스탯이라 필요).
//   코드 상수(CHARACTER_DEFAULT_*)는 10-06 에 없앴다 — 표와 두 벌이었고, 전투를 끈 빌드는 스탯을 안 보낸다(WriteSnapshot 0).
// ==========================================================================
void CStatSystem::RecalcStats(CPlayer* player, bool refillHp)
{
    const CLevelTable& table = _tables.Level();
    player->_maxHp       = table.MaxHp(player->_level);
    player->_attackPower = table.AttackPower(player->_level);
    player->_defense     = table.Defense(player->_level);

#if USE_EQUIP
    // 착용 칸의 몫 — 칸마다 아이템 표의 attack 을 더한다(지금은 무기 칸 하나).
    //   표에서 사라진 번호(패치로 지운 아이템)는 더하지 않는다 — 불러오기가 이미 건너뛰므로 평소엔 안 닿는다.
    for (int w = 0; w < INVENTORY_WORN_SLOTS; ++w)
    {
        const Item& worn = player->_inventory.At(WornIndexOf(static_cast<EquipSlot>(w)));
        if (worn.itemId == 0)
            continue;
        if (const ItemDef* def = _tables.Item().Find(worn.itemId))
            player->_attackPower += def->attack;
    }
#endif

    // 레벨이 올랐을 때만 채운다. 입장 때는 DB 가 들고 있던 체력이 아직 없으므로(스탯만 맞춘다)
    //   호출부가 정한다 — 지금은 스폰도 만피라 둘 다 true 로 들어온다.
    if (refillHp)
        player->_hp = player->_maxHp;
    else if (player->_hp > player->_maxHp)
        player->_hp = player->_maxHp;   // 표를 낮춘 패치 뒤엔 들고 있던 체력이 최대를 넘을 수 있다
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

    RecalcStats(player, true);   // 오르면 체력을 가득 채운다
    _gameMonitor._exp._levelUps.Inc();
    return true;
}
#endif // USE_EXP

#endif // USE_COMBAT
