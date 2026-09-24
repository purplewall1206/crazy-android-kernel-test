# W1.d 开发报告 · ttu file 真路由（try_to_unmap 窗口 file 页：结构性排除 → 注册表路由）

- 片: MV2 W1.d（W1_NATIVE_RMAP_SPEC.md §3.2 file 侧行 / §5 W1.d 行）
- 基线: mv-a0 @ e85a3ade4442（W1.c 已落，干净）· 未 commit · worktree /home/ppw/linux-6.18-mva
- diff: /home/ppw/cortenmm/patches/r07-w1d.diff · **+669/−0**（生产 +310：rmap.c +15、corten_arena.c +265、mm/corten_arena.h +25、include/linux/corten_arena.h +5；测试 +359：mm/corten_arena_test.c 两个新用例）
- 验证: 三套件 KUnit（on1/on2/off1）+ DAS + =n 折叠全过 · checkpatch **0E/0W** · guest 门（页缓存回收重开判据）留主会话
- 日志: /home/ppw/cortenmm/results/r07/w1d/

---

## 1. 实施内容（对照 task 四项）

### 1.1 ttu 对窗口 file 页：守卫路由升级（task 1）

升级形状 = spec §3.2 的原文设计：钩子落 **try_to_unmap 层**（非 _one 层——corten
region 无 vma，_one 的 pvmw 形状进不去），vma-free，三消费者一源（与 B.2 truncate
门共用同一注册表与同一地址推导）。

**rmap.c（+15）**: `try_to_unmap()` 体首一行 `corten_rmap_ttu(folio, flags)`（=n
折叠为空 inline；=off 时 static-branch 首行返回）。

**mm/corten_arena.c（+265）**:

- `corten_rmap_ttu()`（钩子）: 快门 = 一次 `xa_load(corten_inode_regions,
  folio->mapping)` peek（无注册表的 mapping——整个 legacy 世界——一次探测返回；
  权威加载在 i_mmap 读锁内，peek 悬垂安全）。decline 臂（见 §2）。枚举 =
  W1.b `corten_arena_unmap_file_range()` 同型：`list_for_each_entry(reg->regions)`
  + rpoff 推导 `addr = ar->start + ((folio->index - rpoff) << PAGE_SHIFT)`，
  idle/frozen 跳过 + `percpu_ref_tryget_live` 延寿。
- `corten_rmap_ttu_file_one()`（单页降位事务）: **corten_rmap_swap_out 的锁形状**
  （notifier 窗口先开——可睡眠故不nest desc 锁（R6-2/R-W1-3）→ corten_lock_range
  → query → pte_offset_map_lock），**读臂 install（corten_arena_file_read, R4）
  的逐项逆**:
  - 准入: meta 必须 `CORTEN_FILE_MAPPED`（MAPPED/SWAPPED = file-COW 私有拷贝，
    FILE_EVENT spare 语义，reclaim 不得碰）且 PTE present && !special && pfn ==
    folio_pfn（racing 换代/COW/已降位槽一律让路——§2.1 案 B 要求的事务内校验）;
  - 提交: flush_cache → ptep_get_and_clear → flush_tlb_range（**非 defer 首版**，
    OQ-W1-1 登记）→ pte_dirty 传播 folio_mark_dirty（upstream file 分支）→
    update_hiwater_rss → `mm_counter_file -1` → `folio_remove_file_rmap_novma`
    （W1.a wrapper，mapcount 归还）→ `folio_put`（PTE 引用，install 的"fetch 引用
    即 PTE 引用"的对称）;
  - **metadata 不写**: `CORTEN_FILE_MAPPED` 既是虚拟分配又是常驻形态（读臂本就不
    写 slot transition），降位后槽位 **恒等于 V-B.1 的 pristine 记录形态**——任何
    消费者无法区分"被回收"与"从未 fault"，下一次 fault 直接派发 FILE_READ 重读
    pagecache（旧内容或换代新内容，永不匿名零页）。task 锚③的"PTE none + meta
    FILE_MAPPED"由此天然成立;
  - 计数: `corten_nr_ttu_routes`（新，debugfs `ttu_routes` 行 + KUnit 访问器
    `corten_arena_test_ttu_routes()`）+ 命中时 ARENA_STAT_UNMAP_PAGES。

**`folio_not_mapped` 上报口径对齐上游**: 钩子无任何 corten 返回通道——降位经 novma
wrapper 参与 folio 共享 mapcount，钩子返回后普通 rmap_walk 处理 legacy 映射者，
`folio_not_mapped()`（`!folio_mapped()`）给出与 upstream 完全同口径的终判。
KUnit 锚: 共享（window+legacy 双映射者）页经真实入口后 `folio_mapped()==0`
（升级前恒 1、页永驻——这正是本片消除的结构性排除）。

### 1.2 ttu 匿名腿：M6 现状零改动（task 2）

