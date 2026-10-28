// ==========================================================================
// GroundItemManager — 존 하나의 땅 아이템 (USE_DROP)
//
// [책임]
//  - 땅 아이템 목록 소유 — 섹터 칸마다 vector<CGroundItem>(값). 칸 번호·칸 수는 섹터 관리자 것을 그대로 쓴다(CellIndex·CellCount — 식의 사본을 두지 않는다).
//    시야 송신(입장·섹터 이동)이 칸 단위로 읽는다(GetSectorItems). 넣고 빼는 자리는 이 클래스 안뿐이다
//  - 땅 아이템의 규칙 — 놓을 자리·주인·수명 정하기(Drop) · 빼기(Remove) · 수명 훑기(TickExpire) ·
//    주울 것 고르기(FindPickup) · 이 존에 있는 수 세기(_count, 채널 정리 때 ForgetAll)
//  - 송신·번호 발급은 하지 않는다 — 일이 생기면 IGroundItemEvents 로 알리고, 받는 쪽(CItemHandler)이 한다
//
// [왜 떼어냈나]
//  땅은 아이템이 있을 수 있는 그릇 중 하나다(가방·장비 칸과 같은 층) — 그릇이 자기 물건과 규칙을 함께 든다
//  (가방 CInventory 가 칸을 직접 들듯이). 전에는 목록이 섹터 관리자(캐릭터 시야 격자)에, 규칙은 게임 서버에
//  흩어져 있어서, 게임 서버가 전 존 합계를 들고 존이 지워질 때 그 존 몫을 존 밖에서 세어 빼야 했다.
//  섹터 관리자는 이제 캐릭터 목록 셋만 든다 — 섹터는 땅 아이템이 시야 단위로 나뉘는 칸 번호로만 빌려 쓴다.
//
// [계층 경계]
//  CZone·CSectorManager(좌표 → 섹터 좌표 · 칸 번호 · 격자 범위 계산만)·CPlayer(줍는 사람의 생사·좌표·계정만 읽는다)를 안다. 세션·패킷 조립·CGameServer 는 모른다.
//  게임 서버 것(알림 통로·계수기)은 부를 때마다 GroundItemContext 로 받는다 — 몬스터 관리자와 같은 까닭
//  (MonsterManager.h [계층 경계]). 무엇이 떨어질지(드랍 표 굴림)는 CRewardHandler::DropLoot 가 정한다 — 잡은 사람에게 주는
//  보상이라 경험치와 같은 자리다. 여기는 굴린 결과를 받아 땅에 놓는 데서 시작한다.
// ==========================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "BuildConfig.h"   // USE_DROP — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_DROP

#include "Item.h"          // 굴림 결과 — 가방 칸과 같은 타입(Item)의 목록을 받는다
#include "GroundItem.h"    // 칸마다 값으로 든다 — 완전한 타입이 필요하다

class CZone;
class CPlayer;
class CGameMonitor;

// 사유 enum(GroundItemRemoval·PickupReject)은 GroundItem.h — 이 층의 어휘다. 와이어 enum(Protocol.h GroundItemGone·PickupFail)은
//   여기서 모른다. 바꾸는 건 게임 서버(10-06 — 전에는 이 관리자가 Protocol.h 를 포함하고 변환까지 했다).

// 땅 아이템 쪽이 게임 서버에 부탁하는 것 — CItemHandler 가 구현한다(번호 계수기가 거기 있고, 송신은 CSightBroadcaster 에 맡긴다).
//   IMonsterEvents 와 같은 꼴: "패킷을 보내라"가 아니라 "무슨 일이 있었다"로 알리고, 패킷 조립은 받는 쪽이 한다.
//   ★ 순수 가상이라 하나를 빠뜨리면 컴파일이 안 된다.
class IGroundItemEvents
{
public:
    virtual ~IGroundItemEvents() = default;

    // 번호 하나 — 사건이 아니라 부탁이다. 존을 넘어 겹치지 않아야 해서(번호 공간이 서버에 하나) 여기서 못 뽑는다.
    virtual int32_t AllocGroundId() = 0;

    // 떨어짐 — 방금 목록에 넣은 것. 그 섹터 주변 플레이어에게 CREATE(DROP).
    virtual void OnGroundItemDrop(CZone* zone, const CGroundItem& item, int32_t sectorX, int32_t sectorY) = 0;

    // 빠지기 직전 — 아직 목록에 있다. 주변 플레이어에게 DELETE, 이번 틱에 섹터를 넘은 사람에게는 보정 DELETE.
    //   why 는 PICKED(pickerId = 주운 사람)·EXPIRED(pickerId = 0) 둘뿐이다(CLEANED 는 알릴 사람이 없어 ForgetAll 이 센다).
    //   와이어 값(GroundItemGone)으로 바꾸는 건 받는 쪽 — 여기는 「왜 빠졌나」만 말한다.
    virtual void OnGroundItemGone(CZone* zone, int32_t sectorX, int32_t sectorY,
                                  int32_t groundId, GroundItemRemoval why, int32_t pickerId) = 0;
};

// 부를 때마다 넘기는 묶음 — 게임 서버가 만든다(존을 만들 때 꽂지 않는다. MonsterContext 와 같은 까닭).
struct GroundItemContext
{
    IGroundItemEvents& events;
    CGameMonitor&      monitor;
};

class CGroundItemManager
{
public:
    explicit CGroundItemManager(CZone& zone);

    CGroundItemManager(const CGroundItemManager&) = delete;
    CGroundItemManager& operator=(const CGroundItemManager&) = delete;

