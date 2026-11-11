// ==========================================================================
// LinkOnlineList — 접속 장부 줄을 만들고 명단을 보낸다
//
//   존 서버는 같은 패킷(3004 · 3007)을 캐릭터 서버와 월드 서버에 각각 보낸다. 두 연결이 같은 코드를 쓰게 여기 둔다.
//   게임 스레드 전용.
// ==========================================================================
#pragma once

#include "NetIoModel.h"   // 맨 앞 — 소켓 헤더가 Windows.h 보다 먼저 와야 한다

#include <Link/LinkConnector.h>
#include <Protocol/ServerLinkProtocol.h>

class CPlayer;
class CSessionHub;

namespace LinkOnlineList
{
    void Fill(LinkOnlineEntry& out, const CPlayer& player);

    // 지금 접속 중인 사람을 패킷 여러 장으로 보낸다. 돌려주는 값 = 보낸 사람 수.
    //   명단이 비면 안 보낸다 — 받는 쪽은 연결이 끊길 때 이 존의 장부를 비우므로 빈 명단을 알릴 일이 없다
    int Send(CLinkConnector& connector, const CSessionHub& sessionHub);
}
