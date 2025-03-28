drop database if exists test;
create database test dbcompatibility = 'B';

alter system set instr_unique_sql_count to 1000;
delete from statement_history;
\c test

-- 创建测试表
CREATE TABLE t1 (id int);
CREATE TABLE t2 (id int);
CREATE TABLE t3 (id int);

-- 插入测试数据
INSERT INTO t1 VALUES (1);
INSERT INTO t2 VALUES (1);
INSERT INTO t3 VALUES (1);

-- 子存储过程1
CREATE OR REPLACE PROCEDURE func_batch_insert1()
AS  
BEGIN  
    UPDATE t1 SET id = id + 1;
    COMMIT;
    UPDATE t2 SET id = id - 1; 
    COMMIT;
END;
/

-- 子存储过程2
CREATE OR REPLACE PROCEDURE func_batch_insert2()
AS  
BEGIN  
    UPDATE t2 SET id = id + 1;
    UPDATE t2 SET id = id - 1;
END;
/

-- 子存储过程3
CREATE OR REPLACE PROCEDURE func_batch_insert3()
AS  
BEGIN  
    UPDATE t3 SET id = id + 1;
END;
/

-- 主存储过程
CREATE OR REPLACE PROCEDURE func_batch()
AS  
BEGIN  
    SAVEPOINT svp1;
    CALL func_batch_insert1();

    SAVEPOINT svp2;
    CALL func_batch_insert2();
  
    SAVEPOINT svp3;
    CALL func_batch_insert3();
END;
/

--set the slow sql threshold
set instr_unique_sql_track_type = 'all';
set log_min_duration_statement = 0; 
CALL func_batch();
select pg_sleep(3);
\c postgres
select count(*) from statement_history where parent_query_id != 0;
delete from statement_history;

