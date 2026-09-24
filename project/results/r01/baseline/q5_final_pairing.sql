-- 5) FINAL exact pairing, 6.18 downgrade-aware.
--    Event grammar observed on this trace per tid (all sections non-overlapping):
--      munmap: WACQ -> DOWNGRADE(read-acq, no start) ... ~95ms teardown ... RREL
--      mmap:   WACQ -> WREL (~19us)
--      read:   RACQ -> RREL (~7us)
--    Section definitions used:
--      write section = WACQ -> first of {DOWNGRADE, WREL} (FIFO per utid)
--      read  section = RACQ -> RREL (FIFO per utid)
with raw as (
  select e.ts, e.utid, e.name,
         coalesce((select int_value from args a
                   where a.arg_set_id = e.arg_set_id and a.key = 'write'), -1) as wr
  from ftrace_event e
  where e.name like 'mmap_lock%'
),
ev as (
  select ts, utid, name, wr,
         lag(name) over w as pname,
         lag(wr)   over w as pwr,
         lag(ts)   over w as pts
  from raw
  window w as (partition by utid order by ts)
),
cls as (
  select ts, utid,
         case
           when name = 'mmap_lock_acquire_returned' and wr = 1 then 'WACQ'
           when name = 'mmap_lock_acquire_returned' and wr = 0
                and pname = 'mmap_lock_acquire_returned' and pwr = 1
                and (ts - pts) < 100000 then 'DOWNGRADE'
           when name = 'mmap_lock_acquire_returned' and wr = 0 then 'RACQ'
           when name = 'mmap_lock_released' and wr = 1 then 'WREL'
           when name = 'mmap_lock_released' and wr = 0 then 'RREL'
           else 'OTHER'
         end as kind
  from ev
),
wacq as (select utid, ts, row_number() over (partition by utid order by ts) as seq
         from cls where kind = 'WACQ'),
wend as (select utid, ts, kind, row_number() over (partition by utid order by ts) as seq
         from cls where kind in ('DOWNGRADE','WREL')),
racq as (select utid, ts, row_number() over (partition by utid order by ts) as seq
         from cls where kind in ('RACQ','DOWNGRADE')),
rrel as (select utid, ts, row_number() over (partition by utid order by ts) as seq
         from cls where kind = 'RREL'),
whold as (
  select 'WRITE_SECTION' as section, a.utid, (r.ts - a.ts) as ns, r.kind as ended_by
  from wacq a join wend r on a.utid = r.utid and a.seq = r.seq
),
rhold as (
  select 'READ_SECTION' as section, a.utid, (r.ts - a.ts) as ns, 'RREL' as ended_by
  from racq a join rrel r on a.utid = r.utid and a.seq = r.seq
),
allh as (select * from whold union all select * from rhold),
ranked_t as (
  select section, utid, ns,
         row_number() over (partition by section, utid order by ns) as rn,
         count(*)     over (partition by section, utid)              as cnt
  from allh
),
ranked_o as (
  select section, ns,
         row_number() over (partition by section order by ns) as rn,
         count(*)     over (partition by section)              as cnt
  from allh
)
select section, cast(utid as text) as utid, cnt as pairs, avg(ns) as mean_ns,
       min(ns) as min_ns, max(ns) as max_ns,
       max(case when rn = cast(cnt*0.50 + 0.5 as int) then ns end) as p50_ns,
       max(case when rn = cast(cnt*0.90 + 0.5 as int) then ns end) as p90_ns,
       max(case when rn = cast(cnt*0.99 + 0.5 as int) then ns end) as p99_ns
from ranked_t group by section, utid
union all
select section, '*', cnt as pairs, avg(ns) as mean_ns, min(ns) as min_ns, max(ns) as max_ns,
       max(case when rn = cast(cnt*0.50 + 0.5 as int) then ns end) as p50_ns,
       max(case when rn = cast(cnt*0.90 + 0.5 as int) then ns end) as p90_ns,
       max(case when rn = cast(cnt*0.99 + 0.5 as int) then ns end) as p99_ns
from ranked_o group by section
order by section, utid;
