#include "RewardHandler.h"

#if USE_COMBAT && USE_MONSTER

#include "SelfNotifier.h"
#include "SightBroadcaster.h"
#include "CharacterStore.h"   // USE_DB_WORKER — 레벨업 즉시 저장
#include "ItemHandler.h"      // USE_INVENTORY — 바뀐 칸 내보내기 · 땅 아이템 묶음
#include "PartyHandler.h"     // USE_PARTY — 방 다시 들어가기 잠금 넣기
#include "StatSystem.h"
#include "Zone.h"
#include "Player.h"
#include "Monster.h"
#include "GameMonitor.h"
#include "GameDataFiles.h"    // 맵 표 · 드랍 표(RollDrops — DropTable.h) · 아이템 표
#include <Common/ErrorLog.h>
#include <cassert>
#include <string>
#include <random>             // USE_DROP — 드랍 굴림 난수
#include "PacketBuild.h"      // 체력 바뀜(MakeActorStats — 레벨업 체력의 주변 방송) — 게임 서버 나누기 0-②

using namespace PacketBuild;   // 조립 함수를 이름만으로 부른다(GameServer.cpp 와 같은 꼴)

#if USE_EXP && USE_MONSTER
void CRewardHandler::GrantMonsterExp(CZone* zone, CPlayer* killer, int32_t reward)
{
    {   // 옛 OnDeath 6) 의 몸 그대로(10-17 도우미로 뺌 — 막타 경로와 방 보상이 같이 부른다. killer = 받는 사람)
        // ★ 준 뒤에 센다 — GainExp 는 만렙이면 한 톨도 안 쌓는다.
        //   먼저 세면 만렙 이후 잡은 수만큼 "줬다"가 늘어, 계수기가 실제와 갈린다.
        const int64_t beforeExp = killer->_exp;
        const bool leveled = _statSystem.GainExp(killer, reward);
        const bool gained  = (killer->_exp != beforeExp);

        if (gained)
        {
            _gameMonitor._exp._granted.Inc();
            _gameMonitor._exp._grantedAmount.Add(killer->_exp - beforeExp);

            _selfNotifier.SendExp(killer);

            // 저장 표시는 켜지 않는다 — 늘어난 경험치·레벨은 다음 주기 판정(ShouldSave)이 마지막 저장값(_dbSaved)과 견주어 싣는다.
        }

        if (leveled)
        {
            // 늘어난 체력을 본인과 주변에 다시 알린다 — 막대가 옛 최대치로 남지 않게.
            //   excludeSelf=false 라 본인에게도 간다(부활 방송과 같은 꼴).
            _sightBroadcaster.BroadcastAroundSector(zone, killer,
                MakeActorStats(killer->_actorId, killer->_hp, killer->_maxHp), false);

            // 오른 공격력·방어력은 본인에게만 — 남의 공격력은 화면에 안 쓴다(S2C_MY_STATS 는 본인 전용)
            _selfNotifier.SendMyStats(killer);

#if USE_SKILL
            // 늘고 가득 찬 마력도 본인에게만(RecalcStats 가 레벨업 때 채운다)
            _selfNotifier.SendActorMp(killer);
#endif

#if USE_DB_WORKER
            // 레벨업은 바로 저장한다 — 주기(60초) 전에 서버가 죽어도 오른 레벨이 되돌아가지 않게.
            //   한 행 통째(자리·맵·레벨·경험치·체력·마력)라 그 순간 값이 전부 남는다.
            if (_characterStore.IsRunning())
                _characterStore.EnqueuePlayerSave(killer);
#endif
        }
    }
}
#endif

