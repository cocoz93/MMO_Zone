// ==========================================================================
// CSessionHub — 세션 칸 · 플레이어 찾기 · 보내기 · 캐릭터 번호 발급 (게임 서버 나누기 1 단계 · B1)
//
// [책임]
//  - 세션 칸: 세션 번호 → 플레이어. 칸이 플레이어의 주인이다(unique_ptr) — 넣기(AddPlayer) · 비우기(RemovePlayer) ·
//    전원 해제(RemoveAll)가 수명을 정한다. 칸에 넣을 때 플레이어에 세션 번호를 붙인다 — 찾기의 재사용 가드가 그 번호를 본다.
//  - 플레이어 찾기(세션 번호로) · 전원 돌기 · 첫 일치 찾기
//  - 한 사람에게 보내기(SendPacket) — 보낸 수·바이트를 코어 지표로 센다
//  - 수신자 한 명에게 한 장 넣기(RequestSend · RequestSendRaw) — 방송(시야 조각)이 수신자마다 부른다. 소유권·지표는 부르는 쪽(3 단계)
//  - 캐릭터 번호 발급 — 플레이어와 몬스터가 한 번호 공간을 쓴다(몬스터 관리자의 번호 부탁도 여기로 온다)
//
// [경계]
//  네트워크는 빌려 쓴다 — 만들고 띄우고 내리는 것은 게임 서버(B0)이고, 여기는 Init 에서 받은 포인터로 보내기만 한다.
//  주변에 보내기(방송)·묶어 보내기는 시야 조각 몫이다 — 여기는 한 사람에게 한 패킷(받는 사람 고르기·소유권 일괄 확보는 방송 쪽).
//  접근은 게임 스레드 단독이다(잠금 없음).
//
// [모르는 것]
//  존·섹터·패킷 내용·DB·입장 절차를 모른다. 언제 넣고 언제 비우는지(입장·떠나보내기 절차)는 부르는 쪽이 정한다.
// ==========================================================================
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "NetIoModel.h"   // 컴파일타임 I/O 모델 별칭 (IOCP/epoll) — CSession::ExtractIndex 도 여기서 온다
#include "Player.h"

class CMonitorManager;
class CSerialBuffer;

class CSessionHub
{
public:
    // ★ 지표 참조만 든다 — 부르는 쪽(CGameServer)은 이것을 _monitor 보다 뒤에 선언해야 한다(멤버는 선언 순서로 세워진다).
    explicit CSessionHub(CMonitorManager& monitor) : _monitor(monitor) {}

    // ── 세션 칸 ──

    // 칸 준비 — 게임 서버 모드의 Init 이 부른다. 칸 수 = maxClients(세션 색인과 같은 범위).
    void Init(NetIoModel* network, int maxClients);

    // 전원 해제 — 종료(Stop)가 부른다. 칸이 주인이라 비우면 해제된다.
    void RemoveAll();

    // 칸에 넣기 — 세션 번호를 붙이고 칸이 주인이 된다.
    //   그 칸에 아직 살아 있는 플레이어가 있으면(있을 수 없는 경우) 옛 것은 해제하지 않고 놓아 버린다 — 몸의 주석.
    void AddPlayer(int64_t sessionId, std::unique_ptr<CPlayer> owned);

    // 칸 비우기 — 주인이 칸이라 여기서 해제된다. 떠나보내기의 맨 끝이어야 한다(그 뒤로 player 를 못 쓴다).
    void RemovePlayer(CPlayer* player);

    // 칸 수(= maxClients) — 전원 저장이 배치를 미리 잡는 데 쓴다
    size_t SlotCount() const { return _slots.size(); }

    // 슬롯 조회 + 재사용 가드 — 등록된 세션이 아니면 nullptr
    CPlayer* FindPlayer(int64_t sessionId) const
    {
        uint16_t idx = CSession::ExtractIndex(sessionId);
        // [불변식] idx < _slots.size() — 슬롯 배열은 Init에서 maxClients 칸으로 잡히고,
        //   세션 인덱스는 같은 maxClients로 잡힌 CIOCPServer 세션 배열의 인덱스라 항상 그 미만이다.
        //   ※ 바로 아래가 배열 인덱싱이다. 이 불변식이 깨지면 릴리즈에서는 곧장 범위 밖 접근이 된다
        //     (다른 불변식 assert들과 달리 실패 대가가 메모리 손상). 슬롯 크기 산정이나 maxClients
        //     전달 경로를 손보면 반드시 디버그 빌드로 여기를 통과시킬 것.
        assert(idx < _slots.size());
        CPlayer* p = _slots[idx].get();
        return (p != nullptr && p->_sessionId == sessionId) ? p : nullptr;
    }

