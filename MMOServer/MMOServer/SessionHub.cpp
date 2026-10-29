#include "SessionHub.h"
#include "MonitorManager.h"
#include "SerialBuffer.h"
#include <Common/ErrorLog.h>

// ── 세션 칸 ──

void CSessionHub::Init(NetIoModel* network, int maxClients)
{
    _network = network;
    _slots.clear();                       // 인덱스 직접 접근용 슬롯 (sessionId 인덱스로 접근)
    _slots.resize(maxClients);            //   unique_ptr 라 assign(n, nullptr) 대신 — 전부 빈 칸으로
}

void CSessionHub::RemoveAll()
{
    _slots.clear();
}

void CSessionHub::AddPlayer(int64_t sessionId, std::unique_ptr<CPlayer> owned)
{
    // 경계 매핑 등록 — 이제 슬롯이 주인이다
    owned->_sessionId = sessionId;
    std::unique_ptr<CPlayer>& slot = _slots[CSession::ExtractIndex(sessionId)];
    if (slot)
    {
        // 있을 수 없다 — 같은 자리의 끊김 이벤트는 다음 접속보다 먼저 온다(SetPendingEnter 와 같은 불변식).
        //   그래도 그렇게 되면 옛 플레이어는 해제하지 않고 놓아 버린다(release) — 아직 존 목록이 그 주소를
        //   들고 있을 수 있어 여기서 해제하면 다음 틱이 해제된 메모리를 밟는다. 새는 쪽이 덜 위험하다(전에도 새었다).
        SLOG_ERROR("[ENTER] session slot still occupied (slot={}, old session={}, new session={})",
                   CSession::ExtractIndex(sessionId), slot->_sessionId, sessionId);
        assert(false && "슬롯에 살아 있는 플레이어가 있다");
        (void)slot.release();
    }
    slot = std::move(owned);
}

void CSessionHub::RemovePlayer(CPlayer* player)
{
    // 슬롯 비우기 — 주인이 슬롯이라 여기서 해제된다(맨 끝이어야 한다)
    std::unique_ptr<CPlayer>& slot = _slots[CSession::ExtractIndex(player->_sessionId)];
    assert(slot.get() == player);
    slot.reset();
}

// ── 패킷 전송 추상화 ──
// 단일 플레이어에게 패킷 전송 (CSerialBuffer — 소유권 1을 소비)
// 빌더가 반환한 RefCount=1 버퍼를 넘기면, 송신 후 SubRef로 소유권을 회수한다.
//   받는 쪽은 CPlayer* 뿐이다. 세션 -1 검사는 둔다 — 스폰 도중처럼 세션이 잠시 없는 상태가 생기면 이 한 줄이 막는다.
void CSessionHub::SendPacket(CPlayer* target, CSerialBuffer* pMsg)
{
    if (target->_sessionId != -1)
    {
        pMsg->AddRef();   // 송신용 소유권 — RequestSendMsg(CSerialBuffer*)가 소비
        // 게임루프 송신은 묶어 보낼 수 있음을 표시(Deferred) — 실제 지연 여부는 USE_SEND_COALESCING이 결정
        const int dataSize = pMsg->GetDataSize();
        if (_network->RequestSendMsg(target->_sessionId, pMsg, SendFlush::Deferred))
        {
            _monitor._sendPackets.Inc();
            _monitor._sendEnqueuedBytes.Add(static_cast<int64_t>(dataSize));
        }
    }
    pMsg->SubRef();       // 빌더가 넘긴 소유권 1 회수 (세션 무효여도 안전 회수)
}
