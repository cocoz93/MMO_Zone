#pragma once
#include "WinSockDef.h"
#include <cstdint>
#include <string>
#include <random>
#include "../../../ServerCore/Base/RingBuffer.h"
#include "../../../Shared/Protocol/Protocol.h"

struct StatsLocal;
struct MMOStressConfig;

enum class ClientState { DISCONNECTED, CONNECTING, CONNECTED };

class DummyClient
{
public:
    DummyClient()  = default;
    ~DummyClient() { CloseSocket(); }

    // DummyManager가 상태 전이를 트리거
    void StartConnect(const std::string& ip, int port, StatsLocal& stats, int reconnectDelayMs);
    void OnConnected(StatsLocal& stats, int reconnectDelayMs);
    void OnConnectFailed(StatsLocal& stats, int reconnectDelayMs);

    // select()가 읽기 가능 표시 시 호출
    void OnRecv(StatsLocal& stats, int reconnectDelayMs);

    // OnRecv 후 RingBuffer에서 완성된 패킷 파싱
    void ProcessPackets(StatsLocal& stats, const MMOStressConfig& config);

    // 40ms 도달 시 행동 (이동/정지)
    void Tick(StatsLocal& stats, const MMOStressConfig& config, int64_t nowMs);

    // 하트비트 (20초 주기)
    void CheckHeartbeat(StatsLocal& stats, int heartbeatIntervalSec, int64_t nowMs);

    // 송신 링버퍼 → 실제 send()
    void FlushSend(StatsLocal& stats, int reconnectDelayMs);

    bool IsReadyToConnect(int64_t nowMs) const;
    bool IsConnectTimedOut(int64_t nowMs, int timeoutMs) const
    {
        return _state == ClientState::CONNECTING
            && (nowMs - _connectStartMs) >= timeoutMs;
    }
    bool IsReady()        const { return _ready; }
    bool IsConnected()    const { return _state == ClientState::CONNECTED;    }
    bool IsConnecting()   const { return _state == ClientState::CONNECTING;   }
    bool IsDisconnected() const { return _state == ClientState::DISCONNECTED; }
    SOCKET GetSocket()    const { return _sock; }

    // 고해상도 단조 증가 ms 클럭 (QPC 기반).
    // RTT 측정과 틱/타임아웃 타이밍이 동일 클럭을 쓰도록 매니저도 이걸 사용.
    static int64_t NowMs();

private:
    void CloseSocket();
    void Disconnect(StatsLocal& stats, int reconnectDelayMs);
    void ResetState();

    // 패킷 핸들러
    void HandleZoneInfo(const char* packet);
    void HandleCreateMyPlayer(const char* packet);
    void HandleMoveStart(const char* packet);
    void HandleMoveStop(const char* packet);
    void HandleSyncPosition(const char* packet);
    void HandleCreateOtherPlayer(const char* packet);
    void HandleDeletePlayer(const char* packet);
    void HandleChat(const char* packet);
    void HandleSectorUpdates(const char* packet);
    void HandleZoneChangeOk(const char* packet);
    void HandleZoneChangeFail(const char* packet);

    // 이동 로직
    void UpdateLocalPosition(float deltaTime, int mapWidth, int mapHeight);
    void SendMoveStart(StatsLocal& stats);
    void SendMoveStop(StatsLocal& stats);
    void SendHeartbeat(StatsLocal& stats);
    void SendChat(StatsLocal& stats, int64_t nowMs);
    void SendZoneChange(StatsLocal& stats, int targetMapId);

    // 패킷 크기 테이블
    static uint16_t GetPacketSize(MsgType type);

    SOCKET      _sock           = INVALID_SOCKET;
    ClientState _state          = ClientState::DISCONNECTED;
    int64_t     _connectReadyMs = 0;
    int64_t     _connectStartMs = 0;   // CONNECTING 진입 시각 (타임아웃 감지용)

    // 게임 상태
    bool        _ready          = false;
    int32_t     _playerId       = 0;
    float       _x              = 0.0f;
    float       _y              = 0.0f;
    int32_t     _speed          = 0;
    uint8_t     _direction      = 0;   // Direction enum (0=NONE, 1=UP ~ 4=RIGHT)
    bool        _moving         = false;

    // 서버에서 수신한 맵 크기 (S2C_ZONE_INFO)
    int32_t     _mapWidth       = 0;
    int32_t     _mapHeight      = 0;

    // 타이밍
    int64_t     _lastTickMs      = 0;
    int64_t     _lastHeartbeatMs = 0;
    // RTT 측정 (채팅 왕복): 서버가 발신자 본인에게도 S2C_CHAT을 돌려주는 것을 이용.
    // 한 번에 1개만 측정(single outstanding) → 보낸 채팅과 돌아온 채팅이 1:1 매칭.
    static constexpr int64_t RTT_PROBE_TIMEOUT_MS = 3000; // 프로브 유실 시 재무장 임계
    int64_t     _chatSentMs      = 0;  // RTT 프로브 송신 시각 (0 = 미측정 중)
    int64_t     _lastRttMs       = -1; // HandleChat에서 측정한 RTT (-1 = 미측정)
    int64_t     _lastRecvMs      = 0;  // OnRecv 시점 타임스탬프 (RTT 보정용)

    // 난수 생성기 (스레드 로컬이 아닌 인스턴스별)
    std::minstd_rand _rng{std::random_device{}()};

    CRingBufferST  _recvBuf{65535};
    CRingBufferST  _sendBuf{16384};
};
