DROP FUNCTION IF EXISTS pg_catalog.query_node_reform_info() CASCADE;
SET LOCAL inplace_upgrade_next_system_object_oids=IUO_PROC, 2867;
CREATE OR REPLACE FUNCTION pg_catalog.query_node_reform_info
(
    OUT reform_node_id          integer,
    OUT reform_type             text,
    OUT reform_start_time       timestamp with time zone,
    OUT reform_end_time         timestamp with time zone,
    OUT is_reform_success       boolean,
    OUT redo_start_time         timestamp with time zone,
    OUT redo_end_time           timestamp with time zone,
    OUT xlog_total_bytes        int8,
    OUT hashmap_construct_time  timestamp with time zone,
    OUT action                  text
)
 RETURNS SETOF record
 LANGUAGE internal
 STRICT NOT FENCED NOT SHIPPABLE ROWS 64
AS $function$query_node_reform_info$function$;
comment on function pg_catalog.query_node_reform_info
(
    OUT reform_node_id          integer,
    OUT reform_type             text,
    OUT reform_start_time       timestamp with time zone,
    OUT reform_end_time         timestamp with time zone,
    OUT is_reform_success       boolean,
    OUT redo_start_time         timestamp with time zone,
    OUT redo_end_time           timestamp with time zone,
    OUT xlog_total_bytes        int8,
    OUT hashmap_construct_time  timestamp with time zone,
    OUT action                  text
) is 'query node reform information';
