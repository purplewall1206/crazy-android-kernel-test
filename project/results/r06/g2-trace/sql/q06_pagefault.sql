-- q06: fault/page-churn (A6 SQL 2/3 fault_latency proxy: page alloc/free churn)
select count(*) as page_allocs from raw where name='mm_page_alloc';
select count(*) as page_frees from raw where name='mm_page_free';
