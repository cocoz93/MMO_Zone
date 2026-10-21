#pragma once

// ==========================================================================
// Reach — 「손이 닿나」 식 하나 (포털 · NPC)
//
// [왜 따로] 포털(PortalDef::InReach)과 NPC(상점 판정)가 같은 규칙을 쓴다 — 중심과의 거리가 반경 안(경계 포함).
//        식을 두 곳에 적으면 한쪽만 「<」로 바뀌는 날 F 키가 포털엔 닿고 NPC 엔 안 닿는다(웹 live.html 도 식 한 곳).
//        표·좌표만 보는 계산이라 표 옆(이 헤더)에 둔다 — 게임 서버도 존도 모른다.
// ==========================================================================
namespace Reach
{
    // (px, py) 가 중심 (cx, cy) 에서 radius 안인가 — 제곱끼리 견준다(제곱근 없음)
    inline bool InRadius(float px, float py, float cx, float cy, float radius)
    {
        const float dx = px - cx;
        const float dy = py - cy;
        return dx * dx + dy * dy <= radius * radius;
    }
}