#if USE_PARTY && USE_MONSTER
// 방 보상(RewardHandler.h 선언 주석 · room-plan.md 「흐름」). OnDeath 가 몬스터를 지우기 "앞"에 부른다 — monster 는 아직 살아 있는 객체다.
void CRewardHandler::RewardRoom(CZone* zone, CMonster* monster)
{
    const MapDef* map = _gameDataFiles.Maps().Find(zone->GetMapId());
    assert(map != nullptr && map->kind == MapKind::INSTANCE && "방 맵에서만 부른다 — OnDeath 의 IsRoomMap");
    const uint64_t lockUntil = _frameCount + static_cast<uint64_t>(map != nullptr ? map->lockoutSec : 0) * FRAME_PER_SEC;

    // 받는 사람 = 지금 이 존(방)의 캐릭터 중 사람 — 쓰러진 사람도 존에 있다(부활이 옮겨 가기 전까지). 목록을 먼저 떠 둔다 —
    //   보상 중에 존 목록이 바뀌는 길은 없지만(레벨업 방송 · 가방 패킷은 존을 안 바꾼다) 도는 목록의 주인이 존이다
    //   (보스 한 마리에 한 번이라 할당을 아끼지 않는다 — 운영자가 구경하러 들어와 있어도 빠지는 사람이 없게 상한을 두지 않는다)
    std::vector<CPlayer*> people;
    for (CCharacter* c : zone->GetCharacters())
    {
        if (CPlayer* p = As<CPlayer>(c))
            people.push_back(p);
    }
    const int32_t count = static_cast<int32_t>(people.size());

#if USE_DROP
    thread_local std::mt19937 rng{ std::random_device{}() };   // DropLoot 와 같은 꼴(게임 스레드 전용)
#endif
    for (int32_t i = 0; i < count; ++i)
    {
        CPlayer* p = people[i];
        // 알림은 줄을 나눈다 — 머리 한 줄(경험치) + 아이템마다 한 줄. 채팅 칸(웹)이 긴 줄 끝을 「…」로 잘라 한 줄에 다 담으면 뒤 아이템이 안 보인다
        std::string head = monster->_def.name + " 보상 —";

#if USE_EXP
        const int64_t beforeExp = p->_exp;
        GrantMonsterExp(zone, p, monster->_def.exp);   // 전액 — 나누지 않는다(room-plan.md 「사람이 정할 것」 3)
        head += " 경험치 " + std::to_string(p->_exp - beforeExp);   // 만렙이면 0
#endif
        _selfNotifier.SendSystemMessage(p, head);

#if USE_DROP
        // 드랍 표를 사람마다 따로 굴린다 → 가방에 바로(줍기와 같은 TryAdd · FlushInventory) — 넘치면 그 사람 발밑 땅에 그 사람 것으로
        //   (땅의 주인 규칙 그대로 — 10초 본인 먼저, GroundItemManager.h). 굴림 수는 필드 계수(_drop._rolls = 잡은 수)에 안 넣는다
        RollDrops(_gameDataFiles.Drop().RowsOf(monster->_def.monsterId), rng, _lootBuffer);
        _roomOverflow.clear();
        for (const Item& it : _lootBuffer)
        {
            const ItemDef* def = _gameDataFiles.Item().Find(it.itemId);
            if (def == nullptr)
            {
                assert(false && "드랍 표 로드가 아이템 표에 없는 번호를 막는다");
                continue;
            }
            const InvAddResult r = p->_inventory.TryAdd(*def, it.count);
            if (r == InvAddResult::OK)
            {
                _gameMonitor._inventory._itemsAdded.Add(it.count);   // /item · 줍기와 같은 계수기 — 가방에 들어간 개수
                _gameMonitor._room._rewardItemsBag.Add(it.count);
                _selfNotifier.SendSystemMessage(p, "얻음 — " + def->name + " ×" + std::to_string(it.count));
            }
            else
            {
                _gameMonitor._inventory._addRejected[static_cast<int>(r)].Inc();
                _gameMonitor._room._rewardItemsGround.Add(it.count);
                _roomOverflow.push_back(it);
                _selfNotifier.SendSystemMessage(p, "가방이 차서 발밑에 — " + def->name + " ×" + std::to_string(it.count));
            }
        }
        _itemHandler.FlushInventory(p);   // 칸 패킷 + 곧바로 DB 저장(줍기와 같은 길) — 바뀐 칸이 없으면 아무것도 안 간다
        if (!_roomOverflow.empty())
            zone->GetGroundItemManager().Drop(_itemHandler.MakeGroundItemContext(), _roomOverflow, p->_x, p->_y, p->_accountId, _frameCount);
#endif

        _partyHandler.LockRoomReentry(p->_accountId, zone->GetMapId(), lockUntil);   // 표 크기 게이지도 거기서(게임 서버 나누기 7 단계)
        _gameMonitor._room._rewards.Inc();
    }

    SLOG_INFO("[Room] zone {} — {} 보상 {}명 (잠금 {}초)", zone->GetZoneId(), monster->_def.name, count, map != nullptr ? map->lockoutSec : 0);
}
#endif

#if USE_DROP
// 드랍 굴림 — 땅의 아이템(값)과 놓기·수명·주인·줍기 고르기·개수는 존마다 든 CGroundItemManager(GroundItemManager.h) ·
//   그 알림과 줍기는 가방·땅 조각(CItemHandler). 여기는 무엇이 떨어질지 굴려 관리자에 넘기기만(게임 서버 나누기 8 단계).
void CRewardHandler::DropLoot(CZone* zone, CPlayer* killer, CMonster* monster)
{
    _gameMonitor._drop._rolls.Inc();   // 아무것도 안 떨어져도 1 — "잡은 수"와 같아야 한다

    // 게임 스레드 전용 — 서버의 다른 난수 자리와 같은 꼴. 굴림 자체는 순수 함수(DropTable.h RollDrops)
    thread_local std::mt19937 rng{ std::random_device{}() };
    RollDrops(_gameDataFiles.Drop().RowsOf(monster->_def.monsterId), rng, _lootBuffer);
    if (_lootBuffer.empty())
        return;

    // 땅에 놓기는 존의 관리자가 — 아이템마다 OnGroundItemDrop 으로 돌아와 주변에 CREATE(DROP)
    zone->GetGroundItemManager().Drop(_itemHandler.MakeGroundItemContext(), _lootBuffer, monster->_x, monster->_y,
                                      killer->_accountId, _frameCount);
}
#endif // USE_DROP

#endif // USE_COMBAT && USE_MONSTER
