-- 4) EXACT lock-section pairing using the tracepoint args (write flag).
--    6.18 semantics observed: munmap path takes mmap_lock write then
--    mmap_write_downgrade()s to read (emits acquire_returned WITHOUT a
--    start_locking and WITHOUT a matching write-release). Therefore:
--      - pair acquire_returned <-> released FIFO per (utid, write, success=1)
--      - unpaired write-acquires = downgraded sections (counted, expected)
--    hold_ns = duration of one contiguous mmap_lock-held section.
with ev as (
  select e.ts, e.utid, e.name,
         (select int_value from args a where a.arg_set_id = e.arg_set_id and a.key = 'write') as wr,
         (select int_value from args a where a.arg_set_id = e.arg_set_id and a.key = 'success') as ok
  from ftrace_event e
  where e.name in ('mmap_lock_acquire_returned','mmap_lock_released')
),
acq as (select utid, wr, ts from ev where name='mmap_lock_acquire_returned' and ok=1),
rel as (select utid, wr, ts from ev where name='mmap_lock_released'),
pair as (
  select a.utid, a.wr, (r.ts - a.ts) as ns
  from (select utid, wr, ts, row_number() over (partition by utid, wr order by ts) as seq
        from acq) a
  join (select utid, wr, ts, row_number() over (partition by utid, wr order by ts) as seq
        from rel) r
    on a.utid = r.utid and a.wr = r.wr and a.seq = r.seq
),
pranked as (
  select p.utid, p.wr, p.ns,
         row_number() over (partition by p.utid, p.wr order by p.ns) as rn,
         count(*)     over (partition by p.utid, p.wr)           as cnt
  from pair p
)
select case wr when 1 then 'HOLD_WRITE_BY_THREAD' else 'HOLD_READ_BY_THREAD' end as section,
       utid, cnt as pairs, avg(ns) as mean_ns,
       max(case when rn = cast(cnt*0.50 + 0.5 as int) then ns end) as p50_ns,
       max(case when rn = cast(cnt*0.90 + 0.5 as int) then ns end) as p90_ns,
       max(case when rn = cast(cnt*0.99 + 0.5 as int) then ns end) as p99_ns,
       max(ns) as max_ns
from pranked group by utid, wr order by wr desc, pairs desc;

-- unpaired acquires (expected: write acquires consumed by downgrade)
select case a.wr when 1 then 'UNPAIRED_WRITE_ACQ' else 'UNPAIRED_READ_ACQ' end as section,
       count(*) as n
from (select utid, wr, ts, row_number() over (partition by utid, wr order by ts) as seq
      from (select e.utid,
                   (select int_value from args a where a.arg_set_id = e.arg_set_id and a.key='write') as wr,
                   e.ts
            from ftrace_event e where e.name='mmap_lock_acquire_returned') x) a
left join (select utid, wr, ts, row_number() over (partition by utid, wr order by ts) as seq
           from (select e.utid,
                        (select int_value from args a where a.arg_set_id = e.arg_set_id and a.key='write') as wr,
                        e.ts
                 from ftrace_event e where e.name='mmap_lock_released') y) r
  on a.utid = r.utid and a.wr = r.wr and a.seq = r.seq
where r.ts is null
group by a.wr;
