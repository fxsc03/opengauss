-- test plsql's out param override
-- check compatibility --
show sql_compatibility; -- expect A --
drop schema if exists plpgsql_override_out;
create schema plpgsql_override_out;
set current_schema = plpgsql_override_out;
set behavior_compat_options = 'proc_outparam_override';
create or replace package pck1 is
procedure p1;
procedure p1(v1 in varchar2);
procedure p1(v1 in varchar2, v2 in varchar2);
procedure p1(v1 in varchar2, v2 in varchar2, v3 in varchar2);

procedure p1(v1 out int);
procedure p1(v1 out int, v2 out int);
procedure p1(v1 out int, v2 out int, v3 out int);

procedure p1(v1 in varchar2, v2 out int);
procedure p1(v1 in varchar2, v2 out int, v3 out int);

procedure p1(v1 out int, v2 in varchar2);
procedure p1(v1 out int, v2 in varchar2, v3 in varchar2);

procedure p1(v1 inout int, v2 inout int, v3 inout int, v4 inout int);
procedure p1(v1 inout int, v2 inout int, v3 inout varchar2, v4 inout varchar2);

procedure p;
end pck1;
/

create or replace package body pck1 is

procedure p1 is
begin
raise notice 'p1';
end;

procedure p1(v1 in varchar2) is
begin
raise notice 'p1_1_varchar2';
end;

procedure p1(v1 in varchar2, v2 in varchar2) is
begin
raise notice 'p1_2_varchar2';
end;

procedure p1(v1 in varchar2, v2 in varchar2, v3 in varchar2) is
begin
raise notice 'p1_3_varchar2';
end;

procedure p1(v1 out int) is
begin
raise notice 'p1_1_int';
end;

procedure p1(v1 out int, v2 out int) is
begin
raise notice 'p1_2_int';
end;

procedure p1(v1 out int, v2 out int, v3 out int) is
begin
raise notice 'p1_3_int';
end;

procedure p1(v1 in varchar2, v2 out int) is
begin
raise notice 'p1_1_varchar_1_int';
end;

procedure p1(v1 in varchar2, v2 out int, v3 out int) is
begin
raise notice 'p1_1_varchar_2_int';
end;

procedure p1(v1 out int, v2 in varchar2) is  
begin
raise notice 'p1_1_int_1_varchar';
end;

procedure p1(v1 out int, v2 in varchar2, v3 in varchar2) is
begin
raise notice 'p1_1_int_2_varchar';
end;

procedure p1(v1 inout int, v2 inout int, v3 inout int, v4 inout int) is
begin
raise notice 'p1_4_inout_4_int';
end;

procedure p1(v1 inout int, v2 inout int, v3 inout varchar2, v4 inout varchar2) is
begin
raise notice 'p1_4_inout_2_int_2_varchar';
end;

procedure p is
a1 varchar2(10);
a2 varchar2(10);
a3 varchar2(10);
a4 varchar2(10);
b1 int;
b2 int;
b3 int;
b4 int;
begin
a1 := 'a1';
a2 := 'a2';
a3 := 'a3';
a4 := 'a4';
b1 := 1;
b2 := 2;
b3 := 3;
b4 := 4;
p1();
p1(a1);
p1(b1);
p1(a1, a2);
p1(a1, b1);
p1(b1, b2);
p1(b1, a1);
p1(a1, a2, a3);
p1(b1, b2, b3);
p1(a1, b1, b2);
p1(b1, a1, a2);
p1(b1, b2, b3, b4);
p1(b1, b2, a1, a2);
end;
end pck1;
/
-- test procedure override with out args before in args
CREATE OR REPLACE PROCEDURE test_in_out_in(a in int, b inout int, c out int, d in varchar(200), e out varchar2(200))
PACKAGE
AS
DECLARE
new_deptno NUMBER;
BEGIN
raise notice '%,%,%,%,%', a,b,c,d,e;
new_deptno :=10;
new_deptno := new_deptno+a+b;
END;
/
call test_in_out_in(1,2,3,'a','b');
begin;
CURSOR temp_cursor NO SCROLL FOR SELECT test_in_out_in(1,2,3,'a','b');
FETCH FORWARD 1 FROM temp_cursor;
end;
SELECT * from test_in_out_in(1,2,3,'a','b');

