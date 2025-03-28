--
-- CREATE_TYPE
--
--
-- Note: widget_in/out were created in create_function_1, without any
-- prior shell-type creation.  These commands therefore complete a test
-- of the "old style" approach of making the functions first.
--
CREATE TYPE widget (
   internallength = 24,
   input = widget_in,
   output = widget_out,
   typmod_in = numerictypmodin,
   typmod_out = numerictypmodout,
   alignment = double
);

CREATE TYPE city_budget (
   internallength = 16,
   input = int44in,
   output = int44out,
   element = int4,
   category = 'x',   -- just to verify the system will take it
   preferred = true  -- ditto
);

-- Test creation and destruction of shell types
CREATE TYPE shell;
CREATE TYPE shell;   -- fail, type already present
DROP TYPE shell;
DROP TYPE shell;     -- fail, type not exist

--
-- Test type-related default values (broken in releases before PG 7.2)
--
-- This part of the test also exercises the "new style" approach of making
-- a shell type and then filling it in.
--
CREATE TYPE int42;
CREATE TYPE text_w_default;

-- Make dummy I/O routines using the existing internal support for int4, text
CREATE FUNCTION int42_in(cstring)
   RETURNS int42
   AS 'int4in'
   LANGUAGE internal STRICT;
CREATE FUNCTION int42_out(int42)
   RETURNS cstring
   AS 'int4out'
   LANGUAGE internal STRICT;
CREATE FUNCTION text_w_default_in(cstring)
   RETURNS text_w_default
   AS 'textin'
   LANGUAGE internal STRICT;
CREATE FUNCTION text_w_default_out(text_w_default)
   RETURNS cstring
   AS 'textout'
   LANGUAGE internal STRICT;

CREATE TYPE int42 (
   internallength = 4,
   input = int42_in,
   output = int42_out,
   alignment = int4,
   default = 42,
   passedbyvalue
);

CREATE TYPE text_w_default (
   internallength = variable,
   input = text_w_default_in,
   output = text_w_default_out,
   alignment = int4,
   default = 'zippo'
);

CREATE TABLE default_test (f1 text_w_default, f2 int42);

INSERT INTO default_test DEFAULT VALUES;

SELECT * FROM default_test;

-- Test stand-alone composite type

CREATE TYPE default_test_row AS (f1 text_w_default, f2 int42);

CREATE FUNCTION get_default_test() RETURNS SETOF default_test_row AS '
  SELECT * FROM default_test;
' LANGUAGE SQL;

SELECT * FROM get_default_test();

-- Test comments
COMMENT ON TYPE bad IS 'bad comment';
COMMENT ON TYPE default_test_row IS 'good comment';
COMMENT ON TYPE default_test_row IS NULL;
COMMENT ON COLUMN default_test_row.nope IS 'bad comment';
COMMENT ON COLUMN default_test_row.f1 IS 'good comment';
COMMENT ON COLUMN default_test_row.f1 IS NULL;

-- Check shell type create for existing types
CREATE TYPE text_w_default;		-- should fail

DROP TYPE default_test_row CASCADE;

DROP TABLE default_test;

-- Enforce use of COMMIT instead of 2PC for temporary objects

-- Check usage of typmod with a user-defined type
-- (we have borrowed numeric's typmod functions)

CREATE TEMP TABLE mytab (foo widget(42,13,7));     -- should fail
CREATE TEMP TABLE mytab (foo widget(42,13));

SELECT format_type(atttypid,atttypmod) FROM pg_attribute
WHERE attrelid = 'mytab'::regclass AND attnum > 0;

create type i_int2 as(a int,b int);
DECLARE
TYPE DeptRecTyp IS RECORD (
dept_id NUMBER(4) NOT NULL := 10,
dept_name VARCHAR2(30) NOT NULL := 'Administration',
mgr_id NUMBER(6) := 200,
loc_id i_int2 := (19,1)
);
dept_rec DeptRecTyp;
BEGIN
END;
/

drop table employee_tab2_1154887;
drop type person_typ2_1154887;
CREATE OR REPLACE TYPE person_typ2_1154887 AS OBJECT(
     name VARCHAR2(10),
	 gender VARCHAR2(4),
     birthdate DATE,
	 address VARCHAR2(100),
     MEMBER PROCEDURE change_address(new_addr VARCHAR2),
     MEMBER FUNCTION get_info RETURN VARCHAR2
);




CREATE TABLE employee_tab2_1154887(
      eno NUMBER(6),person person_typ2_1154887,
      sal NUMBER(6,2),job VARCHAR2(20)
);


CREATE OR REPLACE TYPE BODY person_typ2_1154887 IS
       MEMBER PROCEDURE change_address(new_addr VARCHAR2)
       IS
       BEGIN
          address:=new_addr;
      END;
      MEMBER FUNCTION get_info RETURN VARCHAR2
      IS
           v_info VARCHaR2(100);
      BEGIN
           v_info := '姓名：'||name||',出生日期：'||birthdate;
           RETURN v_info;
      END;
end;
/


INSERT INTO employee_tab2_1154887(eno,sal,job,person)
      VALUES(1,2000,'软件工程师',
                    person_typ2_1154887('王明','男','2020-12-01','武汉北路55号'));

DECLARE
       v_person person_typ2_1154887;
BEGIN
       SELECT person INTO v_person FROM employee_tab2_1154887
               WHERE eno=1;
       v_person.change_address('江西南昌东街12号'); --改变员工地址
       UPDATE employee_tab2_1154887 SET person=v_person WHERE eno=1;
       raise info '%',v_person.get_info; --获取员工信息
END;
/
CREATE ROLE test_role WITH PASSWORD 'openGauss@123';
CREATE TYPE test_type as (id int);
ALTER TYPE test_type OWNER TO test_role;
ALTER TYPE test_type OWNER TO CURRENT_USER;
ALTER TYPE test_type OWNER TO SESSION_USER;
DROP TYPE test_type;
DROP ROLE test_role;