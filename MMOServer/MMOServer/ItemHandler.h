// ==========================================================================
// CItemHandler — 가방·땅: 물약·장착·줍기 요청과 가방 내보내기, 땅 아이템 알림 (게임 서버 나누기 6 단계 · C6 · USE_INVENTORY)
//
// [책임]
//  - 받은 패킷 넷의 처리기와 그 등록(RegisterRoutes — 물약 · 끼우기 · 빼기 · 줍기) — 컨텐츠 모듈이 자기 줄을 등록한다
//  - 바뀐 칸 내보내기 한 곳(FlushInventory — 칸 패킷 + 그 틱 끝 저장 요청) · 거절 사유를 와이어 값으로(물약 · 장착 · 줍기)
//  - 땅 아이템 알림(IGroundItemEvents — 번호 발급 · 떨어짐 · 빠짐)과 수명 훑기 틱 · 관리자에 넘길 묶음(MakeGroundItemContext) ·
//    존 지우기 직전 남은 땅 아이템 세기(OnErasingZone)
//
// [경계]
//  판정·적용은 규칙에 맡긴다 — 쓰기·장착은 CItemSystem, 줍기 고르기·땅 목록은 존의 CGroundItemManager. 여기는 받기와 내보내기.
//  보내기는 다른 모듈에 맡긴다 — 한 사람 패킷(세션 허브) · 본인 상태(본인 알림) · 주변·땅 아이템 보이기(시야 방송). 칸 저장은 캐릭터 저장소.
//  무엇이 떨어질지(굴림)는 처치 보상 쪽이다 — 여기는 떨어진 뒤의 알림과 줍기만.
//
// [모르는 것]
//  전투·보상·파티·입장 절차. 아이템 표는 읽기만 한다(줍기의 아이템 정의 찾기).
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"          // USE_INVENTORY · USE_EQUIP · USE_DROP · USE_DB_WORKER — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_INVENTORY

#include "GroundItemManager.h"    // USE_DROP — IGroundItemEvents · GroundItemContext · GroundItemRemoval (헤더 안에서 스스로 토글을 본다)

class CPlayer;
class CZone;
class CSerialBuffer;
class CPacketRoutes;
class CSessionHub;
class CSelfNotifier;
class CSightBroadcaster;
class CItemSystem;
class CMapManager;
class CGameMonitor;
class CGameDataFiles;
#if USE_DB_WORKER
class CCharacterStore;
#endif

// 땅 아이템 알림은 비공개 상속 — 꽂는 것은 이 클래스 자신뿐이다(MakeGroundItemContext 가 *this 를 싣는다).
class CItemHandler
#if USE_DROP
    : private IGroundItemEvents
#endif
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(보내기 셋(CSessionHub · CSelfNotifier · CSightBroadcaster) · 저장소 · 아이템 규칙 · 맵 관리자 · 게임 지표 · 기획 데이터 · 틱 번호)보다
    //   뒤에 이것을 선언해야 한다(멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    CItemHandler(CSessionHub& sessionHub, CSelfNotifier& selfNotifier, CSightBroadcaster& sightBroadcaster,
#if USE_DB_WORKER
                 CCharacterStore& characterStore,
#endif
                 CItemSystem& itemSystem, CMapManager& mapManager, CGameMonitor& gameMonitor,
                 const CGameDataFiles& gameDataFiles, const uint64_t& frameCount)
        : _sessionHub(sessionHub), _selfNotifier(selfNotifier), _sightBroadcaster(sightBroadcaster)
#if USE_DB_WORKER
        , _characterStore(characterStore)
#endif
        , _itemSystem(itemSystem), _mapManager(mapManager), _gameMonitor(gameMonitor)
        , _gameDataFiles(gameDataFiles), _frameCount(frameCount) {}

    // 받은 패킷 등록 — 물약 · 끼우기·빼기(USE_EQUIP) · 줍기(USE_DROP). 게임 서버의 등록(RegisterRoutes)이 부른다. false = 등록 실패(기동 거절).
    bool RegisterRoutes(CPacketRoutes& routes);

    // 바뀐 칸을 내보낸다 — 칸 패킷 하나씩(+ 4단계에서 DB 저장 하나씩). 세는 것도 여기 한 곳이다.
    //   ★ 칸을 바꾼 자리는 반드시 부른다. 넣기·쓰기·드랍 어느 길이든 내보내는 길은 이것 하나다.
    void FlushInventory(CPlayer* player);

