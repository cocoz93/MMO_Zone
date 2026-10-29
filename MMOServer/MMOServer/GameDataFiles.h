#pragma once

#include <string>

#include "BuildConfig.h"
#include "MapTable.h"
#include "PortalTable.h"
#include "LevelTable.h"
#include "ItemTable.h"
#include "MonsterTable.h"
#include "SpawnRegionTable.h"
#include "DropTable.h"
#include "SkillTable.h"
#include "MonsterSkillTable.h"
#include "ShopTable.h"
#include "NpcTable.h"
#include "BlockTable.h"

// 담는 자리는 늘 있다 — 맵 표는 토글이 없다(MapTable.h [토글 없음]). 나머지 표는 표마다 토글이 따로다
//   (레벨 표 USE_COMBAT · USE_INVENTORY · USE_MONSTER · USE_DROP · USE_SKILL).
//   처음엔 레벨 표 하나라 USE_EXP 안에 두었다가, 아이템 표가 붙으며 "표가 하나라도 켜져 있으면"으로 꺼냈고,
//   10-09 맵 표가 붙으며 그 조건(USE_GAME_DATA_FILES)도 없앴다.
//   레벨 표가 경험치가 아니라 전투를 따르는 까닭은 LevelTable.h(1레벨 줄 = 바탕 스탯의 유일한 출처).

// ==========================================================================
// CGameDataFiles — 기획 데이터 파일 전부를 담는 자리
//
// [가르는 선] 이 프로젝트는 데이터를 둘로 가른다. 그 선이 설계의 뼈대라 이름이 그걸 말한다.
//        유저 데이터 — 내 레벨·내 경험치·내 좌표. 사람마다 다르다 → DB 의 characters 행
//        기획 데이터 — 레벨 10 이 되려면 얼마, 뱀의 체력은 얼마. 전원 공통이다 → 여기(파일)
//        파일인 이유는 밸런스 값이 코드와 같은 커밋에 들어가 같이 롤백되고 diff 로 보이기 때문.
//        DB 가 나은 조건 둘(서버를 안 내리고 바꾸기 · 여러 대에 배포 누락 방지)은 아직 우리가 아니다.
//
// [늘어날 것] 레벨 표 다음으로 아이템 표가 붙었다(인벤토리). 몬스터 표는 "둘째 종류가 생기는 날"로 미뤄 두었다가
//        10-08 앞당겼다 — 몬스터 사양이 코드 상수·INI 에 흩어져, 존이 생길 때 스스로 깔 재료를 존 쪽이 못 쥐었다.
//        10-09 맵 표가 붙었다 — 맵 크기가 서버 INI 에 있어 스폰 표와 대조하려면 INI 를 들고 와야 했다.
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

    const CMapTable& Maps() const { return _map; }
    const CPortalTable& Portals() const { return _portal; }
    const CNpcTable& Npcs() const { return _npc; }   // 토글 없음 — 맵·포털과 같은 세계 데이터(NpcTable.h [토글 없음])
    const CBlockTable& Blocks() const { return _block; }   // 토글 없음 — 막힌 칸. 존이 자기 맵의 CWalkGrid 를 꺼낸다(CZone::Init)
#if USE_COMBAT
    const CLevelTable& Level() const { return _level; }
#endif
#if USE_INVENTORY
    const CItemTable& Item() const { return _item; }
#endif
#if USE_SHOP
    const CShopTable& Shops() const { return _shop; }
#endif
#if USE_MONSTER
    // 속도 상한 대조(CheckSpeed)는 여기서 안 한다 — 상한은 AI 사정이라 CGameServer::LoadGameDataFiles 가 대조한다.
    const CMonsterTable& Monsters() const { return _monster; }
    const CSpawnRegionTable& SpawnRegions() const { return _spawnRegion; }
#endif
#if USE_DROP
    const CDropTable& Drop() const { return _drop; }
#endif
#if USE_SKILL
    const CSkillTable& Skill() const { return _skill; }
#endif
#if USE_MONSTER && USE_SKILL
    const CMonsterSkillTable& MonsterSkills() const { return _monsterSkill; }
#endif

private:
    CMapTable _map;   // 맨 먼저 읽는다 — 스폰 표가 맵 번호·크기를 이 표와 대조한다(LoadAll 순서)
    CPortalTable _portal;   // 맵 표 바로 다음 — 맵 표만 대조한다. 토글 없음(PortalTable.h [토글 없음])
#if USE_COMBAT
    CLevelTable _level;
#endif
#if USE_INVENTORY
    CItemTable _item;
#endif
#if USE_SHOP
    CShopTable _shop;   // 아이템 표를 먼저 읽고 번호·담는 자리·값을 대조한다(LoadAll 순서)
#endif
    CNpcTable _npc;     // 맵 표 · (상점을 켠 빌드면) 상점 표를 먼저 읽고 대조한다(LoadAll 순서). 토글 없음
    CBlockTable _block; // 맵 표 바로 뒤에 읽고, 포털·NPC·스폰 표를 다 읽은 뒤 「배치 위치가 막힌 칸이 아닌가」를 본다(LoadAll 순서). 토글 없음
#if USE_MONSTER
    CMonsterTable _monster;
    CSpawnRegionTable _spawnRegion;   // 맵 표·몬스터 표를 먼저 읽고 그 결과로 대조한다(LoadAll 순서)
#endif
#if USE_DROP
    CDropTable _drop;   // 아이템 표·몬스터 표를 먼저 읽고 그 결과로 대조한다(LoadAll 순서)
#endif
#if USE_SKILL
    CSkillTable _skill;   // 레벨 표를 먼저 읽고 마력 비용을 최대 마력과 대조한다(LoadAll 순서). 대상·효과 종류는 코드의 이름표와 대조한다(SkillTable.cpp)
#endif
#if USE_MONSTER && USE_SKILL
    CMonsterSkillTable _monsterSkill;   // 몬스터 표·스킬 표를 먼저 읽고 그 결과로 대조한다(LoadAll 순서) — 몬스터가 어떤 스킬을 언제 쓰나
#endif
};
