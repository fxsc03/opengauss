--
--test in_buffer
--
create extension gms_tcp;
create or replace function gms_tcp_test_in_buffer()
    returns void
    language plpgsql
as $function$
declare
    c gms_tcp.connection;
    num integer;
    data varchar2;
    len integer;
begin
    c = gms_tcp.open_connection(remote_host=>'127.0.0.1',
                                remote_port=>12358,
                                in_buffer_size=>20,
                                tx_timeout=>10);
    num = gms_tcp.write_line(c, 'in buffer');
    pg_sleep(1);
    
    num = gms_tcp.write_line(c, 'ok');
    pg_sleep(1);
    num = gms_tcp.available(c,1);
    if num > 0 then
        len = 5;
        data = gms_tcp.get_text(c, len);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;
    
    num = gms_tcp.write_line(c, 'ok');
    pg_sleep(1);
    num = gms_tcp.available(c,1);
    if num > 0 then
        len = 12;
        data = gms_tcp.get_text(c, len);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;
    
    num = gms_tcp.write_line(c, 'ok');
    pg_sleep(1);
    num = gms_tcp.available(c,1);
    if num > 0 then
        len = 13;
        data = gms_tcp.get_text(c, len);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;
    
    num = gms_tcp.write_line(c, 'ok');
    pg_sleep(1);
    num = gms_tcp.available(c,1);
    if num > 0 then
        len = 12;
        data = gms_tcp.get_text(c, len);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;
    
    num = gms_tcp.write_line(c, 'ok');
    pg_sleep(1);
    num = gms_tcp.available(c,1);
    if num > 0 then
        len = 5;
        data = gms_tcp.get_text(c, len);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;
    
    num = gms_tcp.write_line(c, 'ok');
    pg_sleep(1);
    num = gms_tcp.available(c,1);
    if num > 0 then
        len = 17;
        data = gms_tcp.get_text(c, len);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;
    
    num = gms_tcp.write_line(c, 'ok');
    pg_sleep(1);
    num = gms_tcp.available(c,1);
    if num > 0 then
        len = 9;
        data = gms_tcp.get_text(c, len);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;
    
    num = gms_tcp.write_line(c, 'ok');
    pg_sleep(1);
    num = gms_tcp.available(c,1);
    if num > 0 then
        len = 11;
        data = gms_tcp.get_text(c, len);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;
    
    num = gms_tcp.write_line(c, 'ok');
    pg_sleep(1);
    num = gms_tcp.available(c,1);
    if num > 0 then
        data = gms_tcp.get_line(c);
        raise info 'available: %, rcv: %.', num, data;
    end if;

    gms_tcp.close_all_connections();

exception
    when gms_tcp_network_error then
        raise info 'caught gms_tcp_network_error';
        gms_tcp.close_all_connections();
        
    when gms_tcp_bad_argument then
        raise info 'caught gms_tcp_bad_argument';
        gms_tcp.close_all_connections();
        
    when gms_tcp_buffer_too_small then
        raise info 'caught gms_tcp_buffer_too_small';
        gms_tcp.close_all_connections();
        
    when gms_tcp_end_of_input then
        raise info 'caught gms_tcp_end_of_input';
        gms_tcp.close_all_connections();

    when gms_tcp_transfer_timeout then
        raise info 'caught gms_tcp_transfer_timeout';
        gms_tcp.close_all_connections();

    when gms_tcp_partial_multibyte_char then
        raise info 'caught gms_tcp_partial_multibyte_char';
        gms_tcp.close_all_connections();
        
    when others then
        raise info 'caught others';
        gms_tcp.close_all_connections();
end;
$function$;

--
--test read data
--
--get line
create or replace function gms_tcp_test_get_line()
    returns void
    language plpgsql
as $function$
declare
    c gms_tcp.connection;
    num integer;
    data varchar2;
