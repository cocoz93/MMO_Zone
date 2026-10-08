#pragma once

#include <string>

#include "BuildConfig.h"
#include "LevelTable.h"
#include "ItemTable.h"
#include "SpawnRegionTable.h"
#include "DropTable.h"

// 담는 자리는 표가 하나라도 켜져 있으면 산다 — 표마다 토글이 따로다(레벨 표 USE_COMBAT · USE_INVENTORY · USE_MONSTER).
//   처음엔 레벨 표 하나라 USE_EXP 안에 두었다가, 아이템 표가 붙으며 밖으로 꺼냈다.
//   레벨 표가 경험치가 아니라 전투를 따르는 까닭은 LevelTable.h(1레벨 줄 = 바탕 스탯의 유일한 출처).
#if USE_GAME_DATA_FILES

// ==========================================================================
// CGameDataFiles — 기획 데이터 파일 전부를 담는 자리
//
// [가르는 선] 이 프로젝트는 데이터를 둘로 가른다. 그 선이 설계의 뼈대라 이름이 그걸 말한다.
//        유저 데이터 — 내 레벨·내 경험치·내 좌표. 사람마다 다르다 → DB 의 characters 행
//        기획 데이터 — 레벨 10 이 되려면 얼마, 뱀의 체력은 얼마. 전원 공통이다 → 여기(파일)
//        파일인 이유는 밸런스 값이 코드와 같은 커밋에 들어가 같이 롤백되고 diff 로 보이기 때문.
//        DB 가 나은 조건 둘(서버를 안 내리고 바꾸기 · 여러 대에 배포 누락 방지)은 아직 우리가 아니다.
//
// [늘어날 것] 레벨 표 다음으로 아이템 표가 붙었다(인벤토리). 예고된 것이 하나 더 —
//        몬스터 스탯 표(둘째 종류가 생기는 날).
//        표가 하나일 때 줄어드는 코드는 서너 줄뿐이었고, 값어치는 거기가 아니라
//        "기획 데이터는 여기"가 코드에 이름으로 존재하는 것이다.
//
// [상속 안 씀] 표끼리 같은 것은 파일을 줄과 칸으로 쪼개는 일뿐이다. 열이 몇이고 무슨 뜻인지,
//        어떤 규칙으로 검사하는지는 표마다 다르다 — 레벨 표의 "필요 경험치가 단조 증가"는
//        아이템 표에 아무 뜻이 없다. 기반 클래스로 묶으려면 가상 함수를 놓아야 하는데
//        이 리포는 캐릭터 계층에서도 소멸자 말고는 안 두기로 했다(Character.h [가상 함수]).
//
// [실패하면 안 띄운다] 하나라도 못 읽으면 LoadAll 이 false 이고 main 이 서버를 안 띄운다.
//        밸런스 값 없이 도는 서버는 조용히 틀린 게임이 된다. DB 기동 실패와 같은 판단.
// ==========================================================================
class CGameDataFiles
{
public:
    // dir 은 끝에 경로 구분자가 붙은 디렉토리 (Platform::GetExecutableDir 이 그 꼴로 준다).
    //   한 표라도 실패하면 곧바로 false — 뒤엣것을 마저 읽어 봐야 어차피 안 띄운다.
    bool LoadAll(const std::string& dir);

#if USE_COMBAT
    const CLevelTable& Level() const { return _level; }
#endif
#if USE_INVENTORY
    const CItemTable& Item() const { return _item; }
#endif
#if USE_MONSTER
    // 맵 크기와의 대조(CheckMaps)는 여기서 안 한다 — 맵 설정(INI)은 CGameServer::LoadGameDataFiles 가 받아 대조한다.
    const CSpawnRegionTable& SpawnRegions() const { return _spawnRegion; }
#endif
#if USE_DROP
    const CDropTable& Drop() const { return _drop; }
#endif

private:
#if USE_COMBAT
    CLevelTable _level;
#endif
#if USE_INVENTORY
    CItemTable _item;
#endif
#if USE_MONSTER
    CSpawnRegionTable _spawnRegion;
#endif
#if USE_DROP
    CDropTable _drop;   // 아이템 표를 먼저 읽고 그 결과로 대조한다(LoadAll 순서)
#endif
};

#endif // USE_GAME_DATA_FILES
