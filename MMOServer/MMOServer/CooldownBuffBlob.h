// ==========================================================================
// CooldownBuffBlob — characters.skill_cooldowns · buffs 열의 바이트 형식
//
// [책임]
//  - 묶기: (번호, 끝 시각) 목록 → 바이트 · 풀기: 바이트 → 목록. 형식(버전·길이·개수)만 본다
//  - 내용 검사(스킬 번호가 표에 있나 · 버프 종류·값)와 틱 변환·캐릭터 칸 쓰기는 캐릭터 저장소(CCharacterStore) 몫이다
//    (RestoreCooldownsAndBuffs · PackCooldownsAndBuffs)
//
// [형식 — 버전 1] 열마다 따로. 숫자는 little-endian 고정(시프트로 쓴다 — 빌드 대상과 무관)
//   skill_cooldowns : [버전 1B][개수 1B] { [스킬 번호 1B][다시 쓸 수 있는 시각 8B] }  × 개수
//   buffs           : [버전 1B][개수 1B] { [종류 1B][값 4B][끝나는 시각 8B] }        × 개수
//   시각 = UTC 밀리초(1970 기준) — 접속하지 않은 동안에도 흐른다. 빈 값(길이 0) = 돌고 있는 것 없음.
//   ★ 형식을 바꾸면 버전을 올린다 — 옛 버전은 풀기가 BAD 로 돌려 그 열을 버린다.
//     잃어도 쿨타임·버프가 풀리는 정도라 옛 형식을 옮기는 코드는 안 짠다.
//
// [계층 경계] 캐릭터·스킬 표·DB 를 모른다 — 바이트와 목록만. 그래서 형식을 아는 곳이 여기 한 곳이다.
//   토글도 안 본다 — 스킬·전투를 끈 빌드는 부르지 않을 뿐이다.
// ==========================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace CooldownBuffBlob
{
    constexpr uint8_t FORMAT_VERSION = 1;
    constexpr size_t  HEADER_BYTES   = 2;           // 버전 + 개수
    constexpr size_t  COOLDOWN_BYTES = 1 + 8;       // 스킬 번호 + 다시 쓸 수 있는 시각
    constexpr size_t  BUFF_BYTES     = 1 + 4 + 8;   // 종류 + 값 + 끝나는 시각

    // 열의 최대 길이 — 칸이 가득 찼을 때. DB 쪽 버퍼(DBWorker.h DB_SKILL_COOLDOWNS_BYTES · DB_BUFFS_BYTES)와 대조한다(GameServer.cpp).
    constexpr size_t CooldownsMaxBytes(int slots) { return HEADER_BYTES + static_cast<size_t>(slots) * COOLDOWN_BYTES; }
    constexpr size_t BuffsMaxBytes(int kinds)     { return HEADER_BYTES + static_cast<size_t>(kinds) * BUFF_BYTES; }

    struct Cooldown
    {
        uint8_t skillId;
        int64_t readyAtMs;    // 다시 쓸 수 있는 실제 시각(UTC ms)
    };

    struct Buff
    {
        uint8_t kind;         // BuffKind 값 — 범위 검사는 호출부(종류를 아는 쪽)
        int32_t value;
        int64_t expireAtMs;   // 끝나는 실제 시각(UTC ms)
    };

    // 묶기 — n 개를 out 에 쓰고 쓴 길이를 돌려준다. n 이 0 이면 0(빈 값 — 헤더도 안 쓴다).
    //   cap 이 모자라면 0(빈 값)을 돌려준다 — 호출부 static_assert 가 막는 자리라 실제로는 안 나지만 버퍼는 넘지 않게.
    size_t PackCooldowns(const Cooldown* items, int n, uint8_t* out, size_t cap);
    size_t PackBuffs(const Buff* items, int n, uint8_t* out, size_t cap);

    // 풀기 결과 — 빈 값과 불량을 섞지 않는다(불량은 지표·로그로 남겨야 한다).
    enum class Unpack : uint8_t
    {
        EMPTY,   // 길이 0 — 돌고 있는 것 없음(정상)
        OK,      // out 에 n 개(1 이상)
        BAD,     // 버전 모름 · 개수 0 인데 헤더가 있음 · 길이가 개수와 안 맞음 · maxN 초과 — 열을 통째로 버린다
        COUNT
    };

    Unpack UnpackCooldowns(const uint8_t* data, size_t len, Cooldown* out, int maxN, int& n);
    Unpack UnpackBuffs(const uint8_t* data, size_t len, Buff* out, int maxN, int& n);
}
