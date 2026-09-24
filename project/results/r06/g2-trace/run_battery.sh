#!/bin/bash
# G2 trace battery - driven from host via g26, all legs sequential.
set -u
G=/home/ppw/cortenmm/results/r06/g2-trace
LEG=/mnt/hostshare/g2/run_leg.sh
run() { echo "===== LEG $@ ====="; $G/g26 "bash $LEG $@" 2>&1; }
{
# ---- Scenario A: mmap-pf low t8 (seed 20261979), 15s windows
run A-t0-r1    trace t0   mmap-pf low 8 15 20261979
run A-t0-r2    trace t0   mmap-pf low 8 15 20261979
run A-base-r1  trace base mmap-pf low 8 15 20261979
run A-t0-perf  perf  t0   mmap-pf low 8 15 20261979
run A-base-perf perf base  mmap-pf low 8 15 20261979
# ---- Scenario B: unmap-virt low t4 (seed 20263969), 15s windows
run B-t0-r1    trace t0   unmap-virt low 4 15 20263969
run B-base-r1  trace base unmap-virt low 4 15 20263969
run B-t0-perf  perf  t0   unmap-virt low 4 15 20263969
run B-base-perf perf base unmap-virt low 4 15 20263969
} > $G/logs/battery.log 2>&1
echo BATTERY_DONE
