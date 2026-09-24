# M6.T3+T4 · shrinker 压力通道 + 观测面 · 验证报告

日期: 2026-09-20（~08:00 起, 通宵班）
合同: M6_RMAP_SPEC.md T3/T4 + 顺带修 M5.T3 一行遗留
基座: 主树 HEAD `b51754002f2f` (M6.T2 swap 事务) → worktree
`/home/ppw/linux-6.18-m6t34` 分支 `m6-t34`。**未 commit**（review/maintainer 后续）;
快照 diff = `patches/r07-m6t34.diff`（与 worktree `git diff` 字节一致,
5 文件 +1563/−126）。

---

## 1. 落地内容

### T3 shrinker 压力通道（spec sec 2.1 D2）

1. **per-mm registry**: `corten_mm_registry`（mm/corten_arena.c 全局 RCU 双链,
   节点 = `corten_mm_state.shrink_reg`）。`corten_arena_state_create()`
   在 publish 的同一 `corten_arena_alloc_lock` 临界区内
   `list_add_tail_rcu`（"要么全不可见要么全是 victim"）; `corten_arena_mm_exit()`
   先 `list_del_rcu` + **一次 `synchronize_rcu()`** 再 `state_free`
   （shrinker 读者只安全地携带节点指针跨越自己的 RCU 段, 无需 per-node
   refcount; 这是 exit 路径唯一一次 synchronize）。registry 走查每次 RCU 段内
   `mmget_not_zero()` 钉住 mm（mm_users>0 ⇒ exit_mmap/registry 摘除/
   state 释放不会发生; MGLRU `walk_mm` 同款姿态）。
2. **精确常驻计数（count_objects 基座）**: `struct corten_ptdesc` 新增
   `nr_mapped`/`nr_swapped`（long, desc 写锁内维护, 无锁读者 READ_ONCE）,
   在 **仅有的 4 个状态迁移收口点**维护（mm/corten.c: `corten_map`/
   `corten_swap_out`/`corten_unmap`/`corten_txn_meta_drop`; fork 镜像走
   map/mark ⇒ 自动正确; zap 走 corten_unmap ⇒ 同）。由构造精确, 无漂移。
   `corten_mm_state_pages()` 逐窗短 RCU 段（T2 RCU 饥饿教训）+
   `corten_ptdesc_get` pin 协议读快照求和。
3. **两遍 young 位 aging 事务**: 共享 victim walker
   `corten_arena_shrink_walk()`: desc 写锁（`corten_lock_range`）内逐页
   query → ptl 内 `ptep_clear_young_notify()`（**镜像 vmscan.c:3717**,
   ptl 嵌套方向与 MGLRU walker 同向, 但处于事务覆盖锁之下——每个 arena PTE
   写都是事务）。pass 1（age slice, 轮转游标扫新窗, 清 young, 置
   `state->shrink_aged` 标记）与 pass 2（eval slice, 评估上一扫描代已
   标记的窗: young 回升者豁免、cold 者 isolate）。**游标跨扫描轮转、单次
   扫描内不回卷**（同扫描绝不重复 aging 同一窗。r07 复审角案登记: 游标
   枯竭 cursor=0 返回后, 同扫描第 2 片可重 age 同批窗——仅重复清位、无
   正确性影响, 已登记 STATE; 见 §7）。MGLRU 维持结构 skip:
   全 diff 零 `folio_add_lru`; lru_gen walker 对 arena folio 依旧结构性
   不可达（`folio_lru_gen() < 0`）。
4. **批量修（T2 遗留 ~1.4-6ms/页 根因）**: victim 拾取 = 每 mm
   `shrink_lock` trylock（与 shrinker 互斥, 防同 folio 双链上岛）分片
   （≤16 窗/≤512 候选页/片, 自旋持有有界）+ **每次扫描一次
   `__reclaim_pages()` 大批**（folio_alloc_swap → try_to_unmap → ttu 守卫 →
   M6.T2 swap-out 事务 → swap_writeout → __remove_mapping 全上游）+
   旋转游标消灭"每次调用从 frame 0 重扫" + 候选页计预算（稀疏窗不烧预算）。
   zram 为 BLK_FEAT_SYNCHRONOUS 设备（`swap_writepage_bdev_sync`,
   页内联 submit_bio_wait）——批内自然结算, 单调用完成换出（T2 guest 行为
   复现）。
