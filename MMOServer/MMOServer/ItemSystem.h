// ==========================================================================
// ItemSystem — 아이템 쓰기·장착 규칙 (USE_INVENTORY · 장착은 USE_EQUIP)
//
// [책임]
//  - 물약 쓰기: 쓸 수 있나(판정) → 체력 또는 마력 채우기·한 개 줄이기·대기시간
//  - 장착: 끼울·뺄 수 있나(판정) → 칸 둘 맞바꾸기·대기시간·스탯 다시 세우기(CStatSystem)
//  - 그 과정에서 정한 것(성공·거절 사유)을 지표(CGameMonitor)로 센다 — CCombatSystem 과 같은 꼴
//  - 보내기·저장은 하지 않는다 — 바뀐 칸은 가방이 스스로 표시해 두고(CInventory), 부르는 쪽(CGameServer)이
//    FlushInventory(칸 패킷 + 틱 끝 저장 요청)·체력 방송·S2C_MY_STATS·거절 패킷을 낸다
//
// [왜 떼어냈나]
//  StatSystem.h 와 같은 까닭 — 입력이 CPlayer·가방·표뿐인 규칙이 CGameServer 멤버라, 게임 서버가 부르는 쪽은
//  이 규칙을 거꾸로 부를 수 없었다(10-08 아키텍처 2차 검사 2번).
//
// [계층 경계]
//  CPlayer·CInventory·아이템 표·CStatSystem·CGameMonitor 를 안다. 세션·패킷·CGameServer·존은 모른다.
//  거절 사유는 이 층의 어휘(아래 enum 둘) — 와이어 UseItemFail·EquipFail 로 바꾸는 건 게임 서버다
//  (ToPickupFail 과 같은 꼴 — 사유 enum 은 정하는 층에, 10-06 아키텍처 전수검사 7번).
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"   // USE_INVENTORY·USE_EQUIP · FRAME_PER_SEC — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_INVENTORY

class CPlayer;
class CGameDataFiles;
class CGameMonitor;
class CStatSystem;
struct ItemDef;

// 쓰기 거절 사유 — 열거 순서가 곧 확인 순서다(여럿이 겹치면 먼저 걸린 하나로 답한다). 통과 = COUNT.
//   판정을 값을 돌려주는 함수로 둔 이유 — 사유가 일곱이라 호출부에서 if 사슬로 쓰면 하나를 빠뜨려도 안 보인다.
enum class UseItemReject : uint8_t
{
    BAD_SLOT,     // 칸 번호가 어느 탭 구간에도 없다
    EMPTY,        // 빈 칸
    IN_TRADE,     // 거래창에 올린 칸(가방이 잠갔다 — Inventory.h [거래 잠금]) — 와이어 UseItemFail 에서는 맨 끝 값이다(끝에만 붙인다)
    NOT_USABLE,   // 쓸 수 있는 아이템이 아니다(소비 탭이 아니거나 효과가 없다) · 표에서 사라진 번호 포함 · 스킬을 끈 빌드의 마력 물약
    DEAD,         // 죽어 있다
    COOLDOWN,     // 아직 대기 중이다(지난번에 쓴 지 1초가 안 됐다)
    FULL_HP,      // 체력 물약인데 체력이 가득 — 물약을 안 줄인다
    FULL_MP,      // 마력 물약인데 마력이 가득 — 물약을 안 줄인다(FULL_HP 와 겹치지 않는다 — 한 물약 한 효과)
    COUNT
};

// 끼우기·빼기 거절 사유 — 열거 순서가 곧 확인 순서다. 통과 = COUNT.
//   장착을 끈 빌드(USE_EQUIP=0)에도 둔다 — 지표 이름표(GameMonitor.h EquipRejectName)가 가방 토글 안에 있어 이 enum 을 본다.
enum class EquipReject : uint8_t
{
    BAD_SLOT,         // 끼우기: 그런 가방 칸이 없다(없는 번호 · 착용 칸 번호) · 빼기: 착용 칸이 아니다
    EMPTY,            // 빈 칸
    IN_TRADE,         // 끼우기: 거래창에 올린 칸(착용 칸은 못 올려 빼기에는 안 걸린다) — 와이어 EquipFail 에서는 맨 끝 값이다
    NOT_EQUIPPABLE,   // 끼우기: 장비 아이템이 아니다 — 물약 칸을 보내도 여기다(쓰기의 NOT_USABLE 과 같은 꼴) · 표에서 사라진 번호 포함
    DEAD,             // 죽어 있다
    COOLDOWN,         // 아직 대기 중이다(지난번에 끼우거나 뺀 지 1초가 안 됐다)
    TAB_FULL,         // 빼기: 장비 탭에 빈 칸이 없다 — 아무것도 안 바뀐다
    COUNT
};

