--
-- INT4
--

CREATE TABLE INT4_TBL(f1 int4);

INSERT INTO INT4_TBL(f1) VALUES ('   0  ');

INSERT INTO INT4_TBL(f1) VALUES ('123456     ');

INSERT INTO INT4_TBL(f1) VALUES ('    -123456');

INSERT INTO INT4_TBL(f1) VALUES ('34.5');

-- largest and smallest values
INSERT INTO INT4_TBL(f1) VALUES ('2147483647');
INSERT INTO INT4_TBL(f1) VALUES ('-2147483648');

-- bad input values -- should give errors
INSERT INTO INT4_TBL(f1) VALUES ('2147483648');
INSERT INTO INT4_TBL(f1) VALUES ('-2147483649');
INSERT INTO INT4_TBL(f1) VALUES ('1000000000000');
INSERT INTO INT4_TBL(f1) VALUES ('asdf');
INSERT INTO INT4_TBL(f1) VALUES ('     ');
INSERT INTO INT4_TBL(f1) VALUES ('   asdf   ');
INSERT INTO INT4_TBL(f1) VALUES ('- 1234');
INSERT INTO INT4_TBL(f1) VALUES ('123       5');
INSERT INTO INT4_TBL(f1) VALUES ('');


SELECT '' AS five, * FROM INT4_TBL ORDER BY f1;

SELECT '' AS four, i.* FROM INT4_TBL i WHERE i.f1 <> int2 '0' ORDER BY f1;

SELECT '' AS four, i.* FROM INT4_TBL i WHERE i.f1 <> int4 '0' ORDER BY f1;

SELECT '' AS one, i.* FROM INT4_TBL i WHERE i.f1 = int2 '0';

SELECT '' AS one, i.* FROM INT4_TBL i WHERE i.f1 = int4 '0';

SELECT '' AS two, i.* FROM INT4_TBL i WHERE i.f1 < int2 '0' ORDER BY f1;

SELECT '' AS two, i.* FROM INT4_TBL i WHERE i.f1 < int4 '0' ORDER BY f1;

SELECT '' AS three, i.* FROM INT4_TBL i WHERE i.f1 <= int2 '0' ORDER BY f1;

SELECT '' AS three, i.* FROM INT4_TBL i WHERE i.f1 <= int4 '0' ORDER BY f1;

SELECT '' AS two, i.* FROM INT4_TBL i WHERE i.f1 > int2 '0' ORDER BY f1;

SELECT '' AS two, i.* FROM INT4_TBL i WHERE i.f1 > int4 '0' ORDER BY f1;

SELECT '' AS three, i.* FROM INT4_TBL i WHERE i.f1 >= int2 '0' ORDER BY f1;

SELECT '' AS three, i.* FROM INT4_TBL i WHERE i.f1 >= int4 '0' ORDER BY f1;

-- positive odds
SELECT '' AS one, i.* FROM INT4_TBL i WHERE (i.f1 % int2 '2') = int2 '1' ORDER BY f1;

-- any evens
SELECT '' AS three, i.* FROM INT4_TBL i WHERE (i.f1 % int4 '2') = int2 '0' ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 * int2 '2' AS x FROM INT4_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 * int2 '2' AS x FROM INT4_TBL i
WHERE abs(f1) < 1073741824 ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 * int4 '2' AS x FROM INT4_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 * int4 '2' AS x FROM INT4_TBL i
WHERE abs(f1) < 1073741824 ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 + int2 '2' AS x FROM INT4_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 + int2 '2' AS x FROM INT4_TBL i
WHERE f1 < 2147483646 ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 + int4 '2' AS x FROM INT4_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 + int4 '2' AS x FROM INT4_TBL i
WHERE f1 < 2147483646 ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 - int2 '2' AS x FROM INT4_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 - int2 '2' AS x FROM INT4_TBL i
WHERE f1 > -2147483647 ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 - int4 '2' AS x FROM INT4_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 - int4 '2' AS x FROM INT4_TBL i
WHERE f1 > -2147483647 ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 / int2 '2' AS x FROM INT4_TBL i ORDER BY f1;

SELECT '' AS five, i.f1, i.f1 / int4 '2' AS x FROM INT4_TBL i ORDER BY f1;

--
-- more complex expressions
--

-- variations on unary minus parsing
SELECT -2+3 AS one;

SELECT 4-2 AS two;

SELECT 2- -1 AS three;

SELECT 2 - -2 AS four;

SELECT int2 '2' * int2 '2' = int2 '16' / int2 '4' AS true;

SELECT int4 '2' * int2 '2' = int2 '16' / int4 '4' AS true;

SELECT int2 '2' * int4 '2' = int4 '16' / int2 '4' AS true;

SELECT int4 '1000' < int4 '999' AS false;

SELECT 4! AS twenty_four;

SELECT !!3 AS six;

SELECT 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 AS ten;

SELECT 2 + 2 / 2 AS three;

SELECT (2 + 2) / 2 AS two;

-- divide zero
SELECT (0)::int4 / (-2)::int2;
SELECT (0)::int4 / (-2)::int4;
SELECT (0)::int4 / (-2)::int8;

-- corner case
SELECT (-1::int4<<31)::text;
SELECT ((-1::int4<<31)+1)::text;

-- check sane handling of INT_MIN overflow cases
SELECT (-2147483648)::int4 * (-1)::int4;
SELECT (-2147483648)::int4 / (-1)::int4;
SELECT (-2147483648)::int4 % (-1)::int4;
SELECT (-2147483648)::int4 * (-1)::int2;
SELECT (-2147483648)::int4 / (-1)::int2;
SELECT (-2147483648)::int4 % (-1)::int2;

set behavior_compat_options ='unbind_divide_bound';
SELECT (-2147483648)::int4 / (-1)::int2;
SELECT (-2147483648)::int4 / (-1)::int4;
reset behavior_compat_options;