    // 칸 할당 — 존 초기화(CZone::Init)가 섹터 관리자를 세운 직후 부른다. 칸 수는 섹터 관리자(CellCount)에서 읽는다.
    void Init();

    // 한 칸의 땅 아이템 (범위 밖이면 빈 목록) — 시야 송신(입장·섹터 이동)과 줍기 처리가 읽는다.
    //   캐릭터가 아니라 섹터의 ForEachInSector 는 안 돈다. 값으로 담는다(GroundItem.h [값으로 담는다]) —
    //   그래서 벡터 안 자리는 빼면 바뀐다. 자리 번호는 한 번 훑는 동안만 쓰고 들고 있지 않는다.
    const std::vector<CGroundItem>& GetSectorItems(int32_t sectorX, int32_t sectorY) const;

    // 떨어뜨리기 — 굴림 결과(loot)를 (x, y) 둘레에 놓는다. 첫 것은 그 자리, 다음부터 옆 칸.
    //   주인 = ownerAccountId(막타 친 계정), LOOT_LOCK_FRAMES 동안 먼저 줍는다. GROUND_ITEM_LIFE_FRAMES 뒤 사라진다.
    //   아이템마다 목록에 넣은 뒤 OnGroundItemDrop 으로 알린다.
    void Drop(const GroundItemContext& ctx, const std::vector<Item>& loot,
              float x, float y, int64_t ownerAccountId, uint64_t frame);

    // 땅에서 빼는 한 자리 — 알림(OnGroundItemGone) · 사유별 계수 · 게이지 · 목록에서 빼기(자리 바꿔 빼기).
    //   줍기(PICKED)·사라지기(EXPIRED) 둘 다 여기로 온다. 채널 정리(CLEANED)는 알릴 사람이 없어 ForgetAll 이 센다.
    //   ★ 빼면 index 자리로 뒤엣것이 온다 — 훑으며 부르는 쪽은 index 를 올리지 말 것.
    void Remove(const GroundItemContext& ctx, int32_t sectorX, int32_t sectorY, size_t index,
                GroundItemRemoval why, int32_t pickerId);

    // 게임 루프 3-3 — 섹터를 훑어 수명이 다한 것을 뺀다. 이 존 땅에 아무것도 없으면 훑지 않는다.
    //   몇 틱마다 부를지는 게임 서버가 정한다(GROUND_ITEM_SCAN_FRAMES) — 존마다 세면 존이 생긴 틱에 따라
    //   훑는 때가 존마다 달라진다.
    void TickExpire(const GroundItemContext& ctx, uint64_t frame);

    // 채널이 지워지기 직전 — 사람이 없는 채널이라 알릴 사람이 없다. 수만 세고(CLEANED) 목록은 존과 함께 사라진다.
    void ForgetAll(const GroundItemContext& ctx);

    // 줍기 판정 — DEAD → NO_ITEM → NOT_OWNER 순으로 보고 먼저 걸린 하나. 통과면 PickupReject::NONE 과 고른 것의 자리.
    //   TAB_FULL 은 판정이 아니라 가방 넣기의 결과다(CItemHandler::RecvPickup). 사유가 넷이라 값을 돌려주는 함수로 뺐다(CheckUseItem 과 같은 꼴).
    //   돌려주는 건 이 층의 어휘(GroundItem.h) — 와이어 PickupFail 로 바꾸는 건 게임 서버.
    struct PickupTarget { int32_t sectorX = 0; int32_t sectorY = 0; size_t index = 0; };
    PickupReject FindPickup(const CPlayer* player, uint64_t frame, PickupTarget& out) const;

private:
    static constexpr float PICKUP_RANGE = 2.0f;   // 칸 — 공격 사거리(1.5)보다 조금 넉넉하게(서버 좌표는 클라 화면보다 반 왕복 늦다)

    static constexpr uint64_t GROUND_ITEM_LIFE_FRAMES = 60 * FRAME_PER_SEC;   // 60초 — 땅에 남는 시간(플랜 ㉰)
    static constexpr uint64_t LOOT_LOCK_FRAMES        = 10 * FRAME_PER_SEC;   // 10초 — 막타 친 사람이 먼저 줍는 시간(플랜 ㉯)

    // 한 칸에 넣기 · 한 개 빼기 — 목록을 바꾸는 자리는 이 둘뿐이다(Drop · Remove 가 부른다).
    //   빼기는 자리 바꿔 빼기(swap-and-pop)라 맨 뒤엣것이 index 자리로 온다.
    //   훑으면서 빼는 쪽은 뺀 뒤 index 를 올리지 말 것(방금 당겨온 것을 아직 안 봤다).
    void AddAt(const CGroundItem& item, int32_t sectorX, int32_t sectorY);
    void RemoveAt(int32_t sectorX, int32_t sectorY, size_t index);

    CZone& _zone;   // 나를 든 존 — 존이 나보다 오래 산다(멤버)

    // 칸마다 땅 아이템 — 칸 번호는 섹터 관리자 CellIndex(식과 가로칸수는 그쪽 한 곳), 크기는 CellCount
    std::vector<std::vector<CGroundItem>> _items;
    static const std::vector<CGroundItem> EMPTY_ITEMS;   // 범위 밖 요청 때 돌려줄 빈 목록

    // 이 존 땅에 있는 수 — 넣고 빼는 자리(Drop·Remove·ForgetAll)가 전부 이 클래스 안이라 여기서만 바뀐다.
    //   수명 훑기가 빈 존을 건너뛰는 데 쓴다. 전 존 합계는 게이지(_drop._groundItems)가 더하고 빼며 든다.
    int64_t _count = 0;
};

#endif // USE_DROP
