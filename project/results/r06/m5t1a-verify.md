# M5.T1a 忠实 fork — 验证报告（r06/m5t1a，夜窗班）

- 班次: 2026-09-18 夜（21:2x 起，通宵盒 08:00）
- worktree: /home/ppw/linux-6.18-m5t1a，分支 m5-t1a
- 基线: **0719bc6ae74e**（gupfix，主树 HEAD）——本班将分支从 025756094542 rebase
  到该提交（autostash 零冲突；gupfix 的 JThreadBench 零 CFE 判据是本片验证矩阵的
  硬前提，且其 mm/corten_arena{,_test}.c 改动与 T1a 区域不重叠）
- 工作树: 7 文件 +1982/-436 → rebase 后 +2040/-441（含 gupfix 重放的
  mm/corten_arena_test.c protect_flags 用例基线）；**未 commit**（review 后由
  maintainer 班次处理）（23:03 末轮改码后为 +2003/-439, 见 §5-5a 勘误2）
- 备份: /home/ppw/cortenmm/patches/r06-m5t1a.diff（2805 行，rebase 后重导出）
- guest 判据盘: /home/ppw/vm/trixie-m5t1a.img（独立时点盘），VM = tmux
  `m5t1a-vm`，hostfwd 10025，qemu pidfile /home/ppw/vm/qemu-m5t1a.pid

---

## 1. 完成度评估（对前一班的继承）

前一班实际进度远超"代码阶段中途"：代码（含 KUnit 与 COW 分支）14:47-16:57 完成，
16:03 内核 KUnit 曾全绿；但 16:57 有一轮末尾改码（清理 corten-dbg 打印），其后的
验证全部过期；17:09 的 lockdep 构建被杀（Terminated）；guest 侧 fork_isolation
用的是 16:29 旧二进制（无 MAP_FIXED/非 PMD 对齐/无 declare）段错误——**证据作废**，
源码 16:38 已修正，本班重编后双臂 PASS。

本班复核结论: **实现完整，无需续写内核代码**。逐项对照 M5_FORK_SPEC §1.3/§2.2:

| 规格项 | 状态 | 位置 |
|---|---|---|
| frozen 位 + lookup_get 一拍 | ✓ | include/linux/corten_arena.h @frozen; corten_arena.c:2149 lookup_get |
| fork_begin（drain 冻结窗/MODE 继承/子注册表惰性建/next_va 游标/R-G fatal 检查） | ✓ | corten_arena.c:1569 |
| copy_page_range 照常 | ✓ | 未触碰 mm/memory.c（双钩子夹持） |
| fork_commit（SHARED 置位+快照/子 arena 注册/meta 深拷贝/pmd 空窗跳过/解冻单一收口/注入点×2） | ✓ | corten_arena.c:1667/1744/1839/1956/2034 |
| fork_abort（loop_out R-A/R-G unwind） | ✓ | mmap.c loop_out; corten_arena.c:1554 |
| COW 核心（免拷贝 map_count==1 + 拷贝分支; §3.4 时序裁决随 T1a） | ✓ | dispatch :2355-2390; corten_arena_cow_write :2884 |
| fork_demote 删除（无 fallback） | ✓ | demote_scrub/fork_demote 全删; 计数留作历史遥测（本 boot 恒 0） |
| 多片 shadow-VMA 逐 VMA 处理（OQ-1; 洞=标准 copy_page_range） | ✓ | fork_mirror ④-1 for_each_vma_range 逐片; fork_multipiece KUnit（真 __split_vma+punch 形状） |
| MODE 继承 + next_va 游标 | ✓ | fork_begin 首段 |
| SHARED⇒WRITABLE 记录（corten_mark 内建规则） | ✓ | mark_window nm.flags |
| E3 DONTFORK/WIPEONFORK 子跳过 | ✓ | ④-1 any_piece 测试; copy_window 注释（WIPEONFORK=子侧 PT 缺页→不记录） |
| E5 drain 超时带漏继续 + WARN | ✓ | begin 的 note_drain_timeout + unfreeze 的 percpu_ref_reinit WARN |

规格偏离（实现注册，需 review 确认）:
- **CORTEN_INVALID+perm 槽的子侧重表达**: 规格字母写"INVALID 页跳过"，实现把
  "drop-content 后的 KEEP_PERM 槽"重表达为 PRIVATE_ANON+同 perm（协议无法写
  Invalid 槽的 perm；跳过会在子侧重演 r06 rogue ACCERR 形状）。代码注释已声明
  为 DEVIATION——诚实登记，建议接受。
