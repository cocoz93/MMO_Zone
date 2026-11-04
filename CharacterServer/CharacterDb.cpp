#include "CharacterDb.h"

#ifdef _WIN32
#include <winsock2.h>   // mysql.h 이전에 — 구버전 winsock.h 가 끌려오지 않게
#include <mysql.h>
#else
#include <mysql/mysql.h>
#endif

#include <cstring>
#include <string>

#include <Common/ErrorLog.h>

#include "CharacterRules.h"

namespace
{
    constexpr unsigned int ER_DUPLICATE_KEY = 1062;   // ER_DUP_ENTRY
    constexpr int MAP_NOT_ENTERED = -1;               // 「아직 입장 안 함」 — 맵 표는 0~1000 만 허용한다(플랜 3절)
    constexpr int NAME_COLUMN_BYTES = CHAR_NAME_MAX_LEN * 4;   // VARCHAR(6) utf8mb4

    MYSQL_BIND BindLong(int64_t& value)
    {
        MYSQL_BIND bind;
        std::memset(&bind, 0, sizeof(bind));
        bind.buffer_type = MYSQL_TYPE_LONGLONG;
        bind.buffer = &value;
        return bind;
    }

    MYSQL_BIND BindInt(int& value)
    {
        MYSQL_BIND bind;
        std::memset(&bind, 0, sizeof(bind));
        bind.buffer_type = MYSQL_TYPE_LONG;
        bind.buffer = &value;
        return bind;
    }

    MYSQL_BIND BindText(enum_field_types type, const void* data, unsigned long& length)
    {
        MYSQL_BIND bind;
        std::memset(&bind, 0, sizeof(bind));
        bind.buffer_type = type;
        bind.buffer = const_cast<void*>(data);
        bind.buffer_length = length;
        bind.length = &length;
        return bind;
    }

    // 결과 없는 문장. 실패하면 false — 실패 표시까지 끝낸다
    bool ExecStmt(CDbConnection& conn, const char* sql, MYSQL_BIND* binds, const char* what, my_ulonglong* affected = nullptr)
    {
        MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(conn.Prepare(sql));
        if (stmt == nullptr)
            return false;
        if (mysql_stmt_bind_param(stmt, binds) != 0 || mysql_stmt_execute(stmt) != 0)
        {
            conn.NoteStmtError(stmt, what);
            return false;
        }
        if (affected != nullptr)
            *affected = mysql_stmt_affected_rows(stmt);
        return true;
    }

    bool ReleaseExpiredSlots(CDbConnection& conn, int64_t account, int graceDays)
    {
        MYSQL_BIND binds[2] = { BindLong(account), BindInt(graceDays) };
        return ExecStmt(conn,
            "UPDATE character_roster SET slot=NULL "
            "WHERE account_id=? AND slot IS NOT NULL AND deleted_at IS NOT NULL "
            "AND deleted_at <= CURRENT_TIMESTAMP(3) - INTERVAL ? DAY",
            binds, "roster release");
    }

    struct OwnerRow
    {
        bool    queryOk = false;
        bool    found = false;
        int64_t account = 0;
        bool    hasSlot = false;
        bool    hasDeletedAt = false;
        int64_t elapsedSec = 0;
    };

    OwnerRow ReadOwner(CDbConnection& conn, int64_t character)
    {
        OwnerRow row;
        MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(conn.Prepare(
            "SELECT account_id, slot, TIMESTAMPDIFF(SECOND, deleted_at, CURRENT_TIMESTAMP(3)) "
            "FROM character_roster WHERE character_id=?"));
        if (stmt == nullptr)
            return row;

        MYSQL_BIND in = BindLong(character);
        if (mysql_stmt_bind_param(stmt, &in) != 0 || mysql_stmt_execute(stmt) != 0)
        {
            conn.NoteStmtError(stmt, "roster owner");
            return row;
        }

        int64_t account = 0;
        int slot = 0;
        int64_t elapsed = 0;
        bool slotNull = false;
        bool elapsedNull = false;
        MYSQL_BIND out[3] = { BindLong(account), BindInt(slot), BindLong(elapsed) };
        out[1].is_null = &slotNull;
        out[2].is_null = &elapsedNull;
        if (mysql_stmt_bind_result(stmt, out) != 0)
        {
            conn.NoteStmtError(stmt, "roster owner bind");
            return row;
        }

        const int fetched = mysql_stmt_fetch(stmt);
        if (fetched != 0 && fetched != MYSQL_NO_DATA)
        {
            conn.NoteStmtError(stmt, "roster owner fetch");
            return row;
        }
        mysql_stmt_free_result(stmt);

        row.queryOk = true;
        row.found = (fetched == 0);
        if (row.found)
        {
            row.account = account;
            row.hasSlot = !slotNull;
            row.hasDeletedAt = !elapsedNull;
            row.elapsedSec = row.hasDeletedAt ? elapsed : 0;
        }
        return row;
    }

