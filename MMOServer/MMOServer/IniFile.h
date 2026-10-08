#pragma once
// ==========================================================================
// CIniFile — 플랫폼 독립 INI 리더 (Win32 GetPrivateProfile* 대체)
//
//   - 파일을 1회 파싱해 (섹션 → (키 → 값)) 맵으로 보관.
//   - 섹션/키는 대소문자 무시 (GetPrivateProfile과 동일). 값은 원문 바이트 보존.
//   - 라인의 첫 비공백이 ';' 또는 '#' 이면 주석. [Section] / Key=Value.
//   - 값은 앞뒤 공백('\r' 포함) trim + 감싼 큰따옴표 1쌍 제거.
//   - 인코딩 무해석: 구조 토큰([ ] = ; #)이 전부 ASCII라, 값이 CP949든 UTF-8이든
//     raw 바이트 그대로 통과한다. (한글 주석은 스킵되므로 무관)
// ==========================================================================

#include <string>
#include <unordered_map>
#include <fstream>
#include <cstdlib>
#include <cctype>

class CIniFile
{
public:
    // INI 파싱. 열기 성공 true / 실패(파일 없음 등) false.
    bool Load(const std::string& path)
    {
        std::ifstream ifs(path, std::ios::binary);   // binary: CRLF/LF 번역 없이 원문
        if (!ifs.is_open())
            return false;

        std::string curSection;
        std::string line;
        bool firstLine = true;
        while (std::getline(ifs, line))
        {
            if (firstLine)                            // 파일 맨 앞 UTF-8 BOM 방어
            {
                StripUtf8Bom(line);
                firstLine = false;
            }

            std::string t = Trim(line);
            if (t.empty() || t[0] == ';' || t[0] == '#')
                continue;

            if (t.front() == '[')                     // [Section]
            {
                size_t end = t.find(']');
                if (end != std::string::npos)
                    curSection = ToLower(Trim(t.substr(1, end - 1)));
                continue;
            }

            size_t eq = t.find('=');                  // Key=Value
            if (eq == std::string::npos)
                continue;

            std::string key = ToLower(Trim(t.substr(0, eq)));
            std::string val = Unquote(Trim(t.substr(eq + 1)));
            _data[curSection][key] = val;
        }
        _loaded = true;
        return true;
    }

    bool Loaded() const { return _loaded; }

    std::string GetString(const std::string& section, const std::string& key,
                          const std::string& defVal) const
    {
        auto s = _data.find(ToLower(section));
        if (s == _data.end()) return defVal;
        auto k = s->second.find(ToLower(key));
        return (k == s->second.end()) ? defVal : k->second;
    }

    // 키가 있는가 (값이 비어 있어도 있다) — 옮겨 간 옛 키가 남아 있는지 알릴 때
    bool HasKey(const std::string& section, const std::string& key) const
    {
        auto s = _data.find(ToLower(section));
        return s != _data.end() && s->second.find(ToLower(key)) != s->second.end();
    }

    int GetInt(const std::string& section, const std::string& key, int defVal) const
    {
        auto s = _data.find(ToLower(section));
        if (s == _data.end()) return defVal;
        auto k = s->second.find(ToLower(key));
        if (k == s->second.end()) return defVal;                              // 키 없음 → default
        return static_cast<int>(std::strtol(k->second.c_str(), nullptr, 10)); // 비숫자/빈값 → 0 (GetPrivateProfileInt과 동일)
    }

private:
    static std::string ToLower(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static std::string Trim(const std::string& s)
    {
        auto isWs = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        size_t b = 0, e = s.size();
        while (b < e && isWs(s[b]))     ++b;
        while (e > b && isWs(s[e - 1])) --e;
        return s.substr(b, e - b);
    }

    static std::string Unquote(const std::string& s)
    {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            return s.substr(1, s.size() - 2);
        return s;
    }

    static void StripUtf8Bom(std::string& line)
    {
        if (line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
    }

    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> _data;
    bool _loaded = false;
};
