# w3fix4 开发报告 —— MV2 终局收官·工作一（残留台账 #1/#2 修复 + DPA 复测）

- 日期: 2026-10-06
- worktree: /home/ppw/linux-6.18-mva（分支 mv-a0，基 commit 673ba95521de，W1.a-e/MV3.a-b 主体零触碰）
- 补丁: /home/ppw/cortenmm/patches/r07-w3fix4.diff（代码面 mm/ + include/，1428 行 diff）
- 证据目录: results/r07/w3fix4/（worktree）
- 红线自查: INV6 维持（探针 pin 走查只读；pin 协议即 W1.e1 原形）；=n 折叠 16 对象零符号实测；
  不 commit（主会话验证后统一入库）。

## 0. 两句话结论

**UAF 根因**: `corten_arena_check_empty_locked`（declare/adopt 事务的空窗探针）是全树唯一不持 M2a
描述符锁就走查 PT 页内存的读者——pmd 读 present 与 PT 页内存被退休/释放之间无任何屏障，DPA 世界
86s 即 oops（`corten_brk_grow_route → declare_locked → check_empty_locked` 读已释放 PT 页）。
**修法**: 探针在走查前 `corten_ptdesc_get(pfn)` 钉住描述符 → `read_lock_bh(&desc->lock)` 横跨数组
走查 → `desc->stale || desc->mm != mm` 判已退休形跳过计数（`probe_stale_skips` 披露）——退休漏斗
（xa_erase → 写锁下 store stale → put）与该读锁互斥，保证 stale=0 观察后页面不可能退休，W1.e1
pin 协议原形，`corten_arena_frame_ptes_empty`（退出走查的整框判定）同形补齐。

## 1. 修复内容（工作一）

### 1.1 台账 #1: 探针 M2a 排除（mm/corten_arena.c）

- `corten_arena_check_empty_locked`: 逐窗 `pmd = READ_ONCE(*pmdp)` 后 pin+read_lock_bh+stale/mm
  复查；`!desc`（已卸装）与 stale/异 mm 一律计 `corten_nr_probe_stale_skips` 跳过（漏斗契约保证
  退休窗口内容为零）。THP `pmd_leaf` 的 -EBUSY 臂保持。
- `corten_arena_frame_ptes_empty`: 同形 pin 协议（该读者决定整框退休，DPA 同罪形）。
- 屏障完整性核实（本报告复核）: get() 的 `rcu_read_lock + xa_load + refcount_inc_not_zero` 对
  卸装者安全（先 erase 后放基引用）；read_lock 与卸装者的 write_lock_bh 串行化 stale 发布；两只
  free 漏斗（`pte_free` / `___pte_free_tlb`）都在页内存回归分配器之前同步走 hook → stale=0 观察
  后不可能走查已退休页。`desc->lock(读) → ptl` 与事务层 `desc->lock(写) → ptl` 同序，无 ABBA。
- 计数器 `probe_stale_skips` 入 arena_stats 渲染 + KUnit 锚（下）。

### 1.2 台账 #2: arena_stats 渲染挂起（mv3b §3 设计落地）

- `corten_mm_state_pages` 弃 registry xarray 枚举（=on 世界读者在 `xas_find` 内不可杀自旋的
  现场，本次在 P2 VM 活体捕获，见 §4）——改走 arena 观察台账（arenas 文件同源，该文件同世界
  读取正常），每代 `stats_gen` 去重，窗口预算 65536 + 1M 节点硬界。
- `corten_arena_stats_report`/`corten_registry_pin`/pin-round 全部预算化（64 rounds/1M 节点），
  截断统一披露 `stats_walk_truncs`（渲染行 + KUnit 锚）。
- 渲染者与 KUnit 锚: `corten_arena_test_stats_budget(2)` 驱动截断臂、断言
  `stats_walk_truncs` 递增、恢复 65536 后回归零增。

### 1.3 台账 #5（新发现，工作一 DoD 拦路）: brk trim 边界框滞留 → 退出 GPF

- 发现: DPA 复测的 brk 全速流量驱动（2 进程 ×300 轮 grow/trim/release）在**基线内核**确定性
  GPF（单轮即复现）: `corten_arena_mm_exit+0x12d` 写 `0xdead000000000122`（LIST_POISON2）。
- A/B 隔离（base 内核，guest 内四臂）: grow-only ✓ / trim 不全释放 ✓ / 一次性全释放 ✓ /
  **trim+最终全释放 = 崩**——多轮堆叠、DPA、并发皆非必要。
- 根因: `corten_brk_shrink_route1` 的 trim 臂故意保留旧边界框 claim（框内保留页的 tier-1 界与
  事务封印读该框位），但当 newbrk 跌破该框基址后框内已无保留页，claim 不再撤销 → 记录滞留在
  `ar->end` 之上的框位；全释放的框循环只走 `[start_frame, (end-1)>>PMD]`，永远够不到滞留框 →
  退出 R1 走查重新发射一个已 obs_remove（obs.prev 已是 POISON2）且已 kfree_rcu 的记录 → 第二次
  `list_del_rcu` 在毒化 prev 上 GPF。
- 修复: trim 臂在 `(oldbrk-1)>>PMD > last 且 (newbrk-1)>>PMD < bf` 时对旧边界框
  `corten_slot_remove(..., restore=false)`（框内无保留页，保留页语义不受影响——newbrk 仍在框内
  时 claim 照旧保留）。
