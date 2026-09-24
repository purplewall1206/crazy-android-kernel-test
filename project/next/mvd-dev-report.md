# M-V V-D 开发报告：exit 纯 PT 走查 + 上层表自拆 + carrier/region 终拆 + swapoff 复测收口

- worktree: `/home/ppw/linux-6.18-mva`（分支 mv-a0，基线 = 主树 HEAD `8c4706e1e862` = V-C）
- diff: `patches/r07-mvd.diff`（**+666 / −82**，5 文件；生产 ~+456/−82，测试 +210——spec 预估 ~+420/−60+测试，在界内）
- 未 commit（按红线）；checkpatch `--strict` **0E / 0W / 0C**（`results/r07/checkpatch-mvd.txt`，867 行受检）

## 0. 改动统计

| 文件 | 增/删 | 内容 |
|---|---|---|
| mm/corten_arena.c | +456/−74 | exit 走查 + 上层表自拆 + PTE 页记账修复 + 计数器 + S-3 note |
| mm/corten_arena_test.c | +210/−6 | 全生命周期账锚 `corten_arena_test_exit_lifecycle` |
| include/linux/corten_arena.h | +18 | 测试钩声明 + unuse_blind_note（含 =n 内联桩） |
| mm/mmap.c | +7/−4 | exit_mmap 头注释更新（走查次序） |
| mm/swapfile.c | +8 | unuse_mm 的 S-3 盲区计数披露 |

## 1. exit 纯 PT 走查（corten_arena_exit_walk，spec §3.4）

`corten_arena_mm_exit()` 从"排干 + 依赖 legacy free_pgtables"改为对窗口域自走查
（mm_users==0、树/registry 双冻结、mmap_write 全程持有——与 free_pgtables 同级栅栏）：

**相位 A（逐窗 zap + PTE 页退役）**：单 fullmm mmu_gather（INV6：一切 PT 页释放经
pX_free_tlb 批量漏斗，M2a uninstall 由 ___pte_free_tlb 的 corten hook 拥有）。
逐 PMD 窗判走查资格：
- 帧槽 ≠ 本 arena（punch 挖洞帧）→ 遗留域（洞内是 legacy VMA/植入体）；
- 树内有 VMA 相交本窗（targeted-DECLARE 影子、punch 植入体）→ 遗留域
  （后续 unmap_vmas/free_pgtables 以 VMA 界定处理）；
- 其余 → 连续 run 合并，`unmap_chunk_flags(zflags=0)` 全清语义（**swap 条目经
  M6.T2 D7 臂释放：free_swap_and_cache + MM_SWAPENTS −1 + zap_swap_frees 计数**）
  → `corten_arena_free_ptes_span()` 经 pte_free_tlb 释放 PTE 页并 **pmd_clear +
  mm_dec_nr_ptes**（新记账——见 §2）。

**相位 B（上级表自拆，B-2 收口）**：对窗口域触达的每级"独占"页：
- PMD 页（每 PUD_SIZE=1GiB 段）：pud 在位非叶 ∧ 段内无树 VMA ∧ 页内 512 pmd 项
  全 none → `pud_clear + pmd_free_tlb + mm_dec_nr_pmds`；
- PUD 页（每 P4D=512GiB 段）：p4d_clear（l4 运行时即 pgd 槽清除，native_set_p4d
  PTI 感知——与上游 free_p4d_range 同族）+ pud_free_tlb + mm_dec_nr_puds；
- P4D 页（每 PGDIR 段，仅 la57 运行时实存；l4 由 `mm_p4d_folded()` 跳过，
  p4d_free_tlb 本身折叠 no-op）。p4d 页上游本就不入 pgtables_bytes 账
  （free_p4d_range 同样裸释放）。

独占判据 = 上游 floor/ceiling 纪律的无 VMA 泛化：该级页的服务段内无任何树 VMA，
则稍后的 free_pgtables 永不下到该级页（X_none_or_clear_bad 短路）——不重释、不漏
释；页内非 none 残项一律保守保留（残值由 pgtables_bytes 披露，不猜内容）。
邻接 arena 共享段由 done_pmd_seg/done_pud_seg 游标去重（升序遍历下单调）。

