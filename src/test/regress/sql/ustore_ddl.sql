-- predictability
CREATE DATABASE ustore_ddl;
\c ustore_ddl
SET synchronous_commit = on;


SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot_ustore_ddl', 'mppdb_decoding');
/*
 * Check that changes are handled correctly when interleaved with ddl
 */
CREATE TABLE replication_example(id SERIAL PRIMARY KEY, somedata int, text varchar(120))with(storage_type = ustore);
START TRANSACTION;
INSERT INTO replication_example(somedata, text) VALUES (1, 1);
INSERT INTO replication_example(somedata, text) VALUES (1, 2);
COMMIT;
START TRANSACTION;
INSERT INTO replication_example(somedata, text) VALUES (3, 2);
INSERT INTO replication_example(somedata, text) VALUES (3, 3);
COMMIT;

-- collect all changes
SELECT data FROM pg_logical_slot_get_changes('regression_slot_ustore_ddl', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

/*
 * check that disk spooling works
 */
/* display results, but hide most of the output */
SELECT count(*), min(data), max(data)
FROM pg_logical_slot_get_changes('regression_slot_ustore_ddl', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1')
GROUP BY substring(data, 1, 24)
ORDER BY 1,2;

/*
 * check whether we decode subtransactions correctly in relation with each
 * other
 */
CREATE TABLE tr_sub (id serial primary key, path text)with(storage_type = ustore);

-- toplevel, subtxn, toplevel, subtxn, subtxn
START TRANSACTION;
INSERT INTO tr_sub(path) VALUES ('1-top-#1');

SAVEPOINT a;
INSERT INTO tr_sub(path) VALUES ('1-top-1-#1');
INSERT INTO tr_sub(path) VALUES ('1-top-1-#2');
RELEASE SAVEPOINT a;

SAVEPOINT b;
SAVEPOINT c;
INSERT INTO tr_sub(path) VALUES ('1-top-2-1-#1');
INSERT INTO tr_sub(path) VALUES ('1-top-2-1-#2');
RELEASE SAVEPOINT c;
INSERT INTO tr_sub(path) VALUES ('1-top-2-#1');
RELEASE SAVEPOINT b;
COMMIT;

SELECT data FROM pg_logical_slot_get_changes('regression_slot_ustore_ddl', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- check that we handle xlog assignments correctly
START TRANSACTION;
-- nest 80 subtxns
SAVEPOINT subtop;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;SAVEPOINT a;
-- assign xid by inserting
INSERT INTO tr_sub(path) VALUES ('2-top-1...--#1');
INSERT INTO tr_sub(path) VALUES ('2-top-1...--#2');
INSERT INTO tr_sub(path) VALUES ('2-top-1...--#3');
RELEASE SAVEPOINT subtop;
INSERT INTO tr_sub(path) VALUES ('2-top-#1');
COMMIT;

SELECT data FROM pg_logical_slot_get_changes('regression_slot_ustore_ddl', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- make sure rollbacked subtransactions aren't decoded
START TRANSACTION;
INSERT INTO tr_sub(path) VALUES ('3-top-2-#1');
SAVEPOINT a;
INSERT INTO tr_sub(path) VALUES ('3-top-2-1-#1');
SAVEPOINT b;
INSERT INTO tr_sub(path) VALUES ('3-top-2-2-#1');
ROLLBACK TO SAVEPOINT b;
INSERT INTO tr_sub(path) VALUES ('3-top-2-#2');
COMMIT;

SELECT data FROM pg_logical_slot_get_changes('regression_slot_ustore_ddl', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
-- test whether a known, but not yet logged toplevel xact, followed by a
-- subxact commit is handled correctly
START TRANSACTION;
SELECT txid_current() != 0; -- so no fixed xid apears in the outfile
SAVEPOINT a;
INSERT INTO tr_sub(path) VALUES ('4-top-1-#1');
RELEASE SAVEPOINT a;
COMMIT;

-- test whether a change in a subtransaction, in an unknown toplevel
-- xact is handled correctly.
START TRANSACTION;
SAVEPOINT a;
INSERT INTO tr_sub(path) VALUES ('5-top-1-#1');
COMMIT;

SELECT data FROM pg_logical_slot_get_changes('regression_slot_ustore_ddl', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

/*
 * check whether we handle updates/deletes correct with & without a pkey
 */

/* we should handle the case without a key at all more gracefully */
CREATE TABLE table_without_key(id serial, data int)with(storage_type = ustore);
INSERT INTO table_without_key(data) VALUES(1),(2);
DELETE FROM table_without_key WHERE data = 1;
-- won't log old keys
UPDATE table_without_key SET data = 3 WHERE data = 2;
UPDATE table_without_key SET id = -id;
UPDATE table_without_key SET id = -id;
create table bmsql_order_line (
  ol_w_id         integer   not null,
  ol_d_id         integer   not null,
  ol_o_id         integer   not null,
  ol_number       integer   not null,
  ol_i_id         integer   not null,
  ol_delivery_d   timestamp,
  ol_amount       decimal(6,2),
  ol_supply_w_id  integer,
  ol_quantity     integer,
  ol_dist_info    char(24)
)with(storage_type = ustore)
partition by range(ol_d_id)
(
  partition p0 values less than (10),
  partition p1 values less than (100),
  partition p2 values less than (maxvalue)
);
alter table bmsql_order_line add constraint bmsql_order_line_pkey primary key (ol_w_id, ol_d_id, ol_o_id, ol_number);
insert into bmsql_order_line(ol_w_id, ol_d_id, ol_o_id, ol_number, ol_i_id, ol_dist_info) values(1, 1, 1, 1, 1, '123');
update bmsql_order_line set ol_dist_info='ss' where ol_w_id =1;
delete from bmsql_order_line;


-- done, free logical replication slot
SELECT data FROM pg_logical_slot_get_changes('regression_slot_ustore_ddl', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
SELECT pg_drop_replication_slot('regression_slot_ustore_ddl');

drop table replication_example;
drop table tr_sub;
drop table table_without_key;
drop table bmsql_order_line;
-- end
\c regression
DROP DATABASE IF EXISTS ustore_ddl;