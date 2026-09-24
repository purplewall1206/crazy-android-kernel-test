# W1.e1 开发报告 · 匿名页原生换出驱动第一片（R-W1-1 最高风险点）

- 片: MV2 W1.e1（W1_NATIVE_RMAP_SPEC.md §3.4 签名 + §5 W1.e 拆片的 e1 半：驱动 + evict(debugfs) 腿；shrinker 腿 + 守卫降级留 W1.e2）
- 基线: mv-a0 @ 5ef7d968cf47（W1.d 已落，干净）· 未 commit · worktree /home/ppw/linux-6.18-mva
- diff: /home/ppw/cortenmm/patches/r07-w1e1.diff · **+616/−37**（5 文件；生产：mm/corten_arena.c +439/−37 大头、mm/corten_arena.h +34、include/linux/swap.h +2、mm/vmscan.c +9/−1；测试：mm/corten_fault_test.c +169 两个新用例）
- 验证: make -j8 RC=0 · 三套件 KUnit on1/**on2**/off1(flake 复跑)/DAS/最终镜像复跑全绿 · =n 折叠 vmlinux 零 corten 符号 · checkpatch **0E/0W**（764 行 "ready for submission"）· guest zram 往返门留主会话（roundtrip 锚在 guest 有 swap 时自动实跑）
- 日志: /home/ppw/cortenmm/results/r07/w1e1/
- 生产本体: `corten_swap_out_driver()`（corten_arena.c:14081）——**逐位镜像 vmscan.c shrink_folio_list 匿名臂**（实测行号见 §2 对照表，无漂移）+ **M6 swap 事务原样调用零改动**（INV6）+ evict 腿 (addr,folio) pick 重接

---

## 1. 实施内容（对照 task 三项 + 缓解三件）

### 1.1 驱动本体 `corten_swap_out_driver(mm, addr, folio)`（task 1；缓解①③的落点）

形状 = spec §3.4 原文：**前后腿自持 + 事务原样**。调用方契约 = pick 引用（无锁——
trylock 是驱动的 vmscan.c:1174 腿）；返回 true = folio 已释放（内容活在事务装的
swap PTE 后面），false = 保持驻留（pick 引用驱动内归还，全部 keep 臂计数）。

- **锁外 prep（vmscan 匿名臂镜像）**: 形状门（anon+swapbacked+order-0+pin，1360-1365
  镜像；1366-1378 大页臂对 order-0 pick 结构性不可达）→ carrier 解析
  （`corten_arena_lookup_get` + `corten_arena_anchor_vma`，W1.d file 钩子的同款形状；
  mlock 判决镜像 M6.T1 prefilter，计 rmap_rejects）→ `folio_alloc_swap(folio,
  __GFP_HIGH|__GFP_NOWARN)`（vmscan.c:1379 逐字；本树一步完成 entry + memcg charge +
  swapcache 入册 + folio->swap）→ `folio_mark_dirty`（1413 MADV_FREE 注记镜像）。
- **事务（INV6 红线）**: 驱动自开 notifier 窗口（R6-2，walker 的职责移交）→
  **`corten_rmap_swap_out()` 原样调用，本体零字节改动**（defer=false 即内联 flush，
  OQ-W1-1 非 defer 首版与 W1.d 同解）→ 窗口关 + arena 引用还。
- **事务后**: pin 复查（1479-1481）→ `folio_mapping`（1483）→ 脏臂
  `folio_clear_dirty_for_io` + `folio_set_reclaim` + **`swap_writeout(folio, NULL)`**
  （pageout/writeout 的 aops 腿——内含 folio_free_swap entry-reuse 捷径、
  arch_prepare_to_swap、zeromap、zswap、folio_start_writeback+unlock，与上游逐位同；
  zram 同步写在 submit 内结算）→ PAGE_SUCCESS 重锁序列（1547-1563 镜像）→
  **`__remove_mapping(mapping, folio, true, NULL)`**（1629 镜像：freeze(2)=cache+pick、
  decache、put_swap_folio、memcg swapout、workingset shadow 全在上游原函数内）→
  free_it 四件套（1635-1647 镜像：unqueue_deferred_split + folio_batch +
  mem_cgroup_uncharge_folios + try_to_unmap_flush + free_unref_folios）。
