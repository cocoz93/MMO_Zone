#include "CharacterStore.h"

#if USE_DB_WORKER

#include "SessionHub.h"
#include "Player.h"
#include "MapManager.h"         // CMapManager::GetMapIdFromZoneId — 저장 잡의 맵 번호
#include "GameMonitor.h"
#include "GameDataFiles.h"      // 스킬 표 · 아이템 표
#include "BuffSystem.h"         // USE_COMBAT — 지속 효과 칸 되살리기(Restore)
#include "CooldownBuffBlob.h"   // 쿨타임·버프 열의 바이트 형식(토글을 안 보는 헤더)
#include <Common/ErrorLog.h>
#include <cassert>
#include <chrono>

// ==========================================================================
// 캐릭터 저장소 — GameServer.cpp 에서 옮김(몸·주석 그대로 · 게임 서버 나누기 4 단계).
//   바뀐 것: 회신 꺼내기가 불러오기 결과를 알림(_loadEvents)으로 넘김 · 되살리기가 입장 배치 구조체 대신 쓰는 두 칸만 받음.
// ==========================================================================

// ==========================================================================
// DB 저장 파이프라인 (dirty flag 기반 비동기 위치 저장)
// ==========================================================================

bool CCharacterStore::Init(const DBConfig& dbConfig, int savePeriodSec, int workerCount, int queueMax)
{
    // 세 값은 INI 필수(코드에 기본값 없음) — 빠지면 조용히 다른 값으로 도는 대신 서버를 안 띄운다
    if (savePeriodSec <= 0 || workerCount <= 0 || queueMax <= 0)
    {
        SLOG_ERROR("[DB] INI [DB] SavePeriodSec·Workers·QueueMax 는 1 이상이어야 합니다 (save={}, workers={}, qmax={})",
                   savePeriodSec, workerCount, queueMax);
        return false;
    }

    _dbWorker = std::make_unique<CDBWorker>(_gameMonitor._db);   // 저장 파이프라인 지표는 게임 그릇(10-06 코어에서 옮김)
#if USE_INVENTORY
    _dbWorker->SetItemSaveCounters(&_gameMonitor._dbItem);   // Start 전에 — 워커 스레드가 뜨기 전에 자리를 알려 둔다
#endif
    if (!_dbWorker->Start(dbConfig, workerCount, queueMax))
    {
        SLOG_ERROR("[DB] InitDB failed - check MySQL connection/config");
        _dbWorker.reset();
        return false;
    }
    _dbSavePeriodFrames = savePeriodSec * FRAME_PER_SEC;
    _dbSaveFrameCounter = 0;
    return true;
}

#if USE_SKILL || USE_COMBAT   // 쿨타임·버프 칸이 하나도 없는 빌드는 아래 셋을 안 쓴다(쓰지 않는 static 함수 경고)
// 실제 시각(UTC 밀리초) — 쿨타임·버프를 DB 에 남기고 되살릴 때만 쓴다. 접속하지 않은 동안에도 흘러야 해서 틱으로는 못 센다.
//   게임 안의 시간은 여전히 틱(_frameCount)이다 — 이 값은 「틱 ↔ 실제 시각」을 바꾸는 기준점일 뿐이다.
static int64_t WallNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 틱 ↔ ms — 명목 틱(FRAME_INTERVAL_MS)으로 바꾼다. 접속 중 쿨타임·버프는 틱으로 흐르므로 틱 기준과 맞춘다
//   (실측 틱이 명목보다 조금 길어도 그 몇 %는 무해 — 저장 사이에 끝 시각이 조금 밀릴 뿐이다).
//   ms → 틱은 올림이다(쿨타임 표 읽기와 같은 까닭 — 짧아지지 않게).
static int64_t FramesToMs(uint64_t frames) { return static_cast<int64_t>(frames) * FRAME_INTERVAL_MS; }
static uint64_t MsToFramesCeil(int64_t ms) { return static_cast<uint64_t>((ms + FRAME_INTERVAL_MS - 1) / FRAME_INTERVAL_MS); }
#endif

