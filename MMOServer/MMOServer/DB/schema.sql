-- ==========================================================================
-- MMO Zone Server - character persistence (Stage 1: save pipeline)
--
-- Apply once before running the server with USE_DB_WORKER=1:
--   "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -u root -p < schema.sql
--   (PowerShell has no "<" - use: mysql.exe -u root -p -e "source <full path>/schema.sql")
--
-- Must run as root: the game account (game_server) only has SELECT, INSERT, UPDATE.
--   With USE_INVENTORY=1 the server prepares its inventory statements at startup,
--   so a missing inventory table stops the server from starting (by design - better
--   than running with bags that silently never save).
--
-- Model: server memory is the source of truth, DB is a store.
--        The server writes only changed players: every 60 s, and at the end of a
--        tick in which the bag changed or the level went up (one bundle per character).
-- ==========================================================================

CREATE DATABASE IF NOT EXISTS gamedb DEFAULT CHARACTER SET utf8mb4;
USE gamedb;

CREATE TABLE IF NOT EXISTS characters (
    character_id BIGINT      NOT NULL PRIMARY KEY,
    x          FLOAT       NOT NULL DEFAULT 0,
    y          FLOAT       NOT NULL DEFAULT 0,
    map_id     INT         NOT NULL DEFAULT 0,
    level      INT         NOT NULL DEFAULT 1,
    exp        BIGINT      NOT NULL DEFAULT 0,
    hp         INT         NOT NULL DEFAULT -1,
    mp         INT         NOT NULL DEFAULT -1,
    skill_cooldowns VARBINARY(255) NOT NULL DEFAULT '',
    buffs           VARBINARY(255) NOT NULL DEFAULT '',
    money      BIGINT      NOT NULL DEFAULT 0,
    job        INT         NOT NULL DEFAULT 0,
    updated_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                           ON UPDATE CURRENT_TIMESTAMP(3)
) ENGINE=InnoDB;
-- hp/mp = current HP/MP. Max HP/MP are not stored - the server rebuilds them from the
--   level table and equipment on enter, then cuts the saved value down to the max.
--   -1 = never saved (rows from before these columns) -> full on enter.
--    0 = saved while dead (hp only) -> the server applies its respawn rule on enter
--        (outside the town -> town start area, full HP), so nobody logs in dead.
--   Saved with the whole row (every 60 s if changed, at the end of a tick in which the
--   bag changed or the level went up, on logout, on shutdown), not on every hit - losing
--   up to one period of HP on a crash is acceptable when the bag did not change.
-- skill_cooldowns / buffs = packed bytes, one format per column (CooldownBuffBlob.h,
--   version byte first). A character owns several of each, but they are short-lived and
--   always read and written whole with the row, never searched -> one column each, not a table.
--   Empty = nothing running. Times inside are UTC milliseconds, so they keep running while
--   the player is offline. A bad or unknown value is dropped on enter (that cooldown or buff
--   just ends) - it never blocks the login.
--   Saved with the rest of the row (the same moments as hp/mp above). A buff change alone does not trigger a write.
-- money = jeon the character carries (0 .. 100000000, Item.h MONEY_MAX). Part of the bag, so the column is
--   written and read only by a build with the bag (USE_INVENTORY) - a build without it never overwrites it.
--   Saved with the row (a buy or sell changes slots and money in ONE transaction - the save bundle).
--   A value out of range is not loaded (the character starts with 0 and the server logs the character and the
--   old value) - the server never writes one, so it only appears after a hand edit or a broken row.

-- job = the character's job (JobTable.csv jobId). 0 = not chosen yet -> the server sends 0 on enter and the web
--   shows the choosing window. Set once by C2S_CHOOSE_JOB and saved at that moment; it never changes after that.
--   Rows from before this column: once_job_20261102.sql gives each one a random job.

-- ==========================================================================
-- Upgrades for a database created before the column existed.
--   MySQL 8.0 has no "ADD COLUMN IF NOT EXISTS", so each block checks
--   information_schema first. Re-running the whole file is safe.
--   level/exp are one value per character -> extra columns, not a new table.
--   (A new table is only for "one character owns many rows", e.g. inventory.
--    Exception: short-lived state always read and written whole -> packed bytes in one
--    column, e.g. skill_cooldowns/buffs above.)
-- ==========================================================================
SET @add_level := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE characters ADD COLUMN level INT NOT NULL DEFAULT 1 AFTER map_id',
    'DO 0')
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = 'gamedb' AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'level');
PREPARE s FROM @add_level; EXECUTE s; DEALLOCATE PREPARE s;

SET @add_exp := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE characters ADD COLUMN exp BIGINT NOT NULL DEFAULT 0 AFTER level',
    'DO 0')
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = 'gamedb' AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'exp');
PREPARE s FROM @add_exp; EXECUTE s; DEALLOCATE PREPARE s;

