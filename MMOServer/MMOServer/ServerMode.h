#pragma once

// 서버 동작 모드 — INI Mode= 가 고른다(ServerConfig::ParseServerMode). 네트워크 코어는 이것을 모른다:
//   에코 두 모드는 GameServer::Init 이 자르기 부품과 받은 것 넘길 곳(CEchoSink)을 골라 코어에 넘긴다.
//   (전에는 코어 CIOCPServer 가 이 열거형으로 갈랐다 — 코어를 게임과 떼며 이리 옮겼다)
enum class ServerMode
{
    GameCodiEchoTest,    // GameCodi 에코 더미 클라이언트 테스트용 - 헤더: EchoMsgHeader(2byte), size=페이로드크기
    NetWorkLib_EchoTest, // 네트워크 라이브러리 검증용 에코 - 헤더: MsgHeader(4byte), size=전체크기(헤더포함)
    GameServer,          // MMO 게임 서버 (네트워크 + 게임 로직) - 헤더: MsgHeader(4byte), size=전체크기(헤더포함)
};

// INI 에 적는 이름 그대로 — 기동 로그 · 설정 출력이 같이 쓴다
inline const char* ServerModeName(ServerMode mode)
{
    switch (mode)
    {
    case ServerMode::GameCodiEchoTest:    return "GameCodiEchoTest";
    case ServerMode::NetWorkLib_EchoTest: return "NetWorkLib_EchoTest";
    case ServerMode::GameServer:          return "GameServer";
    }
    return "Unknown";
}
