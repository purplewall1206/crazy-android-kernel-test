# M-V A.3d 开发报告：S-5 三终答 + J1 豁免收零（A 系列最终出口片）

- 产出: A.3d 开发 agent（2026-09-23）
- worktree: `/home/ppw/linux-6.18-mva` @ 分支 `mv-a0`，基座 = 主树 HEAD **76a83f8e0d03**
  （A.3c 已入库）；本片为未提交增量，**未 commit**（红线遵守）
- 任务书: 任务派发（A.3d 段）+ `next/va3-dev-brief.md` §2.11/§4 A.3d 行 + §5 D 组；
  语义裁决: `next/j2-audit-draft.md` #13/#20/#23/#28 + **D24（STATE）**——mincore 活跃窗
  真值走查（OQ-MV-11 提前交付）/ msync 窗口 0 对齐匿名 no-op / madvise 空窗终答
- 补丁: `/home/ppw/cortenmm/patches/r07-mva3d.diff`（7 文件 **+1067/−12**，1261 diff 行）
- 行号口径: 本文 file:line = **A.3d 增量后的 worktree 实码**

## 1. 改动总览

| 文件 | 增/删 | 内容 |
|---|---|---|
| mm/corten_arena.c | +452/−1 | J1 豁免（probe 双形短路）+ 植入登记锁元快读与 RCU 退休 + msync skip + mincore 真值走查 + madvise parked 终答 + move_pages 短路 + 四计数器/stats 四行 + gate 注释更新 |
| mm/corten_arena.h | +65 | covers_lockless/msync_skip/mincore_route/move_pages_window 的 =y 声明 + =n static inline 折叠 |
| mm/corten_arena_test.c | +497 | KUnit D 组 5 用例 + msync op worker |
| mm/internal.h | +5 | ksys_msync 原型（本树 -Wmissing-prototypes 强制） |
| mm/migrate.c | +11 | #28 do_pages_stat_array 逐页窗口短路 |
| mm/mincore.c | +19 | do_mincore 入口路由 |
| mm/msync.c | +30/−11 | 三处 skip 臂 + SYSCALL 体提取为 ksys_msync |
| **合计** | **+1067/−12** | 内核 ~+570/−12，测试 +497 |

内核增量（~+570）超 brief §4 预算（~+290）的主因与前几片相同：注释密度对齐本屋
惯例；且 J1 豁免的锁元快读安全论证（§2.2 的 mark 退休改造 + 快照协议）是任务书
"读侧无锁安全性与 A.3c walker 同论证"一句背后的**必要实码**——A.3c walker 读登记
表靠 ctl_lock/稳定性契约，probe 的 RCU 上下文（lock_vma_under_rcu 无 mmap 锁）两者
皆无，直接复用 corten_implant_covers 是 use-after-free（krealloc 同步释放旧数组）。
功能面未越 A.3d 界（walker/fault 终答主体零改动）。

## 2. 逐项落点

### 2.1 J1 豁免收零（任务 1，本片核心目标）

- **corten_j1_slow 双形短路**（mm/corten_arena.c:2368）：计数前先查植入登记表，
  两种"契约内合法树查"形态豁免（probes 与 hits 都不计）：
  1. 查询区间∩窗 ⊆ 登记之并（植入片自身的访问——A.3c gate 首测的那 1 命中即此）；
  2. 查到的 VMA∩窗 ⊆ 登记之并（邻接形状：find_vma(空洞/占用帧地址) 返回下一个
     VMA 恰是植入片，同一合法访问的侧面）。
- **corten_implant_covers_lockless**（:9896）：登记表的 RCU 安全查询形——
  (a) 快照协议：先 READ_ONCE(nr_implants)、smp_rmb、再 READ_ONCE(implants)，
  与 mark 的发布序（指针先、条目后、smp_wmb、计数最后）配对，任何交织下快照的
  count 不超过快照数组的分配界（弱序架构证明成立）；(b) 旧数组经 RCU 退休，
  RCU 段内的读者全程有效。撕裂条目读最坏误答一次咨询计数器，永不越界。