- fork_commit 镜像循环**不持父 ctl_lock**（register_child 要嵌套子 ctl_lock，
  两 mutex 同 lockdep class 不可表达嵌套；注册表写方本就全在 oldmm mmap_write
  之下）。frozen 记账仍在锁内收口。

## 2. 修复清单（本班实际改动）

| # | 动作 | 理由 |
|---|---|---|
| 1 | rebase m5-t1a → 0719bc6ae74e（autostash，零冲突） | 基线缺 gupfix：JThreadBench 判据前提 + 主树 HEAD 对齐 |
| 2 | =y 重建（#15），零新增警告 | 16:57 末轮改码后无构建 |
| 3 | fork_isolation 重编（/tmp/fork_isolation.c 16:38 版）| 16:29 旧二进制段错误证据作废 |
| 4 | 新增 bench/arena-stress/fork_roundtrip.c（1k 页×R 往返，判据 2 的 T1b 预演件） | 验证矩阵补"1k 页往返"证据 |
| 5 | 备份 diff 重导出 + 本报告 | 无备份风险的收口纪律 |

（无内核代码修改——前一班代码经通读复核为完整，直接进入验证。）

## 3. 验证矩阵

### 3a. =y 全量 + KUnit
- 构建: `make -j8` **零新增警告**（build #15, 21:41; 仅上游既有
  objtool cpuidle_enter_state 与 modpost memblock_end_of_DRAM 两条，diff 未触碰
  相关文件; cpuidle 条在 r07 既有 lockdep-build-r06.log 中同样出现）
- KUnit on ×2（corten=on, filter_glob=corten*）:
  - corten 24 pass/0 fail/1 skip; **corten_arena 27/0/0**（26 既有含 gupfix
    protect_flags_kernel_gate + fork_faithful/fork_unwind/fork_multipiece/fork_perm 4 新）;
    corten_fault 24/0/0（dispatch SHARED 状态机 + COW reuse/copy + dual-kthread 竞争）
  - 两轮全绿: kunit-on{1,2}-rebase.log
