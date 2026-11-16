#pragma once
// ==========================================================================
// ServerLinkConfig — 서버 위치 한 파일(Run/bin/ServerLink.ini)
//
//   각 서버는 자기 섹션의 주소로 접속을 기다리고, 위층 섹션의 주소로 접속한다.
//   서버를 옮기면 이 파일만 고친다 — 주소가 서버별 설정에 중복되지 않는다.
//
//   파일이 없거나 값이 틀리면 Load 가 false 다. 부르는 쪽은 기동을 거절한다 —
//   주소를 잘못 적었는데 연결이 조용히 꺼진 채 뜨면 「고르기가 ZONE_DOWN」으로만 보여 원인을 찾기 어렵다.
//   그래서 주소는 기본값을 안 둔다. 나머지 값은 키가 없으면 아래 기본값이다.
// ==========================================================================

#include <string>

struct ServerLinkConfig
{
    struct Address
    {
        std::string ip;
        int port = 0;
    };

    Address account;     // [Account] InternalAddr
    Address character;   // [Character] InternalAddr
    Address world;       // [World] InternalAddr — 섹션이 없으면 월드 서버 없이 도는 것이다(존 서버는 파티를 끈다). 값이 틀리면 거절

    int retrySec = 3;          // 아래층이 위층에 다시 접속하는 주기 — 근거 없이 고른 값(플랜 4절)
    int heartbeatSec = 20;     // 코어 세션 시한 60초(IOCPServer.h)의 3분의 1 — 시한 안에 세 번 보낸다. 받는 쪽은 받을 때마다 시한을 다시 잰다(ProcessRecv).
                               //   다른 패킷이 없을 때 한 번 빠져도 다음 것이 40초에 닿지만, 연달아 두 번 빠지면 다음 것이 시한 끝(60초)에 닿아 끊길 수 있다
    int replyTimeoutSec = 3;   // 위층 응답 시한 — 근거 없이 고른 값(플랜 4절)
    int sessionReserve = 8;    // 세션 표에 서버 간 연결 몫으로 더하는 칸 — 근거 없이 고른 값(플랜 4절)

    // "127.0.0.1:6101" → ip · port. 포트 1~65535, 틀리면 false 이고 out 은 그대로다
    static bool ParseAddress(const std::string& text, Address& out);

    // 실패하면 error 에 어느 값이 왜 틀렸는지를 담는다
    bool Load(const std::string& path, std::string& error);

    bool HasWorld() const { return world.port != 0; }
};