- **corten_implant_mark 增长路径改造**（:9746 增长臂）：krealloc（同步 kfree 旧
  数组的 UAF 源）→ kmalloc_array + memcpy + WRITE_ONCE 发布 +
  **kfree_rcu_mightsleep** 退休；插入路径的 nr 更新改 smp_wmb + WRITE_ONCE
  （发布序的写侧一半）。merge 路径的收缩/原地改不需要屏障（安全界只依赖
  alloc 不缩）。
- **gate_pass 语义闭环**：豁免落地后合法 hit 源归零，gate_report（:10140）注释
  更新为"post-A.3d 纯 MODE workload 的 probes 亦应读零；残余归属 V-C 族（#3/#7）
  或已披露 N-low 行"——渲染逻辑零改动（不动 A.3c 已验证面）。
- KUnit：`j1_implant_exempt`（§2.6）断言植入访问后 probes/hits 差分**双零** +
  audit_gate 渲染 `gate_pass          1`。

### 2.2 S-5① msync 窗口段→0（任务 2，审计 #23）

- `corten_arena_msync_skip(mm, start, end)`（mm/corten_arena.c:11722）：纯函数，
  从 start 起逐帧扫登记表（xa_load），遇第一个未登记帧（洞/植入）停——返回推进后
  的 start（无进展原样返回）。活跃+parked 帧都算占用（A.1 前二者都有预约/shadow
  VMA，匿名 no-op）。
- mm/msync.c 三处接线（**三处调用**，brief §2.11 口径）：循环首 find_vma 前
  （:63 区）、MS_SYNC 分支重取前（:127 区）、else 分支重取前（:136 区）；每处
  skip 后 `start >= end → error = 0` 出口（纯占用段 = 匿名 no-op）。混合区间
  分段处理：占用段跳过不置 unmapped_error、不查树；洞段保持 legacy -ENOMEM；
  植入段 find_vma 找到真 VMA 走文件 msync。
- SYSCALL 体提取为 `ksys_msync`（mm/msync.c:36，原型 mm/internal.h）——测试
  驱动真实循环（含三处 skip 臂）所需的最小提取，无行为变化。

### 2.3 S-5② mincore 活跃窗真值走查（任务 3，审计 #13，D24/OQ-MV-11 提前交付）

- `corten_arena_mincore_route(mm, addr, pages, vec)`（mm/corten_arena.c:11847），
  do_mincore 入口前置（mm/mincore.c:253，-EAGAIN = 走 legacy）：
  - **门**：双门 ∧ MODE ∧ chunk 起点在窗内（跨界 chunk 由 syscall 的 PAGE_SIZE
    分块自然拆成 [窗内段路由][其余 legacy]）；窗内每帧必须在册，否则 -EAGAIN
    （洞→legacy -ENOMEM 保持；植入→legacy 树上真值保持）。
  - **parked 段**：全 0 向量（旧预约 VMA 语义，内容在 park 时已清）。
  - **活跃段真值**：`corten_mincore_fill_frame`（:11800）逐 2M 帧走查——
    corten_arena_pmd 门控下降（空表→0）+ pmd_leaf 防御臂（C20 结构性排除，镜像
    mincore_pte_range 的 THP 全 1）+ pte_offset_map_lock 下逐 PTE：
    present→1 / none_mostly→0 / swap→`corten_mincore_swap_truth`（:11770，
    mincore_swap 匿名臂同口径：swap cache 持 uptodate folio 才 1；**外加设备
    存在门** get_swap_device——生产 PTE 隐含活设备，此门纯防御，合成/换出中
    条目答 0 不崩）。INV6：全程只读（ptl 读锁），零写入。
  - 混合 active+parked 段一次调用内分段作答（分类趟 + 填充趟）。
- D24 口径落实说明：本树无 walk_page_range_novma 导出（6.18 已退役；后继
  walk_page_range_debug 要求 mmap_write），故真值腿为 arena 自带的 PMD 门控
  逐帧走查——与 D24 意图（无 VMA 锚的真值直读）等价，且复用 corten_arena_pmd
  的首跑 GPF 修复（门控下降）。

### 2.4 S-5③ madvise 空窗终答 + move_pages 查询腿（任务 4，审计 #20/#28）

- `corten_arena_window_parked_span`（mm/corten_arena.c:11527）：[start,len) ⊆ 窗 ∧
  每帧在册 ∧ 每帧 idle（reserve 哨兵计 idle——claim 窗标记无内容）——即 A.1 前
  PROT_NONE 预约 VMA 描述的精确 VA 集合；完备性用登记帧计数 == 跨帧数比较。