begin
    c = gms_tcp.open_connection(remote_host=>'127.0.0.1',
                                remote_port=>12358,
                                in_buffer_size=>20480,
                                out_buffer_size=>20480,
                                tx_timeout=>10);
    num = gms_tcp.write_line(c, 'get line');
    gms_tcp.flush(c);

    num = gms_tcp.available(c,1);
    if num > 0 then
        data = gms_tcp.get_line(c, true);
        raise info 'available: %, rcv: %.', num, data;
    end if;

    gms_tcp.close_all_connections();

exception
    when others then
        raise info 'caught others';
        gms_tcp.close_all_connections();
end;
$function$;

--get text
create or replace function gms_tcp_test_get_text()
    returns void
    language plpgsql
as $function$
declare
    c gms_tcp.connection;
    num integer;
    data varchar2;
begin
    c = gms_tcp.open_connection(remote_host=>'127.0.0.1',
                                remote_port=>12358,
                                in_buffer_size=>20480,
                                out_buffer_size=>20480,
                                tx_timeout=>10);
    num = gms_tcp.write_line(c, 'get text');
    gms_tcp.flush(c);

    num = gms_tcp.available(c,1);
    if num > 0 then
        data = gms_tcp.get_text(c, 17, true);
        raise info 'available: %, rcv: %.', num, data;
    end if;

    gms_tcp.close_all_connections();

exception
    when others then
        raise info 'caught others';
        gms_tcp.close_all_connections();
end;
$function$;

--get raw
create or replace function gms_tcp_test_get_raw()
    returns void
    language plpgsql
as $function$
declare
    c gms_tcp.connection;
    num integer;
    data raw;
begin
    c = gms_tcp.open_connection(remote_host=>'127.0.0.1',
                                remote_port=>12358,
                                in_buffer_size=>20480,
                                out_buffer_size=>20480,
                                tx_timeout=>10);
    num = gms_tcp.write_line(c, 'get raw');
    gms_tcp.flush(c);

    num = gms_tcp.available(c,1);
    if num > 0 then
        data = gms_tcp.get_raw(c, 4, true);
        raise info 'available: %, rcv: %.', num, data;
    end if;
    
    num = gms_tcp.available(c,1);
    if num > 0 then
        data = gms_tcp.get_raw(c, 8);
        raise info 'available: %, rcv: %.', num, data;
    end if;

    gms_tcp.close_all_connections();

exception
    when others then
        raise info 'caught others';
        gms_tcp.close_all_connections();
end;
$function$;

--read line
create or replace function gms_tcp_test_read_line()
    returns void
    language plpgsql
as $function$
declare
    c gms_tcp.connection;
    num integer;
    data varchar2;
    len integer;
begin
    c = gms_tcp.open_connection(remote_host=>'127.0.0.1',
                                remote_port=>12358,
                                in_buffer_size=>20480,
                                out_buffer_size=>20480,
                                tx_timeout=>10);
    num = gms_tcp.write_line(c, 'read line');
    gms_tcp.flush(c);

    num = gms_tcp.available(c,1);
    if num > 0 then
        gms_tcp.read_line(c, data, len, true);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;
    
    num = gms_tcp.available(c,1);
    if num > 0 then
        gms_tcp.read_line(c, data, len);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;

    gms_tcp.close_all_connections();

exception
    when others then
        raise info 'caught others';
        gms_tcp.close_all_connections();
end;
$function$;

--read text
create or replace function gms_tcp_test_read_text()
    returns void
    language plpgsql
as $function$
declare
    c gms_tcp.connection;
    num integer;
    data varchar2;
    len integer;
    out_len integer;
begin
    c = gms_tcp.open_connection(remote_host=>'127.0.0.1',
                                remote_port=>12358,
                                in_buffer_size=>20480,
                                out_buffer_size=>20480,
                                tx_timeout=>10);
    num = gms_tcp.write_line(c, 'read text');
    gms_tcp.flush(c);

    num = gms_tcp.available(c,1);
    if num > 0 then
        out_len = 18;
        gms_tcp.read_text(c, data, len, out_len, true);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;

    gms_tcp.close_all_connections();

exception
    when others then
        raise info 'caught others';
        gms_tcp.close_all_connections();
end;
$function$;

create or replace function gms_tcp_test_read_raw()
    returns void
    language plpgsql
