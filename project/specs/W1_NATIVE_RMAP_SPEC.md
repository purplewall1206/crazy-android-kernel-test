# W1_NATIVE_RMAP_SPEC · MV2 W-1: corten 原生反向映射（替代 anon_vma 与 file i_mmap 锚）设计规格
创建: 2026-09-24（规划班, 纯只读调研 + 本文档; 零内核改动零构建）
基线: 主树 **037bfbaed020**（corten-mv-complete, M-V 十二片全落）。
上游: STATE D28（字面完全移除）/ specs/MV2_FULL_REMOVAL_SPEC.md（任务书, W-1 = 拱心石）/
MV_VMA_FREE_SPEC.md §2-3（region record 与 carrier 现状）/ M6_RMAP_SPEC.md（既有守卫与
swap 事务, 本文升级对象）/ docs/PAPER_SPEC.md PS-B5（论文 rmap 口径）。
> 本文所有 6.18 事实断言带 file:line（除特别注明外 mm/ 下相对 /home/ppw/linux-6.18;
> corten_arena.c = mm/corten_arena.c, rmap.c = mm/rmap.c）。

---

## 0. 一句话结论（先给裁决）

**W-1 的 folio→mapper 索引不建任何 per-folio 数据结构**: 匿名侧靠"消费者已知地址 +
结构性排除", file 侧靠 pagecache folio 天然的 (mapping, index) + per-indece corten
region 注册表 O(1) 推导地址 —— 即论文 PS-B5 "命名页→文件对象, 私有匿名→AddrSpace,
rmap 是提示" 的直译; mapcount/stats 经 rmap.c 内新增的 **vma-free wrapper 借用
folio->_mapcount**（不侵入、不新增字段）。W-1 交付 file 侧全原生化 + 匿名侧换出
通道原生化 + 消费面改道; **匿名锚的最终摘除（unanchor flip）被 fork 硬依赖钉死在
W-2**（§3.5 论证）—— W-1 出口 carrier 计数仍非零, D28 终判据在 W-6 收口。

---

## 1. 现状基线（HEAD 037bfbaed020 实测）

### 1.1 锚的现状: 谁在消费 carrier 的 rmap 形状

窗口页今天分两族, 锚的形态不同:

- **匿名 folio**（fault/COW/swap-in 产物）: 装页点
  `folio_add_new_anon_rmap(folio, vma=anchor, addr, RMAP_EXCLUSIVE)` 四处
  （corten_arena.c:7094 map_anon / :7436 COW write / :7804 swap-in / :8314 file-COW
  私有拷贝）—— `__folio_set_anon`（rmap.c:1343-1366）把 `folio->mapping = vma->anon_vma
  + FOLIO_MAPPING_ANON`、`folio->index = linear_page_index()`、置 swapbacked 与
  PageAnonExclusive。锚 = carrier 上的 `anon_vma_prepare()`（corten_arena.c:1033,
  auto-attach :1876 / reactivate re-arm :9826 / fork 子侧 anon_vma_fork :5851）。
- **file folio**（V-B FILE region 的 pagecache 页）: 装页点
  `folio_add_file_rmap_pte(folio, page, vma=carrier)`（corten_arena.c:8144）——
  folio->mapping = address_space 是 pagecache 自带的（filemap 侧写入, 与 corten 无关）,
  corten 侧只做 mapcount+stats; i_mmap 参与 = `vma_interval_tree_insert(carrier,
  &mapping->i_mmap)`（corten_arena.c:1284-1292）。
- 摘除点三处: COW 换旧页 `folio_remove_rmap_pte(old…)`（:7450）/ zap
  `corten_zap_release_page`（:9046-9068, :9067）/ swap-out 事务
  `folio_remove_rmap_ptes`（:13456）。
- **fork**: `copy_page_range(ccarrier, pcarrier)`（corten_arena.c:6212）—— 上游
  `copy_present_ptes`（memory.c:1120-1180）按 `folio_test_anon` 分叉: 匿名走
  `folio_try_dup_anon_rmap_pte`（清 exclusive、mapcount+1、rss=ANONPAGES）, file 走
  `folio_dup_file_rmap_ptes`（mapcount+1、rss=file 族）。**这是锚最深的一根钉子**。

### 1.2 M6 守卫现状（W-1 的升级对象）

- rmap.c 两个 `_one()` 入口的 VM_CORTEN 守卫: try_to_unmap_one（rmap.c:1895-1919:
  prefilter `corten_rmap_unmap_one` + 完成臂 `corten_rmap_swap_out`）与
  try_to_migrate_one（rmap.c:2351-2360: 全形状拒绝, OQ-M6-3）。守卫的地址来源 =
  page_vma_mapped_walk 经 carrier 的 anon_vma 间隔树。
- 回收通道: shrinker pick（corten_arena.c:13696-13810 shrink_walk, pick 自带 addr）
  → `__reclaim_pages`（:13983, vmscan.c:2360）→ shrink_folio_list 形状
  folio_alloc_swap（vmscan.c:1379/1399）→ try_to_unmap → **ttu 经 rmap 把 shrinker
  已知的地址重新发现一遍** → 完成臂事务换出。
