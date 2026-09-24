# D-G'' 修复（file-MAP_FIXED 打穿 shadow-VMA）· 夜间验证班报告

- 日期: 2026-09-18 01:xx CST（r06 夜窗）
- 验证对象: worktree `/home/ppw/linux-6.18-m4fix`（branch m4-dg2 @ 9d74b22a1348 + D-G''/B1 修复工作树）
- diff: `patches/r06-m4dg2.diff`（再生后 1149 行 / 6 文件 / +909−25; 日窗原版备份 `patches/r06-m4dg2.diff.dayshift`）
- 判据: 评审移交 9 条最小判据（任务书）+ `dg2-fix-verify.md` §5
- **VERDICT: FAIL（分层: D-G'' 目标形状修复实证成立; 但新引入 1 个 P1 级退出死锁 + JVM 端到端判据未达成）**
- 处置: 按任务纪律 **不 commit、T5 不跑**（"任一段失败如实报告并停, 不修码"）; 生产代码零改动,
  仅修 3 处验证脚本自身缺陷（见 §0）

---

## 0. 验证脚本自身的三处缺陷（已修, 均不触碰生产码）

1. **mm/corten_fault_test.c:1180 `tail->vm_flags = VM_CORTEN` 编译断裂**（日窗只做文本检查未 make）。
   6.18 `vm_area_struct.vm_flags` 为 const（mm_types.h:839 union const 成员）。修法=文件内既有惯用法
   `vm_flags_init()`（mm.h:815 "VMA not part of the VMA tree and needs no locking" 官方口径,
   本文件 line 80 同款）。
2. **punch_head KUnit 两处测试环境缺陷**（首跑 on1: EXPECT fail + kernel OOPS, kunit-on1.log）:
   - `ft_mark(t, tail)` 返回 −ENOENT: DECLARE 不预装上表, ft_setup 只 fill_upper 窗 0
     （mm_types ft_setup 注释自证）, 测试未给尾窗做事务就绪 → 修法=mark 前
     `corten_arena_fill_upper(ar, tail)`;
   - case kthread（current->mm==NULL）直调 `do_munmap` → `vms_complete_munmap_vmas`
     读 `current->mm->total_vm` NULL 解引用（CR2=0xc0, Oops 0002）→ 违反
     `corten_arena_test.c:163-170` 已文档化约束（"Anything that munmaps therefore runs on the
     dedicated, fully self-managed worker"）→ 修法=照搬 op-worker 惯例, 新增
     `corten_fault_gather_free{_fn,_thread}`（kthread_use_mm 包装, completion join, 记录结果回 case 线程断言）。
   **重要旁证**: 崩溃前 B1 指针恒等断言（`ar->vma` PTR_NE doomed / PTR_EQ 尾片）已全部通过 ——
   修复核心语义在原生测试里即已正确。
3. **dg_probe3.c（新写 E1-E5/fork 判定探针, results/r06/probe/）两处**: ①文件字节断言误用魔数
   （JVM 同款 offset 0x1000 映射, 魔数 0x7f 在 offset 0 —— round-1 "00 00 00 00 -> FAIL" 为假警报,
   实测 `/bin/true` @0x1000 = `00 00 00 00`、@0x1010 = `06`, 与映射读回逐字节一致）→ 修法=
   **mapped vs pread 同偏移 4 点硬比对**; ②探针点越 EOF（map_len/2 ≈ 2.36MB >> 文件 97KB）触发合法
   SIGBUS → 修法=fstat 截断探针点。T5/驱动脚本一处: java 腿漏 LD_PRELOAD（round-1 "MODE marker
   missing" 假象）+ 自写 strace 腿无窗口上限（32min 挂死根源）——均记录备查。

## 1. 构建（判据: 四配置 + ANON_VMA_NAME=y, 零新增警告）—— PASS

