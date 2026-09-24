-- 3a) WAIT latency v2: for each start_locking, the first acquire_returned of the
-- same utid at ts >= start ts (correlated min; approximate under interleaving).
with stk as (
  select utid, ts from ftrace_event where name='mmap_lock_start_locking'
),
acq as (
  select utid, ts from ftrace_event where name='mmap_lock_acquire_returned'
),
wait as (
  select s.utid, (select min(a.ts) from acq a where a.utid = s.utid and a.ts >= s.ts) - s.ts as ns
  from stk s
)
select 'WAIT_OVERALL_V2' as section, '*' as thread,
       count(*) as pairs, avg(ns) as mean_ns, min(ns) as min_ns,
       max(ns) as max_ns
from wait where ns is not null;

-- 3b) per-TID page_fault_user counts (top 12)
select 'PF_BY_THREAD' as section, t.tid, t.name as thread, count(*) as faults
from ftrace_event e join thread t using(utid)
where e.name = 'page_fault_user'
group by e.utid, t.tid, t.name order by faults desc limit 12;

-- 3c) full raw dump of every mmap_lock event (tiny; enables manual audit of pairing)
select 'MMAP_LOCK_DUMP' as section, e.ts, t.tid, t.name as thread, e.name as event
from ftrace_event e join thread t using(utid)
where e.name like 'mmap_lock%'
order by e.ts;
