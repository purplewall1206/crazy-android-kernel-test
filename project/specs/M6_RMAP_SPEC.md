# M6_RMAP_SPEC · CortenMM rmap/swap/回收对接设计规格（M6）
创建: 2026-09-19（规划者班, 只读审计 + 本文档; 基座 = 主树 HEAD 1f8dfc78ae9f,
tag corten-r07-m5t1b, 主树第 21 个项目提交, 6.18）
> 规范关系: docs/PAPER_SPEC.md（PS-x）为论文唯一权威; 本文对 docs/DESIGN.md §6.2
> （v2 chunk-VMA 分阶段 rmap 设计）逐条对照, 冲突处标 **[以实现为准]** 或给理由。
> 本文所有 "6.18 事实" 断言均带 file:line（除特别注明外 mm/ 下均相对
> /home/ppw/linux-6.18）。规模参考: M6.T1-T5 合计预估 ~1000-1500 行 diff。

---

## 1. 现状审计（HEAD 1f8dfc78ae9f 实测）

### 1.1 arena 页的真实生命周期（生产者 → 消费者）

**生产（进 arena）** — MODE auto-mmap: `do_mmap` 路由（mmap.c:428/491）→
`corten_arena_auto_attach`（mmap.c:660, arena.c:2052）→ 纯 mark(PrivateAnon)。
fault: arch 快门（arch/x86/mm/fault.c:1362-1364）或 handle_mm_fault 慢门
（memory.c:6561-6580）→ `corten_arena_fault_once`（arena.c:3995）→
`corten_arena_map_anon`（arena.c:3379）:
- 预分配: `vma_alloc_zeroed_movable_folio` + `mem_cgroup_charge` +
  `folio_throttle_swaprate`（arena.c:3088/3092/3097）→ **arena 页已计 memcg**;
- rmap: `folio_add_new_anon_rmap(folio, vma, addr, RMAP_EXCLUSIVE)`
  （arena.c:3473）。**arena 页有完整匿名 rmap**, 锚进 shadow-VMA 的 anon_vma
  （DECLARE 时 `anon_vma_prepare`, arena.c:478）。上游该函数对非 swapbacked 页
  置 `__folio_set_swapbacked`（rmap.c:1535）→ arena 页 = swapbacked 匿名页;
- **不进 LRU**: "no folio_add_lru_vma() — arena pages stay off the LRU so
  reclaim/migration can never write to them outside a transaction"（arena.c:3474-3477,
  即规划者问的 M3b 注记原文）。上游对照 do_anonymous_page() 的
  folio_add_lru_vma 被**故意省略**——这是当前唯一把 arena 页挡在回收之外的闸门;
- PTE 装 + 同事务 `corten_map`（arena.c:3479/3496）→ meta=CORTEN_MAPPED。

**消费（出 arena）** — 全部经事务:
- munmap/chunk/punch/DONTNEED: `corten_arena_zap_window`（arena.c:4553）在
  covering desc 写锁 + ptl 下 `ptep_get_and_clear` + `folio_remove_rmap_pte` +
  `__tlb_remove_page_size`（arena.c:4676/4703/4712）+ `corten_unmap` 元数据复位;
- mprotect 路由: `corten_arena_protect_window`（arena.c:6144）, meta 先行 + PTE
  `ptep_modify_prot_start/commit`（arena.c:6222/6232）+ mmu_notifier 包裹
  （arena.c:6273-6275）;
- fork: `copy_page_range` 照常 wrprotect = glue 白名单第 1 处（mmap.c:1893-1902,
  STATE D17/DEV-14）+ fork_begin/commit 事务镜像;
- exit: `exit_mmap` 先 `corten_arena_mm_exit` 排空事务（mmap.c:1400-1407,
  arena.c:1310）再走 legacy unmap_vmas 顺序拆表（无并发, 元数据随 state 消亡,
  记为已验收 carve-out）。

**Swapped 状态**: 枚举与编码位已备（corten.h:102 `CORTEN_SWAPPED`,
corten.h:187 `__resv[5]` "swap (dev, blk, off) payload"）, 但**无任何生产者**:
dispatch 对 CORTEN_SWAPPED 返回 STUB（arena.c:3150 "M6 producer: unreachable"）,
真链路撞上即 `WARN_ONCE` + MAPERR（arena.c:4178-4183）。mprotect 对 Swapped 槽
"STUB-family states are left alone"（arena.c:6176-6180）; zap 假定
"swap entries cannot exist in M3"（arena.c:4682-4684）; mprotect 对
!pte_present 直接跳过（arena.c:6240-6242）。

### 1.2 逐路径行为表（M6 前）

