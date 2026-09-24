# W1.e2 开发报告 · ttu 匿名守卫翻转（W-1 收尾片：shrinker 腿重接 + 守卫降级 backstop）

- 片: MV2 W1.e2（W1_NATIVE_RMAP_SPEC.md §5 W1.e 拆片的 e2 半：shrinker 腿 + 守卫降级；§3.2/§3.4 判据）
- 基线: mv-a0 @ 887c4cc33131（W1.e1 已落，干净）· 未 commit · worktree /home/ppw/linux-6.18-mva
- diff: /home/ppw/cortenmm/patches/r07-w1e2.diff · **+598/−308**（4 文件；生产：mm/rmap.c +51/−39、mm/corten_arena.c +167/−158（净删：corten_shrink_reclaim 整函数消亡）、mm/corten_arena.h +58/−17；测试：mm/corten_fault_test.c +322/−94）
- 验证: make -j8 RC=0（触碰文件零警告）· 三套件 KUnit on1/**on2**/off1(+复跑)/DAS/最终镜像复跑全绿 · **PROVE_LOCKING 加跑全绿**（仅已登记 xa_destroy 基线 splat，零新边）· =n 折叠 14 对象零 corten 符号 · checkpatch **0E/0W**（1213 行 "ready for submission"）· guest 门留主会话
- 日志: /home/ppw/cortenmm/results/r07/w1e2/
- 一句话拓扑: **翻转后窗口匿名页的每一条换出路——shrinker pick、evict(debugfs)、以及 ttu 路由的一切残余到达——都收敛到 W1.e1 驱动的同一事务；folio trylock 仲裁双入口（恰好一方换出、另一方 keep），M6 拒绝臂降级为不可达的 WARN+计数 backstop。**

---

## 1. 实施内容（对照 task 四项）

### 1.1 shrinker 腿重接：`__reclaim_pages` 弯路消亡（task 2 的工程本体）

W1.e1 后两通道并存:evict 腿直驱,压力腿仍走 `pick(folio->lru) → corten_shrink_reclaim →
__reclaim_pages → shrink_folio_list → ttu → M6.T2 完成臂`——rmap 走查的唯一贡献是把
pick 自知的地址还给驱动。本片删除该弯路:

- **`corten_shrink_reclaim()` 整函数删除**（连同其 30 行 private-cookie 契约注释——
  folio->lru 借链、stranded-head 愈合、`__reclaim_pages(&one, &corten_nr_swapped_out)`
  逐页调用全部不再需要）。
- **`corten_shrink_mm()` 改用 (addr, folio) picks 直驱**（evict 腿 W1.e1 的同款形状）:
  每 slice `spin_trylock → eval+age（填 picks）→ spin_unlock → 驱动排空`
  （排空睡眠:folio trylock + zram 同步写,锁必须先落——与 evict 循环同注记）;
  返回值改按驱动逐 pick 的 bool 计 freed,keep 计 kept。
- **`corten_shrink_walk` 去 `list` 字段**:pick 点 `if (w->force) {pair} else {lru}`
  分叉合并为无条件 pair（`w->picks` 必填）,`w->force` 保留纯 aging 语义
  （"不看位就 pick + 不清 epoch"）;evict 侧 `w.list = NULL` 行随字段消亡。
- **shrink_swapped 归因口径不变**:picked − kept 逐字保留,注释从"M6.T2 事务"改为
  "原生驱动的事务"——竞态无关性（pick/keep 同点计数）不依赖排空者是谁。
- **`!vma` pick 门裁决:保留**（W1.e1 留给本片的决定）。理由:驱动虽不再需要 rmap 找
  地址,但其 mlock 判决与 flush 形状仍解析 carrier（`corten_arena_anchor_vma`）,
  anchorless pick 会在 carrier lookup 处确定性 keep——不 pick 省掉这次注定徒劳的
  往返（shrink_skipped 计数口径原样）。W-2 驱动去 carrier 时重开（两处块注释已登记）。

### 1.2 ttu 匿名守卫翻转 + "从 ttu 进入"包装臂（task 1）

