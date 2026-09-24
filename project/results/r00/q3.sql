select name, count(*) as n from ftrace_event group by name order by n desc;
select t.name as thread, e.name as event, count(*) as cnt
from ftrace_event e join thread t using(utid)
where e.name like 'mmap_lock%'
group by t.name, e.name order by cnt desc limit 8;