// 형식의 최대 길이가 DB 쪽 버퍼에 들어가야 한다 — 스킬·버프 종류를 늘리면 여기서 막힌다(DBWorker.h 의 숫자를 올릴 것)
#if USE_SKILL
static_assert(CooldownBuffBlob::CooldownsMaxBytes(MAX_SKILL_ID) <= DB_SKILL_COOLDOWNS_BYTES, "스킬 칸을 늘렸으면 DBWorker.h 의 DB_SKILL_COOLDOWNS_BYTES 를 올릴 것");
static_assert(MAX_SKILL_ID <= 255, "스킬 번호를 1바이트에 싣는다(CooldownBuffBlob)");
#endif
#if USE_COMBAT
static_assert(CooldownBuffBlob::BuffsMaxBytes(BUFF_KIND_COUNT) <= DB_BUFFS_BYTES, "버프 종류를 늘렸으면 DBWorker.h 의 DB_BUFFS_BYTES 를 올릴 것");

// 되살릴 버프의 상한 — 지금 버프를 거는 쪽은 스킬뿐이라 스킬 표 읽기의 상한과 같은 값이다(CSkillTable::MAX_BUFF_MUL · MAX_COOLDOWN_MS).
//   버프 칸은 전투 토글 소유라 스킬을 끈 빌드에도 있다 — 그래서 표 클래스 대신 숫자로 두고, 스킬 빌드에선 static_assert 가 둘을 맞춘다.
constexpr int32_t BUFF_RESTORE_MAX_MUL       = 10;
constexpr int64_t BUFF_RESTORE_MAX_REMAIN_MS = 3600000;   // 1시간 — 스킬 표의 지속 상한
#if USE_SKILL
static_assert(BUFF_RESTORE_MAX_MUL == CSkillTable::MAX_BUFF_MUL && BUFF_RESTORE_MAX_REMAIN_MS == CSkillTable::MAX_COOLDOWN_MS,
              "스킬 표의 상한을 바꿨으면 되살리기 상한도 맞출 것");
#endif

