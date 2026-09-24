# D-G'' 新 P1 修复（DECLARE+punch 后 exit_group 挂死）· 修复班报告

- 日期: 2026-09-18 04:4x CST（r06 修复窗）
- 修复对象: worktree `/home/ppw/linux-6.18-m4fix`（branch m4-dg2 @ 9d74b22a1348 + D-G''/B1 工作树 + 本修复）
- diff: `patches/r06-m4dg2.diff`（重导出 1177 行 / 7 文件 / checkpatch --strict **0E 0W 0C**;
  夜班版备份 `patches/r06-m4dg2.diff.night`, 日窗版 `.dayshift`）
- **VERDICT: P1 修复实证成立, 全部复验绿（t0_dod 的 java/mmetis 端到端 rc=0 维持夜班登记的
  T0b 既有下游缺陷口径, 非本班回归）**

---

## 1. 根因（一句话机制）

`mm/memory.c` handle_mm_fault() 的 CortenMM slow hook（[P2-6] 分支, T0b 层引入）在
`FAULT_FLAG_VMA_LOCK` 置位时 `return VM_FAULT_RETRY` 而**未 `vma_end_read()`**——违反 per-VMA
锁契约（x86/arm64/s390/riscv/loongarch/powerpc/arm 的 fault 路径在
`fault & (VM_FAULT_RETRY|VM_FAULT_COMPLETED)` 时**跳过** `vma_end_read()`, 释放责任在被调方;
in-tree 先例 = `vmf_can_call_fault()` memory.c:3611 同款先 `vma_end_read` 再 RETRY）。于是
每一个"VM_CORTEN vma 上经 per-VMA 锁路径 fallback 到 legacy"的页 fault（典型: 读未提交的
arena 上部窗口 → 无事务状态 → CORTEN_FAULT_FALLBACK → `lock_vma_under_rcu` → slow hook 泄漏）
都**永久泄漏 1 个 `vm_refcnt` reader 引用**; 退出时 `exit_mmap → free_pgtables(mm_wr_locked=1)
→ vma_start_write(vma) → __vma_enter_locked` 等 `refcnt == VMA_LOCK_OFFSET+1`（writer-drain
目标）, 泄漏使 refcnt 停在 `VMA_LOCK_OFFSET+2` → **TASK_UNINTERRUPTIBLE 永久等待, SIGKILL
免疫**。

- 为何 D-G'' 后才暴露: pre-fix 该"punch 后存活并退出"路径不可达（读阶段 ACCERR 即崩,
  T0b 对照已证）; D-G'' 修复使 e1b/e5 形状走到干净退出, 泄漏首次变成 fatal。
- 与 punch_split/`__split_vma` 契约无关（取证排除了假设 2/3/4, 见 §2）。

## 2. 取证链（诊断内核, 现场已还原）

1. 复现: dg_probe3 e1b 在带 bug 内核上 **6 秒内 5/5 挂死**, 栈与夜班锚点一致:
   `__vma_start_write+0x5b/0x100 ← free_pgtables+0x11d ← exit_mmap+0x179 ← mmput ← do_exit
   ← do_group_exit ← sys_exit_group`（wchan `__vma_start_write`, SIGKILL 免疫）。
2. 临时诊断（已全部还原, 不在最终 diff 内）:
   a) `__vma_enter_locked` 卡死检测器（等 10s 后 dump 全 vma 树含 refcnt）→
   **挂死 vma = punch 上半拆分产出的 TAIL 片**, `refcnt=0x4000002` = writer 位 + attached +
   **1 个泄漏 reader**; head 片与 file vma refcnt=1 正常。
   b) 对 VM_CORTEN vma 的全部 ref 增减打点 → 泄漏事件:
   ```
   [   64.169071] DBG-REF R+ vma=…ce100 [10201000-10800000) ref=2 ra=do_user_addr_fault+0x273
   （之后无任何 R-; 随后 /proc/maps 的 R+/R- 配对正常, 最终 refcnt 停在 2）
   ```
   触发点 = 探针 `expect_file_bytes()` 的 `p[0x1000]`（=文件 vma 结束后 1 字节 = TAIL 片首字节,
   探针越界 1 字节的读）落在未提交窗口 → fallback → per-VMA 锁路径 → slow hook RETRY 泄漏。
3. 假设排除: punch 路径无任何 vma refcount 直接操作（grep 全量）; `__split_vma` 调用仪式与
   vms_gather/madvise/mprotect 调用方一致（fresh VMA_ITERATOR 从 ma_start 存储合法,
   `vma_iter_prealloc` 先行; map_count 由 `vma_complete` 记账）; `ar->vma=NULL` 降级路径与
   exit_mmap 的 drain 均无涉（挂点在 free_pgtables, drain 早已完成）。

## 3. 修复 diff 摘要（mm/memory.c, +9 −1, 最终 diff 中唯一新增改动）

```c
 	if (corten_enabled_static() && (vma->vm_flags & VM_CORTEN)) {
 		vm_fault_t cret;
 
+		if (flags & FAULT_FLAG_VMA_LOCK)
+			vma_end_read(vma);
 		if ((flags & FAULT_FLAG_VMA_LOCK) || in_atomic())
 			return VM_FAULT_RETRY;
```