- madvise_route 头部新臂（:11595 之后）：parked span 上 DONTNEED/DONTNEED_LOCKED/
  FREE（内容 park 时已清，语义等价 0）+ NORMAL/SEQUENTIAL/RANDOM/COLD 四 hint
  （活跃臂 no-op 的 parked 延伸）→ 终答 1（计数 madvise_parked）；其余行为保持
  legacy（walk → -ENOMEM）——**S-5 残余行**（A.1 前对 PROT_NONE vma 多为 0，
  逐行为补齐留 V-C 前复议，brief 原文登记）。洞窗/活跃帧邻接/混合形态不触发
  （谓词 false → 既有臂），洞窗 -ENOMEM 与 A.1 前一致（从未有 VMA）。
- J1 红利：parked 窗 madvise 不再走 find_vma_prev + lock_vma_under_rcu（#20 的
  两个污染源结构性消失）。
- #28：`corten_arena_move_pages_window`（:11922）+ do_pages_stat_array 逐页短路
  （mm/migrate.c:2499）：MODE ∧ 地址⊂窗 ∧ **非植入登记** → 直接 -EFAULT（errno
  与今天逐字节一致），零树走查；植入地址放行（真 VMA，nid 是真值）。行为零偏移，
  纯廉价化。

### 2.5 观测面

四新计数器（atomic_long，全屋惯例）+ arena_stats 四行：`msync_window_skips` /
`mincore_routes` / `madvise_parked` / `move_pages_window`（mm/corten_arena.c:2704
区渲染）——披露用，guest 判据用来把 j1_probes 残差归因到 S-5 短路次数。

### 2.6 KUnit D 组（mm/corten_arena_test.c:7797-8245，5 用例）

| 锚 | 位置 | 断言要点 | 结果 |
|---|---|---|---|
| msync_window_segments | :7797 | 纯 parked 窗 MS_SYNC→0；委托 VMA+parked 混合→0；parked 头+洞尾→-ENOMEM；纯洞（两 flag）→-ENOMEM；**占用/VMA 腿 probes 差分恰 0**（洞腿各 +1 = A.1 前同样付出的 legacy 洞查，注释写明）；msync_window_skips 计数 >0 | ok |
| mincore_route | :7877 | 非 MODE 透明（-EAGAIN）；活跃窗 seed 两页真值 {1,1,0}；swap-out 手卷后 {1,0,0}（未缓存条目 = mincore_swap 匿名臂口径）；parked 全 0；洞/植入/委托域 -EAGAIN；mincore_routes 计数 | ok |
| madvise_parked_terminal | :7999 | parked 七行为全 1；WILLNEED→0（披露残余行）；洞窗 DONTNEED→0（legacy -ENOMEM 保持）；parked+活跃混合→0（既有臂接管）；活跃窗内 DONTNEED→1（回归锚） | ok |
| j1_implant_exempt | :8086 | 植入 VA 上 find_vma/intersection/prev/lock_vma_under_rcu 四原语 + 邻接帧 find_vma 返回植入片的第五形：**probes/hits 差分双零**；walk==0；gate 渲染 `gate_pass          1`；负控制：未登记外来 VMA 的 hit 恰 +1（豁免由登记表驱动而非窗口盲豁免） | ok |
| move_pages_window | :8185 | parked/洞→true（计数）；植入→false（nid 真值保住）；委托域/窗下界外→false；mode_exit 后→false | ok |

j1_implant_exempt **注册在 B 组尾（C 组前）**：exit-gate 判决是全局累计值，C 组
inv_mv2_inject 的故意违例注入会永久翻红 gate_pass；该锚要在干净前态上断言
"smoke 形状后 gate_pass==1"（注册处注释写明）。

## 3. 验证结果

