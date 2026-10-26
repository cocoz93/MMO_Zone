// ==========================================================================
// StatSystem — 캐릭터 스탯·레벨 규칙 (USE_COMBAT)
//
// [책임]
//  - 최대 체력·공격력·방어력·최대 마력 세우기 — 바탕(플레이어 = 레벨 표 + 착용 장비 · 몬스터 = 몬스터 표) 뒤에 지속 효과 배수(효과 부품 CBuffPart)
//  - 입장 때 저장된 체력·마력 넣기 — 세운 최대치로 깎는다(RestoreHpMp)
//  - 경험치 더하기와 레벨 올리기 · 표 밖 레벨 보정(플레이어만 — 레벨은 사람만 쌓는다)
//  - 그 과정에서 정한 것(레벨업·보정)을 지표(CGameMonitor)로 센다 — 무엇을 정했는지는 여기서만 안다(CCombatSystem 과 같은 꼴)
//  - 보내기·저장·보상은 하지 않는다 — 부르는 쪽(처치 보상 조각 CRewardHandler · 채팅 명령 조각 CCommandHandler 등)이 본인 알림 조각의 SendExp·SendMyStats·SendActorMp(마력 — USE_SKILL)로 보내고, 저장은 마지막 저장값과 견주어 정한다(ShouldSave)
//
// [왜 떼어냈나]
//  규칙의 입력이 CPlayer 필드와 기획 표뿐인데 CGameServer 멤버였다. 그러면 게임 서버가 부르는 쪽(존·몬스터 관리자)은
//  이 규칙을 거꾸로 부를 수 없어, 몬스터 스탯 표가 생기면 존 쪽에 같은 식을 한 벌 더 적게 된다(10-08 아키텍처 2차 검사 2번).
//
// [계층 경계]
//  CCharacter(효과 칸)·CPlayer·CMonster(몬스터 표 행)·CInventory(착용 칸)·레벨 표·아이템 표·CGameMonitor 를 안다. 세션·패킷·CGameServer·존은 모른다.
//  객체는 CGameServer 가 멤버로 든다(표를 든 쪽과 같은 수명) — 존 쪽에 넘기는 건 부를 일이 생길 때.
//  지속 효과가 바뀌면 CBuffSystem 이 RecalcStats 를 다시 부른다(장착 뒤 CItemSystem 이 부르는 것과 같은 꼴).
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"   // USE_COMBAT·USE_EXP — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_COMBAT

class CCharacter;
class CPlayer;
class CHealthPart;
class CAttackPart;
class CGameDataFiles;
class CGameMonitor;

class CStatSystem
{
public:
    // 표는 게임 서버가 든 것을 참조한다 — 기동 때 LoadGameDataFiles 가 채운 뒤에만 부른다.
    CStatSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor);

    // 스탯 함수 — 최대 체력·공격력·방어력·최대 마력을 이 한 곳에서 세운다(입장·레벨업·끼우기·빼기·효과 걸림·풀림 공통).
    //   바탕은 종류로 갈린다(switch — 둘) : 플레이어 = 레벨 표(전투가 켜지면 늘 읽는다 — 코드 상수 없음, 10-06) + 착용 장비, 마력은
    //   스킬(USE_SKILL)이 켜졌을 때만 · 몬스터 = 몬스터 표(생성자가 넣은 것과 같은 값 — 효과가 풀릴 때 돌아갈 바탕이 여기서 다시 선다).
    //   그 뒤 공통으로 지속 효과 배수(효과 부품 CBuffPart)를 곱한다 — 바탕 + 장비 "뒤"라서 장비를 바꿔도 효과가 안 날아간다.
    //   ★ USE_EXP 밖에 두는 이유 — 장착(USE_EQUIP)은 경험치를 꺼도 돌고, 그때도 공격력은 여기서 세워야 한다.
    //     USE_COMBAT 안인 이유 — 스탯을 읽는 곳이 전투뿐이고, 경험치·가방·장착이 전부 USE_COMBAT 을 요구한다.
    //   refill = 체력·마력을 가득 채우나(레벨업). false 면 둘 다 최대치를 넘은 몫만 깎는다. 몬스터는 false 로만 부른다.
    //     입장도 false 다 — 최대치만 세우고 저장된 체력·마력은 RestoreHpMp 가 넣는다.
    void RecalcStats(CCharacter* character, bool refill);

    // 입장 — DB 에서 불러온 체력·마력을 넣는다. ★ RecalcStats 뒤에 부를 것 — 최대치(레벨 표·장비)가 서 있어야 넘은 몫을 깎는다.
    //   체력 0 이하·마력 음수 = 가득. 음수는 「저장된 적 없음」(DB_HP_MP_UNSAVED)이고, 죽은 채 저장된 체력 0 은
    //   부르는 쪽(CEntryHandler::OnCharacterLoaded)이 부활 규칙대로 이미 바꿔 넘긴다. 마력 0 은 그대로다(다 쓰고 나간 값).
    //   규칙(0·음수·최대 초과)은 부품이 든다(CHealthPart::RestoreHp · CAttackPart::RestoreMp) — 여기는 차례(RecalcStats 뒤)를 지키는 입구다.
    void RestoreHpMp(CHealthPart& health, CAttackPart& attack, int32_t hp, int32_t mp);

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
