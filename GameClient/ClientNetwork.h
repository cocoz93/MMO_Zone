#pragma once

#include "../Shared/Protocol/Protocol.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#define NOMINMAX
#include <Windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>

#pragma comment(lib, "ws2_32.lib")

class CGameInstance; // 전방 선언

class CClientNetwork
{
public:
    CClientNetwork();
    ~CClientNetwork();

    bool Connect(const std::string& serverIp, int port);
    void Disconnect();
    bool IsConnected() const { return _connected; }

    // 패킷 전송
    bool SendPacket(const char* data, size_t length);

    // C2S 패킷 전송 함수
    void SendMoveStart(uint8_t direction, float x, float y);
    void SendMoveStop(uint8_t direction, float x, float y);
    void SendChat(const wchar_t* message);
    void SendZoneChange(int32_t targetMapId, int32_t targetChannelIndex = -1);
    void SendHeartbeat();
    void SendEnterGame(int64_t accountId);
    void SendAdminLogin(const char* key);

    // GameInstance 연결 (패킷 핸들러 콜백용)
    void SetGameInstance(CGameInstance* instance) { _gameInstance = instance; }

private:
    // 수신 스레드
    void RecvThread();

    // 수신 패킷 디스패치
    void DispatchPacket(const char* data, uint16_t size);

private:
    SOCKET _socket;
    std::atomic<bool> _connected;
    std::atomic<bool> _running;
    std::thread _recvThread;

    CGameInstance* _gameInstance; // 패킷 수신 시 콜백

    // TCP 스트림 조립용 누적 버퍼
    static constexpr int RECV_BUFFER_SIZE = 8192;
    char _recvBuffer[RECV_BUFFER_SIZE];
    int _recvBufferUsed;
};
