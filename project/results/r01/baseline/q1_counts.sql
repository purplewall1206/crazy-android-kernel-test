-- baseline trace: event counts + coverage window
select name, count(*) as n from ftrace_event group by name order by n desc;
select (max(ts)-min(ts))/1e9 as covered_window_s, min(ts) as ts_min, max(ts) as ts_max from ftrace_event;