| # | 路径 | 入口（6.18 事实） | 对 arena 页的当前行为 | 风险 |
|---|---|---|---|---|
| P1 | kswapd / direct reclaim / memcg reclaim（classic+MGLRU 求同） | `shrink_folio_list`（vmscan.c:1139）, folio 只能来自 `isolate_lru_folios`（vmscan.c:1822） | **结构性不可达**: arena 页不在任何 LRU（缺 folio_add_lru, arena.c:3474）→ 永不隔离/永不 swap out | 正确但**代价**: 压力下 arena 页只认 OOM（DEV-10 wired 妥协） |
| P2 | MGLRU aging（walk_mm） | `walk_pte_range`（vmscan.c:3667）, `get_pfn_folio`（vmscan.c:3618） | `folio_lru_gen(folio) < 0 → return NULL`（vmscan.c:3623-3624）→ 连 young 位都不清（`ptep_clear_young_notify` 在 vmscan.c:3717, 前置了 get_pfn_folio 判空） | **无红线风险**（连 PTE 都不写）; 但注意 DESIGN §6.2 称 MGLRU aging "只读统计不改 PTE"是**错的**——aging 对 LRU 页会清 young 位（vmscan.c:3717）, 将来接入必须走事务 |
| P3 | MGLRU eviction | `lru_gen_shrink_lruvec`（vmscan.c:5210）, 只扫 `folio_lru_gen>=0`（vmscan.c:3623/4612） | 结构性不可达（同 P1） | 同 P1 |
| P4 | MADV_PAGEOUT / process_madvise(PAGEOUT) | `corten_arena_madvise_route` 默认臂（arena.c:6949 `range_overlaps → -EOPNOTSUPP`）; PAGEOUT 不在 hints 白名单（arena.c:6913-6916） | **已拒绝**（PAGEOUT 唯一不依赖 LRU 的回收入口被路由闸死） | 无 |
| P5 | OOM killer / oom_reaper | `__oom_reap_task_mm`（oom_kill.c:541）遍历全部匿名 VMA（含 shadow-VMA）→ **直接 `unmap_page_range`**（oom_kill.c:590） | **红线违规（今日可触发, 违规清单 V1）**: zap_pte_range 裸写 arena PTE + 摘 rmap + folio_put, 不持 desc 写锁、不更新元数据; 与在飞事务仅靠 ptl 串行 → 幸存形状 = meta=CORTEN_MAPPED 而 PTE none → 下次 fault `restore_pte` `WARN_ON_ONCE(1)`（arena.c:3591-3597）; 受害进程本在 SIGKILL 中, 无 UAF（restore 的读-改-写在同一 ptl 临界区内）, 但 INV6/INV7 被破 + 假 WARN | **P0**, T1 修 |
| P6 | try_to_unmap（swap out 的 PTE 写步） | `try_to_unmap`（rmap.c:2267）→ `try_to_unmap_one`（rmap.c:1860）; swap entry 编码落 PTE 在 rmap.c:2201; 摘 rmap + folio_put 在 rmap.c:2220/2224 | **潜伏违规（今日不可达, 违规清单 V2）**: anon_vma 间隔树能经 shadow-VMA 找到 arena PTE（folio_add_new_anon_rmap 已锚, arena.c:3473）, 一旦任何调用方把 arena folio 递进来就会裸写 PTE | T1 守卫: 守卫点 + 事务慢路径 |
| P7 | try_to_migrate（NUMA misfit / migrate_pages 内核路径） | `try_to_migrate_one`（rmap.c:2288, 迁移项 PTE rmap.c:2567） | 同 V2 潜伏; 但内核迁移的隔离端全部要求 folio 在 LRU: compaction `!folio_test_lru → skip`（compaction.c:1101）, hotplug `scan_movable_pages` 同检（memory_hotplug.c:1833）→ 今日不可达 | T1 守卫同点拒绝（迁移语义 Stage1 不做） |
| P8 | migrate_pages/move_pages 系统调用 | migrate.c:2650 `nodes && corten_arena_range_overlaps(mm,0,TASK_SIZE) → -EOPNOTSUPP` | **已拒绝**（M4T0 审计 #9 钩子在） | 无 |
| P9 | NUMA balancing | `task_numa_work` → `change_prot_numa`（fair.c:3629） | **违规清单 V3（可触发, 已被接受的 legacy writer）**: 对 shadow-VMA 整段 protnone 裸写 PTE; arena 自愈 = meta perm 照读重建（arena.c:3560-3567 注记明说 "change_prot_numa() is an unhooked, accepted legacy writer"）。guest 单节点时惰性 | T1 决策: 计数维持 or fair.c 加 VM_CORTEN skip（OQ-M6-5） |
| P10 | THP / mTHP / khugepaged | `hugepage_vma_check` 拒 `VM_NOHUGEPAGE`（include/linux/huge_mm.h:335）; anon fault mTHP 走 `thp_vma_allowable_orders`（memory.c:4530/5093）; khugepaged 同门 | **已排除**: shadowize 置 `VM_CORTEN|VM_NOHUGEPAGE`（arena.c:486）; 防御性 `pmd_leaf` 探测回退（arena.c:3257-3273）; map_anon `WARN_ON_ONCE(VM_HUGEPAGE|VM_HUGETLB)`（arena.c:3402）。**结论: VM_NOHUGEPAGE 一把锁够**（单个 bit 同时被 fault-THP/mTHP/khugepaged/madvise COLLAPSE 检查; COLLAPSE 另被 arena.c:6949 拒） | 无 |
| P11 | hwpoison | `hwpoison_user_mappings` 先调锚（memory-failure.c:1595）→ `corten_arena_hwpoison_check` WARN（arena.c:6958-6984） | 大声拒绝 + 记录（文档化限制, "counted for M6"） | T1 维持; T2 后可评估真 unmap 路由 |
| P12 | swapoff（unuse_vma 遍历） | unuse 只对 swap PTE 动作 | 今日无 swap PTE → no-op | T2 需复测 |
| P13 | GUP fast/slow + pin | fast 走普通 PTE; slow `check_vma_flags` `corten_own` 门（gup.c:1234）, 写 GUP 经 fault 门 | 正常; COW 拒 DMA-pinned 复用（arena.c:3714/3895）; munmap zap 摘 PTE 后 folio 由 pin 引用存活（INV8 天然成立） | T2: swap PTE 出现后 fast GUP 天然跳过（!present）, slow 走 fault 门换入 |
| P14 | userfaultfd / KSM | shadowize 白名单拒 VM_UFFD_*（arena.c:535）; MADV_MERGEABLE 被默认臂拒（arena.c:6949） | 已拒绝（OQ3/OQ4 维持） | 无 |