-- hp/mp default -1 (= never saved -> full), NOT 0: existing rows would read as "died".
SET @add_hp := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE characters ADD COLUMN hp INT NOT NULL DEFAULT -1 AFTER exp',
    'DO 0')
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = 'gamedb' AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'hp');
PREPARE s FROM @add_hp; EXECUTE s; DEALLOCATE PREPARE s;

SET @add_mp := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE characters ADD COLUMN mp INT NOT NULL DEFAULT -1 AFTER hp',
    'DO 0')
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = 'gamedb' AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'mp');
PREPARE s FROM @add_mp; EXECUTE s; DEALLOCATE PREPARE s;

-- skill_cooldowns/buffs default '' (= nothing running) - existing rows start with no cooldown.
SET @add_skill_cooldowns := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE characters ADD COLUMN skill_cooldowns VARBINARY(255) NOT NULL DEFAULT '''' AFTER mp',
    'DO 0')
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = 'gamedb' AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'skill_cooldowns');
PREPARE s FROM @add_skill_cooldowns; EXECUTE s; DEALLOCATE PREPARE s;

SET @add_buffs := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE characters ADD COLUMN buffs VARBINARY(255) NOT NULL DEFAULT '''' AFTER skill_cooldowns',
    'DO 0')
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = 'gamedb' AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'buffs');
PREPARE s FROM @add_buffs; EXECUTE s; DEALLOCATE PREPARE s;

SET @add_money := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE characters ADD COLUMN money BIGINT NOT NULL DEFAULT 0 AFTER buffs',
    'DO 0')
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = 'gamedb' AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'money');
PREPARE s FROM @add_money; EXECUTE s; DEALLOCATE PREPARE s;

-- job default 0 (= not chosen) - see the note under the table.
SET @add_job := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE characters ADD COLUMN job INT NOT NULL DEFAULT 0 AFTER money',
    'DO 0')
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = 'gamedb' AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'job');
PREPARE s FROM @add_job; EXECUTE s; DEALLOCATE PREPARE s;

-- ==========================================================================
-- Inventory (USE_INVENTORY) - one row per bag slot.
--   slot = slot number from Protocol.h INV_TAB_RANGES (equip 0.., use 100.., etc 200..).
--   item_id 0 = empty slot. Rows are never deleted - the game account has no DELETE
--   privilege, and one UPSERT covers add / use / empty alike.
--   The server writes the slot's final value at the end of the tick it changed in,
--   together with the character row in ONE transaction (a "save bundle"), so a crash
--   never leaves a potion used without its heal, or a weapon in two slots / none.
--   Final values: writing the same row twice gives the same result and a retry is safe.
-- ==========================================================================
CREATE TABLE IF NOT EXISTS inventory (
    character_id BIGINT      NOT NULL,
    slot       INT         NOT NULL,
    item_id    INT         NOT NULL DEFAULT 0,
    count      INT         NOT NULL DEFAULT 0,
    updated_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                           ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (character_id, slot)
) ENGINE=InnoDB;