5. **memcg 维度**: `SHRINKER_MEMCG_AWARE` 注册; count/scan 用**活** memcg
   （`get_mem_cgroup_from_mm` + `mem_cgroup_is_descendant`, 尊重运行时
   migration, 不存过期指针）。corten=off 不注册（红线 6）。
6. **debugfs evict 复用批量化**: `corten_arena_evict_mm()` 改走共享 walker
   （force 形状: 不清 young、不喂 aging 标记）+ shrink trylock（忙 =
   shrinker 正在做这批活, `-EBUSY` + evict_busy 计数）+ 游标续扫。
   （r07 复审 B1 修正: 每片 reclaim 前丢 `shrink_lock`、下片
   `spin_trylock` 重取, 与 scan 同形——原实现 reclaim 在自旋锁内跑, 见 §7。）

### T4 观测面

- `arena_stats` 新行: `shrink_scans`（scan 调用数）/ `aging_passes`
  （pass-1 窗龄数=两遍 aging 事务开启数）/ `shrink_swapped`（shrinker 路径
  换出数, **精确口径 = picked − kept**, 并发免扰）/ `shrink_skipped`
  （walker folio 级拒绝: pin/writeback/order）/ `evict_busy`
  （evict/shrinker 互斥计数）+ `resident_pages`/`swapped_pages`
  （per-desc 活值总和: __resv SWAPPED 槽位统计）+
  `swapout_rate`/`swapin_rate`（两次读取间 pages/s 差分, 快照自旋锁保护）。
  `guard_rejects`=既有 `rmap_rejects`、`reap_gate_hits`=既有 `reap_skips`
  （T1/T2 已有, 本片沿用不重造）。
- **口径核对（脚本内建, `bench/share/m6t3-shrink-test.sh` 步骤 4）**:
  冻结采样点上 **memory.swap.current（v2 的 swap 计费, 字节）== smaps Swap
  （shadow-VMA 的 swap PTE 计数, kB）== arena ledger swapped_out（页）**
  三方一致（run13: 69388p vs 277552kB=69388p vs 69356p, 容差内）。
  **发现并核实**: 本树 v2 `memory.stat` **不含 "swap" 行**（swap 计费在
  `memory.swap.current`; memory.stat 的 swap 行是 v1-format 专属）——
  规划口径 "memory.stat swap" 在本内核 = `memory.swap.current`。
  anon 侧: arena 页 charge 在 `corten_arena_folio_prealloc()` 的
  `mem_cgroup_charge()`（T2 前即有）⇒ memory.stat anon 覆盖 arena 常驻页 ✓
  （run13 sample: anon≈2.4MB 与 STOP 后 RSS 3.8MB 同量级, 大头已转 swap）。
  fork 后: smaps Swap 对 swap PTE 照常计数（copy_nonpresent_pte 复制
  + swap_duplicate）, SwapPss 同值（T2 已验, 本轮不变）。

### 顺带修复

- **M5.T3 遗留（1 行）**: `corten_arena_test_gup_state4_pin_zap` 尾部
  `folio_put` ×2 在 count==1 时多放一次 → 删多余 put + 注记（mm/
  corten_arena_test.c）。
- **T2 潜伏 bug（压力通道使其高频化, 本轮必修）**: fork 在存在
  CORTEN_SWAPPED 槽时**必然 abort**——`corten_arena_fork_copy_window()`
  的 default 臂对 Invalid→SWAPPED 调 `corten_mark()`, 状态机拒绝
  （swap-out 只走 `corten_swap_out()`, MAPPED 槽专属）。新 replay 臂:
  **`corten_swap_replay()`**（corten.h/corten.c: 只接受非常驻槽 +
  SWAPPED 载荷, 维护 M6.T3 计数）+ copy_window `case CORTEN_SWAPPED`
  （校验子 PTE == 镜像 entry, WIPEONFORK 留白）+ KUnit
  `corten_arena_test_fork_swapped`（双侧 meta/entry/INV7/registry 计数）。

### 机理级修复（本轮 guest/KUnit 驱动, 全部带根因）

1. **registry 走查 skip 不推进**（首轮 KUnit 挂死）: 渲染器/count 的
   `for(;;)` 重入 `corten_registry_pin` 永远重拾前 16 个 mm →
   `skip += found` 补齐（4 处: 渲染器/count_mms/两个测试钩子）。
