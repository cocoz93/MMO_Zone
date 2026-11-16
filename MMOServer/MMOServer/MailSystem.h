// ==========================================================================
// CMailSystem — 우편 규칙: 판정과 사람별 우편 상태 전이 (mail-plan.md 4절 · 5절 · 7절 · USE_MAIL)
//
// [책임]
//  - 사전 검사 셋 — 목록·읽기·지우기(CheckReadDoor) · 보내기·받기(CheckItemDoor) · 왕복 뒤 다시(CheckAgain)
//  - 보내기 — 형식과 칸·돈을 보고 초안을 만든다(BeginSend · 아무것도 안 줄인다) → 찾기 회신으로 다시 판정하고
//    가방에서 빼고 문장을 대기로 올린다(FinishSend)
//  - 받기 — 복사본에 넣어 보고(CheckTake) 가방에 넣고 문장을 대기로 올린다(ApplyTake · FinishTake)
//  - 계정 우편 받기 — 서버 간 답과 DB 회신마다 다음 걸음을 정한다(AfterAccRead · AfterAccMoved · AfterAccClaim · mail-plan.md 6절)
//  - 운영자 지급(/accmail) — 낱말을 읽어 검사하고(ParseAdminMail) 전 계정 일괄 번호를 만든다(MakeBatchId)
//  - 사람별 우편 상태(CPlayer::_mailOp)의 전이 — 여는 곳(Begin*) · 닫는 곳(End · Confirm)이 전부 여기다
//  - 성사 로그 [MAIL] 한 줄 · 돈 지표 · 성사 수
//
// [모르는 것]
//  패킷 · DB 워커 · 저장소 · 서버 간 연결. 값(MailResult · AccTakeStep)만 돌려준다 — 부르는 쪽은 CMailHandler 하나다.
//  서버 간 답(LinkAccMailFull)은 읽기만 한다 — 시한 · 끊김 · 연결 없음은 핸들러가 ACC_TIMEOUT · ACC_OFFLINE 으로 닫는다.
//  거래와 달리 먼저 움직일 일(틱 · 시한)이 없어 알림 통로가 없다(mail-plan.md 4절 「거래와 다른 점 하나」).
//
// [가방을 바꾸는 곳은 셋] FinishSend(빼기) · ApplyTake(캐릭터 우편 넣기) · AfterAccClaim(계정 우편 넣기). 셋 다 같은 함수 안에서 문장을 대기(CPlayer::_mailDraft, ready)로 올리고
//  로그를 남긴다 — 그래서 플레이어가 곧바로 나가도 문장이 떠난 기록으로 실리고(CCharacterStore::LeaveFinal) 로그도 안 빠진다.
//  칸·돈 패킷과 저장 요청은 부르는 쪽(핸들러)이 FlushInventory · RequestSave 로 낸다.
// ==========================================================================
#pragma once

#include "BuildConfig.h"   // USE_MAIL — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_MAIL

#include <cstdint>
#include <string>
#include <vector>
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
struct LinkAccMailFull;   // ServerLinkProtocol.h — 계정 우편 한 통(읽기 · 받는 중 표시의 답). .cpp 만 연다(소켓 헤더 차례를 이 헤더의 소비자에게 지우지 않게)

// 우편이 되는 맵 종류 — 맵 종류마다 한 줄(거래의 TradeAllowedOn 과 같은 규칙 · 파티 방은 안 됨)
bool MailAllowedOn(MapKind kind);

// 계정 우편 받기의 다음 걸음 — 규칙이 정하고 핸들러가 실행한다(mail-plan.md 6절 받기 1~7번 · 7절 ACC_* 줄)
enum class AccTakeStep : uint8_t
{
    CLOSE,           // 끝 — result 로 알린다(상태는 IDLE 로 닫혔다)
    DONE_CLOSE,      // 이미 옮겼다 — DONE 만 다시 보내고 OK 로 끝(가방에 다시 안 넣는다 · IDLE)
    UNCLAIM_CLOSE,   // 다시 판정이 거절했다 — 내 받는 중 표시를 풀고(UNCLAIM) result 로 끝(IDLE)
    CLAIM,           // 받는 중 표시를 건다(CLAIM_REQ) → ACC_CLAIM
    CHECK_MOVED,     // 「이미 옮겼나」(FIND_MOVED 잡) → ACC_CHECK_MOVED
    OTHER_NAME,      // 받는 중인 다른 캐릭터의 이름(FIND_NAME 잡) → ACC_OTHER_NAME
    SAVE,            // 가방에 넣고 ACC_TAKE_INSERT 를 대기로 올렸다 → ACC_TAKE_SAVE(핸들러가 저장 요청 · 칸 내보내기)
};
struct AccTakeDecision
{
    AccTakeStep step;
    MailResult  result;              // CLOSE · UNCLAIM_CLOSE 의 사유(나머지는 OK)
    int64_t     otherCharacterId;    // OTHER_NAME — 이름을 읽을 캐릭터
};

