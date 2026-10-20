// ==========================================================================
// CRewardHandler — 처치 보상: 몬스터 경험치 · 드랍 굴림 · 방 보상 (게임 서버 나누기 8 단계 · C5 · USE_COMBAT && USE_MONSTER)
//
// [책임]
//  - 몬스터 경험치 주기 한 곳(GrantMonsterExp — 준 뒤에 세기 · 경험치 알림 · 레벨업이면 체력 방송·스탯·마력·즉시 저장)
//  - 드랍 굴림(DropLoot — 표를 굴려 존의 땅 아이템 관리자에 넘긴다 · 주인 = 막타 계정)
//  - 방 보상(RewardRoom — 방 안 사람 각자: 경험치 전액 · 드랍을 가방에 바로 · 넘치면 발밑 땅 · 다시 들어가기 잠금 · 채팅 알림 줄)
//
// [경계]
//  누가 보상을 받나(막타 길인가 방 길인가)는 부르는 쪽(처치 알림 OnDeath)이 고른다 — 여기는 고른 대로 준다.
//  경험치·레벨 규칙은 CStatSystem, 무엇이 떨어지나는 순수 함수 RollDrops(DropTable.h), 땅에 놓기는 존의 CGroundItemManager.
//  보내기는 조각에 맡긴다 — 본인 상태(본인 알림) · 주변 체력(시야 방송) · 가방 칸(가방·땅 조각). 즉시 저장은 캐릭터 저장소, 잠금 표는 파티·방 조각.
//
// [모르는 것]
//  공격 판정·죽음 처리·부활. 세션·패킷 분기·입장 절차.
// ==========================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "BuildConfig.h"     // USE_COMBAT · USE_MONSTER · USE_EXP · USE_DROP · USE_PARTY · USE_DB_WORKER — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_COMBAT && USE_MONSTER

#include "Item.h"            // Item — 굴림 결과 버퍼(멤버 재사용)

class CZone;
class CPlayer;
class CMonster;
class CSelfNotifier;
class CSightBroadcaster;
class CStatSystem;
class CGameMonitor;
class CGameDataFiles;
#if USE_DB_WORKER
class CCharacterStore;
#endif
#if USE_DROP
class CItemHandler;
#endif
#if USE_PARTY
class CPartyHandler;
#endif

class CRewardHandler
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(두 보내기 조각 · 저장소 · 가방·땅 조각 · 파티·방 조각 · 스탯 규칙 · 게임 지표 · 기획 데이터 · 틱 번호)보다
    //   뒤에 이것을 선언해야 한다(멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    CRewardHandler(CSelfNotifier& selfNotifier, CSightBroadcaster& sightBroadcaster,
#if USE_DB_WORKER
                   CCharacterStore& characterStore,
#endif
#if USE_DROP
                   CItemHandler& itemHandler,
#endif
#if USE_PARTY
                   CPartyHandler& partyHandler,
#endif
                   CStatSystem& statSystem, CGameMonitor& gameMonitor, const CGameDataFiles& gameDataFiles, const uint64_t& frameCount)
        : _selfNotifier(selfNotifier), _sightBroadcaster(sightBroadcaster)
#if USE_DB_WORKER
        , _characterStore(characterStore)
#endif
#if USE_DROP
        , _itemHandler(itemHandler)
#endif
#if USE_PARTY
        , _partyHandler(partyHandler)
#endif
        , _statSystem(statSystem), _gameMonitor(gameMonitor), _gameDataFiles(gameDataFiles), _frameCount(frameCount) {}

#if USE_EXP && USE_MONSTER
    // 몬스터 경험치 주기 한 곳 — 막타 경로(OnDeath 6)와 방 보상(RewardRoom)이 같이 쓴다. 준 뒤에 세고, 오르면 알리고 바로 저장한다.
    void GrantMonsterExp(CZone* zone, CPlayer* player, int32_t reward);
#endif

#if USE_PARTY && USE_MONSTER
    // 방 보상(Room.h) — 방 맵의 보스(몬스터 표 boss)가 죽으면 OnDeath 가 막타 경로(경험치·드랍) 대신 부른다. 방(존) 안 사람 각자
    //   (쓰러져 부활을 기다리는 사람 포함 — 아직 방에 있다): ⑴ 경험치 전액 ⑵ 드랍 표를 따로 굴려 가방에 바로, 넘치면 그 사람 발밑 땅에 그 사람 것으로
    //   ⑶ 다시 들어가기 잠금(맵 표 lockoutSec) ⑷ 채팅 [System] 줄 — 경험치 한 줄 + 아이템마다 한 줄. 막타·피해 기록은 안 본다(room-plan.md 「정해진 것」).
    void RewardRoom(CZone* zone, CMonster* monster);
#endif

#if USE_DROP
    // 사람이 몬스터를 잡으면 부른다(OnDeath 의 경험치 블록 뒤 — 같은 조건).
    //   표의 줄마다 굴리고, 결과를 존의 관리자에게 넘겨 땅에 놓는다(아이템마다 OnGroundItemDrop 으로 돌아온다).
    //   주인 = 막타 친 계정 — 굴림은 잡은 사람에게 주는 보상이라 경험치와 같은 이 조각에서 한다.
    void DropLoot(CZone* zone, CPlayer* killer, CMonster* monster);
#endif

private:
    CSelfNotifier&        _selfNotifier;       // 경험치 · 스탯 · 마력 · 알림 줄(본인에게)
    CSightBroadcaster&    _sightBroadcaster;   // 레벨업 체력을 주변에
#if USE_DB_WORKER
    CCharacterStore&      _characterStore;     // 레벨업 틱 끝 저장 요청
#endif
#if USE_DROP
    CItemHandler&         _itemHandler;        // 바뀐 칸 내보내기 · 땅 아이템 묶음
#endif
#if USE_PARTY
    CPartyHandler&        _partyHandler;       // 방 다시 들어가기 잠금 넣기
#endif
    CStatSystem&          _statSystem;         // 경험치·레벨 규칙
    CGameMonitor&         _gameMonitor;        // 경험치 · 드랍 · 가방 · 방 지표
    const CGameDataFiles& _gameDataFiles;      // 맵 표(잠금 시간) · 드랍 표 · 아이템 표
    const uint64_t&       _frameCount;         // 게임 루프의 틱 번호 — 빌린 것, 읽기만(잠금이 풀리는 틱 · 땅 수명)

#if USE_DROP
    std::vector<Item> _lootBuffer;   // 한 마리분 굴림 결과 (멤버 재사용 — 죽을 때마다 할당하지 않게)
#if USE_PARTY
    std::vector<Item> _roomOverflow; // 방 보상 중 가방이 차서 못 넣은 것 — 그 사람 발밑 땅으로(RewardRoom · 멤버 재사용)
#endif
#endif
};

#endif // USE_COMBAT && USE_MONSTER
