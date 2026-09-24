select round((max(ts)-min(ts))/1e9, 2) as span_s_all from raw;
select round((max(ts)-min(ts))/1e9, 2) as span_s_g2c from raw where name='g2c_txn_begin';
