
--
-- VARCHAR
--

CREATE FOREIGN TABLE VARCHAR_TBL(f1 varchar(1)) SERVER mot_server ;

INSERT INTO VARCHAR_TBL (f1) VALUES ('a');

INSERT INTO VARCHAR_TBL (f1) VALUES ('A');

-- any of the following three input formats are acceptable
INSERT INTO VARCHAR_TBL (f1) VALUES ('1');

INSERT INTO VARCHAR_TBL (f1) VALUES (2);

INSERT INTO VARCHAR_TBL (f1) VALUES ('3');

-- zero-length char
INSERT INTO VARCHAR_TBL (f1) VALUES ('');

-- try varchar's of greater than 1 length
INSERT INTO VARCHAR_TBL (f1) VALUES ('cd');
INSERT INTO VARCHAR_TBL (f1) VALUES ('c     ');


SELECT '' AS seven, * FROM VARCHAR_TBL ORDER BY f1;

SELECT '' AS six, c.*
   FROM VARCHAR_TBL c
   WHERE c.f1 <> 'a' ORDER BY f1;

SELECT '' AS one, c.*
   FROM VARCHAR_TBL c
   WHERE c.f1 = 'a';

SELECT '' AS five, c.*
   FROM VARCHAR_TBL c
   WHERE c.f1 < 'a' ORDER BY f1;

SELECT '' AS six, c.*
   FROM VARCHAR_TBL c
   WHERE c.f1 <= 'a' ORDER BY f1;

SELECT '' AS one, c.*
   FROM VARCHAR_TBL c
   WHERE c.f1 > 'a' ORDER BY f1;

SELECT '' AS two, c.*
   FROM VARCHAR_TBL c
   WHERE c.f1 >= 'a' ORDER BY f1;

DROP FOREIGN TABLE VARCHAR_TBL;

--
-- Now test longer arrays of char
--

CREATE FOREIGN TABLE VARCHAR_TBL(f1 varchar(4)) SERVER mot_server ;

INSERT INTO VARCHAR_TBL (f1) VALUES ('a');
INSERT INTO VARCHAR_TBL (f1) VALUES ('ab');
INSERT INTO VARCHAR_TBL (f1) VALUES ('abcd');
INSERT INTO VARCHAR_TBL (f1) VALUES ('abcde');
INSERT INTO VARCHAR_TBL (f1) VALUES ('abcd    ');

SELECT '' AS four, * FROM VARCHAR_TBL ORDER BY f1;
DROP FOREIGN TABLE VARCHAR_TBL;

CREATE DATABASE db_varchar DBCOMPATIBILITY 'B' ENCODING 'UTF8';
\c db_varchar
create Foreign table t_foreign1 (c1 int not null, c2 numeric, c3 char(30));
insert into t_foreign1 values (1, 50, '嘿嘿');
insert into t_foreign1 values (1, 50, '嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿');
insert into t_foreign1 values (1, 50, '嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿嘿');
\c postgres
DROP DATABASE db_varchar;

