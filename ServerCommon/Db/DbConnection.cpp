#include "DbConnection.h"

#ifdef _WIN32
#include <winsock2.h>   // mysql.h 이전에 — 구버전 winsock 자동 include 충돌 차단
#endif
#include <mysql.h>
#include <errmsg.h>     // CR_SERVER_GONE_ERROR / CR_SERVER_LOST (커넥션 유실 감지)

#include <cstring>

#include <Common/ErrorLog.h>

#pragma comment(lib, "libmysql.lib")

void* CDbConnection::OpenRaw(const DBConfig& config)
{
    MYSQL* m = mysql_init(nullptr);
    if (m == nullptr)
    {
        SLOG_ERROR("[DB] mysql_init failed");
        return nullptr;
    }

    unsigned int connTimeout = static_cast<unsigned int>(config.connectTimeoutSec);
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &connTimeout);
    // 읽기/쓰기 타임아웃 — DB가 응답 없이 멈추면 쿼리가 무한 대기하지 않고 실패 반환(재연결 판단 가능)
    unsigned int rwTimeout = static_cast<unsigned int>(config.rwTimeoutSec);
    mysql_options(m, MYSQL_OPT_READ_TIMEOUT, &rwTimeout);
    mysql_options(m, MYSQL_OPT_WRITE_TIMEOUT, &rwTimeout);
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (mysql_real_connect(m,
            config.host.c_str(), config.user.c_str(), config.password.c_str(),
            config.database.c_str(), static_cast<unsigned int>(config.port),
            nullptr, 0) == nullptr)
    {
        SLOG_ERROR("[DB] connect failed: {}", mysql_error(m));
        mysql_close(m);
        return nullptr;
    }

    return m;
}

bool CDbConnection::IsConnectionLostErrno(unsigned int err)
{
    return err == CR_SERVER_GONE_ERROR || err == CR_SERVER_LOST;
}

bool CDbConnection::Open()
{
    Close();
    _mysql = OpenRaw(_config);
    return _mysql != nullptr;
}

void CDbConnection::CloseStatements()
{
    for (auto& entry : _statements)
        mysql_stmt_close(static_cast<MYSQL_STMT*>(entry.second));
    _statements.clear();
}

void CDbConnection::Close()
{
    CloseStatements();
    if (_mysql != nullptr)
    {
        mysql_close(static_cast<MYSQL*>(_mysql));
        _mysql = nullptr;
    }
}

void* CDbConnection::Prepare(const char* sql)
{
    if (_mysql == nullptr)
    {
        NoteNotOpen();
        return nullptr;
    }

    auto found = _statements.find(sql);
    if (found != _statements.end())
        return found->second;

    MYSQL* m = static_cast<MYSQL*>(_mysql);
    MYSQL_STMT* st = mysql_stmt_init(m);
    if (st == nullptr)
    {
        SLOG_ERROR("[DB] mysql_stmt_init failed");
        _jobFailed = true;
        return nullptr;
    }
    if (mysql_stmt_prepare(st, sql, static_cast<unsigned long>(std::strlen(sql))) != 0)
    {
        const unsigned int err = mysql_stmt_errno(st);
        SLOG_ERROR("[DB] prepare failed (errno={}): {}", err, mysql_stmt_error(st));
        mysql_stmt_close(st);
        _jobFailed = true;
        if (IsConnectionLostErrno(err))
            Close();
        return nullptr;
    }

    _statements.emplace(sql, st);
    return st;
}

bool CDbConnection::Exec(const char* sql)
{
    if (_mysql == nullptr)
    {
        NoteNotOpen();
        return false;
    }
    MYSQL* m = static_cast<MYSQL*>(_mysql);
    if (mysql_query(m, sql) != 0)
    {
        const unsigned int err = mysql_errno(m);
        SLOG_ERROR("[DB] {} failed (errno={}): {}", sql, err, mysql_error(m));
        _jobFailed = true;
        if (IsConnectionLostErrno(err))
            Close();
        return false;
    }
    return true;
}

unsigned int CDbConnection::NoteStmtError(void* stmt, const char* what)
{
    MYSQL_STMT* st = static_cast<MYSQL_STMT*>(stmt);
    const unsigned int err = mysql_stmt_errno(st);
    _jobFailed = true;
    if (IsConnectionLostErrno(err))
    {
        SLOG_WARN("[DB] connection lost during {} (errno={})", what, err);
        Close();
    }
    else
    {
        SLOG_ERROR("[DB] {} failed (errno={}): {}", what, err, mysql_stmt_error(st));
    }
    return err;
}
