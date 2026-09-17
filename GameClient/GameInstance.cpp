#include "GameInstance.h"
#include "../ServerCore/Base/Common/ErrorLog.h"
#include <iostream>
#include <chrono>
#include <cstddef>  // offsetof
#define NOMINMAX
#include <Windows.h>

CGameInstance::CGameInstance()
    : _running(false)
    , _keyDown{}
    , _enterPressed(false)
    , _escPressed(false)
    , _chatMode(false)
    , _heartbeatAccumMs(0)
{
}

CGameInstance::~CGameInstance()
{
    Shutdown();
}

bool CGameInstance::Initialize()
{
    _config.Load();

    _network.SetGameInstance(this);

    _renderer.Init();

    _running = true;
    return true;
}

void CGameInstance::Run()
{
    // 서버 접속 재시도
    for (int attempt = 1; attempt <= RECONNECT_MAX_RETRY; ++attempt)
    {
        std::cout << "[Connect] Attempt " << attempt << "/" << RECONNECT_MAX_RETRY
                  << " (" << _config.serverIp << ":" << _config.serverPort << ")" << std::endl;

        if (ConnectToServer(_config.serverIp, _config.serverPort))
            break;

        if (attempt == RECONNECT_MAX_RETRY)
        {
            LOG_ERROR_STREAM("Failed to connect after " << RECONNECT_MAX_RETRY << " attempts.");
            return;
        }

        std::cout << "[Connect] Retrying in " << RECONNECT_INTERVAL_SEC << "s..." << std::endl;
        Sleep(RECONNECT_INTERVAL_SEC * 1000);
    }

    // 현재 Enter 키 상태 반영 (첫 프레임 채팅 모드 진입 방지)
    _enterPressed = (GetKeyState(VK_RETURN) & 0x8000) != 0;

    // 게임 루프 진입
    GameLoop();
}

void CGameInstance::Shutdown()
{
    _running = false;
    _network.Disconnect();
}

bool CGameInstance::ConnectToServer(const std::string& serverIp, int port)
{
    return _network.Connect(serverIp, port);
}

// ==========================================================================
// 게임 루프 (25fps)
// ==========================================================================

void CGameInstance::GameLoop()
{
    using Clock = std::chrono::steady_clock;

    auto prevTime = Clock::now();

    while (_running && _network.IsConnected())
    {
        auto frameStart = Clock::now();

        // deltaTime 계산
        float deltaTime = std::chrono::duration<float>(frameStart - prevTime).count();
        prevTime = frameStart;

        // 1) 네트워크 이벤트 소비
        ProcessNetworkEvents();

        // 2) 존 이동 전환 중 최소 표시 시간 체크 (응답 올 때까지 대기, 타임아웃 없음)
        if (_zoneChanging)
        {
            _zoneChangeElapsedMs += static_cast<int>(deltaTime * 1000.0f);
            if (_zoneChangeResponseReceived && _zoneChangeElapsedMs >= ZONE_CHANGE_MIN_DISPLAY_MS)
            {
                _zoneChanging = false;
                _zoneChangeResponseReceived = false;
            }
        }

        // 3) 콘솔 입력 버퍼에서 키 상태 갱신 후 입력 처리 (전환 중 이동 차단)
        if (_chatMode)
            ProcessChatInput();
        else
        {
            PollConsoleInput();
            if (_zoneChanging)
            {
                // 전환 중에는 ESC 종료만 허용
                bool escDown = _keyDown[VK_ESCAPE];
                if (escDown && !_escPressed)
                {
                    _escPressed = true;
                    _running = false;
                }
                _escPressed = escDown;
            }
            else
                ProcessInput();
        }

        // 4) 하트비트 송신 (20초 간격)
        _heartbeatAccumMs += static_cast<int>(deltaTime * 1000.0f);
        if (_heartbeatAccumMs >= HEARTBEAT_INTERVAL_MS)
        {
            _heartbeatAccumMs -= HEARTBEAT_INTERVAL_MS;
            _network.SendHeartbeat();
        }

        // 5) 클라이언트 예측 이동 갱신 (전환 중 화면 고정)
        if (!_zoneChanging)
        {
            bool myPlayerClamped = false;
            _playerManager.UpdateMovingPlayers(deltaTime, myPlayerClamped);

            // 벽에 닿아 자동 정지된 경우 서버에 MOVE_STOP 전송 (서버 Zone::Tick과 동일 동작)
            if (myPlayerClamped)
            {
                ClientPlayer* me = _playerManager.GetMyPlayer();
                if (me)
                {
                    _network.SendMoveStop(
                        static_cast<uint8_t>(me->direction), me->x, me->y);
                }
            }
        }

        // 6) 렌더링
        _renderer.RenderFrame(
            _playerManager.GetMyPlayer(),
            _playerManager.GetOtherPlayers(),
            _chatLog,
            _chatInput,
            _chatMode,
            _zoneChanging);

        // 7) 프레임 제한
        auto frameEnd = Clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frameEnd - frameStart);
        int sleepMs = FRAME_INTERVAL_MS - static_cast<int>(elapsed.count());
        if (sleepMs > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }
}

