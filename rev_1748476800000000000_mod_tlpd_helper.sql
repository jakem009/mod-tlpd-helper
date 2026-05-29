-- mod-tlpd-helper: create persistent spawn log table
CREATE TABLE IF NOT EXISTS `mod_tlpd_helper_spawn_log` (
    `entry`             INT UNSIGNED NOT NULL COMMENT 'NPC entry (32491 = TLPD, 32630 = Vyragosa)',
    `last_visible_time` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Unix timestamp when creature last became visible',
    `last_death_time`   INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Unix timestamp of last recorded death',
    PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-tlpd-helper: tracks last-visible and last-death times across server restarts';
