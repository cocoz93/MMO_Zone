// ==========================================================================
// CMailHandler — 우편: 우편 요청과 우편 회신 (mail-plan.md · USE_MAIL)
//
// [책임]
//  - 받은 패킷 여섯의 처리기와 그 등록(RegisterRoutes — 목록 · 읽기 · 보내기 · 받기 · 계정 우편 받기 · 지우기)
//  - 우편 규칙(CMailSystem)을 불러 판정을 받고, 통과하면 DB 잡을 낸다(CCharacterStore::RequestMailJob)
//  - 저장소의 우편 회신 받기(IMailStoreEvents — 우편 잡 회신 · 우편 문장 확정) → 사람을 다시 찾아 절차를 잇는다
//  - 가방이 바뀐 뒤 — 저장 요청(RequestSave(MAIL)) · 칸·돈 내보내기(CItemHandler::FlushInventory)
//  - 결과 패킷 넷(S2C_MAIL_LIST · BODY · RESULT · COUNT) 만들기와 보낸 수 · 거절 수 세기
//  - 입장 때 수(OnEntered — 탭마다 S2C_MAIL_COUNT 한 장) · 계정 우편의 「받는 중」 행 이어서 처리(mail-plan.md 6절 「입장 때 다시 처리」)
//  - 계정 우편 탭 — 월드 서버를 거쳐 계정 서버에 묻는다(CAccountMailClient). 목록 · 읽기 · 받기 · 지우기
//  - 운영자 지급(/accmail — CCommandHandler 가 넘긴다) — 한 계정은 바로 · 전 계정은 일괄 번호를 30초 들고 confirm 을 기다린다 · 답을 운영자에게 한 줄로
//
// [경계]
//  DB 를 직접 부르지 않는다 — 잡은 저장소에, 칸·돈과 함께 쓸 문장은 대기 문장(CPlayer::_mailDraft)에 둔다(규칙이 올린다).
//  규칙의 내용(무엇이 되고 안 되나 · 상태 전이 · 계정 우편 받기의 다음 걸음)은 CMailSystem 몫이다 — 여기는 그 값을 패킷 · 잡 · 서버 간 요청으로 바꾼다.
//
// [계정 우편의 답]
//  서버 간 답은 세션 번호 · 캐릭터 번호를 들고 와 사람을 다시 찾는다 — 나갔으면 버린다(계정 서버에 「받는 중」이 남으면 입장 때 이어진다).
//  답 없음(시한 3초 · 월드 서버 끊김) = ACC_TIMEOUT · 못 보냄(연결 없음 · UseServerLink=0) = ACC_OFFLINE · 서버 간 결과는 FromLink 가 옮긴다.
//  월드 서버 연결이 없으면 입장 때 계정 우편 탭의 수는 known=UNKNOWN(못 읽음) — 웹이 0 을 「없음」으로 읽지 않게(9절 S2C_MAIL_COUNT).
// ==========================================================================
#pragma once

#include "BuildConfig.h"     // USE_MAIL — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_MAIL

#include <cstdint>
#include <string>
#include <vector>
#include <Protocol/Protocol.h>   // MailAction · MailTab · MailResult · MailCountKind
#include "CharacterStore.h"  // IMailStoreEvents · DBMailReply · DBMailStmt · DBMailJob
#include "MailSystem.h"      // AdminMailForm — 전 계정 지급 대기가 값으로 든다

class CPlayer;
class CSerialBuffer;
class CPacketRoutes;
class CSessionHub;
class CSelfNotifier;
class CItemHandler;
class CGameMonitor;
class CAccountMailClient;
struct LINK_ACCMAIL_COUNT_ACK;   // ServerLinkProtocol.h — 계정 우편 서버 간 답(.cpp 만 연다)
struct LINK_ACCMAIL_LIST_ACK;
struct LINK_ACCMAIL_READ_ACK;
struct LINK_ACCMAIL_CLAIM_ACK;
struct LINK_ACCMAIL_ERASE_ACK;
struct LINK_ACCMAIL_SEND_ACK;

