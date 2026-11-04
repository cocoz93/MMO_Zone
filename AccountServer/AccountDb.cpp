#include "AccountDb.h"

#ifdef _WIN32
#include <winsock2.h>   // mysql.h 이전에 — 구버전 winsock.h 가 끌려오지 않게
#include <mysql.h>
#else
#include <mysql/mysql.h>
#endif

#include <cstdint>
#include <cstring>

namespace
{
    constexpr unsigned int ER_DUPLICATE_KEY = 1062;   // ER_DUP_ENTRY

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
}

const char* CAccountDb::OpName(Op op)
{
    switch (op)
    {
    case OP_LOGIN: return "login";
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