2. **memcg shrinker-map 位从未置位**（首轮 guest: 250M 上限下 OOM 而
   shrink_scans=0）: memcg 回收只扫 `shrinker_map` 置位的 shrinker;
   arena 页入 memcg 时无人置位 ⇒ 永不被扫。修 = charge 时
   `set_shrinker_bit()`（`corten_arena_folio_prealloc()` + swapin 路径,
   与 THP deferred-splitter 同款）; 修后压力下 945 次扫描/5s。
3. **refault livelock（OQ-M6-8 落地确认）**: x86 `pte_sw_mkyoung()` 为
   no-op ⇒ swap-in 安装的 PTE 非 young, 两遍 aging 会在首次复用前把它
   立刻换出 ⇒ 持续压力下 swap-in/out 风暴、工具饿死。修 = swap-in 安装
   `pte_mkyoung()`（refault grace, 对齐上游 swap cache refault 的
   folio_mark_accessed 恩典; arena 页无 LRU 可依托, 必须显式）。
4. **扫描预算上限**（用户态可感延迟）: 每片 ≤16 窗/≤512 候选页、每调用
   ≤2 片, shrink_lock 自旋持有有界。

---

## 2. 验证矩阵

- **=y 全量构建**（最终内核 #13）: 零新增警告（全 log 仅基线既有
  `cpuidle_enter_state` objtool 与 `memblock_end_of_DRAM` EXPORT_SYMBOL
  两条, r01 起即在）。
- **KUnit**（无盘 qemu, `kunit.filter_glob=corten*`）:
  - corten=on ×2（最终内核 #10 同源）: **24/0/1 + 48/0/0 + 30/0/2 全绿**
    两跑皆然;
  - corten=off ×1: **25/0/0 + 18/0/30 + 6/0/26 全绿**（新用例按设计 skip）;
  - **lockdep 变体（PROVE_LOCKING, 最终代码）**: **24/0/1 + 48/0/0 + 30/0/2
    全绿, 零 lockdep/oops 签名**。
  - 新用例 +4（arena 套件 44→48）: `shrink_registry_count`（registry
    登记/计数/活值 totals/T4 渲染行）、`shrink_aging_two_pass`（pass-1
    标记 → young 页豁免 + cold+pin 页到达拾取门[shrink_skipped 观测]、
    epoch 清位断言）、`shrink_rotation`（双窗轮转 4 扫描序列）、
    `fork_swapped`（swap 后 fork 的 replay 臂）。
  - 既有 flake: `corten_test_txn_uninstall_interlock`（M7 已登记）在
    中途一轮出现 1 次（on1 第一次跑, "worker A never acquired the lock
    phase=5"）, 与 T2 轮同签名、基线内核可复现; 最终矩阵两跑均绿。
- **=n（CORTEN_MM=n）八对象**: memory/mmap/migrate/rmap/swapfile/gup/
  oom_kill/arch(x86)fault 零错误零警告（`m6t34-build-n.log`）。
- **checkpatch --strict（5 改动文件）**: 新增 **0E/0W**
  （`results/r07/checkpatch-m6t34.txt`; arena.c 余 1W = 基线既有 braces
  条, 对基线文件逐字复现核对通过）。

## 3. Guest 判据（spec T5 前置）—— `results/r07/m6t34-guest/`

环境: trixie VM 4G/8vCPU KVM, zram 2G lz4 prio100, cgroup
`memory.max=250M`（瞬时压窗）, arena_stress 4 线程 512M mixed seed42。
脚本 `bench/share/m6t3-shrink-test.sh`, 最终跑 **run13 fails=0**
（`m6t34-guest/m6t3-run13.log`）。

- **shrinker 自然触发换出（无 debugfs 触发）**: memory.max 压至
  RSS−40M 后 **5s 内 945 次 shrink_scans / 972 aging_passes,
  59356 页经 shrinker 换出**（shrink_swapped 精确口径）, swapout_rate
  峰值 ~5-10k 页/s; **RSS 281MB → 3.8MB**; zram used 20KB → 3.7MB
  （magic 图案 lz4 高压缩）。
- **读回校验和一致**: 释放压力后工具跑完 **verdict PASS, ops=984511,
  errors=0 op_errors=0**; **swapins=69356 == swapped_out=69356**。
- **计数对账闭合**: `swapped_out 69356 == swapins 69356 + zap_swap_frees 0`;
  退出后 zram used 精确回基线 20480。