| 项 | 命令/口径 | 结果 |
|---|---|---|
| 构建（=y） | `make -j8` 全量 ×4（增量迭代 + =n 往返恢复后终建） | RC=0；全树仅存 1 条既有基座警告（objtool cpuidle_enter_state），**零新增**（build-y-final.log） |
| KUnit（三套件） | `timeout 480 qemu … corten=on kunit.filter_glob=corten*` | 终 diff 内核两轮逐位全绿：**corten 24/0/1、corten_arena 90/0/0（85 既有 + 5 新）、corten_fault 31/0/2**（kunit-final6/final7.log）。首轮 on1 一例 msync 断言口径错误（洞腿 probe 计漏）+ 一例 swap 真值腿对无设备合成条目崩（已加设备门）、on2 gate 断言序位错（移注册）——三处均修后 on3 起持续全绿；final1–final5 为风格修正迭代轮，两轮连续全绿后仍有 on3 单例 interlock flake 复现判定（corten 套件 txn_uninstall_interlock，A.3a 登记的已知首跑 flake，本片零接触 txn 层，复跑即绿） |
| 内核日志签名 | mva2-verify 同款 lockdep/oops grep | 零命中；WARNING 清单与 A.3c 基线**逐条全同**（8 条：drain-timeout/foll_force×2/txn_begin×2/zap_single/p4/reactivate + j2 inject 自身）；pgtables_bytes 23 = 基线 22 + 1（mincore 锚的 mkvm 植入 VMA，同一 mkvm 工件族）；Bad rss-counter 2 = 基线 2（swap 手卷补记 MM_SWAPENTS 后本片零新增） |
| =n 折叠 | mva2-verify n-objects 扩至 **16 对象**（+mincore.o +msync.o） | RC=0 零警告 + nm 16 对象零 corten 符号；.config 已还原 =y 全量重建（build-n.log） |
| checkpatch | `--strict --no-signoff --ignore FILE_PATH_CHANGES` | **0 errors / 0 warnings / 0 checks**（1261 行；checkpatch-mva3d.txt；两轮修正：对齐×3/kmalloc_array/fallthrough 措辞×5/声明后空行/spinlock 注释×2/多余签名行缩进还原） |
| diff 导出 | `git diff HEAD > patches/r07-mva3d.diff` | 7 文件 +1067/−12；**未 commit**（HEAD 仍 76a83f8e0d03，7 M） |

## 4. 自证清单（红线核对）

1. **INV6**：mincore 走查只读（pte_offset_map_lock 读、零写入）；msync skip/
   madvise 谓词/move_pages 短路是纯读；J1 豁免是计数器侧旁路 ✓
2. **=n 折叠**：新导出面 4 个（covers_lockless/msync_skip/mincore_route/
   move_pages_window）全有 =n 假值/原样返回体；mincore/msync/migrate 三新钩子
   随 =n 折叠消失（16 对象 nm 实证）✓
3. **不动已验证的 J2 walker/fault 终答主体**：corten_audit_j2_scan/walk/
   walk_locked 与 corten_fault_window_maperr 函数体零改动；本片触碰的
   corten_implant_mark 是登记生产者（A.3c 自己改过三处的同一域），改造是
   豁免读侧安全的前置条件，语义（排序+去重+合并+裁剪）逐行保持 ✓
4. **range_overlaps 语义不动**（16+ 拒族调用者依赖 skip-idle）：零触碰 ✓
5. **锁纪律**：mincore/msync 路由在 mmap_read 下（caller 持有）；madvise 谓词
   在 madvise_lock 任意形态（RCU xarray 走查，range_overlaps 同骨架）；
   covers_lockless 快照协议 + RCU 退休（§2.1）使 lock_vma_under_rcu 的 RCU 上下文
   合法；kfree_rcu_mightsleep 只在 mark 的 mutex+mmap_write 冷路径 ✓
6. **热路径默认零开销**：J1 豁免在既有 j1_slow 慢路径内（双门后）；三 syscall
   新臂全部双门（static-branch + corten_mode）+ 窗口判定前置 ✓
7. **计数器 atomic_long**（全屋惯例）✓
8. **不 commit / 与 B 系列无交集**：7 文件全在 mm/ ✓
9. **行号口径**：本文 file:line = 最终实码 ✓

## 5. 与裁决/任务书的口径对表（三处主动裁断，提请追认）