// ==========================================================================
// 네트워크 이벤트 소비 (게임 루프 스레드)
// ==========================================================================

void CGameInstance::ProcessNetworkEvents()
{
    ClientNetworkEvent event;
    while (_eventQueue.Pop(event))
    {
        switch (event.type)
        {
        case ClientNetworkEvent::Type::ZONE_INFO:
            _renderer.SetZoneInfo(event.mapId, event.channelIndex);
            _renderer.SetMapSize(event.mapWidth, event.mapHeight);
            _renderer.SetSectorSize(event.sectorSize);
            _playerManager.SetMapSize(event.mapWidth, event.mapHeight);
            break;

        case ClientNetworkEvent::Type::CREATE_MY_PLAYER:
            _playerManager.SetMyPlayer(
                event.playerId, event.x, event.y,
                static_cast<Direction>(event.direction), event.speed,
                event.displayChar, event.colorIndex);
            AddChatMessage(L"[System] Connected to server.");
            break;

        case ClientNetworkEvent::Type::CREATE_OTHER_PLAYER:
            _playerManager.AddOtherPlayer(
                event.playerId, event.x, event.y,
                static_cast<Direction>(event.direction),
                static_cast<MoveState>(event.moveState), event.speed,
                event.displayChar, event.colorIndex,
                static_cast<SpawnReason>(event.spawnReason));
            break;

        case ClientNetworkEvent::Type::DELETE_PLAYER:
            _playerManager.RemovePlayer(event.playerId);
            break;

        case ClientNetworkEvent::Type::MOVE_START:
        {
            ClientPlayer* player = _playerManager.FindPlayer(event.playerId);
            if (player)
            {
                player->x = event.x;
                player->y = event.y;
                player->direction = static_cast<Direction>(event.direction);
                player->moveState = MoveState::MOVING;
            }
            break;
        }

        case ClientNetworkEvent::Type::MOVE_STOP:
        {
            // 내 캐릭터인 경우 (서버 경계 클램핑 등)
            ClientPlayer* me = _playerManager.GetMyPlayer();
            if (me && me->playerId == event.playerId)
            {
                // 이미 다른 방향으로 이동 중이면 이 MOVE_STOP은 무시
                // (4방향 단일축 이동에서는 서버/클라 클램프 값이 동일하므로 위치 보정 불필요)
                Direction stopDir = static_cast<Direction>(event.direction);
                if (me->moveState == MoveState::MOVING && me->direction != stopDir)
                    break;

                me->x = event.x;
                me->y = event.y;
                me->direction = stopDir;
                me->moveState = MoveState::IDLE;
                break;
            }

            ClientPlayer* player = _playerManager.FindPlayer(event.playerId);
            if (player)
            {
                player->x = event.x;
                player->y = event.y;
                player->direction = static_cast<Direction>(event.direction);
                player->moveState = MoveState::IDLE;
            }
            break;
        }

        case ClientNetworkEvent::Type::CHAT:
        {
            wchar_t buf[CHAT_MSG_MAX_LEN + 8];
            swprintf_s(buf, L"[%c] %s", static_cast<wchar_t>(event.displayChar), event.chatMessage);
            AddChatMessage(buf);
            break;
        }

        case ClientNetworkEvent::Type::SYNC_POSITION:
        {
            ClientPlayer* me = _playerManager.GetMyPlayer();
            if (me && event.playerId == me->playerId)
            {
                me->x = event.x;
                me->y = event.y;
            }
            else
            {
                ClientPlayer* other = _playerManager.FindPlayer(event.playerId);
                if (other)
                {
                    other->x = event.x;
                    other->y = event.y;
                }
            }
            break;
        }

        case ClientNetworkEvent::Type::ZONE_CHANGE_OK:
        {
            int32_t prevSpeed = _playerManager.HasMyPlayer()
                ? _playerManager.GetMyPlayer()->speed : 50;
            _playerManager.Clear();
            _playerManager.SetMyPlayer(
                event.playerId, event.x, event.y, static_cast<Direction>(event.direction), prevSpeed,
                event.displayChar, event.colorIndex);
            _renderer.SetZoneInfo(event.mapId, event.channelIndex);
            _zoneChangeResponseReceived = true;
            wchar_t buf[128];
            swprintf_s(buf, L"[System] Zone changed: Map=%d Channel=%d",
                        event.mapId, event.channelIndex);
            AddChatMessage(buf);
            break;
        }

        case ClientNetworkEvent::Type::ZONE_CHANGE_FAIL:
        {
            _zoneChangeResponseReceived = true;
            const wchar_t* reason;
            switch (event.reason)
            {
            case 0:  reason = L"Not found";            break;
            case 1:  reason = L"All channels full";    break;
            case 2:  reason = L"Already in this channel"; break;
            default: reason = L"Unknown";              break;
            }
            wchar_t buf[128];
            swprintf_s(buf, L"[System] Zone change failed: %s", reason);
            AddChatMessage(buf);
            break;
        }

        case ClientNetworkEvent::Type::ERROR_MSG:
        {
            wchar_t buf[300];
            // char → wchar_t 변환
            wchar_t wMsg[256];
            MultiByteToWideChar(CP_UTF8, 0, event.errorMessage, -1, wMsg, 256);
            swprintf_s(buf, L"[Error] %s", wMsg);
            AddChatMessage(buf);
            break;
        }

        case ClientNetworkEvent::Type::ADMIN_LOGIN_OK:
            AddChatMessage(L"[System] Admin login successful.");
            break;

        case ClientNetworkEvent::Type::ADMIN_LOGIN_FAIL:
            AddChatMessage(L"[System] Admin login failed. Invalid key.");
            break;
        }
    }
}