- hwpoison: `corten_arena_hwpoison_check`（corten_arena.c:13162-13185）走 folio 的
  anon_vma 找 VM_CORTEN avc → WARN; 之后 memory-failure.c:1598-1604 的
  `!folio_test_lru → return true` 结构性早退（file 页过此门但 ttu 被 guard 拒,
  终态同为"响亮失败"）。
- oom_reaper: `corten_oom_reap_skip_vma`（corten_arena.c:13212-13235）整 VMA skip
  —— 不依赖 rmap, W-1 不动。
- swapoff: `unuse_mm` 只走树 VMA（swapfile.c:2459-2478 + :2473 blind_note）——
  窗口 swap PTE 不可见, S-3 开放项（STATE 登记的 ~60 行升级臂, 本文 §5 W1.f 收口）。
- 迁移/compaction: 隔离端 LRU 门（compaction.c:1101 / memory_hotplug.c:1833）+
  migrate.c:2650 路由拒绝 —— 结构性排除, W-1 维持（R2）。

### 1.3 关键上游事实（选型的地基）

| # | 事实 | file:line | 设计含义 |
|---|---|---|---|
| F1 | order-0 folio 的 `__folio_add_rmap`/`__folio_remove_rmap` **只碰 `folio->_mapcount` 与 lruvec stats**; vma 参数仅用于 large-folio mapcount、`mlock_vma_folio`、VM_BUG 检查（`__folio_rmap_sanity_checks` 根本不收 vma, include/linux/rmap.h:397） | rmap.c:1222-1307 / 1658-1776 | **mapcount 维护可以 vma-free 化** —— 借用不需要侵入 |
| F2 | free 时 `page_expected_state` 要求 `mapcount == -1` **且** `page->mapping == NULL` | page_alloc.c:1227-1232 | 借用必须对称归还; mapping=NULL 姿态天然过检 |
| F3 | `rmap_walk_file` 对 `!folio->mapping` 直接 return | rmap.c:3015-3018 | unanchored folio 的 rmap_walk 是**优雅 no-op**, 不是崩溃 |
| F4 | `folio_lock_anon_vma_read` 对 mapping 低两位非 ANON 的 folio 返回 NULL → `rmap_walk_anon` 不迭代 | rmap.c:546-560 / 2903-2906 | 同上, 匿名侧 no-op 安全 |
| F5 | `folio_mapped` = `_mapcount >= 0`; page_idle 双门 `!folio_mapped \|\| !folio_raw_mapping` | page_idle.c:95-113 | 借用让外部 walker "诚实地失败"; -1 姿态让它们直接跳过 |
| F6 | file 页与 legacy 映射者**共享同一 pagecache folio**（dlopen 的库同时被非 MODE 进程映射） | V-B 设计天然 | file 侧 mapcount **必须精确**（migration/reclaim 的 folio_mapped 门是共享的） |
| F7 | fork 的 `copy_present_ptes` 按 `folio_test_anon` 分叉, 匿名臂 `VM_WARN_ON_FOLIO(!folio_test_anon…)` 家族 + rss 计数器分族 | memory.c:1120-1180 | **匿名 unanchor 不能先于 fork metadata 化落地**（§3.5） |
| F8 | B.2 失效门已经声明过 `i_mmap_read > desc(W) > ptl` 边（无回路论证在案） | corten_arena.c:1380-1401 | per-inode 注册表复用同一锁序, INV2 零新增锁类 |

---

## 2. Q1 数据结构: folio→mapper 索引选型（三案对比 + 推荐）

### 2.1 三案

**案 A: per-folio 反向指针（侵入 folio）**
在 folio 上挂"映射了我的人"（mm 指针或 (mm,addr) 数组）。落点候选: `folio->private`
/ `folio->mapping` 低位 / 借用第 3 个计数字段 / 新增 folio 字段。
**拒绝**。逐落点: (a) `folio->private` 在 swap 路径上被 swapcache 的 swp_entry 占用
（add_to_swap 起、swap_free 止）—— 恰是 rmap 最需要工作的窗口, 冲突不可调和;
(b) `folio->mapping` 低两位已被 FOLIO_MAPPING_FLAGS 占用且 page_idle 做无锁乐观扫
（rmap.c:1358-1364 注记）, 塞 mm 指针 = 对调试面撒谎; (c) `_mapcount`/`_entire_mapcount`
语义被内核全局消费, 借用作指针 = 直接炸 F2/F5 的每个消费者; (d) 新增字段 = 改
struct folio 本体, 与"地图不侵入页面"的论文立场（PS-B5: rmap 记录在**页描述符**上,
本移植的描述符 = ptdesc+meta, 不是 struct page）相悖。

