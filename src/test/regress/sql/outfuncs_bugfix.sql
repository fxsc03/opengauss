set log_min_messages = debug5;
set log_min_error_statement = debug5;   
alter system set debug_print_rewritten = on;
alter system set debug_print_plan = on;

drop table t1;
create table t1(col1 int, col2 varchar(20));
insert into t1 values (1,'tom'), (2,'daisy'), (3,'john');
set query_dop=1004;
select * from t1 limit 1;
drop table t1;


reset log_min_messages ;
reset log_min_error_statement ;
alter system set debug_print_rewritten = off;
alter system set debug_print_plan = off;