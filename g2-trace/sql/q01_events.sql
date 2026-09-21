-- q01: full event inventory (count per ftrace event)
select name, count(*) as n from raw group by name order by n desc;
