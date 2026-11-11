#include "AccountDb.h"

#ifdef _WIN32
#include <winsock2.h>   // mysql.h 이전에 — 구버전 winsock.h 가 끌려오지 않게
#include <mysql.h>
#else
#include <mysql/mysql.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <Common/ErrorLog.h>
#include <Text/TextUtf.h>

// 계정 우편의 「살아 있는」 행 — 받는 중은 기한과 무관하게 보인다(mail-plan.md 2절 11번 · 6절 「입장 때 다시 처리」)
#define ACC_MAIL_ALIVE "(state=1 OR (state=0 AND expires_at>NOW(3)))"
// 남은 초 — 지났으면 0. DB 시각으로 센다(서버 시계를 안 쓴다 · 3절)
#define ACC_MAIL_REMAIN "GREATEST(TIMESTAMPDIFF(SECOND, NOW(3), expires_at), 0)"

namespace
{
    constexpr unsigned int ER_DUPLICATE_KEY = 1062;   // ER_DUP_ENTRY
    constexpr int MAIL_TITLE_BYTES = LINK_MAIL_TITLE_CHARS * 4;   // VARCHAR(20) utf8mb4
    constexpr int MAIL_BODY_BYTES  = LINK_MAIL_BODY_CHARS * 4;    // VARCHAR(200) utf8mb4
    constexpr int MAIL_ATTACH_COLUMN = 255;                       // VARBINARY(255) — 와이어 칸(66)보다 크면 손으로 고친 행이다

    enum class SelectOutcome { Found, Missing, Failed };
    enum class InsertOutcome { Inserted, Duplicate, Failed };

    MYSQL_BIND NameBind(const std::string& name, unsigned long& length)
    {
        length = static_cast<unsigned long>(name.size());
        MYSQL_BIND bind;
        std::memset(&bind, 0, sizeof(bind));
        bind.buffer_type = MYSQL_TYPE_STRING;
        bind.buffer = const_cast<char*>(name.data());
        bind.buffer_length = length;
        bind.length = &length;
        return bind;
    }

    SelectOutcome SelectId(CDbConnection& conn, const std::string& name, int64_t& outId)
    {
        MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(conn.Prepare("SELECT account_id FROM accounts WHERE login_name=?"));
        if (stmt == nullptr)
            return SelectOutcome::Failed;

        unsigned long nameLength = 0;
        MYSQL_BIND in = NameBind(name, nameLength);
        if (mysql_stmt_bind_param(stmt, &in) != 0 || mysql_stmt_execute(stmt) != 0)
        {
            conn.NoteStmtError(stmt, "accounts select");
            return SelectOutcome::Failed;
        }

        int64_t id = 0;
        MYSQL_BIND out;
        std::memset(&out, 0, sizeof(out));
        out.buffer_type = MYSQL_TYPE_LONGLONG;
        out.buffer = &id;
        if (mysql_stmt_bind_result(stmt, &out) != 0)
        {
            conn.NoteStmtError(stmt, "accounts select bind");
            return SelectOutcome::Failed;
        }

        const int fetched = mysql_stmt_fetch(stmt);
        if (fetched != 0 && fetched != MYSQL_NO_DATA)
        {
            conn.NoteStmtError(stmt, "accounts select fetch");
            return SelectOutcome::Failed;
        }
        mysql_stmt_free_result(stmt);

        if (fetched == MYSQL_NO_DATA)
            return SelectOutcome::Missing;
        outId = id;
        return SelectOutcome::Found;
    }

    InsertOutcome InsertAccount(CDbConnection& conn, const std::string& name, int64_t& outId)
    {
        MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(conn.Prepare("INSERT INTO accounts(login_name) VALUES(?)"));
        if (stmt == nullptr)
            return InsertOutcome::Failed;

        unsigned long nameLength = 0;
        MYSQL_BIND in = NameBind(name, nameLength);
        if (mysql_stmt_bind_param(stmt, &in) != 0)
        {
            conn.NoteStmtError(stmt, "accounts insert bind");
            return InsertOutcome::Failed;
        }
        if (mysql_stmt_execute(stmt) != 0)
        {
            // 유일 키에 막힌 것은 실패가 아니다 — 실패 표시 없이 돌아가 다시 읽는다
            if (mysql_stmt_errno(stmt) == ER_DUPLICATE_KEY)
                return InsertOutcome::Duplicate;
            conn.NoteStmtError(stmt, "accounts insert");
            return InsertOutcome::Failed;
        }
        outId = static_cast<int64_t>(mysql_stmt_insert_id(stmt));
        return InsertOutcome::Inserted;
    }