- **rmap.c `try_to_unmap_one()`**:M6 臂（prefilter → notifier 窗口 →
  `corten_rmap_swap_out` → defer 簿记）退役,替换为:
  `corten_rmap_unmap_one(..., false)` 为假 → `WARN_ON_ONCE(1)` + 计数 + return false
  （不可达 backstop）;为真 → `corten_swap_out_driver_ttu(mm, address, folio, flags)`
  → **return false（恒）**。恒 false 是走查安全的关键:flipped 臂的驱动调用在成功时
  **释放 folio**,而 rmap_walk_anon 的循环是 `if (!rmap_one(...)) break;` 在
  `done(folio)=folio_not_mapped()` **之前** break——false 返回使走查在 folio 释放后
  零解引用（实测走查代码确认 break 先于 done）。R6-1/R6-2 的窗口与 defer 簿记自
  W1.e1 起已移入驱动（内联 flush）,rmap.c 侧随之净删。
- **flags 语义映射**（臂头注释全谱）:TTU_HWPOISON/迁移调用方不达本臂（backstop 先拒）;
  mlock 判决 flags-盲随驱动（TTU_IGNORE_MLOCK 调用方对锁窗拿到的是安全 keep + 计数,
  而非它请求的 ignore——回收绝不因 ttu 形状放宽可取形状）;defer-flush 族映射到驱动
  内联 flush(OQ-W1-1 非 defer 首版,W1.d 同解)。
- **caller 契约 = 移交(transfer)**:驱动的账本是闭合的(消费恰一个 pick 引用,成败两臂
  都消费),ttu 的锁+引用恰为其账本输入——臂内 `folio_unlock`(锁交还驱动的 trylock 腿)
  + 引用原地转 pick。任何期待锁/引用归还的生产 ttu 调用方都会被此臂破坏——
  **不存在**:腿重接后无生产路径把窗口匿名页喂给 ttu(off-LRU、迁移/hwpoison 隔离端
  结构排除)。
- **prefilter 语义收窄**:M6 拒绝集退役一条——"无 swap entry"拒绝随完成臂消亡
  (`folio_test_swapcache` 判据从 `corten_rmap_unmap_one` 移除),驱动自带 entry 腿,
  swapcache-less folio 现在是**可路由形状**(本地锚实跑覆盖);其余拒绝形状
  (migrate/hwpoison/mlock/pin/large)逐字保留。
- **`try_to_migrate_one()`**:维持全拒绝(spec R2/OQ-M6-3 不翻),拒绝臂升级 backstop
  姿态:同款 `WARN_ON_ONCE(1)` + 既有 rmap_rejects 计数。
- **`corten_swap_out_driver_ttu()`**(corten_arena.c,~40 行含注释):纯入口臂——
  `WARN_ON_ONCE(!folio_test_locked)` 恢复形防线(checkpatch 对 VM_BUG_ON 的
  BUG-族警告的 0W 解,checkpatch 语义即"WARN+恢复";此臂上消费零状态后返回 false)
  → `folio_unlock` → `return corten_swap_out_driver(...)`。**驱动本体零字节改动**
  (红线;函数头注释亦未触碰)。
- **`=n` 折叠**:corten_arena.h 空桩(返回 false;rmap.c 侧 VM_CORTEN 门即折叠门,
  桩不可达,纯折叠形态)。

### 1.3 两通道并存口径更新（task 2 的分析落纸）

竞态面收敛到**单点仲裁 = folio trylock**,三对关系全部闭合:

1. **shrinker pick(drive 中,持锁) vs ttu 进入**:ttu 臂先解锁再进驱动,trylock 对上
   驱动已持有的锁 → 失败 → keep(引用全数归还,零状态写)。反向同理:ttu 的驱动先赢
   trylock,后到的 shrinker pick 在驱动 keep 臂收敛。