- **keep 面**: activate/keep 镜像（1659-1677）——folio_free_swap 的 swap-full 臂
  （1661-1663；事务假路径上 entry 只剩 cache 引用，arm 语义安全，见函数头注释论证）
  + 解锁 + 归还 pick 引用（**无 folio_putback_lru，DEV-10**）+ driver_kept 计数。
- **引用收支总账**（R-W1-1 的核心，函数头注释全文）: pick(1) + folio_alloc_swap 的
  cache(+1) = 2 ≡ 上游 isolation+cache；事务 folio_put_refs 掉 PTE 引用；
  `__remove_mapping` freeze(2) 后 free 批释放 pick。任何 keep 臂驱动自还 pick。
  remove 失败（唯一竞态对手 = 并发 swap-in 抢引用）= 上游同款 keep：folio 留
  swapcache、entry 共享，下轮 pick 收敛（swapcache 成员短路 entry 腿）——上游靠
  LRU 重扫愈合，本驱动靠下一轮 evict/shrink，语义等价（§4 登记）。
- **计数**: `corten_nr_driver_swapped` / `corten_nr_driver_kept`（debugfs
  `driver_swapped`/`driver_kept` 行；W1.f 计划的 driver_swaps 即前者）。

### 1.2 与 ttu 的关系：两通道并存（task 2）

- **evict(debugfs) 腿**重接：pick 从 folio->lru 借链改为 `struct corten_swap_pick`
  的 (addr, folio) 对（GFP_NOWAIT 分配，失败计 shrink_skipped 跳过），排空循环直驱
  `corten_swap_out_driver`，**`__reclaim_pages`/ttu 从此对 evict 不可达**。
- **shrinker 压力腿（W1.e2 前）不动**：仍走 aging→pick(folio->lru)→
  `corten_shrink_reclaim`/`__reclaim_pages`→ttu 匿名臂（M6.T2 完成臂继续服务它）；
  rmap.c 两个守卫一字未动，降级断言是 W1.e2 的内容。两通道并存口径写入
  evict/M6.T3 两处块注释与 corten_arena.h 声明注释。
- pick 门 `!vma` 跳过维持原状（V-A.1 无锚窗口不可 pick）——驱动虽不需要 rmap 找地址，
  但 flush 形状仍需 carrier（W1.d 先例），该门的取消与 shrinker 腿同批（W1.e2）。

### 1.3 KUnit 锚（task 3；缓解②）

mm/corten_fault_test.c 两个新用例（复用 ft_swap_up 设备门先例——无盘 qemu 本地
SKIP/实跑互补，guest swapon zram 后套件重跑即实跑）:

| 用例 | 门 | 断言 |
|---|---|---|
| `corten_fault_test_driver_keep_noswap` (ok 33) | 无 swap 区（本地实跑；swap 机器 skip） | entry 腿失败 → keep 全对称：返回 false、refcount 回 1（PTE 引用）、mapped/mapcount 不动、非 swapcache、PTE 逐位不变、meta MAPPED、ANONPAGES/SWAPENTS 惰性、driver_kept+1、driver_swapped==0 |
| `corten_fault_test_driver_swap_roundtrip` (ok 34) | 有 swap 区（guest 门；本地 SKIP） | 全链收支逐步断言：前置 refcount 1/mapcount 1/无 swap 状态 → 驱动 true（成功即释放，此后零 folio 解引用）→ swap PTE 与 meta entry 逐位一致（INV7 对）+ perm 契约存活 → ANONPAGES−1/SWAPENTS+1/driver_swapped+1 → `swap_duplicate` 成功证明 entry 恰剩 PTE 引用（cache 成员已释放）→ 缺页换回：PTE present + 内容逐位（0x5c 两点）+ meta MAPPED + perm + swapins+1 → 终账 ANONPAGES/SWAPENTS 归零 |

### 1.4 核心文件触碰（最小可见性改动）