### 1.3 红线违规清单（MASTER_PROMPT §7 地雷 2 / DESIGN §3 真源纪律 INV6）

- **V1（可触发, P0）**: oom_reaper `unmap_page_range`（oom_kill.c:590）——
  不经事务写 arena PTE、摘 rmap、丢 folio 引用, 元数据不变（INV6 破; INV7 破
  → arena.c:3591-3597 假 WARN）。压测触发条件: memcg/global OOM + 受害进程含
  arena（MODE 进程天然满足）。
- **V2（潜伏, P1）**: try_to_unmap_one / try_to_migrate_one 的 PTE 写
  （rmap.c:2201/2567）。今日靠"不进 LRU"单点隔离（arena.c:3474）; **任何未来
  代码补一个 folio_add_lru（迁移目标/回收接入/调试钩子）都会静默引爆**——这是
  M6.T1 守卫存在的理由: 把"结构上够不着"升级为"够得着也走事务"。
- **V3（可触发, P2, 已文档化接受）**: change_prot_numa（fair.c:3629）。维持
  自愈语义 + restore 计数可观测; 处置见 OQ-M6-5。
- 非违规的已收口胶水: fork wrprotect（白名单 1/≤3, mmap.c:1893）; exit 拆表
  （顺序性 carve-out, mmap.c:1400-1407）。

### 1.4 swap 可换出性的直接回答（规划者问题 3）

"M6 前 arena 页被 swap 吗? —— 不会。" 链条: swap out 唯一驱动 = 
`shrink_folio_list`（vmscan.c:1139; `folio_alloc_swap` 在 swapfile.c:1441,
writeback 在 page_io.c:379/416/437）, 其输入只能来自 LRU 隔离（vmscan.c:1822）;
旁路入口 PAGEOUT 被路由拒绝（arena.c:6949）。故 Swapped 无生产者、fault 的
STUB WARN（arena.c:4179）不可达。同时**arena 页是"完整可换出形态"**: swapbacked
（rmap.c:1535）+ anon rmap（arena.c:3473）+ MM_ANONPAGES 计账（arena.c:3472）
+ memcg 已计费（arena.c:3092）——缺的只是 LRU 门槛与事务化回收通道, 即 M6 的
全部工作面。

---

## 2. 分阶段设计（对齐 DESIGN §6.2, 以实现为准修订）

### 2.0 与 DESIGN §6.2 的对照结论

| DESIGN §6.2 原文 | 本规格裁定 |
|---|---|
| Stage1(wired, 不入 LRU, memcg 走 shadow), DEV-10 | **维持**（与 §1.1 实测一致; memcg 事实计费在 folio 上, arena.c:3092, shadow-VMA 只承载可见性） |
| Stage2 chunk-VMA(2M 首触建档) 提供 rmap/MGLRU **可见性** | **[以实现为准] 作废 "chunk-VMA" 器件**: shadow-VMA 已是 Linux 侧可见性载体——anon rmap 锚定（arena.c:3473+478）、page_vma_mapped_walk 可达、proc/smaps 照走（task_mmu.c:1065/1157 无需 corten 钩子）。rmap 找页环节不再需要新 VMA 形状（DEV-2 的借道对象从"chunk-VMA"改记"shadow-VMA"）。Stage2 改义为**性能化: 2M 窗批量事务**（§2.2） |
| "MGLRU aging 走 chunk-VMA 的 mm_walk(只读统计, 不改 PTE, 无事务需求)" | **[以实现为准+勘误] 两处**: ①可见性载体是 shadow-VMA; ②aging 并非只读——清 young 位是 PTE 写（vmscan.c:3717）。故 MGLRU 接入=结构 skip 维持 + 自有 aging 事务（§2.1 T3）, 不改 lru_gen walker |
| 换出: Swapped 入 meta(BlockDev,BlockNum,Perm); 换入 fault 走事务+标准 swap cache | **维持**, 落地细节 §2.1 T2 |
| try_to_unmap/迁移/换出对 arena 页 PTE 写经事务（PS-B5） | **维持**, 守卫点设计 §2.1 T1 |

### 2.1 Stage 1（wired）——最小正确

目标: ①V1/V2 收口（回收/rmap 路径对 arena 页"够得着就走事务"）; ②建立
**压力可达的** arena 换出/换入闭环（不进 LRU 前提下, 经 shrinker——见下）;
③kswapd 直返路径对 arena 零行为变化。**红线不变式**: arena 页永不上 LRU
（M6.T1 DoD, ROADMAP"压力下 arena 页不进 LRU"）。

