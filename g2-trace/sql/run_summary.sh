#!/bin/bash
# run_summary.sh <trace> - one compact JSON-ish summary block per trace
TP=/home/ppw/tools/perfetto/linux-amd64/trace_processor_shell
T=$1
echo "@@@@@ TRACE $T"
for q in q07_span q01_events q02_mmap_lock q03_tlb_flush q05_kprobe q06_pagefault q04_sched; do
  echo "##### $q"
  $TP -q /home/ppw/cortenmm/results/r06/g2-trace/sql/$q.sql "$T" 2>/dev/null |
    grep -v 'Loading trace\|Query execution\|Trace loaded'
done
