-- q03: tlb_flush shape (fullmm pages=-1 vs ranged) + IPI raise counts
select case when cast(extract_arg(arg_set_id,'pages') as int64) = -1
            then 'fullmm(-1)' else 'ranged(' || extract_arg(arg_set_id,'pages') || ')' end as shape,
       count(*) as n
from raw where name='tlb_flush' group by shape order by n desc;
select count(*) as ipi_raise_total from raw where name='ipi_raise';
select extract_arg(arg_set_id,'reason') as ipi_reason, count(*) as n
from raw where name='ipi_raise' group by ipi_reason order by n desc limit 5;
-- per-thread flush leaders
select t.name as comm, t.tid, count(*) as flushes,
       sum(case when cast(extract_arg(r.arg_set_id,'pages') as int64) = -1 then 1 else 0 end) as fullmm
from raw r join thread t on r.utid = t.utid
where r.name='tlb_flush' group by 1,2 order by flushes desc limit 10;
