-- ==========================================================================
-- Account server - account table
--
-- Apply once as root (the game accounts cannot create tables):
--   mysql.exe --login-path=mmo_root -e "source <full path>/schema.sql"
--
-- This is a different database from the game DB on purpose: an account is one
-- across all worlds, a game DB is one per world. character_roster.account_id has
-- no foreign key to this table because the two can live on different servers.
--
-- Accounts (created by hand, passwords live in Run/bin/AccountServerConfig.ini):
--   account_server@127.0.0.1 : SELECT, INSERT, UPDATE on accountdb.accounts. No DELETE.
--
-- No password column yet: it comes with the login server. No status column either
-- (banned, withdrawn) until a rule uses one.
-- ==========================================================================

CREATE DATABASE IF NOT EXISTS accountdb CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS accountdb.accounts (
    account_id    BIGINT      NOT NULL AUTO_INCREMENT PRIMARY KEY,
    login_name    VARCHAR(16) NOT NULL,
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    last_login_at DATETIME(3) NULL,
    UNIQUE KEY uq_accounts_login_name (login_name)
) ENGINE=InnoDB;
