#pragma once
// ==========================================================================
// CLobbyLoop — 로비 서버(계정·캐릭터)의 로직 스레드
//
//   코어 이벤트 꺼내기 → 구분 값으로 처리표 고르기 → DB 회신 비우기 → 주기 작업 → 지표.
//   존의 고리는 CGameServer 틱 안에 있어(GameServer.cpp ProcessNetworkEvents) 따로 둔다.
//   코어 큐에는 기다리는 함수가 없어 LogicPeriodMs 마다 확인한다.
//
//   상태는 이 스레드 하나만 만진다 — 세션 표·장부·흐름 클래스에 락이 없다.
//   세션 번호 변환과 패킷 버퍼 반납은 여기가 맡는다. 처리 함수는 SubRef 를 부르지 않는다.
//
//   코어의 _gameLoop 칸과 _gameLoopThreadHandle 을 그대로 쓴다 — thread="gameloop" 라벨이 세 서버에서 같고 기존 식이 job 만 바꿔 맞는다.
// ==========================================================================

#include "IOCPServer.h"   // 맨 앞 — 윈도 소켓 헤더 순서는 이 헤더가 맡는다. SerialBuffer.h 가 먼저 오면 Windows.h 가 옛 winsock.h 를 끌고 온다
#include "MonitorManager.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <queue>
#include <thread>

#include <Identity/GameIds.h>
#include <Link/SessionTag.h>
#include <Lobby/PacketTable.h>

class CDbPipeline;

class CLobbyLoop
{
public:
    using SessionFn = std::function<void(SessionId)>;
    using PeriodicFn = std::function<void(int64_t nowMs)>;

    CLobbyLoop(CIOCPServer& network, CMonitorManager& monitor, int periodMs);
    ~CLobbyLoop();

    CLobbyLoop(const CLobbyLoop&) = delete;
    CLobbyLoop& operator=(const CLobbyLoop&) = delete;

    // ── 기동 때 — Start 전에만 ──
    // 구분 값마다 처리표 하나. 등록 안 된 구분 값의 패킷은 오류로 세고 버린다 — 웹 포트로 온 접속이 서버 간 표를 탈 길이 없다
    void SetRoute(SessionTag tag, const CPacketTable* table, SessionFn onConnected, SessionFn onDisconnected);
    void SetDb(CDbPipeline* db) { _db = db; }
    void SetPeriodic(PeriodicFn periodic) { _periodic = std::move(periodic); }

    void Start();

    // 스레드를 내린다. 코어를 멈추기 전에 부른다 — 남은 이벤트의 버퍼를 반납하고 끝난다
    void Stop();

    // 단조 시각(ms) — 시각을 인자로 받는 부품에 넘긴다
    static int64_t NowMs();

private:
    struct Route
    {
        const CPacketTable* table = nullptr;
        SessionFn onConnected;
        SessionFn onDisconnected;
    };

    void Run();
    void ProcessEvent(NetworkEvent& event);
    void OnReceived(const Route* route, SessionId sessionId, CSerialBuffer* msg);
    const Route* FindRoute(int tag) const;

    CIOCPServer& _network;
    CMonitorManager& _monitor;
    int _periodMs;
    CDbPipeline* _db = nullptr;
    PeriodicFn _periodic;
    Route _routes[SESSION_TAG_COUNT];
    std::queue<NetworkEvent> _localEvents;

    std::atomic<bool> _running{ false };
    std::thread _thread;
};