class CMailHandler final : public IMailStoreEvents
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(세션 허브 · 본인 알림 · CItemHandler · 저장소 · 우편 규칙 · 계정 우편 요청 · 게임 지표)보다
    //   뒤에 이것을 선언해야 한다. 저장소(_characterStore)는 거꾸로 이것보다 앞에 세워져 이것의 참조(IMailStoreEvents)를 받는다 — 생성 중엔 서로 부르지 않는다.
    CMailHandler(CSessionHub& sessionHub, CSelfNotifier& selfNotifier, CItemHandler& itemHandler, CCharacterStore& characterStore,
                 CMailSystem& mailSystem, CAccountMailClient& accountMailClient, CGameMonitor& gameMonitor)
        : _sessionHub(sessionHub), _selfNotifier(selfNotifier), _itemHandler(itemHandler), _characterStore(characterStore)
        , _mailSystem(mailSystem), _accountMailClient(accountMailClient), _gameMonitor(gameMonitor) {}

    CMailHandler(const CMailHandler&) = delete;
    CMailHandler& operator=(const CMailHandler&) = delete;

    // 받은 패킷 등록 — 우편 요청 여섯. 게임 서버의 등록(RegisterRoutes)이 부른다. false = 등록 실패(기동 거절).
    bool RegisterRoutes(CPacketRoutes& routes);

    // 입장 — 탭마다 수 한 장(캐릭터 우편 = COUNT_PENDING 잡의 회신 · 계정 우편 = COUNT_REQ 의 답). 시험 입장 길은 안 보낸다.
    //   계정 우편 답에 이 캐릭터가 받는 중인 행이 있으면 이어서 처리한다(action=RESUME).
    //   CEntryHandler 가 스폰 끝에 부른다(월드 서버에 입장을 알린 뒤).
    void OnEntered(CPlayer* player);

    // ── 저장소의 우편 회신 (IMailStoreEvents) — 회신 꺼내기(CCharacterStore::ProcessDbResults)와 저장 회신이 부른다 ──
    void OnMailDbReply(const DBMailReply& reply) override;
    void OnMailSaved(CPlayer* player, const DBMailStmt& stmt) override;

    // 운영자 지급(/accmail · mail-plan.md 6절 보내기) — CCommandHandler 가 운영자 검사 뒤 넘긴다. 답은 운영자에게 알림 한 줄씩.
    //   한 계정 = 바로 보낸다 · all = 일괄 번호를 만들어 30초 들고 미리 보여 준다(두 줄) · confirm = 같은 운영자 · 30초 안이면 그 번호로 보낸다.
    //   false = 인자 거절(부르는 쪽이 명령 거절 수를 센다). 보낸 뒤의 실패(연결 없음 · 시한 · 계정 서버 결과)는 true 이고 알림으로 알린다.
    bool RunAdminSend(CPlayer* player, const std::vector<std::string>& words);

