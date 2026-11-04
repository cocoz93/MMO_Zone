#pragma once
// ==========================================================================
// CCharacterRules — 캐릭터 서버의 순수 판정: 자리 번호 · 유예 계산 · 유일 키 갈래 · 결과값 이름
//
//   네트워크도 DB도 모른다. 갈래 전수 시험은 Tests/CharacterUnitTest.cpp.
//   이름 글자는 Shared/Identity/CharacterName.h(존 서버도 같은 규칙으로 읽는다).
// ==========================================================================

#include <cstdint>

#include <Protocol/CharacterProtocol.h>

class CCharacterRules
{
public:
    static constexpr int64_t SECONDS_PER_DAY = 86400;

    static bool IsValidSlot(int slot, int slotCount) { return slot >= 0 && slot < slotCount; }

    enum class DeleteState
    {
        Alive,     // deleted_at 없음
        Pending,   // 삭제 대기 — 목록에 나오고 복구할 수 있다
        Expired,   // 유예가 지남 — 목록 · 복구 · 입장이 막힌다
    };

    // elapsedSec = 지운 뒤 지난 초. 판정 시계는 DB 하나다(서버 사이의 시각 비교가 없다)
    static DeleteState ClassifyDelete(bool hasDeletedAt, int64_t elapsedSec, int graceDays);

    // 삭제 대기의 남은 초 — 0 은 「삭제 대기 아님」이라서 대기 중이면 1 이상을 돌려준다
    static int32_t GraceLeftSec(int64_t elapsedSec, int graceDays);

    enum class DuplicateKey { Name, Slot, Unknown };

    // 1062 오류 문구의 키 이름으로 가른다(플랜 6절)
    static DuplicateKey ClassifyDuplicate(const char* errorText);

    // 지표 라벨 값 — 결과값의 소문자 이름
    static const char* ResultName(CharLoginResult result);
    static const char* ResultName(CharCreateResult result);
    static const char* ResultName(CharDeleteResult result);
    static const char* ResultName(CharRestoreResult result);
    static const char* ResultName(CharSelectResult result);
};
