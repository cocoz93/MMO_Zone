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
-- Accounts (created by hand, passwords live in Run/bin/DbPassword.local.ini, which git ignores):
--   account_server@127.0.0.1 : SELECT, INSERT, UPDATE on accountdb.accounts and accountdb.account_mail. No DELETE.
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

-- Account mail - rewards an operator gives to an account. Only the account server writes it.
--   state: 0 not taken, 1 being taken by claim_character_id, 2 taken, 3 deleted (only mail with no money and no attachment).
--   batch_id = one bulk send. uq_batch stops the same bulk send from reaching an account twice.
--   read_at = first read. A mail counts as new while it is unread or still holds money or an attachment.
CREATE TABLE IF NOT EXISTS accountdb.account_mail (
    mail_id            BIGINT         NOT NULL AUTO_INCREMENT PRIMARY KEY,
    account_id         BIGINT         NOT NULL,
    batch_id           BIGINT         NULL,
    sent_by            BIGINT         NOT NULL,
    title              VARCHAR(20)    NOT NULL,
    body               VARCHAR(200)   NOT NULL,
    money              BIGINT         NOT NULL DEFAULT 0,
    attach             VARBINARY(255) NOT NULL DEFAULT '',
    state              TINYINT        NOT NULL DEFAULT 0,
    claim_character_id BIGINT         NULL,
    created_at         DATETIME(3)    NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    read_at            DATETIME(3)    NULL,
    claim_at           DATETIME(3)    NULL,
    done_at            DATETIME(3)    NULL,
    expires_at         DATETIME(3)    NOT NULL,
    KEY ix_mailbox (account_id, state),
    UNIQUE KEY uq_batch (account_id, batch_id)
) ENGINE=InnoDB;
