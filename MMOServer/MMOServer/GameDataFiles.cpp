#include "GameDataFiles.h"

#if USE_EXP

#include <Common/ErrorLog.h>

bool CGameDataFiles::LoadAll(const std::string& dir)
{
    // 표가 늘어나면 여기 형제로 한 줄씩 붙는다. 하나라도 false 면 그 자리에서 끝낸다 —
    //   "레벨 표는 읽었는데 아이템 표는 못 읽은" 반쪽 상태로 서버를 띄우지 않는다.
    if (!_level.Load(dir + "LevelTable.csv"))
        return false;

    SLOG_INFO("[GameData] 기획 데이터 파일을 모두 읽었습니다 ({})", dir);
    return true;
}

#endif // USE_EXP