// 운영자 지급(/accmail)의 내용 — 채팅 낱말에서 읽고 검사를 지난 것(ParseAdminMail · mail-plan.md 6절 보내기)
struct AdminMailForm
{
    bool              all = false;            // 전 계정 — targetAccount 0 · 일괄 번호를 단다(CMailHandler 가 30초 들고 있다)
    int64_t           targetAccount = 0;      // 한 계정이면 그 번호(1 이상)
    int64_t           money = 0;              // 0 ~ MONEY_MAX
    MailAttach::Items items;                  // 0 ~ MAIL_MAX_ITEMS 칸 — 표에 있는 아이템(돈 아이템 아님) · 개수 1 ~ maxStack · 같은 아이템 한 번
    char16_t          title[MAIL_TITLE_MAX]{};   // 1 ~ 20자 · '_' 를 공백으로 바꾼 뒤
    char16_t          body[MAIL_BODY_MAX]{};     // 0 ~ 200자 · 제목 뒤 낱말을 공백 하나로 이은 것
};

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

    // ── 계정 우편 (mail-plan.md 6절 · 7절 ACC_*) — 서버 간 답의 null(시한 · 끊김)과 결과 옮기기는 핸들러가 먼저 본다 ──
    MailResult BeginAccList(CPlayer* p);     // CheckReadDoor → ACC_LIST
    MailResult BeginAccRead(CPlayer* p);     // CheckReadDoor → ACC_READ
    MailResult BeginAccErase(CPlayer* p);    // CheckReadDoor → ACC_ERASE
    MailResult BeginAccTake(CPlayer* p);     // CheckItemDoor → ACC_TAKE_READ
    // 입장 때 이어서 처리 — CheckItemDoor → ACC_CHECK_MOVED · _mailResume. 막히면 아무것도 안 바뀐다(다음 입장 · 목록의 받는 중 줄로 이어진다)
    MailResult BeginAccResume(CPlayer* p);
    // 받기 1 — 읽기 답(ACC_TAKE_READ): CheckAgain → 안 받음이면 판정(NO_ATTACH → ITEM_UNKNOWN → BAG_FULL·MONEY_FULL) → CLAIM ·
    //   받는 중 + 나 → CHECK_MOVED · 받는 중 + 남 → OTHER_NAME · 그 밖 → GONE
    AccTakeDecision AfterAccRead(CPlayer* p, const LinkAccMailFull& mail);
    // 받기 2 — 「이미 옮겼나」 회신(ACC_CHECK_MOVED): 못 읽음 → DB_ERROR · 줄 있음 → DONE_CLOSE · 줄 없음 → CLAIM
    AccTakeDecision AfterAccMoved(CPlayer* p, const DBMailReply& moved);
    // 받기 3 — 받는 중 표시 답(ACC_CLAIM): 받는 중이 아님 → GONE · 남이 받는 중 → OTHER_NAME ·
    //   CheckAgain · 판정이 거절 → UNCLAIM_CLOSE · 통과 → 같은 함수에서 가방에 넣고 ACC_TAKE_INSERT 를 대기로 → SAVE
    AccTakeDecision AfterAccClaim(CPlayer* p, const LinkAccMailFull& mail);

    // ── 닫기 ──
    // 왕복 동작을 IDLE 로 — 회신을 다 처리했을 때 · 잡을 못 넣었을 때. 찾기 중 초안(ready=false)은 버린다. _mailResume 도 끈다.
    //   *_SAVE 에서는 부르지 않는다(가방이 이미 바뀌었다 — 확정 회신(Confirm)만 푼다)
    void End(CPlayer* p);
    // 이 회신이 지금 상태가 기다리던 것인가 — 아니면 있을 수 없는 일이라 부르는 쪽이 오류 로그를 남기고 버린다(7절 「끊김」)
    bool Waiting(const CPlayer* p, DBMailOp op) const;
    // 우편 문장이 든 묶음이 확정됐다 — 상태가 그 문장을 기다리던 *_SAVE 면 IDLE 로 두고(_mailResume 도 끈다) true
    bool Confirm(CPlayer* p, DBMailStmtKind kind);

    // ── 운영자 지급 (/accmail · mail-plan.md 6절 보내기) — 사람별 우편 상태를 안 쓴다(운영자의 가방이 안 바뀐다) ──
    // words = 채팅 낱말(0번이 "/accmail"). 「/accmail <계정번호|all> <돈> <아이템:개수,…|-> <제목> <본문…>」 을 읽어
    //   계정 번호 → 돈 → 아이템(8칸 → 꼴 · 표 · 돈 아이템 아님 · 개수 1 ~ maxStack · 같은 아이템) → 제목 → 본문 차례로 본다.
    //   통과하면 빈 문자열 · 아니면 운영자에게 보일 거절 한 줄(UTF-8). 첨부와 돈이 둘 다 없어도 된다(글만 있는 공지 · 2절 8번).
    //   아이템 검사가 여기 있는 까닭 — 아이템 표가 존 서버에만 있다(6절 「단, 아이템 검사는 존 서버에 있다」)
    std::string ParseAdminMail(const std::vector<std::string>& words, AdminMailForm& out) const;
    // 전 계정 일괄 번호 — 63비트 난수(1 이상). false = 난수를 못 얻음(그 값을 쓰면 안 된다)
    static bool MakeBatchId(int64_t& out);

private:
    // 받기 공통 판정 — NO_ATTACH → ITEM_UNKNOWN(바이트 깨짐 · 돈 음수 · 표에 없음) → BAG_FULL·MONEY_FULL. 아무것도 안 바꾼다.
    //   what · idName 은 오류 로그의 낱말(캐릭터 우편 = "take" · "mail" / 계정 우편 = "acc take" · "account_mail")
    MailResult JudgeAttach(const CPlayer* p, int64_t money, const uint8_t* bytes, int len, int64_t mailId,
                           const char* what, const char* idName, MailAttach::Items& attach) const;
    // 판정이 통과한 첨부 · 돈을 원본 가방에 넣는다(복사본이 됐으니 원본도 된다 · 같은 게임 스레드라 그 사이에 안 바뀐다)
    void PutIntoBag(CPlayer* p, int64_t money, const MailAttach::Items& attach);

    const CGameDataFiles& _tables;
#if USE_TRADE
    const CTradeSystem&   _tradeSystem;   // 거래 신청 · 거래창(Busy) — 빌린 것, 읽기만
#endif
    CGameMonitor&         _gameMonitor;   // 성사 수 · 돈 지표
    MailRules             _rules;
};

#endif // USE_MAIL
