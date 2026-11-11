// ==========================================================================
// CMailHandler — 우편: 우편 요청과 우편 회신 (mail-plan.md · USE_MAIL)
//
// [책임]
//  - 받은 패킷 여섯의 처리기와 그 등록(RegisterRoutes — 목록 · 읽기 · 보내기 · 받기 · 계정 우편 받기 · 지우기)
//  - 우편 규칙(CMailSystem)을 불러 판정을 받고, 통과하면 DB 잡을 낸다(CCharacterStore::RequestMailJob)
//  - 저장소의 우편 회신 받기(IMailStoreEvents — 우편 잡 회신 · 우편 문장 확정) → 사람을 다시 찾아 절차를 잇는다
//  - 가방이 바뀐 뒤 — 저장 요청(RequestSave(MAIL)) · 칸·돈 내보내기(CItemHandler::FlushInventory)
//  - 결과 패킷 넷(S2C_MAIL_LIST · BODY · RESULT · COUNT) 만들기와 보낸 수 · 거절 수 세기
//  - 입장 때 수(OnEntered — 탭마다 S2C_MAIL_COUNT 한 장)
//
// [경계]
//  DB 를 직접 부르지 않는다 — 잡은 저장소에, 칸·돈과 함께 쓸 문장은 대기 문장(CPlayer::_mailDraft)에 둔다(규칙이 올린다).
//  규칙의 내용(무엇이 되고 안 되나 · 상태 전이)은 CMailSystem 몫이다 — 여기는 그 값을 패킷으로 바꾼다.
//
// [지금 — 3단계]
//  계정 우편 탭(tab=1 · C2S_ACCMAIL_TAKE)은 사전 검사 뒤 ACC_OFFLINE 으로 답한다 — 계정 우편(mail-plan.md 12절 8단계)이 들어오기 전까지.
//  입장 때 계정 우편 탭의 수는 known=UNKNOWN(못 읽음)으로 보낸다 — 웹이 0 을 「없음」으로 읽지 않게(9절 S2C_MAIL_COUNT).
// ==========================================================================
#pragma once

#include "BuildConfig.h"     // USE_MAIL — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_MAIL

#include <cstdint>
#include <vector>
#include <Protocol/Protocol.h>   // MailAction · MailTab · MailResult · MailCountKind
#include "CharacterStore.h"  // IMailStoreEvents · DBMailReply · DBMailStmt · DBMailJob

class CPlayer;
class CSerialBuffer;
class CPacketRoutes;
class CSessionHub;
class CItemHandler;
class CMailSystem;
class CGameMonitor;

class CMailHandler final : public IMailStoreEvents
{
public:
    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 넘기는 것들(세션 허브 · CItemHandler · 저장소 · 우편 규칙 · 게임 지표)보다 뒤에 이것을 선언해야 한다.
    //   저장소(_characterStore)는 거꾸로 이것보다 앞에 세워져 이것의 참조(IMailStoreEvents)를 받는다 — 생성 중엔 서로 부르지 않는다.
    CMailHandler(CSessionHub& sessionHub, CItemHandler& itemHandler, CCharacterStore& characterStore,
                 CMailSystem& mailSystem, CGameMonitor& gameMonitor)
        : _sessionHub(sessionHub), _itemHandler(itemHandler), _characterStore(characterStore)
        , _mailSystem(mailSystem), _gameMonitor(gameMonitor) {}

    CMailHandler(const CMailHandler&) = delete;
    CMailHandler& operator=(const CMailHandler&) = delete;

    // 받은 패킷 등록 — 우편 요청 여섯. 게임 서버의 등록(RegisterRoutes)이 부른다. false = 등록 실패(기동 거절).
    bool RegisterRoutes(CPacketRoutes& routes);

    // 입장 — 탭마다 수 한 장(캐릭터 우편 = COUNT_PENDING 잡의 회신 · 계정 우편 = 3단계엔 못 읽음). 시험 입장 길은 안 보낸다.
    //   CEntryHandler 가 스폰 끝에 부른다(월드 서버에 입장을 알린 뒤).
    void OnEntered(CPlayer* player);

    // ── 저장소의 우편 회신 (IMailStoreEvents) — 회신 꺼내기(CCharacterStore::ProcessDbResults)와 저장 회신이 부른다 ──
    void OnMailDbReply(const DBMailReply& reply) override;
    void OnMailSaved(CPlayer* player, const DBMailStmt& stmt) override;

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

    // 가방이 바뀌었다(보내기 · 받기 성사) — 저장 요청을 먼저 내 사유가 MAIL 로 세이게 하고(같은 틱의 둘째 요청은 무시된다) 칸·돈을 내보낸다
    void FlushChanged(CPlayer* player);

    // 결과 — 거절이면 사유별 수를 센다. mailId = 받기 · 지우기 · 읽기의 대상(없으면 0)
    void SendResult(CPlayer* player, MailAction action, MailResult result, int64_t mailId);
    void SendCount(CPlayer* player, MailTab tab, MailCountKind known, int64_t count);
    void SendList(CPlayer* player, const std::vector<DBMailBrief>& lines);

    CSessionHub&     _sessionHub;
    CItemHandler&    _itemHandler;
    CCharacterStore& _characterStore;
    CMailSystem&     _mailSystem;
    CGameMonitor&    _gameMonitor;
};

#endif // USE_MAIL
