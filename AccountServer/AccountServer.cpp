#include "AccountServer.h"

#include <Common/ErrorLog.h>
#include <Link/SessionTag.h>
#include <Lobby/PacketSend.h>
#include <Protocol/AccountProtocol.h>
#include <Protocol/ServerLinkProtocol.h>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // 코어 값 중 설정에 안 내놓는 것 — 시험 서버와 같은 값이다. 로비 서버는 부하를 받는 자리가 아니라 재 본 적이 없다
    constexpr int SEND_WORKERS = 2;
    constexpr int RIO_WORKERS = 2;
    constexpr int COMPLETION_BATCH = 0;
    constexpr int SEND_DEPTH = 1;
    constexpr int64_t SWEEP_PERIOD_MS = 1000;   // 장부 정리 · 게이지 갱신 주기 — 근거 없이 고른 값

    std::vector<std::string> OpNames()
    {
        std::vector<std::string> names;
        for (int i = 0; i < CAccountDb::OP_COUNT; ++i)
            names.emplace_back(CAccountDb::OpName(static_cast<CAccountDb::Op>(i)));
        return names;
    }
}

CAccountServer::CAccountServer(const AccountServerConfig& config, const ServerLinkConfig& link)
    : _config(config)
    , _link(link)
    , _network(config.lobby.port, config.lobby.maxClients + link.sessionReserve, ServerMode::GameServer, _monitor,
               config.lobby.workerThreads, SEND_WORKERS, RIO_WORKERS, COMPLETION_BATCH, SEND_DEPTH)
    , _db(config.lobby.db, config.lobby.dbWorkers, config.lobby.dbQueueMax, OpNames())
    , _loop(_network, _monitor, config.lobby.logicPeriodMs)
    , _peers(_network, _monitor, link.heartbeatSec)
    , _ledger(config.authTokenTtlSec)
    , _loginFlow(_network, _monitor, _db, _ledger, _metrics, config.allowDevLogin)
    , _tokenFlow(_peers, _monitor, _ledger, _metrics)
    , _mailFlow(_peers, _monitor, _db, _metrics, _worldSession, config.mailExpireDays)
    , _webTable(ACCOUNT_PACKET_FIRST, ACCOUNT_PACKET_LAST)
    , _linkTable(LINK_PACKET_FIRST, LINK_PACKET_LAST)
    , _coreMetrics(_monitor)
    , _endpoint(config.lobby.monitorPort, [this]() { return BuildMetricsText(); })
{
}

bool CAccountServer::RegisterRoutes()
{
    bool ok = _webTable.Register<C2S_ACCOUNT_LOGIN>([this](SessionId id, CSerialBuffer* msg) { _loginFlow.OnLogin(id, msg); });
    ok = _linkTable.Register<LINK_HEARTBEAT>([](SessionId, CSerialBuffer*) {}) && ok;   // 받는 것만으로 코어가 세션 시한을 늘린다
    ok = _linkTable.Register<LINK_TOKEN_CHECK_REQ>([this](SessionId id, CSerialBuffer* msg) { _tokenFlow.OnCheck(id, msg); }) && ok;
    ok = _linkTable.Register<LINK_WORLD_REGISTER>([this](SessionId id, CSerialBuffer* msg) { OnWorldRegister(id, msg); }) && ok;
    // 계정 우편 — 월드 서버가 존 서버의 요청을 번호만 바꿔 넘긴다(mail-plan.md 9절)
    ok = _linkTable.Register<LINK_ACCMAIL_COUNT_REQ>([this](SessionId id, CSerialBuffer* msg) { _mailFlow.OnCount(id, msg); }) && ok;
    ok = _linkTable.Register<LINK_ACCMAIL_LIST_REQ>([this](SessionId id, CSerialBuffer* msg) { _mailFlow.OnList(id, msg); }) && ok;
    ok = _linkTable.Register<LINK_ACCMAIL_READ_REQ>([this](SessionId id, CSerialBuffer* msg) { _mailFlow.OnRead(id, msg); }) && ok;
    ok = _linkTable.Register<LINK_ACCMAIL_CLAIM_REQ>([this](SessionId id, CSerialBuffer* msg) { _mailFlow.OnClaim(id, msg); }) && ok;
    ok = _linkTable.Register<LINK_ACCMAIL_DONE>([this](SessionId id, CSerialBuffer* msg) { _mailFlow.OnDone(id, msg); }) && ok;
    ok = _linkTable.Register<LINK_ACCMAIL_UNCLAIM>([this](SessionId id, CSerialBuffer* msg) { _mailFlow.OnUnclaim(id, msg); }) && ok;
    ok = _linkTable.Register<LINK_ACCMAIL_ERASE_REQ>([this](SessionId id, CSerialBuffer* msg) { _mailFlow.OnErase(id, msg); }) && ok;
    ok = _linkTable.Register<LINK_ACCMAIL_SEND_REQ>([this](SessionId id, CSerialBuffer* msg) { _mailFlow.OnSend(id, msg); }) && ok;
    return ok;
}