// ==========================================================================
// 콘솔 입력 버퍼 폴링 — 키 상태 갱신
// (콘솔 입력 버퍼는 포커스된 창에만 이벤트가 들어옴)
// ==========================================================================

void CGameInstance::PollConsoleInput()
{
    HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);
    INPUT_RECORD records[32];
    DWORD numRead;

    while (PeekConsoleInputW(hInput, records, 1, &numRead) && numRead > 0)
    {
        ReadConsoleInputW(hInput, records, 32, &numRead);
        for (DWORD i = 0; i < numRead; ++i)
        {
            if (records[i].EventType != KEY_EVENT)
                continue;
            const auto& ke = records[i].Event.KeyEvent;
            WORD vk = ke.wVirtualKeyCode;
            if (vk < 256)
                _keyDown[vk] = (ke.bKeyDown != 0);
        }
    }
}

// ==========================================================================
// 키보드 입력 처리 — 일반 모드 (이동)
// ==========================================================================

void CGameInstance::ProcessInput()
{
    // 현재 프레임의 키 상태 조사
    bool currentKeys[4];
    currentKeys[0] = _keyDown[VK_UP];
    currentKeys[1] = _keyDown[VK_DOWN];
    currentKeys[2] = _keyDown[VK_LEFT];
    currentKeys[3] = _keyDown[VK_RIGHT];

    // ESC 종료 (엣지 검출 — 채팅 취소 후 연속 종료 방지)
    bool escDown = _keyDown[VK_ESCAPE];
    if (escDown && !_escPressed)
    {
        _escPressed = true;
        _running = false;
        return;
    }
    _escPressed = escDown;

    // Enter → 채팅 모드 진입 (엣지 검출)
    bool enterDown = _keyDown[VK_RETURN];
    if (enterDown && !_enterPressed)
    {
        // 이동 중이면 정지 후 진입
        ClientPlayer* me = _playerManager.GetMyPlayer();
        if (me && me->moveState == MoveState::MOVING)
        {
            _network.SendMoveStop(
                static_cast<uint8_t>(me->direction), me->x, me->y);
            me->moveState = MoveState::IDLE;
        }

        _enterPressed = enterDown;
        _chatMode = true;
        _chatInput.clear();
        FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
        memset(_keyDown, 0, sizeof(_keyDown));
        return;
    }
    _enterPressed = enterDown;

    // 내 캐릭터가 아직 생성되지 않았으면 입력 무시
    if (!_playerManager.HasMyPlayer())
        return;

    // 키 입력으로 4방향 결정 (동시 입력 시 상쇄)
    bool up    = currentKeys[0];
    bool down  = currentKeys[1];
    bool left  = currentKeys[2];
    bool right = currentKeys[3];

    if (up && down)    { up = false; down = false; }
    if (left && right) { left = false; right = false; }

    // 세로 우선 (동시 입력 시 세로 채택)
    Direction newDir = Direction::NONE;
    if (up)         newDir = Direction::UP;
    else if (down)  newDir = Direction::DOWN;
    else if (left)  newDir = Direction::LEFT;
    else if (right) newDir = Direction::RIGHT;

    ClientPlayer* me = _playerManager.GetMyPlayer();

    Direction curDir = (me->moveState == MoveState::MOVING) ? me->direction : Direction::NONE;

    if (newDir != curDir)
    {
        // 이동 중이었으면 정지 전송
        if (me->moveState == MoveState::MOVING)
        {
            _network.SendMoveStop(
                static_cast<uint8_t>(me->direction), me->x, me->y);
            me->moveState = MoveState::IDLE;
        }

        // 새 방향이 있으면 이동 시작 (벽 방향은 차단)
        if (newDir != Direction::NONE &&
            !_playerManager.IsBlockedByWall(me->x, me->y, newDir))
        {
            _network.SendMoveStart(static_cast<uint8_t>(newDir), me->x, me->y);
            me->direction = newDir;
            me->moveState = MoveState::MOVING;
        }
    }

}

