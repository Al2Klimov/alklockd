/* alklockd -- Al Klimov's alarm clock daemon
 *
 * Copyright (C) 2015  Alexander A. Klimov
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

-- designed for PostgreSQL 9.4

CREATE TABLE IF NOT EXISTS alklockd_range (
    id      bigserial       PRIMARY KEY,
    comment varchar(127)    NULL DEFAULT NULL,
    start   date            NOT NULL,
    stop    date            NULL DEFAULT NULL
);

CREATE TABLE IF NOT EXISTS alklockd_group (
    id      serial          PRIMARY KEY,
    name    varchar(127)    NOT NULL
);

CREATE TABLE IF NOT EXISTS alklockd_membership (
    range_id    bigint  NOT NULL REFERENCES alklockd_range ON DELETE CASCADE,
    group_id    integer NOT NULL REFERENCES alklockd_group ON DELETE CASCADE,
    PRIMARY KEY (range_id, group_id)
);

CREATE TABLE IF NOT EXISTS alklockd_override (
    overrider   integer NOT NULL REFERENCES alklockd_group ON DELETE CASCADE,
    overridee   integer NOT NULL REFERENCES alklockd_group ON DELETE CASCADE,
    PRIMARY KEY (overrider, overridee)
);

CREATE TABLE IF NOT EXISTS alklockd_weektime (
    group_id    integer     NOT NULL REFERENCES alklockd_group ON DELETE CASCADE,
    weekday     smallint    NOT NULL,
    alarm_time  time(0)     NOT NULL,
    PRIMARY KEY (group_id, weekday, alarm_time)
);

CREATE TABLE IF NOT EXISTS alklockd_daytime (
    group_id        integer         NOT NULL REFERENCES alklockd_group ON DELETE CASCADE,
    alarm_timestamp timestamp(0)    NOT NULL,
    PRIMARY KEY (group_id, alarm_timestamp)
);


CREATE OR REPLACE FUNCTION alklockd_in_group(date, integer)
    RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
    testee ALIAS FOR $1;
    container ALIAS FOR $2;
BEGIN
    RETURN CASE WHEN (
        SELECT 0 < COUNT(*)
        FROM alklockd_range AS r JOIN alklockd_membership AS m ON r.id=m.range_id
        WHERE m.group_id=container
            AND r.start <= testee AND testee <= COALESCE(r.stop, r.start)
    ) OR (
        SELECT 0 < COUNT(*)
        FROM alklockd_daytime
        WHERE group_id=container AND alarm_timestamp::date = testee
    ) THEN NOT (
        SELECT COALESCE(bool_or(alklockd_in_group(testee, overrider)), FALSE)
        FROM alklockd_override
        WHERE overridee=container
    ) ELSE FALSE END;
END;
$$;

CREATE OR REPLACE FUNCTION alklockd_round5min(time(0))
    RETURNS time(0) LANGUAGE plpgsql AS $$
DECLARE
    time0 CONSTANT time(0) := '00:00:00';
    int_min interval := date_trunc('minute', $1 + interval '150 seconds' - time0);
BEGIN
    RETURN time0 + int_min - make_interval(
        mins := EXTRACT(MINUTE FROM int_min)::smallint % 5
    );
END;
$$;


CREATE OR REPLACE VIEW alklockd_now(alarm_now) AS
    SELECT COUNT(*) > 0
    FROM (
        SELECT group_id, alarm_time
        FROM alklockd_weektime
        WHERE weekday = (EXTRACT(ISODOW FROM CURRENT_DATE) - 1)::smallint
        UNION ALL
        SELECT group_id, alarm_timestamp::time(0) AS alarm_time
        FROM alklockd_daytime
        WHERE alarm_timestamp::date = CURRENT_DATE
    ) AS alarm_time(group_id, alarm_time)
    WHERE alklockd_in_group(CURRENT_DATE, alarm_time.group_id)
        AND alklockd_round5min(LOCALTIME(0)) = alklockd_round5min(alarm_time.alarm_time);
