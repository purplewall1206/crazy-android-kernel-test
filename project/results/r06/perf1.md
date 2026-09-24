# M4 perf1 —— arena fault 通道攻坚（2026-09-20）

- 班次: ~09:45–20:10 CST（用户 09-20 常设指令: 无时间门禁，持续到最终结果; 22:00 前收工）。
- 内核谱系: 主树 HEAD=87383f51a3ff（tag corten-r07-t1c, T1c 池, = M4.T5 run3 #83 同源）为基线,
  本班 **commit 802ff7551bd0 tag `corten-r06-perf1`**, 主树重编 #84→#88 五个验证构建,
  终验二进制 = `bzimg/r06-perf1/bzImage-6.18.32-g87383f51a3ff-88-perf1-final`
  (sha256 `438c1497…2ca5edfc`)。全部 guest 实验在同一 trixie-m4t12.img 8 vCPU/4G/KVM,
  corten=on mitigations=off, 动态 mmbench_dyn（sha 38304f06…, hook 实证进 MODE）。
- 邻道: basecheck/m5t1a-vm/syzkaller SIGSTOP; "vm"(10022) 不可停(tmux 会话持续 CONT),
  实测全线程 CPU ≈3.6% 单核 = 噪声级, 记录在案。
- VM 终态: tmux **vm-perf1**(port 10026, pidfile qemu-perf1.pid) = #88 corten=on 留运行。

## 0. 结论一句话

mmap-pf/unmap-virt 的 T0 大负 **不是 fault 通道慢, 是空间操作路由的 mmu_gather 嵌套
强制全 mm TLB shootdown 风暴**（ftrace 实证: unmap-virt t4 每 op ~2 次全 mm flush
+ remote IPI, 同窗 BASE = 95 次/3s ≈ 0; pages:-1 全 mm 形态占绝对主导）。
三个小 diff（lazy per-window gather / park 元数据整块释放 / park flush 后置到
write→read downgrade）把 G1 三格从大负翻成两大正一小负:

