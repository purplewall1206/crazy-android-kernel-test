# W1.f 开发报告 · unuse 盲区收口 + 观测面收尾（W-1 收官片）

- 片: MV2 W1.f（W1_NATIVE_RMAP_SPEC.md §3.2 swapoff 行 / §5 W1.f 行的 task 子集：
  unuse 窗口枚举臂 + unuse_blind_mms 语义翻转 + KUnit 锚 + 观测面收尾；spec 该行的
  hwpoison 探针项已在 W1.d 以注册表路由 + ttu_routes/decline 计数落地，见 §4 审计）
- 基线: mv-a0 @ b280246bb2f2（W1.e2 已落，ff-only 同步确认干净）· 未 commit ·
  worktree /home/ppw/linux-6.18-mva
- diff: /home/ppw/cortenmm/patches/r07-w1f.diff · **+452/−24**（5 文件；生产：
  mm/corten_arena.c +199/−17、mm/swapfile.c +28/−7、mm/corten_arena.h +19、
  include/linux/corten_arena.h +5；测试：mm/corten_fault_test.c +201）
- 验证: make -j8 RC=0（触碰文件零警告）· 三套件 KUnit on1/**on2**/off1/终镜像复跑全绿 ·
  =n 折叠（vmlinux nm 零 corten 符号）· checkpatch **0E/0W**（549 行
  "ready for submission"）· guest 门留主会话（S-3 两分支复测即 gate 判据）
- 日志: /home/ppw/cortenmm/results/r07/w1f/
- 一句话拓扑: **swapoff 对 MODE mm 的第零步从"树走查 + 盲区计数"翻转为"注册表枚举 +
  逐条 M6 swap-in 事务提前换回"——窗口 swap 条目对 swapoff 可见，V-D 登记的有界自旋
  收敛为一次拉回；盲区台账转常零残量账，W-1 的最后一条消费者改道完成。**

---

## 1. 实施内容（对照 task 四项）

### 1.1 unuse 的窗口枚举臂（task 1）

`corten_arena_unuse_windows(mm, type)`（mm/corten_arena.c :14146，声明 mm/corten_arena.h
:1011，=n 内联桩 :1297）+ 每窗漏斗 `corten_arena_unuse_window()`（:14070）。挂入点
= `unuse_mm()`（mm/swapfile.c :2457）在 `check_stable_address_space` 之后、legacy
VMA 走查之前——一次 unuse_mm 调用 = 臂在前 + legacy 树走查在后，对照 legacy
`unuse_vma` 语义两种形状各归其位：

- **枚举非 VMA walk**：`smp_load_acquire(&mm->corten_state)` 读注册表（与
  `corten_mm_state_pages()` 同一 acquire），`xa_find(&state->arenas, …, XA_PRESENT)`
  逐帧 RCU 短节：reserve 哨兵跳过、`wstart = max(idx<<PMD_SHIFT, ar->start)` /
  `wend = min((idx+1)<<PMD_SHIFT, ar->end)` 帧裁剪。调用方 unuse_mm 持
  mmap_read——DECLARE/RELEASE/reactivate 全被排除，注册表条目对走查稳定。
- **逐条 M6 swap-in 事务提前换回**：每窗每 pass 一把 `corten_lock_range()` 覆盖读
  事务（corten_arena_shrink_walk 同形）扫 512 槽 meta，`state == CORTEN_SWAPPED` 且
  `swp_type(corten_swap_decode(m)) == type` 者入批（≤CORTEN_UNUSE_BATCH=32），
  **落锁后**逐条合成 `corten_fault_ctx{.mm,.ar,.addr,.write=false}` 调
  `corten_arena_swap_in()`——M6 换入事务本体零字节复用（直接换入分配、
  memcg charge、swapcache_prepare 串行化、重锁后 meta/PTE 双重再验证、
  MM_SWAPENTS−1/MM_ANONPAGES+1、swap_free、swapins 计数、self-heal 臂）。
  再查询一次 meta 取新鲜 entry（缩小陈旧窗口），事务内再验证兜底（R-W1-7 的
  "先镜像后装 + 重锁再验证"序原样继承）。