    bool TouchLastLogin(CDbConnection& conn, int64_t id)
    {
        MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(conn.Prepare("UPDATE accounts SET last_login_at=CURRENT_TIMESTAMP(3) WHERE account_id=?"));
        if (stmt == nullptr)
            return false;

        MYSQL_BIND in;
        std::memset(&in, 0, sizeof(in));
        in.buffer_type = MYSQL_TYPE_LONGLONG;
        in.buffer = &id;
        if (mysql_stmt_bind_param(stmt, &in) != 0 || mysql_stmt_execute(stmt) != 0)
        {
            conn.NoteStmtError(stmt, "accounts update");
            return false;
        }
        return true;
    }

    // ── 계정 우편 바인딩 — CharacterDb.cpp 와 같은 꼴 ──

    MYSQL_BIND BindLong(int64_t& value)
    {
        MYSQL_BIND bind;
        std::memset(&bind, 0, sizeof(bind));
        bind.buffer_type = MYSQL_TYPE_LONGLONG;
        bind.buffer = &value;
        return bind;
    }

    MYSQL_BIND BindInt(int& value)
    {
        MYSQL_BIND bind;
        std::memset(&bind, 0, sizeof(bind));
        bind.buffer_type = MYSQL_TYPE_LONG;
        bind.buffer = &value;
        return bind;
    }

    MYSQL_BIND BindText(enum_field_types type, const void* data, unsigned long& length)
    {
        MYSQL_BIND bind;
        std::memset(&bind, 0, sizeof(bind));
        bind.buffer_type = type;
        bind.buffer = const_cast<void*>(data);
        bind.buffer_length = length;
        bind.length = &length;
        return bind;
    }

    // 결과 없는 문장. 실패하면 false — 실패 표시까지 끝낸다
    bool ExecStmt(CDbConnection& conn, const char* sql, MYSQL_BIND* binds, const char* what, int64_t* affected = nullptr)
    {
        MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(conn.Prepare(sql));
        if (stmt == nullptr)
            return false;
        if (mysql_stmt_bind_param(stmt, binds) != 0 || mysql_stmt_execute(stmt) != 0)
        {
            conn.NoteStmtError(stmt, what);
            return false;
        }
        if (affected != nullptr)
            *affected = static_cast<int64_t>(mysql_stmt_affected_rows(stmt));
        return true;
    }

    uint32_t ClampRemain(int64_t sec)
    {
        return static_cast<uint32_t>(std::clamp<int64_t>(sec, 0, UINT32_MAX));
    }

    // DB 의 UTF-8 → 와이어 칸. 패킷 칸은 줄이 안 맞아 참조로 못 넘긴다 — 줄 맞은 칸에 받아 복사한다
    template <int N>
    void CopyText(const char* data, unsigned long size, char16_t (&wire)[N], int64_t mailId, const char* what)
    {
        char16_t cells[N];
        if (!TextUtf::FromUtf8(data, size, cells, N))
            SLOG_ERROR("[AccountDb] 계정 우편 {} 의 {} 이 글자 규칙에 어긋남 — 손으로 고친 행이다. 빈 칸으로 보냄", mailId, what);
        std::memcpy(wire, cells, sizeof(cells));
    }

