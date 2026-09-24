# M3b S4-S7 guest 冒烟验证清单 (r02, review-fix 后)

前置: 本清单对应补丁 `patches/r02-m3b-s46-full.diff`(HEAD=1284a235f751 + M3a + S1-S3 + S4-S7)。
覆盖 M3 DoD 8 条; KUnit 部分由 dev agent 在 host 无盘 qemu 完成, 本清单聚焦 guest 冒烟。
前置状态: S123_VERIFY_DONE 已出现 (S1-S3 验证通过), S4-S7 review 修复(FAIL-1/2, C1-C3)已应用
——**perf 符号判定在 FAIL-2 修复前会阳性(ar->vma 缓存缺失导致热路径 vma_lookup), 本清单按修复后代码执行**。

## 0. 构建与启动

```
# host (worktree linux-6.18-m3b46):
make olddefconfig && make -j6 && cp arch/x86/boot/bzImage $PROJ/bzimg/r02-m3b-s46
# 启动 (corten=on):
tmux kill-session -t vm 2>/dev/null
KERNEL=$PROJ/bzimg/r02-m3b-s46 bash ~/bench/host/launch_vm.sh ~/vm/trixie.img \
  "systemd.mask=sys-kernel-config.mount"    # cmd line 追加 corten=on (按 M2 惯例)
```
启动检查: dmesg 无 panic/WARN/lockdep 报警; `cat /sys/kernel/debug/corten/stats` 有输出。

## 1. DoD: boot corten=on + prctl 声明 (§2.1)
```
~/vm/gssh '/mnt/arena-stress/arena_stress --probe'
# 判定: JSON supported:true / declare 成功路径 (非 -EINVAL/-EOPNOTSUPP/-EPERM)
```

## 2. DoD: arena 内 PF 走事务、无 VMA 树/mmap_lock (§4, §7.3)
```
# ① 触页路径 (S4/S5 主链路):
~/vm/gssh '/mnt/arena-stress/arena_stress 4 10 1024 1 --mode touch --verify'
# ② churn: mmap(MAP_FIXED) mark 事务 + fault + munmap 事务 (S5/S6):
~/vm/gssh '/mnt/arena-stress/arena_stress 8 20 1024 2 --mode churn --fixed --verify'
# ③ mixed (touch+churn 交替, magic 校验):
~/vm/gssh '/mnt/arena-stress/arena_stress 8 15 512 4 --mode mixed --verify'
# ④ 锁竞争变体 (§6.2, 多线程同 2M 窗口):
~/vm/gssh '/mnt/arena-stress/arena_stress 8 10 256 5 --hammer-race'
# ⑤ get_unmapped_area 不得选中 shadow-VMA (churn --fresh):
~/vm/gssh '/mnt/arena-stress/arena_stress 4 10 256 3 --mode churn --fresh'
# 判定: 全部 errors:0 / op_errors:0 / exit 0; ①-④ minflt>0
```

## 3. DoD: perf 确认 fault 路径无 find_vma/mmap_lock 符号 (§7.3)
```
# guest 内:
gssh 'cd /mnt && (./tracebox -c /mnt/fchurn.c --txt -o /mnt/s46.pftrace &) ; \
  ./arena-stress/arena_stress 8 15 512 7 --mode touch; sleep 2'
# host 侧 trace_processor SQL 按 §7.3 判定: user-PF 栈命中
#   find_vma|lock_vma_under_rcu|vma_start_read|lock_mm_and_find_vma|mmap_read_lock|mas_walk
#   → FAIL (get_user/fixup_user_fault 栈内豁免);
#   栈内必含 corten_arena_user_fault + corten_lock_range (>N 次)。
# 注: FAIL-2 修复后 ar->vma 为缓存指针, 热路径不应出现 vma_lookup 符号; 若出现 = 回归。
```

## 4. DoD: /proc/pid/maps 正常 (§3.4)
```
# 运行中快照 (arena_stress stderr 打印 arena_range):
~/vm/gssh 'timeout 20 /mnt/arena-stress/maps_check.sh \
  "/mnt/arena-stress/arena_stress 4 30 256 8 --mode touch" --arena'
# 判定: 恰 1 条(或合并后 1 条) [anon:corten_arena] 区间, 起止与 DECLARE 一致
```

## 5. DoD: fork 防线 (S7, §5.1) — fork 失败 + 父进程无损
```
~/vm/gssh '/mnt/arena-stress/fork_arena_test'
# 判定: "fork failed as designed: errno=13 (ENOMEM)" (dup_mm 把 -EOPNOTSUPP 折叠为
#   -ENOMEM; 设计矩阵 5.1 的 fail-fast 生效即可) + "fork_arena_test: PASS" + exit 0
# 注: glibc 层看不到 EOPNOTSUPP — 已在报告与 dup_mmap 注释记录。
```

## 6. DoD: kselftests/mm 冒烟子集 (§7.2) — 双口径 off/on
```
# corten=off 重启后跑一遍, corten=on 再跑一遍:
~/vm/gssh 'bash /mnt/arena-stress/ksmoke.sh'   # runner 内含 8 个子集 (map_fixed_noreplace,
#   mremap_test, mremap_dontunmap, madv_populate, cow, mkdirty, protection_keys,
#   khugepaged/split_huge_page, gup_longterm)
# 判定: off 全绿; on 与 off 同一 fail-set (新 fail = S4-S7 钩子回归)
```

## 7. KUnit (host 无盘 qemu, dev agent 已跑; 结果存 results/r02/)
```
qemu -smp 2 无盘: kunit filter_glob=corten →
  corten_protocol / corten_fault (10 用例: dispatch/unmap_classify/mmap_classify/
  map_anon/zero_page/sigsegv/restore/fill_upper_race/map_race/chunk_unmap) /
  corten_arena 全绿 ×2
```

## 8. debugfs 快照 (S8 前: arenas/arena_stats 文件未接线, 仅 corten 基础文件)
```
~/vm/gssh 'cat /sys/kernel/debug/corten/stats /sys/kernel/debug/corten/txn'
# 判定: 有输出; faults 计数闭合 (§7.1-④ 的 arena_stats 聚合推迟到 S8 接线)
```

## 拒绝面快速核查 (S6 矩阵, 可选 one-liner)
```
# DECLARE 后依次执行并断言 -EOPNOTSUPP/-EEXIST:
#   mprotect(arena区) / madvise(MADV_HUGEPAGE|DONTNEED 跨界) / mremap(arena区) /
#   mlock(arena区) / mseal(arena区) / mbind / uffd 注册 / PR_SET_VMA_ANON_NAME /
#   MAP_FIXED_NOREPLACE 落区(-EEXIST) / munmap 跨界(-EOPNOTSUPP) / brk 不可达
# (guest 无现成 one-shot 工具, 由 arena_stress churn/fresh + ksmoke 间接覆盖;
#  逐 syscall 注入推迟到 M7 syzkaller focus)
```
