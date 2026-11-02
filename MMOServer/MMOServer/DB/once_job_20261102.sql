-- ==========================================================================
-- One-time: give every existing character a random job (2026-11-02, jobs added).
--
-- Why: jobs came after these rows were saved, so they all hold job 0 ("not chosen").
--   Without this file the server still runs: each of them just sees the choosing window on the next enter.
--
-- NOT part of schema.sql on purpose - schema.sql only adds the column.
--   This file touches only rows still at job 0, so running it twice does not reroll anyone who has a job.
--
-- Jobs (JobTable.csv): 1 warrior, 2 rogue, 3 mage, 4 taoist.
--
-- How (once, after schema.sql):
--   1) Stop the server first, so nothing is saved while this runs.
--   2) "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -u root -p -e "source <full path>/once_job_20261102.sql"
--   3) Compare the two SELECT outputs (before / after). After it, job 0 should be gone.
-- ==========================================================================

USE gamedb;

SELECT 'before' AS step, job, COUNT(*) AS characters FROM characters GROUP BY job ORDER BY job;

UPDATE characters SET job = 1 + FLOOR(RAND() * 4) WHERE job = 0;

SELECT 'after' AS step, job, COUNT(*) AS characters FROM characters GROUP BY job ORDER BY job;
