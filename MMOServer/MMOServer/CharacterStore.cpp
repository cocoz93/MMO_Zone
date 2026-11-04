#include "CharacterStore.h"

#if USE_DB_WORKER

#include "SessionHub.h"
#include "Player.h"
#include "MapManager.h"         // CMapManager::GetMapIdFromZoneId — 저장 잡의 맵 번호
#include "GameMonitor.h"
#include "GameDataFiles.h"      // 스킬 표 · 아이템 표
#include "BuffSystem.h"         // USE_COMBAT — 버프 되살리기(Restore)
#include "CooldownBuffBlob.h"   // 쿨타임·버프 열의 바이트 형식(토글을 안 보는 헤더)
#include <Common/ErrorLog.h>
#include <cassert>
#include <chrono>

// ==========================================================================
// 캐릭터 저장소 — GameServer.cpp 에서 옮김(몸·주석 그대로 · 게임 서버 나누기 4 단계).
//   바뀐 것: 회신 꺼내기가 불러오기 결과를 알림(_loadEvents)으로 넘김 · 되살리기가 입장 배치 구조체 대신 쓰는 두 칸만 받음.
// ==========================================================================

// ==========================================================================
// DB 저장 파이프라인 — 바뀐 자리는 표시만, 계기가 묶음 하나를 워커에(저장 플랜 C · 아래 「저장 묶음」 절)
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

#if USE_SKILL || USE_COMBAT   // 쿨타임·버프가 하나도 없는 빌드는 아래 셋을 안 쓴다(쓰지 않는 static 함수 경고)
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

// 형식의 최대 길이가 DB 쪽 버퍼에 들어가야 한다 — 기술 목록·버프 표 상한을 늘리면 여기서 막힌다(DBWorker.h 의 숫자를 올릴 것)
#if USE_SKILL
static_assert(CooldownBuffBlob::CooldownsMaxBytes(MAX_SKILL_BOOK) <= DB_SKILL_COOLDOWNS_BYTES, "기술 목록 상한을 늘렸으면 DBWorker.h 의 DB_SKILL_COOLDOWNS_BYTES 를 올릴 것");
static_assert(DB_SKILL_COOLDOWNS_BYTES <= 255, "skill_cooldowns 열은 VARBINARY(255) 다(schema.sql)");
static_assert(MAX_SKILL_BOOK <= 255, "묶음의 개수 칸이 1바이트다(CooldownBuffBlob)");
static_assert(MAX_SKILL_ID <= 0xFFFF, "스킬 번호를 2바이트에 싣는다(CooldownBuffBlob 판 2)");
#endif
#if USE_COMBAT
static_assert(CooldownBuffBlob::BuffsMaxBytes(MAX_BUFF_ROWS) <= DB_BUFFS_BYTES, "버프 표 상한을 늘렸으면 DBWorker.h 의 DB_BUFFS_BYTES 를 올릴 것");
static_assert(DB_BUFFS_BYTES <= 255, "buffs 열은 VARBINARY(255) 다(schema.sql)");
static_assert(MAX_BUFF_ROWS <= 255, "묶음의 개수 칸이 1바이트다(CooldownBuffBlob)");
static_assert(MAX_BUFF_ID <= 0xFFFF, "버프 번호를 2바이트에 싣는다(CooldownBuffBlob 판 2)");
#endif

bool CCharacterStore::ShouldSave(const CPlayer* player) const
{
    // 저장 대상 판정(주기·틱 끝 — 나갈 때·종료는 이 판정 없이 쓴다): 덩어리 하나라도 바뀌었나
    //   가방 = 저장 안 된 칸이 있다(CInventory::HasUnsaved — 가방이 스스로 켠다 · 화면용 표시와 따로 — Inventory.h)
#if USE_INVENTORY
    if (player->_inventory.HasUnsaved())
        return true;
#endif
    return CharRowChanged(player);
}

bool CCharacterStore::CharRowChanged(const CPlayer* player) const
{
    // 캐릭터 행 판정 — 참이면 묶음이 행을 싣는다(PrepareBundle 의 SAVE_PART_CHAR_ROW). 열마다 보는 것:
    //   x · y · map_id                   = MOVING(위치가 계속 변하는 중 → 저장 후 dirty가 리셋돼도 매 주기 재저장) · _dbDirty(정지/접속/존변경/클램프 — 일회성, 저장 후 리셋)
    //   level · exp · hp · mp · money    = 값 비교 — 마지막으로 실은 값(_dbSaved)과 다름. 바뀌는 자리마다 켜지 않고 여기서 본다(Player.h)
    //   skill_cooldowns                  = 틱 비교(CooldownsChanged — 바이트는 저장마다 흔들려 못 견준다)
    //   buffs                            = 안 본다 — 버프만 바뀌었다고 저장하지 않는다(11-02 사용자 결정 — 서버가 죽으면 날아가도 되는 값이다).
    //                                      행이 실릴 때 그때까지 걸려 있으면 같이 실리고, 나갈 때·종료는 이 판정 없이 쓴다
    //   ★ 열을 늘리면 여기에도 — 안 넣으면 그 열만 바뀐 변경은 행이 안 실린다(열 수 static_assert 가 DBWorker.cpp kCharColumns 옆에서 부른다)
    if (player->_moveState == MoveState::MOVING || player->_dbDirty)
        return true;
    if (CooldownsChanged(player))
        return true;

    // 저장할 값과 같은 식으로 견준다(MakeCharRow) — 토글을 끈 빌드의 「모름(-1)」도 똑같이 나와 헛저장이 없다.
    const DBCharRow now = MakeCharRow(player);
    const CPlayer::DbSavedValues& saved = player->_dbSaved;
    return now.level != saved.level || now.exp != saved.exp || now.hp != saved.hp || now.mp != saved.mp
        || now.job != saved.job        // 직업 — 고를 때 틱 끝 저장(SaveReason::JOB)이 먼저 실어 보통 여기 안 걸린다. 그 저장이 실패했을 때 다음 주기가 잡는다
        || now.money != saved.money;   // 돈 — 바뀌는 자리가 여럿(줍기·보상·명령·사고팔기)이라 체력·경험치처럼 값으로 본다(Inventory.h [돈])
}

