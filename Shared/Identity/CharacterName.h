#pragma once
// ==========================================================================
// 캐릭터 이름 — 글자 규칙 · 와이어 칸 · DB 글자 변환
//
//   와이어 칸은 UTF-16 6칸(char16_t, 남으면 0)이다. DB 는 UTF-8(character_roster.name).
//   글자 = 2~6글자 · 한글 완성형(가~힣) · 영문 · 숫자. 공백 · 특수문자 · 자모 낱자는 못 쓴다 —
//   허용 글자를 좁혀 닮은 글자와 보이지 않는 글자로 남을 사칭하는 길을 막는다.
//   6글자는 배너에서 레벨 · 맵 글자와 안 겹치게 고른 값이다(이름 플랜 2절, 계산값).
//   대소문자만 다른 이름은 같은 이름이다 — DB 비교 규칙이 판정한다. 여기서 안 본다.
//
//   허용 글자가 전부 BMP 라 대리쌍을 다룰 일이 없다(1글자 = UTF-16 1칸 = UTF-8 1~3바이트).
// ==========================================================================

#include <cstddef>
#include <cstdint>
#include <string>

inline constexpr int CHAR_NAME_MIN_LEN = 2;
inline constexpr int CHAR_NAME_MAX_LEN = 6;

namespace CharacterName
{
    inline bool IsAllowedChar(char16_t c)
    {
        return (c >= u'0' && c <= u'9')
            || (c >= u'A' && c <= u'Z')
            || (c >= u'a' && c <= u'z')
            || (c >= 0xAC00 && c <= 0xD7A3);   // 한글 완성형 가~힣
    }

    // 칸 전체를 본다 — 글자가 끝난 뒤의 칸은 전부 0 이어야 한다
    inline bool IsValid(const char16_t (&name)[CHAR_NAME_MAX_LEN])
    {
        int length = 0;
        while (length < CHAR_NAME_MAX_LEN && name[length] != 0)
        {
            if (!IsAllowedChar(name[length]))
                return false;
            ++length;
        }
        for (int i = length; i < CHAR_NAME_MAX_LEN; ++i)
        {
            if (name[i] != 0)
                return false;
        }
        return length >= CHAR_NAME_MIN_LEN;
    }

    // 와이어 칸 → DB 글자. 형식을 통과한 이름만 넣을 것(허용 글자 밖은 안 다룬다)
    inline std::string ToUtf8(const char16_t (&name)[CHAR_NAME_MAX_LEN])
    {
        std::string out;
        for (int i = 0; i < CHAR_NAME_MAX_LEN && name[i] != 0; ++i)
        {
            const char16_t c = name[i];
            if (c < 0x80)
            {
                out += static_cast<char>(c);
            }
            else
            {
                out += static_cast<char>(0xE0 | (c >> 12));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        return out;
    }

    // DB 글자 → 와이어 칸. 규칙에 어긋나면(손으로 고친 행뿐) false 이고 칸은 전부 0 이다
    inline bool FromUtf8(const char* data, size_t size, char16_t (&out)[CHAR_NAME_MAX_LEN])
    {
        for (char16_t& c : out)
            c = 0;

        char16_t parsed[CHAR_NAME_MAX_LEN]{};
        int length = 0;
        size_t i = 0;
        while (i < size)
        {
            if (length >= CHAR_NAME_MAX_LEN)
                return false;
            const unsigned char first = static_cast<unsigned char>(data[i]);
            if (first < 0x80)
            {
                parsed[length++] = first;
                i += 1;
            }
            else if ((first & 0xF0) == 0xE0 && i + 2 < size)
            {
                const unsigned char second = static_cast<unsigned char>(data[i + 1]);
                const unsigned char third = static_cast<unsigned char>(data[i + 2]);
                if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80)
                    return false;
                parsed[length++] = static_cast<char16_t>(((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F));
                i += 3;
            }
            else
            {
                return false;
            }
        }
        if (!IsValid(parsed))
            return false;
        for (int k = 0; k < CHAR_NAME_MAX_LEN; ++k)
            out[k] = parsed[k];
        return true;
    }
}
