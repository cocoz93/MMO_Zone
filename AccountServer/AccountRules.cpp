#include "AccountRules.h"

bool CAccountRules::IsValidLoginName(const char (&name)[ACCOUNT_LOGIN_NAME_BYTES])
{
    int length = 0;
    while (length < ACCOUNT_LOGIN_NAME_BYTES && name[length] != '\0')
    {
        const char c = name[length];
        const bool digit = (c >= '0' && c <= '9');
        const bool lower = (c >= 'a' && c <= 'z');
        const bool upper = (c >= 'A' && c <= 'Z');
        if (!digit && !lower && !upper)
            return false;
        ++length;
    }
    // 칸을 다 채우고도 NUL 이 없으면 17자 이상이다 — 아래 상한 검사에 같이 걸린다
    return length >= ACCOUNT_LOGIN_NAME_MIN && length <= ACCOUNT_LOGIN_NAME_MAX;
}

std::string CAccountRules::LowerName(const std::string& name)
{
    std::string lower = name;
    for (char& c : lower)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return lower;
}

const char* CAccountRules::LoginResultName(AccountLoginResult result)
{
    switch (result)
    {
    case AccountLoginResult::OK:             return "ok";
    case AccountLoginResult::BAD_FORM:       return "bad_form";
    case AccountLoginResult::LOGIN_DISABLED: return "login_disabled";
    case AccountLoginResult::BUSY:           return "busy";
    case AccountLoginResult::DB_FAIL:        return "db_fail";
    case AccountLoginResult::COUNT:          break;
    }
    return "unknown";
}

const char* CAccountRules::TokenResultName(TokenCheckResult result)
{
    switch (result)
    {
    case TokenCheckResult::Ok:       return "ok";
    case TokenCheckResult::BadSize:  return "bad_size";
    case TokenCheckResult::NotFound: return "not_found";
    case TokenCheckResult::Expired:  return "expired";
    }
    return "unknown";
}

const char* CAccountRules::MailOpName(AccMailOp op)
{
    switch (op)
    {
    case AccMailOp::Count:   return "count";
    case AccMailOp::List:    return "list";
    case AccMailOp::Read:    return "read";
    case AccMailOp::Claim:   return "claim";
    case AccMailOp::Done:    return "done";
    case AccMailOp::Unclaim: return "unclaim";
    case AccMailOp::Erase:   return "erase";
    case AccMailOp::Send:    return "send";
    }
    return "unknown";
}

const char* CAccountRules::MailResultName(LinkMailResult result)
{
    switch (result)
    {
    case LinkMailResult::OK:            return "ok";
    case LinkMailResult::GONE:          return "gone";
    case LinkMailResult::BUSY:          return "busy";
    case LinkMailResult::DB_ERROR:      return "db_error";
    case LinkMailResult::NO_ACCOUNT:    return "no_account";
    case LinkMailResult::CANNOT_DELETE: return "cannot_delete";
    }
    return "unknown";
}
