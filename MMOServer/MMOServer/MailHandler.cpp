#include "MailHandler.h"

#if USE_MAIL

#include "Player.h"
#include <Common/ErrorLog.h>

// 2단계 — 우편 잡을 내는 자리도, 우편 문장을 만드는 자리도 아직 없다. 오면 어딘가 이른 호출이 생긴 것이다
void CMailHandler::OnMailDbReply(const DBMailReply& reply)
{
    SLOG_ERROR("[MAIL] unexpected mail db reply before mail rules (op={}, character={}, ok={})",
               static_cast<int>(reply.op), reply.characterId, reply.ok);
}

void CMailHandler::OnMailSaved(CPlayer* player, const DBMailStmt& stmt)
{
    SLOG_ERROR("[MAIL] unexpected mail saved before mail rules (kind={}, character={}, live={})",
               static_cast<int>(stmt.kind), stmt.characterId, player != nullptr);
}

#endif // USE_MAIL
