-- q04: sched_breakdown (A6 SQL 3/3) - per-thread on-CPU + runqueue wait
select t.name as comm, t.tid,
       count(*) as slices,
       round(sum(iif(ts.state='Running', ts.dur, 0))/1e6, 1) as on_cpu_ms,
       round(sum(iif(ts.state='R', ts.dur, 0))/1e6, 1) as rq_wait_ms
from thread_state ts join thread t on ts.utid = t.utid
group by 1,2 order by on_cpu_ms desc limit 15;