- **T4 口径核对（冻结采样点三方一致）**: memory.swap.current
  284213248B（=69388p）== smaps Swap 277552kB（=69388p）== ledger
  swapped_out 69356p（Δ32 页 < 容差 1643; 差值为采样瞬间在飞事务）。
  另确认: v2 memory.stat 无 swap 行（口径 = memory.swap.current, 见 §1）。
- **批量数字**: 冷态 populate 批 `evict 10000` = **3.5s（2828/s）**;
  暖态（run11, zram 聚簇已热）= **822ms/10k（12150/s）**;
  压力排空后的批 = 776 页/17ms。**corten 侧批量化已兑现**（T2 估算
  ~1.4ms/页、64k 分钟级 → 现 82-350µs/页全含）; 残余为
  **zram 同步写逐页**（page_io.c `swap_writepage_bdev_sync`:
  BLK_FEAT_SYNCHRONOUS ⇒ 页内联 submit_bio_wait, 上游对 zram 的固有形态,
  kswapd 同样受限）——再往上要动 page_io 批提交, 属 T5/上游范围, 不在本片。
- **run_mode_smoke**: 24/26 PASS; `released-arena-unmapped/gone` 2 例失败
  **与 T1/T2 基线逐字相同**（已登记既有问题）; 本轮另观察到该二进制
  退出路径 glibc stack-smash abort（汇总打印之后, 二进制自身缺陷,
  不影响判定, 已记遗留）。
- **JThreadBench**: rc=0, 3 JVM 中位 3205-3418ms（当前宿主多 VM 并行,
  绝对值与 T2 的 639ms 不可直接比, 以 rc=0 无错为准）。
- **dmesg audit**: 零 WARNING/BUG/Oops/INV7 签名（run13 全程）。

## 4. 红线核对

| # | 红线 | 本片执行 |
|---|---|---|
| 1 | arena PTE 写必经事务 | aging = `corten_lock_range` > ptl 内 `ptep_clear_young_notify`; 换出走 M6.T2 事务; 新 PTE 写点全量列表 = walker 的 clear_young + swapin 的 mkyoung, 均在事务内 |
| 2 | Swapped/meta 一致 | 不变式无新违例（INV7 全绿）; fork replay 载荷经校验臂 |
| 3 | pin 页不换出 | walker `folio_maybe_dma_pinned` 拒绝 + shrink_skipped 计数（KUnit 断言） |
| 4 | desc 锁 BH 对称/锁内无睡眠无分配 | walker 全原子（xa_store GFP_NOWAIT）; reclaim 在锁外——scan 与 evict 同形: 自旋锁只护片内 aging/pick（有界自旋）, 每片 reclaim 前丢 `shrink_lock`、下片 `spin_trylock` 重取（busy → break 记 evict_busy; r07 复审 B1 修正了 evict 锁内 reclaim 的失实/缺陷） |
| 5 | DEV-13 无反向边 | 新边 shrink_lock > desc(W,BH) > ptl > notifier-clear-young, 与 vmscan 同向; lockdep 变体零签名 |
| 6 | =n/off 折叠 | shrinker 仅 corten=on 注册; =n 八对象零错; off 套件 skip 全绿 |
| 7 | 不进 LRU | 全 diff 零 folio_add_lru; swapin 亦不 LRU（DEV-10） |

## 5. 遗留 / 登记

1. **`corten_test_txn_uninstall_interlock` flake**（M7 已登记）: 本轮
   中途 1 次, 最终矩阵全绿; 基线可复现。
2. **run_mode_smoke 2 例既有失败**（T1/T2 已登记）+ 二进制退出
   stack-smash abort（新观察, 二进制自身缺陷）。
3. **批量换出的天花板 = zram 同步写**: 10k 页 0.8-3.5s（页 82-350µs,
   zram submit_bio_wait 占绝对大头）。"百 ms 级/10k" 需上游 page_io
   批提交/zram 异步化——登记为 T5 性能化输入, 非 corten 缺陷。
4. **shrinker 扫描的 memcg 位维护**依赖 charge 时置位; 若未来出现
   "不经过 arena charge 而产生 arena 页"的路径需同步补
   `set_shrinker_bit`（代码注释已钉）。
5. **shrink_swapped 计数口径** = picked−kept（本轮从"全局差分近似"
   改为精确）; 与 swapped_out 的差 = 非 shrinker 路径换出（debugfs
   evict 等）。
