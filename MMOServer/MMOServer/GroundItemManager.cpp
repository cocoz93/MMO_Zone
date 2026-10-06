#include "GroundItemManager.h"
#if USE_DROP
#include "Zone.h"             // 나를 든 존 — 섹터·벽 선 자르기
#include "SectorManager.h"    // 좌표 → 칸 번호 (CalcSectorX/Y)
#include "Player.h"           // 줍는 사람의 생사·좌표·계정
#include "GameMonitor.h"      // 드랍 계수기 · GroundItemRemoval
#include <Protocol/Protocol.h> // GroundItemGone · PickupFail
#include <cassert>

const std::vector<CGroundItem> CGroundItemManager::EMPTY_ITEMS;

CGroundItemManager::CGroundItemManager(CZone& zone)
    : _zone(zone)
{
}

void CGroundItemManager::Init()
{
    // 칸 수·칸 번호는 섹터 관리자 것 — 여기서 격자 크기를 베껴 들지 않는다(식이 두 벌이 되지 않게)
    _items.resize(static_cast<size_t>(_zone.GetSectorManager().CellCount()));
}

const std::vector<CGroundItem>& CGroundItemManager::GetSectorItems(int32_t sectorX, int32_t sectorY) const
{
    const CSectorManager& sectors = _zone.GetSectorManager();
    if (!sectors.IsValidSector(sectorX, sectorY))
        return EMPTY_ITEMS;
    return _items[sectors.CellIndex(sectorX, sectorY)];
}

void CGroundItemManager::AddAt(const CGroundItem& item, int32_t sectorX, int32_t sectorY)
{
    const CSectorManager& sectors = _zone.GetSectorManager();
    if (!sectors.IsValidSector(sectorX, sectorY))
        return;
    _items[sectors.CellIndex(sectorX, sectorY)].push_back(item);
}

void CGroundItemManager::RemoveAt(int32_t sectorX, int32_t sectorY, size_t index)
{
    const CSectorManager& sectors = _zone.GetSectorManager();
    if (!sectors.IsValidSector(sectorX, sectorY))
        return;
    std::vector<CGroundItem>& list = _items[sectors.CellIndex(sectorX, sectorY)];
    // [불변식] index 는 부르는 쪽이 방금 이 목록을 훑어 얻은 자리다(그 사이 목록을 안 바꾼다)
    assert(index < list.size());
    list[index] = list.back();   // 순서가 뜻이 없어 자리 바꿔 빼기로 O(1) — 캐릭터 목록(PopFrom)과 같은 꼴
    list.pop_back();
}

void CGroundItemManager::Drop(const GroundItemContext& ctx, const std::vector<Item>& loot,
                              float x, float y, int64_t ownerAccountId, uint64_t frame)
{
    // 놓는 자리 — 첫 아이템은 죽은 자리, 다음부터 옆 칸. 한 점에 포개면 웹이 그림 하나만 그린다.
    static constexpr float OFFSETS[][2] = {
        { 0, 0 }, { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 },
    };
    constexpr size_t OFFSET_COUNT = sizeof(OFFSETS) / sizeof(OFFSETS[0]);

    const CSectorManager& sectors = _zone.GetSectorManager();   // 좌표 → 칸 번호만
    for (size_t i = 0; i < loot.size(); ++i)
    {
        CGroundItem g;
        g.groundId        = ctx.events.AllocGroundId();
        g.item            = loot[i];
        float fx          = x + OFFSETS[i % OFFSET_COUNT][0];
        float fy          = y + OFFSETS[i % OFFSET_COUNT][1];
        _zone.ClampToWallLine(fx, fy);
        g.x               = fx;
        g.y               = fy;
        g.ownerAccountId  = ownerAccountId;
        g.ownerUntilFrame = frame + LOOT_LOCK_FRAMES;
        g.expireFrame     = frame + GROUND_ITEM_LIFE_FRAMES;

        // 아이템마다 자기 섹터 — 경계에 떨어지면 한 번에 떨어진 것들의 섹터가 서로 다를 수 있다
        const int32_t sx = sectors.CalcSectorX(g.x);
        const int32_t sy = sectors.CalcSectorY(g.y);
        AddAt(g, sx, sy);
        ++_count;
        ctx.monitor._drop._dropped.Inc();

        ctx.events.OnGroundItemDrop(&_zone, g, sx, sy);   // 주변에 CREATE(DROP)
    }
    // 게이지는 다 놓은 뒤 한 번 — 전 존 합계라 더하고 뺀다(존마다 든 _count 는 이 존 몫)
    ctx.monitor._drop._groundItems.Add(static_cast<int64_t>(loot.size()));
}