- **mm/vmscan.c**: `__remove_mapping` 去 static + 注释（**零语义改动**）；声明入
  include/linux/swap.h（`__reclaim_pages` 为 corten 导出的同款先例——corten 内建，
  无需 EXPORT）。驱动由此获得上游原版的 freeze/decache 竞态协议，不复制。
- **mm/corten_arena.h**: 声明 + =n 空桩（返回 false；=n 时 evict 入口本身被
  static-key 门挡死，桩不可达，纯折叠形态）。

## 2. 逐位镜像对照表（R-W1-1 缓解①；vmscan.c 行号为 W1.e1 实测，spec 引 1360-1420 无漂移）

| vmscan.c | 上游腿 | 驱动腿（corten_arena.c:14081） | 状态 |
|---|---|---|---|
| 1174 | folio_trylock | 驱动内 trylock（调用方只持 pick 引用） | 逐位（位置移入驱动） |
| 1179-1193 | hwpoison 门 | 窗口匿名页 off-LRU，hwpoison 结构性排除（M6 口径） | 排除项（登记） |
| 1202 | folio_evictable（LRU 族） | 非 LRU 页，不适用 | 排除项（DEV-10） |
| 1205/1329-1341 | may_unmap/references 门 | 调用方 aging（M6.T3 两段；evict=force） | 调用方承担（前置） |
| 1284-1327 | writeback 隔离等待 | pick 门 `folio_test_writeback` 拒（14266 现状） | pick 侧承担 |
| 1343-1353 | demote pass | 不适用 | 排除项 |
| 1360 | anon+swapbacked 判定 | 同款双判定 | 逐位 |
| 1362-1363 | `__GFP_IO` 门 | shrinker scan_objects 已门 __GFP_IO；evict 管理通道 | 调用方承担（登记） |
| 1364-1365 | folio_maybe_dma_pinned | 同款（锁下复查） | 逐位 |
| 1366-1378 | 大页 split 臂 | pick 只收 order-0 | 结构性不可达（登记） |
| 1379 | `folio_alloc_swap(__GFP_HIGH\|__GFP_NOWARN)` | 同字面调用 | **逐位** |
| 1379 失败臂 | THP split 重试 → activate | 直接 keep_locked（order-0 无 split 可试） | 简化（登记） |
| 1402-1413 | MADV_FREE 无条件 mark_dirty | 同款（仅 !swapcache 分支内，与上游同位） | 逐位 |
| 1442-1470 | try_to_unmap + folio_mapped 判决 | **M6.T2 事务原样**（`corten_rmap_swap_out` 零改动）+ 驱动自开 notifier 窗口（R6-2） | 替换点（本片本体） |
| 1479-1481 | 卸映射后 pin 复查 | 同款 | 逐位 |
| 1483 | `mapping = folio_mapping()` | 同款（1563 IO 后重读亦同） | 逐位 |
| 1484-1526 | 脏臂 + `try_to_unmap_flush_dirty` + pageout 前门 | `folio_clear_dirty_for_io` 失败 = PAGE_CLEAN 直落 removal；flush_dirty 无批可刷（内联 flush） | 逐位（flush 形状差登记） |
| writeout() 667-695 | `folio_set_reclaim` + aops->writepage + ACTIVATE/负值臂 | `folio_set_reclaim` + **`swap_writeout(folio, NULL)`**（aops swap 臂本体）+ ACTIVATE 臂（clear_reclaim+keep）+ 负值臂（handle_write_error 形状内联：lock/set_error/unlock→keep） | 逐位 |
| 1540-1563 | PAGE_SUCCESS：writeback/dirty 复查 + 同步写重锁 | 同四段（`!writeback` clear_reclaim、writeback keep[同步设备不可达，镜像保留]、dirty keep、trylock+双查+mapping 重读） | 逐位 |
| 1593-1613 | buffers/private 释放 | 窗口匿名页无 private | 排除项（登记） |
| 1615-1628 | lazyfree 臂（freeze(1)） | swapbacked 门上强制，不可达 | 排除项（登记） |
| 1629-1631 | `__remove_mapping(mapping, folio, true, target_memcg)` | **同函数**（去 static；target_memcg=NULL = 全局回收同参） | 逐位（原函数直调） |
| 1633-1647 | free_it：unlock + unqueue_deferred_split + batch + uncharge + flush + free_unref | 同五件（batch of one） | 逐位 |
| 1659-1674 | activate 臂（set_active/PGACTIVATE） | LRU 族，DEV-10 排除 | 排除项（登记） |
| 1661-1663 | keep 时 swap-tight 的 folio_free_swap | 同款（自护：事务假路径 entry 仅 cache 引用才可能释放） | 逐位 |
| 1675-1683 | keep_locked/keep + putback_lru | unlock + 驱动归还 pick 引用（**无 putback**）+ driver_kept | 逐位减 LRU（DEV-10） |