// ==========================================================================
// 키보드 입력 처리 — 채팅 모드
// ==========================================================================

void CGameInstance::ProcessChatInput()
{
    HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);
    INPUT_RECORD record;
    DWORD numEvents;

    while (PeekConsoleInputW(hInput, &record, 1, &numEvents) && numEvents > 0)
    {
        ReadConsoleInputW(hInput, &record, 1, &numEvents);

        // 키 눌림 이벤트만 처리
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
            continue;

        WORD vk = record.Event.KeyEvent.wVirtualKeyCode;
        wchar_t ch = record.Event.KeyEvent.uChar.UnicodeChar;

        if (vk == VK_ESCAPE) // ESC → 채팅 취소
        {
            _chatMode = false;
            _chatInput.clear();
            _escPressed = true; // 채팅 취소 후 ESC 자동반복에 의한 게임 종료 방지
            return;
        }

        if (vk == VK_RETURN) // Enter → 전송
        {
            if (!_chatInput.empty())
            {
                if (_chatInput[0] == L'/')
                {
                    HandleChatCommand(_chatInput);
                }
                else
                {
                    _network.SendChat(_chatInput.c_str());
                }
            }
            _chatMode = false;
            _chatInput.clear();
            return;
        }

        if (vk == VK_BACK) // Backspace
        {
            if (!_chatInput.empty())
            {
                _chatInput.pop_back();
            }
            continue;
        }

        // 일반 문자 입력 (유니코드) — 글자 수만 제한, 표시는 렌더러에서 스크롤
        if (ch >= 32 && _chatInput.size() < CHAT_MSG_MAX_LEN - 1)
        {
            _chatInput += ch;
        }
    }
}