**D1. rmap 守卫 + 事务慢路径（修 V2）**

在 `try_to_unmap_one` / `try_to_migrate_one` 的 walk 入口（rmap.c:1908
`while (page_vma_mapped_walk(&pvmw))` 之前、mmu_notifier_range_start 之后,
rmap.c:1893-1898）加守卫:

```c
/* rmap.c, 两个 _one() 各一处 */
if (corten_enabled_static() && (vma->vm_flags & VM_CORTEN)) {
	if (corten_rmap_unmap_one(folio, vma, address, flags))
		goto walk_done;   /* 事务内完成: 该 VMA 侧 unmap/换出 */
	goto walk_abort;      /* 迁移/hwpoison 形状 Stage1 拒绝 */
}
```

新接口（mm/corten_arena.c）:

```c
/* 返回 true=已事务化 unmap 本 VMA 内该 folio 的全部 PTE; false=拒绝(调用方 abort)。
 * 前置: 调用方持有 folio lock 与引用, mmu_notifier 已 start。
 * 锁序: folio_lock(调用方) > desc->lock(W,BH) > ptl —— 与 DEV-13 同向, 无反转。 */
bool corten_rmap_unmap_one(struct folio *folio, struct vm_area_struct *vma,
			   unsigned long address, enum ttu_flags flags);
```

语义（plain unmap 形状, 即 TTU_UNMAP 家族、非 TTU_HWPOISON/迁移）:
1. `corten_lock_range(mm, addr, PAGE_SIZE)`（复用 fault 同款 retry/-EAGAIN 语义,
   arena.c:4014-4044 先例）→ desc 写锁;
2. `pte_offset_map_lock` → 校验 PTE present 且 PFN==folio（mismatch=abort）;
3. `folio_alloc_swap(entry)`（swapfile.c:1441 同款, 须在取 desc 锁**前**做完
   ——见锁序 D4; 实际实现把 swap 分配/入 cache 提到第 1 步之前）→
   `swap_cache_add_folio`（swap_state.c:137）;
4. 事务内: `ptep_get_and_clear` → `swap_duplicate` 语义由 entry 安装承担 →
   `set_pte_at(swp_entry_to_pte(entry))`（镜像上游 rmap.c:2148-2201 的编码:
   swp_exclusive/soft_dirty 位）; MM_ANONPAGES-1, MM_SWAPENTS+1;
   `folio_remove_rmap_ptes`（rmap.c:2220 同款）; TLB 按 TTU flags 走
   `should_defer_flush`（rmap.c:699/2055）或 `flush_tlb_page`;
5. 同事务 `corten_map` 不用——新事务接口 `corten_swap_out(txn, addr, entry,
   perm)`: meta 置 `CORTEN_SWAPPED`, `__resv` 编码 entry;
6. `corten_unlock` 后 `folio_put_refs`（rmap.c:2224 顺序）。

拒绝臂: TTU_HWPOISON、迁移项形状 → false（上游 walk_abort → folio 留驻,
hwpoison 维持 WARN 锚, arena.c:6958）。注意: 守卫路径的 **ptl 嵌套方向**与
fault 相同（desc→ptl）, 因为守卫在进入 page_vma_mapped_walk **之前**整体接管,
不带着上游已持有的 ptl 进事务——这是设计成立的唯一形态; 若改在 walk 循环内
逐 PTE 接管会形成 ptl→desc 反向边（死锁）, 明令禁止。

**D2. 压力路径 = shrinker（修 P1 的代价, 实现阶段 1 的换出通道）**

非 LRU 内存的 Linux 原生回收通道是 shrinker（kswapd/direct reclaim 均走
`shrink_slab`）。注册全局 `register_shrinker(&corten_shrinker, "corten-arena")`:

```c
unsigned long corten_shrink_count(struct shrinker *s, struct shrink_control *sc);
unsigned long corten_shrink_scan (struct shrinker *s, struct shrink_control *sc);
/* count = MODE mm 集合内 arena 常驻页数(粗粒度, per-mm 缓存);
 * scan  = 对 nr_to_scan 个受害者执行 D1 的安装序列, folio_trylock 竞争失败即跳过。*/
```

victim 选择器（T3 完成 aging 前用简化策略: 窗口粒度轮转, per-arena 游标;
两遍 young 位 aging 见 T3）。mm 枚举: 新增全局 `corten_mm_registry`
（spinlock 保护的双链表, 节点挂 `corten_mm_state`, enter/exit 时登记/注销;
exit 已有单一收口 mmap.c:1407）。memcg 维度: scan 尊重 sc->memcg（对非目标
memcg 的 mm 跳过, 用 mm->corten_state 内记录的 memcg 指针比对）。备选方案
"arena 页挂 unevictable LRU（mlock 式）以获得计数/统计"**否决**: 违反 T1
DoD 且 lru 锚点会引来 P2/P3 的 walker（见风险 R3）。

**D3. V1（oom_reaper）修法**: fault 门加 MMF_UNSTABLE 拒绝——reaper 在
unmap 前置位 MMF_UNSTABLE（oom_kill.c:552）, 在 `corten_arena_lookup_get`
（arena.c:2917）失败返回 NULL（=legacy fallback, 与 RELEASE 竞争同款）:
新事务从此不再与 reaper 并发; 在飞事务与 reaper 的残余竞争维持"ptl 串行、
受害进程将死、最坏假 WARN"的记录口径（§1.2 P5）。不改 oom_kill.c 的 unmap
本体（给受害进程保留释放 arena 内存的通道——这正是 reaper 的目的）。