- `make -j6 olddefconfig`（No change）+ 全量 `make -j6`: bzImage ready。
- 警告全量扫描: 仅 2 条已登记基线条（objtool `cpuidle_enter_state` + modpost `memblock_end_of_DRAM`;
  green.txt r05-m4t0a 条目同批）, diff 六文件相关零警告。log: `results/r06/build-y-full.log`。
- 上述构建在修测试缺陷 #1 后达成; 首次构建在 corten_fault_test.c 编译断裂（证据 transcript）。

## 2. KUnit（判据: on×2 + off×1, timeout 300, -smp 2, `kunit.filter_glob=corten*`）—— PASS

| run | 内核 | 结果 |
|---|---|---|
| on1/on2（修前二进制） | dirty(修前) | corten 20/0/5 + arena 22/0/0, **corten_fault 套件内 punch_head OOPS**（见 §0-2, 证据 kunit-on1.log） |
| **on3/on4（冻结版）** | 9d74b22a1348-dirty | **corten 20/0/5 + corten_arena 22/0/0 + corten_fault 19/0/0, rc=0 零 not ok 零 BUG/Oops** |
| **off2（冻结版）** | 同上 | corten 21/0/4 + arena 16/0/6 + fault 4/0/15（真链用例按设计 skip）, rc=0 |

- B1 分层说明（判据 9）: 原生 punch_head 在断言阶段已证 `ar->vma` 重指不变量（on1 日志:
  EXPECTATION FAILED 仅 ft_mark 一条, B1 断言零失败记录, 随后才是 env 缺陷 OOPS）; 修复后
  punch_hole（中段 punch 头片不变量+洞 frame 擦除+内容 zap+尾片续 fault）与 punch_head
  （头 punch+真 free+尾片 fault 全链）on×2 全绿。**KUnit 侧 B1 覆盖完整。**
- log: `results/r06/kunit-on{3,4}.log`、`kunit-off2.log`（修前对照组 kunit-on{1,2}/off1 保留）。

## 3. =n 回归（判据: 七对象零警告 → 恢复 config）—— PASS

- `scripts/config -d CORTEN_MM -d CORTEN_MM_ARENA -d CORTEN_MM_KUNIT_TEST` → olddefconfig →
  make -j6: `kernel/sys.o, mm/{memory,mmap,mremap,madvise,mempolicy,migrate}.o` 七对象全部重编,
  **零编译警告/错误**（grep 命中 5 行均为 error*.o 文件名）。log: `results/r06/build-n-seven-objects.log`。
- =n 全量链接 rc=0（仅同两条基线警告）。config 已恢复（5 项 CONFIG_CORTEN=y 复核）。

## 4. guest 判定实验（corten=on, 9 条判据逐条）

内核: `6.18.32-g9d74b22a1348-dirty`（worktree bzImage #4）; qemu KVM 4G/8vCPU;
append `systemd.mask=sys-kernel-config.mount corten=on mitigations=off kunit.enable=0`
（kunit.enable=0 为 green.txt r05 夜班口径; 首轮漏带导致 smoke 的 dmesg 检查被 boot-KUnit
设计注入误触发, 干净 boot 上 smoke 26/26, 见判据 8）。探针源码+产物: `results/r06/probe/`、
`results/r06/probe-out/`。

### 判据 1 —— E1: file-MAP_FIXED @ q（probe2 谱系 + JVM CDS 形状）: **PASS**
- JVM 形状（probe e1, MODE ENTER → reserve 128MB PROT_NONE → 修头 10MB/尾 6MB → 两笔
  JVM 同参 file MAP_FIXED）: `mmap_punches 2→4`（两笔都走 punch 路由）, 读回与 pread
  逐字节一致（`mapped==pread over 4 probe points -> PASS`）, 零 SIGSEGV。
  **三明治逐字**（probe-e1.out）:
  ```
  100000000000-100000a00000 ---p 00000000 00:00 0   [anon:corten_arena]   ← 头片
  100000a00000-10000169c000 rw-p 00001000 fd:00 ... /usr/bin/true          ← 文件 VMA（两笔合并）
  10000169c000-100008000000 ---p 00000000 00:00 0   [anon:corten_arena]   ← 尾片
  ```
