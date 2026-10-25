#pragma once

#include "ClientPlayer.h"
#include "../Shared/Protocol/Protocol.h"
#include "ClientBlockTable.h"   // 막힌 칸 격자 — 서버와 같은 표 · 같은 식(Shared/World/WalkGrid.h)
#include <unordered_map>

// 내 캐릭터 + 주변 플레이어 목록 관리
class CPlayerManager
{
public:
    // 내 캐릭터 설정
    void SetMyPlayer(int32_t playerId, float x, float y, Direction direction, int32_t speed,
                     uint8_t displayChar, uint8_t colorIndex)
    {
        _myPlayer.playerId = playerId;
        _myPlayer.displayChar = displayChar;
        _myPlayer.colorIndex = colorIndex;
        _myPlayer.x = x;
        _myPlayer.y = y;
        _myPlayer.direction = direction;
        _myPlayer.moveState = MoveState::IDLE;
        _myPlayer.speed = speed;
        _hasMyPlayer = true;
    }

    // 다른 플레이어 추가 (시야 진입)
    void AddOtherPlayer(int32_t playerId, float x, float y,
                        Direction direction, MoveState moveState, int32_t speed,
                        uint8_t displayChar, uint8_t colorIndex,
                        SpawnReason reason = SpawnReason::NORMAL)
    {
        ClientPlayer player;
        player.playerId = playerId;
        player.displayChar = displayChar;
        player.colorIndex = colorIndex;
        player.x = x;
        player.y = y;
        player.direction = direction;
        player.moveState = moveState;
        player.speed = speed;
        player.spawnEffectTimer = (reason != SpawnReason::NORMAL) ? SPAWN_EFFECT_DURATION : 0.0f;
        _otherPlayers[playerId] = player;
    }

    // 플레이어 제거 (시야 이탈 / 퇴장)
    void RemovePlayer(int32_t playerId)
    {
        _otherPlayers.erase(playerId);
    }

    // 전체 초기화 (존 이동 시)
    void Clear()
    {
        _hasMyPlayer = false;
        _myPlayer = ClientPlayer{};
        _otherPlayers.clear();
    }

    // MOVING 상태인 플레이어들의 좌표를 방향별로 갱신
    // myPlayerClamped: 내 캐릭터가 벽에 닿아 자동 정지되었으면 true
    void UpdateMovingPlayers(float deltaTime, bool& myPlayerClamped)
    {
        myPlayerClamped = false;

        if (_hasMyPlayer && _myPlayer.moveState == MoveState::MOVING)
        {
            if (Step(_myPlayer, deltaTime))
            {
                // 서버 Zone::Tick과 동일: 경계 클램핑·막힌 칸 → IDLE 전환
                _myPlayer.moveState = MoveState::IDLE;
                myPlayerClamped = true;
            }
        }

        for (auto& pair : _otherPlayers)
        {
            if (pair.second.moveState == MoveState::MOVING)
            {
                if (Step(pair.second, deltaTime))
                {
                    pair.second.moveState = MoveState::IDLE;
                }
            }

            // 스폰 이펙트 타이머 감소
            if (pair.second.spawnEffectTimer > 0.0f)
            {
                pair.second.spawnEffectTimer -= deltaTime;
                if (pair.second.spawnEffectTimer < 0.0f)
                    pair.second.spawnEffectTimer = 0.0f;
            }
        }
    }

    // 조회
    bool HasMyPlayer() const { return _hasMyPlayer; }
    ClientPlayer* GetMyPlayer() { return _hasMyPlayer ? &_myPlayer : nullptr; }

    ClientPlayer* FindPlayer(int32_t playerId)
    {
        auto it = _otherPlayers.find(playerId);
        if (it != _otherPlayers.end())
            return &it->second;
        return nullptr;
    }

    const std::unordered_map<int32_t, ClientPlayer>& GetOtherPlayers() const
    {
        return _otherPlayers;
    }

    // 그쪽으로 한 발이 막혔나 — 맵 끝 벽(아래 IsBlockedByWall) 또는 막힌 칸(서버 CZone::StepBlocked 와 같은 식)
    bool StepBlocked(float x, float y, Direction dir) const
    {
        if (IsBlockedByWall(x, y, dir))
            return true;
        float dx = 0.0f, dy = 0.0f;
        DirVec(dir, dx, dy);
        return _walk.IsBlockedToward(x, y, dx, dy);
    }

