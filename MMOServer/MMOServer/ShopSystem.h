// ==========================================================================
// ShopSystem — 사고팔기 규칙 (USE_SHOP · NPC·상점 플랜 4판)
//
// [책임]
//  - 사기: 살 수 있나(판정) → 가방에 넣기(전부 들어갈 때만) → 돈 빼기
//  - 팔기: 팔 수 있나(판정) → 칸에서 빼기 → 돈 더하기
//  - 사들이는 값(SellPriceOf — 파는 값의 절반 내림) · 그 과정에서 정한 것(성공 · 거절 사유 · 돈 들어옴·나감)을 지표로 센다
//  - 보내기·저장은 하지 않는다 — 바뀐 칸·돈은 가방이 스스로 표시해 두고, 부르는 쪽(CShopHandler)이
//    FlushInventory(칸 패킷 + S2C_MONEY + 틱 끝 저장 요청)·거절 패킷을 낸다(CItemSystem 과 같은 꼴)
//
// [「돈만 빠지고 물건이 안 들어옴」을 막는 자리] ⑴ 메모리 — 게임 로직이 스레드 하나라 한 요청의 판정·넣기·빼기가 안 끊기고,
//  넣기(TryAdd)는 「전부 아니면 아무것도」라 넣기가 된 뒤에만 돈을 뺀다 ⑵ DB — 칸과 돈이 같은 저장 묶음(행 통째 + 저장 안 된 칸) =
//  트랜잭션 하나(저장 플랜 C 의 길 — 여기서 새로 만드는 것이 없다).
//
// [계층 경계]
//  CPlayer·CCharacter(말 걸기 칸)·CInventory·아이템 표·상점 표·CGameMonitor 를 안다. 세션·패킷·CGameServer·존은 모른다.
//  NPC 를 「찾는」 것은 부르는 쪽(존의 섹터를 아는 CShopHandler)이다 — 여기는 찾은 캐릭터(없으면 nullptr)를 받아 판정만 한다.
//  거절 사유는 이 층의 어휘(ShopReject) — 와이어 ShopFail 로 바꾸는 건 CShopHandler 다(UseItemReject → UseItemFail 과 같은 꼴).
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"   // USE_SHOP — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_SHOP

class CPlayer;
class CCharacter;
class CGameDataFiles;
class CGameMonitor;
struct ItemDef;

// 사고팔기 거절 사유 — 와이어 ShopFail 과 같은 열(CShopHandler 가 switch 로 옮긴다).
//   확인 순서는 사기·팔기마다 다르다(아래 Buy · Sell 주석 — 플랜 「흐름」). 겹치면 먼저 걸린 하나로 답한다. 통과 = COUNT.
enum class ShopReject : uint8_t
{
    DEAD,              // 쓰러져 있다
    NO_NPC,            // 그 번호의 캐릭터를 주변에서 못 찾음 · 말 걸기 칸 없음(NPC 아님) · 상점이 없는 NPC
    NOT_IN_REACH,      // 그 NPC 의 손 닿는 범위(지금 자리 + 표의 반경) 밖
    NOT_SOLD_HERE,     // 사기 — 그 상점이 이 물건을 안 판다(아이템 표에 없는 번호 포함)
    NOT_BOUGHT_HERE,   // 팔기 — 그 상점이 이 물건을 안 사들인다 · 사들이는 값이 0
    BAD_SLOT,          // 팔기 — 칸 번호 범위 밖 · 착용 칸(900~) · 빈 칸
    IN_TRADE,          // 팔기 — 거래창에 올린 칸(가방이 잠갔다) — 와이어 ShopFail 에서는 맨 끝 값이다(끝에만 붙인다)
    BAD_COUNT,         // 개수가 1..한도 밖(사기 = 그 물건 maxStack · 팔기 = 그 칸 개수)
    NOT_ENOUGH_MONEY,  // 사기 — 값 × 개수가 가진 돈보다 많다
    BAG_FULL,          // 사기 — 가방 그 탭에 다 안 들어간다(아무것도 안 바뀐다)
    MONEY_FULL,        // 팔기 — 받으면 들 수 있는 돈(MONEY_MAX)을 넘는다(아무것도 안 바뀐다)
    COUNT
};

class CShopSystem
{
public:
    // 표는 게임 서버가 든 것을 참조한다 — 기동 때 LoadGameDataFiles 가 채운 뒤에만 부른다.
    CShopSystem(const CGameDataFiles& tables, CGameMonitor& gameMonitor);

    // 사기 — npc = 요청이 가리킨 캐릭터(부르는 쪽이 주변에서 찾았다 · 못 찾았으면 nullptr).
    //   확인 순서: DEAD → NO_NPC → NOT_IN_REACH → NOT_SOLD_HERE → BAD_COUNT → NOT_ENOUGH_MONEY → BAG_FULL. 통과면 COUNT.
    //   성공 = 가방에 count 개(칸 표시) → 값 × 개수만큼 돈 빼기(돈 바뀜 표시). 부르는 쪽이 FlushInventory 로 내보낸다.
    ShopReject Buy(CPlayer* player, const CCharacter* npc, int32_t itemId, int32_t count);

    // 팔기 — slot = 가방 칸 번호(INV_TAB_RANGES).
    //   확인 순서: DEAD → NO_NPC → NOT_IN_REACH → BAD_SLOT → IN_TRADE → NOT_BOUGHT_HERE → BAD_COUNT → MONEY_FULL. 통과면 COUNT.
    //   성공 = 칸에서 count 개 빼기 → 사들이는 값 × 개수만큼 돈 더하기. 부르는 쪽이 FlushInventory 로 내보낸다.
    ShopReject Sell(CPlayer* player, const CCharacter* npc, uint16_t slot, int32_t count);

    // 사들이는 값(한 개) — 파는 값의 절반, 내림(npc-shop-plan.md 따라 나온 것 3 — 확인된 원본 사례 둘이 다 절반). 규칙은 여기 한 곳
    static int64_t SellPriceOf(const ItemDef& def);

private:
    // 사기·팔기 공통 앞 둘(DEAD · NO_NPC · NOT_IN_REACH) — 통과면 COUNT 이고 outShopId 에 그 NPC 의 상점 번호
    ShopReject CheckNpc(const CPlayer* player, const CCharacter* npc, int32_t& outShopId) const;
    // 판정 — 위 순서대로 보고 먼저 걸린 하나. BAG_FULL 은 판정이 아니라 넣기의 결과라 Buy 가 본다(줍기의 TAB_FULL 과 같은 꼴)
    ShopReject CheckBuy(const CPlayer* player, const CCharacter* npc, int32_t itemId, int32_t count,
                        const ItemDef*& outDef, int64_t& outCost) const;
    ShopReject CheckSell(const CPlayer* player, const CCharacter* npc, uint16_t slot, int32_t count,
                         int& outIndex, int64_t& outGain) const;

    const CGameDataFiles& _tables;
    CGameMonitor&         _gameMonitor;
};

#endif // USE_SHOP