**D4. 锁序（DEV-13 序列扩展, 本规格新增边, 全部与既有边同向）**

```
mmap_lock(W) > ctl_lock > drain-wait > [folio_lock] > [swap cluster/xa_lock]
    > fill_lock > desc->lock(W, BH) > ptl
```
- **folio_lock 嵌在 desc->lock 之外（先取后放）**: rmap 慢路径上游本来就持
  folio lock（try_to_unmap 调用方约定, vmscan/swapcache 同）; 我们自己的
  shrinker 也 folio_trylock 在前。反向边（desc→folio_lock）禁止——COW/fault
  均只在 ptl 内读 mapcount、从不持 desc 锁取 folio lock（arena.c:3673 注记
  R2 同理外推）。
- **swap 锁（cluster_lock / swap_cache xa）也嵌在 desc->lock 之外**: 换出的
  entry 分配与入 cache 全部在 `corten_lock_range` 之前完成（D1 第 3 步前置）,
  事务内只剩 PTE/meta 写 + swap_duplicate/swap_free 原子量 → 事务内零新锁类,
  INV3（desc 锁 BH 对称, 无睡眠/无分配）不破。
- 换入（D5）同形: I/O 与 cache 插入在锁外, 事务内只装 PTE。

**D5. 换入（fault 反向）**: dispatch 新增 `CORTEN_DISP_SWAPIN`（corten.h
枚举 + 纯函数 switch, arena.c:3122）:

```
fault_once: query 得 CORTEN_SWAPPED → 读 __resv 得 entry →
  解锁(corten_unlock) → 锁外准备: swap_cache_get_folio(entry)（swap_state.c:88）
  未命中则 swapin_readahead（swap_state.c:825）+ wait_on_folio（可睡眠, 锁外）→
  重锁 → re-query: entry 变了→retry（图7 语义）; PTE 已非 none→re-dispatch →
  事务内: ptl 下 set_ptes(真页, meta perm 编码) + folio_add_new_anon_rmap
  (RMAP_EXCLUSIVE; 上游 do_swap_page 的 exclusive 判定按 meta.SHARED==0 简化)
  + MM_ANONPAGES+1/MM_SWAPENTS-1 + corten_map(perm, 0)（Swapped→Mapped 合法,
  corten.h:396-398 状态机已许）→ 解锁后 swap_free(entry)。
```
retry 预算: `fault_once` 现有 2 次循环（arena.c:4294 区, handle_mm_fault 同
arena.c:4405-4408）对 SWAPIN 形状放宽为 2+2（解锁/重锁各占一拍）, 计数器
可观测。用户判据: 换入后 checksum 与换出前一致。

**D6. Swapped 编码（__resv[5]）**: `resv[0]=swap type（u8, MAX_SWAPFILES
上限内; zram=一个 type）; resv[1..4]=offset（u32, LE）`。perm 沿 meta.perm
（论文 Swapped(BlockDev,BlockNum,Perm) 的 Perm 位）; zram 即块设备=BlockDev
语义由 Linux swap type 抽象承载。容量核算: 4G zram/4K=1M 槽 ≪ 2^32。
INV7 checker 扩展（T2）: `meta.state==CORTEN_SWAPPED ⇒ PTE 非present且
pte_to_swp_entry(PTE)==__resv 解码`; `PTE none ⇒ meta.state != CORTEN_SWAPPED`。

**D7. 既有消费面的 Swapped 扩展（T2 必改清单, 全在 mm/corten_arena.c）**:
- `corten_arena_zap_window`（arena.c:4642-4741）: 非 present 非 none 的
  swap PTE 分支 → swap_free + MM_SWAPENTS-1 + `corten_unmap`（KEEP_PERM 语义
  不变）; 更新 4682 注记;
- `corten_arena_protect_window`（arena.c:6177-6201/6240-6242）: recorded 白名单
  加 CORTEN_SWAPPED（perm 改写纯 meta; swap PTE 无 perm 位, 无硬件写）;
- `corten_arena_unmap_chunk`/park 全清路径（arena.c:4998/5249, zflags=0 走
  `corten_txn_meta_drop` 整数组丢弃）: 依赖 zap 先 free swap PTE 计数, 顺序
  已被"先 PTE 后 meta"保证;
- fork: `copy_nonpresent_pte`（memory.c:932/1287）天然复制 swap PTE +
  swap_duplicate; meta 镜像拷贝 __resv → T2 增一致性 KUnit;
- mremap grow 走 copy_to_user → fault 换入天然支持（arena.c:6592-6596）,
  shrink 尾 zap 依赖 zap 扩展; move 路径维持拒绝（arena.c:6513-6516）;
- GUP: fast 跳过 !present（无改）; slow→fault 门→D5。

**KUnit 锚（Stage1）**: ①dispatch SWAPIN 分类器纯测; ②守卫拒绝臂
（HWPOISON/迁移 flags → false）; ③编码 roundtrip（entry→__resv→entry,
含 type 边界）; ④MMF_UNSTABLE 门（置位后 lookup_get 返回 NULL）; ⑤zap 遇
swap PTE 的计数/swap_free（合成 swap entry, 不真写盘）。真盘 zram 往返是
guest 判据不进 KUnit。