**语义陷阱（镜像对照发现，全部登记/处置）**:
1. **`folio_alloc_swap` 已折叠 `add_to_swap`**: 本树 swapfile.c:1442 的
   folio_alloc_swap = entry 分配 + `mem_cgroup_try_charge_swap` +
   `swap_cache_add_folio`（设 folio->swap、+1 引用、NR_SWAPCACHE）。spec §3.4
   "folio_alloc_swap + add_to_swap" 两步提法是旧形状——prep 腿一字面镜像 1379 单
   调用即完整，无第二步可写（报告登记，防止 W1.e2 误加双步）。
2. **事务假 keep 的 entry 语义**: 事务所有假臂要么未动 entry、要么 swap_free 自愈
   ⇒ 事务假 ⇒ entry 只剩 cache 引用 ⇒ keep 面的 folio_free_swap 镜像臂此时释放
   entry 安全（不存在活 PTE）；而 remove 失败 keep 的 entry 是 PTE+cache 双引用，
   folio_free_swap 对 cache-only 检查自然 no-op——同一镜像臂两个方向都安全。
3. **swap_writeout 的解锁纪律**: 六个返回臂中 I/O 系全部 folio_unlock（含
   zeromap/zswap 臂），仅 ACTIVATE 臂持锁返回——驱动 keep 臂按锁态分叉
   （keep_locked vs keep），逐位跟上游 1547-1577 的分叉。
4. **驱动 keep 无 LRU 重扫的愈合差**: 上游 remove 失败 keep 回 LRU 下轮重扫；驱动
   keep 靠下一轮 evict/shrink 重 pick（swapcache 成员在 1361 镜像位短路 entry 腿，
   事务原样重跑）。收敛路径存在且等价，但频率上依赖回收通道再次到达——guest 并发
   fault 压测门覆盖（主会话）。
5. **defer-flush 缺席**: 批 flush 静态量（tlb_ubc）是 rmap.c 私有，驱动为自身批边界
   内联 flush（每 pick 一次），OQ-W1-1 非 defer 首版与 W1.d 同解；W1.e2 接 shrinker
   腿时若实测开销显著再议批化。

## 3. 验证结果

| 门 | 结果 |
|---|---|
| make -j8（=y） | RC=0；触碰文件零警告（bzImage 就绪给 guest 门） |
| KUnit on1（corten=on） | **corten 24/0/1 · corten_arena 104/0/0 · corten_fault 32/0/3** 全绿；新锚 `ok 33 driver_keep_noswap`（实跑通过）/ `ok 34 driver_swap_roundtrip # SKIP（无 swap 区，设计内）` |
| KUnit on2（flake 复跑） | **同数全绿**（24/0/1 · 104/0/0 · 32/0/3） |
| KUnit off1（corten=off） | 首跑 1 例 `corten_test_txn_uninstall_interlock` FAIL（**既有 flake 形态**：W1.d off1 同签名登记过，CPU-pin kthread 10s 有界等待，corten_test.c 本片未触碰）→ 复跑 **25/0/0 · 24/0/80 · 7/0/28** 全绿，新锚双 `# SKIP driver requires corten=on`（flake 日志留档 kunit-off1.log/rerun.log） |
| DAS（DEBUG_ATOMIC_SLEEP 变体） | 构建零本片警告 + **24/0/1 · 104/0/0 · 32/0/3** 全绿（驱动的 folio 锁/arena 引用下睡眠点全部合法，无原子段睡眠） |
| =n 折叠 | CONFIG_CORTEN_MM=n 全量 RC=0；5 个消费对象（vmscan/rmap/memory/swapfile/page_io）nm 零 corten 符号；**vmlinux nm 零 corten 符号** |
| checkpatch（patch 模式） | **0 errors, 0 warnings, 764 lines** — "ready for submission"；`git diff --check` 净 |
| 最终镜像复跑（注释修复后重构建） | 24/0/1 · 104/0/0 · 32/0/3 全绿（ shipped 位与绿跑一致） |
| guest 门（留主会话） | spec W1.e 判据：M6.T3 压力门复跑（RSS 降/swap.so 增/INV7 零漂移）+ **zram 往返 checksum** + driver_swap_roundtrip 在 guest swapon 后套件重跑自动实跑（断言含换回内容逐位）+ driver_swapped/driver_kept 观测 |