2. **ttu keep 的愈合**:keep 页靠下一轮 shrink/evict 重 pick 收敛(e1 报告 §2 陷阱 4
   的既录口径)——swapcache 成员在驱动 entry 腿短路,事务原样重跑;ttu 进入不新增
   keep 形态,只复用驱动的。
3. **成功唯一性**:两入口共享 entry/cache/PTE 全部状态,trylock 保证任一时刻至多一个
   事务在飞;"双入口尝试,恰好一方成功"为结构性结论(KUnit 确定性调度锚 + guest 真并发
   压测门)。

块注释更新四处:evict 段(e1 的"coexist until W1.e2"段落)、shrink_mm 头、walker 头、
corten_arena.h 驱动声明与 corten_rmap_ttu/corten_rmap_unmap_one 契约注释、rmap.c
try_to_unmap() 钩子注释(anon 侧措辞:钩子仍不接 anon——走查正是地址发现者,翻转在
_one 守卫内完成)。

### 1.4 KUnit 锚（task 3；corten_fault_test.c）

| 用例 | 门 | 断言 |
|---|---|---|
| `corten_fault_test_rmap_guard` **重写** (ok 28) | corten=on(本地实跑) | 原 M6 拒绝语义断言整体反转为翻转断言:D1 谓词上 plain-unmap 族(0/SYNC/IGNORE_MLOCK/BATCH_FLUSH)全 TRUE 且零计数(swapcache-less 可路由);HWPOISON+migrate 拒绝且恰 +2(backstop 账本,谓词级直驱不触发走查侧 WARN);folio 双臂均零触碰;plain VMA 零过问 |
| `corten_fault_test_ttu_route` **新增** (ok 29) | 无 swap 本地实跑 keep 形 / guest swap 形 | 真 ttu 入口:`folio_lock+folio_get → try_to_unmap(TTU_IGNORE_MLOCK)` → 走查进翻转守卫 → 驱动。keep 形(无 swap 设备):keep 全对称(ref 回 1、解锁、PTE 逐位不变、meta MAPPED、driver_kept+1、driver_swapped 惰性);swap 形(guest):**移交契约实跑**(folio 释放,此后零解引用)+ PTE/meta entry 逐位(INV7)+ ANONPAGES−1/SWAPENTS+1/driver_swapped+1 + swap_duplicate 证 entry 恰剩 PTE 引用 + 缺页换回内容逐位(0x5c 两点)+ 账本归零。**两形共断言 rmap_rejects 恒零**("守卫 backstop 恒零"锚) |
| `corten_fault_test_ttu_dual_entry` **新增** (ok 30) | 无 swap 双 keep / guest 恰一成功 | 双入口收敛确定性调度:A=shrinker 形(pick ref + 测试持锁模拟驱动在飞)→ `corten_swap_out_driver` 直调必 keep(trylock 失败,ref 归还,锁保持);B=ttu 路由(移交解锁把锁交进 trylock)→ 本地 keep/ref 归 1/guest **换出且 A 是唯一 keep**(driver_kept +1 & driver_swapped +1 并存=恰好一方成功);末断言 backstop 零。真并发面(交错调度)归 guest 压测门,报告披露 |
| `ft_swap_out` 助手**重写** + 两调用方改造 | swap 机器(guest) | M6 形(ttu 后显式 swap_writeout+folio 解引用)→ W1.e2 移交形:pick ref 由助手注入(`folio_get`),entry 分配后即捕获,`try_to_unmap` 后**零 folio 解引用**(PTE/meta 验证全走地址);swap_roundtrip 删显式 writeback 块(驱动内联了),swap_zap_free 同参收敛 |

回归口径:on1/on2/off1/DAS/最终镜像全部与 e1 基线同数(e1: 24/0/1 · 104/0/0 ·
32/0/3 → 本片 34/0/3 = +2 新锚,rmap_guard 重写不增数),既有用例零回归。

## 2. INV6/红线核对

