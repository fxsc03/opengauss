create database event_db dbcompatibility 'B';
\c event_db
create schema event_s;
set current_schema = event_s;
set dolphin.b_compatibility to on;

create user event_a sysadmin password 'event_123';
create definer=event_a event e1 on schedule at '2023-01-16 21:05:40' disable do select 1;

select  job_name, nspname from pg_job where dbname='event_b';
show events in a;
show events from a;
show events like 'e';
show events like 'e%';
show events like 'e_';
show events where job_name='e1';

create table event_t(c1 int);
select pkg_service.job_submit(1111,'insert into event_t values(1);',to_date('2060-11-11'),'''1min''::interval');
show events;
show events like 'e%';
show events where job_name='e1';

select pkg_service.job_cancel(1111);
drop table event_t;
drop event if exists e1;
drop user if exists event_a;

reset current_schema;
drop schema event_s;
\c postgres
drop database if exists event_db;
