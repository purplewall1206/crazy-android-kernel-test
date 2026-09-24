-- q02: mmap_lock contention (A6 SQL 1/3 mmap_lock_contention)
-- NOTE: this 6.18 tracepoint carries no wait-usecs arg (discovered keys:
-- mm/memcg_id/write/success); wait time is taken from perf osq/rwsem self%
-- instead. Success-rate is the in-trace contention signal.
select case when extract_arg(arg_set_id,'write')=1 then 'write' else 'read' end as mode,
       count(*) as acquires,
       sum(extract_arg(arg_set_id,'success')) as success,
       round(sum(extract_arg(arg_set_id,'success')) * 100.0 / count(*), 2) as success_pct
from raw where name='mmap_lock_acquire_returned' group by mode;