**走查位置**：j2 审计（A.3c 触发点，判据不变——仍是 mm_exit 头第一句，先于一切
drain/zap）→ **exit_walk** → unpublish（zap 计账仍读 corten_state）→ drain 循环
（zap 的 rmap 锚 = carrier，在描述符终拆时才释放——次序即安全序）→ RCU 尾巴。

### 顺手修复：punch 幸存窗的 exit 泄漏（走查中发现）
旧 V-A.1 循环在 arena 级做 "any_vma → skip 整个 arena"：带 punch 植入体的 arena
其**无 VMA 幸存窗**的内容页与 PTE 页在 mm 死时无人释放（unmap_vmas 只走树）——
真泄漏。V-D 窗粒度判据天然收口（幸存窗走查、洞帧/植入体留给 legacy）。KUnit 侧
可见证据：pgtables BUG 行从 V-C 基线 25 行（残值 4096..24576）降到 16 行且**全部
恰 8192**（= PMD+PUD 一对，见 §5 残留定性）。

## 2. B-2 收口的记账对（pgtables_bytes 归零）

1. **PTE 页半边（既有缺口，本片修）**：`free_ptes_novma` 释放 PTE 页但从不
   `mm_dec_nr_ptes`——fill_upper()/fault 回退的每次安装都经 `__pte_alloc`
   （inc），释放却无 dec → 每无 VMA 窗 4096 字节漂移。重构出 caller-gather 核心
   `corten_arena_free_ptes_span()`（dec 配对），`free_ptes_novma` 变薄壳；
   RELEASE/park/mm_exit 三族调用点同时受益。
2. **上级表半边（本片新建）**：相位 B 的 pmd/pud 自拆 + mm_dec_nr_pmds/puds。
   记账对齐上游：__pmd_alloc/__pud_alloc 的 inc ↔ free_p*_range 同位 dec。

**KUnit 判据已入锚**（§4）：全生命周期后 `mm_pgtables_bytes(mm) == 0` +
exit_upper_pmds/puds 计数 = 足迹（每 mm 每 1GiB/512GiB 段各 1）。

**KUnit 台面残留定性**（非本片回归）：16 条 `BUG: non-zero pgtables_bytes:
8192` 全部来自测试工件几何——测试专用 arena 多在 8MiB 低址（CORTEN_ARENA_TEST_BASE），
与 harness mkvm VMA 同 1GiB/512GiB 段 → 走查按独占判据正确让位，而 legacy
free_pgtables 以 VMA 界（8..16MiB）也覆盖不了整段——**与上游对共享段的边界行为
逐字一致**（基座自身同形）。V-C 基线同位置是 25 行、残值高达 24576（含 PTE 页
漂移 4096 族）——本片后 PTE 漂移族消失、残值封顶 8192。guest 窗口域在 16TiB
专属区（段独占），不受此几何影响。

## 3. carrier/region 终拆次序与 RCU 尾巴（论证，代码次序未变）

终拆序列（既有次序，本片在其前插入走查并逐条复核）：

```
mm_users==0
  ├─ corten_audit_j2_walk_locked()      A.3c 触发点：registry+树原样（V-D 未动）
  ├─ corten_arena_exit_walk()           zap 以 carrier 为 rmap 锚；PTE/上级表退役
  ├─ smp_store_release(corten_state,NULL)  unpublish（无读者可并发）
  ├─ drain 循环（ctl_lock 单边）：
  │    obs_remove → percpu_ref kill+wait → corten_arena_free(arena)：
  │      corten_region_file_teardown()   i_mmap 摘除 + file 引用下放
  │      carrier: unlink_anon_vmas + vm_area_free（carrier 死于描述符）
  │      percpu_ref_exit → kfree_rcu(arena)   ← RCU 尾巴 1
  └─ list_del_rcu(shrink_reg) → synchronize_rcu()（或 A5 快路 call_rcu）→ state free
                                        ← RCU 尾巴 2
```

- **A.3c walker 触发点不变**：仍是 mm_exit 第一句，先于本片走查与一切拆解；
  walker 只读树+registry（mm_users==0 双冻结），判据与调用形态零改动
  （walker 主体未触碰——红线遵守）。