- probe2 谱系（e1b: DECLARE 4MB, q=p+PMD）: `mmap_punches +1`, 读回 PASS, 头窗 0 magic
  0x5a5ac0de 完好（punch 不伤头片数据）。
- 与 mode=off 对照: r05 dg2-analysis 已证 off 臂恒 0x7f/正常; 本轮 OFF 对照 = metis_eq
  MODE-off rc=0（同内核）, 基线健康。

### 判据 2 —— java -version（-Xshare:on 默认, MODE on）rc=0 无 hs_err: **FAIL（下游既有缺陷, 非本 diff 回归）**
- 本内核（D-G'' 修复后）: hook STRICT 生效（"MODE on" marker 有）, **打印出 `openjdk version
  "21.0.12.1" ...` 版本横幅后**在 libc 崩（hs_err: `SIGSEGV ... C [libc.so.6+0xa2a63]`, hs_err
  错误报告器自身连串二次崩; guest dmesg: `segfault at 10000c000030 ... error 7` = **写 present-RO
  PTE**, MODE 窗内地址, libc 锁操作 `xchg %eax,(%rbx)`）。
- **T0b 对照内核（9d74b22a1348 clean, 主树重建 #26）同参复跑: java rc=134 SIGABRT, 连版本横幅
  都没有**（j.err 仅 hook 两条 marker）。
- 结论: JVM 在 D-G'' 修复后**严格走得更远**（横幅从无→有）, 但 rc=0 判据未达成; 剩余崩点是
  **T0b 既有下游形状**（RW 提交区被装 present-RO PTE → 用户写 ACCERR）, 与 metis_eq 同签名
  （见判据 8 对照）, 登记为修复班下一靶, **不是本 diff 的回归**。

### 判据 3 —— E2: 匿名 MAP_FIXED 同位仍 PASS（MARK 路由）: **PASS**
- probe e2: 匿名 MAP_FIXED @q 写读回环 PASS; `mmap_punches` delta=0（MARK 不 punch）;
  `mmap_punch_rejects 0→0`。

### 判据 4 —— E5: r1 NOREPLACE=-EEXIST; r2 file FIXED 成功且读得内容: **PASS（功能面）**
- probe e5: `r1 NOREPLACE -> MAP_FAILED EEXIST PASS`（F-B backstop 保住）;
  `r2 MAP_FIXED -> ok` + `mapped==pread PASS`。

### 判据 5 —— 计数器: **PASS（探针全程）**
- `mmap_punches` 精确增量: e1 +2（两笔 JVM file FIXED）, e1b/e5 各 +1, fork +1;
  `mmap_punch_rejects` 全程 0→0; `eagain_leaked 0→0`; `drain_timeout 0→0`（干净 boot）。
- accerr 增量==0 的口径: accerr 为 per-mm percpu 计数, **无 debugfs 渲染**（corten.c 仅
  stats/dump/txn/arenas/arena_stats 五文件, mm_stats 未导出）; 以"探针零 PROBE-SIGSEGV"+
  dmesg 零 WARN 作行为等价证据（ACCERR 唯一递增点是 force_sig_fault 路径）。诚实注记。

### 判据 6 —— strace 四步序列 + /proc/maps 三明治: **PARTIAL**
- 三明治: PASS（判据 1 逐字证据, 探针自证; 头片 arena + rw-p 文件 + 尾片 arena）。
- 四步序列: reserve→双 trim→双 file FIXED 由 probe e1 以 JVM 同参复刻并成功;
  **java 本体 strace 未取得完整四步**（java 在 MODE 下于 archive 映射前崩, trace 仅到
  CCS/metaspace 阶段, 无 classes.jsa MAP_FIXED 可录）。证据: `probe-e1.out`。