DBCharRow CCharacterStore::MakeCharRow(const CPlayer* player) const
{
    DBCharRow row;
    row.characterId = ToInt(player->_characterId);
    row.x         = player->_x;
    row.y         = player->_y;
    row.mapId     = ToInt(CMapManager::GetMapIdFromZoneId(player->_zoneId));
    row.level     = player->_level;
    row.exp       = player->_exp;
    row.job       = player->_job;
    // 체력·마력 — 값을 세우는 코드가 없는 빌드에선 「모름」을 싣는다. 0 을 실으면 다음 입장이 죽은 채 저장된 행으로 읽는다.
#if USE_COMBAT
    row.hp        = player->_health.Hp();
#else
    row.hp        = DB_HP_MP_UNSAVED;
#endif
#if USE_SKILL
    row.mp        = player->_attack.Mp();
#else
    row.mp        = DB_HP_MP_UNSAVED;
#endif
    PackCooldownsAndBuffs(player, row);   // 쿨타임·버프 — 토글을 끈 빌드는 빈 값
#if USE_INVENTORY
    row.money     = player->_inventory.Money();
#else
    row.money     = 0;   // 가방을 끈 빌드 — 돈 열이 열 표에 없어 안 써진다(칸만 채워 둔다)
#endif
    return row;
}

void CCharacterStore::RecordSaved(CPlayer* player, const DBCharRow& row)
{
    // 「마지막으로 실은 값」 사본 — 다음 판정(ShouldSave)이 이 값과 견준다(Player.h _dbSaved).
    player->_dbSaved.level   = row.level;
    player->_dbSaved.exp     = row.exp;
    player->_dbSaved.hp      = row.hp;
    player->_dbSaved.mp      = row.mp;
    player->_dbSaved.money   = row.money;
    player->_dbSaved.job     = row.job;
    // 쿨타임은 잡의 바이트가 아니라 칸(틱)을 사본으로 남긴다 — 같은 틱에 만든 잡이라 내용은 같다(까닭은 CooldownsAndBuffsChanged)
    RecordSavedCooldowns(player);
}

// ── 쿨타임·버프 ↔ DB 묶음 ─────────────────────────────────────────────────
//   칸은 틱으로 들고(공격 컴포넌트의 기술 목록 _skillBook · 효과 컴포넌트 _slots), DB 엔 실제 시각으로 묶어 넣는다(CooldownBuffBlob.h).
//   저장 = 돌고 있는 칸만 「끝나는 실제 시각」으로 · 입장 = 지난 것은 버리고 남은 몫을 틱으로.

void CCharacterStore::PackCooldownsAndBuffs(const CPlayer* player, DBCharRow& row) const
{
    row.skillCooldowns.len = 0;
    row.buffs.len          = 0;
#if USE_SKILL || USE_COMBAT
    const int64_t nowMs = WallNowMs();
#endif

#if USE_SKILL
    // 다시 쓸 수 있는 틱이 지금보다 뒤인 항목만 — 0(한 번도 안 씀)·지난 항목은 돌고 있지 않다(CSkillSystem 의 판정 frame < ready 와 같은 경계)
    //   [불변식] 목록 길이 ≤ MAX_SKILL_BOOK — 사람 목록은 공용 줄 + 내 직업 줄이고, 표 읽기가 직업마다 그 수를 막았다(SkillTable.cpp ⑼)
    assert(player->_attack._skillBook.size() <= static_cast<size_t>(MAX_SKILL_BOOK));
    CooldownBuffBlob::Cooldown cooldowns[MAX_SKILL_BOOK];
    int nCooldown = 0;
    for (const CAttackComponent::SkillBookEntry& e : player->_attack._skillBook)
    {
        if (e.readyFrame <= _frameCount || nCooldown >= MAX_SKILL_BOOK)
            continue;
        cooldowns[nCooldown++] = { static_cast<uint16_t>(e.skillId), nowMs + FramesToMs(e.readyFrame - _frameCount) };
    }
    row.skillCooldowns.len = static_cast<unsigned long>(
        CooldownBuffBlob::PackCooldowns(cooldowns, nCooldown, row.skillCooldowns.data, sizeof(row.skillCooldowns.data)));
#endif

#if USE_COMBAT
    // 끝나는 틱이 지금보다 뒤인 것만 — 이번 틱의 만료 훑기(CBuffSystem::Tick) 전에 이미 끝난 것은 뺀다(Tick 의 경계 frame >= expire)
    //   [불변식] 목록 길이 ≤ MAX_BUFF_ROWS — 표에 있는 번호만 하나씩 걸린다(Buff.h)
    assert(player->_buffs._activeBuffs.size() <= static_cast<size_t>(MAX_BUFF_ROWS));
    CooldownBuffBlob::Buff buffs[MAX_BUFF_ROWS];
    int nBuff = 0;
    for (const ActiveBuff& active : player->_buffs._activeBuffs)
    {
        if (active.expireFrame <= _frameCount || nBuff >= MAX_BUFF_ROWS)
            continue;
        buffs[nBuff++] = { static_cast<uint16_t>(active.buffId), nowMs + FramesToMs(active.expireFrame - _frameCount) };
    }
    row.buffs.len = static_cast<unsigned long>(
        CooldownBuffBlob::PackBuffs(buffs, nBuff, row.buffs.data, sizeof(row.buffs.data)));
#endif

    (void)player;
}

// 쿨타임이 마지막 저장 때 사본(_dbSaved)과 다른가 — 틱 값끼리 견준다.
//   묶은 바이트(실제 시각)로 견주지 않는 까닭 — 같은 칸이어도 저장할 때마다 환산값이 흔들린다(명목 40ms × 남은 틱 vs 실제로 흐른 시간).
//   실측 틱이 명목보다 조금 길면 60초 사이 끝 시각이 1초 넘게 밀려, 쿨타임이 남은 사람은 매 주기 헛저장된다.
//   끝난 쿨타임 칸은 틱 값이 안 바뀌어 저장을 부르지 않는다
//   — DB 에 지난 항목이 남아도 들어올 때 버려진다(RestoreCooldownsAndBuffs).
bool CCharacterStore::CooldownsChanged(const CPlayer* player)
{
#if USE_SKILL
    // 칸 = 목록 자리 — 목록은 입장 때 한 번 채우고 순서가 안 바뀐다(Player.h _dbSaved). 목록 길이는 MAX_SKILL_BOOK 안(PackCooldownsAndBuffs 의 불변식)
    const std::vector<CAttackComponent::SkillBookEntry>& book = player->_attack._skillBook;
    for (size_t i = 0; i < book.size() && i < static_cast<size_t>(MAX_SKILL_BOOK); ++i)
    {
        if (book[i].readyFrame != player->_dbSaved.skillReadyFrame[i])
            return true;
    }
#endif
    (void)player;
    return false;
}

