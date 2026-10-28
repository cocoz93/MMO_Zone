// ==========================================================================
// CPartyHandler — 파티·방: 파티 요청과 파티 알림, 방 입장·거절과 다시 들어가기 잠금 (게임 서버 나누기 7 단계 · C7 · USE_PARTY)
//
// [책임]
//  - 받은 패킷 여섯의 처리기와 그 등록(RegisterRoutes — 요청 · 답 · 나가기 · 내보내기 · 넘기기 · 해산) — 컨텐츠 모듈이 자기 줄을 등록한다
//  - 파티 알림(IPartyEvents — 요청 · 요청 끝 · 알림 한 줄 · 파티 전체 · 파티원 한 줄)을 패킷으로 바꿔 보내기 · 보낸 수 세기
//  - 방 입장(EnterRoom — 판정 · 방 만들기 · 전원 옮기기)과 거절 사유를 와이어 값으로(SendRoomReject) · 방 닫힘 세기(OnErasingZone — 열린 방 수의 짝)
//  - 방 다시 들어가기 잠금 표(_roomLocks) — 넣기(LockRoomReentry — 방 보상이 부른다) · 초마다 풀린 것 빼기(TickRoomLocks) · 표 크기 게이지
//
// [경계]
//  판정은 규칙에 맡긴다 — 파티(누가 무엇을 할 수 있나 · 요청 표 · 시간 지남 · 파티원 상태 비교)는 CPartySystem, 방 들어가기는 순수 함수 CheckRoomEntry(Room.h).
//  옮기기는 배치(CPlayerPlacement::TransferPlayer), 방 만들기는 맵 관리자(CMapManager::CreateRoom), 한 사람 패킷은 세션 허브.
//  파티 규칙 객체와 그 틱(CPartySystem::Tick)·퇴장 정리(Forget)는 게임 서버가 든다 — 이 클래스는 그 참조를 빌려 부르고, 그 알림을 받는다.
//
// [모르는 것]
//  전투·가방. 방 보상(누가 무엇을 받나)은 처치 보상 쪽이다 — 여기는 보상 뒤 잠금 넣기만 받는다.
//  포털의 반경·생존 검사는 포털 요청 쪽(RecvPortalUse)이 이미 했다. 빈 방을 언제 지울지는 맵 관리자의 빈 채널 정리다 — 여기는 지우기 직전에 센다(OnErasingZone).
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"     // USE_PARTY — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_PARTY

#include "PartySystem.h"     // IPartyEvents · Party · PartyNotice · PartyAskKind
#include "Room.h"            // CRoomLocks(멤버) · RoomEntryResult

class CPlayer;
class CZone;
class CSerialBuffer;
class CPacketRoutes;
class CSessionHub;
class CPlayerPlacement;
class CMapManager;
class CGameMonitor;
struct PortalDef;

// 파티 알림(IPartyEvents)은 공개 상속 — 게임 서버가 파티 규칙(_partySystem)에 이것을 알림 받는 쪽으로 꽂는다(CSightBroadcaster 의 IZoneEvents 와 같은 꼴).
//   알림 함수 자체는 비공개 — 규칙이 인터페이스로만 부른다.
class CPartyHandler : public IPartyEvents
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(세션 허브 · 배치 · 맵 관리자 · 게임 지표 · 틱 번호)보다 뒤에 이것을 선언해야 한다
    //   (멤버는 선언 순서로 세워진다). 파티 규칙(partySystem)만 거꾸로 이것보다 뒤에 세워진다 — 그 알림 받는 쪽이 이 클래스라서.
    //   그래서 생성 중엔 아무것도 쓰지 않는다(참조를 묶기만 — 게임 서버가 *this 를 넘기던 것과 같은 꼴).
    CPartyHandler(CSessionHub& sessionHub, CPlayerPlacement& placement, CPartySystem& partySystem,
                  CMapManager& mapManager, CGameMonitor& gameMonitor, const uint64_t& frameCount)
        : _sessionHub(sessionHub), _placement(placement), _partySystem(partySystem)
        , _mapManager(mapManager), _gameMonitor(gameMonitor), _frameCount(frameCount) {}

    // 받은 패킷 등록 — 파티 요청 여섯. 게임 서버의 등록(RegisterRoutes)이 부른다. false = 등록 실패(기동 거절).
    bool RegisterRoutes(CPacketRoutes& routes);

    // 파티 전용 방(Room.h) — RecvPortalUse 가 목적지가 방 맵이면 여기로 넘긴다(반경·생존은 이미 봤다).
    //   판정(CheckRoomEntry) → 방 만들기(CMapManager::CreateRoom) → 파티장, 그다음 가입 순서로 전원 TransferPlayer(포털 도착 자리).
    //   거절이면 아무도 안 옮기고 누른 사람에게만 알린다(SendRoomReject). 들어가기를 세는 곳은 여기 한 곳(_gameMonitor._room · _portal) —
    //   방이 닫히는 것(열린 방 −1 · 닫힘)은 아래 OnErasingZone 이 센다. 열림(+1)과 닫힘이 이 클래스 안이라 등식이 한 파일에서 닫힌다.
    void EnterRoom(CPlayer* leader, CZone* oldZone, const PortalDef& portal);

    // 존 지우기 직전(빈 채널 정리 — CGameServer::OnErasingZone 이 부른다) — 방 맵의 존이면 열린 방 −1 · 닫힘 +1.
    //   등식: 들어감(_entries[NONE]) = 닫힘 + 열림(GameMonitor.h _room). 방 번호는 이 뒤 다시 쓸 수 있다(CRoomMap::CreateRoom).
    void OnErasingZone(CZone* zone);

    // 방 다시 들어가기 잠금 넣기 — 방 보상이 사람마다 부른다(계정 · 방 맵 → 풀리는 틱). 같은 계정·맵이 있으면 덮는다(CRoomLocks::Lock).
    //   표 크기 게이지(_gameMonitor._room._locks)도 여기서 맞춘다 — 표를 바꾸는 두 길(넣기 · 빼기)이 다 이 클래스 안이라 게이지가 늘 표와 같다.
    void LockRoomReentry(int64_t accountId, int32_t mapId, uint64_t untilFrame);

    // 틱 3-2c(파티 규칙의 틱 뒤) — 초마다 풀린 잠금 빼기 · 표 크기 게이지.
    void TickRoomLocks();