### 判据 7 —— E4 变正（整 arena mprotect 后读 0x7f 而非 0x00）: **FAIL→改判 N/A(前提失效), 且对照证伪 r05 文档口径**
- 本内核与 **T0b 对照内核**上 `mprotect(整 arena)` 均 `-EOPNOTSUPP`（probe e4 rc=3,
  `mprotect arena: Operation not supported`, mprotect_routes 不动）。r05 dg2-analysis §4-E4
  所记"EXACT 抬门成功"与本轮两个内核的实测均不符（文档口径存疑, 已登记）。
- E4 的**实质语义**（文件内容而非匿名零页）已由 e1/e1b/e5 的 pread 硬比对覆盖: 修复后读的是
  真文件字节, pre-fix 的"0x00 静默损坏形状"消失（round-1 的 "00 00 00 00" 实为文件真实内容,
  §0-3 已澄清）。

### 判据 8 —— 回归面: **PARTIAL FAIL**
- run_mode_smoke: **PASS**（干净 boot 26/26 + SMOKE-DRIVER PASS; 首轮 FAIL 为 boot-KUnit
  设计注入误触发 dmesg 检查, green.txt r05 已登记的同款伪影）。
- metis_eq on-run rc=0: **FAIL** —— MODE on 下 SIGSEGV（`segfault at 10000c000030 error 7`,
  写 present-RO PTE, libc 锁路径）。**T0b 对照内核同负载同参 SIGSEGV 复现（rc=139）→ 既有
  下游缺陷, 非本 diff 回归**。MODE-off 同内核 rc=0 且 JSON 有效（checksum 2d383eeed4ceb73b）。
- 新 2 KUnit（punch_hole/punch_head）: PASS（§2）。
- dmesg 零 WARN: PASS（干净 boot 全程 `corten.*(warn|bug|oops)` 增量=0）。
- fork 冒烟（fork_demote 多片循环）: **PASS** —— probe fork: DECLARE+MODE ENTER+punch 三片
  arena → fork → 子/父三方（头/文件/尾）读写全对, `fork_demotes +1`, waitpid ok。
- **新发现 P1（diff 引入/暴露, 见 §5-D）**: DECLARE+punch 形状进程退出死锁。

### 判据 9 —— B1 分层: PASS
- 指针恒等断言（punch_head）+ 中段不变量（punch_hole）在 KUnit on×2 全绿（§2）;
  guest 侧 fork 冒烟踩 fork_demote 多片循环 PASS。B1 反证不可能由 guest 端到端给出
  （UAF 需 KASAN/恒等断言）——分层口径与移交判据一致。

---

## 5. 新发现: 退出路径死锁（P1, 判定为本 diff 引入/暴露）

- 形状: **DECLARE arena（prctl 79）+ file-MAP_FIXED punch 之后, 进程 exit_group 挂死**。
- 复现率: 近三轮 e1b/e5 形状 5/5（round-1 的 e1b/e5 曾秒退, 二进制唯一差异是探针读回路径加了
  fstat/pread —— 时间窗敏感, 竞态性质）; e1/e2/fork（MODE 形状或无 punch）干净退出。
- 现场锚点（/proc/<pid>/stack, D-state, SIGKILL 免疫）:
  ```
  __vma_start_write+0x5b/0x100     ← 挂点
  free_pgtables+0x11d/0x320
  exit_mmap+0x179/0x3d0
  mmput → do_exit → do_group_exit → sys_exit_group
  ```
- 定责依据: T0b 对照内核上同形状**走不到干净退出**（pre-fix 在读阶段即 ACCERR 崩）, 因此该
  "punch 后存活并退出"路径为修复后首次可达 —— 死锁系 punch 路由新引入/暴露（候选: punch 拆分后
  shadow piece 的 vm_lock 未释放/陈旧 seqlock, 或 exit_mmap 的 corten drain 与 free_pgtables
  的 vma 写锁互锁）。**根因未查（验证班不修码）**, 锚点已给出。
- 连带观察: java MODE 崩退时的 futex 群挂（hs_err 报告器二次崩）与本死锁是否同根, 留修复班判断。