6. OQ-M6-8 已落地（swapin grace bit）; OQ-M6-2（SHARED 不换出）、
   OQ-M6-3（迁移互操作）、OQ-M6-7（MADV_PAGEOUT 路由）维持原裁定。

## 6. 产物清单

- diff: `patches/r07-m6t34.diff`（5 文件; 复审处置后 +1604/−126,
  未 commit; 处置前原快照 = `patches/r07-m6t34.diff.prerev` +1563/−126）
- KUnit: `results/r07/m6t34-kunit-{on1,on2,off1,lockdep}.log`;
  复审处置后 `results/r07/m6t34-rev/kunit-{on1,on2,off1,lockdep}.log`
- 构建: `results/r07/m6t34-build-{final,n}.log`, `m6t34-lockdep-build.log`;
  复审处置后 `results/r07/m6t34-rev/build-{y,n,lockdep,y-restore}.log`
- checkpatch: `results/r07/checkpatch-m6t34.txt`（处置后重生成;
  arena.c 余 1W 基线）; 全量输出 `results/r07/m6t34-rev/checkpatch-full.txt`
- guest: `results/r07/m6t34-guest/{m6t3-run13.log, m6t3-dmesg-final.txt,
  boot.log, smoke.log, smoke-full.log, jvm.log}`;
  复审处置后 `results/r07/m6t34-rev/{run13-rev.log, evict-smoke.log,
  lockdep-evict.log, boot-lockdep.log, boot-final.log}`
- 脚本: `bench/share/m6t3-shrink-test.sh`（guest 判据 + 口径核对内建）;
  复审处置新增 `bench/share/m6t34-rev-evict-{lockdep,smoke}.sh` +
  编排 `cortenmm/bin/r07-m6t34-rev-verify.sh`
- 归档: `bzimg/r07-m6t34-rev`（处置后 =y 终版）+ `.sha256`
- VM: `/home/ppw/vm/trixie-m6t34.img`, tmux `m6t34-vm`（port 10030, 留运行）

## 7. 复审 B1-B3 处置（r07 晚班, 2026-09-20 夜–09-21 晨）

复审 NO-GO 3 阻断项逐条处置 + 顺带①(a)。以下为处置后的复验证据。


### 7.1 代码处置（worktree 未提交增量上修, 快照重导出）

1. **B1（evict 自旋锁内睡眠）**: `corten_arena_evict_mm()` 改为与
   `corten_shrink_mm()` 同形——每片 aging 后先 `spin_unlock` 再
   `corten_shrink_reclaim()`（folio trylock/zram 同步写均可睡）, 有下一片才
   `spin_trylock`（busy → break 记 `evict_busy`, 已收各片经 swap delta 照报;
   首轮 busy 维持 `-EBUSY`）; 循环改 `for(;;)` 使**每条退出路径都处于已解锁
   状态**（budget 耗尽在解锁后判定）; 函数注释 "all released before the
   reclaim runs" 失实声明改为如实描述锁丢弃形状; 红线 4 表述同步修正。
2. **B2（xarray 节点泄漏）**: `corten_arena_state_free()` 补
   `xa_destroy(&state->shrink_aged)`——凡跑过 pass-1 aging 的 mm, 退出即
   泄漏 `shrink_aged` 的 xarray 节点。
3. **顺带①(a)（scan 忽略 gfp_mask）**: `corten_shrink_scan_objects()` 补
   `!(sc->gfp_mask & __GFP_IO) → return 0`（上游 superblock shrinker 的
   `__GFP_FS` 先例同型; 门在 `shrink_scans` 计数前）。
4. **B3 登记**: DESIGN.md §6 "MGLRU aging 只读统计、不改 PTE、无事务需求"
   旧文勘误（最小编辑 + r07 标注, 按 SPEC §1.2 P2/P3: 上游 walk_mm aging
   对 LRU 页清 young 位 vmscan.c:3717, arena young 位写必经事务; lru_gen
   对 arena folio 结构性 skip 判定一并文档化; 先例 = M5.T1a 对 §3 白名单）;
   STATE.md 登记 R6-6 两项偏差（scan 睡眠=内核合法但与 R6-6 字面冲突;
   count 用活计数 vs per-mm 缓存值——精确性更强, 记偏差非回归）+ 同扫描
   重 age 角 + (a) gfp_mask (b) count 路径 mmput 可睡（PREEMPT_RCU 合法
   未登记）。


### 7.2 复验矩阵（全部本轮重跑）