private:
    // ── 파티 (USE_PARTY) ──
    // 규칙(누가 무엇을 할 수 있나 · 요청 표 · 시간 지남 · 파티원 상태 비교)은 CPartySystem(_partySystem) — 여기는 받기와, 알림을 패킷으로 바꿔 보내기만.
    //   받기 여섯 — 요청은 누른 사람을 주변 9섹터에서 찾아(FindPlayerAround) 넘기고, 나머지는 번호를 그대로 넘긴다.
    void RecvPartyRequest(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPartyAnswer(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPartyLeave(CPlayer* player);
    void RecvPartyKick(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPartyLeader(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPartyDisband(CPlayer* player);

    // 내 주변 9섹터(시야 — 내 화면에 그려질 수 있는 곳)의 사람 중 이 번호 — 없으면 nullptr. 몬스터는 안 본다(플레이어 목록만).
    //   서버에 번호 → 사람 표가 없어서(세션 번호로만 찾는다 — FindPlayer) 화면에서 누른 사람을 여기서 찾는다. 「화면에 보이는 사람」 검사를 겸한다.
    CPlayer* FindPlayerAround(CPlayer* player, int32_t actorId);

    // 파티 알림 (IPartyEvents) — 뜻은 PartySystem.h. 보내는 수는 여기서 센다(_gameMonitor._party 의 …Sent).
    void OnPartyAsk(CPlayer* to, uint32_t requestId, PartyAskKind kind, const CPlayer* from, const CPlayer* about, uint32_t remainMs) override;
    void OnPartyAskEnd(CPlayer* to, uint32_t requestId, PartyNotice why) override;
    void OnPartyNotice(CPlayer* to, PartyNotice code, int32_t actorId) override;
    void OnPartyInfo(CPlayer* to, const Party* party) override;
    void OnPartyMember(CPlayer* to, const CPlayer* member) override;

    // 거절 사유 → 와이어 — 파티 없음·파티장 아님은 파티 알림의 그 두 번호, 다른 존·쓰러짐·잠금은 방 알림 셋(막는 사람 번호),
    //   번호 다 참은 S2C_ZONE_CHANGE_FAIL 1(「모든 채널 가득 참」). 파티 알림은 파티 규칙의 Notice 로 보낸다(번호별 계수가 거기 한 곳)
    void SendRoomReject(CPlayer* leader, const RoomEntryResult& result);

    CSessionHub&      _sessionHub;    // 파티 패킷(한 사람에게)
    CPlayerPlacement& _placement;     // 방으로 옮기기 · 존 이동 실패(번호 다 참)
    CPartySystem&     _partySystem;   // 파티 규칙 — 빌린 것(게임 서버가 들고, 이 클래스보다 뒤에 세워진다)
    CMapManager&      _mapManager;    // 존 찾기(주변 9섹터) · 방 만들기
    CGameMonitor&     _gameMonitor;   // 파티 보내기 · 포털 · 방 지표
    const uint64_t&   _frameCount;    // 게임 루프의 틱 번호 — 빌린 것, 읽기만(요청 시간 · 잠금 판정 · 쓸기 주기)

    // 방 다시 들어가기 잠금(계정 · 방 맵 → 풀리는 틱) — 보상을 줄 때 넣고(OnDeath 방 분기의 방 보상 → LockRoomReentry) 들어가기 판정이 본다. 서버 메모리만(재시작하면 풀림).
    //   풀린 것은 파티 틱 옆에서 초마다 훑어 뺀다(TickRoomLocks — 게임 루프 3-2c)
    CRoomLocks _roomLocks;
};

#endif // USE_PARTY