rmap.c 的改动**只有** try_to_unmap 体首一行；`try_to_unmap_one` 的 M6.T1/T2 守卫
（:1959 prefilter + 完成臂）与 `try_to_migrate_one` 的 :2448 守卫一字未动。钩子对
`folio_test_anon()` 一律静默返回（file-COW 私有拷贝是 anon folio，继续走锚定
匿名腿换出 = M6.T2 现状；原生换出驱动是 W1.e1/e2）。M6 匿名套件
（corten_fault_test.c rmap_guard/swap_roundtrip 等）on1/on2 全绿 = 匿名腿无扰动
的回归证据。

### 1.3 hwpoison/migrate ttu 路径复核（task 3，语义登记）

复核结论：**两者终态在升级前后逐位相同（拒绝·保持驻留·-EBUSY），登记为
"维持排除"，仅拒绝机制的位置/可观测性变化**:

- **hwpoison**: 窗口 file 页是原生 pagecache（在 LRU），能过 memory-failure.c
  `!folio_test_lru → return true` 早退门到达 `unmap_poisoned_folio()`（与 arena
  匿名页的 off-LRU 早退不同）。升级前: ttu 走 i_mmap 只见 legacy、borrowed
  mapcount 令 `folio_mapped()` 保持 1 → -EBUSY。升级后: 钩子对 `TTU_HWPOISON`
  显式拒绝 + `rmap_rejects` 计数（spec §3.2 "其余维持排除"）→ 同一 -EBUSY 终态。
  KUnit 用 `unmap_poisoned_folio` 的非 hugetlb flag 形状
  （TTU_IGNORE_MLOCK|TTU_SYNC|TTU_HWPOISON）断言 PTE 逐位不变 + meta 不变 + 计数。
- **migrate**: W1.b 后 carrier 不在 i_mmap，`try_to_migrate` 的 walker **根本
  产生不了 carrier**——M6.T1 守卫对 file 侧已不可达；结构性防线移到 migrate
  调用方的 `folio_mapped()` 判决（migrate.c:1343/:1549——窗口 mapcount 令页
  不可迁移，compaction putback）。守卫本体仍武装（KUnit 直驱
  `corten_rmap_unmap_one(..., migrate=true)` 断言 refuse+计数）。这正是 spec 的
  意图（"file 普通回收真路由，其余维持排除"）；OQ-W1-5（是否重开迁移）不动。

### 1.4 锁序与红线

- 新锁组合 `i_mmap_rwsem(read) > mmu_notifier window > desc->lock(W,BH) > ptl`
  = F8 已声明边（B.2/W1.b: i_mmap_read > desc(W) > ptl）与 M6 窗口形状
  （window > desc > ptl）的复合；无回边（事务从不获取 i_mmap/notifier），
  **零新锁类**。"folio 锁 + i_mmap read + desc W"组合 W1.b 的
  `unmap_mapping_folio()` 已在产（VM_BUG_ON(folio_test_locked) 的调用契约）。
- **INV6**: 新 PTE 写点 = `corten_rmap_ttu_file_one()` 的 `ptep_get_and_clear`，
  全程在 desc 写锁事务 + notifier 窗口内（spec 红线表预登记的 "W1.b/W1.d/W1.e
  三处写点" 之 W1.d）。set_pte 无（纯清除）。
- **=n 折叠**: 钩子调用点 =n 折叠为空 inline；n-objects 门 14 对象 RC=0 零警告 +
  nm 零 corten 符号；=off1 三套件新用例 `# SKIP`。
- **匿名腿/W1.a-c 主体**: 未动（diff 仅为加法；rmap.c 两个 _one 守卫、注册表、
  wrapper、失效门原样）。
- **不进 LRU/DEV-10**: 本片不改隔离端——窗口 file 页的 pagecache LRU 是原生
  属性（filemap 自加），与窗口无关（spec 红线 5 原文）。

## 2. KUnit 锚（task 4 对账；mm/corten_arena_test.c，suite 内 ok 23/24）