- **=y 构建零新增 warning**: 夜班处置前全量 #13 在档; 处置后 =y 构建
  #14（KUnit/普通配置 guest 终判件）、lockdep #15、终态重建 #16
  （= bzimg/r07-m6t34-rev）。四份构建日志均零新增 warning（基线两条
  除外: cpuidle objtool / memblock modpost）。
- **KUnit**（无盘 qemu, `kunit.filter_glob=corten*`）: corten=on ×2 与
  corten=off ×1 **全绿**（on: 24/0/1 + 48/0/0 + 30/0/2 两跑皆然;
  off: 25/0/0 + 18/0/30 + 6/0/26）。
- **=n 八对象**: memory/mmap/migrate/rmap/swapfile/gup/oom_kill/
  arch(x86)fault 零错误零警告。
- **checkpatch --strict**（5 改动文件, file 模式）: **0E/0W**
  （corten_arena.c 余 1W = 基线既有 braces 条, 行数 8740; 其余四文件
  0E/0W）。
- **lockdep 变体（PROVE_LOCKING + DEBUG_ATOMIC_SLEEP, 树内切换→复验后恢复）**:
  - lockdep KUnit（corten=on）: 三套件全绿; 已知既有签名 2 处
    （见 7.3）, 回溯均不在本片路径。
  - **guest 实跑 evict 200000000（评审指名的 evict lockdep 盲区首次闭环;
    脚本 `bench/share/m6t34-rev-evict-lockdep.sh`, 日志
    `results/r07/m6t34-rev/lockdep-evict.log`）**: arena_stress 4 线程
    512M populate 273MB → STOP → `echo "$AP 200000000" > evict`:
    **rc=0, 34s, 67565 页换出, 多轮 reclaim 全路径**（片=512 候选页,
    锁内 aging → 锁外 reclaim → trylock 下片）, evict_busy=0;
    **零 D 状态**（完成后 3 次采样全空, 无卡死）;
    **evict 窗口 dmesg: 本片 shrink/evict 路径零签名**（263 处
    sleeping-in-atomic 报告回溯采样 100% = `munmap → corten_arena_munmap_
    route → zap_window 事务` 既有路径, 无一触及 `corten_shrink_*` /
    `corten_arena_evict_*`）; evict 后 CONT → 工具跑完 **verdict PASS,
    ops=1162125, errors=0**（全量换出后数据完整性全读回）。
- **guest 普通配置复跑**（stage1 终版件, 脚本 `m6t3-shrink-test.sh` 即
  run13 同款）: **fails=0**（shrink_scans=943 / aging_passes=1004 /
  shrink_swapped=59333 / swapped_out=69333; swapins=69333 精确对账;
  memory.swap.current == smaps Swap == ledger 三方容差内; verdict PASS,
  ops=1054661）+ **evict 冒烟 rc=0**（独立脚本
  `m6t34-rev-evict-smoke.sh`: 20005 页/9s, verdict PASS, dmesg clean）。
- **快照/归档**: `patches/r07-m6t34.diff` 重导出（5 文件 +1604/−126）,
  原快照备份 `patches/r07-m6t34.diff.prerev`, 两版逐行核对增量 = 本轮
  4 处处置（hunk 头位移除外）; 主构建树恢复 =y 终态零新增警告,
  bzImage 归档 `bzimg/r07-m6t34-rev`
  （sha256=46f151d38e1c56cc153db8cb2a2200b71568f62fefa7c0a6c4e1f63b5d0ec99f;
  guest/KUnit 终判跑于 stage1 同源件 /tmp 归档, 同源关系见
  `m6t34-rev/diff-export.txt`）。


### 7.3 新发现（DEBUG_ATOMIC_SLEEP 首开暴露, 非本片引入——登记移交）

B1 复验把 `DEBUG_ATOMIC_SLEEP` 首次纳入 lockdep 变体（既往 lockdep 轮只有
PROVE_LOCKING, 原子内睡眠不报告）。该开关暴露一处**既有**缺陷:

- **位置**: `corten_arena_zap_window()` 在 desc 写锁临界区（`corten_txn_begin`
  起, write_lock_bh 形态）内调 `tlb_finish_mmu()` →
  `__tlb_batch_free_encoded_pages()`（mm/mmu_gather.c:141 批量页释放可睡）。
