#include "CharacterServer.h"

#include <Common/ErrorLog.h>
#include <Link/SessionTag.h>
#include <Protocol/CharacterProtocol.h>
#include <Protocol/ServerLinkProtocol.h>
#include <sstream>
#include <string>
#include <vector>

#include "Platform/Platform.h"

namespace
{
    // 코어 값 중 설정에 안 내놓는 것 — 계정 서버와 같은 값이다. 로비 서버는 부하를 받는 자리가 아니라 재 본 적이 없다
    constexpr int SEND_WORKERS = 2;
    constexpr int RIO_WORKERS = 2;
    constexpr int COMPLETION_BATCH = 0;
    constexpr int SEND_DEPTH = 1;
    constexpr int64_t SWEEP_PERIOD_MS = 1000;   // 장부 정리 · 게이지 갱신 주기 — 근거 없이 고른 값

    std::vector<std::string> OpNames()
    {
        std::vector<std::string> names;
        for (int i = 0; i < CCharacterDb::OP_COUNT; ++i)
            names.emplace_back(CCharacterDb::OpName(static_cast<CCharacterDb::Op>(i)));
        return names;
    }

    CLinkConnector::Config AccountLink(const ServerLinkConfig& link)
    {
        CLinkConnector::Config config;
        config.peerName = "account";
        config.ip = link.account.ip;
        config.port = link.account.port;
        config.retrySec = link.retrySec;
        config.heartbeatSec = link.heartbeatSec;
        return config;
    }
}

CCharacterServer::CCharacterServer(const CharacterServerConfig& config, const ServerLinkConfig& link)
    : _config(config)
    , _link(link)
    , _network(config.lobby.port, config.lobby.maxClients + link.sessionReserve, ServerMode::GameServer, _monitor,
               config.lobby.workerThreads, SEND_WORKERS, RIO_WORKERS, COMPLETION_BATCH, SEND_DEPTH)
    , _db(config.lobby.db, config.lobby.dbWorkers, config.lobby.dbQueueMax, OpNames())
    , _loop(_network, _monitor, config.lobby.logicPeriodMs)
    , _account(_network, _monitor, AccountLink(link))
    , _peers(_network, _monitor, link.heartbeatSec)
    , _requests(link.replyTimeoutSec)
    , _loginFlow(_network, _monitor, _db, _sessions, _account, _requests, _metrics, config.deleteGraceDays)
    , _rosterFlow(_network, _monitor, _db, _sessions, _zones, _jobs, _looks, _metrics, config.characterSlots, config.deleteGraceDays)
    , _selectFlow(_network, _monitor, _db, _sessions, _zones, _peers, _requests, _metrics)
    , _handoffFlow(_monitor, _peers, _zones, _requests)
    , _zoneFlow(_monitor, _peers, _zones, _requests)
    , _webTable(CHAR_PACKET_FIRST, CHAR_PACKET_LAST)
    , _accountTable(LINK_PACKET_FIRST, LINK_PACKET_LAST)
    , _zoneTable(LINK_PACKET_FIRST, LINK_PACKET_LAST)
    , _coreMetrics(_monitor)
    , _endpoint(config.lobby.monitorPort, [this]() { return BuildMetricsText(); })
{
}

bool CCharacterServer::LoadTables()
{
    const std::string dir = Platform::GetExecutableDir();
    if (!_jobs.Load(dir + "JobTable.csv"))
    {
        SLOG_ERROR("[CharacterServer] JobTable.csv 를 못 읽음");
        return false;
    }
    if (!_looks.Load(dir + "LookTable.csv"))
    {
        SLOG_ERROR("[CharacterServer] LookTable.csv 를 못 읽음");
        return false;
    }
    return true;
}

