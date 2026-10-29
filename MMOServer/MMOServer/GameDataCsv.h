#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ==========================================================================
// GameDataCsv — 기획 데이터 CSV 를 줄과 칸으로 쪼개는 도우미
//
// [왜 따로 있나] 표끼리 같은 것은 "파일을 줄과 칸으로 쪼개는 일"뿐이다(GameDataFiles.h [상속 안 씀]).
//        처음엔 레벨 표 혼자 써서 LevelTable.cpp 안에 두었고, 아이템 표가 생기며 여기로 옮겼다.
//        열이 몇이고 무슨 뜻인지, 어떤 규칙으로 검사하는지는 각 표가 정한다 — 여기는 모른다.
// ==========================================================================
namespace GameDataCsv
{
    // 값으로 읽을 줄 하나 — 줄 번호는 파일 기준이라 오류 메시지가 사람이 볼 줄을 가리킨다.
    struct DataLine { int fileLine; std::string text; };

    // 값으로 읽을 줄만 남긴다 — 빈 줄 · '#' 주석 · 헤더(첫 칸이 숫자가 아닌 줄) 를 버린다.
    //   파일을 못 열면 false. UTF-8 BOM 이 붙어 있으면 턴다.
    bool ReadDataLines(const std::string& path, std::vector<DataLine>& out);

    // 한 줄을 쉼표로 쪼갠다. 앞뒤 공백과 CR 을 턴다(CRLF 파일을 텍스트 모드로 읽어도 안전하게).
    //   ※ 따옴표 안의 쉼표는 모른다 — 칸 안에 쉼표를 쓰지 않는다는 약속으로 산다.
    void SplitCsv(const std::string& line, std::vector<std::string>& out);

    // 문자열이 통째로 정수인가 (뒤에 쓰레기가 붙은 "12abc" 를 12 로 받지 않게)
    bool ParseInt(const std::string& s, int64_t& out);

    // 문자열이 통째로 실수인가 ("1.5" · "2" — 뒤에 쓰레기가 붙은 "1.5x"·"nan"·"inf" 는 아니다)
    bool ParseFloat(const std::string& s, double& out);
}
