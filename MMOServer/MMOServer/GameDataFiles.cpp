#include "GameDataFiles.h"

#include <Common/ErrorLog.h>

bool CGameDataFiles::LoadAll(const std::string& dir)
{
    // 표가 늘어나면 여기 형제로 한 줄씩 붙는다. 하나라도 false 면 그 자리에서 끝낸다 —
    //   "레벨 표는 읽었는데 아이템 표는 못 읽은" 반쪽 상태로 서버를 띄우지 않는다.
    // 맵 표는 토글이 없다 — 모든 빌드가 이 표로 맵을 세운다(MapTable.h [토글 없음])
    if (!_map.Load(dir + "MapTable.csv"))
        return false;
    // 포털 표도 토글이 없다 — 맵 표만 대조한다(PortalTable.h [맵 대조])
    if (!_portal.Load(dir + "PortalTable.csv", _map))
        return false;
    // 막힌 칸 표도 토글이 없다 — 맵 표만 대조한다. 놓이는 자리 대조는 뒤에서 표가 다 선 다음(BlockTable.h [자리 대조])
    if (!_block.Load(dir + "BlockTable.csv", _map))
        return false;
#if USE_COMBAT   // 레벨 표는 전투를 따른다(바탕 스탯) — 경험치만 끈 빌드도 읽는다
    if (!_level.Load(dir + "LevelTable.csv"))
        return false;
#endif
#if USE_INVENTORY
    if (!_item.Load(dir + "ItemTable.csv"))
        return false;
#endif
#if USE_SHOP
    // ★ 아이템 표 "뒤" — 상점 줄의 아이템 번호 · 돈 줄인지 · 값을 대조한다
    if (!_shop.Load(dir + "ShopTable.csv", _item))
        return false;
#endif
    // NPC 표는 토글이 없다 — 맵 표 "뒤"(맵 번호·크기) · 상점을 켠 빌드면 상점 표 "뒤"(상점 번호). 포털과 반경이 겹치면 경고만
    if (!_npc.Load(dir + "NpcTable.csv", _map))
        return false;
#if USE_SHOP
    if (!_npc.CheckShops(_shop))
        return false;
#else
    SLOG_INFO("[NpcTable] 상점을 끈 빌드(USE_SHOP 0) — 상점 번호 대조를 건너뜁니다");
#endif
    _npc.WarnPortalOverlap(_portal);
    // ★ 맵·포털·NPC 표 "뒤" — 시작 구역 · 포털 두 끝 · NPC 자리가 막힌 칸이 아닌가
    if (!_block.CheckPlaces(_map, _portal, _npc))
        return false;
#if USE_MONSTER
    if (!_monster.Load(dir + "MonsterTable.csv"))
        return false;
    // ★ 맵 표·몬스터 표 "뒤" — 스폰 구역의 맵 번호·크기와 몬스터 번호를 두 표와 대조한다
    if (!_spawnRegion.Load(dir + "SpawnRegion.csv", _map, _monster))
        return false;
    // ★ 스폰 표 "뒤" — 몬스터가 깔리는 구역이 막힌 칸에 닿지 않나
    if (!_block.CheckSpawnRegions(_map, _spawnRegion))
        return false;
#endif
#if USE_DROP
    // ★ 아이템 표·몬스터 표 "뒤" — 드랍 표의 아이템 번호·개수와 몬스터 번호를 두 표와 대조한다
    if (!_drop.Load(dir + "DropTable.csv", _item, _monster))
        return false;
#endif
#if USE_SKILL
    // ★ 레벨 표 "뒤" — 마력 비용(costMp)을 닿을 수 있는 최대 마력과 대조한다(USE_SKILL 은 USE_COMBAT 을 요구해 레벨 표가 늘 있다)
    if (!_skill.Load(dir + "SkillTable.csv", _level))
        return false;
#endif
#if USE_MONSTER && USE_SKILL
    // ★ 몬스터 표·스킬 표 "뒤" — 규칙의 몬스터 번호·스킬 번호·스킬의 쓰는 쪽(user)·비용·닿는 거리를 두 표와 대조한다.
    //   맵 표·스폰 표 "뒤"이기도 하다 — 되살리기 줄의 「되살릴 몬스터가 시전자와 같은 맵에 깔리나」를 본다
    if (!_monsterSkill.Load(dir + "MonsterSkill.csv", _monster, _skill, _map, _spawnRegion))
        return false;
#endif

    SLOG_INFO("[GameData] 기획 데이터 파일을 모두 읽었습니다 ({})", dir);
    return true;
}
