#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

// ==========================================================================
// CWalkGrid — 맵 하나를 1×1 칸으로 나눠 칸마다 막힘 여부를 든다 (서버·콘솔 클라 공유)
//
// [무엇] 칸 하나 = (floor(x), floor(y)). 막힌 칸에는 아무도 못 들어간다. 원본은 Run/bin/BlockTable.csv
//        (서버 CBlockTable 이 읽어 세우고, 콘솔 클라는 같은 파일을 맵 번호로 골라 읽는다 · 웹 live.html 은 같은 식을 JS 로 따로 쓴다).
//
// [맵 끝은 여기 없다] 맵 끝은 옛 클램프(0 ~ 폭-1) 그대로다 — 그 상한을 웹·콘솔·부하 더미가 같이 쓰고 있어 막힌 칸으로 바꾸면 끝 좌표가 바뀐다.
//        맵 밖 칸은 여기서 「안 막힘」이다. 맵 끝을 먼저 자르고 그다음 막힌 칸을 보는 것이 부르는 쪽 순서다.
//
// [멈추는 자리] 막힌 칸에 들어가려던 걸음은 막은 축만 그 칸 경계에서 STOP_GAP 앞으로 되돌린다(ResolveStep).
//        옛 자리로 통째로 돌리지 않는 까닭 — 한 틱 걸음 길이가 속도마다 달라 벽 앞 빈틈이 들쭉날쭉해진다.
//        STOP_GAP 이 와이어 좌표 양자화 단위(1/512 — Protocol.h POS_QUANT_SCALE)보다 커야 받은 쪽이 반올림으로 벽 안에 들어가지 않는다.
//
// [미끄러짐 없음] 부딪히면 멈춘다 — 맵 끝 벽과 같은 규칙. 벽을 따라 미끄러지게 하려면 ResolveStep 과 웹 사본을 같이 바꾼다.
// ==========================================================================
class CWalkGrid
{
public:
    static constexpr float STOP_GAP  = 1.0f / 64.0f;     // 막힌 칸 경계에서 멈추는 간격(위 [멈추는 자리]) — 2의 거듭제곱이라 오차 없음
    static constexpr float PROBE     = STOP_GAP * 2.0f;  // 「그쪽으로 한 발이 막혔나」를 볼 거리 — 멈춘 자리에서 재면 반드시 막힌 칸에 닿는다
    static constexpr float SUB_STEP  = 0.5f;             // 한 걸음을 이 길이 이하로 나눠 본다 — 빠른 걸음이 얇은 칸을 건너뛰지 않게

    void Init(int32_t width, int32_t height)
    {
        _width  = width;
        _height = height;
        _blocked.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
        _blockedCount = 0;
    }