    // 한 통 읽기 — 살아 있는 행만. 없으면 No
    CAccountDb::MailOne SelectOne(CDbConnection& conn, int64_t account, int64_t mailId)
    {
        CAccountDb::MailOne one;
        MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(conn.Prepare(
            "SELECT mail_id, state, COALESCE(claim_character_id, 0), money, " ACC_MAIL_REMAIN ", title, body, attach "
            "FROM account_mail WHERE mail_id=? AND account_id=? AND " ACC_MAIL_ALIVE));
        if (stmt == nullptr)
            return one;

        MYSQL_BIND in[2] = { BindLong(mailId), BindLong(account) };
        if (mysql_stmt_bind_param(stmt, in) != 0 || mysql_stmt_execute(stmt) != 0)
        {
            conn.NoteStmtError(stmt, "account_mail select");
            return one;
        }

        int64_t id = 0;
        int state = 0;
        int64_t claim = 0;
        int64_t money = 0;
        int64_t remain = 0;
        char title[MAIL_TITLE_BYTES + 1]{};
        unsigned long titleLength = MAIL_TITLE_BYTES;
        char body[MAIL_BODY_BYTES + 1]{};
        unsigned long bodyLength = MAIL_BODY_BYTES;
        uint8_t attach[MAIL_ATTACH_COLUMN]{};
        unsigned long attachLength = sizeof(attach);
        MYSQL_BIND out[8] = {
            BindLong(id), BindInt(state), BindLong(claim), BindLong(money), BindLong(remain),
            BindText(MYSQL_TYPE_STRING, title, titleLength), BindText(MYSQL_TYPE_STRING, body, bodyLength),
            BindText(MYSQL_TYPE_BLOB, attach, attachLength)
        };
        if (mysql_stmt_bind_result(stmt, out) != 0)
        {
            conn.NoteStmtError(stmt, "account_mail select bind");
            return one;
        }

        const int fetched = mysql_stmt_fetch(stmt);
        if (fetched != 0 && fetched != MYSQL_NO_DATA)
        {
            conn.NoteStmtError(stmt, "account_mail select fetch");   // 잘림(MYSQL_DATA_TRUNCATED)도 여기 — 칸이 열 최대라 생기면 열이 바뀐 것이다
            return one;
        }
        mysql_stmt_free_result(stmt);

        if (fetched == MYSQL_NO_DATA)
        {
            one.found = CAccountDb::Found::No;
            return one;
        }
        if (attachLength > static_cast<unsigned long>(LINK_MAIL_ATTACH_BYTES))
        {
            // 일부만 보내면 존 서버가 다른 첨부로 읽을 수 있다 — 받기를 막고 운영자가 고치게 둔다
            SLOG_ERROR("[AccountDb] 계정 우편 {} 의 첨부가 {}B — 와이어 칸 {}B 를 넘음(손으로 고친 행). 읽기 · 받기를 DB 실패로 답함",
                       id, attachLength, LINK_MAIL_ATTACH_BYTES);
            return one;
        }

        LinkAccMailFull& mail = one.mail;
        mail.state = static_cast<uint8_t>(state);
        mail.mailId = id;
        mail.claimCharacterId = claim;
        mail.money = money;
        mail.remainSec = ClampRemain(remain);
        CopyText(title, titleLength, mail.title, id, "제목");
        CopyText(body, bodyLength, mail.body, id, "본문");
        mail.attachLen = static_cast<uint8_t>(attachLength);
        std::memcpy(mail.attach, attach, attachLength);
        one.found = CAccountDb::Found::Yes;
        return one;
    }

    // 받는 중(character) 행 하나를 바꾼다 — 받음 · 풀기가 같은 꼴이다
    CAccountDb::MailWrite UpdateClaimed(CDbConnection& conn, const char* sql, int64_t account, int64_t mailId, int64_t character, const char* what)
    {
        CAccountDb::MailWrite write;
        if (!conn.IsOpen())
        {
            conn.NoteNotOpen();
            return write;
        }
        MYSQL_BIND binds[3] = { BindLong(mailId), BindLong(account), BindLong(character) };
        write.ok = ExecStmt(conn, sql, binds, what, &write.rows);
        return write;
    }
}

const char* CAccountDb::OpName(Op op)
{
    switch (op)
    {
    case OP_LOGIN:         return "login";
    case OP_MAIL_COUNT:    return "mail_count";
    case OP_MAIL_LIST:     return "mail_list";
    case OP_MAIL_READ:     return "mail_read";
    case OP_MAIL_CLAIM:    return "mail_claim";
    case OP_MAIL_DONE:     return "mail_done";
    case OP_MAIL_UNCLAIM:  return "mail_unclaim";
    case OP_MAIL_ERASE:    return "mail_erase";
    case OP_MAIL_SEND_ONE: return "mail_send_one";
    case OP_MAIL_SEND_ALL: return "mail_send_all";
    case OP_COUNT: break;
    }
    return "unknown";
}