    // 전원 돌기 — 칸 차례대로, 빈 칸은 건너뛴다. fn(CPlayer*) 안에서 칸을 비우면 안 된다(그럴 땐 아래 FindPlayerIf).
    template <typename Fn>
    void ForEachPlayer(Fn&& fn) const
    {
        for (const auto& slot : _slots)
        {
            CPlayer* player = slot.get();
            if (player == nullptr)
                continue;
            fn(player);
        }
    }

    // 첫 일치 찾기 — 칸 차례대로 pred(CPlayer*) 가 참인 첫 플레이어, 없으면 nullptr.
    //   찾은 뒤에 그 플레이어를 떠나보내도 된다(돌기가 이미 끝났다).
    template <typename Pred>
    CPlayer* FindPlayerIf(Pred&& pred) const
    {
        for (const auto& slot : _slots)
        {
            CPlayer* player = slot.get();
            if (player != nullptr && pred(player))
                return player;
        }
        return nullptr;
    }

    // ── 보내기 ──
    // 한 사람에게 가는 S2C는 직렬화 버퍼(CSerialBuffer)로 조립 후 아래로 전송한다(주변 방송은 CSightBroadcaster::BroadcastAroundSector).

    // 단일 플레이어에게 패킷 전송 (CSerialBuffer — 소유권 1을 소비)
    // 빌더가 반환한 RefCount=1 버퍼를 넘기면, 송신 후 SubRef로 소유권을 회수한다.
    //   받는 쪽은 CPlayer* 뿐이다 — 몬스터를 넘기면 컴파일이 안 된다(Player.h [세션]).
    void SendPacket(CPlayer* target, CSerialBuffer* pMsg);

    // 수신자 한 명에게 한 장 넣기 — 방송(시야 조각)이 수신자마다 부른다(게임 서버 나누기 3 단계).
    //   소유권(AddRef/SubRef)과 보낸 수·바이트 세기는 부르는 쪽 몫이다 — 방송은 소유권을 한 번에 잡고(배치 AddRef) 지표를
    //   모아 한 번에 센다(SendPacket 과 다른 까닭). 이것을 따로 두는 까닭은 세션 번호를 아는 곳을 여기 하나로 두려는 것.
    //   돌려주는 값 = 송신 큐에 들어갔나(네트워크 그대로). 게임 루프 송신이라 늘 Deferred(SendPacket 과 같다).
    bool RequestSend(CPlayer* target, CSerialBuffer* pMsg)
    {
        return _network->RequestSendMsg(target->_sessionId, pMsg, SendFlush::Deferred);
    }
    // 연접한 바이트 덩어리 넣기 — 수신섹터 digest(USE_BROADCAST_BUNDLE)가 주민마다 부른다. 버퍼 소유권이 없다(바이트 복사).
    bool RequestSendRaw(CPlayer* target, const char* data, int size)
    {
        return _network->RequestSendRaw(target->_sessionId, data, size);
    }

    // ── 캐릭터 번호 발급 ──
    //   플레이어와 몬스터가 같이 쓴다(몬스터는 CGameServer::AllocActorId — IMonsterEvents 를 거쳐 온다).
    //   단일 증가 카운터라 둘이 충돌하지 않는다. int32 라 2^31 위로 못 올라온다(지정 계정 번호 하한 ACCOUNT_ID_MIN 이 여기에 기댄다).
    int32_t AllocActorId() { return _nextActorId++; }

private:
    CMonitorManager& _monitor;        // 코어 지표 — 보낸 수·바이트(SendPacket)
    NetIoModel*      _network = nullptr;   // 빌린 것 — 주인은 CGameServer(Init 에서 받는다)

    // 네트워크 경계: sessionId → CPlayer* (수신 시 플레이어 조회)
    //   sessionId 상위 16비트 = 세션 슬롯 인덱스 → 해시맵 대신 인덱스 배열 직접 접근 (캐시미스 제거).
    //   슬롯 재사용(ABA)은 FindPlayer의 _sessionId 일치 검사로 방어. 접근은 게임 스레드 단독.
    //   ★ 플레이어의 주인이 이 슬롯이다(unique_ptr) — 손으로 delete 하지 않는다. 게임 중인 플레이어를 놓는 길은
    //     RemovePlayer(떠나보내기 끝 — CGameServer::ReleasePlayer) 하나, 종료는 RemoveAll 이 해제한다.
    //     입장 중 슬롯에 넣기 전까지는 CGameServer::SpawnPlayer 의 지역 unique_ptr 가 든다.
    std::vector<std::unique_ptr<CPlayer>> _slots;

    int32_t _nextActorId = 1;  // 전역 캐릭터 번호 카운터 (싱글스레드 게임 루프)
};
