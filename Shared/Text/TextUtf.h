#pragma once
// ==========================================================================
// TextUtf — 와이어의 UTF-16 칸 ↔ DB 의 UTF-8 · 글자 칸 검사
//
//   와이어 글자 칸은 char16_t 고정 칸이다(글자가 끝나면 0, 그 뒤 칸도 전부 0). DB 는 utf8mb4 다.
//   이름은 허용 글자가 BMP 뿐이라 CharacterName.h 가 따로 다룬다. 여기는 우편 제목 · 본문처럼 아무 글자나 들어오는 칸이다
//   (이모지 같은 BMP 밖 글자 = 대리쌍 2칸). 존 서버(캐릭터 우편)와 계정 서버(계정 우편)가 함께 쓴다.
//
//   ★ 홀로 남은 대리쌍(짝 없는 0xD800~0xDFFF)과 깨진 UTF-8 은 false — 그대로 넣으면 utf8mb4 가 거절하거나 다른 글자로 바뀐다.
//     웹의 maxlength 는 UTF-16 칸을 세서 대리쌍을 반으로 자를 수 있다. 그래서 서버가 반드시 본다.
// ==========================================================================

#include <cstddef>
#include <cstdint>
#include <string>

namespace TextUtf
{
    inline bool IsHighSurrogate(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
    inline bool IsLowSurrogate(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

    // 글자 수 = 첫 0 앞까지의 칸 수. 첫 0 뒤 칸에 값이 있으면 -1 — 끝 표시 뒤에 숨긴 글자를 막는다
    inline int CellLength(const char16_t* cells, int capacity)
    {
        int length = 0;
        while (length < capacity && cells[length] != 0)
            ++length;
        for (int i = length; i < capacity; ++i)
        {
            if (cells[i] != 0)
                return -1;
        }
        return length;
    }

    // 대리쌍이 모두 짝을 이루나 — 높은 칸 바로 뒤에 낮은 칸
    inline bool PairsOk(const char16_t* cells, int length)
    {
        for (int i = 0; i < length; ++i)
        {
            if (IsHighSurrogate(cells[i]))
            {
                if (i + 1 >= length || !IsLowSurrogate(cells[i + 1]))
                    return false;
                ++i;
            }
            else if (IsLowSurrogate(cells[i]))
            {
                return false;
            }
        }
        return true;
    }

    // 제어 문자(0x20 미만 · 0x7F)가 있나. allowNewline 이면 '\n'(0x0A) 하나만 봐준다(본문 줄바꿈 · mail-plan 2절 5번).
    //   '\r' 은 봐주지 않는다 — 줄바꿈 한 번이 두 칸(두 글자)이 되면 웹 maxlength 와 서버의 글자 수가 어긋난다
    inline bool HasControl(const char16_t* cells, int length, bool allowNewline)
    {
        for (int i = 0; i < length; ++i)
        {
            const char16_t c = cells[i];
            if (c == u'\n' && allowNewline)
                continue;
            if (c < 0x20 || c == 0x7F)
                return true;
        }
        return false;
    }

    // UTF-16 → UTF-8. 홀로 남은 대리쌍이면 false 이고 out 은 비어 있다
    inline bool ToUtf8(const char16_t* cells, int length, std::string& out)
    {
        out.clear();
        out.reserve(static_cast<size_t>(length) * 3);
        for (int i = 0; i < length; ++i)
        {
            uint32_t cp = cells[i];
            if (IsHighSurrogate(cells[i]))
            {
                if (i + 1 >= length || !IsLowSurrogate(cells[i + 1]))
                {
                    out.clear();
                    return false;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(cells[i + 1]) - 0xDC00);
                ++i;
            }
            else if (IsLowSurrogate(cells[i]))
            {
                out.clear();
                return false;
            }

            if (cp < 0x80)
            {
                out += static_cast<char>(cp);
            }
            else if (cp < 0x800)
            {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else if (cp < 0x10000)
            {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
        return true;
    }

    // UTF-8 → UTF-16 칸(capacity 칸 · 남은 칸은 0). 아래면 false 이고 칸은 전부 0 이다:
    //   깨진 바이트 · 잘린 글자 · 지나치게 긴 인코딩 · 대리쌍 영역(U+D800~DFFF) · U+10FFFF 초과 · 글자 0(NUL) · 칸이 모자람
    //   글자 0 을 거절하는 까닭 — 칸 중간에 0 이 끼면 끝 표시로 읽혀 뒤 글자가 사라진다(CellLength 가 -1)
    inline bool FromUtf8(const char* data, size_t size, char16_t* out, int capacity)
    {
        for (int k = 0; k < capacity; ++k)
            out[k] = 0;
        auto fail = [&]() {
            for (int k = 0; k < capacity; ++k)
                out[k] = 0;
            return false;
        };

        int length = 0;
        size_t i = 0;
        while (i < size)
        {
            const unsigned char lead = static_cast<unsigned char>(data[i]);
            uint32_t cp = 0;
            size_t extra = 0;
            uint32_t minimum = 0;
            if (lead < 0x80)
            {
                cp = lead;
            }
            else if ((lead & 0xE0) == 0xC0)
            {
                cp = lead & 0x1F;
                extra = 1;
                minimum = 0x80;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                cp = lead & 0x0F;
                extra = 2;
                minimum = 0x800;
            }
            else if ((lead & 0xF8) == 0xF0)
            {
                cp = lead & 0x07;
                extra = 3;
                minimum = 0x10000;
            }
            else
            {
                return fail();   // 이어지는 바이트(10xxxxxx)로 시작 · 0xF8 이상
            }
            if (size - i <= extra)
                return fail();   // 글자가 잘렸다
            for (size_t k = 1; k <= extra; ++k)
            {
                const unsigned char next = static_cast<unsigned char>(data[i + k]);
                if ((next & 0xC0) != 0x80)
                    return fail();
                cp = (cp << 6) | (next & 0x3F);
            }
            if (cp == 0 || cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                return fail();

            if (cp >= 0x10000)
            {
                if (length + 2 > capacity)
                    return fail();
                cp -= 0x10000;
                out[length++] = static_cast<char16_t>(0xD800 + (cp >> 10));
                out[length++] = static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
            }
            else
            {
                if (length + 1 > capacity)
                    return fail();
                out[length++] = static_cast<char16_t>(cp);
            }
            i += 1 + extra;
        }
        return true;
    }
}