// ==========================================================================
// S2C 패킷 핸들러 — 이벤트 큐에 Push (수신 스레드에서 호출)
// ==========================================================================

void CGameInstance::OnZoneInfo(const MSG_S2C_ZONE_INFO* msg)
{
    ClientNetworkEvent event{};
    event.type = ClientNetworkEvent::Type::ZONE_INFO;
    event.mapId = msg->mapId;
    event.channelIndex = msg->channelIndex;
    event.mapWidth = msg->mapWidth;
    event.mapHeight = msg->mapHeight;
    event.sectorSize = msg->sectorSize;
    _eventQueue.Push(std::move(event));
}

void CGameInstance::OnCreateMyPlayer(const MSG_S2C_CREATE_MY_PLAYER* msg)
{
    ClientNetworkEvent event{};
    event.type = ClientNetworkEvent::Type::CREATE_MY_PLAYER;
    event.playerId = msg->playerId;
    event.displayChar = msg->displayChar;
    event.colorIndex = msg->colorIndex;
    event.x = msg->x;
    event.y = msg->y;
    event.direction = msg->direction;
    event.speed = msg->speed;
    _eventQueue.Push(std::move(event));
}

void CGameInstance::OnCreateOtherPlayer(const MSG_S2C_CREATE_OTHER_PLAYER* msg)
{
    ClientNetworkEvent event{};
    event.type = ClientNetworkEvent::Type::CREATE_OTHER_PLAYER;
    event.playerId = msg->playerId;
    event.displayChar = msg->displayChar;
    event.colorIndex = msg->colorIndex;
    event.x = msg->x;
    event.y = msg->y;
    event.direction = msg->direction;
    event.moveState = msg->moveState;
    event.spawnReason = msg->spawnReason;
    event.speed = msg->speed;
    _eventQueue.Push(std::move(event));
}

void CGameInstance::OnDeletePlayer(const MSG_S2C_DELETE_PLAYER* msg)
{
    ClientNetworkEvent event{};
    event.type = ClientNetworkEvent::Type::DELETE_PLAYER;
    event.playerId = msg->playerId;
    _eventQueue.Push(std::move(event));
}

void CGameInstance::OnMoveStart(const MSG_S2C_MOVE_START* msg)
{
    ClientNetworkEvent event{};
    event.type = ClientNetworkEvent::Type::MOVE_START;
    event.playerId = msg->playerId;
    event.x = msg->x;
    event.y = msg->y;
    event.direction = msg->direction;
    _eventQueue.Push(std::move(event));
}

void CGameInstance::OnMoveStop(const MSG_S2C_MOVE_STOP* msg)
{
    ClientNetworkEvent event{};
    event.type = ClientNetworkEvent::Type::MOVE_STOP;
    event.playerId = msg->playerId;
    event.x = msg->x;
    event.y = msg->y;
    event.direction = msg->direction;
    _eventQueue.Push(std::move(event));
}