#if USE_DROP
    // 관리자에 넘길 묶음 — 부를 때마다 만든다(MakeMonsterContext 와 같은 꼴).
    GroundItemContext MakeGroundItemContext();

    // 틱 3-3 — 25틱에 한 번 존마다 관리자의 수명 훑기(TickExpire)를 부른다. 섹터 변경 대기열을 비운 뒤라
    //   "같은 틱에 섹터를 넘은 사람" 보정이 필요 없다. 땅에 아무것도 없는 존은 관리자가 바로 끝낸다(부하 시험은 늘 그렇다).
    void TickGroundItems();

    // 존 지우기 직전(빈 채널 정리 — CGameServer::OnErasingZone 이 부른다) — 그 존 땅에 남은 아이템을 「치움(CLEANED)」으로 센다.
    //   목록은 존과 함께 사라지고 알릴 사람이 없다(존이 비었다). 세는 몸은 존의 관리자(CGroundItemManager::ForgetAll).
    void OnErasingZone(CZone* zone);

    static constexpr int GROUND_ITEM_SCAN_FRAMES = FRAME_PER_SEC;   // 1초에 한 번 훑는다 — 사라지는 때가 1초 늦을 수 있다
#endif

private:
    // 아이템 쓰기(지금은 물약만) — CItemSystem::UseItem 을 불러, 거절이면 사유를 본인에게(와이어 값으로 바꿔),
    //   성공이면 칸 갱신 + 채운 쪽 통보(체력 물약 = 체력 주변 방송 · 마력 물약 = 마력 본인만). 판정·적용·세기는 CItemSystem 이다.
    void RecvUseItem(CPlayer* player, CSerialBuffer* pMsg);

#if USE_EQUIP
    // ── 장착 (USE_EQUIP) ──
    // 끼우기(가방 칸 → 무기 칸, 차 있으면 맞바꿈) · 빼기(무기 칸 → 장비 탭 첫 빈 칸).
    //   성공 = (CItemSystem: 칸 둘 맞바꾸기 → 대기시간 → RecalcStats) → FlushInventory(칸 패킷 둘 + 틱 끝 저장 요청) → S2C_MY_STATS.
    //   성공 알림은 따로 없다. 거절 = S2C_EQUIP_FAIL(요청 칸 번호, 사유) — 아무것도 안 바뀐다.
    void RecvEquip(CPlayer* player, CSerialBuffer* pMsg);
    void RecvUnequip(CPlayer* player, CSerialBuffer* pMsg);

    // 받기 둘의 몸 — CItemSystem::Equip → 거절 답 또는 내보내기. 끼우기·빼기가 같은 길을 탄다.
    void ApplyEquip(CPlayer* player, uint16_t slot, bool unequip);
#endif

#if USE_DROP
    // 줍기 — Z 한 번에 2칸 안에서 내가 주울 수 있는 가장 가까운 것 하나(고르는 건 서버 — 존의 관리자 FindPickup).
    //   가방 넣기(TryAdd)가 먼저, 땅에서 빼기가 나중 — 반대면 가득 찬 가방에서 아이템이 사라진다.
    void RecvPickup(CPlayer* player, CSerialBuffer* pMsg);

    // 땅 아이템 알림과 번호 부탁 (IGroundItemEvents) — 뜻은 GroundItemManager.h.
    int32_t AllocGroundId() override;                                         // _nextGroundId — 존을 넘어 번호 공간이 하나
    void OnGroundItemDrop(CZone* zone, const CGroundItem& item,
                          int32_t sectorX, int32_t sectorY) override;         // 주변에 CREATE(DROP)
    void OnGroundItemGone(CZone* zone, int32_t sectorX, int32_t sectorY,
                          int32_t groundId, GroundItemRemoval why, int32_t pickerId) override;   // 주변 + 같은 틱 이동자에게 DELETE — 와이어 값 변환은 여기
    //   두 알림의 보내기(주변 · 같은 틱 이동자 · 한 사람에게 보이기·지우기)는 CSightBroadcaster(_sightBroadcaster) — 게임 서버 나누기 3 단계
#endif

    CSessionHub&          _sessionHub;         // 한 사람 패킷(칸 · 거절)
    CSelfNotifier&        _selfNotifier;       // 마력 물약 · 장착 뒤 스탯
    CSightBroadcaster&    _sightBroadcaster;   // 체력 물약 주변 방송 · 땅 아이템 보이기·지우기
#if USE_DB_WORKER
    CCharacterStore&      _characterStore;     // 가방이 바뀐 틱 끝 저장 요청
#endif
    CItemSystem&          _itemSystem;         // 쓰기·장착 규칙
    CMapManager&          _mapManager;         // 존 찾기 · 존마다 돌기
    CGameMonitor&         _gameMonitor;        // 가방 · 드랍 지표
    const CGameDataFiles& _gameDataFiles;      // 아이템 표(줍기)
    const uint64_t&       _frameCount;         // 게임 루프의 틱 번호 — 빌린 것, 읽기만(대기시간 · 땅 수명 · 주인 시간)

#if USE_DROP
    int32_t _nextGroundId = 1;       // 땅의 아이템 번호 — 캐릭터 번호와 따로 센다(1초에 하나씩 떨어져도 68년)
    int     _groundItemScanCounter = 0;   // TickGroundItems 의 25틱 세기
#endif
};

#endif // USE_INVENTORY