| 红线 | 本片执行 |
|---|---|
| INV6(窗口 PTE 写必经事务) | **新 PTE 写点 = 零**:路由复用 W1.e1 驱动 → 原样事务;本片只删了旧臂的 PTE 写路径(同一事务本体)。`corten_rmap_swap_out`/`corten_swap_out`/`corten_swapin_sync_meta` 零字节改动 |
| 驱动本体(W1.e1)零改动 | `corten_swap_out_driver` 函数体**逐字节原样**(含函数头注释);唯一触碰是驱动 keep 臂注释里对已删除函数 `corten_shrink_reclaim` 的一处指名——同名函数本片消亡,悬空引用必须除名,**纯注释、零语义**(评审可见披露)。新臂与守卫翻转即红线许可的"只加入口臂与守卫翻转";shrinker 腿重接是 spec §5 e2 半的既定内容(非驱动本体) |
| =n 折叠 | 新臂空桩 + 既有门折叠;14 对象 nm 零 corten 符号 |
| 不 commit | worktree 仅 4 个 modified 文件,未暂存未提交 |
| vmscan.c 零改动 | `__reclaim_pages` 保留(上游 reclaim_pages 出口仍用);corten 侧唯一调用点随 corten_shrink_reclaim 消亡 |

## 3. 验证结果

| 门 | 结果 |
|---|---|
| make -j8(=y) | RC=0;触碰文件(corten_arena/rmap/corten_fault_test)零警告;bzImage 就绪 |
| KUnit on1(corten=on) | **corten 24/0/1 · corten_arena 104/0/0 · corten_fault 34/0/3** 全绿;新锚 ok 28(重写 rmap_guard)/ok 29 ttu_route(keep 形实跑)/ok 30 ttu_dual_entry(双 keep 形实跑) |
| KUnit on2(flake 复跑) | 同数全绿 |
| KUnit off1(corten=off) | 25/0/0 · 24/0/80 · 7/0/30 全绿(首跑即绿,无 flake)+ 复跑同绿;新锚双 SKIP |
| DAS(DEBUG_ATOMIC_SLEEP) | 构建零本片警告 + 24/0/1 · 104/0/0 · 34/0/3 全绿(臂在 arena 引用下的解锁/驱动睡眠点合法) |
| **PROVE_LOCKING(加跑)** | 三套件全绿;全日志**恰 1 条 lockdep 签名 = 已登记基线 xa_destroy splat**(mva1-verify.md:115 同签名同计数,A.0 交接注记,先于 W1 全系列存在);**翻转新增锁边(anon_vma 读锁跨越驱动 swap-cluster/desc/ptl/freeze/写回睡眠)零告警**——ttu_route/dual_entry 在 PROVE_LOCKING 下实跑了该嵌套。本片风险主轴(锁边)由本跑闭合 |
| =n 折叠 | 14 对象 RC=0 零警告,nm 零 corten 符号(mva2-verify n-objects) |
| checkpatch(patch 模式) | **0 errors, 0 warnings, 1213 lines** — "ready for submission";`git diff --check` 净 |
| 最终镜像复跑(WARN 修复后重构建) | 24/0/1 · 104/0/0 · 34/0/3 全绿(shipped 位一致) |
| guest 门(留主会话) | ① zram 往返 512MiB readback(M6.T3 压力门复跑:RSS 降/swap.so 增/INV7 零漂移);② **ttu 匿名压测**:swap 上电后 ttu_route/dual_entry/swap_roundtrip/swap_zap_free/driver_swap_roundtrip 五锚套件重跑自动转实跑(断言含换回内容逐位与"恰好一方成功");③ 并发 fault 压测覆盖 keep 愈合通道(驱动收敛) |

## 4. 登记与披露

- **守卫翻转后 rmap_rejects 的语义**:从"M6 拒绝账本"变为"不可达 backstop 账本"(WARN
  一次性 + 计数)。两处 WARN 站点(try_to_unmap_one/try_to_migrate_one 拒绝分支)在
  生产与套件中均不可达——锚断言其计数恒零;若点亮,含义 = "守卫分析漏了一条路径"。
