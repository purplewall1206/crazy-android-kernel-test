-- q00: schema + arg-key discovery (run once per trace shape)
select name, count(*) as n from raw group by name order by n desc;
select distinct a.key from args a join raw r on a.arg_set_id = r.arg_set_id
  where r.name = 'mmap_lock_acquire_returned';
select distinct a.key from args a join raw r on a.arg_set_id = r.arg_set_id
  where r.name = 'tlb_flush';
select distinct a.key from args a join raw r on a.arg_set_id = r.arg_set_id
  where r.name = 'mmap_lock_released';
select distinct a.key from args a join raw r on a.arg_set_id = r.arg_set_id
  where r.name = 'mmap_lock_start_locking';
select distinct a.key from args a join raw r on a.arg_set_id = r.arg_set_id
  where r.name like 'g2c%';