- **收敛契约与 legacy 逐点对齐**：瞬时形状（-EAGAIN 竞态、描述符迁移中、并发
  shrinker 重换出）**不是错误**——批间 rescan 直到扫空（每窗每调用
  ≤CORTEN_UNUSE_PASSES=8 pass），带外的留给 `try_to_unuse()` 外层 retry（mmlist
  重走，`inuse_pages != 0` 即重入）——legacy 的"换入竞争靠外层重试"逐字同构；
  `signal_pending` → -EINTR（legacy 同答）；硬失败（-ENOMEM 分配腿、-EFAULT
  坏对/毒读）传播使 swapoff 响亮失败，并点亮 §1.2 的残量账。
- **fork 共享条目**：父先扫则父得私有驻留副本（entry 2→1），子后扫再拉（1→0，
  条目死亡）——各自 swap_duplicate/swap_free 账目闭合；两份私有副本即 fork-COW
  本来的收敛形（legacy 经 swapcache 共享 folio，arena 直换入形为每 mm 一份，
  冷路径语义正确，登记为已知内存形态差）。
- **INV6**：本臂零 PTE/零 meta 裸写——一切窗口 PTE+meta 写在 corten_arena_swap_in
  的事务体内（原样）；扫描只读（覆盖读事务），I/O 全部在锁外（INV3，
  swap_in 自持锁循环）。

### 1.2 unuse_blind_mms 语义翻转（task 2）

- swapfile.c 侧：`corten_arena_unuse_blind_note(mm)` 从"每个注册 mm 无条件计数"
  改为**仅臂硬失败（ret != 0 且 != -EINTR）时计数**——计数语义 = "臂未能扫净的
  残量盲区"，历史口径（V-D：树走查对 carrier 窗盲、条目 ride 到 fault/exit）在
  三处注释全量保留（计数器声明 ：405、note 函数头 :4000、debugfs 渲染行 ：3050），
  注释明示"expected zero / 历史形态只存在于本页"。
- 恒零断言双面：KUnit（§1.3 swapins 对账后断言 blind 台账不前进）+ guest
  （S-3 脚本 W1.f 判据刷新：unuse_blind_mms 前进 = FAIL——含义 = 臂的硬失败腿
  点亮，登记为升级信号）。
- spec §3.2 swapoff 行的收口即此翻转：S-3 开放项关闭。

### 1.3 KUnit 锚（task 3；corten_fault_test.c +201）

| 用例/助手 | 门 | 断言 |
|---|---|---|
| `corten_fault_test_unuse_windows` **新增**（ok 38；无 swap/corten=off 双 skip 门约定） | corten=on + swap 上电（guest 套件复跑自动实跑） | 真链三段：①窗口槽经 `ft_swap_out`（真 ttu 路由 → 驱动）换出；②legacy 槽经新助手 `ft_legacy_swap_out` 换出（真 `handle_mm_fault` 装页于普通树 VMA + shrink_folio_list 前奏 + **plain ttu**（VM_CORTEN 门不达，标准 unmap 装 swap PTE）+ clear-for-io/swap_writeout settle）；③**swapoff 的每 mm 步本体**——`corten_arena_test_unuse_mm()`（swapfile.c :2503 测试钩，直调 static unuse_mm）一次调用断言：两 PTE present + 内容逐位（窗 0x5c 两点 / legacy 0xa5）、窗 meta CORTEN_MAPPED + FT_PERM_RW + payload 归零、MM_SWAPENTS 2→0、MM_ANONPAGES 2、**swapins 台账恰 +1**（legacy unuse_pte 不计 corten 台账——两路径可区分对账）、**unuse_blind_mms 台账零前进（翻转锚）**、双 `swap_duplicate() < 0`（条目设备侧双亡）。多 swap 设备时（type 不同）legacy 侧断言按 `same_type` 收窄，窗侧断言无条件 |
| `ft_legacy_swap_out` 新助手 | 同上 | ft_swap_out 的 plain-VMA 对偶；EXPECT 纪律（folio 锁泄漏防护），folio 锁即 LRU 页的存活保持 |

