-- q05: corten kprobe counts (per-op transaction activity)
select name, count(*) as n from raw where name like 'g2c%' group by name order by n desc;
-- top threads issuing corten transactions
select t.name as comm, t.tid, count(*) as txns
from raw r join thread t on r.utid = t.utid
where r.name='g2c_txn_begin' group by 1,2 order by txns desc limit 10;