    // 1062 뒤 — 같은 계정 · 자리 · 이름 행이면 앞선 시도가 이미 들어간 것이다
    bool FindSameCreate(CDbConnection& conn, int64_t account, int slot, const std::string& name, int64_t& outCharacter, bool& queryOk)
    {
        queryOk = false;
        MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(conn.Prepare(
            "SELECT character_id FROM character_roster "
            "WHERE account_id=? AND slot=? AND name=? AND deleted_at IS NULL"));
        if (stmt == nullptr)
            return false;

        unsigned long nameLength = static_cast<unsigned long>(name.size());
        MYSQL_BIND in[3] = { BindLong(account), BindInt(slot), BindText(MYSQL_TYPE_STRING, name.data(), nameLength) };
        if (mysql_stmt_bind_param(stmt, in) != 0 || mysql_stmt_execute(stmt) != 0)
        {
            conn.NoteStmtError(stmt, "roster same");
            return false;
        }

        int64_t character = 0;
        MYSQL_BIND out = BindLong(character);
        if (mysql_stmt_bind_result(stmt, &out) != 0)
        {
            conn.NoteStmtError(stmt, "roster same bind");
            return false;
        }

        const int fetched = mysql_stmt_fetch(stmt);
        if (fetched != 0 && fetched != MYSQL_NO_DATA)
        {
            conn.NoteStmtError(stmt, "roster same fetch");
            return false;
        }
        mysql_stmt_free_result(stmt);

        queryOk = true;
        if (fetched != 0)
            return false;
        outCharacter = character;
        return true;
    }
}

const char* CCharacterDb::OpName(Op op)
{
    switch (op)
    {
    case OP_LIST:    return "list";
    case OP_CREATE:  return "create";
    case OP_DELETE:  return "delete";
    case OP_RESTORE: return "restore";
    case OP_SELECT:  return "select";
    case OP_COUNT:   break;
    }
    return "unknown";
}

CCharacterDb::ListResult CCharacterDb::LoadList(CDbConnection& conn, AccountId account, int graceDays)
{
    ListResult result;
    if (!conn.IsOpen())
    {
        conn.NoteNotOpen();
        return result;
    }

    int64_t accountValue = ToInt(account);
    if (!ReleaseExpiredSlots(conn, accountValue, graceDays))
        return result;

    MYSQL_STMT* stmt = static_cast<MYSQL_STMT*>(conn.Prepare(
        "SELECT r.character_id, r.slot, r.name, c.job, c.level, r.look, "
        "TIMESTAMPDIFF(SECOND, r.deleted_at, CURRENT_TIMESTAMP(3)) "
        "FROM character_roster r JOIN characters c ON c.character_id = r.character_id "
        "WHERE r.account_id=? AND r.slot IS NOT NULL ORDER BY r.slot"));
    if (stmt == nullptr)
        return result;

    MYSQL_BIND in = BindLong(accountValue);
    if (mysql_stmt_bind_param(stmt, &in) != 0 || mysql_stmt_execute(stmt) != 0)
    {
        conn.NoteStmtError(stmt, "roster list");
        return result;
    }

    int64_t character = 0;
    int slot = 0;
    char name[NAME_COLUMN_BYTES + 1]{};
    unsigned long nameLength = sizeof(name);
    int job = 0;
    int level = 0;
    uint8_t look[CHAR_LOOK_BYTES]{};
    unsigned long lookLength = sizeof(look);
    int64_t elapsed = 0;
    bool elapsedNull = false;

    MYSQL_BIND out[7] = {
        BindLong(character), BindInt(slot), BindText(MYSQL_TYPE_STRING, name, nameLength),
        BindInt(job), BindInt(level), BindText(MYSQL_TYPE_BLOB, look, lookLength), BindLong(elapsed)
    };
    out[6].is_null = &elapsedNull;
    if (mysql_stmt_bind_result(stmt, out) != 0)
    {
        conn.NoteStmtError(stmt, "roster list bind");
        return result;
    }

    for (;;)
    {
        std::memset(look, 0, sizeof(look));
        const int fetched = mysql_stmt_fetch(stmt);
        if (fetched == MYSQL_NO_DATA)
            break;
        if (fetched != 0 && fetched != MYSQL_DATA_TRUNCATED)
        {
            conn.NoteStmtError(stmt, "roster list fetch");
            return result;
        }

        if (static_cast<int>(result.rows.size()) >= CHAR_LIST_MAX)
        {
            SLOG_ERROR("[CharacterDb] 계정 {} 의 줄이 목록 상한 {} 을 넘음 — 뒤는 안 보냄", accountValue, CHAR_LIST_MAX);
            continue;
        }

        CharListEntry row{};
        row.characterId = character;
        row.slot = static_cast<int8_t>(slot);
        row.job = job;
        row.level = level;
        std::memcpy(row.look, look, sizeof(row.look));
        char16_t wireName[CHAR_NAME_MAX_LEN];   // 패킷 칸은 줄이 안 맞아 참조로 못 넘긴다
        if (!CharacterName::FromUtf8(name, nameLength, wireName))
            SLOG_ERROR("[CharacterDb] 캐릭터 {} 이름이 글자 규칙에 어긋남 — 손으로 고친 행이다", character);
        std::memcpy(row.name, wireName, sizeof(row.name));
        row.deleteLeftSec = elapsedNull ? 0 : CCharacterRules::GraceLeftSec(elapsed, graceDays);
        result.rows.push_back(row);
    }
    mysql_stmt_free_result(stmt);

    result.ok = true;
    return result;
}