**案 B: 派生（region/frame 表 + 窗口走查; 推荐）**
反向问题按页族分流:
- **file 页**: folio 自带 `(folio->mapping, folio->index)`（pagecache 固有, F6）→
  查 per-inode corten region 注册表（§4）→ `addr = region->start + ((folio->index
  - region->rpoff) << PAGE_SHIFT)`，O(#region/inode)（dlopen 场景个位数）, 再在事务内
  校验 PTE present 且 PFN 相符（corten_rmap_swap_out :13320 已是此形状）。
- **匿名页**: **不建 folio→addr**。内部消费者全部已知地址: shrinker pick 自带 addr
  （:13773 pick 处）、COW 判定用 meta.SHARED（PS-C3 的 map_count==1 语义, fork 是唯一
  跨 mm 共享源、fork 对双侧标 SHARED 已是既有契约）、fork 走窗口快照（:6221-6252）、
  unuse/exit 走窗口走查（exit_walk :3471 同型）。folio-keyed 的**外部**消费者
  （hwpoison/migrate/page_idle）结构性排除（§3）。
- 内存代价 **零**; 查询单价: file O(1) 推导 + O(#region) 枚举, 匿名内部 O(0)
  （地址已知）/ 外部 O(不回答, 排除）; 锁: 全部经既有 desc 写锁事务, 零新锁类。
- **论文口径核对（PS-B5）**: "命名页→文件对象（内含 AddrSpace 树）" = file 侧
  (mapping, index)→注册表推导, 直译; "私有匿名→AddrSpace" = 论文的页描述符带
  AddrSpace 反链 —— 本移植**不建这条反链**, 因为唯一需要它的消费者已被排除;
  登记为披露项（R-W1-6, §6）。"rmap 是提示, 改页表必经事务"（§4.5 原文）= 本设计
  的消费者改道全部落事务（§3.1-3.3）, 语义一致。

**案 C: 全局 per-inode/per-mm folio→frames 索引（radix/哈希, PFN→(mm,addr)）**
**拒绝**。8B/4K页 ≈ 0.2% 常驻内存（JVM 压测 8G 窗口 ≈ 16MB 纯索引）; fault 快路径
+2 次 xarray 读写（mmap-pf 是已披露的最贵固定成本, 不再加码）; 收益 = O(1)
folio→addr —— 而 §2.1 案 B 的消费者分析表明**没有任何在产消费者需要这个查询**
（内部已知地址、外部被排除）。为不存在的消费者付常驻代价, 违反 DEV-10 以来
"arena 页零额外锚点"的整条设计线。

### 2.2 子决策: mapcount 借用（vma-free wrapper）

不建指针 ≠ 不碰 mapcount。**推荐: 借用** —— 经 rmap.c 内新增 4 个 vma-free 公共
wrapper 维护 `folio->_mapcount` + lruvec stats（NR_ANON_MAPPED / NR_FILE_MAPPED,
镜像 `__folio_mod_stat`, rmap.c:1208-1227）+ swapbacked/AnonExclusive 位:

```c
/* mm/rmap.c 新增（__folio_add_rmap 是本文件 static inline, wrapper 必须同居） */
void folio_add_anon_rmap_novma(struct folio *folio);   /* swapbacked 位 + mapcount 0
						        * + AnonExclusive, 不碰 mapping */
void folio_remove_anon_rmap_novma(struct folio *folio);/* mapcount-1 + stats +
						        * 最后一次清 AnonExclusive */
void folio_add_file_rmap_novma(struct folio *folio);   /* mapcount+1 + NR_FILE_MAPPED */
void folio_remove_file_rmap_novma(struct folio *folio);/* 对称 */
```

对照 V-A.1 的 "-1 姿态"（永不计数, folio 恒 unmapped）: 借用在三点上胜出 ——
① F6: file 页与 legacy 共享 folio, mapcount 必须算上 corten 侧, 否则 legacy 侧的
migration/回收门（folio_mapped）在共享页上读到假阴性; ② F2: 借用的对称归还让
free 检查天然通过, 而 -1 姿态下任何路径替我们 +1（如 W-1 过渡期的 fork file 臂）
就出现无法归还的漂移; ③ KPF_MAPPED/memory.stat AnonPages 调试面保真（M6.T4 的
观测口径不回退）。风险: wrapper 忘配对 = free 时 VM_BUG（响亮, 非静默）——
INV7 checker 加锚（§5 W1.c）。

---

## 3. Q2+Q3 登记摘除点与消费者改道（逐点表）

### 3.1 登记摘除点总表（W-1 落 vs W-2 落, 界线 = §3.5 fork 约束）

| # | 生命点 | file:line（现码） | 今日操作（carrier 锚） | W-1 原生操作 |
|---|---|---|---|---|
| R1 | fault 装匿名页 | corten_arena.c:7094 | folio_add_new_anon_rmap(carrier) | **不变**（flip 在 W-2; wrapper 已备） |
| R2 | COW write 新页 | :7436 | 同上 | 不变（同上） |
| R3 | COW 换下的旧 **file** 页 | :7450 | folio_remove_rmap_pte(old, carrier) | **→ folio_remove_file_rmap_novma**（W1.c 翻转） |
| R4 | file read 臂装页 | :8144 | folio_add_file_rmap_pte(carrier) | **→ folio_add_file_rmap_novma**（W1.c） |
| R5 | file-COW 私有拷贝（匿名化） | :8314 | folio_add_new_anon_rmap(carrier) | 不变（W-2） |
| R6 | swap-in 回装 | :7804 | folio_add_new_anon_rmap(carrier) | 不变（W-2; wrapper 在驱动事务内调用） |
| R7 | zap 释放 | :9054-9075（remove :9067） | folio_remove_rmap_pte(anchor)，anon/file 按 folio_test_anon 分族 | **file 臂 → novma remove**（W1.c）; anon 臂不变; 分族依据逐步从 folio_test_anon 改为 meta/region class（anon 侧留 W-2, 避免单点半翻） |
| R8 | swap-out 事务 | :13456 | folio_remove_rmap_ptes(carrier) | **→ folio_remove_anon_rmap_novma**（W1.e, 驱动内置） |
| R9 | fork dup | :6212 copy_page_range(ccarrier,pcarrier) | 上游 copy_present_ptes（memory.c:1120） | **不变**（W-2 的 fork_copy_ptes 用 W-1 的 wrapper 做 file 页 dup 与 COW 锚 —— MV2 任务书原文） |
| R10 | exit 拆页 | exit_walk :3471 → zap 族 | 同 R7 | 同 R7 |
| R11 | file region 注册/注销 | i_mmap insert/remove :1284/:1301 | carrier 入 mapping->i_mmap | **→ per-inode region 注册表**（W1.b, §4） || R12 | 失效事件 | memory.c:4182 gate → :1432 unmap_file_event | i_mmap 枚举到 carrier 才触发 | **→ mapping 级枚举钩子**（W1.b, §4） |

锁序: R3/R4/R7/R8 全部在既有事务临界区内（desc 写锁 > ptl, wrapper 本身无锁,
仅原子量与 per-cpu stat —— INV3 不破）; R11/R12 复用 B.2 已声明的
`i_mmap_rwsem(r/w) > desc(W) > ptl`（F8, corten_arena.c:1380-1401）, **零新增锁类,
零新增边**。DEV-13 总序不变。

### 3.2 消费者改道: rmap_walk / ttu 家族

| 消费者 | 入口 | 今日 | W-1 终态 |
|---|---|---|---|
| try_to_unmap（回收换出的 PTE 写步） | rmap.c:2308 | 经 carrier anon_vma 找回地址 → :1895 守卫 → :13320 完成臂 | **匿名侧改道**: shrinker/evict 不再喂 `__reclaim_pages`, 改用 W1.e 原生驱动（地址已知, §3.4）→ ttu 对匿名窗口页**不可达**; rmap.c:1895 匿名臂成兜底断言（保留, 计数）。**file 侧真路由**: try_to_unmap 层新增 vma-free 钩子 `corten_rmap_ttu(folio, flags)`（放 :2308 函数体首, 非 _one 层 —— corten region 无 vma, _one 的 pvmw 形状进不去）: folio 为 file 且其 mapping 注册表非空 → 逐 region 推导地址 → 单页事务（复用 unmap_chunk_flags 的 FILE_EVENT 降位形状, meta MAPPED→FILE_MAPPED, KEEP_PERM）→ novma remove。TTU_HWPOISON/迁移形状拒绝+计数（M6 姿势升级为"file 普通回收真路由, 其余维持排除"） |
| try_to_migrate | rmap.c:2288/:2351 | 全拒绝（OQ-M6-3） | 维持拒绝; :2351 守卫对 file carrier 消失后不可达 → 钩子内 decline 臂承接计数（R2 登记: W-1 后可选择性重开, 立项另裁） |
| rmap_walk（page_idle / hwpoison collect_procs 等） | rmap.c:3022-3041 | 走 anon_vma / i_mmap, 都经 carrier | 匿名 unanchor 后（W-2）: F3/F4 保证**优雅 no-op**（mapping NULL → 两个 walk 族各自早退）; file 侧 walk 只见 legacy 映射者, 漏 corten PTE = page_idle 对窗口 young 位不清（无害, young 是提示）与 hwpoison collect 漏 MODE 进程（= 今日 guard 拒绝的同等终态, §3.3） |
| hwpoison 探针 | corten_arena.c:13162（走 folio anon_vma）+ memory-failure.c:1595/:1598-1604 | WARN + LRU 门早退 | W-1: file 探针改走 per-inode 注册表命中（mapping 有 corten region → WARN + 计数）; 匿名探针不变（仍锚定）。W-2 后: `folio_get_anon_vma` 返 NULL → 探针换为 `hwpoison_probe_unanchored()` 计数器（debugfs 可观测）, 匿名 hwpoison = 排除+披露（升级路径 = corten_mm_registry mm 扫描, 不在 W-1/W-2 建, 登记 OQ-W1-4） |
| oom_reaper | corten_arena.c:13212 | VM_CORTEN VMA skip | 不变（树空后 for_each_vma 天然空转, skip_vma 对 carrier 本就不可达） |
| swapoff unuse | swapfile.c:2459-2478 | 树走查, 窗口盲（:2473 note） | **W1.f 枚举升级**: unuse_mm 增加 corten 臂 —— per-mm 窗口走查找 meta==CORTEN_SWAPPED 且 entry 匹配 → corten_swapin_sync_meta（:13500 同步镜像）+ unuse_pte 等价事务。收 S-3 开放项 |
| vmscan 隔离/MGLRU | vmscan.c:1822/3623 | LRU 结构门 | 不变（零 folio_add_lru 红线维持; MGLRU 结构 skip 维持） |
| compaction/hotplug | compaction.c:1101 等 | !LRU skip | 不变 |

### 3.3 folio->mapping == NULL 后的调试面假设点（逐点处置）

| 假设点 | 现码 | unanchor 后行为 | 处置 |
|---|---|---|---|
| dump_page | mm/debug.c:73-106 | `mapping:(null)` 打印; `folio_mapping()` NULL → 不 dump_mapping | 无害, 文档化（REPORT 披露节） |
| /proc/kpageflags KPF_MAPPED | 由 mapcount 推 | mapcount 借用 → **保真** | 无需处置 |
| /proc/kpagecount | refcount | PTE 引用在 → 保真 | 无需处置 |
| page_idle | page_idle.c:95-113 | F5 双门 → 匿名 skip / file 走查漏窗口 young | 接受（young 是提示位）; OQ-W1-3 |
| smaps/statm/Rss | V-C 双源 PTE 走查 | 不读 folio->mapping | 无需处置 |
| memory.stat AnonPages/FileMapped | lruvec stats | novma wrapper 维护 | 保真（W1.a KUnit 锚） |
| PageAnonExclusive 消费者（swap 编码/COW） | corten 侧自产自销 | wrapper 维护, swap 驱动读它编码 swp_exclusive（:13444 同款） | 事务内自洽 |
| `folio_mapping()` 通用调用面（writeback/memcg） | 全内核 | 匿名窗口页不经 writeback（swap 走 swapcache 私有链）; memcg charge 在 alloc 时已定 | W1.e 驱动验证锚覆盖 |

### 3.4 Q2 配套: 匿名换出原生驱动（W-1 的最大新件）

现状链条 `pick(addr) → __reclaim_pages → ttu 经 rmap 重发现 addr → 完成臂事务`
里, rmap 的唯一贡献是**把驱动已知的地址还给驱动**。W1.e 删除这条弯路:

```c
/* mm/corten_arena.c 新增（签名级） */
/* 单页原生换出: 入参自带地址, 返回是否换出成功。
 * 形状 = 现 corten_rmap_swap_out(:13320) 的 vma-free 化 + 前后腿自持:
 *   锁外 prep:  folio_alloc_swap(镜像 vmscan.c:1379) + add_to_swap(swapcache)
 *   事务内:    pte clear → swp PTE 安装 → MM_ANONPAGES/SWAPENTS →
 *              folio_remove_anon_rmap_novma → corten_swap_out(meta→SWAPPED)
 *   事务后:    defer-flush 簿记(R6-1 同款) → swap_writepage(zram 同步形状)
 *              → writeback 完成后 __remove_mapping 形状释放
 * 锁序: folio_trylock(调用方) > [swap cluster 锁] > desc->lock(W,BH) > ptl
 *       （D4 序零变化 —— entry 分配/cache 插入仍在 desc 锁之前完成） */
bool corten_swap_out_driver(struct mm_struct *mm, unsigned long addr,
			    struct folio *folio);
/* shrinker/evict 的批量腿: 取代 corten_shrink_reclaim(:13948) 的
 * __reclaim_pages 循环; pick 列表改 (addr, folio) 二元组。 */
unsigned long corten_shrink_swap_list(struct mm_struct *mm,
				      struct list_head *picks);
```

不做的形状（登记不隐藏）: 异步 writeback 设备的完成回调路径首版只支持 zram 同步
形状（guest 门只有 zram; page_io.c:416 异步腿留 W-1 后按需补, OQ-W1-5）;
SHARED 页继续不换出（OQ-M6-2 维持）。

### 3.5 边界论证: 为什么匿名 unanchor flip 在 W-2 不在 W-1

F7 的分叉是硬的: `copy_present_ptes`（memory.c:1120-1180）对
`!folio_test_anon(folio)` 一律走 file 臂 —— unanchored 匿名页被 fork 复制时
`folio_dup_file_rmap_ptes` 数值上成立（mapcount+1）, 但 **rss 记入 MM_FILEPAGES**
（fork 对拍锚必炸）、且 zap 侧 `corten_zap_release_page`（:9052 `folio_test_anon`
分族）会跟着记错族。半翻状态（fork 用 legacy 复制 + zap 用 meta 分族 + rss 补差）
要同时改 fork/zap/COW 三处计数器且没有干净的对拍锚 —— 这正是 W-2 "fork_copy_ptes
纯 metadata 化" 的工程本体。故 W-1 的匿名侧做到"**锚的最后一个消费者只剩 fork 与
GUP 探针**"为止: 换出通道（ttu）、失效枚举（i_mmap）、hwpoison 探针、unuse 全部
不再看 carrier 的 anon_vma/i_mmap; `corten_arena_carrier_alloc` 的 `prepare` 参数
与 anon_vma_prepare 在 W-1 期间保留（fork 的 anon_vma_fork :5851 仍需）。

W-1 出口的可测断言: **rmap.c 两个守卫的匿名臂计数为零**（ttu 不可达）、
`corten_nr_carriers` 仍非零（W-2 收）、J1-J4 全绿维持。

---

## 4. Q4 file 页的 i_mmap 等价物: per-inode corten region 注册表

### 4.1 数据结构（零核心结构侵入）

```c
/* include/linux/corten_arena.h */
struct corten_inode_regions {		/* 每文件一个, 惰性分配 */
	struct list_head	regions;	/* region->rfile_nodes 挂入 */
	unsigned int		nr;
	/* 占位: 失效事件直方图 (T4 观测) */
};
/* 全局索引: xarray, key = (unsigned long)mapping, value = 上述头。
 * 注册表自身的锁 = mapping->i_mmap_rwsem（读写语义与今天的 interval-tree
 * 完全同位 —— attach 持写、枚举持读, F8 的边序原样复用）。 */
```

region 侧加 `struct list_head rfile_node`（内嵌, 无新分配）; attach 时
`corten_region_register_file`（现有, corten_arena.c:5871 附近）在 i_mmap insert 的
同一锁窗口换成注册表插入; teardown（corten_region_file_teardown :1327）对称。
全局 xarray 的 xa_lock 与 i_mmap_rwsem 的取序: 先 xa（注册表头分配/回收）后
i_mmap_rwsem（链表操作）—— xa 临界区内无 i_mmap 操作, 无环。

### 4.2 枚举源翻转（unmap_mapping_range 家族）

`unmap_mapping_pages`/`unmap_mapping_folio`（memory.c, 经 :4182 的既有 corten gate
到达 carrier）改为 **mapping 级前置钩子**:

```c
/* memory.c unmap_mapping_pages() 体内, i_mmap 树走查之前 */
corten_arena_unmap_file_range(mapping, &hba_index... /* pgoff 范围 */, even_cows);
/* 逐 region: 交叠 [rpoff, rpoff+npages) → 地址区间 → corten_arena_unmap_chunk_flags
 * (KEEP_PERM | (!even_cows ? FILE_EVENT : 0)) —— B.2 的事务体原样复用 (:1432-1469)。
 * 旧 vma-keyed gate (:1432) 与 zap_single backstop 保留为断言（应永远零命中）。 */
```

单页形状 `unmap_mapping_folio`（hwpoison file 腿用）同钩子（推导地址 O(1), §2.1）。
`ttu file 真路由`（§3.2）共用同一注册表与同一单页事务 —— 三消费者一源。

备选记录（不选）: 扩 `struct address_space` 加字段（侵入核心结构, 且 6.18 merge
冲突面大）; 全局 interval tree（第二棵树 = INV7×2 负担, 注册表只需 O(#region) 线性
枚举, dlopen 场景个位数）。

---

## 5. Q5 切片化（6 片, 每片 ≤300 行, 依赖线性）

| ID | 内容 | 主要 diff | 行估 | KUnit 锚（mm/corten_arena_test.c 增） | guest 判据 |
|---|---|---|---|---|---|
| **W1.a** | vma-free wrapper 四函数（rmap.c）+ 匿名 wrapper 的位/计数语义 + =n 折叠 | rmap.c ~90, corten_arena.h ~30, 测试 ~120 | ~240 | ①novma add/remove roundtrip: mapcount -1→0→-1, NR_ANON_MAPPED/NR_FILE_MAPPED ±1; ②swapbacked/AnonExclusive 位设置与最后摘除清位; ③free 路径 page_expected_state 过检（合成 folio）; ④=n 构建折叠 | 无行为变化（零调用点）—— 回归集原样绿 |
| **W1.b** | per-inode 注册表 + attach/teardown 翻转 + unmap_mapping_range/folio 枚举源翻转 + 旧 gate 降级为断言 | corten_arena.c ~260, memory.c ~15, 测试 ~80 | ~355→压到 ≤300（旧 gate 删 ~60 抵扣） | ①注册/注销/refcount 对称（file 引用仍 region 持有）; ②truncate 路由事务原语义（KEEP_PERM/even_cows 两形状, 复用 B.2 既有锚扩展）; ③注册表并发 attach/teardown lockdep 用例 | dlopen 后 truncate 库文件: smaps 归零 + checksum + J1-J4 维持（B.2 guest 门复跑） |
| **W1.c** | file 装页/摘除点翻转（:8144/:7450/:9067 file 臂 → novma wrapper）+ INV7 checker 加"file mapcount 对称"锚 | corten_arena.c ~90, 测试 ~90 | ~180 | ①file read fault 后 mapcount 与 legacy 映射者共享计数正确（合成双映射）; ②COW 换旧页/ zap 的计数对称; ③INV7 零漂移扩展 | JVM dlopen 工作负载: memory.stat file_mapped 与 smaps Rss:File 交叉核对不回退 |
| **W1.d** | ttu file 真路由: try_to_unmap 层钩子 + 单页降位事务 + migrate/hwpoison decline 臂 | rmap.c ~40, corten_arena.c ~220, 测试 ~60 | ~320→压到 ≤300（复用 unmap_chunk_flags ~80 现成） | ①钩子对无 corten region 的 mapping 零开销快门; ②decline 臂（HWPOISON/migrate flags）计数; ③降位事务后 PTE none + meta FILE_MAPPED + 下次 fault 重读 pagecache 一致 | **页缓存回收重开判据**: 压 memcg → window-mapped file 页被回收（swap.so 无关, 用 file 页 workingset 计数验证）→ 重新 fault 内容一致; compaction 对窗口 file 页迁移仍拒绝（计数） |
| **W1.e** | 匿名换出原生驱动（§3.4 签名）+ shrinker/evict 弃 __reclaim_pages + rmap.c:1895 匿名臂降级断言 | corten_arena.c ~280, corten_arena.h ~20, 测试 ~80 | ~380→**拆两片**: W1.e1 驱动+evict(debugfs 腿) ~200 / W1.e2 shrinker 腿+守卫降级 ~160 | ①驱动单页 roundtrip（合成 entry, 不写盘）: 计数/swapcache/refcount 对称; ②defer-flush 簿记形状; ③pick 列表 (addr,folio) 配对完整性; ④rmap 守卫匿名臂计数为零的可达性锚 | M6.T3 压力门复跑: memory.max 压 MODE → swap.so 增长 / RSS 降 / INV7 零漂移 / **ttu 计数恒零**（新断言）; zram 往返 checksum |
| **W1.f** | unuse 枚举升级（收 S-3）+ hwpoison 探针 file 侧改道注册表 + 观测计数器（ttu_hook_unmaps/driver_swaps/registry_size/unanchored_probes） | swapfile.c ~25, corten_arena.c ~140, 测试 ~60 | ~225 | ①unuse 臂对 SWAPPED 槽的 entry 匹配与镜像同步; ②探针注册表命中计数; ③render_dbg 新字段 | swapoff 复测: read-back 换入计数**非平坦**（S-3 开放项关闭）+ swapoff 干净退出 |

行估合计: **~1530 行**（含测试）。每片独立走完整协议链（review/三套件/=n/guest 门/
严格门, R4 纪律）。顺序依赖: a → (b → c) → d → e1 → e2 → f; b/c 可与 a 并行评审。

---

## 6. Q6 风险表 + 工程量重估

### 6.1 风险表

| ID | 风险 | 概率/影响 | 缓解 |
|---|---|---|---|
| R-W1-1 | **匿名换出驱动脱离 shrink_folio_list 脚手架后的 folio 生命周期**: entry 分配→swapcache→事务→writeout→释放 的引用/计数对称没有上游安全网（ttu 曾是网）, 错一处 = 活 PTE 下 UAF 或泄漏; R6-1 defer-flush 语义需自复刻 | 中/**高** | 驱动严格镜像 vmscan.c:1360-1420 匿名 swapbacked 臂 + rmap.c:2120-2201 编码（完成臂已是逐位镜像, :13320 注记）; W1.e1 KUnit 合成 entry 对称锚; guest zram 往返 + 并发 fault 压测; INV7 扩展"PTE none ⇒ 非 SWAPPED"双向 |
| R-W1-2 | **失效枚举漏 region**: 注册表与 region 生命周期错拍（fork 子侧注册/park/teardown 竞态）→ truncate 后窗口残留 stale PTE = 静默数据损坏 | 低-中/**高** | 注册/注销与 i_mmap insert/remove 同锁窗口同序（B.2 的 F8 论证原样）; KUnit 并发 attach/teardown + lockdep; guest truncate+并发 dlopen 矩阵; 旧 gate 降级为永零断言兜底 |
| R-W1-3 | ttu file 路由的 notifer/defer-flush 交互（R6-1/R6-2 家族在钩子形状下的重演） | 中/中 | 钩子在 try_to_unmap 层自带 invalidate window（镜像 :1893 形状）; 首版非 defer 形状（decline 计数）, T5 型实测后放开 |
| R-W1-4 | mapcount 借用的错配对在**共享 file 页**上污染 legacy 映射者的计数（跨域爆炸半径） | 低/**高** | 全部配对点在事务临界区内（§3.1 表 R3/R4/R7/R8）; INV7 加 mapcount 对称锚; W1.c 单独成片小步翻转 |
| R-W1-5 | W-1 过渡期"双记账"复杂度: file 侧已 novma、匿名仍锚定, fork 的 copy_page_range 对两类混合 arena 的路径 | 中/中 | fork 路径 W-1 零改动（R9 不变是边界条件）; 对拍锚全量复跑每片 guest 门 |
| R-W1-6 | 匿名 unanchor（W-2 后）: hwpoison 失去 folio→mm 线索, 受害 MODE 进程不被 kill（重复 MCE）—— 论文有反链、我们没有 | 低（guest 无 poison 注入）/中-高（产品语义） | 披露项进 REPORT; 升级路径 = corten_mm_registry 扫描臂（~80 行, OQ-W1-4 立项另裁）; 探针计数器可观测 |
| R-W1-7 | unuse 枚举升级与换入并发: unuse_pte 等价事务与 fault swap-in 的重入 | 低/中 | 复用 corten_swapin_sync_meta 的先镜像后装序（:13500 注记的 DEV-13 方向）; KUnit 合成 entry 用例 |

### 6.2 工程量重估（对比 D20-a "×3" 说法）

D20-a 当时的口径: "拒 carrier = 重写 fork/COW/GUP ×3 工程量"（STATE.md:710-712）。
以现码为准重估:

- **W-1 实测规模: ~1530 行 / 6+1 片**（§5）, 其中 ~60% 是加法机械（wrapper/注册表/
  钩子/观测）, 高危翻转集中在 W1.d/W1.e 两片。
- W-2（fork_copy_ptes + GUP 摘 carrier + 匿名 unanchor flip + anon wrapper 上线）
  按同一密度估 ~1100-1400 行 / 4-5 片（fork 的 PT 复制臂是大头, M5 的窗口快照机械
  可复用 ~40%）。
- 合计 W-1+W-2 ≈ 2.6-2.9k 行 ≈ M-V 系列（9 片 4.7k 行）的 60% —— **×3 的量级判断
  当时是对的, 但它是可分解的**: W-1 单独约等于 ×1, 且 W-1 出口保持 J1-J4 全绿 +
  回归集零改动, 不存在"半重写"的不稳定长台阶。真实增量风险不在行数, 在 R-W1-1/R-W1-2
  两个无上游安全网的事务族。

### 6.3 红线核对表（每片 review 逐条打勾）

| # | 红线 | W-1 执行 |
|---|---|---|
| 1 | rmap/回收对窗口 PTE 写必经事务（PS-B5/INV6） | 驱动/降位事务/ttu 钩子全部 desc 写锁内（§3.1/§3.4）; INV6 新增写点清单 = W1.b/W1.d/W1.e 三处 |
| 2 | 唯一真源（PS-B2/INV7） | mapcount 借用对称锚 + "PTE none ⇒ 非 SWAPPED" 双向 + 注册表-region 生命周期锚 |
| 3 | desc 锁 BH 对称/无睡眠无分配（INV3） | wrapper 原子量+per-cpu stat; 驱动 I/O 与 entry 分配在事务外（D4 序维持） |
| 4 | DEV-13 锁序无反向边 | 零新锁类; i_mmap_rwsem 复用既有边（F8）; xa_lock 先于 i_mmap（§4.1 无环论证） |
| 5 | 不进 LRU（DEV-10） | 零 folio_add_lru; file ttu 路由不改隔离端（pagecache 原生 LRU 与窗口无关） |
| 6 | =n/off 折叠（INV9） | wrapper/钩子/注册表全包 corten_enabled_static 或编译期折叠; 三套件每片 |
| 7 | J1-J4 维持 | 每片 guest 门含 J1-J4; W-1 出口断言: ttu 匿名臂计数恒零、carriers 仍非零（W-2 收） |

### 6.4 OQ 清单（提请主 agent/规划者裁决）

- **OQ-W1-1** ttu file 路由首版是否含 TTU_BATCH/defer 全形状（倾向: 非 defer 先行,
  R-W1-3 实测后放开）。
- **OQ-W1-2** page_idle 对窗口 file 页 young 不清: 接受（提示位语义）或补钩子
  （倾向接受, 披露）。
- **OQ-W1-3** 原生驱动 writeback 完成形状: zram 同步先行（倾向）, 异步设备
  （page_io.c:416 腿）W-1 后按需。
- **OQ-W1-4** hwpoison 匿名受害进程枚举升级（registry 扫描 ~80 行）何时立项
  （W-1 不做, R-W1-6 披露）。
- **OQ-W1-5** W1.d 是否顺带重开 compaction 对窗口 file 页的迁移（现在 decline;
  迁移 entry 事务是 OQ-M6-3 的 file 子集, 倾向不混片）。
- **OQ-W1-6** anon unanchor 后 NR_ANON_MAPPED 是否继续维持（mapcount 借用延续性）——
  倾向延续（W2 复用 W1.a wrapper, 无额外决策点, 此处仅登记口径）。

---

## 附: 本文引用的关键 file:line 索引（评审复核用）

corten_arena.c: carrier 区 925-1077（alloc :981 / free :1054 / anchor_vma :1071）;
i_mmap 1284/1301; region_file_teardown 1327; 失效门 1432-1469; exit_walk 3471; mm_exit 3685;
auto-attach carrier 1876; fork_register_child 5804-5910（anon_vma_fork :5851）;
copy_page_range 调用 6212; 窗口快照/copy 6221-6260/5973; reactivate re-arm 9826;
map_anon 7060-7135（rmap add :7094, LRU 注记 :7096）; COW write 7296-7480
（add :7436 / remove old :7450）; swap-in 7799-7810（add :7804）; file read 臂
8048-8160（add :8144）; file-COW 8184-8345（add :8314）; zap release 9054-9075
（remove :9067）; hwpoison 13162-13185; oom skip 13212-13235; rmap_unmap_one
13259-13318; rmap_swap_out 13320-13490（remove :13456）; swapin_sync_meta 13500;
shrinker 区 13550-14000（walk :13703 / pick ~:13773 / __reclaim_pages 调用 :13983）;
corten_shrink_reclaim 13955; evict 14250。
rmap.c: 守卫 1895-1919 / 2351-2360; __folio_set_anon 1343-1366; __folio_mod_stat
1210-1227; __folio_add_rmap 1222-1307; __folio_remove_rmap 1658-1776;
folio_add_new_anon_rmap 1522-1570 区; folio_lock_anon_vma_read 546-575;
rmap_walk_anon 2885-2930; __rmap_walk_file 2952-2994; rmap_walk_file 2996-3019
（!mapping return :3015）; rmap_walk 3022-3041; try_to_unmap 2308-2320。
其他: memory.c 1120-1180（copy_present_ptes）/ 1499（copy_page_range）/
4182-4184（失效 gate）; swapfile.c 2459-2478（unuse_mm + :2473 note）;
page_alloc.c 1227-1232（free 状态检查）; page_idle.c 95-113; vmscan.c
1360-1420（匿名 swapbacked 回收臂）/ 2360（__reclaim_pages）/ 1379/1399
（folio_alloc_swap）; memory-failure.c 1572-1640（hwpoison_user_mappings,
:1598 LRU 门）; include/linux/rmap.h 397-430（sanity checks 不收 vma）;
include/linux/corten.h 163-188（corten_pte_meta/__resv）;
mm/corten_arena.h 842/864-900（守卫与完成臂声明）。
