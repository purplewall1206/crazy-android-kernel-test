# M-V V-E 开发报告：brk 委托裁决落章 + 白名单 live 断言 + brk 计数 + 终判据电池（M-V 系列终章片）

基座：worktree `linux-6.18-mvb`（分支 mv-b，ff 到 309674279b1d = V-D）。
本片不改任何行为面（观察/审计/文档 + guest 电池件），红线全守：INV6 ✓
（新代码全部只读走查，零 PTE 写点，无第 4 白名单写点）；=n 折叠 ✓
（`corten_brk_note`/`corten_j1_probe` 的 =n inline 折叠，sys_brk 四处
一行调用在 =n 下编译为空）；未 commit ✓。

## 1. LoC 终态账增量素材（REPORT M-V 章用）

git numstat（对 HEAD 309674279b1d）：

| 文件 | +/− | 内容 |
|---|---|---|
| mm/mmap.c | +9/−0 | sys_brk 四臂 note（一行一臂 + 一段注释） |
| mm/corten_arena.h | +86/−2 | 白名单 enum/API、brk note inline（=y 实体 + =n 折叠）、J1 探针 heap 臂 |
| include/linux/corten_arena.h | +35/−0 | 共享 `enum corten_brk_arm`（两 Kconfig 面都要命名）+ KUnit 锚声明 |
| mm/corten_arena.c | +347/−3 | 11 计数器、heap note、brk note_slow、白名单纯分类器 + 全树 scan + 三入口 + pid 后端 + audit_gate 扩展 + arena_stats 五行 + KUnit 锚块（~69 行） |
| mm/corten.c | +42/−0 | debugfs `whitelist`（写 "<pid>"，j2_walk 同型） |
| mm/corten_arena_test.c | +220/−0 | 2 个 KUnit 锚（brk_delegation / whitelist_audit） |
| **合计** | **+739/−5** | 生产 ~+470（其中注释占近半；纯代码 ~+310，KUnit 锚块另 ~60）；测试 +220 |

- spec 预估 ~+150 是"断言/计数/文档"最小核；实增量主要花在白名单分类器
  的**九类枚举 + 全树走查 + 双入口锁形 + pid 触发后端**（J2 完整形态不可
  再小的骨架）与 house 注释密度。数字如实入账。
- 电池件（guest 侧，非内核 LoC）：`bench/share/mve-battery/` 五件
  （mve_battery.sh 310 行 / mve_workload.c 340 行含静态产物 /
  mve_heap_trace.bt / mve_s3_swapoff.sh / README 判据容差表）。

## 2. 各交付件设计

### 2.1 brk 委托裁决（OQ-MV-7 测量落地 → V-E.1 落章）

**测量面（三件套，J1 的 (a)/(b) 双口径同型）**：
1. **内核 brk 四臂计数**（`sys_brk` 内 `corten_brk_note()`，双门
   `corten_enabled_static() && mm->corten_mode`，=n/=off/非 MODE 一次折叠
   分支）：GROW（do_brk_flags 安装）/ SHRINK（do_vmi_align_munmap 修剪）/
   NOOP（页对齐 break 不变）/ REJECT（out: 尾——min_brk/rlimit/guard/对齐
   全部拒绝形状共享）。debugfs `arena_stats` 五行：`brk_grow/shrink/noop/
   reject/heap_lookups`。
2. **heap 域 find_vma 计数**（OQ-MV-7 分子）：`corten_j1_probe()` inline
   在窗口判定不中后追加 heap 臂——`start ∈ [start_brk, brk)` 即
   `corten_j1_heap_note()`。五个探针挂点（find_vma/find_vma_intersection/
   find_vma_prev/lock_vma_under_rcu/uffd_lock_vma）共享同一 inline，故障
   路径（lock_vma_under_rcu）自动覆盖——正是 heap fault 主项。观测不优化
   （spec §3.5 原话），无行为挂靠。
3. **bpftrace 分母**（`mve_heap_trace.bt`）：kprobe find_vma +
   lock_vma_under_rcu，`mm->corten_mode==1` 过滤，三桶计数
   （window/heap/other）+ 非 MODE 基线桶。裁决份额 = heap/mode_total。

**KUnit 侧已测数**（corten=on boot，`corten_arena_test_brk_delegation`）：
- 双门：非 MODE mm 上 note 零计数；MODE mm 上四臂各计各的、零串计；
- heap 探针：MODE mm 的 heap 域 find_vma ×2 → `heap_lookups` +2、
  `j1_probes` 零扰动；heap 下界之下不计数。
- **份额数字本身是 guest 量**（需要真实 brk/mmbench 流量），主会话跑
  `mve_battery.sh brk` 回填 REPORT（槽位见 §4）。