class CItemSystem
{
public:
    // 표는 게임 서버가 든 것을 참조한다 — 기동 때 LoadGameDataFiles 가 채운 뒤에만 부른다.
    CItemSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor, CStatSystem& statSystem);

    // 아이템 쓰기(지금은 물약만) — 판정 → 성공이면 물약이 채우는 쪽(체력·마력) 채우기(최대치에서 멈춘다)·한 개 줄이기·대기시간.
    //   통과면 COUNT 이고 outUsed 에 쓴 아이템 — 무엇을 채웠는지는 그 표 값(healHp·healMp)이 말한다. 거절이면 outUsed 는 nullptr.
    //   frame = 지금 틱(CGameServer::_frameCount) — 대기시간의 기준.
    //   바뀐 칸은 가방이 표시해 둔다 — 부르는 쪽이 FlushInventory 로 내보내고, 채운 쪽을 알린다(체력은 주변 방송, 마력은 본인만).
    UseItemReject UseItem(CPlayer* player, uint16_t slot, uint64_t frame, const ItemDef*& outUsed);

#if USE_EQUIP
    // 끼우기(가방 칸 → 무기 칸, 차 있으면 맞바꿈) · 빼기(무기 칸 → 장비 탭 첫 빈 칸). 통과면 COUNT.
    //   unequip = false 면 끼우기(slot = 가방 칸), true 면 빼기(slot = 착용 칸).
    //   성공 = 칸 둘 맞바꾸기 → 대기시간 → 스탯 다시 세우기(체력은 그대로).
    //   부르는 쪽이 FlushInventory(칸 패킷 둘 + 틱 끝 저장 요청) → S2C_MY_STATS 순으로 내보낸다.
    EquipReject Equip(CPlayer* player, uint16_t slot, bool unequip, uint64_t frame);
#endif

private:
    // 쓸 수 있나 — UseItemReject 순서대로 보고 먼저 걸린 하나. 통과면 COUNT 이고 outIndex·outDef 에 쓸 칸과 아이템.
    UseItemReject CheckUseItem(const CPlayer* player, uint16_t slot, uint64_t frame, int& outIndex, const ItemDef*& outDef) const;

#if USE_EQUIP
    // 되나 — EquipReject 순서대로 보고 먼저 걸린 하나. 통과면 COUNT 이고 outFrom·outTo 에 맞바꿀 두 색인.
    //   ★ 끼우기는 가방 칸이면 탭을 안 가린다: 물약 칸을 보내면 BAD_SLOT 이 아니라 NOT_EQUIPPABLE 이다(쓰기가 NOT_USABLE 인 것과 같다).
    //     BAD_SLOT 은 "그런 가방 칸이 없다"(없는 번호 · 착용 칸 번호)뿐이다.
    EquipReject CheckEquip(const CPlayer* player, uint16_t slot, bool unequip, uint64_t frame, int& outFrom, int& outTo) const;
#endif

    // 물약 대기시간 1초 — 서버 틱 수로 잰다(사용자 결정 09-23). 틱 주기가 바뀌어도 1초가 되게 FRAME_PER_SEC 로 쓴다.
    static constexpr uint64_t ITEM_USE_COOLDOWN_FRAMES = FRAME_PER_SEC;
#if USE_EQUIP
    // 끼우기·빼기 대기시간 1초 — 물약과 따로 센다(플랜 ㉰).
    static constexpr uint64_t EQUIP_COOLDOWN_FRAMES = FRAME_PER_SEC;
#endif

    const CGameDataFiles& _tables;
    CGameMonitor&         _gameMonitor;
    CStatSystem&          _statSystem;   // 장착 뒤 공격력 다시 세우기 — 가방은 전투를 요구해서(BuildConfig.h) 늘 있다
};

#endif // USE_INVENTORY