void CGameInstance::OnChat(const MSG_S2C_CHAT* msg)
{
    ClientNetworkEvent event{};
    event.type = ClientNetworkEvent::Type::CHAT;
    event.playerId = msg->playerId;
    event.displayChar = msg->displayChar;
    event.colorIndex = msg->colorIndex;

    // 가변 길이: header.size 기반으로 메시지 글자 수 역산
    uint16_t msgBytes = msg->header.size - static_cast<uint16_t>(offsetof(MSG_S2C_CHAT, message));
    uint16_t msgLen = msgBytes / sizeof(ChatChar);   // 글자 수는 와이어 문자 폭으로 나눈다
    if (msgLen > CHAT_MSG_MAX_LEN - 1)
        msgLen = CHAT_MSG_MAX_LEN - 1;
    // 와이어 문자 타입(ChatChar) → 클라 내부 표현(wchar_t). 폭이 같다고 가정하지 않고 원소별로 옮긴다.
    for (uint16_t i = 0; i < msgLen; ++i)
        event.chatMessage[i] = static_cast<wchar_t>(msg->message[i]);
    event.chatMessage[msgLen] = L'\0';

    _eventQueue.Push(std::move(event));
}

void CGameInstance::OnSyncPosition(const MSG_S2C_SYNC_POSITION* msg)
{
    ClientNetworkEvent event{};
    event.type = ClientNetworkEvent::Type::SYNC_POSITION;
    event.playerId = msg->playerId;
    event.x = msg->x;
    event.y = msg->y;
    _eventQueue.Push(std::move(event));
}

// 섹터 묶음(USE_SECTOR_AGGREGATION): 한 섹터의 이번 틱 변경분.
// 엔트리마다 최종 상태를 기존 MOVE_START/STOP 이벤트로 분해해 큐에 넣는다(소비측·이벤트타입 재사용).
// 가변 길이라 header.size로 실제 엔트리 수를 역산해 상한 방어(서버 비정상 시 오버런 차단 — 더미 HandleSectorUpdates와 동일).
void CGameInstance::OnSectorUpdates(const MSG_S2C_SECTOR_UPDATES* msg)
{
    uint16_t count = msg->count;
    uint16_t avail = static_cast<uint16_t>(
        (msg->header.size - offsetof(MSG_S2C_SECTOR_UPDATES, entries)) / sizeof(SectorUpdateEntry));
    if (count > avail)
        count = avail;

    for (uint16_t i = 0; i < count; ++i)
    {
        const SectorUpdateEntry& e = msg->entries[i];

        // moveState: 0=IDLE → MOVE_STOP, 그 외=MOVING → MOVE_START (두 이벤트 모두 x/y/direction 사용)
        // 좌표·방향·이동상태는 엔트리에 눈금과 니블로 담겨 오므로 여기서 원래 값으로 되돌린다.
        ClientNetworkEvent event{};
        event.type      = (UnpackState(e.dirState) != 0)
            ? ClientNetworkEvent::Type::MOVE_START
            : ClientNetworkEvent::Type::MOVE_STOP;
        event.playerId  = e.playerId;
        event.x         = DequantizePos(e.qx);
        event.y         = DequantizePos(e.qy);
        event.direction = UnpackDir(e.dirState);
        _eventQueue.Push(std::move(event));
    }
}

// 멤버십 인바운드 묶음(USE_MEMBERSHIP_INBOUND_BUNDLE): 시야 진입 상대 N명이 배치 1패킷으로 온다.
// 엔트리마다 기존 CREATE_OTHER_PLAYER 이벤트로 분해해 큐에 넣는다(소비측·이벤트타입 재사용 — OnCreateOtherPlayer와 동일 필드).
// 가변 길이라 header.size로 실제 엔트리 수를 역산해 상한 방어(OnSectorUpdates와 동일).
void CGameInstance::OnCreatePlayerBatch(const MSG_S2C_CREATE_PLAYER_BATCH* msg)
{
    uint16_t count = msg->count;
    uint16_t avail = static_cast<uint16_t>(
        (msg->header.size - offsetof(MSG_S2C_CREATE_PLAYER_BATCH, entries)) / sizeof(CreatePlayerBatchEntry));
    if (count > avail)
        count = avail;

    for (uint16_t i = 0; i < count; ++i)
    {
        const CreatePlayerBatchEntry& e = msg->entries[i];

        ClientNetworkEvent event{};
        event.type = ClientNetworkEvent::Type::CREATE_OTHER_PLAYER;
        event.playerId = e.playerId;
        event.displayChar = e.displayChar;
        event.colorIndex = e.colorIndex;
        event.x = e.x;
        event.y = e.y;
        event.direction = e.direction;
        event.moveState = e.moveState;
        event.spawnReason = e.spawnReason;
        event.speed = e.speed;
        _eventQueue.Push(std::move(event));
    }
}

