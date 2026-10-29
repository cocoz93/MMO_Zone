#pragma once

#include "ClientConfig.h"
#include "ClientNetwork.h"
#include "PlayerManager.h"
#include "ConsoleRenderer.h"
#include "NetworkEventQueue.h"
#include <string>
#include <vector>

class CGameInstance
{
public:
    CGameInstance();
    ~CGameInstance();

    // 초기화 및 실행
    bool Initialize();
    void Run();
    void Shutdown();

    // 네트워크 연결
    bool ConnectToServer(const std::string& serverIp, int port);

    // S2C 패킷 핸들러 (수신 스레드에서 호출 → 이벤트 큐에 Push)
    void OnZoneInfo(const MSG_S2C_ZONE_INFO* msg);
    void OnCreateMyPlayer(const MSG_S2C_CREATE_MY_PLAYER* msg);
    void OnCreateOtherPlayer(const MSG_S2C_CREATE_OTHER_PLAYER* msg);
    void OnDeletePlayer(const MSG_S2C_DELETE_PLAYER* msg);
    void OnMoveStart(const MSG_S2C_MOVE_START* msg);
    void OnMoveStop(const MSG_S2C_MOVE_STOP* msg);
    void OnChat(const MSG_S2C_CHAT* msg);
    void OnSyncPosition(const MSG_S2C_SYNC_POSITION* msg);
    void OnSectorUpdates(const MSG_S2C_SECTOR_UPDATES* msg);  // 섹터 묶음(USE_SECTOR_AGGREGATION): 엔트리별 이동 이벤트로 분해
    void OnCreatePlayerBatch(const MSG_S2C_CREATE_PLAYER_BATCH* msg);  // 멤버십 인바운드 묶음: 엔트리별 기존 CREATE 이벤트로 분해
    void OnDeletePlayerBatch(const MSG_S2C_DELETE_PLAYER_BATCH* msg);  // 멤버십 인바운드 묶음: 엔트리별 기존 DELETE 이벤트로 분해
    void OnZoneChangeOk(const MSG_S2C_ZONE_CHANGE_OK* msg);
    void OnZoneChangeFail(const MSG_S2C_ZONE_CHANGE_FAIL* msg);
    void OnError(const MSG_S2C_ERROR* msg);
    void OnAdminLoginOk();
    void OnAdminLoginFail();
    void OnExp(const MSG_S2C_EXP* msg);   // 서버 USE_EXP: 내 레벨·경험치 (본인에게만 온다)

private:
    // 남의 캐릭터 한 명분 → CREATE_OTHER_PLAYER 이벤트 (수신 스레드). OnCreateOtherPlayer 와 묶음의 엔트리마다 부른다 —
    //   둘이 같은 구조체(Protocol.h CreatePlayerEntry)라 이벤트로 옮겨 담는 칸도 여기 한 곳
    void PushCreateOther(const CreatePlayerEntry& e);

    // 게임 루프 (25fps)
    void GameLoop();

    // 이벤트 큐 소비 (게임 루프 스레드에서)
    void ProcessNetworkEvents();

    // 키보드 입력 처리
    void PollConsoleInput();    // 콘솔 버퍼에서 키 상태 갱신
    void ProcessInput();
    void ProcessChatInput();

    // 채팅 로그에 메시지 추가
    void AddChatMessage(const std::wstring& msg);

    // 채팅 커맨드 처리 (/map 등)
    void HandleChatCommand(const std::wstring& command);

private:
    ClientConfig _config;
    CClientNetwork _network;
    CPlayerManager _playerManager;
    CConsoleRenderer _renderer;
    CNetworkEventQueue _eventQueue;
    bool _running;

    // 키 상태 추적 (콘솔 입력 버퍼 기반, 포커스 창만 수신)
    bool _keyDown[256];   // 가상 키코드별 눌림 상태
    bool _enterPressed;
    bool _escPressed;

    // 채팅
    std::vector<std::wstring> _chatLog;
    std::wstring _chatInput;
    bool _chatMode;
    static constexpr int MAX_CHAT_LOG = 96; // 렌더러 표시 줄 수(24) × 4

    // 프레임 설정 (서버 동일)
    static constexpr int FRAME_PER_SEC = 25;
    static constexpr int FRAME_INTERVAL_MS = 1000 / FRAME_PER_SEC;  // 40ms

    // 존 이동 전환 상태 (요청~응답 사이 딤 오버레이 + 입력 차단)
    bool _zoneChanging = false;
    bool _zoneChangeResponseReceived = false;
    int  _zoneChangeElapsedMs = 0;
    static constexpr int ZONE_CHANGE_MIN_DISPLAY_MS = 500;

    // 하트비트 (서버 타임아웃 60초 대비 20초 간격)
    static constexpr int HEARTBEAT_INTERVAL_MS = 20000;
    int _heartbeatAccumMs;

    // 재접속 설정 (서버가 아직 안 떴을 때 대기용)
    static constexpr int RECONNECT_INTERVAL_SEC = 3;
    static constexpr int RECONNECT_MAX_RETRY = 5;
};