private:
    void RecvList(CPlayer* player, CSerialBuffer* pMsg);
    void RecvRead(CPlayer* player, CSerialBuffer* pMsg);
    void RecvSend(CPlayer* player, CSerialBuffer* pMsg);
    void RecvTake(CPlayer* player, CSerialBuffer* pMsg);
    void RecvAccTake(CPlayer* player, CSerialBuffer* pMsg);
    void RecvDelete(CPlayer* player, CSerialBuffer* pMsg);

    // 잡 넣기 — 세션 번호 · 캐릭터 번호를 실어 낸다. false = 넣기 거절(큐 상한 · 워커 없음) — 부르는 쪽이 BUSY 로 닫는다(mail-plan.md 7절 「빠져나오는 길」)
    bool Ask(CPlayer* player, DBMailJob& job);

    // 회신 갈래 — 사람을 찾고 상태가 그 회신을 기다린 것을 OnMailDbReply 가 본 뒤에 온다
    void OnListReply(CPlayer* player, const DBMailReply& reply);
    void OnBodyReply(CPlayer* player, const DBMailReply& reply);
    void OnEraseReply(CPlayer* player, const DBMailReply& reply);
    void OnFindReply(CPlayer* player, const DBMailReply& reply);
    void OnTakeReply(CPlayer* player, const DBMailReply& reply);
    void OnCountReply(CPlayer* player, const DBMailReply& reply);
    void OnMovedReply(CPlayer* player, const DBMailReply& reply);   // 계정 우편 받기 — 「이미 옮겼나」
    void OnNameReply(CPlayer* player, const DBMailReply& reply);    // 계정 우편 받기 — 받는 중인 다른 캐릭터의 이름

    // ── 계정 우편 (mail-plan.md 6절) — 요청을 못 보내면(연결 없음) 그 자리에서 ACC_OFFLINE 으로 닫는다 ──
    void AskAccCount(CPlayer* player, int64_t prevClaim);          // prevClaim = 방금 이어서 처리한 번호(같은 번호가 다시 오면 멈춘다) · 처음엔 0
    void AskAccListPage(CPlayer* player, int64_t beforeMailId, int page);
    void AskAccTakeRead(CPlayer* player, int64_t mailId);
    bool AskAccClaim(CPlayer* player, int64_t mailId);
    void StartAccResume(CPlayer* player, int64_t mailId);
    // 서버 간 답 — 세션 번호 · 캐릭터 번호로 사람을 다시 찾는다. ack == nullptr = 답 없음(시한 · 끊김)
    void OnAccCountAck(SessionId sessionId, CharacterId characterId, int64_t prevClaim, const LINK_ACCMAIL_COUNT_ACK* ack);
    void OnAccListAck(SessionId sessionId, CharacterId characterId, int page, const LINK_ACCMAIL_LIST_ACK* ack);
    void OnAccReadAck(SessionId sessionId, CharacterId characterId, int64_t mailId, const LINK_ACCMAIL_READ_ACK* ack);
    void OnAccTakeReadAck(SessionId sessionId, CharacterId characterId, int64_t mailId, const LINK_ACCMAIL_READ_ACK* ack);
    void OnAccClaimAck(SessionId sessionId, CharacterId characterId, int64_t mailId, const LINK_ACCMAIL_CLAIM_ACK* ack);
    void OnAccEraseAck(SessionId sessionId, CharacterId characterId, int64_t mailId, const LINK_ACCMAIL_ERASE_ACK* ack);
    // 규칙이 정한 다음 걸음을 실행한다 — resume = 이 받기가 입장 때 이어서 처리인가(상태를 닫기 전에 읽어 둔 값)
    void RunAccStep(CPlayer* player, int64_t mailId, bool resume, const AccTakeDecision& decision);
    // 계정 우편 받기의 끝 — 결과 한 장(이어서 처리면 action=RESUME 이고 수를 다시 묻는다). 상태는 이미 IDLE 이다
    void CloseAccTake(CPlayer* player, int64_t mailId, bool resume, MailResult result, const char16_t* name = nullptr);
    // 받음(DONE) — 답이 없다. 못 보내면 경고만(받는 중으로 남아 그 캐릭터의 입장 때 「이미 옮겼나」가 다시 보낸다)
    void SendDone(int64_t accountId, int64_t mailId, int64_t characterId);
    void SendAccListPage(CPlayer* player, const LINK_ACCMAIL_LIST_ACK& ack, int count, bool first, bool more);

    // 운영자 지급 — batchId 0 = 한 계정. 못 보내면(연결 없음) 그 자리에서 알린다
    void SendAdminMail(CPlayer* player, const AdminMailForm& form, int64_t batchId);
    void OnAdminSendAck(SessionId sessionId, CharacterId characterId, int64_t target, int64_t batchId, const LINK_ACCMAIL_SEND_ACK* ack);

    // 가방이 바뀌었다(보내기 · 받기 성사) — 저장 요청을 먼저 내 사유가 MAIL 로 세이게 하고(같은 틱의 둘째 요청은 무시된다) 칸·돈을 내보낸다
    void FlushChanged(CPlayer* player);

    // 결과 — 거절이면 사유별 수를 센다. mailId = 받기 · 지우기 · 읽기의 대상(없으면 0) · name = OTHER_CHARACTER 의 이름(CHAR_NAME_MAX_LEN 칸 · 없으면 nullptr)
    void SendResult(CPlayer* player, MailAction action, MailResult result, int64_t mailId, const char16_t* name = nullptr);
    void SendCount(CPlayer* player, MailTab tab, MailCountKind known, int64_t count);
    void SendList(CPlayer* player, const std::vector<DBMailBrief>& lines);

    CSessionHub&        _sessionHub;
    CSelfNotifier&      _selfNotifier;       // 운영자 지급의 알림 한 줄
    CItemHandler&       _itemHandler;
    CCharacterStore&    _characterStore;
    CMailSystem&        _mailSystem;
    CAccountMailClient& _accountMailClient;
    CGameMonitor&       _gameMonitor;

    // 전 계정 지급 대기 — 한 칸(6절 「CMailHandler가 운영자 캐릭터 · 내용 · 시각과 함께 30초 동안 들고 있고」).
    //   all 을 다시 치면 새 내용 · 새 번호로 바뀐다. confirm 으로 보낸 뒤에도 30초가 지날 때까지 남는다 — 답을 못 받았을 때
    //   confirm 을 다시 쳐도 같은 번호라 두 번 안 들어간다(6절 「시한 3초 안에 답이 안 오면」).
    struct AdminBatch
    {
        bool          active = false;
        CharacterId   operatorId{};
        int64_t       batchId = 0;
        uint64_t      createdMs = 0;   // CEnterGate::NowMs
        AdminMailForm form;
    };
    static constexpr uint64_t ADMIN_BATCH_HOLD_MS = 30000;   // mail-plan.md 6절 보내기 「30초 동안 들고 있고」
    AdminBatch _adminBatch;
};

#endif // USE_MAIL