// 되살릴 버프 값이 그 종류에 맞나 — 종류마다 뜻이 달라 switch 로 전수한다(종류를 늘리면 static_assert 가 여기로 부른다).
static bool IsRestorableBuffValue(BuffKind kind, int32_t value)
{
    static_assert(BUFF_KIND_COUNT == 1, "BuffKind 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
    switch (kind)
    {
    case BuffKind::ATTACK_MUL: return value >= 2 && value <= BUFF_RESTORE_MAX_MUL;   // 스킬 표 읽기와 같은 범위(1 은 아무 일도 안 한다)
    case BuffKind::COUNT:      break;
    }
    return false;
}
#endif

bool CCharacterStore::ShouldSave(const CPlayer* player) const
{
    // 저장 대상 판정(주기 저장만 — 나갈 때·종료는 이 판정 없이 쓴다):
    //   MOVING   = 위치가 계속 변하는 중 → 저장 후 dirty가 리셋돼도 매 주기 재저장 필요
    //   _dbDirty = 정지/접속/존변경/클램프로 생긴 미저장 변경(일회성, 저장 후 리셋)
    //   값 비교  = 레벨·경험치·체력·마력·쿨타임·버프 칸이 마지막으로 실은 값(_dbSaved)과 다름 — 바뀌는 자리마다 켜지 않고 여기서 본다(Player.h)
    if (player->_moveState == MoveState::MOVING || player->_dbDirty)
        return true;
    if (CooldownsAndBuffsChanged(player))
        return true;

    // 저장할 값과 같은 식으로 견준다(MakeSaveJob) — 토글을 끈 빌드의 「모름(-1)」도 똑같이 나와 헛저장이 없다.
    const DBSaveJob now = MakeSaveJob(player);
    const CPlayer::DbSavedValues& saved = player->_dbSaved;
    return now.level != saved.level || now.exp != saved.exp || now.hp != saved.hp || now.mp != saved.mp;
}

DBSaveJob CCharacterStore::MakeSaveJob(const CPlayer* player) const
{
    DBSaveJob job;
    job.accountId = player->_accountId;
    job.x         = player->_x;
    job.y         = player->_y;
    job.mapId     = CMapManager::GetMapIdFromZoneId(player->_zoneId);
    job.level     = player->_level;
    job.exp       = player->_exp;
    // 체력·마력 — 값을 세우는 코드가 없는 빌드에선 「모름」을 싣는다. 0 을 실으면 다음 입장이 죽은 채 저장된 행으로 읽는다.
#if USE_COMBAT
    job.hp        = player->_hp;
#else
    job.hp        = DB_HP_MP_UNSAVED;
#endif
#if USE_SKILL
    job.mp        = player->_mp;
#else
    job.mp        = DB_HP_MP_UNSAVED;
#endif
    PackCooldownsAndBuffs(player, job);   // 쿨타임·버프 — 토글을 끈 빌드는 빈 값
    return job;
}

void CCharacterStore::MarkSaveQueued(CPlayer* player, const DBSaveJob& job)
{
    // 큐에 들어간 잡을 기록한다 — 다음 주기 판정(ShouldSave)이 이 값과 견준다.
    //   백프레셔로 버려지면 부르는 쪽이 _dbDirty 를 되살린다(TickPeriodicSave) — dirty 가 서 있으면 값 비교와 무관하게 다시 실린다.
    player->_dbDirty         = false;
    player->_dbSaved.level   = job.level;
    player->_dbSaved.exp     = job.exp;
    player->_dbSaved.hp      = job.hp;
    player->_dbSaved.mp      = job.mp;
    // 쿨타임·버프는 잡의 바이트가 아니라 칸(틱)을 사본으로 남긴다 — 같은 틱에 만든 잡이라 내용은 같다(까닭은 CooldownsAndBuffsChanged)
    RecordSavedCooldownsAndBuffs(player);
}

// ── 쿨타임·버프 ↔ DB 묶음 ─────────────────────────────────────────────────
//   칸은 틱으로 들고(CCharacter::_skillReadyFrame · _buffs), DB 엔 실제 시각으로 묶어 넣는다(CooldownBuffBlob.h).
//   저장 = 돌고 있는 칸만 「끝나는 실제 시각」으로 · 입장 = 지난 것은 버리고 남은 몫을 틱으로.

void CCharacterStore::PackCooldownsAndBuffs(const CPlayer* player, DBSaveJob& job) const
{
    job.skillCooldowns.len = 0;
    job.buffs.len          = 0;
#if USE_SKILL || USE_COMBAT
    const int64_t nowMs = WallNowMs();
#endif

#if USE_SKILL
    // 다시 쓸 수 있는 틱이 지금보다 뒤인 칸만 — 0(한 번도 안 씀)·지난 칸은 돌고 있지 않다(CSkillSystem 의 판정 frame < ready 와 같은 경계)
    CooldownBuffBlob::Cooldown cooldowns[MAX_SKILL_ID];
    int nCooldown = 0;
    for (int id = 1; id <= MAX_SKILL_ID; ++id)
    {
        const uint64_t ready = player->_skillReadyFrame[id];
        if (ready <= _frameCount)
            continue;
        cooldowns[nCooldown++] = { static_cast<uint8_t>(id), nowMs + FramesToMs(ready - _frameCount) };
    }
    job.skillCooldowns.len = static_cast<unsigned long>(
        CooldownBuffBlob::PackCooldowns(cooldowns, nCooldown, job.skillCooldowns.data, sizeof(job.skillCooldowns.data)));
#endif

#if USE_COMBAT
    // 켜져 있고 끝나는 틱이 지금보다 뒤인 칸만 — 이번 틱의 만료 훑기(CBuffSystem::Tick) 전에 이미 끝난 칸은 뺀다(Tick 의 경계 frame >= expire)
    CooldownBuffBlob::Buff buffs[BUFF_KIND_COUNT];
    int nBuff = 0;
    for (int k = 0; k < BUFF_KIND_COUNT; ++k)
    {
        const BuffSlot& slot = player->_buffs._slots[k];
        if (!slot.IsActive() || slot.expireFrame <= _frameCount)
            continue;
        buffs[nBuff++] = { static_cast<uint8_t>(k), slot.value, nowMs + FramesToMs(slot.expireFrame - _frameCount) };
    }
    job.buffs.len = static_cast<unsigned long>(
        CooldownBuffBlob::PackBuffs(buffs, nBuff, job.buffs.data, sizeof(job.buffs.data)));
#endif

    (void)player;
}

// 칸이 마지막 저장 때 사본(_dbSaved)과 다른가 — 틱 값끼리 견준다.
//   묶은 바이트(실제 시각)로 견주지 않는 까닭 — 같은 칸이어도 저장할 때마다 환산값이 흔들린다(명목 40ms × 남은 틱 vs 실제로 흐른 시간).
//   실측 틱이 명목보다 조금 길면 60초 사이 끝 시각이 1초 넘게 밀려, 쿨타임이 남은 사람은 매 주기 헛저장된다.
//   끝난 쿨타임 칸은 틱 값이 안 바뀌어 저장을 부르지 않는다(버프 칸은 만료 때 CBuffSystem::Tick 이 비워 한 번 실린다)
//   — DB 에 지난 항목이 남아도 들어올 때 버려진다(RestoreCooldownsAndBuffs).
bool CCharacterStore::CooldownsAndBuffsChanged(const CPlayer* player)
{
#if USE_SKILL
    for (int id = 1; id <= MAX_SKILL_ID; ++id)
    {
        if (player->_skillReadyFrame[id] != player->_dbSaved.skillReadyFrame[id])
            return true;
    }
#endif
#if USE_COMBAT
    for (int k = 0; k < BUFF_KIND_COUNT; ++k)
    {
        const BuffSlot& now   = player->_buffs._slots[k];
        const BuffSlot& saved = player->_dbSaved.buffs[k];
        if (now.expireFrame != saved.expireFrame || now.value != saved.value)
            return true;
    }
#endif
    (void)player;
    return false;
}

void CCharacterStore::RecordSavedCooldownsAndBuffs(CPlayer* player)
{
#if USE_SKILL
    for (int id = 0; id <= MAX_SKILL_ID; ++id)
        player->_dbSaved.skillReadyFrame[id] = player->_skillReadyFrame[id];
#endif
#if USE_COMBAT
    for (int k = 0; k < BUFF_KIND_COUNT; ++k)
        player->_dbSaved.buffs[k] = player->_buffs._slots[k];
#endif
    (void)player;
}

// 입장 — 불러온 묶음을 풀어 칸을 채운다(SpawnPlayer, 스탯 세우기 「앞」 — 버프 배수가 한 번의 RecalcStats 에 들어가게).
//   검사 셋: 형식(버전·길이 — CooldownBuffBlob) · 내용(표에 있는 스킬인가 · 버프 종류·값) · 시각(이미 지났나 · 상한).
//   어느 검사에 걸려도 입장은 막지 않는다 — 그 칸이 풀릴 뿐이다. 세는 곳은 여기 한 곳(GameMonitor.h EnterCounters).
//   불량·건너뜀이 있었으면 다음 주기에 한 번 다시 써 행을 정리한다(_dbDirty) — 안 쓰면 칸이 바뀌기 전까지 입장마다 같은 불량을 다시 만난다
//   (형식 불량은 경고 로그 · 건너뜀은 지표만).
void CCharacterStore::RestoreCooldownsAndBuffs(CPlayer* player, const DBResult* cooldownsAndBuffs, bool dropBuffs)
{
#if USE_SKILL || USE_COMBAT
    if (cooldownsAndBuffs != nullptr)
    {
        const DBResult& row = *cooldownsAndBuffs;
        CGameMonitor::EnterCounters& enter = _gameMonitor._enter;
        const int64_t nowMs = WallNowMs();
        bool rewrite = false;

        // 풀기 결과 한 가지를 센다 — 열마다 같은 규칙. 값이 셋이라 switch(EMPTY 를 OK 나 BAD 에 섞지 않는다)
        static_assert(static_cast<int>(CooldownBuffBlob::Unpack::COUNT) == 3, "Unpack 을 늘렸으면 아래 switch 에도 갈래를 넣을 것");
        auto unpacked = [&](CooldownBuffBlob::Unpack r, const char* column, unsigned long len) -> bool
        {
            switch (r)
            {
            case CooldownBuffBlob::Unpack::OK:    return true;
            case CooldownBuffBlob::Unpack::EMPTY: return false;
            case CooldownBuffBlob::Unpack::BAD:
            case CooldownBuffBlob::Unpack::COUNT:
                break;
            }
            enter._timerBad.Inc();
            rewrite = true;
            SLOG_WARN("[DB] {} unreadable -> dropped (account={}, len={})", column, row.accountId, len);
            return false;
        };

#if USE_SKILL
        {
            CooldownBuffBlob::Cooldown items[MAX_SKILL_ID];
            int n = 0;
            const unsigned long len = row.skillCooldowns.len;
            const CooldownBuffBlob::Unpack r = (len > DB_SKILL_COOLDOWNS_BYTES)
                ? CooldownBuffBlob::Unpack::BAD   // 버퍼보다 긴 값 — 앞부분만 읽혔다(DBWorker.cpp RunLoadOnce)
                : CooldownBuffBlob::UnpackCooldowns(row.skillCooldowns.data, len, items, MAX_SKILL_ID, n);
            if (unpacked(r, "skill_cooldowns", len))
            {
                for (int i = 0; i < n; ++i)
                {
                    const int32_t   id  = items[i].skillId;
                    const SkillDef* def = _gameDataFiles.Skill().Find(id);
                    if (def == nullptr || player->_skillReadyFrame[id] != 0)   // 표에 없는 번호(패치로 지움) · 같은 칸이 둘(불량)
                    {
                        enter._timerSkipped.Inc();
                        rewrite = true;
                        continue;
                    }
                    // 지난 것부터 거른다 — 그 뒤의 뺄셈은 양수라 넘치지 않는다(망가진 행의 극단값이어도)
                    if (items[i].readyAtMs <= nowMs)
                    {
                        enter._timerExpired.Inc();   // 접속하지 않은 동안 다 돌았다(정상)
                        continue;
                    }
                    // 남은 틱은 표의 쿨타임으로 자른다 — 시계가 튀었거나 패치로 쿨타임을 줄였을 때 묶이지 않게.
                    //   자르기를 바꾸기 앞에 둔다 — 터무니없이 큰 값을 틱으로 바꾸다 넘치지 않게.
                    const int64_t  remainMs = items[i].readyAtMs - nowMs;
                    const uint64_t frames   = (remainMs >= FramesToMs(def->cooldownFrames)) ? def->cooldownFrames : MsToFramesCeil(remainMs);
                    if (frames == 0)
                    {
                        enter._timerExpired.Inc();   // 쿨타임 0 인 스킬(표를 고친 패치) — 돌 것이 없다
                        continue;
                    }
                    player->_skillReadyFrame[id] = _frameCount + frames;
                    enter._cooldownRestored.Inc();
                }
            }
        }
#endif

#if USE_COMBAT
        if (!dropBuffs)   // 죽은 채 저장된 행이면 버프는 풀지 않고 버린다(GameMonitor.h — 행 단위로 _deadRespawn 이 센다)
        {
            CooldownBuffBlob::Buff items[BUFF_KIND_COUNT];
            int n = 0;
            const unsigned long len = row.buffs.len;
            const CooldownBuffBlob::Unpack r = (len > DB_BUFFS_BYTES)
                ? CooldownBuffBlob::Unpack::BAD
                : CooldownBuffBlob::UnpackBuffs(row.buffs.data, len, items, BUFF_KIND_COUNT, n);
            if (unpacked(r, "buffs", len))
            {
                for (int i = 0; i < n; ++i)
                {
                    if (items[i].kind >= BUFF_KIND_COUNT || !IsRestorableBuffValue(static_cast<BuffKind>(items[i].kind), items[i].value))
                    {
                        enter._timerSkipped.Inc();
                        rewrite = true;
                        continue;
                    }
                    // 지난 것부터 거른다 — 그 뒤의 뺄셈은 양수라 넘치지 않는다(쿨타임과 같은 순서)
                    if (items[i].expireAtMs <= nowMs)
                    {
                        enter._timerExpired.Inc();
                        continue;
                    }
                    // 남은 시간이 상한보다 길면 시계가 튀었거나 행이 망가진 것이라 건너뛴다(BUFF_RESTORE_MAX_REMAIN_MS)
                    const int64_t remainMs = items[i].expireAtMs - nowMs;
                    if (remainMs > BUFF_RESTORE_MAX_REMAIN_MS)
                    {
                        enter._timerSkipped.Inc();
                        rewrite = true;
                        continue;
                    }
                    // 칸·지표(_buff._restored)·목록은 CBuffSystem 이 맡는다. 스탯은 호출부(SpawnPlayer)의 RecalcStats 가 곧 세운다.
                    if (!_buffSystem.Restore(player, static_cast<BuffKind>(items[i].kind), items[i].value, MsToFramesCeil(remainMs), _frameCount))
                    {
                        enter._timerSkipped.Inc();   // 같은 종류가 둘(불량) — 첫 것만 썼다
                        rewrite = true;
                    }
                }
            }
        }
#endif

        if (rewrite)
            player->_dbDirty = true;
    }
#else
    (void)cooldownsAndBuffs;
    (void)dropBuffs;
#endif

    // 되살린 칸이 곧 DB 값이다 — 사본을 맞춰 두지 않으면 첫 주기에 같은 값을 헛저장한다
    RecordSavedCooldownsAndBuffs(player);
}

void CCharacterStore::EnqueuePlayerSave(CPlayer* player)
{
    // 수용된 경우에만 dirty 해제 — 백프레셔로 드롭됐는데 지워버리면 그 변경은 복원 통로가 없다.
    const DBSaveJob job = MakeSaveJob(player);
    if (_dbWorker->Enqueue(job))
        MarkSaveQueued(player, job);
}

void CCharacterStore::TickPeriodicSave()
{
    if (++_dbSaveFrameCounter < _dbSavePeriodFrames)
        return;
    _dbSaveFrameCounter = 0;

    // 서버 메모리 = 진실. 저장 대상(ShouldSave)만 배치에 모아 워커에 1회 핸드오프.
    //   thread_local 재사용으로 매 주기 할당 회피 (게임 스레드 단독 접근).
    thread_local std::vector<DBSaveJob> batch;
    thread_local std::vector<CPlayer*>  owners;    // batch와 나란한 소유자 (드롭 시 dirty 복원용)
    thread_local std::vector<size_t>    dropped;
    batch.clear();
    owners.clear();
    _sessionHub.ForEachPlayer([&](CPlayer* player)
    {
        if (ShouldSave(player))
        {
            batch.push_back(MakeSaveJob(player));
            owners.push_back(player);
            MarkSaveQueued(player, batch.back());   // 저장 요청 후 리셋 (드롭되면 아래에서 되돌림)
        }
    });
    _dbWorker->EnqueueBatch(batch, &dropped);   // lock 1회 + notify 1회

    // 백프레셔로 버려진 잡은 dirty를 되살려 다음 주기에 재시도한다.
    //   되돌리지 않으면 정지(IDLE) 상태 플레이어의 변경은 ShouldSave 대상에서 빠져 영영 유실된다.
    //   호출은 전부 게임 스레드 단독이라 owners 포인터는 이 시점에 유효하다.
    for (size_t idx : dropped)
        owners[idx]->_dbDirty = true;
}

void CCharacterStore::SaveAllPlayers()
{
    // 종료는 유실 방지 우선 — dirty/MOVING 무관하게 전원 최종 저장(안전망).
    // 주기 저장(ShouldSave 필터)과 목적이 다름: 여기선 dirty 추적을 신뢰하지 않고 무조건 다 씀.
    std::vector<DBSaveJob> batch;
    batch.reserve(_sessionHub.SlotCount());
    _sessionHub.ForEachPlayer([&](CPlayer* player)
    {
        batch.push_back(MakeSaveJob(player));
        MarkSaveQueued(player, batch.back());
    });
    _dbWorker->EnqueueBatch(batch);   // 배치 1회 핸드오프
}

// 멈추기 — 몸은 CGameServer::Stop 의 DB 블록에서 옮긴 그대로(부르는 자리도 그대로 — 게임 스레드를 join 한 뒤).
void CCharacterStore::Shutdown()
{
    // 게임 스레드 종료 후(단일 스레드 시점) 전원 최종 저장 → 드레인 → 워커 종료
    if (_dbWorker)
    {
        SaveAllPlayers();
        _dbWorker->Shutdown(0);
        _dbWorker.reset();
    }
}

static int64_t SteadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool CCharacterStore::RequestCharacterLoad(int64_t sessionId, int64_t accountId)
{
    DBLoadJob job;
    job.accountId = accountId;
    job.sessionId = sessionId;       // 결과가 돌아왔을 때 주인을 찾는 열쇠 — 세션 번호 "전체"
    job.requestNs = SteadyNowNs();

    // 거절되면 결과가 오지 않는다 → requested 에 넣지 않고 따로 센다(등식이 안 흐려지게).
    //   호출부는 false 를 받으면 그 세션을 끊어야 한다 — 매달아 두면 영영 안 들어온다.
    if (_dbWorker->EnqueueLoad(job))
    {
        _gameMonitor._enter._requested.Inc();
        return true;
    }
    _gameMonitor._enter._rejected.Inc();
    return false;
}

void CCharacterStore::ProcessDbResults()
{
    if (!_dbWorker)
        return;

    // _dbResults 는 직전 틱에 모두 소비되어 비어 있다 — 빈 벡터를 워커 쪽과 맞바꾼다(락 1회).
    _dbWorker->SwapOutResults(_dbResults);
    if (_dbResults.empty())
        return;

    const int64_t nowNs = SteadyNowNs();

    static_assert(DB_JOB_KIND_COUNT == (USE_INVENTORY ? 3 : 2), "DBJobKind 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
    for (const DBResult& result : _dbResults)
    {
        switch (result.kind)
        {
        case DBJobKind::LOAD_CHARACTER:
            _loadEvents.OnCharacterLoaded(result, nowNs);   // 받는 쪽이 판정·스폰한다(알림 — 게임 서버 나누기 0-③ ⑷)
            break;

        case DBJobKind::SAVE_POSITION:   // 저장은 편도 — 결과를 돌려주지 않는다. 여기 오면 워커 쪽 결함이다
#if USE_INVENTORY
        case DBJobKind::SAVE_ITEM_SLOT:  // 칸 저장도 편도 — 성공·실패는 워커가 _dbItem 에 센다
#endif
        case DBJobKind::COUNT:
            SLOG_ERROR("[DB] unexpected result kind {}", static_cast<int>(result.kind));
            break;
        }
    }
    _dbResults.clear();
}

#if USE_INVENTORY
#if USE_DB_WORKER
static_assert(DB_INV_MAX_ROWS >= INVENTORY_ALL_SLOTS, "결과에 실을 가방 줄 수가 가방·착용 칸 수보다 적다");

namespace
{
    // 가방의 불러오기 거절 → 지표 라벨. 거절은 가방(LoadSlot)이 정하고 라벨로 옮기는 건 여기 — ToPickupFail 과 같은 꼴
    InvLoadSkip ToInvLoadSkip(InvLoadResult r)
    {
        switch (r)
        {
        case InvLoadResult::WRONG_TAB: return InvLoadSkip::WRONG_TAB;
        case InvLoadResult::BAD_COUNT: return InvLoadSkip::BAD_COUNT;
        case InvLoadResult::OK:        break;
        }
        assert(false && "거절이 아닌 결과를 건너뛴 줄로 세려 했다");
        return InvLoadSkip::BAD_COUNT;
    }
}

void CCharacterStore::LoadInventoryRows(CPlayer* player, const DBResult& result)
{
    CGameMonitor::InventoryCounters& counters = _gameMonitor._inventory;
    auto skip = [&](InvLoadSkip why, const DBInvRow& r)
    {
        counters._loadSkipped[static_cast<int>(why)].Inc();
        SLOG_WARN("[INV] load skip {} (account={}, slot={}, item={}, count={})",
                  InvLoadSkipName(why), player->_accountId, r.slot, r.itemId, r.count);
    };

    // 사유는 가장 기본적인 것부터 본다 — 칸이 틀리면 아이템을 볼 것도 없다
    for (int i = 0; i < result.invCount; ++i)
    {
        const DBInvRow& r = result.inv[i];

        InvTab tab;
        int index = -1;
        if (r.slot < 0 || r.slot > 0xFFFF || !FindInvSlot(static_cast<uint16_t>(r.slot), tab, index))
        {
            skip(InvLoadSkip::BAD_SLOT, r);   // uint16 로 줄이기 전에 범위부터 — 큰 값이 엉뚱한 칸으로 접히지 않게
            continue;
        }
        const ItemDef* def = _gameDataFiles.Item().Find(r.itemId);
        if (def == nullptr)
        {
            skip(InvLoadSkip::UNKNOWN_ITEM, r);
            continue;
        }
        // 탭·개수는 가방 규칙이라 가방이 본다 — 여기는 거절을 지표 라벨로 옮겨 세기만
        const InvLoadResult loaded = player->_inventory.LoadSlot(index, *def, r.count);
        if (loaded != InvLoadResult::OK)
        {
            skip(ToInvLoadSkip(loaded), r);   // 깎아 넣지 않는다 — 깎으면 사라진 걸 아무도 모른다
            continue;
        }
        counters._loadedSlots.Inc();
    }

    if (result.invOverflow > 0)
    {
        counters._loadSkipped[static_cast<int>(InvLoadSkip::TOO_MANY_ROWS)].Add(result.invOverflow);
        SLOG_WARN("[INV] load skip too_many_rows x{} (account={}) — 결과 자리 {} 를 넘은 줄",
                  result.invOverflow, player->_accountId, DB_INV_MAX_ROWS);
    }
}
#endif
#endif // USE_INVENTORY

#endif // USE_DB_WORKER