| 决策点 | 本片处置 | 备选/理由 |
|---|---|---|
| mincore 真值腿实现 | arena PMD 门控逐帧 PTE 直读（house machinery） | walk_page_range_novma 在本树（6.18）已不存在，后继 walk_page_range_debug 断言 mmap_write（mincore 持 read 不可用）；D24 的"无 VMA 锚真值"意图等价落实 |
| madvise 空窗终答的"空窗" | **parked span**（每帧 idle 在册）而非"无活跃 region 即终答" | brief §2.11 权威口径（occupied_incl_idle∧非活跃）；纯洞窗 madvise A.1 前后都是 -ENOMEM，无回归可修，保持 legacy（KUnit 锚之） |
| msync/madvise/mincore 洞窗腿 | 保持 legacy（-ENOMEM + 一次树查） | 洞查询在 A.1 前同样付树查（S-5 前合法形状，非 J1 污染源）；为它们加零树查短路收益极小（A.3b §6.9 同判） |

## 6. 残留与移交

1. **msync/madvise 的跨界混合形态**（窗+委托域跨越、parked+洞在同一 madvise
   区间）：按 brief 口径保持 legacy 分段行为（mincore 因 syscall 分块天然分段，
   msync 因 skip 分段，madvise 因逐 VMA walk 分段）；尾部 -ENOMEM（msync/
   madvise 的 unmapped_error 形状）是 A.1 前后一致的既有语义，非回归
2. **S-5 残余行为行**：madvise WILLNEED/POPULATE 系等对 parked 窗仍 -ENOMEM
   （A.1 前对 PROT_NONE vma 多为 0）——brief 登记的 V-C 前复议项，KUnit
   WILLNEED→0 锚披露
3. **move_pages 活跃窗 nid**：短路只廉价化（同 errno）；"numa 工具对 arena
   页的 nid 真值"与 #3/#7 同属 V-C gup_probe 归零族
4. **mincore swap 真值腿的设备门**：对无设备条目答 0（防御）；若未来合成
   swap 条目进入窗口（migration 条目已由 non_swap_entry 臂覆盖），口径与
   mincore_swap 匿名臂一致
5. **A 系出口严格终判**：guest 判据见 §7；bpftrace 双口径（brief §6.11）不变

## 7. guest 验收判据（A 系出口门，主会话跑）

入口：`cat /sys/kernel/debug/corten/audit_gate`（j2_* 与 S-5 四计数在 arena_stats
同步可见）。

1. **标准门**：run13 + churn 全电池后 **gate_pass == 1** ∧ **j1_hits == 0** ∧
   j2_violations 差分 == 0 ∧ j2_first_violation == 0x0（A.3c §7 判据原样）。
   A.3d 后 j1_hits 的唯一合法来源（植入访问）已被豁免，**任何非零 hits 都是
   违例**——门首次收紧为无条件
2. **j1_probes 差分分级**（理想为零）：
   - 预期零来源：msync/madvise on parked 窗（S-5 短路）、mincore（vma_lookup
     本不在五原语内）、植入访问（豁免）、fault #1/#2（A.3b）、uffd（#29）、
     hint/NOREPLACE 前置（A.3b 扫尾）
   - 合法 S-5 前形状残差（非零时的定性清单，对表 arena_stats 归因）：洞窗
     msync/madvise 查询（每查询 1 次树查，A.1 前同形）；GUP-slow（#3，
     gup_window_miss 计数对拍）；远程访问 expand_stack（#7，remote_win_short
     对拍）；低频行 #30/#32/#37/#38/#40（A.3b §6 披露）
   - 若 probes 残差无法归入上述任何一类 → 新形状，回报主会话定性
3. **S-5 四计数对拍**：workload 内有 park 的形状应见 msync_window_skips /
   madvise_parked 增长（与 workload 的 msync/madvise 调用数同阶）；零增长 +
   strace 出现窗内 -ENOMEM → S-5 门未生效，报缺陷
4. **strace diff**：mincore/msync/madvise(DONTNEED 系+四 hint) 对占用窗零
   新错误（S-5①②③ 的 DoD）；洞窗 -ENOMEM 保持（非回归）

## 8. 一句话结论

S-5 三条 -ENOMEM 型语义回归全部按 D24 裁决恢复 A.1 前基线（占用窗口径），
J1 豁免把植入访问从计数器侧摘除——audit_gate 的 gate_pass 在 smoke 植入契约
形状下首次转绿（KUnit 已锚），A 系出口门只剩 guest 侧 run13+churn 的最终读数。
