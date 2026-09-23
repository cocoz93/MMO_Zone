#include "DummyClient.h"
#include <Windows.h>
#include <cstring>
#include <cstddef>  // offsetof
#include "MMOStats.h"
#include "MMOStressConfig.h"

// ─────────────────────────────────────────────────────────────────
// 유틸
// ─────────────────────────────────────────────────────────────────
int64_t DummyClient::NowMs()
{
    // QueryPerformanceCounter 기반 ms. GetTickCount64는 분해능이 15~16ms라
    // (timeBeginPeriod(1)로도 안 바뀜 — 실측 확인) RTT가 0/15/16ms로 양자화돼
    // 히스토그램 하위 버킷이 무의미해진다. QPC는 syscall이 아니라 비용도 낮다.
    static const int64_t s_freq = []() -> int64_t {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);   // Win7+ 항상 성공
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    // 곱셈 먼저로 ms 정밀도 보존 (부팅 이후 단조 증가, 상대 비교만 사용)
    return c.QuadPart * 1000 / s_freq;
}

void DummyClient::CloseSocket()
{
    if (_sock != INVALID_SOCKET)
    {
        closesocket(_sock);
        _sock = INVALID_SOCKET;
    }
}

void DummyClient::ResetState()
{
    _ready          = false;
    _playerId       = 0;
    _x              = 0.0f;
    _y              = 0.0f;
    _speed          = 0;
    _direction      = 0;
    _moving         = false;
    _lastTickMs      = 0;
    _lastHeartbeatMs = 0;
    _chatSentMs      = 0;
    _lastRttMs       = -1;
    _lastRecvMs      = 0;   // 재접속 세션의 첫 OnRecv가 "끊겨 있던 시간"을 gap으로 세지 않도록
    _recvBuf.Clear();
    _sendBuf.Clear();
}

void DummyClient::Disconnect(StatsLocal& stats, int reconnectDelayMs)
{
    if (_state == ClientState::CONNECTED)
    {
        stats.connectedDelta -= 1;
        if (_ready)
            stats.readyDelta -= 1;
    }

    CloseSocket();
    _state          = ClientState::DISCONNECTED;
    _connectReadyMs = NowMs() + reconnectDelayMs;
    ResetState();
}

bool DummyClient::IsReadyToConnect(int64_t nowMs) const
{
    return _state == ClientState::DISCONNECTED && nowMs >= _connectReadyMs;
}

// ─────────────────────────────────────────────────────────────────
// 연결
// ─────────────────────────────────────────────────────────────────
void DummyClient::StartConnect(const std::string& ip, int port,
                               StatsLocal& stats, int reconnectDelayMs)
{
    _sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_sock == INVALID_SOCKET)
    {
        stats.connectFail += 1;
        _connectReadyMs = NowMs() + reconnectDelayMs;
        return;
    }

    // 논블로킹 모드
    u_long mode = 1;
    ioctlsocket(_sock, FIONBIO, &mode);

    // 소켓 옵션
    int bufSize = 65536;
    setsockopt(_sock, SOL_SOCKET, SO_SNDBUF,    (char*)&bufSize, static_cast<int>(sizeof(bufSize)));
    setsockopt(_sock, SOL_SOCKET, SO_RCVBUF,    (char*)&bufSize, static_cast<int>(sizeof(bufSize)));
    int noDelay = 1;
    setsockopt(_sock, IPPROTO_TCP, TCP_NODELAY, (char*)&noDelay, static_cast<int>(sizeof(noDelay)));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    int result = connect(_sock, reinterpret_cast<sockaddr*>(&addr),
                         static_cast<int>(sizeof(addr)));
    int err    = WSAGetLastError();

    if (result == 0 || err == WSAEWOULDBLOCK || err == WSAEINPROGRESS)
    {
        _state = ClientState::CONNECTING;
        _connectStartMs = NowMs();
    }
    else
    {
        stats.connectFail += 1;
        CloseSocket();
        _connectReadyMs = NowMs() + reconnectDelayMs;
    }
}