    // 칸 사각형을 막는다 — 양끝 포함. 맵 밖 칸은 건너뛴다(표가 읽을 때 맵 안인지 이미 본다)
    void Block(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
    {
        for (int32_t cy = y0; cy <= y1; ++cy)
            for (int32_t cx = x0; cx <= x1; ++cx)
            {
                if (!InGrid(cx, cy))
                    continue;
                uint8_t& c = _blocked[Index(cx, cy)];
                if (c == 0) { c = 1; ++_blockedCount; }
            }
    }

    int32_t Width() const { return _width; }
    int32_t Height() const { return _height; }
    int32_t BlockedCount() const { return _blockedCount; }

    bool IsBlockedCell(int32_t cx, int32_t cy) const
    {
        return InGrid(cx, cy) && _blocked[Index(cx, cy)] != 0;
    }

    // 좌표가 막힌 칸 안인가 — NaN 은 「안 막힘」(맵 안 판정이 먼저 거른다)
    bool IsBlockedAt(float x, float y) const
    {
        if (!(x >= 0.0f) || !(y >= 0.0f))
            return false;
        return IsBlockedCell(static_cast<int32_t>(std::floor(x)), static_cast<int32_t>(std::floor(y)));
    }

    // 지금 자리에서 (dx, dy) 쪽으로 한 발이 막혔나 — 출발·방향 바꾸기 때 본다. (dx, dy) 는 단위 벡터(서버 DirectionStep)
    bool IsBlockedToward(float x, float y, float dx, float dy) const
    {
        return IsBlockedAt(x + dx * PROBE, y + dy * PROBE);
    }

    // 한 걸음 — (x, y) 에서 (nx, ny) 로 가다 막힌 칸에 들어가면 그 앞에서 멈춘 자리로 (nx, ny) 를 고친다. 고쳤으면 true.
    //   (x, y) 는 막힌 칸이 아니어야 한다(입장·저장 자리 검사가 보장). 맵 끝 클램프는 부르는 쪽이 먼저 한다(머리 주석).
    bool ResolveStep(float x, float y, float& nx, float& ny) const
    {
        if (_blockedCount == 0)
            return false;

        const float ddx = nx - x, ddy = ny - y;
        const float len = std::sqrt(ddx * ddx + ddy * ddy);
        const int32_t n = len > SUB_STEP ? static_cast<int32_t>(std::ceil(len / SUB_STEP)) : 1;

        float px = x, py = y;   // 마지막으로 안 막힌 자리
        for (int32_t i = 1; i <= n; ++i)
        {
            const float t  = static_cast<float>(i) / static_cast<float>(n);
            const float qx = (i == n) ? nx : x + ddx * t;
            const float qy = (i == n) ? ny : y + ddy * t;
            if (!IsBlockedAt(qx, qy))
            {
                px = qx; py = qy;
                continue;
            }

            // 막힌 칸 (cx, cy) 로 넘어간 축만 경계 앞으로 — 안 넘어간 축은 간 만큼 둔다(그 축으로는 같은 칸 안이다)
            const int32_t cx = static_cast<int32_t>(std::floor(qx)), cy = static_cast<int32_t>(std::floor(qy));
            const int32_t pcx = static_cast<int32_t>(std::floor(px)), pcy = static_cast<int32_t>(std::floor(py));
            float rx = qx, ry = qy;
            if (cx != pcx) rx = (cx > pcx) ? static_cast<float>(cx) - STOP_GAP : static_cast<float>(cx + 1) + STOP_GAP;
            if (cy != pcy) ry = (cy > pcy) ? static_cast<float>(cy) - STOP_GAP : static_cast<float>(cy + 1) + STOP_GAP;
            nx = rx; ny = ry;
            return true;
        }
        return false;
    }

    // 선분이 막힌 칸을 지나나 — 클라가 보낸 좌표를 받을지 볼 때(벽 건너편 좌표로 집을 뚫지 못하게)
    bool SegmentBlocked(float x0, float y0, float x1, float y1) const
    {
        if (_blockedCount == 0)
            return false;
        const float ddx = x1 - x0, ddy = y1 - y0;
        const float len = std::sqrt(ddx * ddx + ddy * ddy);
        // 칸보다 잘게 — 대각선이 칸 모서리만 스치는 경우도 잡게 반의반 칸
        const int32_t n = static_cast<int32_t>(std::ceil(len / (SUB_STEP * 0.5f))) + 1;
        for (int32_t i = 0; i <= n; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            if (IsBlockedAt(x0 + ddx * t, y0 + ddy * t))
                return true;
        }
        return false;
    }

private:
    bool InGrid(int32_t cx, int32_t cy) const { return cx >= 0 && cy >= 0 && cx < _width && cy < _height; }
    size_t Index(int32_t cx, int32_t cy) const { return static_cast<size_t>(cy) * static_cast<size_t>(_width) + static_cast<size_t>(cx); }

    int32_t _width  = 0;
    int32_t _height = 0;
    int32_t _blockedCount = 0;      // 0 이면 판정을 통째로 건너뛴다(막힌 줄 없는 맵 — 지금 던전 전부)
    std::vector<uint8_t> _blocked;  // 칸마다 1 = 막힘. 맵 최대 128×128 = 16KB(비트로 줄이지 않는다 — 존마다가 아니라 맵마다 하나라 작다)
};