// 멤버십 인바운드 묶음: 시야 이탈 상대 N명 — 엔트리별 기존 DELETE_PLAYER 이벤트로 분해.
void CGameInstance::OnDeletePlayerBatch(const MSG_S2C_DELETE_PLAYER_BATCH* msg)
{
    uint16_t count = msg->count;
    uint16_t avail = static_cast<uint16_t>(
        (msg->header.size - offsetof(MSG_S2C_DELETE_PLAYER_BATCH, entries)) / sizeof(DeletePlayerBatchEntry));
    if (count > avail)
        count = avail;

    for (uint16_t i = 0; i < count; ++i)
    {
        ClientNetworkEvent event{};
        event.type = ClientNetworkEvent::Type::DELETE_PLAYER;
        event.playerId = msg->entries[i].playerId;
        _eventQueue.Push(std::move(event));
    }
}

void CGameInstance::OnZoneChangeOk(const MSG_S2C_ZONE_CHANGE_OK* msg)
{
    ClientNetworkEvent event{};
    event.type = ClientNetworkEvent::Type::ZONE_CHANGE_OK;
    event.playerId = msg->playerId;
    event.displayChar = msg->displayChar;
    event.colorIndex = msg->colorIndex;
    event.direction = msg->direction;
    event.x = msg->x;
    event.y = msg->y;
    event.mapId = msg->mapId;
    event.channelIndex = msg->channelIndex;
    _eventQueue.Push(std::move(event));
}

void CGameInstance::OnZoneChangeFail(const MSG_S2C_ZONE_CHANGE_FAIL* msg)
{
    ClientNetworkEvent event{};
    event.type = ClientNetworkEvent::Type::ZONE_CHANGE_FAIL;
    event.reason = msg->reason;
    _eventQueue.Push(std::move(event));
}

void CGameInstance::OnError(const MSG_S2C_ERROR* msg)
{
    ClientNetworkEvent event{};
    event.type = ClientNetworkEvent::Type::ERROR_MSG;
    strncpy_s(event.errorMessage, msg->message, 255);
    _eventQueue.Push(std::move(event));
}

void CGameInstance::OnAdminLoginOk()
{
    ClientNetworkEvent event{};
    event.type = ClientNetworkEvent::Type::ADMIN_LOGIN_OK;
    _eventQueue.Push(std::move(event));
}

void CGameInstance::OnAdminLoginFail()
{
    ClientNetworkEvent event{};
    event.type = ClientNetworkEvent::Type::ADMIN_LOGIN_FAIL;
    _eventQueue.Push(std::move(event));
}

// ==========================================================================
// 채팅 로그 관리
// ==========================================================================

void CGameInstance::AddChatMessage(const std::wstring& msg)
{
    // 콘솔 표시 폭(78자) 기준으로 줄바꿈 분할
    static constexpr size_t LINE_WIDTH = 78;
    if (msg.size() <= LINE_WIDTH)
    {
        _chatLog.push_back(msg);
    }
    else
    {
        for (size_t pos = 0; pos < msg.size(); pos += LINE_WIDTH)
            _chatLog.push_back(msg.substr(pos, LINE_WIDTH));
    }

    // 최대 라인 수 초과 시 오래된 메시지 제거
    while (static_cast<int>(_chatLog.size()) > MAX_CHAT_LOG)
    {
        _chatLog.erase(_chatLog.begin());
    }
}

