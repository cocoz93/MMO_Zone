#pragma once
// ==========================================================================
// CAccountDb — 계정 표의 prepared 문장과 바인딩. 스레드와 큐는 CDbPipeline 이 맡는다
//
//   DB 워커 스레드에서 돈다. mysql.h 를 헤더에 안 넣는다(CDbConnection 과 같은 까닭).
// ==========================================================================

#include <cstdint>
#include <string>

#include <Db/DbConnection.h>
#include <Identity/GameIds.h>
#include <Protocol/ServerLinkProtocol.h>

class CAccountDb
{
public:
    // mmo_db_query_seconds 의 op — OpName 과 같은 차례
    enum Op : int
    {
        OP_LOGIN = 0,
        OP_MAIL_COUNT,
        OP_MAIL_LIST,
        OP_MAIL_READ,
        OP_MAIL_CLAIM,
        OP_MAIL_DONE,
        OP_MAIL_UNCLAIM,
        OP_MAIL_ERASE,
        OP_MAIL_SEND_ONE,
        OP_MAIL_SEND_ALL,
        OP_COUNT
    };

    static const char* OpName(Op op);

    struct LoginRow
    {
        bool      ok = false;        // false = DB 실패(이름 잘못이 아니다)
        bool      created = false;   // 이번에 새로 만들었다
        AccountId accountId = NO_ACCOUNT;
    };

    // 있으면 번호를 읽고, 없으면 INSERT 로 발급한다. 끝에 last_login_at 을 갱신한다.
    //   같은 이름이 동시에 오면 유일 키가 한쪽을 막고(1062), 막힌 쪽은 다시 읽어 같은 번호를 받는다.
    //   워커가 달라도, 계정 서버가 둘이어도 답이 같다 — 워커 고르기에 기대지 않는다.
    static LoginRow FindOrCreate(CDbConnection& conn, const std::string& loginName);

    // ── 계정 우편(mail-plan.md 6절) ──
    //   「살아 있는」 우편 = 받는 중(기한과 무관) · 또는 안 받았고 기한 안. 받음 · 지움은 어디에도 안 나온다.
    //   글자는 UTF-8 로 받고(부르는 쪽이 TextUtf 로 검사 · 변환) 읽을 때 와이어 칸으로 바꾼다 — 바꿀 수 없는 행은 오류 로그를 남기고 빈 칸으로 둔다

    struct MailCount
    {
        bool     ok = false;
        uint16_t count = 0;         // 안 받았고 (안 읽었거나 돈 · 첨부가 남은) 기한 안의 우편 수 — 65535 에서 멈춘다
        int64_t  claimMailId = 0;   // character 가 받는 중인 가장 작은 번호 · 0 = 없음
    };
    static MailCount CountMail(CDbConnection& conn, int64_t account, int64_t character);

    // 큰 번호부터 한 장(최대 LINK_ACCMAIL_LIST_ROWS 줄). beforeMailId 0 = 처음부터
    struct MailList
    {
        bool ok = false;
        bool more = false;   // 이 장 뒤에 더 있다(한 줄 더 읽어 본다)
        int  count = 0;
        LinkAccMailLine lines[LINK_ACCMAIL_LIST_ROWS]{};
    };
    static MailList ListMail(CDbConnection& conn, int64_t account, int64_t beforeMailId);

    enum class Found { Yes, No, Failed };
    struct MailOne
    {
        Found found = Found::Failed;
        LinkAccMailFull mail{};
    };
    // 읽기 — 살아 있으면 read_at 을 처음 한 번만 적고 한 통을 돌려준다
    static MailOne ReadMail(CDbConnection& conn, int64_t account, int64_t mailId);
    // 받는 중 표시 — 안 받았고 기한 안이면 character 로 표시한다. 그 뒤 늘 다시 읽는다(이미 받는 중이면 적혀 있는 캐릭터가 나온다)
    static MailOne ClaimMail(CDbConnection& conn, int64_t account, int64_t mailId, int64_t character);

    // 결과 없는 쓰기 — rows = 바뀐 행 수
    struct MailWrite
    {
        bool    ok = false;
        int64_t rows = 0;
    };
    static MailWrite DoneMail(CDbConnection& conn, int64_t account, int64_t mailId, int64_t character);      // 받는 중(character) → 받음
    static MailWrite UnclaimMail(CDbConnection& conn, int64_t account, int64_t mailId, int64_t character);   // 받는 중(character) → 안 받음
    static MailWrite EraseMail(CDbConnection& conn, int64_t account, int64_t mailId);                        // 안 받음 · 돈 0 · 첨부 없음 → 지움

    struct SendRequest
    {
        int64_t     targetAccount = 0;   // 0 = 전 계정
        int64_t     batchId = 0;         // 0 = 없음(한 계정 지급만)
        int64_t     sentBy = 0;
        int64_t     money = 0;
        std::string title;               // UTF-8
        std::string body;                // UTF-8
        std::string attach;              // 존 서버가 만든 바이트 그대로
        int         expireDays = 0;
    };
    // 한 계정이면 accounts 에 있을 때만 한 줄(rows 0 = 없는 계정) · 전 계정이면 한 문장으로 전부(같은 batchId 는 uq_batch 로 두 번 안 들어간다)
    static MailWrite SendMail(CDbConnection& conn, const SendRequest& request);
};
