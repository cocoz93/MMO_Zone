#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ==========================================================================
// CJobTable — 직업 목록 (Run/bin/JobTable.csv)
//
// [클래스 없음] 직업마다 클래스를 두지 않는다. 직업이 가르는 것은 숫자와 목록뿐이다 —
//        스탯은 레벨 표의 job 칸, 기술은 스킬 표의 job 칸. 직업 추가 = 이 표 한 줄 + 두 표에 줄.
//
// [토글 없음] 맵 표처럼 늘 읽는다 — 캐릭터 칸(CPlayer::_job)과 DB 열이 토글 없이 늘 있어서.
//
// [0 은 직업이 아니다] 0 = 아직 안 고름(DB 기본값). 표에는 1 부터 적는다.
//        BASE_JOB(1) 줄은 반드시 있어야 한다 — 고르기 전 캐릭터의 스탯과, 레벨 표에 자기 줄이 없는 직업의 스탯이 이 직업 줄을 쓴다.
// ==========================================================================
class CJobTable
{
public:
    static constexpr int32_t BASE_JOB   = 1;
    static constexpr int32_t MAX_JOB_ID = 64;    // 레벨 표가 번호를 색인으로 쓴다 — 오타 하나가 큰 배열을 만들지 않게

    struct JobDef
    {
        int32_t     jobId = 0;
        std::string name;     // UTF-8. 서버는 로그에만 쓴다 — 화면 글자는 웹이 같은 파일에서 읽는다
    };

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    bool Load(const std::string& path);

    // 와이어로 온 번호(아무 값이나 올 수 있다)를 그대로 넣어도 된다. 0 은 직업이 아니라 false.
    bool Has(int32_t jobId) const;

private:
    std::vector<JobDef> _rows;
};