| task 锚 | 用例 | 断言 |
|---|---|---|
| reclaim 系入口 | `corten_arena_test_ttu_file_route` | 真实 `try_to_unmap(folio, TTU_BATCH_FLUSH)`（shrink_folio_list 形状）: ttu_routes +1、rmap_rejects 不动、folio_mapped 0、mapcount 0、refcount −2（window PTE ref + legacy walk 的 PTE ref）、PTE none、meta FILE_MAPPED、mm_counter_file −2、NR_FILE_MAPPED 回基线；refault 后 mapcount +1、内容逐位一致、meta 仍 FILE_MAPPED |
| truncate 系入口（同一 region） | 同上后半 | `unmap_mapping_pages(..., even_cows=true)`（W1.b 枚举）再降一次: mapcount −1、ttu_routes 不动、meta INVALID——两族入口一注册表并存 |
| 共享页 folio_not_mapped 对齐 | 同上 | window+legacy 双映射者一次 ttu 双清（升级前 folio_mapped 恒 1） |
| TTU 分臂覆盖 | `corten_arena_test_ttu_declines` | TTU_HWPOISON 形状拒+计数（rmap_rejects +1）; try_to_migrate(0) 静默不可达（计数不动）; 直驱 M6.T1 守卫 refuse（+1）; 无注册表 mapping 快门（legacy folio 正常 ttu 至 folio_mapped 0，两计数恒定） |
| hwpoison/migrate 前后对比 | 同上 | 两形状后 PTE 逐位 == installed、meta FILE_MAPPED、folio_mapped 1 → 与升级前终态相同（登记 §1.3） |
| spec §5 W1.d ①②③ | 两用例 | ①零开销快门 ②decline 计数 ③降位事务后 PTE none + meta FILE_MAPPED + 重读一致 |

## 3. 验证结果

| 门 | 结果 |
|---|---|
| make -j8（=y） | RC=0（收尾 rebuild 再 RC=0，bzImage 就绪给 guest 门） |
| KUnit corten=on（on1） | **corten 24/0/1 · corten_arena 104/0/0 · corten_fault 31/0/2** 全绿（新用例 `ok 23 ttu_file_route` / `ok 24 ttu_declines`） |
| KUnit on2（flake 复跑） | **同数全绿**（`ok 23/24` 复现） |
| KUnit corten=off（off1） | 首跑 1 例 `corten_test_txn_uninstall_interlock` FAIL（**既有 flake 形态**: corten_test.c 未触碰文件，CPU-pin kthread 10s 有界等待超时，30s runtime 签名; 与 W1.d 无耦合面）→ 复跑 **25/0/0 · 24/0/80 · 7/0/26 全绿**，新用例 `# SKIP`（flake 日志留档 kunit-off1.flake.log） |
| DEBUG_ATOMIC_SLEEP | 全绿（新事务的 flush-under-desc(W,BH) 无原子睡眠） |
| =n 折叠（14 对象 + nm） | RC=0 零警告 + 零 corten 符号 |
| checkpatch（patch 模式） | **0 errors, 0 warnings, 729 lines** — "ready for submission"（注: `checkpatch -f` 直怼 diff 文本会误报 hunk 空行，`git diff --check` 亦净） |

## 4. 登记与披露

- **语义登记（task 3）**: hwpoison/migrate 终态不变（-EBUSY·拒绝·保持驻留），
  变化仅两点: hwpoison 的拒绝从"结构性（mapcount 卡住）"变为"显式拒绝+计数"，
  migrate 的防线从 M6.T1 守卫移至调用方 folio_mapped 判决（守卫对 file 侧不可达,
  对匿名腿仍武装）。
- **OQ-W1-1（defer/batch 形状）**: 首版非 defer——钩子内联 flush_tlb_range，忽略
  TTU_BATCH_FLUSH 的 should_defer_flush（eager flush 恒正确，仅少摊销）；
  T5 型实测后再放开（与 R-W1-3 缓解一致）。
- **OQ-W1-5（重开迁移）**: 未动，本片复核支持维持 decline。
- **mlock**: 钩子镜像 M6.T1 prefilter 的 VM_LOCKED 检查（carrier 携带判决，
  TTU_IGNORE_MLOCK 通行），拒绝计入 rmap_rejects。
- **规模注**: task 估 ~260 行；实落生产 +310（含计数器/render/访问器 ~35 与
  注释密度）+ 测试 +359（两个全链用例）。W1.b/c 同比例超注（spec 对测试行估
  持续偏低），无删除性偏差。
- **W1.f 衔接**: `corten_nr_ttu_routes`/`ttu_routes` 即 W1.f 计划的
  `ttu_hook_unmaps` 前身（届时更名或并列均可）。
- **guest 门（留主会话）**: spec W1.d 判据——压 memcg → window-mapped file 页被
  回收（file 页 workingset 计数验证，swap.so 无关）→ 重新 fault 内容一致；
  compaction 对窗口 file 页迁移仍拒绝（imap/migrate 零 hit + 页驻留）。

## 5. 文件清单

- /home/ppw/linux-6.18-mva/mm/rmap.c（try_to_unmap 体首钩子调用）
- /home/ppw/linux-6.18-mva/mm/corten_arena.c（corten_rmap_ttu /
  corten_rmap_ttu_file_one / ttu_routes 计数器-render-访问器）
- /home/ppw/linux-6.18-mva/mm/corten_arena.h（声明 + =n 空桩）
- /home/ppw/linux-6.18-mva/include/linux/corten_arena.h（KUnit 访问器声明）
- /home/ppw/linux-6.18-mva/mm/corten_arena_test.c（两个 W1.d 用例 + 注册）
- /home/ppw/cortenmm/patches/r07-w1d.diff（导出，759 行）