- **安全序**：zap（用 carrier）→ drain（释放 carrier）；region 注销（obs_remove
  先于 drain 等待，drain-timeout 期间不可见性已决）→ 文件载荷 teardown（在
  carrier free 之前——teardown 读 carrier 找 interval-tree 节点）→ 描述符
  kfree_rcu。walk 与 drain 均 ctl-free：锁序 mmap > ctl 不反转（DEV-13）。
- **RCU 尾巴**：arena 描述符 kfree_rcu 覆盖 V-C region 读者（mmap_read 或 RCU 下
  xa 帧槽读）；state 的 shrink_reg 摘链 + grace period 既有两形态（A5 arena-less
  call_rcu 快路 / 含 arena synchronize_rcu）不变。

## 4. KUnit 锚：合成 mm 全生命周期账（新用例，95/95 全绿）

`corten_arena_test_exit_lifecycle`（spec 终局锚形态）：
mmap（auto 接管双窗）→ fault（两窗 mapped 页 + 虚拟分配）→ mprotect 路由降权 →
madvise/DONTNEED 形 chunk 清 → mremap 原址收缩（尾窗退役）→ **fork**（镜像
begin/commit，子窗 PT 就位）→ 子 mm 经真实 mmput()→exit_mmap() 退出 →
**swap**（M6.T2 手卷 swap-out：合成条目 + MM_SWAPENTS+1 + Swapped 元数据）→
**park**（第三窗精确 munmap 路由入池）→ exit walk（直接调用，断言在延迟 mm
释放的此侧）。

断言族：
- **maple 白名单 ∧ J1==0**：`corten_audit_j2_walk(mm)==0`（INV-MV2 终局）+
  j1_hits 零前进；
- **账目三元组闭合**：全局 ptdescs/meta_arrays Δ==0（含子 mm 份额）；folio 侧
  读自 mm：MM_ANONPAGES==0 ∧ MM_SWAPENTS==0（swap 条目经 zap 释放，
  zap_swap_frees 恰 +1）；
- **pgtables_bytes == 0**（B-2 判据入 KUnit；harness VMA 不 fault，窗口域是唯一
  PT 足迹）；
- **上层表计数对账**：exit_upper_pmds Δ==2、puds Δ==2（父+子各一对；窗口均在
  16TiB 专属段）、p4ds Δ==（l4 折叠 ? 0 : 1——la57 下树空子 mm 释放其 p4d 页，
  父 mm 256TiB 段含 harness VMA 按上游边界纪律保留）；
- drain-timeout 零前进（降级臂不回归，`drain_timeout_stat` 既有件继续绿）。

## 5. 验证结论

| 项 | 结果 |
|---|---|
| make -j8 | PASS（bzImage #91；=n 折叠复核：CORTEN 全关后 swapfile.o/mmap.o/memory.o 干净编译，桩内联生效） |
| KUnit 三套件（corten=on, filter_glob=corten*） | **corten 24 pass/1 skip（门约定）、corten_arena 95 pass/0 fail、corten_fault 31 pass/2 skip**——kunit-on-final.log + kunit-on-final2.log（终源连续两跑，加中期两跑共 4 绿） |
| KUnit（corten=off 默认引导） | 三套件全绿（72 skip = 门约定），kunit-off1.log |
| lockdep/oops 签名 grep | 零命中（recursive locking/deadlock/unsafe locking/bad unlock/DEBUG_LOCKS_WARN/BUG/GPF） |
| WARNING 指纹对拍 V-C 基线 | **逐条全同 8 条**（drain-timeout/foll_force×2/txn_begin×2/zap_single/reactivate/j2 inject 自身），行号平移=本片插入代码 |
| "non-zero pgtables_bytes" | 基线 25 行（4096..24576）→ 本片 16 行**全部恰 8192**（PMD+PUD 对，测试低址几何的共享段让位，与上游边界行为一致）；PTE 漂移族（4096 分量）消失 |
| "Bad rss-counter MM_SWAPENTS −1" | 2 行 = 基线波动上界（mvc on2-4 同为 2，RCU 延迟 mmdrop 的时序浮现；on1 同为 1 时亦有）；`get_swap_device: Bad swap …05d` 1 条 = 新锚合成条目，与基线 …077（inv7_swapped 族）同类 |
| checkpatch --strict | **0E / 0W / 0C**（867 行，results/r07/checkpatch-mvd.txt） |
| 红线 | INV6 ✓（一切 PT 释放经 pX_free_tlb 漏斗）；=n 折叠 ✓；未 commit ✓；V-C 渲染与 A.3c walker 主体零触碰 ✓；无第 4 白名单写点 ✓（走查经 unmap_chunk_flags 事务臂 + 标准 tlb 漏斗，mm_users==0 carve-out 形态） |

