#include "GameDataFiles.h"

#if USE_GAME_DATA_FILES

#include <Common/ErrorLog.h>

bool CGameDataFiles::LoadAll(const std::string& dir)
{
    // 표가 늘어나면 여기 형제로 한 줄씩 붙는다. 하나라도 false 면 그 자리에서 끝낸다 —
    //   "레벨 표는 읽었는데 아이템 표는 못 읽은" 반쪽 상태로 서버를 띄우지 않는다.
#if USE_COMBAT   // 레벨 표는 전투를 따른다(바탕 스탯) — 경험치만 끈 빌드도 읽는다
    if (!_level.Load(dir + "LevelTable.csv"))
        return false;
#endif
#if USE_INVENTORY
    if (!_item.Load(dir + "ItemTable.csv"))
        return false;
#endif
#if USE_MONSTER
    if (!_spawnRegion.Load(dir + "SpawnRegion.csv"))
        return false;
#endif
#if USE_DROP
    // ★ 아이템 표 "뒤" — 드랍 표의 아이템 번호·개수를 아이템 표와 대조한다
    if (!_drop.Load(dir + "DropTable.csv", _item))
        return false;
#endif

    SLOG_INFO("[GameData] 기획 데이터 파일을 모두 읽었습니다 ({})", dir);
    return true;
}

#endif // USE_GAME_DATA_FILES