回归口径：on1/on2/终镜像 34/0/**4**（e2 基线 34/0/3 + 新锚本地 skip），off1
7/0/**31**（基线 7/0/30 + 新锚 off skip）；corten 24/0/1 · corten_arena 104/0/0
与基线同数，既有用例零回归。

### 1.4 观测面收尾（task 4）

全计数器扫描（93 个 `corten_nr_*` 逐个对 inc/read 引用）：

- **死计数器 0**：唯一"只渲染不计数"的 `fork_demotes` 是 M5 声明点的既录历史遥测
  （:545 注释原文 "stays as the historical T0 telemetry and no longer grows"），
  与 unuse_blind_mms 同类的历史口径件，非缺陷，不动。
- **漏挂点 1 处已补**：`implant_drops`（V-A.3a 植入注册分配失败丢弃，"normally
  zero" 披露件）自 A.3a 起有 inc 无 reader——arena_stats 渲染补行
  `implant_drops`（corten_arena.c :2927，置于 backstop 组旁，注明 W1.f 收尾补挂）。
- **spec §5 W1.f 四计数器终形态**：`ttu_hook_unmaps` = `ttu_routes`（W1.d 落，
  渲染 ：2936 + 测试钩 + atomic_long_add 供数 ✓）；`driver_swaps` =
  `driver_swapped`/`driver_kept`（W1.e1 落，声明注释 :316 明示改名，渲染 ✓）；
  `registry_size` = `corten_arena_test_registry_size()`（W1.b 落，KUnit 锚消费，
  per-mapping 口径 + debugfs arenas 全局账互补 ✓）；`unanchored_probes` 无 W-1
  生产者——匿名 unanchor 在 W-2，W-1 出口 carrier 计数仍非零（spec §3.5 既定
  出口），hwpoison file 侧改道注册表已在 W1.d 落地（注册表命中路由 +
  `ttu_routes` 计数）， decline 残量账 = `rmap_rejects`（渲染 ：3024，backstop
  恒零锚 W1.e2 已入）。该项登记为 W-2 复活点，非本片缺口。
- **J1 探针族核对**：j1_probes/j1_hits/fault_fallback_window/uffd_window_reject/
  gup_probes/gup_probe_rejects/gup_window_miss/remote_win_short 全部渲染
  （arena_stats :2962-2984 区）+ 测试钩双收，W-1 后无新增窗口探针点（GUP 慢道
  面 V-C 定形），终版无漂移。
- **debugfs 渲染终版**：arena_stats 渲染本片净变 = unuse_blind_mms 行注释翻转 +
  implant_drops 补行，其余逐行原样；三套件中 ft_named_counter 族锚全程解析渲染
  面，即渲染可读性的回归证明。

## 2. INV6/红线核对

| 红线 | 本片执行 |
|---|---|
| INV6（窗口 PTE 写必经事务） | 本片**零新 PTE/meta 写点**：换回复用 M6 swap-in 事务本体（逐字节未动），扫描只读；unuse_pte 的 legacy 写不受影响（shadow-VMA 白名单 #3 原样） |
| 勿动 W1.a-e 主体 | rmap.c 零触碰；corten_swap_out_driver/corten_swap_out/corten_swapin_sync_meta/注册表/守卫翻转逐字节原样；唯一交集 = 新函数复用其导出面 |
| =n 折叠 | `corten_arena_unuse_windows` 内联桩（mm/corten_arena.h :1297）；测试钩仅 CONFIG_CORTEN_MM_ARENA_KUNIT_TEST 下有定义与引用；=n vmlinux `nm` **零 corten 符号**（build-n.log） |
| 不 commit | worktree 5 个 modified 文件，未暂存未提交 |
| DEV-13/锁序 | 零新锁类零新边：臂运行于 unuse_mm 的 mmap_read 下（arena 生命周期稳定），扫描 = 既有覆盖读事务形，换入 = swap_in 自持锁循环（folio/cluster/desc/ptl 原序） |

## 3. 验证结果

| 门 | 结果 |
|---|---|
| make -j8（=y） | RC=0；触碰文件零警告（bzImage #157 起两轮，终镜像 #158 后复跑；注：中途一次并发 make 竞争产出 =n 镜像被 "Unknown kernel command line corten=on" 抓出，清理后顺序重建复跑——已记入过程） |
| KUnit on1（corten=on） | **corten 24/0/1 · corten_arena 104/0/0 · corten_fault 34/0/4** 全绿；新锚 ok 38 本地双门 SKIP（swap/corten=on 门约定） |
| KUnit on2（flake 复跑） | 同数全绿 |
| KUnit off1（corten=off 默认引导） | 25/0/0 · 24/0/80 · 7/0/**31** 全绿（+1 = 新锚 off skip） |
| 终镜像复跑（干净重建后） | 24/0/1 · 104/0/0 · 34/0/4 全绿（shipped 位一致） |
| WARNING/BUG 指纹对拍 | on1 vs e2 on1：WARNING 10 条、pgtables_bytes 20 行**逐条同族同数**（V-D 定性的 8192 测试几何族），零 lockdep/oops/GPF 签名 |
| =n 折叠 | RC=0；vmlinux nm **零 corten 符号**；仅基线既有 modpost memblock 警告（mva0 起 r07 各片 in-file，与 e2 =n 增量构建未触发 modpost 同因） |
| checkpatch --strict（patch 模式） | **0E / 0W / 0C**，549 行 "ready for submission"（checkpatch-w1f.txt）；导出后源零改动（diff 对拍 PATCH-CURRENT） |
| guest 门（留主会话） | ① S-3 两分支复测（mve_s3_swapoff.sh **已按 W1.f 判据刷新**：branch B 的 swapoff 现自拉条目——自旋判据改为"有界 sweep + 可 SIGINT"，retry 仅在设备仍 on 时执行，**unuse_blind_mms 前进 = FAIL**）；② 新锚 unuse_windows 在 guest 有 swap 时自动实跑（三段真链：窗槽真路由换出 / legacy 槽真链换出 / unuse_mm 一次调用双对账 + 内容逐位 + 盲账零前进）；③ M6.T3 压力门 + zram 往返回归 |

## 4. 登记与披露

- **unuse 驱动换入的语义差（与 legacy unuse_pte 逐点）**：① 换入走直换入形
  （每 mm 私有新 folio），fork 共享条目换回后为两份私有副本——legacy 经
  swapcache 共享，冷路径内存形态差，正确性等价（各 mm 账目闭合）；②
  PGMAJFAULT 在 swap_in 内自增（unuse 触发的换回也计，属"换页类事件"的宽松
  口径，未改事务本体）；③ 毒读（PageHWPoison）→ -EFAULT 传播使 swapoff 失败
  ——legacy unuse_pte 会装毒标记 PTE 消化条目，arena 形答响亮失败（毒页 guest
  门无注入，升级路径 = 臂内补毒标记臂，登记不隐藏）。
- **瞬时竞争不失败**：-EAGAIN/描述符迁移/并发重换出 → 窗内重扫 + try_to_unuse
  外层 retry 收敛（V-D "有界自旋、可中断、非泄漏"契约的 W1.f 收口形态）；
  guest 判据 = swapoff 终究成功 + 无 D 态 + SIGINT 可断。
- **mve_s3_swapoff.sh 判据刷新**（bench/share/mve-battery/，guest 侧工件随判据
  更新）：branch B 语义从"盲持自旋 + 退出口收口"改为"W1.f sweep 自拉"，盲账
  从 note 升级为 FAIL；branch A 增加"盲账不动"判据。V-D 报告 §6 的两分支设计
  文本由本报告 §3/§4 接续，主会话跑新判据即可。
- **观测面终态登记**：implant_drops 补渲染（A.3a 起的漏挂点）；fork_demotes
  维持历史遥测（声明点注释已录）；unanchored_probes 无 W-1 生产者，W-2 匿名
  unanchor 时随 hwpoison 匿名枚举一并立项（OQ-W1-4 同批）。
- **规模注**：task 估 ~60-100 行；实落生产 +268/−24 净 +244（注释密度高：
  臂头收敛契约、三处盲账历史口径、渲染注记按 W1 系列惯例随码）+ 测试 +201。
  超注主因 = swap-in 复用省下的本体被契约注释换成了评审可读性，无删除性偏差。

## 5. 文件清单

- /home/ppw/linux-6.18-mva/mm/corten_arena.c（unuse_window/unuse_windows
  :14033-14190、blind 账注释翻转 :405/:4000/:3050、implant_drops 渲染 :2927）
- /home/ppw/linux-6.18-mva/mm/corten_arena.h（臂声明 :1001-1011 + =n 桩 :1297）
- /home/ppw/linux-6.18-mva/mm/swapfile.c（unuse_mm 挂臂 + 盲账残量形 :2457-2500、
  测试钩 :2503-2512）
- /home/ppw/linux-6.18-mva/include/linux/corten_arena.h（测试钩声明 :1040-1046）
- /home/ppw/linux-6.18-mva/mm/corten_fault_test.c（ft_legacy_swap_out :3431、
  unuse_windows 锚 :3503、注册 :3649）
- /home/ppw/cortenmm/bench/share/mve-battery/mve_s3_swapoff.sh（W1.f 判据刷新）
- /home/ppw/cortenmm/patches/r07-w1f.diff（导出，581 行；导出后对拍当前源一致）
- 日志：/home/ppw/cortenmm/results/r07/w1f/（kunit-on1/on2/off1/on-final +
  build-n/build-y-final + checkpatch-w1f.txt）