（附注释块说明契约与 wedge 机理; `in_atomic()` 行保持逐字节不动 → checkpatch 0W。）
语义: VMA_LOCK 置位时该分支必然 RETRY, 先释放 reader 引用再返回, 与被调方释放契约对齐;
in_atomic 且无 VMA_LOCK 的路径（持 mmap_lock, 无 vma 引用）不变。

## 4. 复验矩阵（内核 #9 = 最终二进制; KUnit 另含 #5-#8 交叉证据）

| 项 | 结果 | 证据 |
|---|---|---|
| probe e1b（原 5/5 挂死形状） | **10/10 rc=0 干净退出, 零 D 状态**; 累计 40+ 次零挂 | 本班多轮 + guest 终验 |
| probe e5/e1/e2/fork | rc=0 全过, 干净退出 | /tmp/f-*.out（guest） |
| e4 | rc=3 -EOPNOTSUPP = 夜班改判 N/A 的 T0b 既有口径（两内核一致） | dg2-verify §判据7 |
| EEXIST ~3% | **探针伪影**: ASLR brk 堆偶尔压到 0x10000000 硬编码 BASE; `setarch -R` 25/25 全过 | probe_dbg maps 抓证 |
| run_mode_smoke | **26/26 + SMOKE-DRIVER PASS**（#7 与 #9 各一次） | guest |
| KUnit on（-smp 4, corten=on） | corten 24/0/1 + arena 22/0/0 + fault 19/0/0, 零 not ok: **#5/#6/#8/#9/#10 五次全绿** | kunit-on{5,6,8,9,10}-fix2.log |
| KUnit on 唯一一次 fail | on7: `txn_uninstall_interlock`（夜班 -smp 2 基线从不运行该 3-CPU 时序用例, runtime 20.3s）——**宿主重载 flake**: 同二进制 on8/on9/on10 全绿（on10 宿主无干扰） | kunit-on7 vs 8/9/10 |
| KUnit off（=y 内核去 corten=on） | corten 25/0/0 + arena 16/0/6 + fault 4/0/15, 零 not ok（-smp 4, 覆盖强于夜班 -smp 2 的 21/16/4） | kunit-off4-fix2.log |
| =n 七对象回归 | kernel/sys.o + mm/{memory,mmap,mremap,madvise,mempolicy,migrate}.o 全重编, 零警告, 全链接 rc=0; config 已恢复 5×CONFIG_CORTEN=y | build-n-seven-fix2.log / build-y-restore-fix2.log |
| checkpatch --strict | **0 errors / 0 warnings / 0 checks**（1177 行 / 7 文件） | patches/r06-m4dg2.diff |
| t0_dod java 腿 | off rc=0 横幅正常; on = MODE marker + **完整版本横幅**后 libc SIGSEGV（=夜班"T0b 既有下游缺陷"逐位同形, 本班无回归无越位）; 崩退进程**正常回收, 无退出楔死** | t0dod-fix2/ |
| t0_dod metis 腿 | off rc=0（checksum 2d383eeed4ceb73b）; on rc=139 = **夜班登记的 T0b 既有形状（同参复现 rc 一致）**, OQ-D 口径不计 | /tmp/metis.*.out |

## 5. 观察（不阻塞）

1. java/mmetis on-leg 的"写 present-RO PTE"ACCERR 崩点 = 夜班移交清单 #2 的 T0b 既有下游
   缺陷, 仍待新立任务（本修复不改写它, 也不应改写它）。
2. 崩退 JVM 曾留一个 D 态孤儿, wchan `v9fs_evict_inode`（9p inode 清理停顿, 栈在
   get_signal/do_group_exit, **非** `__vma_start_write`）——guest 9p 环境伪影, 与 CortenMM
   无关; 复位后消失。夜班"futex 群挂是否同根"问题就此了结: 不同根（futex 群挂为 JVM 自身
   崩溃次生, S 态可杀; P1 楔死为内核泄漏, D 态免杀, 本修复已除）。
3. `corten_test_txn_{mutex_overlap,mutex_disjoint,hole_race,uninstall_interlock}` 需要
   ≥3 online CPU; 夜班 -smp 2 口径下恒 skip。建议后续把 KUnit 判据固定在 -smp 4。

## 6. 处置

- P1 修复实证成立、全绿 → `patches/r06-m4dg2.diff` 已重导出（7 文件: 原 6 文件 + mm/memory.c）。
- **按纪律停在这里**: commit 与否由主 agent 决策（夜班建议判据仍然有效: 判据 2 java rc=0 落在
  T0b 既有下游缺陷上, 需规划者定夺分批）。
- VM 现场盒: 已重启为修复后内核 #9（corten=on, bzImage 同源 worktree）, 复现物 share 原样。

*证据: results/r06/{kunit-on5..10-fix2.log, kunit-off4-fix2.log, build-n-seven-fix2.log,
build-y-restore-fix2.log, patches/r06-m4dg2.diff}; guest 侧 /mnt/r6dg/t0dod-fix2/。*
