#pragma once
// ==========================================================================
// CLookTable — 고를 수 있는 외형 (Run/bin/LookTable.csv)
//
//   시작할 때 읽어 없는 번호를 BAD_LOOK 으로 거절한다. 웹은 같은 파일에서 번호를 그림으로 바꾼다.
//   표에 없는 것 = 해골 · 좀비 · 초록 피부(사용자 결정 11-04).
//   파일이 틀리면 Load 가 false 이고 서버가 안 뜬다 — 빈 표로 뜨면 만들기가 전부 BAD_LOOK 으로만 보인다.
// ==========================================================================

#include <cstdint>
#include <string>

#include <Identity/CharacterLookBlob.h>

class CLookTable
{
public:
    enum class Part : int { Skin = 0, Hair = 1, Top = 2, Bottom = 3, COUNT };

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다
    bool Load(const std::string& path);

    // 성별과 부위 넷이 전부 표에 있으면 true. 와이어로 온 값을 그대로 넣어도 된다
    bool IsAllowed(const CharacterLookBlob::Look& look) const;

    int Count(uint8_t gender, Part part) const;   // 시험 · 기동 로그용

private:
    static constexpr int GENDER_SLOTS = 3;   // 0 은 안 쓴다 — 1 남 · 2 여
    static constexpr int ID_SLOTS = 256;     // 1바이트 칸이라 256 을 못 넘는다

    bool Has(uint8_t gender, Part part, uint8_t id) const;

    bool _allowed[GENDER_SLOTS][static_cast<int>(Part::COUNT)][ID_SLOTS]{};
};