set behavior_compat_options = '';
call test_in_out_in(1,2,3,'a','b');
begin;
CURSOR temp_cursor NO SCROLL FOR SELECT test_in_out_in(1,2,'a');
FETCH FORWARD 1 FROM temp_cursor;
end;
SELECT * from test_in_out_in(1,2,'a');

----
-- test in/out/inout args
----

-- test procedure
CREATE OR REPLACE PROCEDURE iob_proc(a in int, b out int, c inout int)
AS
DECLARE
BEGIN
raise notice '%,%,%', a,b,c;
END;
/
set behavior_compat_options = '';
call iob_proc(1,2,3); -- ok
call iob_proc(1,2);
select * from iob_proc(1,2,3);
select * from iob_proc(1,2); -- ok

set behavior_compat_options = 'proc_outparam_override';
call iob_proc(1,2,3); -- ok
call iob_proc(1,2);
select * from iob_proc(1,2,3); -- ok
select * from iob_proc(1,2);

CREATE OR REPLACE PROCEDURE bio_proc(a inout int, b in int, c out int)
AS
DECLARE
BEGIN
raise notice '%,%,%', a,b,c;
END;
/
set behavior_compat_options = '';
call bio_proc(1,2,3); -- ok
call bio_proc(1,2);
select * from bio_proc(1,2,3);
select * from bio_proc(1,2); -- ok

set behavior_compat_options = 'proc_outparam_override';
call bio_proc(1,2,3); -- ok
call bio_proc(1,2);
select * from bio_proc(1,2,3); -- ok
select * from bio_proc(1,2);

CREATE OR REPLACE PROCEDURE obi_proc(a out int, b inout int, c in int)
AS
DECLARE
BEGIN
raise notice '%,%,%', a,b,c;
END;
/
set behavior_compat_options = '';
call obi_proc(1,2,3); -- ok
call obi_proc(1,2);
select * from obi_proc(1,2,3);
select * from obi_proc(1,2); -- ok

set behavior_compat_options = 'proc_outparam_override';
call obi_proc(1,2,3); -- ok
call obi_proc(1,2);
select * from obi_proc(1,2,3); -- ok
select * from obi_proc(1,2);

-- test function
CREATE OR REPLACE FUNCTION iob_func(a in int, b out int, c inout int) RETURNS SETOF RECORD
AS $$
DECLARE
BEGIN
raise notice '%,%,%', a,b,c;
return;
END
$$
LANGUAGE plpgsql;
set behavior_compat_options = '';
call iob_func(1,2,3); --ok
call iob_func(1,2);
select * from iob_func(1,2,3);
select * from iob_func(1,2); -- ok

set behavior_compat_options = 'proc_outparam_override';
call iob_func(1,2,3);
call iob_func(1,2);
select * from iob_func(1,2,3);
select * from iob_func(1,2); -- ok

CREATE OR REPLACE FUNCTION bio_func(a inout int, b in int, c out int) RETURNS SETOF RECORD
AS $$
DECLARE
BEGIN
raise notice '%,%,%', a,b,c;
return;
END
$$
LANGUAGE plpgsql;
set behavior_compat_options = '';
call bio_func(1,2,3); -- ok
call bio_func(1,2);
select * from bio_func(1,2,3);
select * from bio_func(1,2); -- ok

set behavior_compat_options = 'proc_outparam_override';
call bio_func(1,2,3);
call bio_func(1,2);
select * from bio_func(1,2,3);
select * from bio_func(1,2); -- ok

CREATE OR REPLACE FUNCTION obi_func(a out int, b inout int, c in int) RETURNS SETOF RECORD
AS $$
DECLARE
BEGIN
raise notice '%,%,%', a,b,c;
return;
END
$$
LANGUAGE plpgsql;
set behavior_compat_options = '';
call obi_func(1,2,3); -- ok
call obi_func(1,2);
select * from obi_func(1,2,3);
select * from obi_func(1,2); -- ok

set behavior_compat_options = 'proc_outparam_override';
call obi_func(1,2,3);
call obi_func(1,2);
select * from obi_func(1,2,3);
select * from obi_func(1,2); -- ok

drop procedure test_in_out_in;
drop package pck1;