## 4. 登记与披露

- **INV6**: 本片对 M6 事务本体零改动（`corten_rmap_swap_out`/`corten_swap_out`/
  `corten_swapin_sync_meta` 逐字节原样）；驱动的新 PTE 写点 = 零（事务内的写点
  原样，驱动只加了窗口与前后腿）。vmscan.c 唯一改动 = `__remove_mapping` 去 static
  （`__reclaim_pages` 先例；内建调用无 EXPORT 需求）。
- **R8（folio_remove_anon_rmap_novma 驱动内置翻转）有意未做**: spec §3.1 R8 行写
  "W1.e 驱动内置"，但 task 红线"本片不改事务本体——只加驱动层"优先——驱动原样调
  事务，事务内仍是 `folio_remove_rmap_ptes(carrier)`（W-1 期锚仍在，数值上与 novma
  wrapper 对 order-0 完全等价，F1：vma 参数只影响大页 mapcount/mlock 检查）。翻转
  与守卫降级同批走 W1.e2（届时事务的未来——收敛为驱动私有或参数化——一并裁决）。
- **refcount 语义注**: ft fixture 的 mapcount==1 = `folio_mapcount()` 口径（raw
  `_mapcount` 0），移除后归 0——与 W1.a wrapper 的借用约定一致。
- **keep 愈合通道**: remove 失败的 swapcache-resident keep 态靠下一轮 pick 收敛
  （§2 陷阱 4）；本地锚覆盖 keep 对称性，并发 swap-in 竞态臂由 guest 并发 fault
  压测覆盖。
- **uptodate 前置**: `folio_alloc_swap` 的 VM_BUG_ON(uptodate) 与 ttu 路径同门
  （m6t34 的 67565 页实跑已证生产页在 pick 时满足；测试锚镜像 ft_swap_out 的
  显式 `__folio_mark_uptodate`）。
- **规模注**: spec 估 e1 ~200 行；实落生产 ~518（含 ~180 行镜像对照注释——R-W1-1
  缓解要求对照表随码）+ 测试 +169。超注部分为注释密度与双锚，无删除性偏差。
- **W1.e2 衔接**: shrinker 腿重接（`corten_shrink_reclaim` 届时消亡）+ rmap.c
  守卫匿名臂降级断言 + `!vma` pick 门重开裁决 + R8 翻转 + spec §5 W1.e 锚③④
  （pick 配对完整性已被 e1 的 (addr,folio) 结构预置；ttu 匿名臂计数恒零断言待 e2）。

## 5. 文件清单

- /home/ppw/linux-6.18-mva/mm/corten_arena.c（corten_swap_out_driver :14081、
  corten_swap_pick :14377、walk force 分支、corten_arena_evict_mm 重接 :15060、
  driver_swapped/driver_kept 计数+render :318/:3031）
- /home/ppw/linux-6.18-mva/mm/corten_arena.h（声明 + =n 桩）
- /home/ppw/linux-6.18-mva/mm/corten_fault_test.c（driver_keep_noswap :2913 /
  driver_swap_roundtrip :2968 + 注册）
- /home/ppw/linux-6.18-mva/mm/vmscan.c（__remove_mapping 去 static :743）
- /home/ppw/linux-6.18-mva/include/linux/swap.h（__remove_mapping 声明 :408）
- /home/ppw/cortenmm/patches/r07-w1e1.diff（导出，793 行）