CAccountDb::LoginRow CAccountDb::FindOrCreate(CDbConnection& conn, const std::string& loginName)
{
    LoginRow row;
    if (!conn.IsOpen())
    {
        conn.NoteNotOpen();
        return row;
    }

    int64_t id = 0;
    switch (SelectId(conn, loginName, id))
    {
    case SelectOutcome::Failed:
        return row;
    case SelectOutcome::Found:
        break;
    case SelectOutcome::Missing:
        switch (InsertAccount(conn, loginName, id))
        {
        case InsertOutcome::Failed:
            return row;
        case InsertOutcome::Inserted:
            row.created = true;
            break;
        case InsertOutcome::Duplicate:
            if (SelectId(conn, loginName, id) != SelectOutcome::Found)
                return row;
            break;
        }
        break;
    }

    if (!TouchLastLogin(conn, id))
    {
        row.created = false;   // 답이 DB_FAIL 이므로 다음 로그인이 이미 있는 행을 읽는다 — 새로 만든 것으로 세지 않는다
        return row;
    }

    row.ok = true;
    row.accountId = AccountId{ id };
    return row;
}

CAccountDb::MailCount CAccountDb::CountMail(CDbConnection& conn, int64_t account, int64_t character)
{
    MailCount result;
    if (!conn.IsOpen())
    {
        conn.NoteNotOpen();
        return result;
    }

    // 붉은 수 기준(11-11) — 글만 있는 우편은 읽으면 빠지고, 돈 · 첨부가 남은 우편은 읽어도 남는다
    MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(conn.Prepare(
        "SELECT CAST(COALESCE(SUM(state=0 AND expires_at>NOW(3) AND (read_at IS NULL OR money>0 OR attach<>'')), 0) AS SIGNED), "
        "COALESCE(MIN(IF(state=1 AND claim_character_id=?, mail_id, NULL)), 0) "
        "FROM account_mail WHERE account_id=? AND state IN (0,1)"));
    if (stmt == nullptr)
        return result;

    MYSQL_BIND in[2] = { BindLong(character), BindLong(account) };
    if (mysql_stmt_bind_param(stmt, in) != 0 || mysql_stmt_execute(stmt) != 0)
    {
        conn.NoteStmtError(stmt, "account_mail count");
        return result;
    }

    int64_t count = 0;
    int64_t claim = 0;
    MYSQL_BIND out[2] = { BindLong(count), BindLong(claim) };
    if (mysql_stmt_bind_result(stmt, out) != 0)
    {
        conn.NoteStmtError(stmt, "account_mail count bind");
        return result;
    }
    const int fetched = mysql_stmt_fetch(stmt);
    if (fetched != 0)   // 묶음 함수라 줄이 늘 하나다 — 없으면 실패다
    {
        conn.NoteStmtError(stmt, "account_mail count fetch");
        return result;
    }
    mysql_stmt_free_result(stmt);

    result.ok = true;
    result.count = static_cast<uint16_t>(std::clamp<int64_t>(count, 0, UINT16_MAX));
    result.claimMailId = claim;
    return result;
}

