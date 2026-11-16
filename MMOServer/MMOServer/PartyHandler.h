// ==========================================================================
// CPartyHandler — 파티·방: 파티 요청을 월드 서버로 넘기고, 월드 서버의 알림을 웹 패킷으로 옮기고, 방 입장·거절을 맡는다 (게임 서버 나누기 7 단계 · C7 · USE_PARTY)
//
// [책임]
//  - 받은 패킷 여섯의 처리기와 그 등록(RegisterRoutes — 요청 · 답 · 나가기 · 내보내기 · 넘기기 · 해산) — 컨텐츠 모듈이 자기 줄을 등록한다
//  - 월드 서버가 보낸 파티 패킷 다섯을 받아 사본(CPartyCopy)을 고치고 S2C_PARTY_* 로 보내기
//  - 이 존 서버 사람의 상태를 0.5초마다 사본과 견줘 바뀐 줄을 월드 서버로 보내기 · 월드 서버가 다시 붙으면 소속을 증언하기
//  - 방 입장(EnterRoom — 판정 · 방 만들기 · 전원 옮기기)과 거절 사유를 와이어 값으로(SendRoomReject) · 방 닫힘 세기(OnErasingZone — 열린 방 수의 짝)
//
// [경계]
//  규칙(누가 무엇을 할 수 있나 · 요청 표 · 시간 지남)은 월드 서버 CPartySystem(WorldServer/PartySystem.h), 방 들어가기는 순수 함수 CheckRoomEntry(Room.h).
//  옮기기는 배치(CPlayerPlacement::TransferPlayer), 방 만들기는 맵 관리자(CMapManager::CreateRoom), 한 사람 패킷은 세션 허브.
//
// [월드 서버가 없을 때] 사본은 그대로 두고 바꾸는 요청만 NO_WORLD 로 거절한다. 파티원 거르기 · 보스방은 사본으로 계속 돈다.
//
// [모르는 것]
//  전투·가방. 방 보상(누가 무엇을 받나)은 처치 보상 쪽이다 — 여기는 보상 뒤 잠금 넣기만 받는다.
//  포털의 반경·생존 검사는 포털 요청 쪽(RecvPortalUse)이 이미 했다. 빈 방을 언제 지울지는 맵 관리자의 빈 채널 정리다 — 여기는 지우기 직전에 센다(OnErasingZone).
// ==========================================================================
#pragma once

#include <Identity/GameIds.h>
#include <cstdint>
#include <unordered_map>

#include "BuildConfig.h"     // USE_PARTY — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_PARTY

#include "PartyCopy.h"       // Party · CPartyCopy
#include "Room.h"            // RoomEntryResult

class CPlayer;
class CZone;
class CSerialBuffer;
class CPacketRoutes;
class CSessionHub;
class CPlayerPlacement;
class CMapManager;
class CGameMonitor;
class CWorldLink;
struct PortalDef;

class CPartyHandler
{
public:
    // 파티원 상태 비교 주기 — 12틱(약 0.5초). 파티가 없으면 비교가 바로 끝난다(부하 시험엔 파티가 없다).
    static constexpr uint64_t MEMBER_SCAN_FRAMES = FRAME_PER_SEC / 2;

    // ★ 참조만 든다 — 생성 중엔 아무것도 쓰지 않는다. _worldLink 는 CGameServer 에서 이 클래스보다 뒤에 세워진다
    CPartyHandler(CSessionHub& sessionHub, CPlayerPlacement& placement, CWorldLink& worldLink,
                  CMapManager& mapManager, CGameMonitor& gameMonitor, const uint64_t& frameCount)
        : _sessionHub(sessionHub), _placement(placement), _worldLink(worldLink)
        , _mapManager(mapManager), _gameMonitor(gameMonitor), _frameCount(frameCount) {}

    // 보이는 채널 번호를 만드는 값 — Start 전
    void Configure(int zoneServerId, int channelBlock) { _zoneServerId = zoneServerId; _channelBlock = channelBlock; }

    // 받은 패킷 등록 — 파티 요청 여섯 · 월드 서버 패킷 다섯. 게임 서버의 등록(RegisterRoutes)이 부른다. false = 등록 실패(기동 거절).
    bool RegisterRoutes(CPacketRoutes& routes);

    // 파티 전용 방(Room.h) — RecvPortalUse 가 목적지가 방 맵이면 여기로 넘긴다(반경·생존은 이미 봤다).
    //   판정(CheckRoomEntry) → 방 만들기(CMapManager::CreateRoom) → 파티장, 그다음 가입 순서로 전원 TransferPlayer(포털 도착 자리).
    //   거절이면 아무도 안 옮기고 누른 사람에게만 알린다(SendRoomReject). 들어가기를 세는 곳은 여기 한 곳(_gameMonitor._room · _portal) —
    //   방이 닫히는 것(열린 방 −1 · 닫힘)은 아래 OnErasingZone 이 센다. 열림(+1)과 닫힘이 이 클래스 안이라 등식이 한 파일에서 닫힌다.
    //   파티장이 오프라인이면 누른 파티원이 파티장을 맡고 이번 입장은 끝난다 — 맡은 뒤 포털을 다시 누른다.
    void EnterRoom(CPlayer* leader, CZone* oldZone, const PortalDef& portal);

