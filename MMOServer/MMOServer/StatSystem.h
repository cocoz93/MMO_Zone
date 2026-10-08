// ==========================================================================
// StatSystem — 플레이어 스탯·레벨 규칙 (USE_COMBAT)
//
// [책임]
//  - 최대 체력·공격력·방어력 세우기(레벨 표 + 착용 장비) · 경험치 더하기와 레벨 올리기 · 표 밖 레벨 보정
//  - 그 과정에서 정한 것(레벨업·보정)을 지표(CGameMonitor)로 센다 — 무엇을 정했는지는 여기서만 안다(CCombatSystem 과 같은 꼴)
//  - 보내기·저장 표시·보상은 하지 않는다 — 부르는 쪽(CGameServer)이 SendExp·SendMyStats·_dbDirty 로 한다
//
// [왜 떼어냈나]
//  규칙의 입력이 CPlayer 필드와 기획 표뿐인데 CGameServer 멤버였다. 그러면 게임 서버가 부르는 쪽(존·몬스터 관리자)은
//  이 규칙을 거꾸로 부를 수 없어, 몬스터 스탯 표가 생기면 존 쪽에 같은 식을 한 벌 더 적게 된다(10-08 아키텍처 2차 검사 2번).
//
// [계층 경계]
//  CPlayer·CInventory(착용 칸)·레벨 표·아이템 표·CGameMonitor 를 안다. 세션·패킷·CGameServer·존은 모른다.
//  객체는 CGameServer 가 멤버로 든다(표를 든 쪽과 같은 수명) — 존 쪽에 넘기는 건 부를 일이 생길 때.
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"   // USE_COMBAT·USE_EXP — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_COMBAT

class CPlayer;
class CGameDataFiles;
class CGameMonitor;

class CStatSystem
{
public:
    // 표는 게임 서버가 든 것을 참조한다 — 기동 때 LoadGameDataFiles 가 채운 뒤에만 부른다.
    CStatSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor);

    // 스탯 함수 — 최대 체력·공격력·방어력을 이 한 곳에서 세운다(입장·레벨업·끼우기·빼기 공통).
    //   바탕 = 레벨 표(전투가 켜지면 늘 읽는다 — 코드 상수 없음, 10-06).
    //   ★ USE_EXP 밖에 두는 이유 — 장착(USE_EQUIP)은 경험치를 꺼도 돌고, 그때도 공격력은 여기서 세워야 한다.
    //     USE_COMBAT 안인 이유 — 스탯을 읽는 곳이 전투뿐이고, 경험치·가방·장착이 전부 USE_COMBAT 을 요구한다.
    //   refillHp = 체력을 가득 채우나(레벨업·스폰). false 면 최대치를 넘은 몫만 깎는다.
    void RecalcStats(CPlayer* player, bool refillHp);

#if USE_EXP
    // 경험치를 더하고, 올라갈 만큼 올린다. 레벨이 올랐으면 true(스탯도 다시 세우고 레벨업 수를 센다).
    //   ★ 부르는 자리가 둘이다 — 몬스터를 잡았을 때, 그리고 불러왔을 때.
    //     틱마다 레벨을 보는 자리가 없어서, 불러올 때 안 부르면 표를 낮춘 패치 뒤에 들어온 사람이
    //     "몬스터를 하나 잡기 전까지" 옛 레벨로 서 있게 된다.
    bool GainExp(CPlayer* player, int64_t amount);

    // 레벨을 표 안으로 내린다 (행이 표 밖 레벨을 들고 있을 때). 내렸으면 true(보정 수도 여기서 센다).
    //   ★ 접근자가 조용히 clamp 하지 않는 이유가 이것 — 여기서 한 번 내리고 세야
    //     "표 밖 레벨이 몇 건이었나"가 지표에 남는다.
    bool ClampLevelToTable(CPlayer* player);
#endif

private:
    const CGameDataFiles& _tables;
    CGameMonitor&         _gameMonitor;
};

#endif // USE_COMBAT