-- test override procedure with error param
set behavior_compat_options='proc_outparam_override';
drop package if exists pck1;
create type o1_test as (v01 number, v03 varchar2, v02 number);
create or replace package pck1 is
procedure p1(a o1_test,b out varchar2);
procedure p1(a2 int[], b2 out varchar2);
end pck1;
/

create or replace package body pck1 is
procedure p1(a o1_test,b out varchar2) is
begin
b:=a.v01;
raise info 'b:%',b;
end;
procedure p1(a2 int[], b2 out varchar2) is
begin
b2:=a2(2);
raise info 'b2:%',b2;
end;
end pck1;
/

-- should error
declare
begin
pck1.p1((1,'b',2),'a');
end;
/
drop table if exists test_tb;
create table test_tb(c1 int,c2 varchar2);
insert into test_tb values(1,'a'),(2,'b'),(3,'c');
drop package if exists pck1;
create or replace package pck1 is
type tp1 is record(v01 int, v02 varchar2);
procedure p1(a inout tp1,b varchar2);
end pck1;
/

create or replace package body pck1 is
procedure p1(a inout tp1,b varchar2) is
begin
select * into a from test_tb where c2=b;
end;
end pck1;
/
declare
var pck1.tp1;
begin
perform pck1.p1(var,'a');
end;
/
set behavior_compat_options='proc_outparam_override';
create or replace procedure p2(a int,b out int) is
begin
raise info 'a:%', a+1;
end;
/
drop table if exists test_tb;
create table test_tb(c1 int,c2 varchar2);
insert into test_tb values(1,'a'),(2,'b'),(3,'c');
drop package if exists pck1;
create or replace package pck1 is
type tp1 is table of varchar2(1024) index by varchar2(4000);
procedure p1(out_var out tp1,in_var varchar2);
end pck1;
/
create or replace package body pck1 is
procedure p1(out_var out tp1,in_var varchar2) is
begin
select c1 into out_var(in_var) from test_tb limit 1;
out_var('aa'):='aa';
end;
end pck1;
/
declare
var pck1.tp1;
begin
perform pck1.p1(var,'a');--不支持，报错
end;
/
\df
drop package pck1;
drop type o1_test;
set behavior_compat_options = '';

create or replace procedure proc_test
as
work_date varchar2;
begin
work_date:='202208';
end;
/

call proc_test();

create or replace procedure proc_test
as
workZ varchar2;
begin
workZ:='202208';
end;
/

call proc_test();

create or replace procedure proc_test
as
read_1 varchar2;
begin
read_1:='202208';
end;
/

call proc_test();

create or replace procedure proc_test
as
transaction_1 varchar2;
begin
transaction_1:='202208';
end;
/

call proc_test();

create or replace procedure proc_test
as
isolation1 varchar2;
begin
isolation1:='202208';
end;
/

call proc_test();

create or replace procedure proc_test
as
deferrableZ varchar2;
begin
deferrableZ:='202208';
end;
/

call proc_test();

create or replace procedure proc_test
as
not_1 varchar2;
begin
not_1:='202208';
end;
/


create or replace function f1(a out text) return text
is
b text;
begin
return b;
end;
/
 
create or replace function f3() return text
as
declare
buf text;
ddd text;
b text;
begin
return buf;
end;
/
 
call f3();
 
create or replace function f1(a out text) return text
is
b text;
begin
a :='aaa';
return b;
end;
/
 
create or replace function f3() return text
as
declare
buf text;
ddd text;
b text;
begin
perform f1(buf);
return buf;
end;
/
 
call f3();
 
create or replace function f1(c int, a out text) return text
is
b text;
begin
c:=1;
a :='aaa';
return b;
end;
/
 
create or replace function f3() return text
as
declare
buf text;
ddd text;
b text;
begin
perform f1(1,buf);
return buf;
end;
/
 
call f3();
create or replace procedure pro2(i_col1 out int)
is
a int;
begin
a := 9;
end;
/

declare
a int;
begin
perform pro2(a);
end;
/

call proc_test();
drop procedure proc_test;

set behavior_compat_options='proc_outparam_override';
set plsql_compile_check_options='outparam';
create procedure p11(a out int) package is
begin
a:=12;
end;
/
create procedure p11(a out varchar2) package is
begin
a:='12aa';
end;
/
create procedure p11(a out int[]) package is
begin
a:=12;
end;
/
create procedure p11(a in int, b out int) package is
begin
a:=12;
b:=66;
end;
/

