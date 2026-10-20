// ==========================================================================
// CPlayerPlacement — 배치: 존에 넣기·옮기기·빼기·떠나보내기와 도착 차례 (게임 서버 나누기 5 단계 · B4)
//
// [책임]
//  - 존에서 존으로 옮기기 한 곳(TransferPlayer) · 존에서 빼기 한 곳(RemoveFromZone — 플레이어·몬스터 공용)
//  - 떠나보내기 한 곳(LeaveGame: 존에서 빼기 → 놓기) · 놓기(최종 저장 → 「떠남」 알림 → 칸 비우기)
//  - 도착 차례(NotifyArrival) — 존 정보 → 내 캐릭터 → 본인 상태 → 주변과 서로 생성. ★ 이 차례가 곧 와이어 차례다
//
// [경계]
//  틱 너머로 캐릭터를 드는 컨텐츠 목록(부활 대기열 · 지속 효과 · 파티)은 직접 안 만진다 — 「존에서 빠짐 · 떠남」 알림(IPlacementEvents)으로
//  알리고 받는 쪽이 뺀다(게임 서버 나누기 0-③ ⑴⑶). 차례는 알림 앞뒤로 지킨다(시야 몫 → 알림 → 존 소속 · 저장 → 알림 → 칸 해제).
//  보내기는 세 조각에 맡긴다 — 한 사람 패킷(세션 허브) · 본인 상태(본인 알림) · 주변(시야 방송). 저장은 캐릭터 저장소
//  (단, 존 이동의 저장 표시 _dbDirty 는 TransferPlayer 가 직접 켠다 — 바꾼 쪽은 표시만, 언제 쓰는지는 저장소가 정한다 · 저장 플랜 C).
//  들어갈 수 있는지(목적지·정원·같은 존·포털 규칙)는 부르는 쪽이 이미 봤다 — 입장은 실패가 없다(CZone::EnterZone 주석).
//
// [모르는 것]
//  입장 판정(대기표·불러오기) · 존 이동 요청 판정 · 전투·파티 규칙. 어느 길로 왔는지는 도착 종류(ArrivalKind) 하나로만 받는다.
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"   // USE_DB_WORKER — 안 읽으면 아래 #if 가 조용히 0 이 된다

class CPlayer;
class CCharacter;
class CZone;
class CMapManager;
class CGameMonitor;
class CSessionHub;
class CSelfNotifier;
class CSightBroadcaster;
#if USE_DB_WORKER
class CCharacterStore;
#endif

// 배치가 캐릭터를 내보낼 때 알리는 통로 — 틱 너머로 그 캐릭터의 주소를 드는 목록이 거기서 뺀다(빼지 않으면 다음 틱이 해제된 메모리를 밟는다).
//   CGameServer 가 구현한다 — 정리할 목록이 전투 조각 · 효과 규칙 · 파티 규칙에 흩어져 있어 게임 서버가 받아 차례대로 나눠 부른다(게임 서버 나누기 9 단계 결정). 순수 가상이라 빠뜨리면 컴파일이 안 된다.
class IPlacementEvents
{
public:
    virtual ~IPlacementEvents() = default;

    // 존에서 빠지려 한다 — 시야 몫(주변 DELETE · 이동 묶음 빼기)은 끝났고 존 소속(CZone::LeaveZone)은 아직이다.
    //   종류를 안 가린다(플레이어 퇴장·존 이동 · 죽은 몬스터 · 사라지는 몬스터). 존 이동도 이 길을 지난다.
    virtual void OnLeavingZone(CCharacter* who) = 0;

    // 세계를 떠나려 한다 — 최종 저장은 끝났고 칸 비우기(객체 해제)는 아직이다. 존 이동·죽음은 이 길을 안 지난다.
    //   알림을 보낼 세션은 이미 없다(받는 쪽은 이 사람에게 아무것도 보내지 않는다).
    virtual void OnLeavingGame(CPlayer* player) = 0;
};

class CPlayerPlacement
{
public:
    // 존에 들어간 뒤 알리는 길 — 어떻게 왔나가 셋을 가른다: 내 캐릭터를 알리는 패킷 · 가방·스탯까지 보내나 · 등장 사유.
    //   되돌리기 길은 없다 — 존 이동은 확인을 다 끝낸 뒤 옮기므로 들어가다 실패하지 않는다(CZone::EnterZone 주석).
    enum class ArrivalKind : uint8_t
    {
        CONNECT,       // 처음 접속 — CREATE_MY · 가방 · 공격력/방어력까지, 사유 CONNECT
        ZONE_CHANGE,   // 존 이동(요청·포털) — ZONE_CHANGE_OK 가 내 캐릭터를 알림, 사유 ZONE_TRANSFER
        RESPAWN,       // 마을 부활 — 존 이동과 같고 ZONE_CHANGE_OK 바로 뒤에 본인에게 RESPAWN(체력·되살아남)
        COUNT
    };
    // 옮길 자리 — 없으면(nullptr) 그 존의 「자리 없이 들어오는」 규칙(TransferPlayer 주석)
    struct SpawnAt { float x; float y; };

    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(세 보내기 조각 · 저장소 · 맵 관리자 · 게임 지표)보다 뒤에 이것을 선언해야 한다
    //   (멤버는 선언 순서로 세워진다). 생성 중엔 쓰지 않는다.
    CPlayerPlacement(CSessionHub& sessionHub, CSelfNotifier& selfNotifier, CSightBroadcaster& sightBroadcaster,
#if USE_DB_WORKER
                     CCharacterStore& characterStore,
#endif
                     CMapManager& mapManager, CGameMonitor& gameMonitor, IPlacementEvents& events)
        : _sessionHub(sessionHub), _selfNotifier(selfNotifier), _sightBroadcaster(sightBroadcaster)
#if USE_DB_WORKER
        , _characterStore(characterStore)
#endif
        , _mapManager(mapManager), _gameMonitor(gameMonitor), _events(events) {}