CAccountDb::MailList CAccountDb::ListMail(CDbConnection& conn, int64_t account, int64_t beforeMailId)
{
    MailList result;
    if (!conn.IsOpen())
    {
        conn.NoteNotOpen();
        return result;
    }

    // 한 줄 더 읽어 「더 있다」를 안다 — 장마다 요청한다(9절 「여러 장 답을 안 쓰고 장마다 요청한다」)
    MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(conn.Prepare(
        "SELECT mail_id, money, title, " ACC_MAIL_REMAIN ", state, read_at IS NOT NULL, COALESCE(claim_character_id, 0), attach "
        "FROM account_mail WHERE account_id=? AND " ACC_MAIL_ALIVE " AND (?=0 OR mail_id<?) "
        "ORDER BY mail_id DESC LIMIT 11"));
    static_assert(LINK_ACCMAIL_LIST_ROWS + 1 == 11, "LIMIT 은 한 장 줄 수 + 1");
    if (stmt == nullptr)
        return result;

    MYSQL_BIND in[3] = { BindLong(account), BindLong(beforeMailId), BindLong(beforeMailId) };
    if (mysql_stmt_bind_param(stmt, in) != 0 || mysql_stmt_execute(stmt) != 0)
    {
        conn.NoteStmtError(stmt, "account_mail list");
        return result;
    }

    int64_t id = 0;
    int64_t money = 0;
    char title[MAIL_TITLE_BYTES + 1]{};
    unsigned long titleLength = MAIL_TITLE_BYTES;
    int64_t remain = 0;
    int state = 0;
    int read = 0;
    int64_t claim = 0;
    uint8_t attach[MAIL_ATTACH_COLUMN]{};
    unsigned long attachLength = sizeof(attach);
    MYSQL_BIND out[8] = {
        BindLong(id), BindLong(money), BindText(MYSQL_TYPE_STRING, title, titleLength), BindLong(remain),
        BindInt(state), BindInt(read), BindLong(claim), BindText(MYSQL_TYPE_BLOB, attach, attachLength)
    };
    if (mysql_stmt_bind_result(stmt, out) != 0)
    {
        conn.NoteStmtError(stmt, "account_mail list bind");
        return result;
    }

    for (;;)
    {
        const int fetched = mysql_stmt_fetch(stmt);
        if (fetched == MYSQL_NO_DATA)
            break;
        if (fetched != 0)
        {
            conn.NoteStmtError(stmt, "account_mail list fetch");
            return result;
        }
        if (result.count >= LINK_ACCMAIL_LIST_ROWS)
        {
            result.more = true;   // 11번째 줄 — 담지 않는다. 남은 줄은 free_result 가 버린다
            break;
        }

        LinkAccMailLine& line = result.lines[result.count++];
        line.mailId = id;
        line.money = money;
        CopyText(title, titleLength, line.title, id, "제목");
        line.remainSec = ClampRemain(remain);
        line.state = static_cast<uint8_t>(state);
        line.read = read != 0 ? 1 : 0;
        line.claimCharacterId = claim;
        if (attachLength > static_cast<unsigned long>(LINK_MAIL_ATTACH_BYTES))
        {
            // 목록은 그림만 그린다 — 줄은 보이고 첨부 그림만 빠진다. 받기 · 읽기는 SelectOne 이 막는다
            SLOG_ERROR("[AccountDb] 계정 우편 {} 의 첨부가 {}B — 와이어 칸 {}B 를 넘음(손으로 고친 행). 목록에서 첨부를 뺌",
                       id, attachLength, LINK_MAIL_ATTACH_BYTES);
            line.attachLen = 0;
        }
        else
        {
            line.attachLen = static_cast<uint8_t>(attachLength);
            std::memcpy(line.attach, attach, attachLength);
        }
    }
    mysql_stmt_free_result(stmt);

    result.ok = true;
    return result;
}

CAccountDb::MailOne CAccountDb::ReadMail(CDbConnection& conn, int64_t account, int64_t mailId)
{
    MailOne one;
    if (!conn.IsOpen())
    {
        conn.NoteNotOpen();
        return one;
    }

    // 처음 읽은 때만 적는다 — 두 번 읽어도 처음 시각이 남는다(6절 「입장 때 다시 처리」 끝 문단)
    MYSQL_BIND binds[2] = { BindLong(mailId), BindLong(account) };
    if (!ExecStmt(conn,
            "UPDATE account_mail SET read_at=COALESCE(read_at, NOW(3)) WHERE mail_id=? AND account_id=? AND " ACC_MAIL_ALIVE,
            binds, "account_mail read"))
        return one;
    return SelectOne(conn, account, mailId);
}

CAccountDb::MailOne CAccountDb::ClaimMail(CDbConnection& conn, int64_t account, int64_t mailId, int64_t character)
{
    MailOne one;
    if (!conn.IsOpen())
    {
        conn.NoteNotOpen();
        return one;
    }

    // 0행이어도 실패가 아니다 — 이미 받는 중이면 다시 읽어 적혀 있는 캐릭터를 돌려준다(6절 받기 4번)
    MYSQL_BIND binds[3] = { BindLong(character), BindLong(mailId), BindLong(account) };
    if (!ExecStmt(conn,
            "UPDATE account_mail SET state=1, claim_character_id=?, claim_at=NOW(3) "
            "WHERE mail_id=? AND account_id=? AND state=0 AND expires_at>NOW(3)",
            binds, "account_mail claim"))
        return one;
    return SelectOne(conn, account, mailId);
}