CCharacterDb::CreateOutcome CCharacterDb::Create(CDbConnection& conn, const CreateRequest& request, int graceDays)
{
    CreateOutcome outcome;
    if (!conn.IsOpen())
    {
        conn.NoteNotOpen();
        return outcome;
    }

    int64_t account = ToInt(request.account);
    int slot = request.slot;
    int job = request.job;
    const std::string name = CharacterName::ToUtf8(request.name);

    // 트랜잭션 밖에서 — 뒤의 ROLLBACK 이 풀린 자리까지 되돌리면 안 된다
    if (!ReleaseExpiredSlots(conn, account, graceDays))
        return outcome;

    if (!conn.Exec("START TRANSACTION"))
        return outcome;

    const auto abortWithFail = [&conn]() { conn.Exec("ROLLBACK"); };

    MYSQL_STMT* insertRoster = static_cast<MYSQL_STMT*>(conn.Prepare(
        "INSERT INTO character_roster(account_id, slot, name, look) VALUES(?,?,?,?)"));
    if (insertRoster == nullptr)
    {
        abortWithFail();
        return outcome;
    }

    unsigned long nameLength = static_cast<unsigned long>(name.size());
    unsigned long lookLength = CharacterLookBlob::V1_BYTES;
    MYSQL_BIND rosterBinds[4] = {
        BindLong(account), BindInt(slot),
        BindText(MYSQL_TYPE_STRING, name.data(), nameLength),
        BindText(MYSQL_TYPE_BLOB, request.look, lookLength)
    };
    if (mysql_stmt_bind_param(insertRoster, rosterBinds) != 0)
    {
        conn.NoteStmtError(insertRoster, "roster insert bind");
        abortWithFail();
        return outcome;
    }
    if (mysql_stmt_execute(insertRoster) != 0)
    {
        if (mysql_stmt_errno(insertRoster) != ER_DUPLICATE_KEY)
        {
            conn.NoteStmtError(insertRoster, "roster insert");
            abortWithFail();
            return outcome;
        }

        // 유일 키에 막힌 것은 실패가 아니다 — 어느 키인지 갈라 답한다
        const CCharacterRules::DuplicateKey key = CCharacterRules::ClassifyDuplicate(mysql_stmt_error(insertRoster));
        if (key == CCharacterRules::DuplicateKey::Unknown)
        {
            conn.NoteStmtError(insertRoster, "roster insert(모르는 유일 키)");   // 키 이름이 바뀌면 여기로 와서 눈에 띈다
            abortWithFail();
            return outcome;
        }
        if (!conn.Exec("ROLLBACK"))
            return outcome;

        int64_t sameCharacter = 0;
        bool queryOk = false;
        if (FindSameCreate(conn, account, slot, name, sameCharacter, queryOk))
        {
            outcome.result = CharCreateResult::OK;
            outcome.characterId = CharacterId{ sameCharacter };
            outcome.list = LoadList(conn, request.account, graceDays);
            return outcome;
        }
        if (!queryOk)
            return outcome;

        outcome.result = (key == CCharacterRules::DuplicateKey::Name) ? CharCreateResult::TAKEN : CharCreateResult::SLOT_TAKEN;
        return outcome;
    }

    int64_t character = static_cast<int64_t>(mysql_stmt_insert_id(insertRoster));
    int mapId = MAP_NOT_ENTERED;
    MYSQL_BIND characterBinds[3] = { BindLong(character), BindInt(mapId), BindInt(job) };
    if (!ExecStmt(conn, "INSERT INTO characters(character_id, map_id, job) VALUES(?,?,?)", characterBinds, "characters insert"))
    {
        abortWithFail();
        return outcome;
    }

    if (!conn.Exec("COMMIT"))
        return outcome;

    outcome.result = CharCreateResult::OK;
    outcome.characterId = CharacterId{ character };
    outcome.list = LoadList(conn, request.account, graceDays);   // 실패해도 만들기는 성공이다 — 목록만 안 간다
    return outcome;
}