| 格 (动态口径, 同 boot BASE vs T0, ABAB×3 ×2 轮中位) | T5-run3 (#83) | **perf1 (#87/88)** | 判定 |
|---|---|---|---|
| unmap-virt low t4 | -83.8% | **+1186% / +1258%** (0.055→0.71 ops/µs) | 大胜, 两轮无重叠 |
| unmap low t8 | +113.3% | **+142% / +153%** (0.00145→0.00359) | 胜且扩大 |
| mmap-pf low t8 | -74.2% | **-14.3% / -15.5%** (0.00126→0.00107) | 残差=机制成本(§4) |
| pf low t8（非三格, 补充） | +29.3% (CV 15~79 登记高方差族) | **-7.1% / -10.9%** | ≤11% ≈ fault 通道裸成本 |
| mmap low t8（参考） | +132~296% 族 | +289% (0.0104→0.0406) | 维持大胜 |

## 1. 剖景（perf record -F 1999 -g, guest, 各 10~15s; 原始 data 留 guest /root/perf1）

### 1.1 mmap-pf low t8 (#83 基线, T0 vs BASE)

| T0 臂 | self% | BASE 臂 | self% |
|---|---|---|---|
| osq_lock | 73.35% | _raw_spin_unlock_irqrestore (wake_up_q←rwsem_wake) | 29.04% |
| x2apic_send_IPI_allbutself | 12.42% | x2apic_send_IPI | 28.50% |
| rwsem_spin_on_owner | 11.01% | finish_task_switch | 19.97% |
| 调用方: down_write←corten_arena_munmap_route 36.75% / down_write_killable←vm_mmap_pgoff 36.60% | | | |

T0 臂的 osq 自旋 = write 锁临界区里有 CPU 密集工作（路由 zap+flush 等待）; BASE 臂的
形态 = IPI 等待 + 睡眠唤醒。**corten_arena_fault_once 及其调用链在两臂 top-35 都不存在**
—— "arena fault 通道 ~2× legacy" 的直接原因不在 fault 侧。

### 1.2 unmap-virt low t4 (#83 基线, T0 臂)

| self% | 函数 | 调用链 |
|---|---|---|
| 27.27% | __send_ipi_mask | flush_tlb_mm_range←tlb_finish_mmu←**corten_arena_mmap_route**(18.32%) / **unmap_chunk_flags**(8.95%) |
| 22.99% | rwsem_spin_on_owner | down_write_killable←vm_mmap_pgoff |
| 22.27% | osq_lock | mmap/munmap write 竞争 |

BASE 臂 top = 纯 mmap_lock VMA 分裂/合并竞争（osq 44% + rwsem 16%），无 IPI 块
（virtual unmap 无 PTE → legacy 零 flush）。

### 1.3 ftrace tlb_flush 实证（判据级）

| 窗口 | T0 (MODE) | BASE |
|---|---|---|
| unmap-virt t4 3s 事件数 | **82,942** | **95** |
| 形态（2s 窗） | pages:-1 全 mm 46.3K + pages:4 ranged 2.1K | 仅 pages:-1 task-switch 族 ≈0 |

全 mm 形态的根因链: 路由的 mmu_gather 并发重叠 → `mm->tlb_flush_pending > 1` →
`tlb_finish_mmu()` 读 `mm_tlb_flush_nested()` 强制 `fullmm=1; freed_tables=1`
（mm/mmu_gather.c）→ 即使本窗零 PTE 清除也发全 mm IPI → IPI 延迟拉长 gather 存活窗
→ 重叠概率进一步上升（自持风暴）。mmbench unmap-virt 每 op = 1 次 chunk munmap 路由 +
1 次 MAP_FIXED refill mark 路由 = 每 op 2 个 gather, 4 线程下 pending>1 近乎恒真。

### 1.4 mmap-pf low t1（#83, 单线程无竞争纯每-op 成本）

T0 0.01738 ops/µs vs BASE 0.02686 = 1.55×。T0 flat: **corten_arena_zap_window 23.4%**
（EXACT munmap → T1c park → zap 整个 2M 帧 = 512 槽 reset, 而 op 只有 4 页内容）+
元数据层（corten_txn_meta 6.1 + xas_load 4.7 + corten_slot_index 3.4 + corten_query 2.9
≈17%）+ check_empty_locked 2.0%（take 侧 512 PTE 复扫）。

## 2. 优化点（按 perf 证据排序; 各自独立可回退, 失败尝试如实记录）

### perf1a-v1（mmap_write 序列化 chunk gather）——**已回滚, 记录在案**

假设: gather 互斥（全路由持 mmap_write）→ pending≤1 → 嵌套强制 flush 消失。
结果: unmap-virt t4 修好（0.33×→3.0×）**但 unmap t8 的 +113% 胜被毁成 -56%**
（0.00315→0.00070）——touched 形态的并行 flush 是 T0 在 unmap 上反超 legacy 的来源,
write 锁内串行化 IPI 等待是错误交易。完整回滚, 代码不在 commit 里。

### perf1a-v2（**lazy per-window gather**, 最终采纳）

zap_window/zap_untracked_window 在 PTE 锁下先扫窗: 零 !none PTE → 元数据-only 路径,
**不开 gather**（pending 不增 → 嵌套不可能 → 零 flush）; 有内容 → 行为同旧（ranged
flush, folio_put 在 flush 后）。扫描对一切 PTE 生产者免竞争（覆盖 desc 写锁排除 fault
路径与其它事务 zap; 已跟踪窗 legacy funnel 不可写）。r03 defect C 语义保持: 扫描按
PTE 内容判定, 元数据-only 分支仅在"全窗证明无 PTE"时走。

### perf1b（**park 元数据整块释放**, corten_txn_meta_drop）

EXACT munmap → T1c park 欠"全槽 pristine (Invalid/perm-0)"。旧路径逐槽
corten_unmap = 每窗 512 次 query, 而 16KB op 只有 ~4 槽有过内容。新路径: 在 desc 写锁
（事务内）整块 `kfree` 元数据数组; `corten_query()` 的 meta==NULL 分支给出**逐字节相同**
的 pristine 答案, 下次 mark 经 meta_ensure 重建（GFP_NOWAIT, 失败恢复路径既有）。
UNMAP_PAGES 计数保持精确（释放前快扫计数）。**仅在 walk 完全成功时释放**——中途 -EAGAIN
会留活 PTE, "活 PTE 无元数据"正是 r03 defect C 形状, 该路径由调用方真 RELEASE 兜底。
反例护栏: KEEP_PERM（chunk drop / mprotect 契约存活）语义不受影响, 仍逐槽。

### perf1c（**park flush 后置到 write→read downgrade**）

park 的 shootdown 等待原来在 mmap_write 临界区内（8 线程串行化各自 IPI 等待 = t4/t8
残差主因）。改为: 调用方 pool_release 开 route 级 gather → park zap（写锁内, 排除
一切事务）→ park 簿记 → **ctl_unlock → mmap_write_downgrade → tlb_finish_mmu →
read_unlock** —— 与 legacy `vms_complete_munmap_vmas()` 完全同位。正确性: downgrade
后 pool take 需写锁 ⇒ 被钉住的窗在旧 TLB 射完成前不可能被复热; 失败路径同一 gather
在真 RELEASE 后收尾（部分 zap 的页同样 flush 后才释放）。

逐构建前后数字（5s 快验单发, ops/µs, T0 vs BASE 同 boot）:

| 构建 | unmap-virt t4 | unmap t8 | mmap-pf t8 | 备注 |
|---|---|---|---|---|
| #83 基线 (run3) | 0.0120 / 0.0504 | 0.00315 / 0.00148 | 0.00033 / 0.00118 | run3 同源 |
| #84 perf1a-v1 | 0.163 / 0.055 | **0.00070 / 0.00158** | — | v1 毁 unmap → 回滚 |
| #85 perf1a-v2 | **0.791 / 0.061** | 0.00348 / 0.00159 | 0.00037 / 0.00120 | 风暴消除 |
| #86 +perf1b | 0.725 / 0.058 | 0.00354 / 0.00165 | 0.00037 / 0.00133 | t1 1.55×→1.18× |
| #87 +perf1c | 0.685 / 0.052 | 0.00346 / 0.00158 | **0.00108 / 0.00120** | write 段 IPI 移出 |
| #88 终验 | 0.702 / 0.054 | 0.00353 / 0.00142 | 0.00114 / 0.00133 | = #87+checkpatch 空白 |

## 3. 正确性面

- **KUnit on 全套 ×2**（#87 与终验 #88, 无盘 boot `kunit.filter_glob=corten*` corten=on）:
  **85 ok / 0 not-ok**, 两条 WARNING（corten.c:864/811 txn_begin 注入设计探针）=
  m5t1a kunit-on1-final18.log 基线同形同址（Tainted W 基线既有）。
- guest 正确性探针（#87, 2.1M 次 park 后）: dmesg corten warn/bug/oops = 0;
  `munmap_releases == pool_parks == 2,100,181`（精确对账）; pool_hits 2,100,164;
  **meta_arrays == ptdescs == 317**（perf1b 释放/重建平衡, 零泄漏）; auto_fallbacks 0,
  drain_timeout 0。
- checkpatch --strict: **0 errors / 0 warnings / 0 checks**（536 行 diff）。
- 语义零变化论证: 三点全部是"何时开/收 gather"与"以等价表示承载 pristine 态"的
  实现层变化; flush 顺序语义（folio_put 在 flush 后）不变; KEEP_PERM 契约路径逐槽
  语义未动; KUnit 全套锚未改一例。

## 4. 机制成本结论（M8 诚实素材）

**能消的（本轮已消）**: ①PTE-less 窗上的 gather 与嵌套强制全 mm flush（unmap-virt
风暴, ~13× 形态逆转）; ②park 的 O(512) 逐槽 reset（整块释放等价）; ③write 锁内的
flush 等待（后置 downgrade, legacy 同位）。

**不能消（当前设计粒度下的诚实成本）**:
1. **mmap-pf t8 残差 -14.3/-15.5%**: 拆解 = 纯 fault 通道 **-7.1~-10.9%**（pf 格,
   两轮网格一致; 每 fault 一次事务: fill_upper + lock_range(desc rwlock write, BH) +
   txn_owned xa_load + query + mark —— 这是论文 Fig.5/8 的 per-fault transaction 语义
   本身）+ take/park 簿记 ~3-5%（parkable xa 校验、check_empty 512-PTE 复扫 =
   parked 窗 FOLL_FORCE 可装 PTE 的防线, 不可省）。终态 profile 两臂同形
   （x2apic 33% / 唤醒 23% / 上下文切换 19% vs BASE 28.5/29/20）, corten 侧仅
   zap_window 1.84% 在榜 = 差异已进入 IPI/调度噪声地板。
2. **T1c 池生命周期每 op 两次 mmap_write 段**（take + park）: 与 legacy mmap/munmap
   各一两次同量, 但临界区含路由工作; 进一步压缩需要把 mark 改批处理（4 页一事务）,
   属 M8 fault 域重构, 不是小 diff。
3. **pf/low 家族高方差**（登记 CV 15~79%）: 单发 5s 窗在 -21%~+29% 间摆动, 网格
   中位收敛在 -7~-11%; 与 run3 的 +29.3% (CV 54/79) 不可靠区分, 以网格口径登记。

**失败尝试记录**: perf1a-v1（write 锁序列化 gather）修复 unmap-virt 但把 unmap t8
+113% 打成 -56%, 已完整回滚 —— 结论: touched 形态的 flush 并行性是 T0 在 unmap 格
反超 legacy 的第一来源, 任何"全路由 gather 互斥"设计都不可取; 最终方案用"无内容不开
gather"达成同一 pending≤1 效果且保留并行。

## 5. 证据清单

- commit `802ff7551bd0`（3 文件 +270/-58）, tag **corten-r06-perf1**; diff 存档
  `results/r06/perf1.diff`。
- bzimg/r06-perf1/: #84（v1 失败尝试）/ #85（v2）/ #86（+1b）/ #87（+1c, 两轮网格跑在
  此二进制）/ **#88 final**（=commit 源码, KUnit 终验+spot check）+ SHA256SUMS。
- guest 剖景与网格: `results/r06/perf1/`（abab/, abab2/ 全 JSON 行, report_*.txt 两臂
  flat profile, mmb_*.out 原始输出）; perf .data 留 guest /root/perf1（vm-perf1 留运行,
  端口 10026）。
- KUnit: results/r06/kunit-perf1-on1.log（#87）、kunit-perf1-final.log（#88, 85/0/0）。
- green.txt: 追加 r06-perf1 条目。

—— perf1 收工: 三格判定 **unmap-virt 大胜 / unmap 胜扩大 / mmap-pf 残差 -14~-15% =
fault 通道机制成本（纯通道 ≤11%, 路由簿记 ~3-5%）**, KUnit 全绿, dmesg 零 splat,
2.1M 次 park 对账精确。G1 剩余缺口从"大负 2-4×"压缩到"一位数百分比", 且每项残余均有
归因与不可消除性论证。