## 6. S-3 / OQ-MV-6 swapoff 复测：判据设计与登记

**内核侧（本片落地的披露）**：`unuse_mm()` 每次访问带 arena 的 mm 时
`corten_arena_unuse_blind_note()` 计数（`unuse_blind_mms`，debugfs arena_stats
行）——纯观测不路由（spec §4 倾向"swapoff 非热路径，不补 unuse 分支，计数披露"）。

**行为定性（代码级论证，guest 实测定）**：unuse_mm 树界 → 无 VMA 窗的 swap 条目
对其盲；try_to_unuse 以 `swap_usage_in_pages==0` 为循环条件——盲条目在持有者 mm
退出（V-D exit walk 的 zap 释放条目）或窗口 fault（do_swap_page 慢车道经 fault
门换入并下放 swap 计数）前不归零 → **swapoff 有界自旋**（可中断：signal_pending
出口），非泄漏、非挂死。targeted-DECLARE 影子窗（树内 VM_CORTEN）的条目今天即可
被 unuse 走到——盲区仅 A.2 后的 carrier 窗。

**guest 判据（建议脚本 g-s3，主会话纳入门）**：
1. 真盘 swapon（非 zram 循环）→ MODE 进程窗口写 N 页 → 内存压力/`corten` shrinker
   驱动换出（swapped_out>0，VmSwap>0 佐证）；
2. 分支 A（先读回）：进程全量读回窗口（swapins>0）→ `swapoff` 应**干净成功**，
   unuse_blind_mms>0 但无自旋（所有条目已换回）；
3. 分支 B（盲持）：不读回，另一进程 `swapoff -a` → 观察：自旋期间 VmSwap 不减、
   `unuse_blind_mms` 增长；`SIGINT` 可中断（EBUSY/INTR 语义与上游一致）；
   随后 MODE 进程 exit → 数秒内 inuse_pages 归零、swapoff 重试成功——**V-D
   exit walk 是盲条目的收口通道**（zap_swap_frees 前进恰为盲条目数）；
4. 判定：无永久泄漏（swapoff 终究成功）∧ 无未中断挂死 ∧ 计数披露齐
   （unuse_blind_mms/zap_swap_frees/swapins）→ S-3 按 DEV 披露收口；若分支 B
   实测出现不可中断挂死或条目永久滞留 → 升级回炉（顺手路由：unuse_mm 加 region
   枚举臂，~60 行，届时立项）。

## 7. guest 验收判据（写给主会话的门）

1. **标准门**：r07-integrated 口径全量回归（=on/=off 双跑；dmesg corten 零新增
   WARN/BUG；J1==0/J2 gate_pass=1）；
2. **严格门（本片新）**：fork_roundtrip 1000 + memcg OOM-kill 大 MODE 进程
   （reap+exit 干净，C19/C27 合流）+ punch 幸存窗 workload（植入体在 arena 内，
   exit 后 MemFree 对账——§1 顺手修复的实证）；
3. **串口零 pgtables BUG 行**：全生命周期 MODE 压力（多窗多进程退出）后串口
   `grep -c "non-zero pgtables_bytes"` == 0（guest 窗口域 16TiB 段独占，无
   KUnit 低址几何的共享段让位）；`/proc/meminfo` PageTables 稳定性（MODE 进程
   退出前后对账）；debugfs exit_upper_pmds/puds 计数与进程窗足迹一致；
4. **swapoff 复测脚本**：§6 g-s3 两分支。

## 8. 遗留与移交

- mvb4 §5.2 的 mm_alloc 子 mm 残留族（fork harness 工件）与本片无关，维持登记；
- S-3 若 guest 实测升级 → unuse_mm region 枚举臂立项（~60 行）；
- V-E（brk 委托裁决+白名单 live 断言）按轨道继续，依赖本片 exit 走查已就位。