void CGroundItemManager::Remove(const GroundItemContext& ctx, int32_t sectorX, int32_t sectorY, size_t index,
                                GroundItemRemoval why, int32_t pickerId)
{
    const int32_t groundId = GetSectorItems(sectorX, sectorY)[index].groundId;   // 빼기 전에 읽는다

    // 와이어의 사유 — 빼는 까닭 셋 중 알릴 사람이 있는 것은 둘뿐이다(CLEANED 는 ForgetAll)
    GroundItemGone wire = GroundItemGone::EXPIRED;
    switch (why)
    {
    case GroundItemRemoval::PICKED:  wire = GroundItemGone::PICKED;  break;
    case GroundItemRemoval::EXPIRED: wire = GroundItemGone::EXPIRED; break;
    case GroundItemRemoval::CLEANED:
    case GroundItemRemoval::COUNT:
        assert(false && "CLEANED 는 알릴 사람이 없다 — ForgetAll 이 센다");
        return;
    }

    // 주변 DELETE · 같은 틱에 섹터를 넘은 사람 보정 — 받을 사람 고르기와 송신은 게임 서버(OnGroundItemGone)
    ctx.events.OnGroundItemGone(&_zone, sectorX, sectorY, groundId, wire, pickerId);

    RemoveAt(sectorX, sectorY, index);
    ctx.monitor._drop._removed[static_cast<int>(why)].Inc();
    --_count;
    ctx.monitor._drop._groundItems.Add(-1);
}

void CGroundItemManager::TickExpire(const GroundItemContext& ctx, uint64_t frame)
{
    if (_count == 0)
        return;   // 이 존 땅에 아무것도 없다 — 섹터를 훑지 않는다(부하 시험은 더미가 안 때려 늘 여기서 끝난다)

    const CSectorManager& sectors = _zone.GetSectorManager();
    for (int32_t sy = 0; sy < sectors.GetSectorCountY(); ++sy)
    {
        for (int32_t sx = 0; sx < sectors.GetSectorCountX(); ++sx)
        {
            const std::vector<CGroundItem>& list = GetSectorItems(sx, sy);
            for (size_t i = 0; i < list.size(); )
            {
                if (frame >= list[i].expireFrame)
                    Remove(ctx, sx, sy, i, GroundItemRemoval::EXPIRED, 0);   // 뒤엣것이 i 로 온다 — 올리지 않는다
                else
                    ++i;
            }
        }
    }
}

void CGroundItemManager::ForgetAll(const GroundItemContext& ctx)
{
    int64_t n = 0;
    const CSectorManager& sectors = _zone.GetSectorManager();
    for (int32_t sy = 0; sy < sectors.GetSectorCountY(); ++sy)
    {
        for (int32_t sx = 0; sx < sectors.GetSectorCountX(); ++sx)
            n += static_cast<int64_t>(GetSectorItems(sx, sy).size());
    }
    // 존마다 센 수와 실제 목록이 같아야 한다 — 어긋나면 넣고 빼는 자리 중 하나가 _count 를 빠뜨린 것
    assert(n == _count);
    if (n == 0)
        return;

    ctx.monitor._drop._removed[static_cast<int>(GroundItemRemoval::CLEANED)].Add(n);
    _count -= n;
    ctx.monitor._drop._groundItems.Add(-n);
}

PickupFail CGroundItemManager::FindPickup(const CPlayer* player, uint64_t frame, PickupTarget& out) const
{
    if (player->_isDead)
        return PickupFail::DEAD;

    // 반경이 걸치는 섹터만 참조로 훑는다(공격 대상 찾기와 같은 꼴 — 복사 0). 섹터 관리자는 좌표 → 칸 번호만
    const CSectorManager& sectors = _zone.GetSectorManager();
    const int32_t sx0 = sectors.CalcSectorX(player->_x - PICKUP_RANGE);
    const int32_t sx1 = sectors.CalcSectorX(player->_x + PICKUP_RANGE);
    const int32_t sy0 = sectors.CalcSectorY(player->_y - PICKUP_RANGE);
    const int32_t sy1 = sectors.CalcSectorY(player->_y + PICKUP_RANGE);
    const float rangeSq = PICKUP_RANGE * PICKUP_RANGE;

    bool  anyInRange = false;   // 반경 안에 뭔가 있긴 했다 — "아무것도 없음(NO_ITEM)"과 "남의 것뿐(NOT_OWNER)"을 가른다
    bool  found      = false;
    float bestSq     = 0.0f;
    for (int32_t sy = sy0; sy <= sy1; ++sy)
    {
        for (int32_t sx = sx0; sx <= sx1; ++sx)
        {
            const std::vector<CGroundItem>& list = GetSectorItems(sx, sy);
            for (size_t i = 0; i < list.size(); ++i)
            {
                const CGroundItem& g = list[i];
                const float dx = g.x - player->_x;
                const float dy = g.y - player->_y;
                const float dSq = dx * dx + dy * dy;
                if (dSq > rangeSq)
                    continue;
                anyInRange = true;

                // 내 것이거나(막타 친 계정) 우선 시간이 끝났으면 주울 수 있다
                const bool mine = g.ownerAccountId == 0 || g.ownerAccountId == player->_accountId ||
                                  frame >= g.ownerUntilFrame;
                if (!mine)
                    continue;

                if (!found || dSq < bestSq)
                {
                    found  = true;
                    bestSq = dSq;
                    out.sectorX = sx;
                    out.sectorY = sy;
                    out.index   = i;
                }
            }
        }
    }

    if (found)
        return PickupFail::COUNT;
    return anyInRange ? PickupFail::NOT_OWNER : PickupFail::NO_ITEM;
}

#endif // USE_DROP