drop procedure if exists p112(out int);
create procedure p112(a out int) is
declare
aa varchar2;
a varchar2;
begin
aa := p11(p11(p11('2aaa')));--常量报错
raise info 'aa=%',aa;
end;
/


drop procedure if exists p112(out int);
create procedure p112(a out int) is
declare
aa varchar2;
a varchar2;
begin
aa := p11(p11('2aaa'));--常量报错
raise info 'aa=%',aa;
end;
/

drop procedure if exists p112(out int);
create procedure p112(a out int) is
declare
aa varchar2;
a varchar2;
begin
aa := p11('2aaa');--常量报错
raise info 'aa=%',aa;
end;
/

drop procedure if exists p112(out int);
create procedure p112(a out int) is
declare
aa varchar2;
a varchar2;
begin
aa := p11(1+1);--常量报错
raise info 'aa=%',aa;
end;
/

drop procedure if exists p112(out int);
create procedure p112(a out int) is
declare
aa varchar2;
a varchar2;
begin
aa := p11(a);--常量不报错
raise info 'aa=%',aa;
end;
/

drop procedure if exists p112(out int);
create procedure p112(a out int) is
declare
aa varchar2;
a int[];
begin
aa := p11(a);--常量不报错
raise info 'aa=%',aa;
end;
/

drop procedure if exists p112(out int);
create procedure p112(a out int) is
declare
aa varchar2;
bb int;
begin
aa := p11(p11('2aaa'),bb);--常量报错
raise info 'aa=%',aa;
end;
/

drop package if exists pck1;
create or replace package pck1 is
type tp_1 is record(v01 number, v03 varchar2, v02 number);
procedure p1(a out int);
procedure p1(b out tp_1);
end pck1;
/

create or replace package body pck1 is
procedure p1(a out int) is
begin
a:=12;
end;
procedure p1(b out tp_1) is
begin
b.v01:=13;
raise info 'b:%', b;
end;
end pck1;
/

declare
begin
perform pck1.p1(2);--常量报错
end;
/

drop package if exists pck1;
create or replace package pck1 is
type tp_1 is record(v01 number, v03 varchar2, v02 number);
type tp_2 is varray(10) of int;
procedure p1(a in tp_1,c inout tp_2,b out varchar2);
procedure p1(a in tp_2,c inout tp_1,b out tp_1);
end pck1;
/

create or replace package body pck1 is
procedure p1(a in tp_1,c inout tp_2,b out varchar2) is
begin
c(1):=a.v01;
b:=a.v03;
raise info 'b:%',b;
end;
procedure p1(a in tp_2,c inout tp_1,b out tp_1) is
begin
c.v03:=a(1);
b:=(a(1),c.v03,a(2));
raise info 'b:%',b;
end;
end pck1;
/

declare
var1 pck1.tp_2;
var2 varchar2;
begin
perform pck1.p1((1,'bb','11'),array[2,3,4],var2);--报错
end;
/

declare
var1 pck1.tp_2;
var2 varchar2;
begin
perform pck1.p1(a=>(1,'bb','11'),c=>var1,b=>'aa');--报错
end;
/

create table test_table2 (coll int,col2 text);
insert into test_table2 values (1, 'test');
create or replace function test_function2(out rl refcursor, out r2 refcursor)
returns refcursor as 
$$
begin
open rl for select coll from test_table2;
open r2 for select col2 from test_table2;
return r2;
end;
$$
LANGUAGE 'plpgsql';
call test_function2('','');

set behavior_compat_options = 'proc_outparam_override';
create or replace package pck2 is
    FUNCTION pkg_mem_case(a int,self1 int) RETURN VARCHAR2;
    procedure pkg_mem_case(a int,self1 inout int);
end pck2;
/

set behavior_compat_options = '';
create or replace package pck2 is
    FUNCTION pkg_mem_case(a int,self1 int) RETURN VARCHAR2;
    procedure pkg_mem_case(a int,self1 inout int);
end pck2;
/
select count(*) from pg_proc where proname = 'pkg_mem_case';
drop package pck2;

set plsql_compile_check_options='';
drop package body if exists pck1;
drop package body pck1;
drop package body pck1;
drop package body if exists pck1;
drop package if exists pck1;
drop schema if exists plpgsql_override_out cascade;