- KUnit 锚: `corten_arena_test_brk_boundary_strand`（未对齐边界的 grow→两轮跨界 trim→全释放，
  断言全释放后 `corten_arena_test_registry_records()==0`，并复一轮 re-grow；修复前该锚读 1 且
  fixture 退出 GPF）+ 新测试钩 `corten_arena_test_registry_records`。
- 分类: **基线即溃**（stash-diff 基线内核 76fd8d5f9aaf8f80 同 churn 同 GPF 实证），非 w3fix4
  回归；归档为残留台账 #5（W-4 brk region 路由族）。r07 时代 iter 注释里的 "systemd-tmpfiles
  GPF"（survivor 子句）修的是另一形状（head-punched 记录）；本形状 it->last 守卫天然不设防。

## 2. DPA 复测（台账 #1 DoD）: PASS

- 构建: `CONFIG_DEBUG_PAGEALLOC=y + DEBUG_PAGEALLOC_ENABLE_DEFAULT + PAGE_OWNER=y`（#389 含
  台账 #5 修复，03c5b2eb84b8f3ac）；世界 = 原始定罪 boot: `corten=on corten_mode_default=on
  page_owner=on`。
- brk 全速流量: 2 进程 ×300 轮（16 PMD 窗 grow + 3M/1M 异步长 trim + 全释放）双进程 rc=0；
  路由计数 `brk_region_grows=11778 / shrinks=9766 / adopts=913 / brk_legacy=0`——100% 路由、
  零 legacy 退化。
- oops: **零**（churn 期间与读数期间；boot 相 6 条命中全部为已登记既有族——5×服务退出
  pgtables_bytes 残差 [mv3c 时代已知噪声] + 1×exec-default 递归类 lockdep 投诉 [MV3.a，
  不同 mm 对象的嵌套获取，PROVE_LOCKING 下 1 次/boot，fs/exec.c 本片零触碰]）。
- arena_stats 读: **rc=0 ×2**（churn 后首读即回，120 行；台账 #2 修复在 DPA 世界成立；
  `stats_walk_truncs=2` 为披露性截断，非挂起）；audit_gate rc=0。
- 采证: results/r07/w3fix4/{dpa-retest-verdict.txt, dpa-arena-stats.txt, dpa-audit-gate.txt,
  console-dpa.log, console-dpa-base.log（基线 GPF）, console-dpa-base-ab.log（A/B 四臂）,
  dpa-boot.sh}。

## 3. 验证汇总（终镜像 #393, 143fe83e1a1602d5）

| 门 | 结果 | 证据 |
|---|---|---|
| KUnit 三套件 filter_glob=corten*（flake 复跑判定） | **绿**: =off×3 + =on×4（on1 的 file_fork_mirror 单点翻rig为已登记 flake 族，复跑即绿，mv3c flake-note 同族）; 终镜像 on×1(140/140)+off×1(25/28/7, skip 对账精确 112) | kunit-on1..6, kunit-off1..4, kunit-{on,off}-final.log |
| KUnit 新锚 | probe_stale_pt（合成退休 desc + present PMD + 毒化页内存 → declare 过 + skip+1，双世界跑）; stats_walk_budget; brk_boundary_strand（=on 140 内） | 同上 |
| =n 16 对象零符号 | **绿** memory/mmap/migrate/rmap/swapfile/gup/oom_kill/mempolicy/mremap/madvise/mprotect/sys/fork/exec/mincore/arch-x86-fault 全零 | n-symbols-w3fix4.txt, build-n-w3fix4.log |
| checkpatch --strict 全量 diff | **0E/0W/1C**（1C = spinlock_t 声明无注释，历史同款披露） | checkpatch-w3fix4.txt |
| guest 双 boot | **=on**: churn 100 轮 rc=0、brk_legacy=0（100% 接管）、exec_default_enters=215、stats rc=0、probe_stale_skips=0；**=off**: 分离证明 arenas=0/auto_mmaps=0、legacy brk 4433 披露、oopses=0、dmesg 仅 boot 横幅 | console-gate-{on,off}.log, gate-boot.sh |

## 4. P2 VM 活体标本（台账 #2 的 before 证据）

残留台账 #2 的挂起在 P2 VM（旧内核 #365）上活体捕获: 六个自 16:41 起的 R 态不可杀
`grep arena_stats` 读者（各 130+ min CPU），SIGKILL 免疫，新读同挂；sysrq-t 栈 =
`xas_find/xas_load ← corten_mm_state_pages+0x108 ← corten_arena_stats_report`——mv3b §3 签名
同形。采证: results/r07/mv3d/p2-stats-hang-sysrq.txt + p2-counters.txt（处置记录）。

## 5. 披露（非红）

1. exec-default 递归类 lockdep 投诉（MV3.a，1 次/boot，仅 PROVE_LOCKING+default=on 世界可见，
   不同 mm 对象嵌套；本片零触碰 fs/exec.c）——建议 MV2 收官后以 `mmap_write_lock_nested` 形
   小片收口。
2. boot 相 pgtables_bytes 残差（服务退出形，mv3c 时代已登记噪声族；本片未加重：=off 世界为零）。
3. file_fork_mirror 单点 flake（1/6 次 =on 运行，mv3c flake-note 已登记同族；复跑判定绿）。
4. 工作树附带改动（非本片代码）: mv3d-gate.sh SSH 韧性（台账 #3）、REPORT.md MV3.d verdict 表
   （台账 #4 采证侧）——见 triage-report.md。
