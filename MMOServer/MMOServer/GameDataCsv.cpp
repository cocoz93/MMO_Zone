#include "GameDataCsv.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace GameDataCsv
{
    bool ReadDataLines(const std::string& path, std::vector<DataLine>& out)
    {
        std::ifstream f(path);
        if (!f.is_open())
            return false;

        out.clear();
        std::string line;
        int no = 0;
        while (std::getline(f, line))
        {
            ++no;
            // UTF-8 BOM 이 붙어 있으면 첫 줄 앞 세 바이트를 턴다 — 편집기가 붙여 놓는 일이 있다
            if (no == 1 && line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3);

            size_t b = line.find_first_not_of(" \t\r\n");
            if (b == std::string::npos)      continue;   // 빈 줄
            if (line[b] == '#')              continue;   // 주석
            if (!std::isdigit(static_cast<unsigned char>(line[b])))
                continue;                                // 헤더 — 첫 칸이 숫자가 아니면 값 줄이 아니다

            out.push_back({ no, line });
        }
        return true;
    }

    void SplitCsv(const std::string& line, std::vector<std::string>& out)
    {
        out.clear();
        std::string cell;
        std::istringstream ss(line);
        while (std::getline(ss, cell, ','))
        {
            size_t b = cell.find_first_not_of(" \t\r\n");
            size_t e = cell.find_last_not_of(" \t\r\n");
            out.push_back(b == std::string::npos ? std::string() : cell.substr(b, e - b + 1));
        }
    }

    bool ParseInt(const std::string& s, int64_t& out)
    {
        if (s.empty())
            return false;
        char* end = nullptr;
        const long long v = std::strtoll(s.c_str(), &end, 10);
        if (end != s.c_str() + s.size())
            return false;
        out = static_cast<int64_t>(v);
        return true;
    }
}
