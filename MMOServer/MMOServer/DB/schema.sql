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
    account_id BIGINT      NOT NULL PRIMARY KEY,
    x          FLOAT       NOT NULL DEFAULT 0,
    y          FLOAT       NOT NULL DEFAULT 0,
    map_id     INT         NOT NULL DEFAULT 0,
    level      INT         NOT NULL DEFAULT 1,
    exp        BIGINT      NOT NULL DEFAULT 0,
    hp         INT         NOT NULL DEFAULT -1,
    mp         INT         NOT NULL DEFAULT -1,
    skill_cooldowns VARBINARY(255) NOT NULL DEFAULT '',
    buffs           VARBINARY(255) NOT NULL DEFAULT '',
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
--   Saved with the rest of the row (the same moments as hp/mp above).

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
    account_id BIGINT      NOT NULL,
    slot       INT         NOT NULL,
    item_id    INT         NOT NULL DEFAULT 0,
    count      INT         NOT NULL DEFAULT 0,
    updated_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                           ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (account_id, slot)
) ENGINE=InnoDB;

-- Queries used by CDBWorker (numeric-only params -> no injection surface):
--   (the column lists are built from one column table in DBWorker.cpp - kCharColumns / kInvColumns)
--   Save bundle = character row UPSERT + one slot UPSERT per changed slot.
--     With slots it is wrapped in START TRANSACTION ... COMMIT (ROLLBACK on error);
--     a bundle with no slots is the single row statement (autocommit).
--     INSERT INTO characters (account_id,x,y,map_id,level,exp,hp,mp,skill_cooldowns,buffs)
--       VALUES (?,?,?,?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE x=?,y=?,map_id=?,level=?,exp=?,hp=?,mp=?,skill_cooldowns=?,buffs=?
--     INSERT INTO inventory (account_id,slot,item_id,count) VALUES (?,?,?,?)
--       ON DUPLICATE KEY UPDATE item_id=?, count=?
--   Load:
--     SELECT x,y,map_id,level,exp,hp,mp,skill_cooldowns,buffs FROM characters WHERE account_id=?
--   Inventory load (second statement of the same load job, run even when the
--   character row is missing - see DBWorker.cpp RunLoadOnce):
--     SELECT slot,item_id,count FROM inventory WHERE account_id=? AND item_id<>0 ORDER BY slot