- **触发面**: arena unmap/DONTNEED/munmap 路由 + 退出 teardown 的 zap 事务
  全族。KUnit 中 2 例（`corten_arena_test_gup_state4_pin_zap` /
  `corten_fault_test_zap_keep_perm`, 套件仍全绿——报告不致失败）; guest
  压测/teardown 累计 268 处（同位置, 1s 级周期=工具 mixed 模式 munmap 节律）。
- **判定**: 五轮复审未现 = 该开关从未开过; 非 B1（回溯不在 shrink/evict）、
  非本片引入（M4 zap 事务形状）。**本班不改**（修法 = tlb_finish_mmu 移出
  desc 写锁临界区, 涉及 covering-lock 协议重组, 远超 ~20 行终审增量）,
  登记移交 maintainer/M7 裁决。B1 判据相应按窗口划清: evict 窗口零本片
  路径签名（已达成）, zap 路径报告如实登记。
  （→ 同夜后续班次已修复, 见 §8。）


### 8 zap_window tlb_finish_mmu 协议修正（r07 深班后续, 2026-09-21 凌晨）

§7.3 移交项本班修复（worktree 未提交增量上再叠加, 快照重导出; 修法照
perf1c 802ff7551bd0 "flush 后置到锁外" 的先例结构）。

#### 8.1 根因

`corten_arena_zap_window()` 的窗口级 mmu_gather（[perf1] lazy gather）
在同一函数内以 `tlb_finish_mmu()` 收尾, 其批量页释放路径
`__tlb_batch_free_encoded_pages()`（mm/mmu_gather.c:141 `cond_resched()`,
以及 free_pages_and_swap_cache 族）可睡, 而该函数整体运行在 desc 写锁
（`corten_lock_range` → `corten_txn_begin` 的 write_lock_bh）临界区内。
同族暴露还有批溢出强制冲刷点 `tlb_flush_mmu()`（zap_window 内层 force
路径; park 路由的 caller-owned gather 同样命中）——即 §7.3 的
unmap/DONTNEED/munmap/teardown 全族 268 处的机制。约束边界:
`tlb_gather_mmu()` 本身= 纯初始化 + atomic pending 计数, 原子安全, 可留在
锁内; ptl 嵌套 desc 写锁（DEV-13）不受影响。

#### 8.2 修法

- **gather 打开留在锁内, 收尾全部移出**: zap_window 不再 flush/finish。
  批量溢出经新增的 per-window `struct corten_zap_win`（driver 持有: lazy
  gather 槽 + 游标 + force 标记）上抛; 溢出页的 PTE 已清、页引用已入批,
  续轮只补它的元数据复位（原 force_flush 语义逐字保留）。
- **两个 driver 重组为轮循环**（`corten_arena_unmap_chunk_flags`、
  `corten_arena_mmap_route` 的 MAP_FIXED 重填）: desc 写锁内 zap 一段 →
  放锁 → 锁外 `tlb_flush_mmu()` → 重锁（每轮都锁全窗, 保持 txn 覆盖与
  MAP_FIXED 尾轮 corten_mark 的范围契约）→ 从游标续走; 窗口 lazy gather
  的 `tlb_finish_mmu()` 在窗口最后一轮放锁后由 driver 执行（含全部错误
  出口, 无泄漏路径）。park 路由的 caller-owned gather 语义不变（[perf1c]
  post-downgrade 收尾保留）, 中途溢出冲刷同样走轮循环（mmap_write 下,
  可睡语境合法）。
- **语义红线核对**: flush 仍严格"PTE 清理后、folio 释放前"——该次序由
  mmu_gather 内部成立, 与临界区位置无关; 每个 TLB 区间恰一次冲刷不变
  （溢出轮 + 尾轮 = 原 force 循环同构; 常态下 gather 容量远大于 2M 窗,
  与修复前一样一轮完成, 锁往返零新增）。轮间放锁窗口可插入的 FRESH 缺页
  补填 = 与 munmap/DONTNEED 返回后的补填同形（事务性 PTE+metadata 成对,
  不产生 r03 缺陷 C 的裸 PTE 形状; 对其翻译的偶发 shootdown 属无害冗余
  冲刷）。zap_untracked_window 无 desc 锁（-ENOENT/-EOPNOTSUPP 臂）,
  原样保留。

#### 8.3 DEBUG_ATOMIC_SLEEP 前后对照（同驱动同负载, guest 实跑）