CAccountDb::MailWrite CAccountDb::DoneMail(CDbConnection& conn, int64_t account, int64_t mailId, int64_t character)
{
    return UpdateClaimed(conn,
        "UPDATE account_mail SET state=2, done_at=NOW(3) WHERE mail_id=? AND account_id=? AND state=1 AND claim_character_id=?",
        account, mailId, character, "account_mail done");
}

CAccountDb::MailWrite CAccountDb::UnclaimMail(CDbConnection& conn, int64_t account, int64_t mailId, int64_t character)
{
    return UpdateClaimed(conn,
        "UPDATE account_mail SET state=0, claim_character_id=NULL, claim_at=NULL WHERE mail_id=? AND account_id=? AND state=1 AND claim_character_id=?",
        account, mailId, character, "account_mail unclaim");
}

CAccountDb::MailWrite CAccountDb::EraseMail(CDbConnection& conn, int64_t account, int64_t mailId)
{
    MailWrite write;
    if (!conn.IsOpen())
    {
        conn.NoteNotOpen();
        return write;
    }
    // 첨부도 돈도 없는 안 받은 우편만 — 캐릭터 우편의 지우기와 같은 규칙(2절 3번). 행은 남는다(3절)
    MYSQL_BIND binds[2] = { BindLong(mailId), BindLong(account) };
    write.ok = ExecStmt(conn,
        "UPDATE account_mail SET state=3, done_at=NOW(3) WHERE mail_id=? AND account_id=? AND state=0 AND money=0 AND attach=''",
        binds, "account_mail erase", &write.rows);
    return write;
}

CAccountDb::MailWrite CAccountDb::SendMail(CDbConnection& conn, const SendRequest& request)
{
    MailWrite write;
    if (!conn.IsOpen())
    {
        conn.NoteNotOpen();
        return write;
    }

    int64_t batch = request.batchId;
    int64_t sentBy = request.sentBy;
    int64_t money = request.money;
    int days = request.expireDays;
    int64_t target = request.targetAccount;
    unsigned long titleLength = static_cast<unsigned long>(request.title.size());
    unsigned long bodyLength = static_cast<unsigned long>(request.body.size());
    unsigned long attachLength = static_cast<unsigned long>(request.attach.size());
    MYSQL_BIND binds[8] = {
        BindLong(batch), BindLong(sentBy),
        BindText(MYSQL_TYPE_STRING, request.title.data(), titleLength),
        BindText(MYSQL_TYPE_STRING, request.body.data(), bodyLength),
        BindLong(money),
        BindText(MYSQL_TYPE_BLOB, request.attach.data(), attachLength),
        BindInt(days), BindLong(target)
    };

    // 한 문장 — 전부 들어가거나 전부 안 들어간다. 같은 batch_id 는 uq_batch 에 걸려 그 줄만 건너뛴다(바뀐 행 0 · 6절 보내기)
    //   CLIENT_FOUND_ROWS 를 안 켜서(DbConnection.cpp mysql_real_connect) 건너뛴 줄은 rows 에 안 든다
    if (request.targetAccount != 0)
    {
        write.ok = ExecStmt(conn,
            "INSERT INTO account_mail(account_id, batch_id, sent_by, title, body, money, attach, expires_at) "
            "SELECT account_id, NULLIF(?, 0), ?, ?, ?, ?, ?, DATE_ADD(NOW(3), INTERVAL ? DAY) FROM accounts WHERE account_id=? "
            "ON DUPLICATE KEY UPDATE batch_id=batch_id",
            binds, "account_mail send one", &write.rows);
    }
    else
    {
        write.ok = ExecStmt(conn,
            "INSERT INTO account_mail(account_id, batch_id, sent_by, title, body, money, attach, expires_at) "
            "SELECT account_id, ?, ?, ?, ?, ?, ?, DATE_ADD(NOW(3), INTERVAL ? DAY) FROM accounts "
            "ON DUPLICATE KEY UPDATE batch_id=batch_id",
            binds, "account_mail send all", &write.rows);   // binds[7](target) 은 안 쓴다 — 자리표 수만큼만 읽힌다
    }
    return write;
}