void DummyClient::OnConnected(StatsLocal& stats, int reconnectDelayMs)
{
    // SO_ERROR 확인 (writable이어도 실패일 수 있음)
    int sockErr = 0;
    int optLen  = static_cast<int>(sizeof(sockErr));
    getsockopt(_sock, SOL_SOCKET, SO_ERROR, (char*)&sockErr, &optLen);

    if (sockErr != 0)
    {
        OnConnectFailed(stats, reconnectDelayMs);
        return;
    }

    _state = ClientState::CONNECTED;
    stats.connectedDelta += 1;
    stats.connectTotal += 1;
    ResetState();

    // 입장 요청 — 접속 직후 한 번. 서버가 이걸 받아야 캐릭터를 불러온다(USE_LOGIN).
    //   ResetState 가 송신 버퍼를 비우므로 반드시 그 뒤에 넣는다.
    SendEnterGame(stats);
}

void DummyClient::OnConnectFailed(StatsLocal& stats, int reconnectDelayMs)
{
    stats.connectFail += 1;
    CloseSocket();
    _state          = ClientState::DISCONNECTED;
    _connectReadyMs = NowMs() + reconnectDelayMs;
}

// ─────────────────────────────────────────────────────────────────
// 수신
// ─────────────────────────────────────────────────────────────────
void DummyClient::OnRecv(StatsLocal& stats, int reconnectDelayMs)
{
    // 오버플로 진단: 갱신 "전" 값이 직전 수신 시각이다(0 = 이 세션의 첫 수신 → 경과 0으로 둔다).
    // _lastRecvMs 갱신 시점 자체는 옛 코드와 같아 RTT 계산(HandleChat)에 영향 없다.
    const int64_t nowMs = NowMs();
    const int64_t gapMs = (_lastRecvMs > 0) ? (nowMs - _lastRecvMs) : 0;
    _lastRecvMs = nowMs;

    char buf[4096];
    int64_t chunkBytes = 0;   // 이번 OnRecv가 커널에서 꺼낸 총 바이트
    int64_t recvCalls  = 0;   // 이번 OnRecv의 recv 호출 횟수

    // WOULDBLOCK까지 반복 수신 — 커널 버퍼에 4096B 이상 쌓여 있을 때 대응
    while (true)
    {
        int bytes = recv(_sock, buf, static_cast<int>(sizeof(buf)), 0);
        recvCalls += 1;

        if (bytes > 0)
        {
            stats.recvBytes += bytes;
            chunkBytes += bytes;   // 링 적재 실패분도 "커널에서 꺼낸 양"이므로 포함
            if (_recvBuf.Enqueue(buf, static_cast<size_t>(bytes)) == 0)
            {
                // 링버퍼 오버플로우
                stats.recvBufferOverflow += 1;
                stats.ovfGapMsSum  += gapMs;
                stats.ovfBytesSum  += chunkBytes;
                stats.ovfRecvCalls += recvCalls;
                stats.RecordRecvChunk(gapMs, chunkBytes);
                Disconnect(stats, reconnectDelayMs);
                return;
            }
            continue;
        }

        stats.RecordRecvChunk(gapMs, chunkBytes);

        if (bytes == 0)
        {
            stats.disconnectFromServer += 1;
            Disconnect(stats, reconnectDelayMs);
            return;
        }

        // bytes < 0 (SOCKET_ERROR)
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return;  // 정상: 더 이상 읽을 데이터 없음

        stats.disconnectFromServer += 1;
        Disconnect(stats, reconnectDelayMs);
        return;
    }
}

