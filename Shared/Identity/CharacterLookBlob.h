#pragma once
// ==========================================================================
// CharacterLookBlob — character_roster.look 형식을 아는 유일한 코드
//
//   형식 = [판 번호 1B][성별][피부][머리][상의][하의](각 1B). 값은 배열 순서가 아니라 외형 표 번호다.
//   칸을 나누지 않는 까닭: 부위는 늘어날 수 있고, 외형으로 찾거나 집계하는 기능이 없으며 항상 통째로 읽는다.
//   부위가 늘면 판 번호를 올리고 옛 판은 읽어서 새 판으로 옮긴다(CooldownBuffBlob.h 와 같은 규칙).
//   32바이트 = 판 번호를 뺀 부위 31개까지의 여유다.
//
//   캐릭터 서버가 쓰고 존 서버는 읽기만 한다. 번호가 외형 표에 있는지는 CLookTable 이 본다 — 여기는 꼴만 본다.
// ==========================================================================

#include <cstdint>
#include <cstring>

namespace CharacterLookBlob
{
    inline constexpr uint8_t VERSION_1 = 1;
    inline constexpr int V1_BYTES = 6;         // 판 번호 + 부위 5개
    inline constexpr int COLUMN_BYTES = 32;    // VARBINARY(32) · 와이어 칸

    inline constexpr uint8_t GENDER_MALE = 1;  // 0 = 외형 없음(소유 표에 없는 옛 행 · 더미)
    inline constexpr uint8_t GENDER_FEMALE = 2;

    struct Look
    {
        uint8_t gender = 0;
        uint8_t skin = 0;
        uint8_t hair = 0;
        uint8_t top = 0;
        uint8_t bottom = 0;
    };

    // DB 칸이든 와이어 칸이든 같은 함수로 읽는다. 모르는 판 · 짧음 · 형식 뒤에 값이 있으면 false
    inline bool Read(const void* data, int size, Look& out)
    {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        if (size < V1_BYTES || size > COLUMN_BYTES || bytes[0] != VERSION_1)
            return false;
        for (int i = V1_BYTES; i < size; ++i)
        {
            if (bytes[i] != 0)
                return false;
        }
        out.gender = bytes[1];
        out.skin = bytes[2];
        out.hair = bytes[3];
        out.top = bytes[4];
        out.bottom = bytes[5];
        return true;
    }

    // 지금 판으로 쓴다. 쓴 바이트 수(V1_BYTES)를 돌려준다
    inline int Write(const Look& look, uint8_t (&out)[COLUMN_BYTES])
    {
        std::memset(out, 0, sizeof(out));
        out[0] = VERSION_1;
        out[1] = look.gender;
        out[2] = look.skin;
        out[3] = look.hair;
        out[4] = look.top;
        out[5] = look.bottom;
        return V1_BYTES;
    }
}