**Guest 判据（Stage1 出口）**: ①memory pressure（cgroup memory.max 压
MODE 进程）→ shrink_slab 计数>0、swap.so 增长、RSS 下降、无 WARN/无 INV7
漂移、`/proc/pid/smaps` 的 Swap 字段非零（task_mmu.c:1065 对 swap PTE 照常
统计, 无需钩子）; ②kswapd 回归: classic/MGLRU 扫描路径对 arena 页零触碰
（rss 不被 LRU 路径动过, checksum 一致）; ③OOM 复现实验: 受害进程 reap 后
不出现 arena.c:3591 假 WARN。

### 2.2 Stage 2（2M 窗批量, 性能化）——**[以实现为准] 重定义**

原 §6.2 Stage2 的"建档给可见性"已由 shadow-VMA 承担（§2.0）; 本 Stage2 =
**批量与局部性**:
- **批量事务**: `corten_txn` 升级为迭代器（corten.h:344-346 "M4+ turns the
  cursor into an iterator" 的既定方向）, shrinker 以 2M 窗为单位一次
  `corten_lock_range(win)` 内整窗 victim 处理（一次 desc 写锁摊 N 个
  swap 安装）; munmap/protect 已是整窗事务, 对齐其形状（arena.c:6277-6340）;
- **批量 unmap/mark**: 换出按 2M 窗聚集 entry 分配（folio_alloc_swap 的
  聚簇分配利好 zram 顺序压缩）; aging 也整窗两遍化;
- **延迟测量**: 与 Stage1 相比 unmap/unmap-virt 微基准与 swap 吞吐回归
  （M1 同参）, 目标: swap 路径每页 desc 锁获取次数 ≤1。
- 接口/锁序/KUnit/guest 判据与 Stage1 同型, 切片 T5 覆盖。
- 明确**不做**（后续切片/OQ）: 共享（SHARED）页换出（OQ-M6-2）、迁移互操作
  （OQ-M6-3）、LATR、LRU 接入反转。

---

## 3. 红线核对表（评审逐条打勾用）

| # | 红线（出处） | M6 执行 | 核对方法 |
|---|---|---|---|
| 1 | rmap/回收对 arena PTE 写必经事务（PS-B5 论文 §4.5 原文; DESIGN §3.1; MASTER_PROMPT §7 地雷 2） | V2 守卫 =rmap.c 两 `_one()` 入口（§2.1 D1）; V1 =MMF_UNSTABLE 门（D3）; V3 =接受+计数（OQ-M6-5）; shrinker 换出自身在事务内（D1 步骤 4-5） | INV6 评审: 新增 PTE 写点全量列表 = rmap.c 守卫调用 + arena.c D5/D7; INV7 checker 运行期零漂移 |
| 2 | Swapped entry 与 meta 一致性（PS-B2 唯一真源） | `__resv` 编码 D6; 写点仅在 desc 写锁内; INV7 扩展双向断言（D6） | KUnit 编码锚 + guest INV7 checker 零漂移 |
| 3 | GUP pin 页不换出/不丢 | shrinker victim 先 `folio_trylock` + `folio_maybe_dma_pinned` 拒绝（对齐 vmscan.c:1364/1480 的上游语义）; D1 拒绝臂不碰 pin 形状; munmap zap 摘 PTE 后 folio 由 pin 存活（INV8, §1.2 P13）。**勘误**: 规划简报称 "M5.T3 zap_pinned 已有"——树上无 zap_pinned 符号（grep 全 corten 文件为零）, 实际已有的 pin 纪律是 COW 的 `folio_maybe_dma_pinned` 拒复用（arena.c:3714/3895）, M6 复用的是这两处语义而非 zap_pinned 函数 | guest io_uring/9p pin + 压力换出并存测试; KUnit pin 拒绝臂 |
| 4 | desc 锁 BH 对称、锁内无睡眠无分配（include/linux/corten.h:520-532; arena.c:3077-3080） | D1/D5 全部 I/O、分配、folio 操作在锁外; 事务内只 PTE/meta | 评审 + lockdep 构建回归（M7 件） |
| 5 | DEV-13 锁序无反向边（STATE D13） | D4 新边同向; 禁止 walk 循环内带 ptl 进事务（D1 注） | lockdep + KUnit 嵌套用例 |
| 6 | =n/off 折叠（INV9） | 守卫全包 `corten_enabled_static()`（先例 gup.c:1234/memory.c:6561）; shrinker 仅 corten=on 注册 | =n 构建三套件 |
| 7 | 不进 LRU（T1 DoD/DEV-10） | 不加任何 folio_add_lru; 否决 unevictable-LRU 备选（§2.1 D2） | guest 压力下 lru 无增长 + 代码评审 |

---

## 4. 切片表（每片 ≤1 夜验收; T1/T2 为 L, 其余 M/S）