**裁决结论（按判据写，预期+依据）**：委托域维持（V-E.1），region 化不
立项（OQ-MV-7 关闭）。依据三条（spec §3.5 论证 + 本片测量面）：
(a) ABI 约束不可绕（sbrk(0)/MORECORE 连续性、M_TRIM_THRESHOLD 原址
shrink）；(b) 账目/消费面零回归（RLIMIT_DATA、`[heap]` 注记、
VM_GROWSUP 扩展语义全部现役）；(c) 收益面受"目标 workload 走 mmap 窗"
前提保护——电池阈值：<5%（或全电池 brk 流量为零）维持；5–25% 登记
复测；>25% 且目标 workload 复测仍高才立项 V-E.2（do_brk_flags 路由化
~400 行，kernel 侧 brk→region 翻译，glibc 无关）。**guest 数字回来后
按此表落最终一句**；若实测越线，裁决翻面、V-E.2 立项——以数据说话。

**vm_brk_flags 不挂钩**（登记）：内核内部 brk（binfmt 等）非用户流量，
OQ 度量对象是 sys_brk；KUnit 无法直调 sys_brk（`__do_sys_brk` 为文件
局部符号，syscall 包装器形态），四臂调用点是单行 note + 电池
`brk_grow>0 ∧ shrink>0 ∧ noop>0 ∧ reject>0` 接线证明（guest 判据）。

### 2.2 白名单 live 断言（J2 完整形态）

**分类器**（`corten_whitelist_classify`，纯函数）：树上每个 VMA 归一桶——
窗口域：SHADOW（VM_CORTEN 靶向影子/幸存片）∨ IMPLANT（植入登记内）∨
**VIOLATION**（即 INV-MV2 谓词，全树版）；委托域：BRK（`[start_brk,
PAGE_ALIGN(brk))` 内无文件 VMA——spec §3.5 "brk VMA 显式登记"的落点）∨
STACK（VM_GROWSUP/GROWSDOWN）∨ SPECIAL（arch_vma_name：vdso/vvar/
vsyscall）∨ FILE（vm_file：exec 期映射与 MAP_SHARED 同桶）∨ ANON ∨
UNCLASSIFIED（披露桶，非判罚——VMA 层合法服务窗外一切）。

**走查器**：`corten_whitelist_scan` 全树一遍（RCU maple 走查 + 稳定
implants 镜像，j2 scan 同锁形）；`_walk`（自足型，ctl_lock）/`_walk_locked`
（mmap_write/mm_users==0 契约同 j2）；**live 挂点两处**——每个 MODE mm
exit 时自动（mm_exit 头部、j2 walk 旁）+ debugfs `whitelist`（写
"<pid>"，j2_walk 同型）。返回 = 窗口违例数；heap VMA >1 记
`wl_brk_anomalies`（sys_brk 产不出分裂堆，正计数=分类气味，不 WARN）。

**账目**：`wl_walks / wl_violations / wl_brk_vmas / wl_delegated_vmas /
wl_unclassified / wl_brk_anomalies` 六行入 `audit_gate`；
**gate_pass 扩展**为 `!j1_hits ∧ !j2_viol ∧ !wl_viol ∧ !wl_anom`。
两走查器断言同一窗口谓词 → 电池交叉断言累计 `wl_violations ==
j2_violations`（不等=一条看到了另一条没看到的形状，先分类）。

### 2.3 终判据电池（bench/share/mve-battery/，主会话跑）

一脚本 `mve_battery.sh [j1][j2][j3][j4][brk][all]`，判据/容差全表在
`README.md`（摘要见 §4）。S-3 两分支独立成 `mve_s3_swapoff.sh`（V-D
报告 §6 判据逐条落码：分支 A 读回后 swapoff 干净成功；分支 B 有界自旋
+SIGINT 可中断+持有者 exit 后 inuse_pages 秒级归零+重试成功；
`unuse_blind_mms/zap_swap_frees/swapins` 披露齐）。零改动回归集
（smoke 26/26、JTB 3×3、metis/dedup/psearchy checksum、lmbench）属主
会话标准门，README 列清单不复述。

## 3. KUnit 锚（2 新例，corten_arena 97→99）

- `corten_arena_test_brk_delegation`：双门沉默性、四臂独立计数、heap
  探针命中/不越界、j1 对零扰动。
- `corten_arena_test_whitelist_audit`：分裂堆（2×BRK + 恰 1 anomaly）、
  栈/影子/植入/匿名分桶精确断言（histogram 锚）、外来窗口 VMA 恰 1
  违例 + 登记自证清零（INV-MV2 inject 同契约）、gate 渲染携带 wl 行
  与扩展判定。**表尾注册**（gate_pass 累计量在注入后归零，必须在所有
  gate_pass==1 断言之后跑）。
- KUnit 未覆盖（登记）：SPECIAL/FILE 桶（合成 VMA 无真 vm_file/arch
  名，guest whitelist 触发真进程覆盖）；sys_brk 调用点本身（局部符号，
  见 §2.1）。

