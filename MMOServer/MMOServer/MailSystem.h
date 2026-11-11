// ==========================================================================
// CMailSystem — 우편 규칙: 판정과 사람별 우편 상태 전이 (mail-plan.md 4절 · 5절 · 7절 · USE_MAIL)
//
// [책임]
//  - 사전 검사 셋 — 목록·읽기·지우기(CheckReadDoor) · 보내기·받기(CheckItemDoor) · 왕복 뒤 다시(CheckAgain)
//  - 보내기 — 형식과 칸·돈을 보고 초안을 만든다(BeginSend · 아무것도 안 줄인다) → 찾기 회신으로 다시 판정하고
//    가방에서 빼고 문장을 대기로 올린다(FinishSend)
//  - 받기 — 복사본에 넣어 보고(CheckTake) 가방에 넣고 문장을 대기로 올린다(ApplyTake · FinishTake)
//  - 사람별 우편 상태(CPlayer::_mailOp)의 전이 — 여는 곳(Begin*) · 닫는 곳(End · Confirm)이 전부 여기다
//  - 성사 로그 [MAIL] 한 줄 · 돈 지표 · 성사 수
//
// [모르는 것]
//  패킷 · DB 워커 · 저장소 · 서버 간 연결. 값(MailResult)만 돌려준다 — 부르는 쪽은 CMailHandler 하나다.
//  거래와 달리 먼저 움직일 일(틱 · 시한)이 없어 알림 통로가 없다(mail-plan.md 4절 「거래와 다른 점 하나」).
//
// [가방을 바꾸는 곳은 둘] FinishSend(빼기) · ApplyTake(넣기). 둘 다 같은 함수 안에서 문장을 대기(CPlayer::_mailDraft, ready)로 올리고
//  로그를 남긴다 — 그래서 플레이어가 곧바로 나가도 문장이 떠난 기록으로 실리고(CCharacterStore::LeaveFinal) 로그도 안 빠진다.
//  칸·돈 패킷과 저장 요청은 부르는 쪽(핸들러)이 FlushInventory · RequestSave 로 낸다.
// ==========================================================================
#pragma once

#include "BuildConfig.h"   // USE_MAIL — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_MAIL

#include <cstdint>
#include <Protocol/Protocol.h>   // MailResult · MSG_C2S_MAIL_SEND
#include "MapTable.h"            // MapKind — 우편이 되는 맵 종류
#include "MailAttach.h"          // MailAttach::Items
#include "DB/DBWorker.h"         // DBMailReply · DBMailRow · DBMailOp · DBMailStmtKind

class CPlayer;
class CGameDataFiles;
class CGameMonitor;
#if USE_TRADE
class CTradeSystem;
#endif

// 우편이 되는 맵 종류 — 맵 종류마다 한 줄(거래의 TradeAllowedOn 과 같은 규칙 · 파티 방은 안 됨)
bool MailAllowedOn(MapKind kind);

// 운영 값 — ServerConfig [Mail]. 기본값은 사용자 결정(mail-plan.md 1절 · 11-05) 그대로다
struct MailRules
{
    int64_t sendFee    = 100;   // 보내기 수수료(전) — 첨부·돈과 상관없이 한 통에 한 번
    int32_t expireDays = 30;    // 기한(일) — expires_at 은 DB 시각으로 센다(DATE_ADD(NOW(3), …))
    int32_t maxUnread  = 50;    // 받는 사람의 살아 있는 우편 수가 이 이상이면 TARGET_FULL — DB 제약이 아니라 코드 검사다
};

class CMailSystem
{
public:
    CMailSystem(const CGameDataFiles& tables,
#if USE_TRADE
                const CTradeSystem& tradeSystem,
#endif
                CGameMonitor& gameMonitor);

    CMailSystem(const CMailSystem&) = delete;
    CMailSystem& operator=(const CMailSystem&) = delete;

    void Configure(const MailRules& rules) { _rules = rules; }   // main 이 Start 전에 — 게임 서버의 ConfigureMail
    const MailRules& Rules() const { return _rules; }

