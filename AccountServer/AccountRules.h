#pragma once
// ==========================================================================
// CAccountRules — 계정 서버의 순수 판정: 로그인 이름 형식 · 결과값 이름
//
//   네트워크도 DB도 모른다. 갈래 전수 시험은 Tests/AccountUnitTest.cpp.
// ==========================================================================

#include <cstdint>
#include <string>

#include <Protocol/AccountProtocol.h>
#include <Protocol/ServerLinkProtocol.h>

// 계정 우편 요청 종류 — mmo_account_mail_total{op} 의 차례(mail-plan.md 6절 · 9절 「계정 서버 CAccountMailFlow」)
enum class AccMailOp : uint8_t
{
    Count = 0,   // 붉은 수 · 이 캐릭터가 받는 중인 행
    List,
    Read,
    Claim,       // 받는 중 표시
    Done,        // 받음 — 답 없음
    Unclaim,     // 받는 중 풀기 — 답 없음
    Erase,
    Send,        // 운영자 지급(한 계정 · 전 계정)
};
inline constexpr int ACC_MAIL_OP_COUNT = 8;

class CAccountRules
{
public:
    // 영문·숫자 4~16자. 칸 안에 NUL 이 없으면 틀림. NUL 뒤 바이트는 안 본다
    static bool IsValidLoginName(const char (&name)[ACCOUNT_LOGIN_NAME_BYTES]);

    // 대소문자만 다른 이름은 같은 계정이다(DB 비교 규칙이 판정한다).
    //   DB 워커를 고르는 열쇠도 같아야 차례가 안 뒤바뀌므로 소문자로 맞춘다
    static std::string LowerName(const std::string& name);

    // 지표 라벨 값 — 정한 목록뿐이다
    static const char* LoginResultName(AccountLoginResult result);
    static const char* TokenResultName(TokenCheckResult result);
    static const char* MailOpName(AccMailOp op);
    static const char* MailResultName(LinkMailResult result);
};