// ─────────────────────────────────────────────────────────────────
// 패킷 크기 테이블
// ─────────────────────────────────────────────────────────────────
uint16_t DummyClient::GetPacketSize(MsgType type)
{
    switch (type)
    {
    case MsgType::S2C_ZONE_INFO:           return sizeof(MSG_S2C_ZONE_INFO);
    case MsgType::S2C_CREATE_MY_PLAYER:    return sizeof(MSG_S2C_CREATE_MY_PLAYER);
    case MsgType::S2C_CREATE_OTHER_PLAYER: return sizeof(MSG_S2C_CREATE_OTHER_PLAYER);
    case MsgType::S2C_DELETE_PLAYER:       return sizeof(MSG_S2C_DELETE_PLAYER);
    case MsgType::S2C_MOVE_START:          return sizeof(MSG_S2C_MOVE_START);
    case MsgType::S2C_MOVE_STOP:           return sizeof(MSG_S2C_MOVE_STOP);
    case MsgType::S2C_CHAT:               return static_cast<uint16_t>(offsetof(MSG_S2C_CHAT, message) + sizeof(ChatChar)); // 가변 길이: 최소 크기
    case MsgType::S2C_SECTOR_UPDATES:     return static_cast<uint16_t>(offsetof(MSG_S2C_SECTOR_UPDATES, entries));        // 가변 길이: 최소 = header+count
    case MsgType::S2C_CREATE_PLAYER_BATCH: return static_cast<uint16_t>(offsetof(MSG_S2C_CREATE_PLAYER_BATCH, entries));  // 가변 길이: 최소 = header+count
    case MsgType::S2C_DELETE_PLAYER_BATCH: return static_cast<uint16_t>(offsetof(MSG_S2C_DELETE_PLAYER_BATCH, entries));  // 가변 길이: 최소 = header+count
    case MsgType::S2C_SYNC_POSITION:      return sizeof(MSG_S2C_SYNC_POSITION);
    case MsgType::S2C_ZONE_CHANGE_OK:     return sizeof(MSG_S2C_ZONE_CHANGE_OK);
    case MsgType::S2C_ZONE_CHANGE_FAIL:   return sizeof(MSG_S2C_ZONE_CHANGE_FAIL);
    // USE_COMBAT: 전투 패킷 — 더미는 내용을 쓰지 않지만 크기는 알아야 한다.
    //   모르는 타입이면 아래 default가 0을 돌려주고, 0은 packetParseFail → Disconnect라
    //   전투 패킷이 섹터에 뿌려지는 순간 더미가 전부 떨어진다.
    case MsgType::S2C_ATTACK:             return sizeof(MSG_S2C_ATTACK);
    case MsgType::S2C_DAMAGE:             return sizeof(MSG_S2C_DAMAGE);
    case MsgType::S2C_DEATH:              return sizeof(MSG_S2C_DEATH);
    case MsgType::S2C_RESPAWN:            return sizeof(MSG_S2C_RESPAWN);
    case MsgType::S2C_ACTOR_STATS:        return sizeof(MSG_S2C_ACTOR_STATS);
    // USE_EXP: 경험치·레벨 — 위 전투 패킷과 같은 이유로 크기만 등록한다.
    //   ★ 더미는 몬스터를 안 잡지만 이 패킷을 반드시 받는다 — 스폰 직후 본인에게 한 장 간다.
    //     빠뜨리면 전투가 없어도 더미 전원이 접속하자마자 떨어진다.
    case MsgType::S2C_EXP:                return sizeof(MSG_S2C_EXP);
    default: return 0;
    }
}