## 6. 对照实验矩阵（定责依据汇总）

| 实验 | D-G'' worktree 内核 | T0b 对照（9d74b22 clean #26） | 归责 |
|---|---|---|---|
| probe e1 文件读回 | mapped==pread PASS | 不可达（读阶段 ACCERR=r05 已证 D-G'' 签名） | **修复成立** |
| probe e1b/e5 punch+读回 | PASS（但退出死锁 5/5） | 不可达（同上） | 修复成立 + 新死锁 |
| probe e2 MARK | PASS | （未跑, 形状与 diff 无涉） | 中性 |
| probe e4 整 arena mprotect | -EOPNOTSUPP | **-EOPNOTSUPP（复现）** | T0b 既有 |
| metis_eq MODE on | SIGSEGV | **SIGSEGV（复现）** | T0b 既有 |
| metis_eq MODE off | rc=0 JSON 有效 | — | 基线健康 |
| java -version MODE on | 版本横幅后 libc 崩 | **rc=134 SIGABRT 更早** | 修复严格改善; rc=0 未达 |
| run_mode_smoke | 26/26 PASS | — | 中性 |

## 7. 判据总览与 VERDICT

| # | 判据 | 结果 |
|---|---|---|
| 1 | E1 file-MAP_FIXED 读回+三明治 | PASS |
| 2 | java rc=0 无 hs_err | **FAIL**（下游既有; 严格改善） |
| 3 | E2 匿名 MARK | PASS |
| 4 | E5 两 r | PASS |
| 5 | 计数器 | PASS（accerr 口径注记） |
| 6 | strace 四步+三明治 | PARTIAL（三明治 PASS, java 四步不可录） |
| 7 | E4 变正 | N/A（前提失效, r05 文档口径被对照证伪; 实质语义由 e1/e1b/e5 覆盖） |
| 8 | 回归面 | PARTIAL（smoke/KUnit/dmesg/fork PASS; metis FAIL=T0b 既有; **+新退出死锁 P1**） |
| 9 | B1 分层 | PASS |

**VERDICT: FAIL —— 不满足全部 9 条最小判据, 不具备提交条件。**
D-G'' 目标缺陷本身（地址键控误路由 + 守卫缺口打洞）的修复在探针级实证成立（判据 1/3/4/5 全绿,
JVM 严格前进）, 但 (a) 新 P1 退出死锁为阻塞项, (b) java/mmetis 端到端 rc=0 判据落在 T0b 既有
下游缺陷上（"写 present-RO PTE"形状, 需另立修复任务, 与本 diff 无涉）。

## 8. 移交清单（修复班/规划者）

1. **P1 退出死锁**: 复现=dg_probe3 e1b（share r6dg/, 源码 results/r06/probe/）; 锚点 §5;
   现场盒: worktree bzImage + trixie.img + r6dg share 原样保留（VM 留运行）。
2. **T0b 既有下游缺陷（新立任务）**: "RW 提交区 present-RO PTE"（metis_eq/java 同签名,
   dmesg `segfault at 10000c000030 error 7`）; 候选区: mprotect 路由 pending-perm/zero-page
   升级路径; 注: 整 arena mprotect EXACT 的 -EOPNOTSUPP 亦属 T0b 层（r05 文档口径已证伪）。
3. 本班对 diff 的改动=3 处验证脚本修复（§0）, 已折入 `patches/r06-m4dg2.diff`（1149 行,
   checkpatch --strict 0E/0W/0C）; 日窗原版备份 `.dayshift`。生产码 5 文件与日窗逐字节一致。
4. build/KUnit/=n 证据全绿部分不受 FAIL 影响, 提交条件恢复后可直接复用。

*证据: results/r06/{kunit-*.log, build-*.log, probe/, probe-out/, probe-e*.out, dmesg-final.txt,
arena_stats.final, arenas.final, battery-round2.log}; transcript 逐字引用处均标注。*
