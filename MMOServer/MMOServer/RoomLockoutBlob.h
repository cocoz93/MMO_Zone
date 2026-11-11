// ==========================================================================
// RoomLockoutBlob — characters.room_lockouts 열의 바이트 형식
//
// [책임]
//  - 묶기: (방 맵 번호, 끝나는 시각) 목록 → 바이트 · 풀기: 바이트 → 목록. 형식(판 번호·길이·개수)만 본다
//  - 지난 칸 빼기와 캐릭터 칸 쓰기는 캐릭터 저장소(CCharacterStore::RestoreRoomLockouts) 몫이다
//
// [형식] 숫자는 little-endian 고정(시프트로 쓴다 — 빌드 대상과 무관)
//   판 1 : [판 번호 1B = 1][칸 수 1B] { [방 맵 번호 4B][끝나는 시각 8B] } × 칸 수
//   시각 = UTC 밀리초(1970 기준). 프레임 번호는 존 서버마다 다르고 재시작하면 0부터 세므로 쓸 수 없다.
//   끝나는 시각을 적으므로 접속을 끊어도 시간이 흐른다. 빈 값(길이 0) = 잠금 없음.
//
// [계층 경계] 캐릭터·맵 표·DB 를 모른다 — 바이트와 목록만. 그래서 형식을 아는 곳이 여기 한 곳이다.
// ==========================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace RoomLockoutBlob
{
    constexpr uint8_t VERSION      = 1;
    constexpr size_t  HEADER_BYTES = 2;        // 판 번호 + 칸 수
    constexpr size_t  ENTRY_BYTES  = 4 + 8;    // 방 맵 번호 + 끝나는 시각
    constexpr int     MAX_SLOTS    = 16;       // 칸 상한 — 194바이트라 VARBINARY(255) 에 든다. 방 맵이 이보다 많으면 맵 표가 기동을 거절한다(CMapTable::Load)
    constexpr size_t  MAX_BYTES    = HEADER_BYTES + static_cast<size_t>(MAX_SLOTS) * ENTRY_BYTES;

    struct Lockout
    {
        int32_t mapId;
        int64_t untilMs;   // 끝나는 실제 시각(UTC ms)
    };

    // 이 형식의 시각 단위로 지금을 잰다 — 잠금을 넣는 쪽·판정하는 쪽·불러오는 쪽이 같은 시계를 본다
    int64_t NowMs();

    // 묶기 — n 개를 out 에 쓰고 쓴 길이를 돌려준다. n 이 0 이면 0(빈 값 — 머리도 안 쓴다). cap 이 모자라면 0.
    size_t PackLockouts(const Lockout* items, int n, uint8_t* out, size_t cap);

    // 풀기 결과 — 빈 값과 불량을 섞지 않는다(불량은 지표·로그로 남겨야 한다)
    enum class Unpack : uint8_t
    {
        EMPTY,   // 길이 0 — 잠금 없음(정상)
        OK,      // out 에 n 개(1 이상)
        BAD,     // 판 번호 모름 · 칸 수 0 인데 머리가 있음 · 길이가 칸 수와 안 맞음 · maxN 초과 — 열을 통째로 버린다
        COUNT
    };
    Unpack UnpackLockouts(const uint8_t* data, size_t len, Lockout* out, int maxN, int& n);
}