// ==========================================================================
// 채팅 커맨드 처리
// ==========================================================================

void CGameInstance::HandleChatCommand(const std::wstring& command)
{
    if (command == L"/map" || command == L"/map list")
    {
        AddChatMessage(L"[System] Usage: /map <id> | /map random");
        return;
    }

    if (command.size() > 5 && command.substr(0, 5) == L"/map ")
    {
        std::wstring arg = command.substr(5);

        if (arg == L"random")
        {
            // 이동 중이면 정지 (이전 존에서 유령 이동 방지)
            ClientPlayer* me = _playerManager.GetMyPlayer();
            if (me && me->moveState == MoveState::MOVING)
            {
                _network.SendMoveStop(
                    static_cast<uint8_t>(me->direction), me->x, me->y);
                me->moveState = MoveState::IDLE;
            }

            _network.SendZoneChange(-1);
            _zoneChanging = true;
            _zoneChangeElapsedMs = 0;
            AddChatMessage(L"[System] Requesting random zone change...");
            return;
        }

        try
        {
            int32_t mapId = std::stoi(arg);

            // 이동 중이면 정지 (이전 존에서 유령 이동 방지)
            ClientPlayer* me = _playerManager.GetMyPlayer();
            if (me && me->moveState == MoveState::MOVING)
            {
                _network.SendMoveStop(
                    static_cast<uint8_t>(me->direction), me->x, me->y);
                me->moveState = MoveState::IDLE;
            }

            _network.SendZoneChange(mapId);
            _zoneChanging = true;
            _zoneChangeElapsedMs = 0;

            wchar_t buf[128];
            swprintf_s(buf, L"[System] Requesting zone change to map %d...", mapId);
            AddChatMessage(buf);
        }
        catch (...)
        {
            AddChatMessage(L"[System] Invalid map ID. Usage: /map <id> | /map random");
        }
        return;
    }

    // ── 채널 이동 ──

    if (command == L"/ch" || command == L"/ch list")
    {
        AddChatMessage(L"[System] Usage: /ch <channel_number>");
        return;
    }

    if (command.size() > 4 && command.substr(0, 4) == L"/ch ")
    {
        std::wstring arg = command.substr(4);

        try
        {
            int32_t channelIndex = std::stoi(arg);

            // 이동 중이면 정지 (이전 존에서 유령 이동 방지)
            ClientPlayer* me = _playerManager.GetMyPlayer();
            if (me && me->moveState == MoveState::MOVING)
            {
                _network.SendMoveStop(
                    static_cast<uint8_t>(me->direction), me->x, me->y);
                me->moveState = MoveState::IDLE;
            }

            int32_t currentMapId = _renderer.GetMapId();
            _network.SendZoneChange(currentMapId, channelIndex);
            _zoneChanging = true;
            _zoneChangeElapsedMs = 0;

            wchar_t buf[128];
            swprintf_s(buf, L"[System] Requesting channel change to ch %d...", channelIndex);
            AddChatMessage(buf);
        }
        catch (...)
        {
            AddChatMessage(L"[System] Invalid channel number. Usage: /ch <channel_number>");
        }
        return;
    }

    // ── 운영자 인증 ──

    if (command.size() > 7 && command.substr(0, 7) == L"/admin ")
    {
        std::wstring arg = command.substr(7);

        // wchar_t → char 변환 (ASCII key)
        char key[ADMIN_KEY_MAX_LEN]{};
        for (size_t i = 0; i < arg.size() && i < ADMIN_KEY_MAX_LEN - 1; ++i)
            key[i] = static_cast<char>(arg[i]);

        _network.SendAdminLogin(key);
        AddChatMessage(L"[System] Requesting admin login...");
        return;
    }

    AddChatMessage(L"[System] Unknown command. Available: /map, /ch, /admin");
}
