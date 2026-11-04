#pragma once
// ==========================================================================
// LobbyConfig — 로비 서버(계정·캐릭터) 공통 설정 : [Server] · [DB]
//
//   숫자 기본값은 전부 근거 없이 고른 값이다(플랜 6절). 포트와 DB 접속 값은 기본값이 없다 —
//   없으면 기동을 거절한다. 틀린 값으로 조용히 뜨면 원인이 안 보인다(ServerLinkConfig 와 같은 규칙).
// ==========================================================================

#include <string>

#include <Db/DbConnection.h>

class CIniFile;

struct LobbyConfig
{
    static constexpr int MAX_SESSION_SLOTS = 65535;   // 세션 인덱스가 SessionID 상위 16bit 라서 — 존 서버 MAX_CLIENTS_LIMIT 와 같은 까닭

    int port = 0;              // 웹 포트
    int monitorPort = 0;       // 지표 포트
    int maxClients = 1000;     // 웹 정원
    int workerThreads = 2;     // 코어 워커
    int logicPeriodMs = 10;    // 로직 스레드 주기
    int dbWorkers = 2;
    int dbQueueMax = 1000;
    DBConfig db;

    // 실패하면 error 에 어느 값이 왜 틀렸는지를 담는다
    bool Read(const CIniFile& ini, std::string& error);

    // 세션 표 = 웹 정원 + 서버 간 여유 칸. 세션 번호에 안 담기면 false
    bool CheckSessionSlots(int reserve, std::string& error) const;

    // 첫 섹션 값이 읽혔는지 기동 로그로 본다
    void Print() const;
};