## 4. guest 终判据电池判据表（V-E 收口 = 系列 DoD）

| 判据 | 电池腿 | 判据（容差） |
|---|---|---|
| J1 | j1 | (a) `j1_probes`/`j1_hits` 纯 MODE 负载 delta==**0**（严格，A.3d 口径维持）；(b) bpftrace window 桶==**0**。无容差 |
| J2 | j2 | 活体 `whitelist <pid>` rc=0；`wl_violations`/`wl_brk_anomalies` delta==**0**；`wl_walks` 前进；交叉 `wl_violations==j2_violations`；组成三行披露非判据 |
| J3 | j3 | in-boot V-C oracle PASS；`BASE_SNAP` 给定时 cmp_j3.sh：maps/procmap 逐字节、pagemap flag 位、smaps 块头零差，V-C 登记桶按预算容差。**A.1 基线快照需重拍**（`bzimg/r07-mva1/` boot 后同 oracle 脚本拍一次；V-C 当时快照未持久化） |
| J4 | j4 | `mve_workload j4` 6/6 PASS rc=0（活窗读/PROT_NONE 拒/parked 拒/堆读/写往返）；`gup_probes>0` |
| OQ-MV-7 | brk | 四臂 delta 全>0（接线证明）+ heap 份额三档阈值（§2.1）+ 内核/bpftrace 双口径同量级 |
| S-3 | s3 脚本 | 两分支判据（§2.3）；D 态挂死或条目滞留=FAIL 升级 |
| 回归 | 主会话 | V-D 绿逐字复现（本片零行为面） |

**REPORT M-V 章回填槽位**：heap 份额 %（bpftrace 口径）/ 内核
heap_lookups 差分 / mmbench 动态腿读数 / j2 组成三行 / S-3 两分支结果。

## 5. 验证记录（本片已跑）

- `make -j8` 全量构建 ✓（#40/#41）；
- KUnit 三套件：plain 复跑 **corten 25/0/0 + corten_arena 23/0/76
  (skip=on 门控, 0 fail) + corten_fault 7/0/26**（首跑
  corten_test_txn_uninstall_interlock 时序 flake 1 例，M7 口径复跑绿，
  与本片无关——本片未触 txn 代码）；**corten=on：corten 24/0/1 +
  corten_arena 99/0/0（97+2 新锚）+ corten_fault 31/0/2**，与 V-D 基线
  逐位同 + 新锚全绿（白名单注入的 WARN_ONCE 为设计路径，与
  inv_mv2_inject 同形）；
- lockdep 变体（PROVE_LOCKING）构建+三套件：见下方补记；
- checkpatch --strict 全 diff：**0E/0W/0C**（887 行，
  `patches/r07-mve.diff`）；
- =n 折叠：corten_brk_note/corten_j1_probe inline 折叠路径编译 ✓
  （=n 对象集验证见补记）；
- 电池件自测：mve_workload 静态构建 0 警告；brk 循环数学在宿主内核
  200 cycles 验证（四臂 + break 连续性 + heap 数据存活 + raw brk(0x1000)
  拒绝路径——glibc sbrk 大负增量会吞拒绝，拒绝臂必须 raw syscall，已
  按此实现）；两脚本 bash -n 通过；bpftrace 件语法按整数 map 单元写
  （避免聚合 printf 兼容坑），guest 实测留主会话。

## 6. 遗留与分级建议（移交主会话）

**P2（收口动作，主会话本周期）**：
1. guest 电池全套（J1–J4 + brk + S-3 + 回归集）→ 回填 §4 槽位 →
   REPORT M-V 章定稿 + OQ-MV-7 裁决句落章；
2. green.txt 入册（r07-mve 行，形态同 mvd）。

**P3（登记不立项，V-C 残差维持原分级）**：
- PROCMAP_QUERY or-next 型窗口查询 residual find_vma（覆盖型已免；
  V-C 报告 §7 登记，YAGNI 维持）；
- bpf_iter_task_vma / trace 符号化单树源（纯保真观察面，无错误路径）；
- smaps 窗口行零值桶 / numa_maps 简化（V-C 登记披露维持）。

**P4（条件触发，阈值在电池 README）**：
- OQ-MV-7 越线 → V-E.2 立项（~400 行 do_brk_flags 路由化）；
- S-3 分支 B 挂死/滞留 → unuse region 枚举臂立项（~60 行）；
- OQ-MV-8/9/10/11/12 维持 spec 原分级（本片无新证据）。

**系列终态声明素材**：V-E 收口后 M-V 全系列 13 提交（A.0/A.1/A.2ab/
A.3ad/V-B.1-4/V-C/V-D/V-E），J1–J4 电池就绪，窗口域零 VMA + 委托白名单
封闭（J2 完整形态）+ 白名单 live 断言常驻（exit 自动 + debugfs 手动）。
