// ==========================================================================
// CooldownBuffBlob — characters.skill_cooldowns · buffs 열의 바이트 형식
//
// [책임]
//  - 묶기: (번호, 끝 시각) 목록 → 바이트 · 풀기: 바이트 → 목록. 형식(버전·길이·개수)만 본다
//  - 내용 검사(스킬·버프 번호가 표에 있나)와 틱 변환·캐릭터 칸 쓰기는 캐릭터 저장소(CCharacterStore) 몫이다
//    (RestoreCooldownsAndBuffs · PackCooldownsAndBuffs)
//
// [형식] 열마다 따로, 버전도 열마다 따로. 숫자는 little-endian 고정(시프트로 쓴다 — 빌드 대상과 무관)
//   skill_cooldowns 판 2 : [버전 1B = 2][개수 1B] { [스킬 번호 2B][다시 쓸 수 있는 시각 8B] }  × 개수   ← 쓰기는 이 판만
//   skill_cooldowns 판 1 : [버전 1B = 1][개수 1B] { [스킬 번호 1B][다시 쓸 수 있는 시각 8B] }  × 개수   ← 읽기만(11-01 전 행)
//   buffs           판 2 : [버전 1B = 2][개수 1B] { [버프 번호 2B][끝나는 시각 8B] }            × 개수   ← 쓰기는 이 판만
//   buffs           판 1 : [버전 1B = 1][개수 1B] { [종류 1B][값 4B][끝나는 시각 8B] }        × 개수   ← 읽기만(11-02 전 행). 종류 0(공격력 배수) = 버프 1(이중공격) · 값은 버린다(표 몫)
//   시각 = UTC 밀리초(1970 기준) — 접속하지 않은 동안에도 흐른다. 빈 값(길이 0) = 돌고 있는 것 없음.
//   ★ 형식을 바꾸면 버전을 올리고, 옛 판은 읽어서 새 판으로 옮긴다(11-01 사용자 결정 — 잃으면 쿨타임·버프가 풀리는 정도라도 버리지 않는다).
//     옮기기 = 풀기가 옛 판을 새 판과 같은 목록으로 돌려주고(outOldFormat), 저장소가 다음 저장에 새 판으로 쓰게 한다(CCharacterStore::RestoreCooldownsAndBuffs).
//     모르는 버전만 BAD(그 열을 버린다).
//
// [계층 경계] 캐릭터·스킬 표·DB 를 모른다 — 바이트와 목록만. 그래서 형식을 아는 곳이 여기 한 곳이다.
//   토글도 안 본다 — 스킬·전투를 끈 빌드는 부르지 않을 뿐이다.
// ==========================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace CooldownBuffBlob
{
    constexpr uint8_t COOLDOWN_VERSION    = 2;           // 쓰는 판
    constexpr uint8_t COOLDOWN_VERSION_V1 = 1;           // 읽기만 하는 옛 판(스킬 번호 1바이트)
    constexpr uint8_t BUFF_VERSION        = 2;           // 쓰는 판
    constexpr uint8_t BUFF_VERSION_V1     = 1;           // 읽기만 하는 옛 판(종류 + 값)
    constexpr size_t  HEADER_BYTES        = 2;           // 버전 + 개수
    constexpr size_t  COOLDOWN_BYTES      = 2 + 8;       // 스킬 번호(u16) + 다시 쓸 수 있는 시각
    constexpr size_t  COOLDOWN_BYTES_V1   = 1 + 8;       // 옛 판 — 스킬 번호(u8) + 시각
    constexpr size_t  BUFF_BYTES          = 2 + 8;       // 버프 번호(u16) + 끝나는 시각
    constexpr size_t  BUFF_BYTES_V1       = 1 + 4 + 8;   // 옛 판 — 종류 + 값 + 끝나는 시각
    constexpr uint16_t BUFF_ID_OF_V1_ATTACK_MUL = 1;     // 옛 판 종류 0(공격력 배수)을 맡은 버프 — 그때 사람에게 걸리는 효과는 이중공격뿐이었다(격노는 몬스터 전용이라 DB 에 없다)

    // 열의 최대 길이 — 칸이 가득 찼을 때(쓰는 판 기준 — 옛 판은 항목이 더 짧다). DB 쪽 버퍼(DBWorker.h DB_SKILL_COOLDOWNS_BYTES · DB_BUFFS_BYTES)와 대조한다(CharacterStore.cpp).
    constexpr size_t CooldownsMaxBytes(int slots) { return HEADER_BYTES + static_cast<size_t>(slots) * COOLDOWN_BYTES; }
    constexpr size_t BuffsMaxBytes(int rows)      { return HEADER_BYTES + static_cast<size_t>(rows) * BUFF_BYTES; }

    struct Cooldown
    {
        uint16_t skillId;
        int64_t  readyAtMs;   // 다시 쓸 수 있는 실제 시각(UTC ms)
    };

    struct Buff
    {
        uint16_t buffId;      // 표에 있나는 호출부가 본다(표를 아는 쪽). 옛 판의 모르는 종류는 0(표에 없는 번호)으로 온다
        int64_t  expireAtMs;  // 끝나는 실제 시각(UTC ms)
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

    // outOldFormat = 옛 판(1)을 읽었다 — OK 일 때만 뜻이 있다. 부르는 쪽이 다음 저장에 새 판으로 쓰게 한다(옮기기)
    Unpack UnpackCooldowns(const uint8_t* data, size_t len, Cooldown* out, int maxN, int& n, bool& outOldFormat);
    Unpack UnpackBuffs(const uint8_t* data, size_t len, Buff* out, int maxN, int& n, bool& outOldFormat);
}