    // 존 지우기 직전(빈 방 정리 — CGameServer::OnErasingZone 이 방 맵의 존일 때만 부른다) — 열린 방 −1 · 닫힘 +1.
    //   등식: 들어감(_entries[NONE]) = 닫힘 + 열림(GameMonitor.h _room). 방 번호는 이 뒤 다시 쓸 수 있다(CRoomMap::CreateRoom).
    void OnErasingZone(CZone* zone);

    void Tick();                    // 틱마다 — MEMBER_SCAN_FRAMES 마다 상태 비교
    void AddPlayer(CPlayer* player);   // 입장 — 입장 알림(CWorldLink::NotifyEnter)보다 앞. 명단이 왔을 때 이 사람을 찾을 수 있어야 한다
    void Forget(CPlayer* player);      // 퇴장

    void OnWorldUp();     // 등록을 보낸 뒤 — 이 존 서버 사람의 소속을 증언한다
    void OnWorldDown();   // 열린 요청 창을 닫는다 — 월드 서버의 요청 표가 사라졌을 수 있다

    // /party 이름 — 월드 서버가 장부에서 이름을 찾는다
    void InviteByName(CPlayer* me, const char16_t (&name)[LINK_NAME_CHARS]);

private:
    // ── 플레이어 요청 — 월드 서버로 넘긴다 ──
    void RecvPartyRequest(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPartyAnswer(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPartyLeave(CPlayer* player);
    void RecvPartyKick(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPartyLeader(CPlayer* player, CSerialBuffer* pMsg);
    void RecvPartyDisband(CPlayer* player);

    // 내 주변 9섹터(시야 — 내 화면에 그려질 수 있는 곳)의 사람 중 이 번호 — 없으면 nullptr. 몬스터는 안 본다(플레이어 목록만).
    //   화면에서 누른 사람을 여기서 찾는다. 「화면에 보이는 사람」 검사를 겸한다.
    CPlayer* FindPlayerAround(CPlayer* player, ActorId actorId);

    // 월드 서버로 보낸다 — 끊겨 있으면 누른 사람에게 NO_WORLD. op 의 op · 보낸 사람 · 레벨은 여기서 채운다
    void SendOp(CPlayer* me, LinkPartyOpKind kind, LINK_PARTY_OP op);

    // ── 월드 서버 패킷 ──
    void RecvWorldAsk(CSerialBuffer* msg);
    void RecvWorldAskEnd(CSerialBuffer* msg);
    void RecvWorldNotice(CSerialBuffer* msg);
    void RecvWorldInfo(CSerialBuffer* msg);
    void RecvWorldMember(CSerialBuffer* msg);

    ActorId ActorOf(const LinkPartyPerson& person) const;   // 이 존 서버에 있으면 액터 번호, 아니면 0
    int     VisibleChannelOf(const CPlayer* player) const;  // 방 맵은 0
    LinkPartyMemberRow LiveRow(const Party::Member& m) const;   // 이 존 서버 사람은 지금 값, 아니면 월드 서버가 보낸 값

    CSerialBuffer* MakePartyInfo(const Party* party) const;
    CSerialBuffer* MakePartyMember(const Party::Member& m) const;
    void SendMemberToLocals(const Party& party, const Party::Member& m);
    void SendInfoToLocals(const Party& party);

    // 존 서버가 정한 거절 — 번호별로 센다. 월드 서버가 낸 알림은 월드 서버가 세므로 이 길을 안 지난다
    void NoticeLocal(CPlayer* to, PartyNotice code, const LinkPartyPerson& who, ActorId actorId);

    // 사본과 지금 값을 견준다 — 바뀐 줄을 batch 에 쌓아 꽉 차면 보낸다. 월드 서버가 끊겨 있으면 이 존 서버의 파티원에게 직접 보낸다
    void ScanParty(Party& party, LinkPartyStateRow* batch, int& count);
    void ScanAll();

    // 거절 사유 → 와이어 — 파티 없음·파티장 아님은 파티 알림의 그 두 번호, 다른 존·쓰러짐·잠금은 방 알림 셋(막는 사람),
    //   번호 다 참은 S2C_ZONE_CHANGE_FAIL 1(「모든 채널 가득 참」).
    void SendRoomReject(CPlayer* leader, const RoomEntryResult& result);

    CSessionHub&      _sessionHub;    // 파티 패킷(한 사람에게)
    CPlayerPlacement& _placement;     // 방으로 옮기기 · 존 이동 실패(번호 다 참)
    CWorldLink&       _worldLink;     // 월드 서버로 보내기
    CMapManager&      _mapManager;    // 존 찾기(주변 9섹터) · 방 만들기
    CGameMonitor&     _gameMonitor;   // 파티 거절 · 포털 · 방 지표
    const uint64_t&   _frameCount;    // 게임 루프의 틱 번호 — 빌린 것, 읽기만(쓸기 주기)

    CPartyCopy _copy;
    std::unordered_map<int64_t, uint32_t> _openAsks;   // 답을 기다리는 창 — 캐릭터 번호 → 요청 번호(받는 사람마다 하나)
    uint64_t _lastScanFrame = 0;
    int      _zoneServerId = 0;
    int      _channelBlock = 0;
};

#endif // USE_PARTY