    // 순서 = 존 정보 → 내 캐릭터 → 레벨·경험치 → (접속만) 가방 전체 → 공격력·방어력 → 주변과 서로 생성(BroadcastEnterZone).
    //   ★ 이 순서가 곧 와이어 순서다 — 클라가 존·나를 알고 나서 숫자와 남을 채운다. 본인 전용 패킷이 늘면 여기 한 곳에.
    void NotifyArrival(CPlayer* player, CZone* zone, ArrivalKind kind);

    // 존에서 존으로 옮기기 한 곳 — 존 이동 요청(RecvZoneChange)이 부른다. 다른 길로 존을 옮길 일이 생기면 이 함수를 탄다.
    //   퇴장 방송 → 새 존 입장 → 존 이동 수 → 저장 표시 → 동기화 기준 → 도착 알림(kind).
    //   at == nullptr 이면 그 존의 「자리 없이 들어오는」 규칙(CZone::EnterZone → CalcSpawnPos), 있으면 그 자리(EnterZoneAt).
    //   들어갈 수 있는지(목적지·정원·같은 존)는 부르는 쪽이 이미 봤다 — 입장은 실패가 없다(CZone::EnterZone 주석).
    void TransferPlayer(CPlayer* player, CZone* oldZone, CZone* newZone, ArrivalKind kind, const SpawnAt* at);

    //   RemoveFromZone     — 존에서 빼기: 시야 몫(시야 조각의 BroadcastLeaveZone) → 「존에서 빠짐」 알림(부활 대기열 정리) → CZone::LeaveZone. 종류를 안 가린다 —
    //                        플레이어 퇴장·존 이동과 죽은 몬스터(OnDeath 끝)·사라지는 몬스터가 같이 쓴다
    void RemoveFromZone(CZone* zone, CCharacter* player);

    // 떠나보내기 — 게임 중인 플레이어를 세계에서 내보내는 한 곳: 존에서 빼기(존을 찾으면) → 놓기(ReleasePlayer).
    //   끊김(OnDisconnected)과 같은 계정 재접속(KickDuplicateAccount)이 같이 부른다. 부른 뒤에는 player 를 쓰지 말 것.
    void LeaveGame(CPlayer* player);

    // 존 이동 실패 통보(S2C_ZONE_CHANGE_FAIL) — 존 이동 요청·포털·방 입장 거절이 부른다. reason 은 와이어 값 그대로(Protocol.h 의 그 구조체 — 고르는 것은 부르는 쪽).
    void SendZoneChangeFail(CPlayer* target, uint8_t reason);

private:
    // 게임 중인 플레이어를 놓는 한 곳 — 존에서 나간 "뒤"에 부른다(지금은 떠나보내기 한 곳. 존 이동 복귀 실패 길은 10-09 에 없앴다 —
    //   존 이동은 확인을 다 끝내고 옮겨서 들어가다 실패하지 않는다, CZone::EnterZone 주석).
    //   최종 저장 → 틱 너머로 이 플레이어를 드는 목록에서 빼기(「떠남」 알림) → 슬롯 비우기(여기서 해제), 이 순서
    //   (저장이 먼저인 까닭은 몸의 ★ 주석 — 5 단계에서 옮기며 옛 주석의 「빼기 → 저장」 차례를 코드에 맞춰 고침).
    //   ★ 존 퇴장(RemoveFromZone)은 부르는 쪽 몫 — 존을 못 찾은 끊김(LeaveGame 의 zone == nullptr)은 퇴장 없이 놓기만 한다.
    //     CZone::LeaveZone 은 _zoneId 를 안 지우므로, 이미 존을 나간 길이 다시 생겨 이 안에서 _zoneId 로 나가면 같은 존을 두 번 나간다.
    //   ★ 부른 뒤에는 player 를 쓰지 말 것 — 이미 해제됐다.
    void ReleasePlayer(CPlayer* player);

    // 패킷별 전송 함수 (Fill + Send)
    void SendZoneInfo(CPlayer* target, CZone* zone);
    void SendCreateMyPlayer(CPlayer* target);
    void SendZoneChangeOk(CPlayer* target, int32_t mapId, int32_t channelIndex);

    CSessionHub&       _sessionHub;         // 한 사람 패킷 · 칸 비우기
    CSelfNotifier&     _selfNotifier;       // 도착 차례의 본인 상태 패킷
    CSightBroadcaster& _sightBroadcaster;   // 퇴장의 시야 몫 · 입장의 주변과 서로 생성
#if USE_DB_WORKER
    CCharacterStore&   _characterStore;     // 놓기 전 최종 저장
#endif
    CMapManager&       _mapManager;         // 떠나보내기의 존 찾기
    CGameMonitor&      _gameMonitor;        // 존 이동 수
    IPlacementEvents&  _events;             // 「존에서 빠짐 · 떠남」 — 틱 너머 목록을 든 쪽이 뺀다
};
