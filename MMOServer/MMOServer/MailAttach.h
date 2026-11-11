#pragma once
// ==========================================================================
// MailAttach — 우편 첨부 열(character_mail.attach · account_mail.attach)의 형식을 아는 유일한 코드
//
//   형식 = [판 번호 1B][칸 수 1B] + 칸마다 [item_id i32][count i32](리틀 엔디언). 첨부 아이템이 없으면 빈 값(0바이트)이다.
//   첨부를 본문과 같은 행에 두는 까닭 — 다른 테이블에 두면 한쪽 읽기만 실패했을 때 첨부가 저장에서 날아갈 수 있다(mail-plan 3절).
//   「첨부 없음」을 빈 값 하나로만 두는 까닭 — 지우기 · 붉은 수의 SQL 이 attach='' 한 가지로 가른다. 그래서 칸 0 개짜리 판([1][0])은 읽기에서 거절한다.
//   칸이 늘면 판 번호를 올리고 옛 판은 읽어서 새 판으로 옮긴다(CharacterLookBlob.h 와 같은 규칙).
//
//   존 서버가 쓰고 읽는다. 계정 서버 · 월드 서버는 바이트를 그대로 나르기만 한다(뜻을 모른다).
//   아이템 번호가 아이템 표에 있는지는 여기서 안 본다 — 꼴만 본다(표는 받을 때 CMailSystem 이 본다).
//   같은 아이템이 두 칸에 있어도 된다 — 보내는 쪽 가방의 두 칸이 같은 물약일 수 있다.
// ==========================================================================

#include <cstdint>

#include <Protocol/Protocol.h>   // MAIL_MAX_ITEMS · MailItemEntry

namespace MailAttach
{
    inline constexpr uint8_t VERSION_1 = 1;
    inline constexpr int HEAD_BYTES = 2;    // 판 번호 + 칸 수
    inline constexpr int ENTRY_BYTES = 8;   // item_id i32 + count i32 — inventory.item_id · count 가 INT 다
    inline constexpr int MAX_BYTES = HEAD_BYTES + ENTRY_BYTES * MAIL_MAX_ITEMS;
    static_assert(MAX_BYTES == 66, "첨부 바이트 상한이 바뀌었다 — 열(VARBINARY(255))과 서버 간 패킷의 첨부 칸을 같이 볼 것");

    struct Items
    {
        int           count = 0;
        MailItemEntry items[MAIL_MAX_ITEMS]{};
    };

    namespace Detail
    {
        inline void PutI32(uint8_t* p, int32_t v)
        {
            const uint32_t u = static_cast<uint32_t>(v);
            p[0] = static_cast<uint8_t>(u);
            p[1] = static_cast<uint8_t>(u >> 8);
            p[2] = static_cast<uint8_t>(u >> 16);
            p[3] = static_cast<uint8_t>(u >> 24);
        }

        inline int32_t GetI32(const uint8_t* p)
        {
            return static_cast<int32_t>(uint32_t{ p[0] } | (uint32_t{ p[1] } << 8) | (uint32_t{ p[2] } << 16) | (uint32_t{ p[3] } << 24));
        }
    }

    // 쓴 바이트 수를 돌려준다 — 칸이 0 개면 0(빈 값). 칸 수가 0 ~ MAIL_MAX_ITEMS 밖이면 -1(부르는 쪽 결함)
    inline int Write(const Items& in, uint8_t (&out)[MAX_BYTES])
    {
        if (in.count < 0 || in.count > MAIL_MAX_ITEMS)
            return -1;
        if (in.count == 0)
            return 0;
        out[0] = VERSION_1;
        out[1] = static_cast<uint8_t>(in.count);
        for (int i = 0; i < in.count; ++i)
        {
            uint8_t* entry = out + HEAD_BYTES + i * ENTRY_BYTES;
            Detail::PutI32(entry, in.items[i].itemId);
            Detail::PutI32(entry + 4, in.items[i].count);
        }
        return HEAD_BYTES + in.count * ENTRY_BYTES;
    }

    // 빈 값 = 칸 0 개 · true. 아래면 false 이고 out 은 칸 0 개다(그래서 받기는 ITEM_UNKNOWN 으로 거절하고 우편은 남는다):
    //   모르는 판 · 칸 수가 1 ~ MAIL_MAX_ITEMS 밖 · 길이가 칸 수와 안 맞음 · 개수 1 미만 · 아이템 번호 1 미만
    inline bool Read(const void* data, int size, Items& out)
    {
        out = Items{};
        if (size == 0)
            return true;
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        if (size < HEAD_BYTES || size > MAX_BYTES || bytes[0] != VERSION_1)
            return false;
        const int count = bytes[1];
        if (count < 1 || count > MAIL_MAX_ITEMS || size != HEAD_BYTES + count * ENTRY_BYTES)
            return false;

        Items parsed;
        parsed.count = count;
        for (int i = 0; i < count; ++i)
        {
            const uint8_t* entry = bytes + HEAD_BYTES + i * ENTRY_BYTES;
            parsed.items[i].itemId = Detail::GetI32(entry);
            parsed.items[i].count = Detail::GetI32(entry + 4);
            if (parsed.items[i].itemId < 1 || parsed.items[i].count < 1)
                return false;
        }
        out = parsed;
        return true;
    }
}
