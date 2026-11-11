#pragma once
// ==========================================================================
// WorldServerConfig — Run/bin/WorldServerConfig.ini
//
//   월드 서버는 DB 를 쓰지 않아서 로비 공통 설정(LobbyConfig 는 [DB] 가 필수다)을 안 쓴다.
//   서버 간 주소는 ServerLink.ini 의 [World].
//   런타임 INI — CP949 무BOM CRLF(CLAUDE.md).
// ==========================================================================

#include <string>

struct WorldServerConfig
{
    static constexpr int MAX_SESSION_SLOTS = 65535;   // 세션 인덱스가 SessionID 상위 16bit 라서 — LobbyConfig 와 같은 까닭

    int port = 0;              // 코어가 늘 여는 포트 — 월드 서버는 웹 손님이 없어 들어오면 바로 끊는다
    int monitorPort = 0;       // 지표 포트
    int maxClients = 1;        // 위 포트의 정원 — 받을 손님이 없어 코어가 허용하는 최소
    int workerThreads = 2;     // 코어 워커 — 로비 서버와 같은 값. 재 본 적이 없다
    int logicPeriodMs = 10;    // 로직 스레드 주기 — 로비 서버와 같은 값. 재 본 적이 없다
    int worldId = 1;           // 계정 서버에 알리는 번호 — 월드는 지금 하나뿐이다

    // 실패하면 error 에 어느 값이 왜 틀렸는지를 담는다
    bool Load(const std::string& path, std::string& error);

    // 세션 표 = 정원 + 서버 간 여유 칸. 세션 번호에 안 담기면 false
    bool CheckSessionSlots(int reserve, std::string& error) const;

    // 첫 섹션 값이 읽혔는지 기동 로그로 본다
    void Print() const;
};