// 로그인을 마치지 않은 세션이 C2S_CHAR_LOGIN 말고 다른 패킷을 보내면 오류 + 끊기(플랜 6절)
CPacketTable::Handler CCharacterServer::LoggedInOnly(CPacketTable::Handler handler)
{
    return [this, handler = std::move(handler)](SessionId id, CSerialBuffer* msg)
    {
        const CCharSessionTable::Entry* entry = _sessions.Find(id);
        if (entry == nullptr)
        {
            _monitor._packetErrors.Inc();
            return;
        }
        switch (entry->stage)
        {
        case CCharSessionTable::Stage::LoggedIn:
            handler(id, msg);
            break;
        case CCharSessionTable::Stage::Replaced:
            break;   // 새 창에 밀려 끊기는 중 — 끊김이 오기 전에 도착한 패킷이다
        case CCharSessionTable::Stage::Fresh:
        case CCharSessionTable::Stage::Verifying:
            _monitor._packetErrors.Inc();
            _network.RequestDisconnectSession(ToInt(id));
            break;
        }
    };
}

bool CCharacterServer::RegisterRoutes()
{
    bool ok = _webTable.Register<C2S_CHAR_LOGIN>([this](SessionId id, CSerialBuffer* msg) { _loginFlow.OnLogin(id, msg); });
    ok = _webTable.Register<C2S_CHAR_CREATE>(LoggedInOnly([this](SessionId id, CSerialBuffer* msg) { _rosterFlow.OnCreate(id, msg); })) && ok;
    ok = _webTable.Register<C2S_CHAR_DELETE>(LoggedInOnly([this](SessionId id, CSerialBuffer* msg) { _rosterFlow.OnDelete(id, msg); })) && ok;
    ok = _webTable.Register<C2S_CHAR_RESTORE>(LoggedInOnly([this](SessionId id, CSerialBuffer* msg) { _rosterFlow.OnRestore(id, msg); })) && ok;
    ok = _webTable.Register<C2S_CHAR_SELECT>(LoggedInOnly([this](SessionId id, CSerialBuffer* msg) { _selectFlow.OnSelect(id, msg); })) && ok;
    ok = _webTable.Register<C2S_CHAR_HEARTBEAT>(LoggedInOnly([](SessionId, CSerialBuffer*) {})) && ok;   // 받는 것만으로 코어가 세션 시한을 늘린다

    ok = _accountTable.Register<LINK_HEARTBEAT>([](SessionId, CSerialBuffer*) {}) && ok;
    ok = _accountTable.Register<LINK_TOKEN_CHECK_ACK>([this](SessionId, CSerialBuffer* msg) { _loginFlow.OnTokenAck(msg); }) && ok;

    ok = _zoneTable.Register<LINK_HEARTBEAT>([](SessionId, CSerialBuffer*) {}) && ok;
    ok = _zoneTable.Register<LINK_ZONE_REGISTER>([this](SessionId id, CSerialBuffer* msg) { _zoneFlow.OnRegister(id, msg); }) && ok;
    ok = _zoneTable.Register<LINK_ZONE_ONLINE_LIST>([this](SessionId id, CSerialBuffer* msg) { _zoneFlow.OnOnlineList(id, msg); }) && ok;
    ok = _zoneTable.Register<LINK_ENTER_NOTICE_ACK>([this](SessionId id, CSerialBuffer* msg) { _zoneFlow.OnNoticeAck(id, msg); }) && ok;
    ok = _zoneTable.Register<LINK_ZONE_CHAR_ENTER>([this](SessionId id, CSerialBuffer* msg) { _zoneFlow.OnCharEnter(id, msg); }) && ok;
    ok = _zoneTable.Register<LINK_ZONE_CHAR_LEAVE>([this](SessionId id, CSerialBuffer* msg) { _zoneFlow.OnCharLeave(id, msg); }) && ok;
    ok = _zoneTable.Register<LINK_HANDOFF_REQ>([this](SessionId id, CSerialBuffer* msg) { _handoffFlow.OnRequest(id, msg); }) && ok;
    return ok;
}

