// ==========================================================================
// CAccountMailClient — 존 서버의 계정 우편 요청: 요청 번호 · 시한 · 답 짝 맞추기 (mail-plan.md 9절 · USE_MAIL)
//
// [책임]
//  - 요청마다 CLinkRequests::Begin → 월드 서버로 보냄. 못 보내면 Cancel 하고 false — 부르는 쪽이 바로 ACC_OFFLINE 으로 닫는다
//  - 월드 서버가 되돌린 답 여섯을 받아 짝을 지운다(처리표는 CWorldLink 의 것 · 등록은 Start 전)
//  - 월드 서버가 끊기면 대기를 전부 done(nullptr) 로 끝낸다 — 시한까지 기다리지 않는다
//
// [경계]
//  게임 규칙을 모른다 — 답을 그대로 넘기고 해석은 CMailHandler 가 한다(사람 찾기 · 상태 확인 · MailResult 로 옮기기).
//  시한 뒤에 온 답은 짝이 없어 버려진다. 그러면 계정 서버에 「받는 중」이 남고 그 캐릭터의 입장 때 이어진다(6절).
//
// [끄기] UseServerLink=0 이면 월드 서버 연결이 없어 모든 요청이 false — 계정 우편만 ACC_OFFLINE 이고 캐릭터 우편은 그대로다
// [스레드] 게임 스레드 단독. 지표는 CLinkRequests 의 mmo_link_reply_seconds{request} · mmo_link_timeout_total{request}
// ==========================================================================
#pragma once

#include "BuildConfig.h"     // USE_MAIL — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_MAIL

#include "NetIoModel.h"      // 맨 앞 — 소켓 헤더가 Windows.h 보다 먼저 와야 한다(ServerLinkProtocol.h 가 코어 헤더를 끌어온다)

#include <cstdint>
#include <functional>
#include <Link/LinkRequests.h>
#include <Protocol/ServerLinkProtocol.h>

class CSerialBuffer;
class CWorldLink;

class CAccountMailClient
{
public:
    // ack == nullptr = 답을 못 받았다(시한 · 월드 서버 끊김) — 부르는 쪽이 ACC_TIMEOUT 으로 닫는다
    template <typename TAck>
    using ReplyFn = std::function<void(const TAck* ack)>;

    // ★ 참조만 든다 — 부르는 쪽(CGameServer)은 _worldLink · _linkRequests 보다 뒤에 이것을 선언해야 한다
    CAccountMailClient(CWorldLink& worldLink, CLinkRequests& requests) : _worldLink(worldLink), _requests(requests) {}

    CAccountMailClient(const CAccountMailClient&) = delete;
    CAccountMailClient& operator=(const CAccountMailClient&) = delete;

    bool RegisterRoutes();   // 답 여섯 — false = 등록 실패(기동 거절)
    void OnWorldDown();      // 대기를 전부 done(nullptr)

    // ── 답이 있는 요청 — false = 월드 서버 연결이 없어 못 보냄(done 은 안 불린다) ──
    bool Count(int64_t account, int64_t character, ReplyFn<LINK_ACCMAIL_COUNT_ACK> done);
    bool List(int64_t account, int64_t beforeMailId, ReplyFn<LINK_ACCMAIL_LIST_ACK> done);
    bool Read(int64_t account, int64_t mailId, ReplyFn<LINK_ACCMAIL_READ_ACK> done);
    bool Claim(int64_t account, int64_t mailId, int64_t character, ReplyFn<LINK_ACCMAIL_CLAIM_ACK> done);
    bool Erase(int64_t account, int64_t mailId, ReplyFn<LINK_ACCMAIL_ERASE_ACK> done);
    bool Send(LINK_ACCMAIL_SEND_REQ& request, ReplyFn<LINK_ACCMAIL_SEND_ACK> done);   // requestId 는 여기서 채운다

    // ── 답이 없는 둘 — false = 못 보냄. 행이 받는 중으로 남아 그 캐릭터의 입장 때 이어진다 ──
    bool Finish(int64_t account, int64_t mailId, int64_t character);    // LINK_ACCMAIL_DONE — 받는 중 → 받음
    bool Unclaim(int64_t account, int64_t mailId, int64_t character);   // LINK_ACCMAIL_UNCLAIM — 받는 중 → 안 받음

private:
    template <typename TRequest, typename TAck>
    bool Ask(LinkRequestKind kind, TRequest& request, ReplyFn<TAck> done);
    template <typename TAck>
    void OnAck(CSerialBuffer* msg);

    CWorldLink&    _worldLink;
    CLinkRequests& _requests;
};

#endif // USE_MAIL