    // 벽 경계에 닿은 상태에서 해당 방향으로 이동 불가 여부
    bool IsBlockedByWall(float x, float y, Direction dir) const
    {
        float maxX = static_cast<float>(_mapWidth) - 1.0f;
        float maxY = static_cast<float>(_mapHeight) - 1.0f;

        bool atLeft   = (x <= 0.0f);
        bool atRight  = (x >= maxX);
        bool atTop    = (y <= 0.0f);
        bool atBottom = (y >= maxY);

        switch (dir)
        {
        case Direction::LEFT:  return atLeft;
        case Direction::RIGHT: return atRight;
        case Direction::UP:    return atTop;
        case Direction::DOWN:  return atBottom;
        default: return false;
        }
    }

public:
    // 맵 크기 설정 (서버에서 S2C_ZONE_INFO로 수신)
    void SetMapSize(int width, int height)
    {
        _mapWidth = width;
        _mapHeight = height;
    }

    // 그 맵의 막힌 칸을 실행 파일 옆 BlockTable.csv 에서 다시 읽는다(존이 바뀔 때마다 — S2C_ZONE_INFO). 파일이 없으면 false · 막힌 칸 없이 걷는다
    bool LoadWalkGrid(int32_t mapId, int width, int height)
    {
        return LoadClientWalkGrid(mapId, width, height, _walk);
    }

private:
    // 스폰 이펙트 지속 시간 (초)
    static constexpr float SPAWN_EFFECT_DURATION = 2.0f;

    // 맵 크기 (서버에서 S2C_ZONE_INFO로 수신)
    int _mapWidth = 120;
    int _mapHeight = 120;
    CWalkGrid _walk;   // 지금 맵의 막힌 칸 — 비어 있으면(막힌 칸 0) 판정을 건너뛴다

    // 맵 경계 클램핑 (서버 Zone::Tick과 동일 조건) — 클램핑 발생 시 true 반환
    bool ClampToMap(ClientPlayer& player)
    {
        bool clamped = false;
        if (player.x < 0.0f)                          { player.x = 0.0f; clamped = true; }
        if (player.x >= static_cast<float>(_mapWidth))  { player.x = static_cast<float>(_mapWidth) - 1.0f; clamped = true; }
        if (player.y < 0.0f)                          { player.y = 0.0f; clamped = true; }
        if (player.y >= static_cast<float>(_mapHeight)) { player.y = static_cast<float>(_mapHeight) - 1.0f; clamped = true; }
        return clamped;
    }

    // 한 걸음 — 이동 · 맵 끝 클램핑 · 막힌 칸(서버 CZone::Tick 과 같은 순서). 멈춰야 하면 true
    bool Step(ClientPlayer& player, float deltaTime)
    {
        const float fromX = player.x, fromY = player.y;
        ApplyMovement(player, deltaTime);
        bool clamped = ClampToMap(player);
        if (_walk.ResolveStep(fromX, fromY, player.x, player.y))
            clamped = true;
        return clamped;
    }

    static void DirVec(Direction dir, float& dx, float& dy)
    {
        switch (dir)
        {
        case Direction::UP:    dx = 0.0f;  dy = -1.0f; break;
        case Direction::DOWN:  dx = 0.0f;  dy = 1.0f;  break;
        case Direction::LEFT:  dx = -1.0f; dy = 0.0f;  break;
        case Direction::RIGHT: dx = 1.0f;  dy = 0.0f;  break;
        default:               dx = 0.0f;  dy = 0.0f;  break;
        }
    }

    // 방향별 좌표 이동 적용
    static void ApplyMovement(ClientPlayer& player, float deltaTime)
    {
        float dist = player.speed * deltaTime;
        switch (player.direction)
        {
        case Direction::UP:    player.y -= dist; break;
        case Direction::DOWN:  player.y += dist; break;
        case Direction::LEFT:  player.x -= dist; break;
        case Direction::RIGHT: player.x += dist; break;
        default: break;
        }
    }

private:
    ClientPlayer _myPlayer;
    bool _hasMyPlayer = false;
    std::unordered_map<int32_t, ClientPlayer> _otherPlayers;
};
