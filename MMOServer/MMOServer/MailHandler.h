// ==========================================================================
// CMailHandler — 우편: 우편 요청과 우편 회신 (mail-plan.md · USE_MAIL)
//
// [책임]
//  - 저장소의 우편 회신 받기(IMailStoreEvents — 우편 잡 회신 · 우편 문장 확정)
//  - (3단계부터) 받은 우편 패킷의 처리기와 그 등록 · 우편 규칙(CMailSystem) 부르기 · 결과 패킷
//
// [경계]
//  DB 를 직접 부르지 않는다 — 잡은 저장소(CCharacterStore::RequestMailJob)에, 칸·돈과 함께 쓸 문장은 대기 문장(CPlayer::_mailDraft)에 둔다.
//
// [지금 — 2단계]
//  빈 껍데기다. 패킷 등록이 없고 우편 잡도 안 내므로 두 알림은 올 일이 없다 — 오면 ERROR 로그로 드러낸다.
// ==========================================================================
#pragma once

#include "BuildConfig.h"     // USE_MAIL — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_MAIL

#include "CharacterStore.h"  // IMailStoreEvents · DBMailReply · DBMailStmt

class CMailHandler final : public IMailStoreEvents
{
public:
    CMailHandler() = default;

    CMailHandler(const CMailHandler&) = delete;
    CMailHandler& operator=(const CMailHandler&) = delete;

    // ── 저장소의 우편 회신 (IMailStoreEvents) — 회신 꺼내기(CCharacterStore::ProcessDbResults)와 저장 회신이 부른다 ──
    void OnMailDbReply(const DBMailReply& reply) override;
    void OnMailSaved(CPlayer* player, const DBMailStmt& stmt) override;
};

#endif // USE_MAIL