// ─────────────────────────────────────────────────────────────────
// 패킷 파싱
// ─────────────────────────────────────────────────────────────────
void DummyClient::ProcessPackets(StatsLocal& stats, const MMOStressConfig& config)
{
    static constexpr size_t MAX_PACKET_SIZE = 2048;

    while (true)
    {
        if (_recvBuf.GetDataSize() < sizeof(MsgHeader)) break;

        MsgHeader hdr;
        if (_recvBuf.Peek(&hdr, sizeof(MsgHeader)) == 0) break;

        uint16_t totalSize = hdr.size;

        // 알려진 패킷인지 확인 (최소 크기 검증 — 채팅 등 가변 길이 대응)
        uint16_t expected = GetPacketSize(hdr.type);
        if (expected == 0 || totalSize < expected)
        {
            // 알 수 없는 패킷 타입 또는 크기 부족 → 연결 종료
            stats.packetParseFail += 1;
            Disconnect(stats, config.reconnectIntervalMs);
            return;
        }

        // 상한 검증 — 서버 버그로 비정상 크기 수신 시 스택 오버플로 방지
        if (totalSize > MAX_PACKET_SIZE)
        {
            stats.packetParseFail += 1;
            Disconnect(stats, config.reconnectIntervalMs);
            return;
        }

        if (_recvBuf.GetDataSize() < totalSize) break;

        char packet[MAX_PACKET_SIZE];
        if (_recvBuf.Dequeue(packet, totalSize) == 0) break;

        stats.recvPackets += 1;

        switch (hdr.type)
        {
        case MsgType::S2C_CREATE_MY_PLAYER:
        {
            bool wasReady = _ready;
            HandleCreateMyPlayer(packet);
            if (_ready && !wasReady) stats.readyDelta += 1;
            break;
        }
        case MsgType::S2C_MOVE_START:          HandleMoveStart(packet);         break;
        case MsgType::S2C_MOVE_STOP:           HandleMoveStop(packet);          break;
        case MsgType::S2C_SYNC_POSITION:       HandleSyncPosition(packet);      break;
        case MsgType::S2C_CREATE_OTHER_PLAYER: HandleCreateOtherPlayer(packet); break;
        case MsgType::S2C_DELETE_PLAYER:       HandleDeletePlayer(packet);      break;
        case MsgType::S2C_CHAT:
            HandleChat(packet);
            if (_lastRttMs >= 0)
            {
                stats.RecordRtt(_lastRttMs);
                _lastRttMs = -1;
            }
            break;
        case MsgType::S2C_SECTOR_UPDATES:      HandleSectorUpdates(packet);     break;
        case MsgType::S2C_CREATE_PLAYER_BATCH:                                  break;  // 읽고 버림 (더미는 타인 미추적 — HandleCreateOtherPlayer와 동일)
        case MsgType::S2C_DELETE_PLAYER_BATCH:                                  break;  // 읽고 버림
        case MsgType::S2C_ZONE_CHANGE_OK:      HandleZoneChangeOk(packet);      break;
        case MsgType::S2C_ZONE_CHANGE_FAIL:
            HandleZoneChangeFail(packet);
            stats.zoneChangeFail += 1;
            break;
        case MsgType::S2C_ZONE_INFO:
            HandleZoneInfo(packet);
            break;
        default: break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────
// 패킷 핸들러
// ─────────────────────────────────────────────────────────────────
void DummyClient::HandleZoneInfo(const char* packet)
{
    auto* msg = reinterpret_cast<const MSG_S2C_ZONE_INFO*>(packet);
    _mapWidth = msg->mapWidth;
    _mapHeight = msg->mapHeight;
}

void DummyClient::HandleCreateMyPlayer(const char* packet)
{
    auto* msg = reinterpret_cast<const MSG_S2C_CREATE_MY_PLAYER*>(packet);
    _playerId = msg->playerId;
    _x        = msg->x;
    _y        = msg->y;
    _speed    = msg->speed;
    _ready    = true;
}

void DummyClient::HandleMoveStart(const char* /*packet*/)
{
    // 타 플레이어 이동 시작 통보 — 더미는 추적하지 않으므로 읽고 버림.
    // (RTT는 채팅 왕복으로 측정한다: 서버가 MOVE_START를 발신자 본인에게는
    //  보내지 않아 자기 echo 기반 측정이 불가능하기 때문)
}

void DummyClient::HandleMoveStop(const char* packet)
{
    // 서버 권위 정지 통보: 본인 것만 좌표/상태 동기화
    // (경계 클램핑 등으로 서버가 IDLE 처리한 경우 더미도 _moving을 꺼야
    //  로컬 예측이 서버와 어긋나지 않는다)
    auto* msg = reinterpret_cast<const MSG_S2C_MOVE_STOP*>(packet);
    if (msg->playerId != _playerId)
        return;  // 타 플레이어 정지는 더미가 추적하지 않음 → 무시
    _x      = msg->x;
    _y      = msg->y;
    _moving = false;
}

void DummyClient::HandleSyncPosition(const char* packet)
{
    auto* msg = reinterpret_cast<const MSG_S2C_SYNC_POSITION*>(packet);
    if (msg->playerId != _playerId)
        return;
    _x = msg->x;
    _y = msg->y;
}

void DummyClient::HandleCreateOtherPlayer(const char* /*packet*/)
{
    // 읽고 버림
}

void DummyClient::HandleDeletePlayer(const char* /*packet*/)
{
    // 읽고 버림
}

void DummyClient::HandleChat(const char* packet)
{
    // RTT 측정: 서버는 발신자 본인에게도 S2C_CHAT(playerId=자기)을 돌려준다
    // (RecvChat의 excludeSelf=false). 보낸 시각과의 차이가 채팅 왕복 RTT.
    auto* msg = reinterpret_cast<const MSG_S2C_CHAT*>(packet);
    if (msg->playerId == _playerId && _chatSentMs > 0)
    {
        int64_t rtt = _lastRecvMs - _chatSentMs;  // OnRecv 배치 수신 시각 기준
        if (rtt < 0) rtt = 0;
        _chatSentMs = 0;
        _lastRttMs  = rtt;  // stats 접근 가능한 ProcessPackets에서 RecordRtt
    }
}

void DummyClient::HandleSectorUpdates(const char* packet)
{
    // 섹터 묶음: count개 엔트리 중 본인 것만 좌표/상태 동기화 (타 플레이어는 더미가 추적 안 함).
    // 가변 길이라 header.size로 실제 엔트리 수를 역산해 상한 방어(서버 비정상 시 오버런 차단).
    auto* msg = reinterpret_cast<const MSG_S2C_SECTOR_UPDATES*>(packet);

    uint16_t count = msg->count;
    uint16_t avail = static_cast<uint16_t>(
        (msg->header.size - offsetof(MSG_S2C_SECTOR_UPDATES, entries)) / sizeof(SectorUpdateEntry));
    if (count > avail)
        count = avail;

    for (uint16_t i = 0; i < count; ++i)
    {
        const SectorUpdateEntry& e = msg->entries[i];
        if (e.playerId == _playerId)
        {
            _x      = DequantizePos(e.qx);   // 눈금 → 좌표 (엔트리는 좌표를 uint16 눈금으로 싣는다)
            _y      = DequantizePos(e.qy);
            _moving = (UnpackState(e.dirState) != 0);
        }
    }
}

void DummyClient::HandleZoneChangeOk(const char* packet)
{
    auto* msg = reinterpret_cast<const MSG_S2C_ZONE_CHANGE_OK*>(packet);
    _playerId = msg->playerId;
    _x        = msg->x;
    _y        = msg->y;
    _moving   = false;
}

void DummyClient::HandleZoneChangeFail(const char* packet)
{
    (void)packet;
    // zoneChangeFail 카운터는 ProcessPackets의 switch에서 증가
}

// ─────────────────────────────────────────────────────────────────
// 이동 로직
// ─────────────────────────────────────────────────────────────────
void DummyClient::UpdateLocalPosition(float deltaTime, int mapWidth, int mapHeight)
{
    // 실측 경과시간 기반 예측 — 고정 0.04 대신 실제 틱 간격을 사용해야
    // 서버(실측 deltaTime)와 이동 속도가 일치한다 (틱 게이트 분해능/부하로 인한
    // 체계적 좌표 지연 방지)
    float dist = static_cast<float>(_speed) * deltaTime;

    switch (_direction)
    {
    case 1: /* UP    */ _y -= dist; break;
    case 2: /* DOWN  */ _y += dist; break;
    case 3: /* LEFT  */ _x -= dist; break;
    case 4: /* RIGHT */ _x += dist; break;
    default: break;
    }

    // 맵 경계 클램핑 (서버 Zone::Tick과 동일)
    float maxX = static_cast<float>(mapWidth)  - 1.0f;
    float maxY = static_cast<float>(mapHeight) - 1.0f;
    if (_x < 0.0f) _x = 0.0f;
    if (_x >= static_cast<float>(mapWidth))  _x = maxX;
    if (_y < 0.0f) _y = 0.0f;
    if (_y >= static_cast<float>(mapHeight)) _y = maxY;
}

void DummyClient::SendMoveStart(StatsLocal& stats)
{
    // 랜덤 방향 (1~4)
    std::uniform_int_distribution<int> dist(1, 4);
    _direction = static_cast<uint8_t>(dist(_rng));
    _moving = true;

    MSG_C2S_MOVE_START msg;
    msg.header.size = sizeof(msg);
    msg.header.type = MsgType::C2S_MOVE_START;
    msg.direction   = _direction;
    msg.x           = _x;
    msg.y           = _y;

    if (_sendBuf.Enqueue(&msg, sizeof(msg)) == 0)
    {
        _moving = false;
        stats.sendBufferFull += 1;
        return;
    }
    stats.sendPackets += 1;
    stats.moveStartSent += 1;
}

void DummyClient::SendMoveStop(StatsLocal& stats)
{
    _moving = false;

    MSG_C2S_MOVE_STOP msg;
    msg.header.size = sizeof(msg);
    msg.header.type = MsgType::C2S_MOVE_STOP;
    msg.direction   = _direction;
    msg.x           = _x;
    msg.y           = _y;

    if (_sendBuf.Enqueue(&msg, sizeof(msg)) == 0)
    {
        _moving = true;
        stats.sendBufferFull += 1;
        return;
    }
    stats.sendPackets += 1;
    stats.moveStopSent += 1;
}

void DummyClient::SendHeartbeat(StatsLocal& stats)
{
    MSG_C2S_HEARTBEAT msg;
    msg.header.size = sizeof(msg);
    msg.header.type = MsgType::C2S_HEARTBEAT;

    if (_sendBuf.Enqueue(&msg, sizeof(msg)) == 0)
    {
        stats.sendBufferFull += 1;
        return;
    }
    stats.sendPackets += 1;
    stats.heartbeatSent += 1;
}

void DummyClient::SendEnterGame(StatsLocal& stats)
{
    // 계정 번호 0 = "번호를 지정하지 않음" — 서버가 접속마다 새 번호를 붙인다.
    //   지정 번호를 쓰면 더미 전부가 "같은 계정 찾기"를 타게 돼 부하 구성이 달라진다.
    //   ※ 조회는 0 으로 와도 그대로 나간다 — 자동 번호는 재시작마다 1부터 다시 세는데
    //     DB 행은 남아 있어서, 새 번호가 지난 회차의 행과 겹칠 수 있기 때문이다.
    MSG_C2S_ENTER_GAME msg{};   // 입장권 64바이트를 0 으로 채우려면 값 초기화가 필요하다
    msg.header.size = sizeof(msg);
    msg.header.type = MsgType::C2S_ENTER_GAME;
    msg.accountId   = 0;

    if (_sendBuf.Enqueue(&msg, sizeof(msg)) == 0)
    {
        stats.sendBufferFull += 1;
        return;
    }
    stats.sendPackets += 1;
}

// MMO 더미 채팅 문장 풀
// 패킷 크기 = MsgHeader(4B) + (글자수+1)*2B, 길이별 균등 분포 (~10~128B)
static const wchar_t* s_chatMessages[] = {
    // --- ~10~16B (2~5글자) ---
    L"ㅎㅇ",
    L"ㅎㅎ",
    L"ㄱㄱ",
    L"ㅇㅋ",
    L"ㅁㅊ",
    L"ㄷㄷ",
    L"대박",
    L"미쳤다",
    // --- ~14~18B (4~6글자) ---
    L"ㅋㅋㅋㅋ",
    L"버프 좀",
    L"딜 세다",
    L"파티 구함",
    L"결투 ㄱ?",
    L"아 죽었다",
    L"마을 가야함",
    L"힐러 어디감",
    L"레벨업 축하",
    L"탱커 구해요",
    // --- ~20~26B (7~10글자) ---
    L"스태프 팝니다",
    L"강화석 삽니다",
    L"수고하셨습니다",
    L"보스 언제 젠?",
    L"점검 언제임?",
    L"여기 트랩 있음",
    L"포션 다 떨어짐",
    L"길드전 몇시임?",
    L"부활 어디서 함?",
    L"던전 같이 갈 사람",
    // --- ~28~46B (11~20글자) ---
    L"사냥터 어디가 좋음?",
    L"오늘 접속률 왜 이래",
    L"+9 검 얼마에 팔아요?",
    L"이 퀘스트 어떻게 깸?",
    L"경험치 얼마 남았어?",
    L"나 이만 간다 내일 보자",
    L"다음에 또 같이 하자 재밌었다",
    L"이번 보스 공략 영상 봤어?",
    L"강화 시도했는데 또 실패함 강화석 다 날아갔다",
    L"길드 레이드 시간 정했으면 공지 올려주세요",
    // --- ~48~76B (21~35글자) ---
    L"지금 서버 사람 너무 많아서 사냥터 자리 잡기 힘들다",
    L"오늘 업데이트 내용 봤어? 신규 던전 추가됐다고 하던데",
    L"다음 주에 대규모 업데이트 있다던데 패치 내용 아는 사람?",
    L"아이템 강화 확률이 왜 이렇게 낮은 거야 세 번 연속 실패",
    L"보스 패턴 모르는 사람 있으면 미리 말해주세요",
    // --- ~78~106B (36~50글자) ---
    L"이번 이벤트 보상이 진짜 좋다 매일 접속만 해도 전설 장비 뽑기권 준다",
    L"PvP 시즌 언제 끝나요? 랭크 좀 더 올리고 싶은데 시간이 없어서 걱정이다",
    L"탱커가 어그로 놓치면 힐러부터 죽으니까 제발 도발기 쿨타임 잘 봐주세요",
    L"서버 점검 끝나면 바로 접속해서 한정 아이템 사야 하는데 알람 맞춰놔야겠다",
    L"길드 가입 조건이 어떻게 되나요? 레벨이랑 장비 기준 좀 알려주시면 감사하겠습니다",
    // --- ~108~128B (51~61글자) ---
    L"보스전 시작하기 전에 버프 먼저 다 걸고 갑시다 힐러는 뒤에서 탱커 위주로 힐 부탁드려요",
    L"던전 입장 전에 포션이랑 버프 스크롤 충분히 챙겨오세요 보스가 세 페이즈까지 있어서 장기전 각오하셔야 합니다",
    L"파티 구성은 탱커 한 명 힐러 두 명 딜러 세 명으로 가겠습니다 부족한 역할 있으면 지금 말씀해주세요 바로 구하겠습니다",
};
static constexpr int s_chatMessageCount = _countof(s_chatMessages);

void DummyClient::SendChat(StatsLocal& stats, int64_t nowMs)
{
    std::uniform_int_distribution<int> dist(0, s_chatMessageCount - 1);
    const wchar_t* text = s_chatMessages[dist(_rng)];
    int len = static_cast<int>(wcslen(text));

    MSG_C2S_CHAT msg{};
    msg.header.type = MsgType::C2S_CHAT;

    // 와이어 문자 타입(ChatChar)으로 옮겨 담는다 — 문장 풀은 클라 내부 표현(wchar_t)이라
    //   경계에서 변환한다. 폭이 같다고 가정하지 않으려고 원소별로 옮긴다.
    if (len > CHAT_MSG_MAX_LEN - 1)
        len = CHAT_MSG_MAX_LEN - 1;
    for (int i = 0; i < len; ++i)
        msg.message[i] = static_cast<ChatChar>(text[i]);
    msg.message[len] = u'\0';

    uint16_t sendSize = static_cast<uint16_t>(
        sizeof(MsgHeader) + (len + 1) * sizeof(ChatChar));
    msg.header.size = sendSize;

    if (_sendBuf.Enqueue(&msg, sendSize) == 0)
    {
        stats.sendBufferFull += 1;
        return;
    }
    // RTT 프로브 무장(single outstanding): 미측정 중이거나, 직전 프로브가
    // 응답 없이 오래 묵었으면(존 이동/유실) 재무장해 측정이 멈추지 않게 한다.
    if (_chatSentMs == 0 || (nowMs - _chatSentMs) > RTT_PROBE_TIMEOUT_MS)
        _chatSentMs = nowMs;
    stats.sendPackets += 1;
    stats.chatSent += 1;
}

void DummyClient::SendZoneChange(StatsLocal& stats, int targetMapId)
{
    MSG_C2S_ZONE_CHANGE msg;
    msg.header.size  = sizeof(msg);
    msg.header.type  = MsgType::C2S_ZONE_CHANGE;
    msg.targetMapId  = targetMapId;
    msg.targetChannelIndex = -1;  // 자동배정

    if (_sendBuf.Enqueue(&msg, sizeof(msg)) == 0)
    {
        stats.sendBufferFull += 1;
        return;
    }
    stats.sendPackets += 1;
    stats.zoneChangeSent += 1;
}

void DummyClient::Tick(StatsLocal& stats, const MMOStressConfig& config, int64_t nowMs)
{
    if (!_ready) return;

    // tickIntervalMs(40ms) 도달 체크
    if (nowMs - _lastTickMs < config.tickIntervalMs)
        return;
    // _lastTickMs를 덮어쓰기 전에 실제 경과시간을 확보 (예측 deltaTime)
    float deltaTime = static_cast<float>(nowMs - _lastTickMs) / 1000.0f;
    _lastTickMs = nowMs;

    // 이동 중이면 좌표 갱신
    if (_moving)
        UpdateLocalPosition(deltaTime, _mapWidth, _mapHeight);

    // 랜덤 행동 결정
    std::uniform_int_distribution<int> roll(1, 100);
    int r = roll(_rng);

    if (_moving)
    {
        // 이동 중: stopProbability% 확률로 정지
        if (r <= config.stopProbability)
            SendMoveStop(stats);
    }
    else
    {
        // 정지 중: 확률 구간 분배
        int cursor = 0;

        cursor += config.moveProbability;
        if (r <= cursor)
        {
            SendMoveStart(stats);
            return;
        }

        cursor += config.chatProbability;
        if (r <= cursor)
        {
            SendChat(stats, nowMs);
            return;
        }

        cursor += config.zoneChangeProbability;
        if (r <= cursor)
        {
            SendZoneChange(stats, config.targetMapId);
            return;
        }

        // 나머지: idle
    }
}

void DummyClient::CheckHeartbeat(StatsLocal& stats, int heartbeatIntervalSec, int64_t nowMs)
{
    if (!_ready) return;

    if (nowMs - _lastHeartbeatMs >= static_cast<int64_t>(heartbeatIntervalSec) * 1000)
    {
        _lastHeartbeatMs = nowMs;
        SendHeartbeat(stats);
    }
}

// ─────────────────────────────────────────────────────────────────
// 송신 링버퍼 flush
// ─────────────────────────────────────────────────────────────────
void DummyClient::FlushSend(StatsLocal& stats, int reconnectDelayMs)
{
    if (!IsConnected()) return;

    while (_sendBuf.GetDataSize() > 0)
    {
        size_t directSize = _sendBuf.GetDirectReadSize();
        if (directSize == 0) break;

        int sent = send(_sock, _sendBuf.GetReadPtr(), static_cast<int>(directSize), 0);

        if (sent == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                return;
            stats.sendError += 1;
            Disconnect(stats, reconnectDelayMs);
            return;
        }

        if (sent == 0) return;

        stats.sendBytes += sent;
        _sendBuf.Consume(static_cast<size_t>(sent));
    }
}
