#include "AccountMetrics.h"

#include <Monitoring/MetricsText.h>

#include "AccountRules.h"

void CAccountMetrics::Write(std::ostringstream& ss) const
{
    MetricsText::WriteHead(ss, "mmo_account_login_total", "Account login replies by result", "counter");
    for (int i = 0; i < LOGIN_RESULTS; ++i)
    {
        ss << "mmo_account_login_total{result=\"" << CAccountRules::LoginResultName(static_cast<AccountLoginResult>(i)) << "\"} "
           << _login[i].load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    MetricsText::WriteCounter(ss, "mmo_account_created_total", "Accounts created on first login", _created.load(std::memory_order_relaxed));
    MetricsText::WriteCounter(ss, "mmo_auth_token_issued_total", "Login tokens issued", _issued.load(std::memory_order_relaxed));

    MetricsText::WriteHead(ss, "mmo_auth_token_total", "Login token checks by result", "counter");
    for (int i = 0; i < TOKEN_CHECK_RESULT_COUNT; ++i)
    {
        ss << "mmo_auth_token_total{result=\"" << CAccountRules::TokenResultName(static_cast<TokenCheckResult>(i)) << "\"} "
           << _token[i].load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    MetricsText::WriteGauge(ss, "mmo_auth_token_pending", "Unexpired login tokens not yet used", _pending.load(std::memory_order_relaxed));
}
