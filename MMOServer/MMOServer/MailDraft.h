#pragma once
// MailDraft — 우편 대기 문장(mail-plan.md 5절 「저장 묶음에서 바뀌는 것」)
//
// [무엇] 칸·돈을 줄인 뒤 다음 저장 묶음에 실을 우편 문장 하나와, 그 문장이 줄인 칸 목록.
//        CPlayer::_mailDraft(대기) → CPlayer::_dbFlight.mail(비행) → 확정 알림(IMailStoreEvents::OnMailSaved)으로 옮겨 다닌다.
// [누가] 채우기 = 우편 규칙(CMailSystem — 3단계) · 옮기기 = 저장소(CCharacterStore) · 읽기 = 우편 핸들러(확정 뒤 안내).
// [ready] false = 받는 이름 찾기 왕복 중인 초안 — 아직 아무것도 안 줄였으니 묶음에 안 실리고, 나가면 버린다.
//         true  = 칸·돈을 줄였다 — 다음 묶음에 꼭 실려야 한다(판정 ShouldSave 가 참이 된다).
#include "BuildConfig.h"

#if USE_MAIL

#include <cstdint>
#include <Identity/CharacterName.h>   // CHAR_NAME_MAX_LEN
#include <Protocol/Protocol.h>        // MAIL_MAX_ITEMS · MAIL_TITLE_MAX · MAIL_BODY_MAX
#include "DB/DBWorker.h"              // DBMailStmt · DB_MAIL_*_BYTES
#include "MailAttach.h"               // MailAttach::MAX_BYTES

// DB 칸 크기와 와이어 글자 수가 같은 값에서 나왔나 — 한쪽만 바꾸면 여기서 막힌다(DBWorker.h 는 Protocol.h 를 안 읽는다)
static_assert(DB_MAIL_TITLE_BYTES  == static_cast<size_t>(MAIL_TITLE_MAX) * 4, "제목 글자 수를 바꿨으면 DBWorker.h DB_MAIL_TITLE_BYTES 와 schema.sql 을 같이 고칠 것");
static_assert(DB_MAIL_BODY_BYTES   == static_cast<size_t>(MAIL_BODY_MAX) * 4,  "본문 글자 수를 바꿨으면 DBWorker.h DB_MAIL_BODY_BYTES 와 schema.sql 을 같이 고칠 것");
static_assert(DB_MAIL_ATTACH_BYTES == static_cast<size_t>(MailAttach::MAX_BYTES), "첨부 바이트 상한을 바꿨으면 DBWorker.h DB_MAIL_ATTACH_BYTES 를 같이 고칠 것");
static_assert(DB_MAIL_NAME_BYTES   == static_cast<size_t>(CHAR_NAME_MAX_LEN) * 4, "보낸 이름 칸은 캐릭터 이름 칸과 같다(schema.sql from_name)");

// 문장이 줄인 칸 하나 — 확정 뒤 안내 · 실패 로그에 쓴다(칸 자체는 가방이 이미 줄였다 · 저장 표시도 가방이 켰다)
struct MailDraftItem
{
    int32_t index;    // 가방 칸 색인
    int32_t itemId;
    int32_t count;
};

struct MailDraft
{
    DBMailStmt    stmt{};
    bool          ready = false;
    uint8_t       itemCount = 0;                 // items 중 쓰는 줄 수(0 ~ MAIL_MAX_ITEMS)
    MailDraftItem items[MAIL_MAX_ITEMS] = {};
};

#endif // USE_MAIL