void CCharacterStore::RecordSavedCooldowns(CPlayer* player)
{
#if USE_SKILL
    const std::vector<CAttackComponent::SkillBookEntry>& book = player->_attack._skillBook;
    for (size_t i = 0; i < book.size() && i < static_cast<size_t>(MAX_SKILL_BOOK); ++i)
        player->_dbSaved.skillReadyFrame[i] = book[i].readyFrame;
#endif
    (void)player;
}

// 입장 — 불러온 묶음을 풀어 칸을 채운다(SpawnPlayer, 스탯 세우기 「앞」 — 버프 효과가 한 번의 RecalcStats 에 들어가게).
//   검사 셋: 형식(버전·길이 — CooldownBuffBlob) · 내용(표에 있는 스킬·버프인가) · 시각(이미 지났나 · 표의 값으로 자름).
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
            SLOG_WARN("[DB] {} unreadable -> dropped (character={}, len={})", column, row.characterId, len);
            return false;
        };

#if USE_SKILL
        {
            CooldownBuffBlob::Cooldown items[MAX_SKILL_BOOK];
            int n = 0;
            bool oldFormat = false;
            const unsigned long len = row.skillCooldowns.len;
            const CooldownBuffBlob::Unpack r = (len > DB_SKILL_COOLDOWNS_BYTES)
                ? CooldownBuffBlob::Unpack::BAD   // 버퍼보다 긴 값 — 앞부분만 읽혔다(DBWorker.cpp RunLoadOnce)
                : CooldownBuffBlob::UnpackCooldowns(row.skillCooldowns.data, len, items, MAX_SKILL_BOOK, n, oldFormat);
            if (unpacked(r, "skill_cooldowns", len))
            {
                // 옛 판(1 — 번호 1바이트)이면 다음 주기에 새 판으로 다시 쓴다(옮기기 · 11-01). 내용은 같은 목록이라 아래 검사를 그대로 지난다
                if (oldFormat)
                {
                    enter._cooldownOldFormat.Inc();
                    rewrite = true;
                }
                for (int i = 0; i < n; ++i)
                {
                    const int32_t   id  = items[i].skillId;
                    const SkillDef* def = _gameDataFiles.Skill().Find(id);
                    // 표에 없는 번호(패치로 지움) · 이 캐릭터의 기술 목록에 없는 번호(사람 기술이 아니게 된 줄) · 같은 번호가 둘(불량 — 앞 것이 이미 걸었다)
                    CAttackComponent::SkillBookEntry* entry = (def == nullptr) ? nullptr : player->_attack.FindSkill(id);
                    if (entry == nullptr || entry->readyFrame != 0)
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
                    entry->readyFrame = _frameCount + frames;
                    enter._cooldownRestored.Inc();
                }
            }
        }
#endif

