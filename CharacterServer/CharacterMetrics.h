#pragma once
// ==========================================================================
// CCharacterMetrics — 캐릭터 서버 지표 값과 지표 글자
//
//   모든 갈래를 시작부터 0 으로 내보낸다 — 줄이 처음 생기는 순간의 증가는 rate() 가 못 보고,
//   줄이 없으면 「0번」과 「못 읽음」을 가르지 못한다(플랜 6절).
//   kind 는 자기 결과값만 낸다 — 만들기에 not_mine 줄이 있으면 갈래 개수가 거짓이 된다.
//   세기는 로직 스레드 · 읽기는 지표 스레드라 값마다 원자 값이다.
// ==========================================================================

#include <atomic>
#include <cstdint>
#include <sstream>

#include <Protocol/CharacterProtocol.h>

class CCharacterMetrics
{
public:
    void Count(CharLoginResult result) { Add(_login[static_cast<int>(result)]); }
    void Count(CharCreateResult result) { Add(_create[static_cast<int>(result)]); }
    void Count(CharDeleteResult result) { Add(_delete[static_cast<int>(result)]); }
    void Count(CharRestoreResult result) { Add(_restore[static_cast<int>(result)]); }
    void Count(CharSelectResult result) { Add(_select[static_cast<int>(result)]); }

    void SetOnline(int count) { _online.store(count, std::memory_order_relaxed); }
    void SetZones(int count) { _zones.store(count, std::memory_order_relaxed); }

    void Write(std::ostringstream& ss) const;

private:
    static void Add(std::atomic<int64_t>& value) { value.fetch_add(1, std::memory_order_relaxed); }

    std::atomic<int64_t> _login[static_cast<int>(CharLoginResult::COUNT)]{};
    std::atomic<int64_t> _create[static_cast<int>(CharCreateResult::COUNT)]{};
    std::atomic<int64_t> _delete[static_cast<int>(CharDeleteResult::COUNT)]{};
    std::atomic<int64_t> _restore[static_cast<int>(CharRestoreResult::COUNT)]{};
    std::atomic<int64_t> _select[static_cast<int>(CharSelectResult::COUNT)]{};
    std::atomic<int64_t> _online{ 0 };
    std::atomic<int64_t> _zones{ 0 };
};
