-- ==========================================================================
-- One-time move: every existing character -> the town start area (2026-10-12, town map).
--
-- Why: map 0 used to be the snake map (120x120). Since the town map change, map 0 is the
--   town (60x60) and the snake map is the dungeon, map 1 (Run/bin/MapTable.csv).
--   Saved rows still hold map_id 0 with snake-map coordinates, which mean nothing in the town.
--   Without this file the server still runs: a saved spot outside the town (x or y >= 60) falls back
--   to the town start area with a warning log line, and a spot inside stays where it is.
--   This file just puts everyone at the start area (27..33) at once, like a new character.
--
-- NOT part of schema.sql on purpose - schema.sql must be safe to run again, and this one moves
--   everyone back to the town every time it runs.
--
-- How (once):
--   1) Stop the server first. A running server saves dirty players every 60 s and would write
--      the old spots back.
--   2) "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -u root -p -e "source <full path>/once_town_20261012.sql"
--   3) Compare the two SELECT outputs (before / after).
-- ==========================================================================

USE gamedb;

SELECT 'before' AS step, map_id, COUNT(*) AS characters FROM characters GROUP BY map_id;

UPDATE characters SET map_id = 0, x = 30, y = 30;

SELECT 'after' AS step, map_id, COUNT(*) AS characters FROM characters GROUP BY map_id;