#if USE_COMBAT
        if (!dropBuffs)   // 죽은 채 저장된 행이면 버프는 풀지 않고 버린다(GameMonitor.h — 행 단위로 _deadRespawn 이 센다)
        {
            CooldownBuffBlob::Buff items[MAX_BUFF_ROWS];
            int n = 0;
            bool oldFormat = false;
            const unsigned long len = row.buffs.len;
            const CooldownBuffBlob::Unpack r = (len > DB_BUFFS_BYTES)
                ? CooldownBuffBlob::Unpack::BAD
                : CooldownBuffBlob::UnpackBuffs(row.buffs.data, len, items, MAX_BUFF_ROWS, n, oldFormat);
            if (unpacked(r, "buffs", len))
            {
                // 옛 판(1 — 종류 + 값)이면 다음 주기에 새 판으로 다시 쓴다(옮기기 · 11-01). 내용은 같은 목록이라 아래 검사를 그대로 지난다
                if (oldFormat)
                {
                    enter._buffOldFormat.Inc();
                    rewrite = true;
                }
                for (int i = 0; i < n; ++i)
                {
                    // 표에 없는 번호(패치로 지움 · 옛 판의 모르는 종류)
                    const BuffDef* def = _gameDataFiles.Buffs().Find(items[i].buffId);
                    if (def == nullptr)
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
                    // 남은 틱은 표의 지속으로 자른다 — 시계가 튀었거나 패치로 지속을 줄였을 때 오래 남지 않게(쿨타임과 같은 규칙).
                    //   자르기를 바꾸기 앞에 둔다 — 터무니없이 큰 값을 틱으로 바꾸다 넘치지 않게.
                    const int64_t  remainMs = items[i].expireAtMs - nowMs;
                    const uint64_t frames   = (remainMs >= FramesToMs(def->durationFrames)) ? def->durationFrames : MsToFramesCeil(remainMs);
                    // 목록·지표(_buff._restored)는 CBuffSystem 이 맡는다. 스탯은 호출부(SpawnPlayer)의 RecalcStats 가 곧 세운다.
                    if (!_buffSystem.Restore(player, *def, frames, _frameCount))
                    {
                        enter._timerSkipped.Inc();   // 같은 번호가 둘(불량) — 첫 것만 썼다
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
    RecordSavedCooldowns(player);
}

void CCharacterStore::SaveAllPlayers()
{
    // 종료는 유실 방지 우선 — dirty/MOVING 무관하게 전원 최종 저장(안전망).
    // 주기 저장(ShouldSave 필터)과 목적이 다름: 여기선 dirty 추적을 신뢰하지 않고 무조건 다 씀.
    //   전원을 떠난 기록으로 넘긴다(나갈 때와 같은 마지막 묶음 — 행 + 표시 칸 ∪ 비행 칸) → 드레인 뒤 Shutdown 이
    //   회신을 꺼내 확정 안 된 기록을 「잃음」으로 센다. 실패 중이던 떠난 기록도 한 번 더 싣는다.
    //   짝 고리가 걸린 사람은 떠난 기록을 붙잡고(LeaveFinal), 두 사람의 최종을 짝 묶음으로 같은 큐에 낸다 —
    //   떠 있던 짝 묶음이 있어도 같은 큐(작은 캐릭터의 워커) 뒤에 줄 서므로 뒤집히지 않는다(늦게 온 옛 회신은 번호가 안 맞아 주인 없음).
    _sessionHub.ForEachPlayer([&](CPlayer* player)
    {
        LeaveFinal(player, SaveTrigger::SHUTDOWN);
    });
    for (auto& kv : _leaveRecords)
    {
        if (!kv.second.inFlight && !kv.second.paired)
            SendLeaveRecord(kv.second, SaveTrigger::SHUTDOWN);
    }
    for (PairLink& link : _pairs)
        SendPair(link, SaveTrigger::SHUTDOWN);
}

// 멈추기 — 부르는 자리는 CGameServer::Stop 의 DB 블록 그대로(게임 스레드를 join 한 뒤).
void CCharacterStore::Shutdown()
{
    // 게임 스레드 종료 후(단일 스레드 시점) 전원 최종 저장 → 드레인 → 마지막 꺼내기 → 워커 종료
    if (_dbWorker)
    {
        SaveAllPlayers();
        _dbWorker->Shutdown(0);

        // 마지막 꺼내기 — 드레인이 끝나 회신이 다 들어와 있다. 확정된 떠난 기록을 지우고, 남은 것은 잃음.
        //   불러오기 결과는 버린다(게임 스레드가 이미 끝나 들여보낼 데가 없다 — 전에도 버려졌다).
        _dbWorker->SwapOutResults(_dbResults, _dbSaveReplies);
        for (const DBSaveReply& reply : _dbSaveReplies)
            OnSaveReply(reply);
        _dbSaveReplies.clear();
        _dbResults.clear();
        for (const auto& kv : _leaveRecords)
        {
            SLOG_ERROR("[DB] save LOST at shutdown (character={}, rows={}) — DB 가 마지막 묶음을 확정하지 않았다",
                       kv.first, kv.second.rows.size());
        }
        _gameMonitor._save._lost.Add(static_cast<int64_t>(_leaveRecords.size()));
        _leaveRecords.clear();
        _gameMonitor._save._pendingLeave.Store(0);

        _dbWorker.reset();
    }
}

// ==========================================================================
// 저장 묶음 — 계기 넷 · 비행 기록 · 떠난 기록 (save-unify-plan.md 「목표 구조」)
//   바뀐 자리는 표시만 한다(자리 _dbDirty · 값 비교 _dbSaved · 가방 _unsaved). 계기가 오면 표시를 비행 기록으로 옮겨
//   묶음 하나(바뀐 캐릭터 행 + 저장 안 된 칸 — 실을 덩어리는 parts)를 넣고, 회신이 확정(성공)하거나 되살린다(실패). 캐릭터당 비행 중 묶음은 하나.
//   나간 사람의 묶음은 떠난 기록이 주인 — 실패하면 다음 주기에 다시 싣고, 확정 전엔 같은 캐릭터 불러오기를 실패로 돌린다.
// ==========================================================================

// ==========================================================================
// 짝 저장 — 거래 한 건의 두 사람(헤더 「짝 저장」 · trade-plan.md 「저장 — 왜 붙잡나」)
// ==========================================================================
bool CCharacterStore::CanSaveTradeNow(const CPlayer* a, const CPlayer* b) const
{
    if (!_dbWorker)
        return true;   // DB 를 안 쓰는 빌드·설정 — 맞바꾸기만 한다
    return a->_dbFlight.bundleNo == 0 && b->_dbFlight.bundleNo == 0 && !a->_dbFlight.paired && !b->_dbFlight.paired;
}

void CCharacterStore::SaveTrade(CPlayer* a, CPlayer* b)
{
    if (!_dbWorker)
        return;
    assert(CanSaveTradeNow(a, b) && "부르는 쪽(거래)이 맞바꾸기 전에 물었다");
    PairLink link;
    link.character[0] = a->_characterId;  link.session[0] = a->_sessionId;
    link.character[1] = b->_characterId;  link.session[1] = b->_sessionId;
    a->_dbFlight.paired = true;
    b->_dbFlight.paired = true;
    _pairs.push_back(link);
    SendPair(_pairs.back(), SaveTrigger::TRADE);   // 거절돼도 고리는 남는다 — 다음 주기에 다시(TickSaves)
}

CCharacterStore::PairLink* CCharacterStore::FindPair(CharacterId characterId, int* outSide)
{
    for (PairLink& link : _pairs)
    {
        for (int s = 0; s < 2; ++s)
        {
            if (link.character[s] == characterId)
            {
                if (outSide != nullptr)
                    *outSide = s;
                return &link;
            }
        }
    }
    return nullptr;
}

bool CCharacterStore::SendPair(PairLink& link, SaveTrigger trigger)
{
    // 쪽마다 머리 — 살아 있는 사람은 지금 값(PrepareBundle — 표시를 비행 기록으로), 나간 쪽은 붙잡힌 떠난 기록(새 번호)
    DBSaveBundle head[2]{};
    CPlayer*     player[2] = { nullptr, nullptr };
    LeaveRecord* rec[2]    = { nullptr, nullptr };
    for (int s = 0; s < 2; ++s)
    {
        _pairRows[s].clear();
        if (link.session[s] != NO_OWNER_SESSION)
        {
            player[s] = _sessionHub.FindPlayer(link.session[s]);
            assert(player[s] != nullptr && player[s]->_characterId == link.character[s] && "나가면 LeaveFinal 이 세션을 0 으로 바꾼다");
            assert(player[s]->_dbFlight.paired && player[s]->_dbFlight.bundleNo == 0 && "고리가 걸린 동안 혼자 묶음을 안 냈다 · 실패면 되살렸다");
            PrepareBundle(player[s], head[s], _pairRows[s]);
        }
        else
        {
            auto it = _leaveRecords.find(link.character[s]);
            if (it == _leaveRecords.end() || !it->second.paired)
            {
                // 있을 수 없다 — 붙잡힌 기록은 짝 성공 회신으로만 지워지고, 그때 고리도 풀린다. 고리는 남는다(다음 주기에 또 이 줄)
                SLOG_ERROR("[DB] save pair side missing (character={}, partner={}) — pair not sent",
                           link.character[s], link.character[1 - s]);
                for (int t = 0; t < s; ++t)
                {
                    if (player[t] != nullptr)
                        AbortBundle(player[t]);
                }
                return false;
            }
            rec[s] = &it->second;
            if (++_leaveSeq == 0)
                _leaveSeq = 1;
            rec[s]->head.bundleNo  = _leaveSeq;
            rec[s]->head.slotBegin = 0;
            rec[s]->head.slotCount = static_cast<int32_t>(rec[s]->rows.size());
            head[s] = rec[s]->head;
        }
    }

    const DBInvRow* rows[2];
    int             count[2];
    for (int s = 0; s < 2; ++s)
    {
        const std::vector<DBInvRow>& v = (rec[s] != nullptr) ? rec[s]->rows : _pairRows[s];
        rows[s]  = v.empty() ? nullptr : v.data();
        count[s] = static_cast<int32_t>(v.size());
    }
    if (!_dbWorker->EnqueueSavePair(head[0], rows[0], count[0], head[1], rows[1], count[1]))
    {
        for (int s = 0; s < 2; ++s)
        {
            if (player[s] != nullptr)
                AbortBundle(player[s]);   // 표시를 되살린다 — 다음 짝 묶음이 다시 싣는다
        }
        link.inFlight = false;
        return false;
    }

    for (int s = 0; s < 2; ++s)
    {
        if (player[s] != nullptr)
            CommitBundle(player[s], head[s], trigger);   // 비행 중으로 — 회신은 지금 회신 길(살아 있는 사람)을 탄다
        else
        {
            rec[s]->inFlight = true;                     // 회신은 떠난 기록 길을 탄다(캐릭터 + 번호)
            _gameMonitor._save._txn[static_cast<int>(trigger)].Inc();
        }
        link.bundleNo[s] = head[s].bundleNo;
        link.replied[s]  = false;
    }
    link.ok       = true;
    link.inFlight = true;
    return true;
}

void CCharacterStore::OnPairReply(const DBSaveReply& reply)
{
    int side = 0;
    PairLink* link = FindPair(reply.characterId, &side);
    if (link == nullptr || !link->inFlight || link->bundleNo[side] != reply.bundleNo || link->replied[side])
        return;   // 짝 묶음의 머리가 아니다(혼자 묶음 · 종료 때 다시 낸 짝 뒤에 온 옛 짝의 회신)
    link->replied[side] = true;
    link->ok = link->ok && reply.ok;
    if (!link->replied[1 - side])
        return;   // 다른 쪽 회신을 기다린다(워커가 둘을 이어 넣어 대개 같은 꺼내기에 온다)

    if (!link->ok)
    {
        // 실패 — 쪽마다 회신 길이 이미 되살렸다(살아 있는 사람 AbortBundle · 떠난 기록 inFlight=false). 고리를 두고 다음 주기에 짝째로.
        link->inFlight = false;
        return;
    }

    // 성공 — 고리를 풀고, 쪽마다 붙잡아 둔 것을 다시 흐르게 한다
    const PairLink done = *link;
    _pairs.erase(_pairs.begin() + (link - _pairs.data()));
    for (int s = 0; s < 2; ++s)
        ReleasePairSide(done.character[s], done.session[s]);
}

void CCharacterStore::ReleasePairSide(CharacterId characterId, SessionId sessionId)
{
    if (sessionId != NO_OWNER_SESSION)
    {
        CPlayer* player = _sessionHub.FindPlayer(sessionId);
        if (player != nullptr && player->_characterId == characterId)
        {
            player->_dbFlight.paired = false;
            RequestSave(player, SaveReason::AGAIN);   // 짝 묶음 뒤에 바뀐 것이 있으면 이번 틱 끝에 혼자 묶음으로(판정이 거짓이면 헛일 없음)
        }
        return;
    }
    // 나간 쪽 — 그 기록이 아직 있으면 짝 묶음에 안 실린 것이다(짝이 떠 있는 동안 나가 붙잡힌 것 · 실린 것은 성공 회신에 지워졌다)
    auto it = _leaveRecords.find(characterId);
    if (it != _leaveRecords.end() && it->second.paired)
    {
        it->second.paired = false;
        if (!it->second.inFlight)
            SendLeaveRecord(it->second, SaveTrigger::LEAVE);
    }
}

void CCharacterStore::SetInflight(int delta)
{
    _inflightCount += delta;
    assert(_inflightCount >= 0);
    _gameMonitor._save._inflight.Store(_inflightCount);
}

void CCharacterStore::PrepareBundle(CPlayer* player, DBSaveBundle& head, std::vector<DBInvRow>& rows)
{
    CPlayer::DbFlight& f = player->_dbFlight;
    assert(f.bundleNo == 0 && "비행 중 묶음은 캐릭터당 하나 — 부르는 쪽이 먼저 본다");

    // ★ 행 판정은 맨 앞에서 — 아래 표시 옮기기(_dbDirty = false)·사본 갱신(RecordSaved) 뒤에 보면 이동 중이 아닌 한 늘 「안 바뀜」이다
    const bool rowChanged = CharRowChanged(player);

    head           = DBSaveBundle{};
    head.row       = MakeCharRow(player);
    head.sessionId = player->_sessionId;
    head.bundleNo  = f.nextNo;
    f.nextNo = (f.nextNo == UINT32_MAX) ? 1 : f.nextNo + 1;   // 0 은 「비행 없음」이라 안 쓴다

    // 표시 → 비행 기록(지우지 않고 옮긴다 — 실패 회신이면 되살린다)
    f.savedBefore    = player->_dbSaved;
    f.posDirty       = player->_dbDirty;
    player->_dbDirty = false;
    RecordSaved(player, head.row);

    head.slotBegin = static_cast<uint32_t>(rows.size());
    head.slotCount = 0;
#if USE_INVENTORY
    f.slots = player->_inventory.TakeUnsaved();
    for (int i = 0; i < INVENTORY_ALL_SLOTS; ++i)
    {
        if (!f.slots.test(static_cast<size_t>(i)))
            continue;
        const Item& s = player->_inventory.At(i);   // 칸의 최종값 — 빈 칸은 item 0 으로 덮는다(DB 계정에 DELETE 권한이 없다)
        rows.push_back(DBInvRow{ static_cast<int32_t>(CInventory::SlotNumberOf(i)), s.itemId, s.count });
        ++head.slotCount;
    }
#endif

    // 실을 덩어리 — 행은 바뀌었을 때만(save-skip-row-plan.md). 칸이 0줄이면 행을 싣는다: 판정 없이 부르는 짝 묶음(SendPair)도
    //   빈 묶음이 되지 않게(PushBundleLocked 단언) · 주기·틱 끝은 판정이 참이었으니 칸이 없으면 행이 바뀐 것이다.
    head.parts = (rowChanged || head.slotCount == 0) ? SAVE_PART_CHAR_ROW : 0;
}

void CCharacterStore::AbortBundle(CPlayer* player)
{
    // 되살림 셋 — 자리 표시 · 값 비교 기준 · 가방 칸. 판정이 다시 참이 되어 다음 계기에 실린다.
    CPlayer::DbFlight& f = player->_dbFlight;
    player->_dbSaved = f.savedBefore;
    if (f.posDirty)
        player->_dbDirty = true;
#if USE_INVENTORY
    player->_inventory.MarkUnsaved(f.slots);
    f.slots.reset();
#endif
}

void CCharacterStore::CommitBundle(CPlayer* player, const DBSaveBundle& head, SaveTrigger trigger)
{
    CPlayer::DbFlight& f = player->_dbFlight;
    f.bundleNo = head.bundleNo;
    f.again    = false;
    SetInflight(+1);
    _gameMonitor._save._txn[static_cast<int>(trigger)].Inc();
}

bool CCharacterStore::SendBundle(CPlayer* player, SaveTrigger trigger)
{
    CPlayer::DbFlight& f = player->_dbFlight;
    if (f.paired)
        return false;     // 짝 고리 — 짝 묶음이 확정될 때까지 혼자 안 낸다(풀리면 ReleasePairSide 가 다시 요청한다)
    if (f.bundleNo != 0)
    {
        f.again = true;   // 비행 중 — 회신 뒤 틱 끝에 다시(상태 표 C·E2 → D)
        return false;
    }
    DBSaveBundle head;
    _batchRows.clear();   // 주기 묶음은 이미 넣었다(TickSaves 차례 주기 → 목록) — 같은 버퍼를 다시 쓴다
    PrepareBundle(player, head, _batchRows);
    if (!_dbWorker->EnqueueSave(head, _batchRows.empty() ? nullptr : _batchRows.data(), head.slotCount))
    {
        AbortBundle(player);   // 큐 거절(dropped 는 워커가 셌다) — 표시가 남아 다음 계기에
        return false;
    }
    CommitBundle(player, head, trigger);
    return true;
}

void CCharacterStore::RequestSave(CPlayer* player, SaveReason reason)
{
    CPlayer::DbFlight& f = player->_dbFlight;
    if (f.listed)
        return;   // 이번 틱 끝에 이미 판정한다 — 사유가 여럿이어도 묶음은 하나
    f.listed = true;
    _tickEndSessions.push_back(player->_sessionId);
    _gameMonitor._save._request[static_cast<int>(reason)].Inc();
}

void CCharacterStore::TickSaves()
{
    // ① 주기 — 판정 참인 전원을 한 번에(슬롯당 락 1회). 비행 중이면 다시 필요만.
    //   목록보다 먼저 — 주기가 목록의 사람까지 실어, 목록 차례에선 판정이 거짓이라 헛 묶음이 없다.
    if (++_dbSaveFrameCounter >= _dbSavePeriodFrames)
    {
        _dbSaveFrameCounter = 0;
        _batchHeads.clear();
        _batchRows.clear();
        _batchOwners.clear();
        _sessionHub.ForEachPlayer([&](CPlayer* player)
        {
            if (!ShouldSave(player))
                return;
            if (player->_dbFlight.paired)
                return;   // 짝 고리 — 아래 짝 다시 싣기가 이 사람의 지금 값을 짝 묶음으로 싣는다
            if (player->_dbFlight.bundleNo != 0)
            {
                player->_dbFlight.again = true;
                return;
            }
            DBSaveBundle head;
            PrepareBundle(player, head, _batchRows);
            _batchHeads.push_back(head);
            _batchOwners.push_back(player);
        });
        _dbWorker->EnqueueSaveBatch(_batchHeads, _batchRows, &_batchRefused);
        for (size_t idx : _batchRefused)   // 거절된 묶음 — 그 자리에서 되살린다(상태 표 B·E2 거절)
        {
            AbortBundle(_batchOwners[idx]);
            _batchOwners[idx] = nullptr;
        }
        for (size_t i = 0; i < _batchOwners.size(); ++i)
        {
            if (_batchOwners[i] != nullptr)
                CommitBundle(_batchOwners[i], _batchHeads[i], SaveTrigger::PERIOD);
        }

        // 실패한 떠난 기록 다시 싣기 — 살아 있는 사람과 같은 간격(주기). 죽은 DB 를 틱마다 두드리지 않는다.
        //   붙잡힌 기록(짝 고리)은 혼자 안 낸다 — 아래 짝 묶음의 한쪽으로 간다.
        for (auto& kv : _leaveRecords)
        {
            if (!kv.second.inFlight && !kv.second.paired)
                SendLeaveRecord(kv.second, SaveTrigger::LEAVE);
        }

        // 실패·거절된 짝 다시 싣기 — 같은 간격. 짝째로만(두 사람의 지금 값 · 나간 쪽은 붙잡힌 떠난 기록)
        for (PairLink& link : _pairs)
        {
            if (!link.inFlight)
                SendPair(link, SaveTrigger::TRADE);
        }
    }

    // ② 틱 끝 목록 — 세션 번호로 찾는다(같은 틱에 나간 사람은 없다 — 표시는 나갈 때 묶음이 실었다)
    for (SessionId sessionId : _tickEndSessions)
    {
        CPlayer* player = _sessionHub.FindPlayer(sessionId);
        if (player == nullptr)
            continue;
        player->_dbFlight.listed = false;
        if (ShouldSave(player))
            SendBundle(player, SaveTrigger::TICK_END);
    }
    _tickEndSessions.clear();
}

void CCharacterStore::FillFinalRows(CPlayer* player, std::vector<DBInvRow>& rows)
{
#if USE_INVENTORY
    // 표시 칸 ∪ 비행 중 칸 — 비행 중이던 묶음이 나중에 실패해도 그 칸이 빠지지 않게(최종값이라 두 번 써도 같다)
    const CInventory::SlotBits bits = player->_inventory.TakeUnsaved() | player->_dbFlight.slots;
    player->_dbFlight.slots.reset();
    for (int i = 0; i < INVENTORY_ALL_SLOTS; ++i)
    {
        if (!bits.test(static_cast<size_t>(i)))
            continue;
        const Item& s = player->_inventory.At(i);
        rows.push_back(DBInvRow{ static_cast<int32_t>(CInventory::SlotNumberOf(i)), s.itemId, s.count });
    }
#else
    (void)player;
    (void)rows;
#endif
}

void CCharacterStore::SaveOnLeave(CPlayer* player)
{
    // 나갈 때 — 판정 없이 늘 쓴다(안전망 · 지금과 같음). 바로 큐에 — 같은 캐릭터 재접속의 불러오기가 이 뒤에 줄 선다.
    LeaveFinal(player, SaveTrigger::LEAVE);
}

void CCharacterStore::LeaveFinal(CPlayer* player, SaveTrigger trigger)
{
    DBSaveBundle head{};
    head.parts     = SAVE_PART_ALL;   // 판정 없이 다 쓴다(안전망) — ★ 0 은 「안 씀」이라 이 줄이 빠지면 나갈 때 행이 안 써진다
    head.row       = MakeCharRow(player);
    head.sessionId = NO_OWNER_SESSION;   // 주인 = 떠난 기록(회신은 캐릭터 번호로 찾는다)
    std::vector<DBInvRow> rows;
    FillFinalRows(player, rows);
    if (player->_dbFlight.bundleNo != 0)
    {
        player->_dbFlight.bundleNo = 0;   // 비행 중이던 묶음의 회신은 주인 없음으로 떨어진다 — 칸은 위 마지막 묶음에 실렸다
        SetInflight(-1);
    }
    // 짝 고리 — 떠난 기록을 붙잡는다(큐에 안 넣음). 비행 중이던 것이 짝 묶음이면 그 짝은 다른 큐에 있을 수 있어,
    //   지금 내면 이 기록이 먼저 써지고 늦게 온 짝 묶음이 그보다 옛 값으로 덮는다(trade-plan.md 「위험」). 짝이 확정되면 그때 혼자 낸다.
    //   고리의 이쪽은 이제 떠난 기록이다(세션 0) — 짝을 다시 낼 때 이 기록을 그 쪽 머리로 쓴다.
    const bool hold = player->_dbFlight.paired;
    if (hold)
    {
        int side = 0;
        if (PairLink* link = FindPair(player->_characterId, &side))
            link->session[side] = NO_OWNER_SESSION;
        player->_dbFlight.paired = false;   // 플레이어는 곧 해제된다 — 고리는 기록 쪽이 든다
    }
    PutLeaveRecord(head, std::move(rows), trigger, hold);
}

void CCharacterStore::PutLeaveRecord(DBSaveBundle head, std::vector<DBInvRow>&& rows, SaveTrigger trigger, bool hold)
{
    const CharacterId characterId{ head.row.characterId };
    auto it = _leaveRecords.find(characterId);
    if (it != _leaveRecords.end())
    {
        // 있을 수 없다 — 확정 전엔 같은 캐릭터 불러오기를 실패로 돌려(ProcessDbResults) 그 캐릭터이 다시 들어와 나갈 수 없다.
        //   생기면 새 것으로 덮는다(최종값이라 새 것이 맞다).
        SLOG_ERROR("[DB] leave record overwritten (character={}, old bundle={})", characterId, it->second.head.bundleNo);
    }
    LeaveRecord& rec = _leaveRecords[characterId];
    rec.head           = head;
    rec.rows           = std::move(rows);
    rec.head.slotCount = static_cast<int32_t>(rec.rows.size());
    rec.inFlight       = false;
    rec.paired         = hold;
    if (!hold)
        SendLeaveRecord(rec, trigger);
    _gameMonitor._save._pendingLeave.Store(static_cast<int64_t>(_leaveRecords.size()));
}

void CCharacterStore::SendLeaveRecord(LeaveRecord& rec, SaveTrigger trigger)
{
    // 실을 때마다 새 번호 — 늦게 온 옛 시도의 회신이 새 시도를 확정하지 않게
    if (++_leaveSeq == 0)
        _leaveSeq = 1;
    rec.head.bundleNo  = _leaveSeq;
    rec.head.slotBegin = 0;
    if (_dbWorker->EnqueueSave(rec.head, rec.rows.empty() ? nullptr : rec.rows.data(), rec.head.slotCount))
    {
        rec.inFlight = true;
        _gameMonitor._save._txn[static_cast<int>(trigger)].Inc();
    }
    else
    {
        rec.inFlight = false;   // 큐 거절 — 기록이 남아 다음 주기(또는 같은 캐릭터 불러오기 앞)에 다시
    }
}

void CCharacterStore::OnSaveReply(const DBSaveReply& reply)
{
    CGameMonitor::SaveCounters& counters = _gameMonitor._save;

    if (reply.sessionId != NO_OWNER_SESSION)
    {
        // 살아 있는 사람 — 세션 번호 전체 + 캐릭터 + 비행 번호가 다 맞아야 주인이다
        CPlayer* player = _sessionHub.FindPlayer(reply.sessionId);
        if (player == nullptr || player->_characterId != reply.characterId || player->_dbFlight.bundleNo != reply.bundleNo)
        {
            counters._reply[static_cast<int>(SaveReplyResult::ORPHAN)].Inc();   // 나간 사람 · 같은 캐릭터 재접속 전의 묶음
            return;
        }
        CPlayer::DbFlight& f = player->_dbFlight;
        f.bundleNo = 0;
        SetInflight(-1);
        if (reply.ok)
        {
#if USE_INVENTORY
            f.slots.reset();   // 그 칸은 DB 에 확정됐다
#endif
            counters._reply[static_cast<int>(SaveReplyResult::OK)].Inc();
            if (f.again)
            {
                f.again = false;
                RequestSave(player, SaveReason::AGAIN);   // 비행 중에 선 다시 필요 — 이번 틱 끝에
            }
        }
        else
        {
            AbortBundle(player);   // 되살림 — 곧바로 다시 안 보낸다(다음 계기에 · 죽은 DB 를 틱마다 두드리지 않게)
            f.again = false;
            counters._reply[static_cast<int>(SaveReplyResult::FAILED)].Inc();
        }
        return;
    }

    // 떠난 기록 — 캐릭터 + 번호
    auto it = _leaveRecords.find(reply.characterId);
    if (it == _leaveRecords.end() || !it->second.inFlight || it->second.head.bundleNo != reply.bundleNo)
    {
        counters._reply[static_cast<int>(SaveReplyResult::ORPHAN)].Inc();
        return;
    }
    if (reply.ok)
    {
        _leaveRecords.erase(it);
        counters._reply[static_cast<int>(SaveReplyResult::OK)].Inc();
    }
    else
    {
        it->second.inFlight = false;   // 다음 주기 · 같은 캐릭터 불러오기 앞에 다시 싣는다
        counters._reply[static_cast<int>(SaveReplyResult::FAILED)].Inc();
    }
    counters._pendingLeave.Store(static_cast<int64_t>(_leaveRecords.size()));
}

static int64_t SteadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool CCharacterStore::RequestCharacterLoad(SessionId sessionId, CharacterId characterId)
{
    DBLoadJob job;
    job.characterId = characterId;
    job.sessionId = sessionId;       // 결과가 돌아왔을 때 주인을 찾는 열쇠 — 세션 번호 "전체"
    job.requestNs = SteadyNowNs();

    // 그 캐릭터의 떠난 기록이 실패 중이면 먼저 다시 싣는다 — 같은 슬롯 큐라 이 불러오기가 그 뒤에 줄 서서, 성공하면 새 값을 읽는다.
    //   비행 중이면 그대로 둔다(이미 앞에 있다). 확정 전에 온 불러오기 결과는 ProcessDbResults 가 실패로 돌린다.
    //   붙잡힌 기록(짝 고리)은 안 낸다 — 혼자 내면 다른 큐의 짝 묶음과 차례가 뒤집힌다. 기록이 있는 한 결과는 실패로 돌려진다(아래).
    auto leave = _leaveRecords.find(characterId);
    if (leave != _leaveRecords.end() && !leave->second.inFlight && !leave->second.paired)
        SendLeaveRecord(leave->second, SaveTrigger::LEAVE);

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

    // 둘 다 직전 틱에 모두 소비되어 비어 있다 — 빈 벡터를 워커 쪽과 맞바꾼다(락 1회 · 저장 회신과 불러오기 결과를 같이).
    _dbWorker->SwapOutResults(_dbResults, _dbSaveReplies);

    // ★ 저장 회신 먼저 — 같은 캐릭터의 묶음 회신은 뒤 불러오기 결과보다 먼저 들어온다(같은 큐 차례 · 묶음마다 곧바로 회신).
    //   회신을 다 처리한 뒤 불러오기 결과를 보면, 그 캐릭터의 떠난 기록이 확정됐는지 이미 안다(아래 불러오기 차례).
    for (const DBSaveReply& reply : _dbSaveReplies)
    {
        OnSaveReply(reply);
        OnPairReply(reply);   // 뒤 — 떠난 기록 쪽 머리가 먼저 지워져야 고리를 풀 때 「짝에 안 실린 붙잡힌 기록」만 남는다
    }
    _dbSaveReplies.clear();

    if (_dbResults.empty())
        return;

    const int64_t nowNs = SteadyNowNs();

    static_assert(DB_JOB_KIND_COUNT == 2, "DBJobKind 를 늘렸으면 아래 switch 에도 갈래를 넣을 것");
    for (const DBResult& result : _dbResults)
    {
        switch (result.kind)
        {
        case DBJobKind::LOAD_CHARACTER:
            // 불러오기 차례 — 그 캐릭터의 떠난 기록이 아직 확정 안 됐으면 이 결과는 옛 값일 수 있다 → 실패로 돌려 끊는다
            //   (받는 쪽 FAILED 분기 그대로 — 클라가 다시 접속하는 것이 재시도다). 옛 값으로 들여보내면 아이템이 되돌아간다.
            if (result.code != DBResultCode::FAILED && _leaveRecords.count(result.characterId) != 0)
            {
                SLOG_WARN("[DB] load blocked — leave record not confirmed yet (character={}, session={})",
                          result.characterId, result.sessionId);
                DBResult blocked = result;
                blocked.code = DBResultCode::FAILED;
                _loadEvents.OnCharacterLoaded(blocked, nowNs);
                break;
            }
            _loadEvents.OnCharacterLoaded(result, nowNs);   // 받는 쪽이 판정·스폰한다(알림 — 게임 서버 나누기 0-③ ⑷)
            break;

        case DBJobKind::SAVE_BUNDLE:     // 저장 묶음의 회신은 저장 회신 목록으로 온다 — 불러오기 결과 목록에 오면 결함
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
        SLOG_WARN("[INV] load skip {} (character={}, slot={}, item={}, count={})",
                  InvLoadSkipName(why), player->_characterId, r.slot, r.itemId, r.count);
    };

    // 가진 돈 — 망가진 값(음수 · 상한 초과)은 안 넣는다(0 으로 시작). 서버는 상한 밖을 쓰지 않으므로 DB 를 손으로 고쳤거나 깨졌을 때만 닿고,
    //   그 값은 크기도 믿을 수 없다 — 잘라 넣으면 깨진 큰 값이 1억을 쥐여 준다(npc-shop-plan.md 따라 나온 것 7 · 10-21 사용자 정정).
    //   다음 저장이 0 으로 덮으므로 캐릭터과 원래 값을 로그에 남긴다 — 되살리기는 그 값으로 손으로. 입장은 안 막는다(쿨타임·버프 칸 불량과 같은 규칙)
    if (result.money >= 0 && result.money <= MONEY_MAX)
    {
        player->_inventory.LoadMoney(result.money);
    }
    else
    {
        counters._loadSkipped[static_cast<int>(InvLoadSkip::BAD_MONEY)].Inc();
        SLOG_ERROR("[INV] load skip bad_money (character={}, money={}) — 0 으로 시작 · 다음 저장이 이 값을 덮는다(되살리기는 이 줄의 값으로)",
                   player->_characterId, result.money);
    }

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
        SLOG_WARN("[INV] load skip too_many_rows x{} (character={}) — 결과 자리 {} 를 넘은 줄",
                  result.invOverflow, player->_characterId, DB_INV_MAX_ROWS);
    }
}
#endif
#endif // USE_INVENTORY

#endif // USE_DB_WORKER