as $function$
declare
    c gms_tcp.connection;
    num integer;
    data raw;
    len integer;
    out_len integer;
begin
    c = gms_tcp.open_connection(remote_host=>'127.0.0.1',
                                remote_port=>12358,
                                in_buffer_size=>20480,
                                tx_timeout=>10);
    num = gms_tcp.write_line(c, 'read raw');
    gms_tcp.flush(c);

    num = gms_tcp.available(c,1);
    if num > 0 then
        out_len = 3;
        gms_tcp.read_raw(c, data, len, out_len, true);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;
    
    num = gms_tcp.available(c,1);
    if num > 0 then
        out_len = 4;
        gms_tcp.read_raw(c, data, len, out_len, true);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;
    
    num = gms_tcp.available(c,1);
    if num > 0 then
        out_len = 8;
        gms_tcp.read_raw(c, data, len, out_len);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;

    gms_tcp.close_all_connections();

exception
    when others then
        raise info 'caught others';
        gms_tcp.close_all_connections();
end;
$function$;

--write line
create or replace function gms_tcp_test_write_line()
    returns void
    language plpgsql
as $function$
declare
    c gms_tcp.connection;
    num integer;
    data varchar2;
    len integer;
begin
    c = gms_tcp.open_connection(remote_host=>'127.0.0.1',
                                remote_port=>12358,
                                in_buffer_size=>20480,
                                newline=>'LF',
                                tx_timeout=>10);
    num = gms_tcp.write_line(c, 'write line');
    pg_sleep(1);
    num = gms_tcp.write_line(c, '0123456789');

    num = gms_tcp.available(c,1);
    if num > 0 then
        gms_tcp.read_line(c, data, len);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;

    gms_tcp.close_all_connections();

exception
    when others then
        raise info 'caught others';
        gms_tcp.close_all_connections();
end;
$function$;

--write text
create or replace function gms_tcp_test_write_text()
    returns void
    language plpgsql
as $function$
declare
    c gms_tcp.connection;
    num integer;
    data varchar2;
    len integer;
begin
    c = gms_tcp.open_connection(remote_host=>'127.0.0.1',
                                remote_port=>12358,
                                in_buffer_size=>20480,
                                --out_buffer_size=>20480,
                                tx_timeout=>10);
    num = gms_tcp.write_text(c, 'write text', 10);
    pg_sleep(1);
    num = gms_tcp.write_text(c, '0123456789', 6);

    num = gms_tcp.available(c,1);
    if num > 0 then
        gms_tcp.read_line(c, data, len);
        raise info 'available: %, rcv: %(%).', num, data, len;
    end if;

    gms_tcp.close_all_connections();

exception
    when others then
        raise info 'caught others';
        gms_tcp.close_all_connections();
end;
$function$;

create or replace function gms_tcp_test_error_in_buffer_size()
    returns void
    language plpgsql
as $function$
declare
    c gms_tcp.connection;
begin
    c = gms_tcp.open_connection(remote_host=>'127.0.0.1',
                                remote_port=>12358,
                                in_buffer_size=>40480,
                                out_buffer_size=>20480,
                                newline=>'lf',
                                tx_timeout=>10);
    gms_tcp.close_all_connections();

exception
    when gms_tcp_network_error then
        raise info 'caught gms_tcp_network_error';
        gms_tcp.close_all_connections();
        
    when gms_tcp_bad_argument then
        raise info 'caught gms_tcp_bad_argument';
        gms_tcp.close_all_connections();
        
    when gms_tcp_buffer_too_small then
        raise info 'caught gms_tcp_buffer_too_small';
        gms_tcp.close_all_connections();
        
    when gms_tcp_end_of_input then
        raise info 'caught gms_tcp_end_of_input';
        gms_tcp.close_all_connections();

    when gms_tcp_transfer_timeout then
        raise info 'caught gms_tcp_transfer_timeout';
        gms_tcp.close_all_connections();

    when gms_tcp_partial_multibyte_char then
        raise info 'caught gms_tcp_partial_multibyte_char';
        gms_tcp.close_all_connections();
        
    when others then
        raise info 'caught others';
        gms_tcp.close_all_connections();
