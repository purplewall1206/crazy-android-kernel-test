-- mmap_lock pairing per TID (strict alternation assumption: every
-- acquire_returned of a utid is later released by the same utid, FIFO by rank).
-- hold_ns  = acquire_returned -> released   (locked section duration)
-- wait_ns  = start_locking -> acquire_returned (lock acquisition wait, 0 = uncontended fastpath)
-- NOTE: CTEs are repeated per statement: a WITH clause only scopes one statement.

-- 1) HOLD latency per thread (tid)
with ev as (
  select ts, name, utid,
         row_number() over (partition by utid, name order by ts) as seq
  from ftrace_event
  where name in ('mmap_lock_acquire_returned','mmap_lock_released')
),
hold as (
  select a.utid, (r.ts - a.ts) as ns
  from (select utid, seq, ts from ev where name='mmap_lock_acquire_returned') a
  join (select utid, seq, ts from ev where name='mmap_lock_released') r
    on a.utid = r.utid and a.seq = r.seq
),
hranked as (
  select t.utid, t.tid, t.name as thread, h.ns,
         row_number() over (partition by h.utid order by h.ns) as rn,
         count(*)     over (partition by h.utid)           as cnt
  from hold h join thread t using(utid)
)
select 'HOLD_BY_THREAD' as section, utid, tid, thread,
       cnt as pairs, avg(ns) as mean_ns,
       max(case when rn = cast(cnt*0.50 + 0.5 as int) then ns end) as p50_ns,
       max(case when rn = cast(cnt*0.90 + 0.5 as int) then ns end) as p90_ns,
       max(case when rn = cast(cnt*0.99 + 0.5 as int) then ns end) as p99_ns,
       max(ns) as max_ns
from hranked group by utid, tid, thread order by pairs desc;

-- 2) HOLD latency overall
with ev as (
  select ts, name, utid,
         row_number() over (partition by utid, name order by ts) as seq
  from ftrace_event
  where name in ('mmap_lock_acquire_returned','mmap_lock_released')
),
hold as (
  select a.utid, (r.ts - a.ts) as ns
  from (select utid, seq, ts from ev where name='mmap_lock_acquire_returned') a
  join (select utid, seq, ts from ev where name='mmap_lock_released') r
    on a.utid = r.utid and a.seq = r.seq
)
select 'HOLD_OVERALL' as section, '*' as thread,
       count(*) as pairs, avg(ns) as mean_ns,
       max(case when rn = cast(cnt*0.50 + 0.5 as int) then ns end) as p50_ns,
       max(case when rn = cast(cnt*0.90 + 0.5 as int) then ns end) as p90_ns,
       max(case when rn = cast(cnt*0.99 + 0.5 as int) then ns end) as p99_ns,
       max(ns) as max_ns
from (select ns, row_number() over (order by ns) as rn, count(*) over () as cnt from hold);

-- 3) WAIT latency (start_locking -> acquire_returned) overall
with ev as (
  select ts, name, utid,
         row_number() over (partition by utid, name order by ts) as seq
  from ftrace_event
  where name in ('mmap_lock_start_locking','mmap_lock_acquire_returned')
),
wait as (
  select s.utid, (a.ts - s.ts) as ns
  from (select utid, seq, ts from ev where name='mmap_lock_start_locking') s
  join (select utid, seq, ts from ev where name='mmap_lock_acquire_returned') a
    on s.utid = a.utid and s.seq = a.seq
)
select 'WAIT_OVERALL' as section, '*' as thread,
       count(*) as pairs, avg(ns) as mean_ns,
       max(case when rn = cast(cnt*0.50 + 0.5 as int) then ns end) as p50_ns,
       max(case when rn = cast(cnt*0.90 + 0.5 as int) then ns end) as p90_ns,
       max(case when rn = cast(cnt*0.99 + 0.5 as int) then ns end) as p99_ns,
       max(ns) as max_ns
from (select ns, row_number() over (order by ns) as rn, count(*) over () as cnt from wait);