驱动 `bench/share/m6t34-zapfix-ds.sh`: arena_stress mixed 15s + churn 15s
+ `ds_dontneed`（新工具, MODE arena 上 MADV_DONTNEED 环回, 实测
12.7-16.0 万 ops/15s）15s; 判跑件均为 lockdep 变体
（PROVE_LOCKING+DEBUG_ATOMIC_SLEEP）。

| 臂 | 件 | sleeping splat | 其它告警 |
|---|---|---|---|
| A 修复前 | /tmp/m6t34-rev-bzimage-lockdep（§7.3 登记时状态） | **47**（全部 mm/mmu_gather.c:141, 三形态齐发） | 0 |
| B 修复后 | /tmp/m6t34-zapfix-bzimage-lockdep | **0** | 0 |

- 日志: `m6t34-zapfix/ds-prefix.log`（A, fails=1）、`ds-fix.log`（B,
  fails=0）。与 §7.3 的 268 处（累计）同签名同路径, 单轮 15s×3 即 47 处,
  判据"修复前必现/修复后为零"达成。
- **lockdep KUnit（严格零签名判据）**: §7.3 时该判据还须为 zap 签名设
  "已知放行"; 本轮起撤销放行——on×2 全绿（102 ok / 0 fail）, 零
  lockdep/oops 签名（`kunit-lockdep-fix.log`/`-fix2.log`）。既往 2 例
  （gup_state4_pin_zap / zap_keep_perm）同步绝迹。

#### 8.4 回归矩阵（全部本轮重跑, 修复后件）

- **=y 全量**: 零新增警告（基线 2 条不变, build-y-restore.log）。
- **KUnit corten\* on×2/off×1**: 全绿——on: 24+48+30=102 ok/0 fail
  （`kunit-on1-rerun.log`/`-on2-rerun.log`）; off: 25+18+6=49 ok/0 fail
  （`kunit-off1-rerun.log`）。
- **=n 八对象**: RC=0 零警告（build-n.log）。
- **checkpatch --strict 五文件**: 0E; 唯一 1W = arena.c:3976 braces 基线
  （本轮未触, m6t34-rev 轮同值）。
- **guest 普通配置（/tmp/m6t34-zapfix-bzimage-final）**: run13
  fails=0（swapped_out=swapins=70077 对账, memory.stat==smaps==ledger,
  arena_stress 512M mixed PASS）; JThreadBench 2000×3 rc=0 ×3 JVM（中位
  3840/3933/4119ms——宿主多 VM 并行, 与 m6t34-rev 轮 3205-3418ms 同口径
  慢 ~15%, 趋势无害）; 终版 dmesg 静默（final-dmesg-count=0）。
- **归档**: =y 修复后件 → `bzimg/r07-m6t34-zapfix`
  （sha256=7dd8c3e2f27926a780a46afc119469ae8d0d31a8, 与 stage3 判跑件
  字节一致）; 快照重导出 `patches/r07-m6t34.diff`（5 文件 +1838/−223）,
  修前快照备份 `patches/r07-m6t34.diff.prezap`, 两版增量 452 行 = 本修
  （zap_window/driver 重组 + 注释）。

#### 8.5 登记在案（非阻塞）

- **宿主过载 KUnit flake 2 例**: 首轮 =y on2/off1 中
  `corten_test_txn_uninstall_interlock` 失败（"worker A never acquired"=
  10s 观察窗超时; `violations==1` = 主线程 200ms 睡眠在宿主 vCPU 饥饿下
  超睡 ~10s, 撞上 worker A 自身 20s 持锁 deadline 的 a_err 出口——
  1718/1719/1728/1729 行四断言全可由同一超睡序列解释）。与本修代码路径
  零交集（协议层测试 vs arena zap 路径）; 同一二进制重跑即绿, 修复前基线
  件控制组同绿, 时间线证据（测试墙钟 4s→24.2s ≈ A 的 20s deadline）在
  `kunit-on2.log`/`kunit-off1.log`（flake 样本, 留档）。未改测试; 宿主
  多 VM 并行时该 10s/20s 窗口偏紧, 留 M7 顺手加宽。
- **ds_dontneed 工具首版** DECLARE -EINVAL（声明范围须恰为一整个 VMA,
  首版多映射 2M 对齐垫）, 已修为 2M 对齐 hint 扫描（arena_stress 同型）;
  §8.3 的 A/B 两臂均已用修正版工具复跑（A 臂 47 splat 含 DONTNEED 形状
  实跑 ops=126874）。
