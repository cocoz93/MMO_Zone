#pragma once

#include "../Shared/World/WalkGrid.h"
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <Windows.h>

// ==========================================================================
// 막힌 칸 표 읽기 — 실행 파일 옆 BlockTable.csv(서버와 같은 파일 · Run/bin)에서 그 맵 줄만 골라 격자를 세운다.
//   값 검사는 서버가 기동 때 했다(BlockTable.cpp) — 여기는 숫자 다섯을 읽어 칸을 막기만 한다.
//   줄 거르기는 서버 GameDataCsv 와 같다(빈 줄 · '#' 주석 · 첫 칸이 숫자가 아닌 헤더). 칸 = mapId,x0,y0,x1,y1,note
//   파일이 없으면 false — 부르는 쪽이 막힌 칸 없이 걷는다(콘솔은 관측용이라 안 멈춘다 · 서버의 멈춤 통보가 맞춰 준다).
// ==========================================================================
inline bool LoadClientWalkGrid(int32_t mapId, int32_t width, int32_t height, CWalkGrid& out)
{
    out.Init(width, height);

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring path(exePath);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"BlockTable.csv";

    std::ifstream file(path);
    if (!file)
        return false;

    std::string line;
    while (std::getline(file, line))
    {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF)   // UTF-8 BOM
            line = line.substr(3);
        if (line.empty() || line[0] < '0' || line[0] > '9')
            continue;

        std::vector<int32_t> v;
        std::stringstream ss(line);
        std::string cell;
        while (v.size() < 5 && std::getline(ss, cell, ','))
        {
            try { v.push_back(std::stoi(cell)); }
            catch (...) { break; }
        }
        if (v.size() == 5 && v[0] == mapId)
            out.Block(v[1], v[2], v[3], v[4]);
    }
    return true;
}
