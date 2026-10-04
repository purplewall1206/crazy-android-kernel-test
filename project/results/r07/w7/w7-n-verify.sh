#!/bin/bash
# w7-n-verify.sh -- =n 折叠验证: corten 对象零产出 + mm/fs/proc/kernel 对象零 corten 符号。
set -u
RES=/home/ppw/linux-6.18/project/results/r07/w7
K=/home/ppw/linux-6.18-mva
echo "=== corten*.o 产出 (期望 0):"
ls $K/mm/corten*.o 2>/dev/null | wc -l
echo "=== mm + fs/proc + kernel 对象 corten 符号 (期望 0):"
n=$(
	for o in $K/mm/*.o $K/fs/proc/*.o $K/kernel/*.o $K/kernel/futex/*.o; do
		nm "$o" 2>/dev/null | grep -i corten && echo "HIT: $o"
	done | tee $RES/nm-n-w7.txt | wc -l)
echo "nm hits: $n"
echo "=== 对象数:"
ls $K/mm/*.o 2>/dev/null | wc -l
ls $K/fs/proc/*.o 2>/dev/null | wc -l
echo "=== task_mmu.o =n 复核:"
nm $K/fs/proc/task_mmu.o 2>/dev/null | grep -ci corten
