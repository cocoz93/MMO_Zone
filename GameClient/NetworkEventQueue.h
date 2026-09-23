#pragma once

#include "ClientPlayer.h"
#include <cstdint>
#include <queue>
#include <mutex>
#include <string>

// 수신 스레드 → 게임 루프 간 이벤트 전달용 구조체
struct ClientNetworkEvent
{
    enum class Type
    {
        ZONE_INFO,
        CREATE_MY_PLAYER,
        CREATE_OTHER_PLAYER,
        DELETE_PLAYER,
        MOVE_START,
        MOVE_STOP,
        CHAT,
        SYNC_POSITION,
        ZONE_CHANGE_OK,
        ZONE_CHANGE_FAIL,
        ERROR_MSG,
        ADMIN_LOGIN_OK,
        ADMIN_LOGIN_FAIL,
        EXP          // 서버 USE_EXP — 내 레벨·경험치 (본인에게만 온다)
    };

    Type type;

    // 공용 필드 (패킷 타입에 따라 사용)
    int32_t playerId;
    uint8_t displayChar;
    uint8_t colorIndex;
    float x;
    float y;
    uint8_t direction;
    uint8_t moveState;
    uint8_t spawnReason;
    int32_t speed;

    // 존 정보
    int32_t mapWidth;
    int32_t mapHeight;
    int32_t sectorSize;

    // 존 이동
    int32_t mapId;
    int32_t channelIndex;
    uint8_t reason;

    // 레벨·경험치 — playerId 를 안 싣는다(늘 본인이다)
    int32_t level;
    int64_t exp;
    int64_t levelBaseExp;
    int64_t nextLevelExp;

    // 채팅 / 에러
    wchar_t chatMessage[512];
    char errorMessage[256];
};

// 스레드 안전 이벤트 큐
class CNetworkEventQueue
{
public:
    void Push(ClientNetworkEvent&& event)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _queue.push(std::move(event));
    }

    bool Pop(ClientNetworkEvent& out)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_queue.empty())
            return false;
        out = _queue.front();
        _queue.pop();
        return true;
    }

private:
    std::queue<ClientNetworkEvent> _queue;
    std::mutex _mutex;
};
