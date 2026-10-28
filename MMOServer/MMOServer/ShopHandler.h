// ==========================================================================
// CShopHandler — 상점: 사기·팔기 요청 (NPC·상점 플랜 4판 · 작업 C10 · USE_SHOP)
//
// [책임]
//  - 받은 패킷 둘의 처리기와 그 등록(RegisterRoutes — 사기 · 팔기) — 컨텐츠 모듈이 자기 줄을 등록한다
//  - 요청이 가리킨 NPC 를 주변에서 찾기(캐릭터 번호 → 그 캐릭터 · 섹터 범위 = NPC 반경 상한)
//  - 결과 내보내기 — 성공이면 가방 내보내기(FlushInventory — 칸 패킷 + S2C_MONEY + 틱 끝 저장 요청) · 거절이면 S2C_SHOP_FAIL(사유)
//
// [경계]
//  판정·적용(사유 순서 · 넣기 · 돈)은 CShopSystem. 칸·돈 내보내기와 저장 요청은 CItemHandler::FlushInventory 한 곳 —
//  가방을 바꾸는 길이 늘어도 내보내는 길은 하나다. NPC 를 깔고 알리는 것은 CNpcHandler(C9) — 이 클래스는 NPC 를 「찾기」만 한다.
//  서버는 「상점을 열었다」는 상태를 안 든다 — 요청마다 처음부터 다시 본다(멀어지면 다음 요청이 NOT_IN_REACH).
//
// [모르는 것]
//  전투 · 보상 · 파티 · 입장 · NPC 깔기. 말 걸기(인사·메뉴)는 웹 혼자다(서버 왕복 없음).
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"   // USE_SHOP — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_SHOP

class CPlayer;
class CCharacter;
class CSerialBuffer;
class CPacketRoutes;
class CSessionHub;
class CItemHandler;
class CShopSystem;
class CMapManager;
enum class ShopReject : uint8_t;

class CShopHandler
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(세션 허브 · CItemHandler · 상점 규칙 · 맵 관리자)보다 뒤에 이것을 선언해야 한다.
    CShopHandler(CSessionHub& sessionHub, CItemHandler& itemHandler, CShopSystem& shopSystem, CMapManager& mapManager)
        : _sessionHub(sessionHub), _itemHandler(itemHandler), _shopSystem(shopSystem), _mapManager(mapManager) {}

    // 받은 패킷 등록 — 사기 · 팔기. 게임 서버의 등록(RegisterRoutes)이 부른다. false = 등록 실패(기동 거절).
    //   상점을 끈 빌드(USE_SHOP 0)는 이 클래스가 없어 크기 줄도 없다 — 사고팔기 요청은 분기 표가 조용히 버린다(USE_PARTY 0 과 같은 꼴).
    bool RegisterRoutes(CPacketRoutes& routes);

private:
    void RecvShopBuy(CPlayer* player, CSerialBuffer* pMsg);
    void RecvShopSell(CPlayer* player, CSerialBuffer* pMsg);

    // 요청이 가리킨 캐릭터 번호를 그 사람 주변에서 찾는다 — 섹터 범위 = NPC 반경 상한(CNpcTable::MAX_RADIUS). 못 찾으면 nullptr.
    //   세 목록(사람 · 맞을 수 있는 것 · 그 밖)을 다 본다 — 싸우는 NPC 가 생겨도 같은 찾기다. NPC 인지는 판정(말 걸기 칸)이 가른다.
    const CCharacter* FindAround(const CPlayer* player, int32_t actorId) const;

    // 결과 내보내기 — 통과(COUNT)면 FlushInventory, 거절이면 S2C_SHOP_FAIL(와이어 값으로 옮겨서)
    void Finish(CPlayer* player, ShopReject reject);

    CSessionHub&  _sessionHub;    // 거절 패킷(본인)
    CItemHandler& _itemHandler;   // 칸·돈 내보내기 + 틱 끝 저장 요청 한 곳
    CShopSystem&  _shopSystem;    // 판정·적용
    CMapManager&  _mapManager;    // 그 사람의 존(섹터) 찾기
};

#endif // USE_SHOP
