#pragma once

#include <cstdint>

// 맵 설정 데이터 — 설정 읽기(ServerConfig)가 채우고 맵 관리자(CMapManager::RegisterMap)가 받는다.
//   둘이 같이 쓰는 값이라 따로 둔다 — 설정 쪽이 MapManager.h 를 넣으면 존 층 전체(Zone.h · 섹터 · 몬스터 · 땅 아이템)를 끌어온다.
struct MapConfig
{
    int32_t mapId;
    int32_t mapWidth;
    int32_t mapHeight;
    int32_t sectorSize;
    int32_t maxPlayersPerChannel;  // 채널당 최대 인원 (소프트 리밋)
};