bool CCharacterServer::Start()
{
    if (!LoadTables())
        return false;

    if (!RegisterRoutes())
    {
        SLOG_ERROR("[CharacterServer] 처리표 등록 실패 — 번호대 밖이거나 같은 번호를 두 번 등록했다");
        return false;
    }

    if (!_db.Start())
    {
        SLOG_ERROR("[CharacterServer] DB 연결 실패");
        return false;
    }

    _network.SetUntaggedSessionLimit(_config.lobby.maxClients);   // 웹 세션이 여유 칸을 못 먹게 — 서버 간 연결 자리가 늘 남는다
    if (!_network.AddListener(_link.character.ip.c_str(), _link.character.port, static_cast<int>(SessionTag::LinkDown)))
    {
        SLOG_ERROR("[CharacterServer] 서버 간 포트 주소가 틀림: {}:{}", _link.character.ip, _link.character.port);
        _db.Stop();
        return false;
    }
    if (!_network.Start())
    {
        SLOG_ERROR("[CharacterServer] 코어 시작 실패");
        _db.Stop();
        return false;
    }
    _started = true;

    _loop.SetRoute(SessionTag::Web, &_webTable,
        [this](SessionId id) { _sessions.Add(id); },
        [this](SessionId id) { _sessions.Remove(id); });
    _loop.SetRoute(SessionTag::LinkUp, &_accountTable,
        [this](SessionId id) { _account.OnConnected(id); },
        [this](SessionId id) { _account.OnDisconnected(id); });
    _loop.SetRoute(SessionTag::LinkDown, &_zoneTable,
        [this](SessionId id) { _peers.OnConnected(id); },
        [this](SessionId id) { _peers.OnDisconnected(id); });
    _loop.SetDb(&_db);
    _loop.SetPeriodic([this](int64_t nowMs) { Periodic(nowMs); });

    _account.SetHandlers(
        [](SessionId id) { SLOG_INFO("[CharacterServer] 계정 서버 연결됨 (session={})", ToInt(id)); },
        [this]()
        {
            SLOG_WARN("[CharacterServer] 계정 서버 끊김");
            _requests.FailAll(LinkRequestKind::TokenCheck);   // 시한까지 기다리지 않는다
        });
    _peers.SetHandlers(
        [](SessionId id) { SLOG_INFO("[CharacterServer] 존 서버 접속 (session={})", ToInt(id)); },
        [this](SessionId id)
        {
            SLOG_WARN("[CharacterServer] 존 서버 끊김 (session={})", ToInt(id));
            _zoneFlow.OnDisconnected(id);
        });

    _loop.Start();
    _account.Start();

    if (!_endpoint.Start())
        SLOG_WARN("[CharacterServer] 지표 포트 {} 를 못 열었다 — 서버는 그대로 돈다", _config.lobby.monitorPort);

    SLOG_INFO("[CharacterServer] Started - web {} · link {}:{} · metrics {}",
              _config.lobby.port, _link.character.ip, _link.character.port, _config.lobby.monitorPort);
    return true;
}

void CCharacterServer::Stop()
{
    if (!_started)
        return;
    _started = false;

    _endpoint.Stop();
    _account.Stop();       // 코어를 내리기 전에 — 종료 중 세션 표를 건드리지 않게(코어 약속)
    _loop.Stop();          // 받을 쪽부터 — 남은 이벤트의 버퍼를 반납한다
    _db.Stop();            // 남은 작업을 끝낸다. 회신은 안 꺼낸다
    _network.ShutdownServer();
}

void CCharacterServer::Periodic(int64_t nowMs)
{
    _account.Tick(nowMs);
    _peers.Tick(nowMs);
    _requests.ExpireDue(nowMs);

    if (nowMs >= _nextSweepMs)
    {
        _nextSweepMs = nowMs + SWEEP_PERIOD_MS;
        _zones.Sweep(nowMs);
        _metrics.SetOnline(_zones.OnlineCount());
        _metrics.SetZones(_zones.ZoneCount());
    }
}

std::string CCharacterServer::BuildMetricsText()
{
    std::ostringstream ss;
    _coreMetrics.Write(ss);
    _metrics.Write(ss);
    _db.WriteMetrics(ss);
    _account.WriteMetrics(ss);
    _requests.WriteMetrics(ss);
    return ss.str();
}