- **mlock 的 flags-盲映射**:TTU_IGNORE_MLOCK 调用方对锁窗拿到 keep 而非 ignore
  (驱动判决无 flags 通道,红线禁止改驱动本体)。这是从严映射,登记为已知语义差;
  上游 ttu IGNORE_MLOCK 家族(kswapd 直收)结构性不达此处(无 LRU)。
- **transfer 契约的锋利边**:flipped 臂消费 ttu 调用方的锁+引用。当前零生产调用方
  (结构性:off-LRU + 迁移/hwpoison 隔离排除);若 W-2+ 新增 ttu 消费面,必须按
  "不再触碰 folio"契约写——臂头与 rmap.c 守卫注释均已写明。选择 transfer 而非
  "保险引用"形的理由:驱动账本闭合(恰 2 冻结),保险引用使 freeze(2) 永久失败 →
  ttu 路由永远 keep → "ttu 不再结构性排除"名存实亡。
- **R8 翻转(folio_remove_anon_rmap_novma 进事务)仍未做**(e1 §4 既录,维持):事务本体
  零改动红线压倒 spec §3.1 R8 行;order-0 下 carrier 形与 novma wrapper 数值等价(F1),
  W-2 匿名 unanchor 时随事务归属一并裁决。
- **`!vma` pick 门保留**(1.1 末条);W-2 驱动去 carrier 时与 R8 同批重开。
- **lockdep 基线 splat**:kunit-lk 日志中 1 条 `inconsistent {SOFTIRQ-ON-W} ->
  {IN-SOFTIRQ-W}`(xa_destroy ← corten_arena_state_free ← rcu_core)为 **A.0 交接
  既录基线事件**(mva1-verify.md:115;declare-侧 xa_store 任务态注册 vs RCU 回调
  softirq 侧取锁),与本片零关联(本片不触碰 xa/declare/state_free/rcu),修复建议
  (顺手 `__xa_destroy` 类改造)维持未采纳登记。
- **WARNING 清单对账**:on 系日志 WARNING 集与 e1 基线逐条同族(txn_begin×2/
  zap_single/fill_upper 等 8 条 = 既有测试的设计性 WARN_ON_ONCE,行号因本片注释
  插入 ±1 漂移),零新增。
- **规模注**:task 估 ~200 行;实落生产 ~294 净增(注释密度高,三处契约注释按
  W1 系列惯例随码)+ 测试 +228。超注部分为 rmap_guard 重写(反转断言非纯新增)与
  双形锚,无删除性偏差。
- **spec §5 W1.e 锚④(ttu 匿名臂计数恒零)**:由 ttu_route/dual_entry 末断言覆盖
  (rmap_rejects 恒零);锚③(pick 配对完整性)由 e1 的 (addr,folio) 结构 + 本片
  shrink/evict 共用该结构覆盖(构造性)。

## 5. 文件清单

- /home/ppw/linux-6.18-mva/mm/rmap.c(try_to_unmap_one 守卫翻转 :1976-2009、
  try_to_migrate_one backstop WARN :2455-2478、try_to_unmap 钩子注释 :2410-2430)
- /home/ppw/linux-6.18-mva/mm/corten_arena.c(corten_swap_out_driver_ttu :14301-14340、
  corten_rmap_unmap_one 契约收窄 :13483-13540、corten_shrink_reclaim 消亡、
  corten_shrink_mm 重接 :14804-14870、walk/pick/evict 注释与结构同步)
- /home/ppw/linux-6.18-mva/mm/corten_arena.h(新臂声明 + 契约注释 + =n 桩
  :868-885/909-960/980-1010/1215-1235)
- /home/ppw/linux-6.18-mva/mm/corten_fault_test.c(rmap_guard 重写 :2524-2608、
  ft_swap_out 移交形 :2717-2790、ttu_route :3062-3165、ttu_dual_entry :3167-3280、
  注册 +2)
- /home/ppw/cortenmm/patches/r07-w1e2.diff(导出,1267 行)
- 日志:/home/ppw/cortenmm/results/r07/w1e2/(kunit-on1/on2/off1/off1.rerun/das/lk/
  final + build-das/build-lk/build-y-final)
