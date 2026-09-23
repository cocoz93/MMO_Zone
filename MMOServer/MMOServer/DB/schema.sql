-- ==========================================================================
-- MMO Zone Server - character persistence (Stage 1: save pipeline)
--
-- Apply once before running the server with USE_DB_WORKER=1:
--   "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -u root -p < schema.sql
--
-- Model: server memory is the source of truth, DB is a store.
--        The server flushes only "dirty" (changed) players periodically.
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
    updated_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                           ON UPDATE CURRENT_TIMESTAMP(3)
) ENGINE=InnoDB;

-- ==========================================================================
-- Upgrades for a database created before the column existed.
--   MySQL 8.0 has no "ADD COLUMN IF NOT EXISTS", so each block checks
--   information_schema first. Re-running the whole file is safe.
--   level/exp are one value per character -> extra columns, not a new table.
--   (A new table is only for "one character owns many rows", e.g. inventory.)
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

-- Queries used by CDBWorker (numeric-only params -> no injection surface):
--   Save (UPSERT):
--     INSERT INTO characters (account_id,x,y,map_id,level,exp) VALUES (?,?,?,?,?,?)
--     ON DUPLICATE KEY UPDATE x=?, y=?, map_id=?, level=?, exp=?
--   Load:
--     SELECT x,y,map_id,level,exp FROM characters WHERE account_id=?