void CAccountServer::OnWorldRegister(SessionId sessionId, CSerialBuffer* msg)
{
    LINK_WORLD_REGISTER packet;
    if (!PacketSend::Read(msg, packet))
    {
        _monitor._packetErrors.Inc();
        return;
    }
    // 월드 서버가 재시작한 뒤 옛 연결이 코어 세션 시한까지 남아 있어도 새 연결이 바로 맡는다
    if (_worldSession != NO_SESSION && _worldSession != sessionId)
    {
        SLOG_WARN("[AccountServer] 월드 서버가 다시 등록 — 옛 연결을 끊음 (session={})", ToInt(_worldSession));
        _peers.Disconnect(_worldSession);
    }
    _worldSession = sessionId;
    SLOG_INFO("[AccountServer] 월드 서버 {} 등록 (session={})", packet.worldId, ToInt(sessionId));
}

bool CAccountServer::Start()
{
    if (!RegisterRoutes())
    {
        SLOG_ERROR("[AccountServer] 처리표 등록 실패 — 번호대 밖이거나 같은 번호를 두 번 등록했다");
        return false;
    }

    if (!_db.Start())
    {
        SLOG_ERROR("[AccountServer] DB 연결 실패");
        return false;
    }

    _network.SetUntaggedSessionLimit(_config.lobby.maxClients);   // 웹 세션이 여유 칸을 못 먹게 — 서버 간 연결 자리가 늘 남는다
    if (!_network.AddListener(_link.account.ip.c_str(), _link.account.port, static_cast<int>(SessionTag::LinkDown)))
    {
        SLOG_ERROR("[AccountServer] 서버 간 포트 주소가 틀림: {}:{}", _link.account.ip, _link.account.port);
        _db.Stop();
        return false;
    }
    if (!_network.Start())
    {
        SLOG_ERROR("[AccountServer] 코어 시작 실패");
        _db.Stop();
        return false;
    }
    _started = true;

    _loop.SetRoute(SessionTag::Web, &_webTable, nullptr, nullptr);
    _loop.SetRoute(SessionTag::LinkDown, &_linkTable,
        [this](SessionId id) { _peers.OnConnected(id); },
        [this](SessionId id) { _peers.OnDisconnected(id); });
    _loop.SetDb(&_db);
    _loop.SetPeriodic([this](int64_t nowMs) { Periodic(nowMs); });
    _peers.SetHandlers(
        [](SessionId id) { SLOG_INFO("[AccountServer] 서버 간 연결 접속 (session={})", ToInt(id)); },
        [this](SessionId id)
        {
            SLOG_WARN("[AccountServer] 서버 간 연결 끊김 (session={})", ToInt(id));
            if (_worldSession == id)
                _worldSession = NO_SESSION;
        });
    _loop.Start();

    if (!_endpoint.Start())
        SLOG_WARN("[AccountServer] 지표 포트 {} 를 못 열었다 — 서버는 그대로 돈다", _config.lobby.monitorPort);

    SLOG_INFO("[AccountServer] Started - web {} · link {}:{} · metrics {}",
              _config.lobby.port, _link.account.ip, _link.account.port, _config.lobby.monitorPort);
    return true;
}

void CAccountServer::Stop()
{
    if (!_started)
        return;
    _started = false;

    _endpoint.Stop();
    _loop.Stop();          // 받을 쪽부터 — 남은 이벤트의 버퍼를 반납한다
    _db.Stop();            // 남은 로그인 작업을 끝낸다. 회신은 안 꺼낸다
    _network.ShutdownServer();
}

void CAccountServer::Periodic(int64_t nowMs)
{
    _peers.Tick(nowMs);
    _loginFlow.Tick(nowMs);

    if (nowMs >= _nextSweepMs)
    {
        _nextSweepMs = nowMs + SWEEP_PERIOD_MS;
        _ledger.Sweep(nowMs);
        _metrics.SetPending(_ledger.LiveCount());
    }
}

std::string CAccountServer::BuildMetricsText()
{
    std::ostringstream ss;
    _coreMetrics.Write(ss);
    _metrics.Write(ss);
    _db.WriteMetrics(ss);
    return ss.str();
}