    // ── 사전 검사 — 먼저 걸린 하나를 돌려준다(mail-plan.md 5절 「입력 검사」 1번 줄) ──
    MailResult CheckReadDoor(const CPlayer* p) const;   // NO_CHARACTER → NOT_HERE → BUSY(진행 중 동작)
    MailResult CheckItemDoor(const CPlayer* p) const;   // 위 + DEAD → BUSY(거래) → BUSY(짝 고리)
    MailResult CheckAgain(const CPlayer* p) const;      // 왕복 뒤 — NOT_HERE → DEAD. 거래는 안 본다(왕복 사이엔 _mailOp 때문에 거래가 못 끼어든다)

    // ── 왕복 동작 열기 — 검사가 통과하면 상태를 그 동작으로 둔다. OK 가 아니면 아무것도 안 바뀌었다 ──
    MailResult BeginList(CPlayer* p);    // CheckReadDoor → LIST
    MailResult BeginRead(CPlayer* p);    // CheckReadDoor → READ
    MailResult BeginErase(CPlayer* p);   // CheckReadDoor → ERASE
    MailResult BeginTake(CPlayer* p);    // CheckItemDoor → TAKE_READ

    // 보내기 1 — 입력 검사 표(5절) 차례대로 보고 초안(ready=false)을 만든다 → FIND. 아무것도 안 줄인다
    MailResult BeginSend(CPlayer* p, const MSG_C2S_MAIL_SEND& form);
    // 보내기 2 — 찾기 회신으로 다시 판정(5절 「찾기 회신 뒤」) → 칸·돈을 줄이고 문장을 대기로 → SEND_SAVE.
    //   거절이면 초안을 버리고 IDLE. 줄인 칸은 가방의 바뀐 칸 표시에 켜져 있다(부르는 쪽이 FlushInventory)
    MailResult FinishSend(CPlayer* p, const DBMailReply& found);

    // 받기 판정 — 복사본에 넣어 본다. 아무것도 안 바꾼다(계정 우편의 「받는 중」 걸기 전에도 쓴다 — 8단계)
    MailResult CheckTake(const CPlayer* p, int64_t money, const MailAttach::Items& attach) const;
    // 받기 — READ_ONE 회신으로 CheckAgain → DB_ERROR → GONE → ApplyTake. 거절이면 IDLE
    MailResult FinishTake(CPlayer* p, const DBMailReply& reply);
    // 받기의 몸 — NO_ATTACH → ITEM_UNKNOWN → BAG_FULL·MONEY_FULL → 가방에 넣고 TAKE_UPDATE 를 대기로 → TAKE_SAVE.
    //   거절이면 아무것도 안 바뀐다(상태도 그대로 — 부르는 쪽이 닫는다)
    MailResult ApplyTake(CPlayer* p, const DBMailRow& row);

    // ── 닫기 ──
    // 왕복 동작을 IDLE 로 — 회신을 다 처리했을 때 · 잡을 못 넣었을 때. 찾기 중 초안(ready=false)은 버린다.
    //   *_SAVE 에서는 부르지 않는다(가방이 이미 바뀌었다 — 확정 회신(Confirm)만 푼다)
    void End(CPlayer* p);
    // 이 회신이 지금 상태가 기다리던 것인가 — 아니면 있을 수 없는 일이라 부르는 쪽이 오류 로그를 남기고 버린다(7절 「끊김」)
    bool Waiting(const CPlayer* p, DBMailOp op) const;
    // 우편 문장이 든 묶음이 확정됐다 — 상태가 그 문장을 기다리던 *_SAVE 면 IDLE 로 두고 true
    bool Confirm(CPlayer* p, DBMailStmtKind kind);

private:
    const CGameDataFiles& _tables;
#if USE_TRADE
    const CTradeSystem&   _tradeSystem;   // 거래 신청 · 거래창(Busy) — 빌린 것, 읽기만
#endif
    CGameMonitor&         _gameMonitor;   // 성사 수 · 돈 지표
    MailRules             _rules;
};

#endif // USE_MAIL