CCharacterDb::DeleteOutcome CCharacterDb::Delete(CDbConnection& conn, AccountId account, CharacterId character, bool inGame, int graceDays)
{
    DeleteOutcome outcome;
    if (!conn.IsOpen())
    {
        conn.NoteNotOpen();
        return outcome;
    }

    const OwnerRow owner = ReadOwner(conn, ToInt(character));
    if (!owner.queryOk)
        return outcome;
    if (!owner.found || owner.account != ToInt(account))
    {
        outcome.result = CharDeleteResult::NOT_MINE;
        return outcome;
    }
    if (inGame)
    {
        outcome.result = CharDeleteResult::IN_GAME;
        return outcome;
    }

    if (!owner.hasDeletedAt)   // 이미 지웠으면 그대로 OK — 앞선 회신이 유실된 경우다
    {
        int64_t characterValue = ToInt(character);
        int64_t accountValue = ToInt(account);
        MYSQL_BIND binds[2] = { BindLong(characterValue), BindLong(accountValue) };
        if (!ExecStmt(conn, "UPDATE character_roster SET deleted_at=CURRENT_TIMESTAMP(3) WHERE character_id=? AND account_id=? AND deleted_at IS NULL",
                      binds, "roster delete"))
            return outcome;
    }

    outcome.result = CharDeleteResult::OK;
    outcome.list = LoadList(conn, account, graceDays);
    return outcome;
}

CCharacterDb::RestoreOutcome CCharacterDb::Restore(CDbConnection& conn, AccountId account, CharacterId character, int graceDays)
{
    RestoreOutcome outcome;
    if (!conn.IsOpen())
    {
        conn.NoteNotOpen();
        return outcome;
    }

    const OwnerRow owner = ReadOwner(conn, ToInt(character));
    if (!owner.queryOk)
        return outcome;
    if (!owner.found || owner.account != ToInt(account))
    {
        outcome.result = CharRestoreResult::NOT_MINE;
        return outcome;
    }

    switch (CCharacterRules::ClassifyDelete(owner.hasDeletedAt, owner.elapsedSec, graceDays))
    {
    case CCharacterRules::DeleteState::Alive:
        break;   // 이미 복구됨 — 그대로 OK
    case CCharacterRules::DeleteState::Expired:
        outcome.result = CharRestoreResult::EXPIRED;
        return outcome;
    case CCharacterRules::DeleteState::Pending:
    {
        if (!owner.hasSlot)   // 손으로 고치지 않는 한 안 생기는 조합 — 돌아갈 자리가 없다
        {
            outcome.result = CharRestoreResult::EXPIRED;
            return outcome;
        }
        int64_t characterValue = ToInt(character);
        int64_t accountValue = ToInt(account);
        MYSQL_BIND binds[2] = { BindLong(characterValue), BindLong(accountValue) };
        if (!ExecStmt(conn, "UPDATE character_roster SET deleted_at=NULL WHERE character_id=? AND account_id=? AND slot IS NOT NULL",
                      binds, "roster restore"))
            return outcome;
        break;
    }
    }

    outcome.result = CharRestoreResult::OK;
    outcome.list = LoadList(conn, account, graceDays);
    return outcome;
}

CharSelectResult CCharacterDb::CheckSelect(CDbConnection& conn, AccountId account, CharacterId character)
{
    if (!conn.IsOpen())
    {
        conn.NoteNotOpen();
        return CharSelectResult::DB_FAIL;
    }

    const OwnerRow owner = ReadOwner(conn, ToInt(character));
    if (!owner.queryOk)
        return CharSelectResult::DB_FAIL;
    if (!owner.found || owner.account != ToInt(account))
        return CharSelectResult::NOT_MINE;
    if (owner.hasDeletedAt)
        return CharSelectResult::DELETED;
    return CharSelectResult::OK;
}