| ID | 内容 | 接口/主要 diff | diff 预估 | 测试 | 依赖 |
|---|---|---|---|---|---|
| **M6.T1** | 审计落地+守卫: ①MMF_UNSTABLE 门（D3, arena.c:2917 ~10 行）; ②rmap 守卫+`corten_rmap_unmap_one` 事务慢路径（D1; rmap.c ~12 行 + arena.c ~140 行）; ③NUMA/PAGEOUT/OQ 决策登记; ④本文件 §1 审计结论回填 REPORT 草稿 | §2.1 D1/D3 | ~200 行/5 文件（rmap.c, corten_arena.c/.h, corten.h, 测试） | KUnit ②③④ + guest: OOM 复现无假 WARN; 压力下 rss 不被 LRU 路径回收 | 无（HEAD 即可） |
| **M6.T2** | swap out/in 事务: D1 步骤 3-6 完整化 + D5 换入 + D6 编码 + D7 消费面扩展 + INV7 checker 扩展 + retry 预算 | `corten_swap_out/swap_in`, `CORTEN_DISP_SWAPIN` | ~500 行（arena.c 为主, corten.h, INV7 checker 所在 corten.c/arena.c, 测试 ~120） | KUnit ①③⑤+fork/gup 一致性; guest: 手工触发（debugfs "evict N 页"入口, T2 自带最小 shrink 桩）zram 往返 checksum 正确 | T1 |
| **M6.T3** | 压力/MGLRU 对接: `corten_mm_registry` + shrinker 注册 + count/scan + 两遍 young 位 aging（aging 事务: desc 写锁内 `ptep_test_and_clear_young`, 镜像 vmscan.c:3717 语义但经事务）; MGLRU 维持结构 skip 的判定文档化（§1.2 P2/P3 勘误进 DESIGN §6.2） | §2.1 D2 | ~300 行 | KUnit: registry 登记/注销、count/scan 合成; guest: memory.max 压 MODE 进程→swap.so 持续增长→撤销压力→swap.si 换回 | T2 |
| **M6.T4** | 观测: debugfs arena_stats 扩展（swapped_out/in, shrink_scans, aging_passes, guard_rejects, reap_gate_hits）; memory.stat/smaps 口径核对脚本 | stats enum（include/linux/corten_arena.h:71-106 追加） | ~100 行 | KUnit render_dbg; guest: memory.stat anon/swap 与 smaps Rss/Swap 交叉核对 | T2 |
| **M6.T5** | guest 压力判据: arena 压 4G→zram（lz4, D5 配置）换入换出正确→memory.stat/smaps 合理; INV7 零漂移; fork/GUP/mremap 与 swap 并存矩阵; 数据落盘 results/rNN/m6 | 脚本（bench/）, 零内核 diff | ~0 内核 / ~200 脚本 | 全矩阵 + 与 M1 基线同参对比 | T3+T4 |

关键依赖说明: T1 不依赖 T2/T3（守卫先行, V1/V2 即刻收口）; shrinker 通道在
T2 仅有桩（debugfs 手工触发）, T3 才给真压力; ROADMAP 旧 M6.T1-T4 编号由本
表 T1-T5 取代（T1 审计确认并入本片, 见 §4 表头）。

---

## 5. 风险表

| ID | 风险 | 概率/影响 | 缓解 |
|---|---|---|---|
| R6-1 | **try_to_unmap 的 TTU_BATCH/defer-flush 交互**: D1 接管后 flush 语义须与上游逐位一致（rmap.c:699/2055 defer、2055 处 per-page flush）; defer 时 folio_put_refs 时机错会 UAF | 中/高 | T1 评审逐 TTU flag 对照; guest swap+并发 fault 压测; 必要时首版只支持非 defer 形状（defer→拒绝 abort, 下轮补） |
| R6-2 | **mmu_notifier 交互**: D1 在 notifier start 区间内进事务取 desc 写锁——若某 notifier 回调反向取同一 desc 锁即死锁; 且 zap 路由（munmap）至今**无 notifier 包裹**（仅 mprotect 有, arena.c:6273-6275）, Stage1 换出路径引入第二个不带 notifier 的 PTE 写者集合会放大 secondary-MMU 撕裂 | 中/高 | D1 明确"在 notifier start 之后才允许进事务"; shrinker scan 全程包 MMU_NOTIFY_CLEAR（镜像 try_to_unmap_one, rmap.c:1893）; zap 无包裹的既有缺口登记为 Stage2 项（guest 无 KVM/牛 secondary-MMU 用户于 arena, 记录口径） |
| R6-3 | **MGLRU/经典扫描对 shadow-VMA 的假设反转**: 当前安全完全系于"页不在 LRU"（vmscan.c:3623/1822 两个结构性闸门）; 任何未来改动让 arena folio 入 LRU（迁移目标页、回收互操作、他人补的 folio_add_lru_vma）都会让 lru_gen aging 直接裸清 arena PTE young 位（vmscan.c:3717）= 静默红线回归 | 低频/高 | T1 守卫把 V2 收口为"够得着也走事务"; 风险登记册加 R 条目+代码注释钉死 arena.c:3474; M8 报告 LoC/披露节提及 |
| R6-4 | swap cache 与 folio 生命周期: 换出后 folio 挂 swapcache（NR_SWAPCACHE 计费）直到全部 swap_free; fault 换入与 munmap zap 并发时 entry 计数错 → 数据丢失（上游 swap_duplicate/free 对称, rmap.c:2148/memory.c:932 同源） | 中/高 | 计数对称性走上游原语不自造; T2 KUnit 合成 entry 对称性; INV7 扩展含"PTE none ⇒ 非 Swapped" |
| R6-5 | zram 压缩路径: 聚簇 entry 分配影响 zram 压缩率/碎片; lz4 上限（D5）; writeback（page_io.c:416/437 bdev 路径）在 zram 上为同步形态 | 中/中 | T5 实测压缩比/吞吐; 必要时 Stage2 聚簇分配裁剪; 不改 zram 本体 |
| R6-6 | shrinker 注册面: count/scan 在 reclaim 上下文被调, 不得睡眠/取 mmap_write; folio_trylock 失败跳过策略可能导致 count 虚高 | 中/中 | scan 全程 trylock+快进快出; count 用 per-mm 缓存值（T3）; lockdep 构建 |
| R6-7 | MMF_UNSTABLE 门对 fork（dup_mmap 走 legacy PTL 路径复制 shadow-VMA）的影响: OOM 窗口内 fork 的 arena 复制退化为 legacy 语义 | 低/中 | 记录口径; INV7 在 fork_commit 侧已校; 压测覆盖 |
| R6-8 | NUMA balancing 留守（V3）: 多节点机器上 restore 风暴（protnone→fault→restore 循环）损耗 | 低（guest 单节点）/中 | restore 计数遥测（T4）; OQ-M6-5 预留 fair.c skip 选项 |

