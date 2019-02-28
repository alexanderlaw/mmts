-- set connection strings to nodes
select conninfo as node1 from mtm.nodes() where id = 1 \gset
select conninfo as node2 from mtm.nodes() where id = 2 \gset
select conninfo as node3 from mtm.nodes() where id = 3 \gset


-- check that implicit empty transactions works fine
create table t (a int, b text);
create or replace function f1() returns trigger as $$begin raise notice 'b: %', new.b; return NULL; end$$ language plpgsql;
create trigger tr1 before insert on t for each row execute procedure f1();
insert into t values (1, 'asdf');
copy t from stdout;
1	baz
\.


-- test mixed temp table and persistent write
\c :node1
CREATE TEMPORARY TABLE box_temp (f1 box);
CREATE TABLE box_persistent (f1 box);

insert into box_temp values('(45,55,45,49)');
insert into box_persistent values('(45,55,45,49)');

begin;
insert into box_temp values('(45,55,45,49)');
insert into box_persistent values('(45,55,45,49)');
commit;

table box_temp;
table box_persistent;

begin;
create temporary table sp_test_t(i serial primary key);
create table sp_test(i int primary key);
commit;

create temporary table sp_test_t1(i serial primary key);
create table sp_test1(i int primary key);

\c :node2
table box_temp;
table box_persistent;
table sp_test1;

\c :node1
create table t_tempddl_mix(id int primary key);
insert into t_tempddl_mix values(1);
begin;
insert into t_tempddl_mix values(42);
create temp table tempddl(id int);
commit;

table t_tempddl_mix;

\c :node2
table t_tempddl_mix;


-- test CTA replication inside explain
\c :node1
DO $$
BEGIN
	EXECUTE 'EXPLAIN ANALYZE create table explain_cta as select 42 as col;';
END$$;
table explain_cta;

\c :node3
table explain_cta;


--- test schemas
\c :node1
create user user1;
create schema user1;
alter schema user1 owner to user1;
create table user1.test(i int primary key);
table test;
table user1.test;

\c :node2
table test;
table user1.test;


--- scheduler example with secdefs and triggers
\c :node1
CREATE TABLE aaa (
    id   int primary key,
    text text
);
CREATE TABLE aaa_copy (LIKE aaa);
ALTER  TABLE aaa_copy ADD submit_time timestamp NOT NULL DEFAULT now();
ALTER  TABLE aaa_copy ADD submitter text NOT NULL DEFAULT session_user;
ALTER  TABLE aaa_copy ADD version_id SERIAL NOT NULL;
ALTER  TABLE aaa_copy ADD PRIMARY KEY (id, version_id);

CREATE FUNCTION add_aaa(
  aid   integer
) RETURNS integer AS
$BODY$
DECLARE
  nid integer;
BEGIN
  INSERT INTO aaa (id, text) VALUES (aid, 'zzz') RETURNING id INTO nid;
  RETURN nid;
END
$BODY$
LANGUAGE plpgsql SECURITY DEFINER;

CREATE FUNCTION drop_aaa(
  aid   integer
) RETURNS integer AS
$BODY$
BEGIN
  DELETE FROM aaa WHERE id = aid;
  RETURN aid;
END
$BODY$
LANGUAGE plpgsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION on_aaa_update() RETURNS TRIGGER
AS $BODY$
DECLARE
  aaa_id integer;
BEGIN
  aaa_id := NEW.id;
  INSERT INTO aaa_copy VALUES (NEW.*);
  IF TG_OP = 'UPDATE' THEN
    INSERT INTO aaa_copy VALUES (NEW.*);
  END IF;
  RETURN OLD;
END
$BODY$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION on_aaa_delete() RETURNS TRIGGER
AS $BODY$
DECLARE
  aaa_id INTEGER;
BEGIN
  aaa_id := OLD.id;
  DELETE FROM aaa_copy WHERE id = aaa_id;
  RETURN OLD;
END
$BODY$ LANGUAGE plpgsql;

CREATE TRIGGER aaa_update_trigger
AFTER UPDATE OR INSERT ON aaa
  FOR EACH ROW EXECUTE PROCEDURE on_aaa_update();

CREATE TRIGGER aaa_delete_trigger
BEFORE DELETE ON aaa
  FOR EACH ROW EXECUTE PROCEDURE on_aaa_delete();

select add_aaa(58);
select add_aaa(5833);
select add_aaa(582);

delete from aaa;

table aaa;
table aaa_copy;

\c :node3

table aaa;
table aaa_copy;


-- check our custom publications don't interfere with updates/deletes
\c :node1
create table tbl(id int);
insert into tbl values (42);
update tbl set id = id * 2;
table tbl;

\c :node2
table tbl;
drop table tbl;


-- search path checks
\c :node1
set search_path to '';
create table sp_test(i int primary key);
create table public.sp_test(i int primary key);
reset search_path;
drop table sp_test;
create table sp_test(i int primary key);


-- portals

BEGIN;
DECLARE foo1 CURSOR WITH HOLD FOR SELECT 1;
DECLARE foo2 CURSOR WITHOUT HOLD FOR SELECT 1;
SELECT name FROM pg_cursors ORDER BY 1;
CLOSE ALL;
SELECT name FROM pg_cursors ORDER BY 1;
COMMIT;