-- Queries used by CDBWorker (numeric-only params -> no injection surface):
--   (the column lists are built from one column table in DBWorker.cpp - kCharColumns / kInvColumns)
--   Save bundle = character row UPSERT (only when the row changed - leave/shutdown always)
--     + one slot UPSERT per changed slot.
--     Two or more statements are wrapped in START TRANSACTION ... COMMIT (ROLLBACK on error);
--     a single statement (row only, or one slot only) runs alone (autocommit).
--     INSERT INTO characters (character_id,x,y,map_id,level,exp,hp,mp,skill_cooldowns,buffs,job,money)
--       VALUES (?,?,?,?,?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE x=?,y=?,map_id=?,level=?,exp=?,hp=?,mp=?,skill_cooldowns=?,buffs=?,job=?,money=?
--       (money only in a build with the bag - USE_INVENTORY)
--     INSERT INTO inventory (character_id,slot,item_id,count) VALUES (?,?,?,?)
--       ON DUPLICATE KEY UPDATE item_id=?, count=?
--   Load:
--     SELECT x,y,map_id,level,exp,hp,mp,skill_cooldowns,buffs,job,money FROM characters WHERE character_id=?
--   Inventory load (second statement of the same load job, run even when the
--   character row is missing - see DBWorker.cpp RunLoadOnce):
--     SELECT slot,item_id,count FROM inventory WHERE character_id=? AND item_id<>0 ORDER BY slot

-- Character ownership: who owns which character, name, look, delete state.
-- The character server writes it; the zone server only reads name and look.
--   slot:         0-3. NULL once the delete grace period is over (the seat is given back).
--   name:         2-6 letters. Case-insensitive unique by collation (Shared/Identity/CharacterName.h).
--   look:         versioned blob (Shared/Identity/CharacterLookBlob.h).
--   deleted_at:   delete is a mark, rows are never removed - the character server has no DELETE grant.
-- AUTO_INCREMENT starts at 1e12: load-test dummies use small ids (sessions count up from 1)
-- and the two ranges must never meet.
--
-- Accounts (created by hand, password lives in Run/bin/DbPassword.local.ini, which git ignores):
--   character_server@127.0.0.1 : SELECT, INSERT, UPDATE on gamedb.character_roster;
--                                SELECT, INSERT on gamedb.characters;
--                                SELECT on gamedb.inventory.
--                                No DELETE, nothing on accountdb.
CREATE TABLE IF NOT EXISTS character_roster (
    character_id BIGINT        NOT NULL AUTO_INCREMENT PRIMARY KEY,
    account_id   BIGINT        NOT NULL,
    slot         TINYINT       NULL,
    name         VARCHAR(6)    NOT NULL,
    look         VARBINARY(32) NOT NULL,
    created_at   DATETIME(3)   NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    deleted_at   DATETIME(3)   NULL,
    UNIQUE KEY uq_roster_name (name),
    UNIQUE KEY uq_roster_slot (account_id, slot),
    KEY ix_roster_account (account_id)
) ENGINE=InnoDB AUTO_INCREMENT=1000000000000;

-- ==========================================================================
-- World server: owner columns and boss room lockouts on characters.
--   owner_zone = the zone server (ZoneServerId) that loaded this character last. 0 = no zone server yet.
--   owner_seq  = how many times the owner changed. A zone server raises it only when it takes the row over,
--                so reloading on the same zone server does not fence its own unconfirmed saves.
--   Every save bundle reads both with SELECT ... FOR UPDATE and rolls back when they are not its own.
--   Neither is in kCharColumns: the UPSERT would then overwrite the owner on every save.
--   room_lockouts = packed bytes (RoomLockoutBlob.h, version byte first): map_id + end time in UTC milliseconds.
--                The time keeps running while the player is offline. Empty = no lockout.
-- ==========================================================================
SET @add_owner_zone := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE characters ADD COLUMN owner_zone INT NOT NULL DEFAULT 0 AFTER job',
    'DO 0')
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = 'gamedb' AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'owner_zone');
PREPARE s FROM @add_owner_zone; EXECUTE s; DEALLOCATE PREPARE s;

SET @add_owner_seq := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE characters ADD COLUMN owner_seq BIGINT UNSIGNED NOT NULL DEFAULT 0 AFTER owner_zone',
    'DO 0')
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = 'gamedb' AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'owner_seq');
PREPARE s FROM @add_owner_seq; EXECUTE s; DEALLOCATE PREPARE s;

SET @add_room_lockouts := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE characters ADD COLUMN room_lockouts VARBINARY(255) NOT NULL DEFAULT '''' AFTER owner_seq',
    'DO 0')
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = 'gamedb' AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'room_lockouts');
PREPARE s FROM @add_room_lockouts; EXECUTE s; DEALLOCATE PREPARE s;

-- ==========================================================================
-- Character mail - mail between players. Only the zone server writes it.
--   attach = version byte + slot count + (item_id, count) x slots (MailAttach.h). Empty = no attachment.
--   send_key = random bytes per sent mail: COMMIT can fail after it succeeded, and the bundle is written again.
--   from_account_mail_id = the account mail this row came from. Its UNIQUE key stops one account mail from
--   being taken twice, so rows here must stay as long as the account mail row says "taken".
--   Rows are never deleted - the game account has no DELETE privilege. deleted_at marks them.
-- ==========================================================================
CREATE TABLE IF NOT EXISTS character_mail (
    mail_id              BIGINT         NOT NULL AUTO_INCREMENT PRIMARY KEY,
    character_id         BIGINT         NOT NULL,
    from_character_id    BIGINT         NOT NULL DEFAULT 0,
    from_name            VARCHAR(6)     NOT NULL DEFAULT '',
    title                VARCHAR(20)    NOT NULL,
    body                 VARCHAR(200)   NOT NULL,
    money                BIGINT         NOT NULL DEFAULT 0,
    attach               VARBINARY(255) NOT NULL DEFAULT '',
    send_key             BINARY(16)     NULL,
    from_account_mail_id BIGINT         NULL,
    created_at           DATETIME(3)    NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    read_at              DATETIME(3)    NULL,
    expires_at           DATETIME(3)    NOT NULL,
    taken_at             DATETIME(3)    NULL,
    deleted_at           DATETIME(3)    NULL,
    UNIQUE KEY uq_send_key (send_key),
    UNIQUE KEY uq_from_account (from_account_mail_id),
    KEY ix_mailbox (character_id, taken_at, deleted_at)
) ENGINE=InnoDB;