---

## 6. OPEN QUESTION 清单（提请规划者/主 agent 裁决）

- **OQ-M6-1** shrinker 的 mm 枚举器形态: 全局 registry 双链（本规格默认）vs
  复用 debugfs obs ledger 扩展为 per-mm。倾向前者（obs 是 per-arena, 粒度错）。
- **OQ-M6-2** SHARED（fork 后 COW）arena 页是否换出: Stage1/2 否（victim 需
  meta.SHARED==0 && folio_mapcount==1）; 双侧同 entry 换出留 Stage3（需 COW
  fault 与 swapin 的合流分派, 论文 §4.3 有语义、工程量大）。
- **OQ-M6-3** 迁移互操作（remove_migration_pte 路由化）: Stage1 拒绝已安全;
  何时立项（CMA 压力场景才现形, 见风险 R6-9 候补: arena 页占 MIGRATE_MOVABLE
  pageblock 却不可迁移 → 长期碎片化, 影响后续大页/CMA 分配——如实披露级）。
- **OQ-M6-4** hwpoison 真路由（TTU_HWPOISON 形状进事务 + poison entry 入
  __resv? __resv 已满 5 字节, 需另想办法或维持拒绝+WARN）。
- **OQ-M6-5** change_prot_numa: 维持"接受+计数"或 fair.c task_numa_work 加
  VM_CORTEN skip（单点 ~3 行; 代价=NUMA 平衡对 arena 失效——本就无多节点收益）。
- **OQ-M6-6** swapin readahead（swap_state.c:825）首版开还是关: readahead 的
  VMA 侧（swap_vma_ra_win, swap_state.c:685）以 vmf->vma=shadow-VMA 工作,
  语义无碍; 倾向首版维持默认开, T5 实测后裁。
- **OQ-M6-7** MADV_PAGEOUT 是否改路由为"对目标 range 调 shrinker victim 路径"
  （现在是 -EOPNOTSUPP 拒绝, 语义上 PROCESS_MADVISE 用户可见的便利损失）。
- **OQ-M6-8** `pte_sw_mkyoung`（arena.c:3424）使新页即 young: 对两遍 aging 是
  利好（新生页免遭立即换出）, 但确认 x86 该宏实际展开（若为 no-op 则 aging
  首轮可能误伤新页, 需在 aging 事务中加"安装后一个窗口期豁免"）。

---

## 附: 本文引用的关键 file:line 索引（便于评审复核）

arena 侧: arena.c=mm/corten_arena.c — 生产 3088/3092/3097/3422-3426/3472/3473/
3479/3496; LRU 注记 3474-3477; shadowize 478/486; dispatch 3150/3170-3172;
STUB WARN 4178-4183; restore/NUMA 注记 3560-3567; !present WARN 3591-3597;
zap 4642-4741（swap 注记 4682-4684）; protect 6177-6201/6240-6242/6273-6275;
madvice 拒绝 6949; hwpoison 锚 6958-6984; mm_exit 1310-1363; lookup_get 2917;
白名单 535-540; COW pin 3714/3895; mremap 6513-6516/6592-6596; pin 拒复用=
无 zap_pinned（勘误, §3.3）。
上游侧: rmap.c 1522-1535/1860/1893-1898/1908/2148-2201/2201/2220/2224/2267/
2288/2567/699/2055; vmscan.c 1139/1357/1364/1463/1480/1822/3618-3629/3667/
3717/5210; swapfile.c 1441; swap_state.c 88/137/407/524/685/825; page_io.c
288/379/416/437; memory.c 932/1287/4530/4595/4690/5093/6561-6580;
oom_kill.c 541/552/590; fair.c 3629; compaction.c 1101; memory_hotplug.c 1833;
memory-failure.c 1595; gup.c 1234; migrate.c 2650; huge_mm.h 335;
task_mmu.c 1065/1157; mmap.c 428/491/660/1209/1400-1407/1893-1902/2021-2024;
mprotect.c 917; mremap.c 2012; fault.c 1362-1364; fork.c 1058-1062。