- KUnit off ×1: corten 25/0/0; corten_arena 18/0/**9 skip**; corten_fault 4/0/**20 skip**
  （skip=off 臂设计性, 与 gupfix 基线口径一致）: kunit-off1-rebase.log

### 3b. =n 回归 + checkpatch
- checkpatch --strict 全量 diff: **0 errors / 0 warnings / 0 checks**（2749 行）
- =n 七对象回归: **PASS**（§5: 八对象 nm 零 corten 符号, 零警告）

### 3c. guest 判据（corten=on, kernel g0719bc6ae74e-dirty #15, 8 vCPU/4G KVM）
| 判据 | 结果 |
|---|---|
| **fork_isolation 父子隔离校验和**（2 arena 形状含 routed partial commit） | **PASS 双臂**: corten before=after=e1c81840e4123903, child rewrote-own-copy; legacy 同值（跨臂等价）rc=0 |
| **fork_roundtrip 1k 页往返**（64M/16 窗, 1000 页, 1000 轮 fork→校验→子写→exit→父校验+父重触 COW） | **PASS 1000 轮**（§5: checksum 076534f9ba241483 稳定, 零错零泄漏, rc=0） |
| **metis_eq MODE 全量**（8 线程, 1.6GB text1600, STRICT hook） | **rc=0**, checksum **8a8db99075665220** = legacy 臂同值（跨臂一致）; 16MB 双臂 + 前班 08:41 基准三方同值 2cf5c1fb5e348dfd; 每轮 **fork-probe OK**（arena 活跃+2000 线程形态下 fork+waitpid, 子 exit 42）→ **OQ-D 闭环** |
| **JThreadBench 2000 线程×3 reps ×3 JVM** | **3/3 rc=0, 零 ClassFormatError**, counter=2000×3, median 3031/3064/3039ms（gupfix 基线 2999ms 带内）, fork-probe OK ×3 → gupfix 成果未被 fork 改动破坏 |
| **run_mode_smoke** | **26/26 PASS** + SMOKE PASS + SMOKE-DRIVER PASS（×3 轮） |
| java HelloFmt（LD_PRELOAD hook STRICT） | 3/3 rc=0, [hello] done, fork-probe OK |
| debugfs 对账 | fork_faithful=12（每次有 arena 的 fork +1）, fork_demotes=**0**（demote 已死, 历史遥测不再增长）, fork_skips=0 |
| dmesg | 零 Oops/BUG/panic; **39 条 find_vma_intersection assert WARN（rwsem.h:217）** = **主树既有残留**（rogue-fix.md §6-2 登记: fault_owned tier-2 RCU walk 的 mmap_assert, java 类加载高频, 功能无害）; T1a diff 未触碰该函数。全落在 java/CDS 形状, 非 fork 路径 |

### 3d. lockdep 变体（M7 联动）
- **PASS**（§5: 变体构建 #17 + 无盘 KUnit 全绿 + 零 splat; 收尾已恢复普通配置重建 #18）。
  注意: §3a/§3c 夜间证据实际产自 lockdep 开启的内核（§5 勘误1）——即全部夜间 guest
  证据额外经历了 lockdep 开销下的运行, 无一 splat。

## 4. 纪律与登记（回填 STATE 用）

- **R-A（frozen 泄漏）**: 解冻收口单一函数 corten_arena_fork_unfreeze_locked;
  begin 内部无 frozen 失败路径（唯一可失败点=子 state 分配, 发生在任何冻结前）;
  KUnit fork_unwind 注入 stage 1（commit 入口）与 stage 2（首 arena 镜像后）,
  两阶段断言 frozen==false + faithful 不计数。guest 全程 fork 后 frozen 无泄漏
  （fork_faithful=12 对账）。
- **R-B（mapcount 竞争）**: folio_mapcount 在 desc 写锁+ptl 下读（corten_arena_cow_write）;
  "解除方先清 PTE 后减 count ⇒ 观测值≥真值 ⇒ 只会多拷贝"论证注释在函数头;
  dual-kthread KUnit（corten_fault_test.c corten_ft_c0/c1）锚单次清除。
- **INV7**: copy_page_range 的 wrprotect 记为 corten_glue_pte_write 白名单第 1 处
  （DEV-14）——代码内 mmap.c:1879 / corten_arena.h:298 / corten_arena.c:1511
  三处一致注释。DESIGN.md §3.2 白名单表回填留 maintainer 班次（"≤3 处, 已用 1"）。
- **DEV-14**: fork = copy_page_range（PTE 层）+ 事务 meta 镜像; "不走
  copy_page_range"作废（M5_FORK_SPEC §1.3）。DESIGN §6 回填待 review。
- **DEV-15**: fork 冻结窗（frozen 位）为移植自造语义; REPORT 披露清单项。
- **DEV-11 废止**: fork_demote 全删; fork_demotes 计数留历史遥测（本 boot 恒 0）。
- **OQ-1**: 多片 shadow-VMA（D-G'' punch 后主线形状）逐 VMA 处理已实现——父侧
  ④-1 逐片判 VM_CORTEN, 洞由标准 copy_page_range 复制, 子侧 vma 缓存镜像父缓存
  （piece 不在则 NULL, 既有安全降级）; fork_multipiece KUnit 用真 __split_vma+
  xa_erase 复现 punch 终态。
- **OQ-3**: MADV_DONTFORK/WIPEONFORK 按"legacy 放行+子跳过"实现（④-1/E3）。
- **时间线诚实声明**: 3a/3c 的证据全部产自 rebase 后代码（g0719bc6ae74e-dirty,
  即工作树现状, 无后修改）; 前一班 16:00-17:09 的旧日志全部作废弃用。

## 5. 夜窗构建窗口执行（2026-09-19 00:2x–01:5x, timegate 免费窗口内, 全部 PASS）

### 5a. 执行前勘察更正（诚实登记, 三条）
1. **配置污染**: 前班 17:09 被杀的 lockdep 构建把 `PROVE_LOCKING=y`（连带
   LOCKDEP / DEBUG_LOCK_ALLOC / DEBUG_SPINLOCK / DEBUG_MUTEXES / DEBUG_RT_MUTEXES /
   DEBUG_RWSEMS / DEBUG_WW_MUTEX_SLOWPATH 全家族）留在 .config（本班 21:28 只恢复了
   CORTEN 三项 =y, 未清 lockdep）。因此 §3a 的 build #14/#15 与 §3c 全部 guest 证据
   实际产自 **lockdep 开启的内核**（运行中 guest dmesg 可见 RCU lockdep /
   MAX_LOCKDEP_*）——功能性结论更严不打折, 但"＝y 全量(普通)"表述按此更正。
2. **23:03 报告后改码**: mm/corten_arena.c 在本报告定稿（22:14）之后的 23:03 又有一轮
   修改, 工作树现为 **7 文件 +2003/-439**（本报告头部 +2040/-441 已过期; 备份 diff
   r06-m5t1a.diff 21:53 导出, 亦早于该轮）。改码后唯一 KUnit（kunit-on1-final.log,
   23:09, kernel #16）corten 套件 1 fail: `corten_test_txn_uninstall_interlock`
   （corten_test.c:1718/1719/1728/1729, 该用例自身 runtime 20.27s）——与
   r07/m7-preheat.md §2 已登记的"interlock 固定等待窗在 lockdep 开销下偏紧"时序
   flake 同用例同形状。本班 00:53 复跑（同源码 #17）全绿, 未复现; 按 r07 先例
   （run1 fail→复跑绿→M7 清单登记）处理。
3. **前班 1000/100 轮无保存输出**: 接手时 guest（#16, 23:05 启动, 空闲）`fork_faithful
   =1100`、dmesg 零 splat——前班确已在 #16 上跑过 1000+100 轮但输出未落盘。本班全部
   带日志重跑（下述）, boot 累计 fork_faithful 1100→2200。guest 证据内核均为
   **#16（23:04, lockdep 变体）**, 与 #17 同源码（#17 仅 =n/=y 往返后的重链）。

### 5b. 四项结果
- [x] **=n 七对象回归**: `scripts/config -d CORTEN_MM -d CORTEN_MM_ARENA
  -d CORTEN_MM_KUNIT_TEST && make olddefconfig` → 48s 构建八对象**零警告** → nm 零
  corten 符号:
  ```
  kernel/sys.o: corten_symbols=0    mm/mmap.o:     corten_symbols=0
  mm/memory.o:  corten_symbols=0    mm/migrate.o:  corten_symbols=0
  mm/mempolicy.o: corten_symbols=0  mm/mremap.o:   corten_symbols=0
  mm/madvise.o: corten_symbols=0    mm/mprotect.o: corten_symbols=0
  ```
  config 自备份精确恢复（五项 CORTEN 复归 =y）。
  （build-n-seven-r2.log / nm-n-seven-r2.log）
- [x] **lockdep 变体构建 + KUnit**: 配置本已是 lockdep 变体（5a-勘误1, 无需再 -e）→
  `make -j8` 重链 **#17**（17m49s, 零新增警告, 仅既有 objtool cpuidle_enter_state 与
  modpost memblock_end_of_DRAM 两条）→ 无盘 qemu KUnit `filter_glob=corten*` on×1
  （timeout 900 未触, 实测 ~32s, cmd: `console=ttyS0 panic=-1 corten=on
  kunit.filter_glob=corten*`）:
  ```
  # corten:       pass:24 fail:0 skip:1 total:25
  # corten_arena: pass:27 fail:0 skip:0 total:27
  # corten_fault: pass:24 fail:0 skip:0 total:24
  ```
  **lockdep splat = 0**（BUG:/lockdep:/possible deadlock/DEBUG_LOCKS_WARN 全零）;
  3 条 WARNING（corten.c:864/811, vmstat.c:397）与两轮绿色基线
  kunit-on{1,2}-rebase.log 逐条相同（测试路径既有噪声, 非新增）。
  （build-lockdep-r2.log / kunit-on1-lockdep-r2.log）
  **恢复普通配置重建**: 清除 lockdep 全家族 8 项（5a-勘误1）→ olddefconfig（对主树
  config 残余差异只剩 worktree 基线既有的 GKI 组缺项, 无反向新增）→ `make -j8` →
  **bzImage #18**（01:09, 14,771,200 字节 —— 与 16:29 干净 bzImage-m5t1a-y 同字节数）,
  零新增警告。config 快照: config-y-plain。（build-y-plain-restore.log）
- [x] **fork_roundtrip 1000 轮**（guest #16 lockdep 内核, corten=on; 判据: fork→子
  全 arena 校验→子写→exit→父校验+父重触 COW 全程零错; 二进制 = bench/arena-stress/
  fork_roundtrip.c 22:46 版, gcc -static -O2）:
  ```
  round 100..1000 done（每 100 轮一行, MemFree 稳定于 3.47–3.49 GB, 无泄漏）
  [corten] PASS: 1000 rounds x 1k pages, checksum 076534f9ba241483 stable, parent/child isolated
  rc=0
  ```
  一次通过, 无重试。（roundtrip-1000-r2.log）
- [x] **lockdep 下 arena_stress churn + fork 压测**（M5_FORK_SPEC §2.1 判据 3 前半;
  口径注明见下）:
  - `arena_stress 8 15 512 4 --mode churn --verify`（lockdep 内核, 8 线程 15s 一轮）:
    ```
    {"bench":"arena_stress","threads":8,"seconds":15,"arena_mb":512,"mode":"churn",
     "variant":"fixed","seed":4,"ops":9203,"checksum":"03d9eda075e9cb32",
     "errors":0,"op_errors":0,"arena_ok":true,"mem_ok":true,"wall_s":15.035}  rc=0
    ```
  - `fork_roundtrip 100`（corten 臂）: `[corten] PASS: 100 rounds x 1k pages,
    checksum 076534f9ba241483 ...` rc=0（与 1000 轮同校验和）。（roundtrip-100-r2.log）
  - **零 lockdep splat**: 本 boot 基线 0 → 全部压测后仍 **0**（含前班未留痕的
    1100 次 fork）; 对账: `fork_faithful 1100→2200`（+1000+100, 与有 arena 的 fork
    次数精确一致）, `fork_demotes=0 / fork_skips=0 / drain_timeout=0`（收口干净）。
  - **口径**: 判据 3 字面形状（8 线程持续 fault 与主线程 fork() 循环 1k 次**并发**）
    留 T1b 全形状; 本班证据 = 8 线程 churn 15s 一轮（零错）+ fork_roundtrip 100 轮
    （每轮 fork 时 1k 页 arena 活跃, 近似"fork 1k 次"）, 且同 boot lockdep 内核累计
    **2200 次有 arena fork 零 splat**（≥1k 次 = 前班 1100 + 本班 1100, 含一次性
    全部对账）。（churn-8t15s-r2.log / r2-baseline.txt / r2-final.txt）
- [x] **（原列"若时间余量"）fork_roundtrip legacy 臂 200 轮**:
  ```
  [legacy] PASS: 200 rounds x 1k pages, checksum 076534f9ba241483 stable, parent/child isolated
  rc=0
  ```
  跨臂同校验和（内容等价）。（roundtrip-legacy200-r2.log）

**四项全过 → M5T1A_VERIFY_DONE 已 touch（/home/ppw/cortenmm/results/r06/）。**

## 6. 遗留 / 下一班

1. **M5.T1b**（SPEC §2.2）: fork_arena_test v2 文件重写（本班的
   fork_roundtrip.c 即其核心形状, 已放 bench/arena-stress/）、debugfs 计数收口、
   kselftests/mm 8 件套、INV7 checker SHARED⇒PTE-RO 豁免条目。
2. **spec §2.1 判据 3**（8 线程 arena_stress + fork 1k 次 lockdep 零 splat）:
   T1b 判据, 待 lockdep 构建就绪后可提前跑。（夜窗更新: 前半已有本班证据——churn 15s
   零错 + 同 boot lockdep 累计 2200 次有 arena fork 零 splat, 口径见 §5; **并发全形状**
   仍留 T1b）
3. tier-2 WARN 残留（主树级）: rogue-fix.md §6-2 建议改 lockless 变体或压 assert,
   未在本片范围。
4. metis_eq MODE 全量 28.4s vs legacy 19.6s（+45%）: 单值非 ABAB 中位, 不作 G5
   判定; T4 lat_proc/JVM 矩阵按 EVAL §2 出正式数。

---

## 7. 终态补跑（2026-09-19 02:0x–02:5x, maintainer 收口班; 详见 m5t1a-final-verify.md）

评审 PASS-with-conditions 要求的终态证据链已补齐: 23:03 folio 所有权修复之后
零源码改动（git status/diff --stat +2003/-439 + mtime 核实）, 全部判据在普通
配置 bzImage #18 上复出——

- KUnit on×1 + off×1（无盘 qemu）: on 24/0/1 + 27/0/0 + 24/0/0, off 25/0/0 +
  18/0/9s + 4/0/20s, 双轮零 not ok 零 splat（取代 23:09 #16 的 flake 轮为权威）。
- guest 重启进 #18: fork_isolation 双臂 PASS（e1c81840e4123903, 与本报告 §3c
  同值）; metis_eq MODE 全量 rc=0（8a8db99075665220, 三方同值）+ fork-probe OK
  （OQ-D 闭环落在终态代码）; **fork_roundtrip 1000 轮 PASS**（076534f9ba241483,
  MemFree 平稳——23:03 COW-reuse 泄漏修法闭合证明, 上轮 OOM 即死于此路径）;
  JThreadBench 2000×3 rc=0 零 CFE + fork-probe OK; run_mode_smoke 26/26 +
  SMOKE-DRIVER PASS。对账 fork_faithful 0→1005, demotes/skips/drain 全 0。
- §3c 前班原始日志缺档由 guest-final/ 目录回填（终态重跑的原始输出）。
- 时延注记: 本轮 JThreadBench/metis_eq 与 syzkaller M7 预热同宿受载, 判据不涉
  时延。判定: **终态全绿, T1a 具备合入条件**（commit corten-r07-m5t1a）。