end;
$function$;

create or replace function gms_tcp_test_error_out_buffer_size()
    returns void
    language plpgsql
as $function$
declare
    c gms_tcp.connection;
begin
    c = gms_tcp.open_connection(remote_host=>'127.0.0.1',
                                remote_port=>12358,
                                in_buffer_size=>20480,
                                out_buffer_size=>40480,
                                newline=>'lf',
                                tx_timeout=>10);
    gms_tcp.close_all_connections();

exception
    when gms_tcp_network_error then
        raise info 'caught gms_tcp_network_error';
        gms_tcp.close_all_connections();
        
    when gms_tcp_bad_argument then
        raise info 'caught gms_tcp_bad_argument';
        gms_tcp.close_all_connections();
        
    when gms_tcp_buffer_too_small then
        raise info 'caught gms_tcp_buffer_too_small';
        gms_tcp.close_all_connections();
        
    when gms_tcp_end_of_input then
        raise info 'caught gms_tcp_end_of_input';
        gms_tcp.close_all_connections();

    when gms_tcp_transfer_timeout then
        raise info 'caught gms_tcp_transfer_timeout';
        gms_tcp.close_all_connections();

    when gms_tcp_partial_multibyte_char then
        raise info 'caught gms_tcp_partial_multibyte_char';
        gms_tcp.close_all_connections();
        
    when others then
        raise info 'caught others';
        gms_tcp.close_all_connections();
end;
$function$;

--
--char_set
--
create or replace function gms_tcp_test_char_set()
    returns void
    language plpgsql
as $function$
declare
    c gms_tcp.connection;
    num integer;
    data varchar2;
begin
    c = gms_tcp.open_connection(remote_host=>'127.0.0.1',
                                remote_port=>12358,
                                in_buffer_size=>20480,
                                out_buffer_size=>20480,
                                cset=>'gbk',
                                tx_timeout=>10);
    num = gms_tcp.write_line(c, 'char set');
    gms_tcp.flush(c);

    num = gms_tcp.available(c,1);
    if num > 0 then
        data = gms_tcp.get_line(c);
        raise info 'available: %, rcv: %.', num, data;
    end if;

    gms_tcp.close_all_connections();

exception
    when others then
        raise info 'caught others';
        gms_tcp.close_all_connections();
end;
$function$;

create or replace function gms_tcp_test_quit()
    returns void
    language plpgsql
as $function$
declare
    c gms_tcp.connection;
    num integer;
begin
    c = gms_tcp.open_connection(remote_host=>'127.0.0.1',
                                remote_port=>12358,
                                tx_timeout=>10);
    num = gms_tcp.write_line(c, 'quit');

    gms_tcp.close_all_connections();

exception
    when others then
        raise info 'caught others';
        gms_tcp.close_all_connections();
end;
$function$;

select pg_sleep(5);

select gms_tcp_test_in_buffer();
select gms_tcp_test_get_line();
select gms_tcp_test_get_text();
select gms_tcp_test_get_raw();
select gms_tcp_test_read_line();
select gms_tcp_test_read_text();
select gms_tcp_test_read_raw();
select gms_tcp_test_write_line();
select gms_tcp_test_write_text();
select gms_tcp_test_error_in_buffer_size();
select gms_tcp_test_error_out_buffer_size();
select gms_tcp_test_char_set();
select gms_tcp_test_quit();

drop function gms_tcp_test_in_buffer();
drop function gms_tcp_test_get_line();
drop function gms_tcp_test_get_text();
drop function gms_tcp_test_get_raw();
drop function gms_tcp_test_read_line();
drop function gms_tcp_test_read_text();
drop function gms_tcp_test_read_raw();
drop function gms_tcp_test_write_line();
drop function gms_tcp_test_write_text();
drop function gms_tcp_test_error_in_buffer_size();
drop function gms_tcp_test_error_out_buffer_size();
drop function gms_tcp_test_char_set();
drop function gms_tcp_test_quit();
