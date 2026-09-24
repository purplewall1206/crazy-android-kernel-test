# W1.f2 开发报告 · S-3 升级臂：unuse 的条目形状对齐（swapcache-backed pull）+ branch A 环境修正

- 片: MV2 W1.f2（V-D 报告 §6 预案落地臂：W1_NATIVE_RMAP_SPEC.md §3.2 swapoff 行的
  S-3 gate 升级回炉——"对齐两种条目形态的枚举" + mve_s3_swapoff.sh 压力段修正）
- 基线: mv-a0 @ b280246bb2f2 · W1.f 未 commit 在树上（按红线在其上继续）·
  worktree /home/ppw/linux-6.18-mva
- diff: /home/ppw/cortenmm/patches/r07-w1f2.diff · **+793/−24**（922 行；= W1.f+W1.f2
  两波合并导出，W1.f 未 commit 故同树；W1.f2 增量 ≈ **+341/−0**：生产 +240——
  mm/corten_arena.c 单 hunk +235、mm/corten_arena.h +4、mm/swapfile.c +1；
  测试 +100——corten_fault_test.c 新锚；纯码核 ≈85 行，其余为系列惯例契约注释，
  W1.f 同款超注理由）· 对拍 r07-w1f.diff：同 hunk 位、臂块体 173→408、锚块
  206→306、头声明 19→23、挂入点注释 22→23
- 验证: make -j8 RC=0（终镜像 #161/#163，触碰文件零警告）· 三套件 KUnit
  on1/**on2**/off1 全绿（34/0/**5** · 7/0/**32** = W1.f 基线各 +1 skip = 新锚）·
  =n 折叠（vmlinux nm **零 corten 符号**）· checkpatch --strict patch 模式
  **0E/0W/0C**（922 行 "ready for submission"）· guest 门留主会话（脚本与重建的
  静态 workload 已就位 bench/share/mve-battery/，**guest 侧工件需重同步**）
- 日志: /home/ppw/cortenmm/results/r07/w1f2/
- 一句话拓扑: **branch B 的 D-state 不是枚举盲区复发而是 pull 形状缺口——真盘
  （异步写回）上驱动换出条目带着 SWAP_HAS_CACHE 滞留，W1.f 的直换入 pull 在
  swapcache_prepare 上 uninterruptible 空转永不收敛；W1.f2 在臂内把该形状改道
  unuse_pte 语义的 cached-folio map（M6 事务内提交），一次 unuse_mm 即收敛。**

---

## 1. branch B 根因链（gate 实况 → 代码级闭合）

gate 实况（results/r07/w1f/s3.log，W1.f 内核 + 旧判据 guest 工件）：
swapoff 陷入 **uninterruptible D-state**，SIGINT 不可断；期间 **swapins=0**、
**unuse_blind_mms 0→0**；holder exit 后 zap_swap_frees +16384（exit walk 释放）、
inuse 归零、重试 swapoff PASS。四个观测逐条对应下链：

1. **gate 的 swap 源是真实 ext4 swap file**（/root/bench/s3swap）——异步写回
   设备。branch B 的 16384 页全部经 shrinker pick → **W1.e1 驱动**换出。
2. **驱动在异步设备上的 post-transaction keep**（vmscan.c:1483 的镜像）：事务
   提交后（PTE=swap entry、meta=CORTEN_SWAPPED，与 M6 完全同形——两形状都经
   `corten_rmap_swap_out()` 提交，meta 层无法区分），`swap_writeout` 只提交
   I/O → `folio_test_writeback` → driver `goto keep` → **folio 滞留 swapcache、
   swap_map = (PTE + SWAP_HAS_CACHE)**，keep 臂丢弃 pick 引用。zram 同步写走
   folio_free_swap 捷径、条目 cache-less——KUnit（QEMU 无 swap，锚 skip）与
   旧 guest 门（zram）从不见该形状，W1.f 锚全绿不矛盾。
3. **滞留的 cache folio 永不被清理**：上游靠"下一次 shrink pass 清掉
   swapcache folio"收敛，而 arena folio 从不入 LRU（DEV-10）——该 folio 在
   PTE 引用消失前是回收不可见的滞留件（W1.e1 异步形状的既成事实，本片不改
   驱动，见 §4 登记）。
4. **W1.f 枚举臂扫描完全正确**：注册表枚举、meta 扫描、type 过滤全部命中
   16384 槽 → 逐条 `corten_arena_swap_in()` 直换入 → `swapcache_prepare(entry)`
   = `__swap_duplicate(entry, SWAP_HAS_CACHE)` → **-EEXIST** → swap_in 内
   `schedule_timeout_uninterruptible(1)` 微睡重试 5 秒（/proc 采样即 **D 态**、
   SIGINT 永远落在 uninterruptible 窗口内——**两个"escalate"观测的出处**）
   → -EAGAIN → 臂按"瞬时竞态"契约忽略 → 下一 pass 同批槽 → 8 pass 后按契约
   return 0（**成功**）→ try_to_unuse 外层 retry 永不收敛。swapins=0（计数器
   在 swap_in 提交腿末尾，pull 从未到达）、盲账零（无硬失败）——臂"体面地
   什么都没拉回"。
5. holder exit → exit walk zap 释放 PTE 引用（zap_swap_frees +16384）+
   free_swap_and_cache 收掉 cache 件 → inuse→0 → swapoff 通过、重试 PASS——
   V-D 防线如设计兜底，但正臂确实空转。

branch A（同 log）：旧 guest 工件的 workload s3 模式在读回段 **segfault** →
holder 死 → exit walk 先清条目 → swapoff 走 `!swap_usage_in_pages` 早退——
"clean PASS" 是**早退假阳性**（log 里脚本自己注明），读回从未发生
（swapins=0）。叠加刷新版脚本自身的环境缺陷（§3）。

**结论**：V-D §6 预案的枚举臂（W1.f）已覆盖 unuse_mm 场景；branch B 播出的是
**pull 的第二种条目形状**——协调层预判的"unuse 枚举只认一种条目形态"成立，
但差异不在 meta 编码（两形状同形），在 **swapcache 成员性**（设备的写回
同步性决定）：cache-less 条目走直换入，swapcache-backed 条目须 map **cached
folio 本尊**——上游 unuse_pte_range() 的既有答案。

## 2. 修法（W1.f2 臂内形状对齐）

### 2.1 `corten_arena_unuse_cache_pull()`（mm/corten_arena.c，unuse 块内）

swapcache-backed 条目的 pull = **unuse_pte_range() 语义经 M6 事务**：

- pull 循环对每槽先 `swap_cache_get_folio(entry)`（meta 外唯一的形状判据）：
  命中且 `folio->swap == entry` → cache-pull；未命中或 decache 竞态 → 回退
  既有直换入（`corten_arena_swap_in` 本体逐字节未动，zram/同步形状原路）。
- cache-pull 序（folio 由调用方 lock + wait_writeback，unuse_pte_range 纪律）：
  毒页/非 uptodate → -EFAULT（臂的响亮对，W1.f 口径）→ 事务重验证
  （meta==CORTEN_SWAPPED 且 entry 同、PTE==swap entry——R-W1-7 再验证）→
  `corten_meta_ensure_locked`（与 swap_in 提交腿配对）→ **MM_SWAPENTS−1/
  MM_ANONPAGES+1、folio_get、arch_swap_restore、装 PTE**（soft-dirty 携带、
  OQ-M6-8 young 恩典、rmap 按 folio anon 与否取 `folio_add_anon_rmap_pte`/
  `folio_add_new_anon_rmap` 分支——unuse_pte 同 fork；VMA-less 跳过，驱动
  从不换出 anchorless 窗故该形状防御性）→ **SWAPPED→MAPPED 同事务**
  （corten_map；非 exclusive 条目 corten_mark 重臂 CORTEN_PF_SHARED+WRITABLE，
  fork-COW 契约与 swap_in 提交腿同形）→ 事务外 `swap_free`（PTE 引用，持过
  meta 提交——swap_in 的 strictly-stronger 形）。
- **收敛分工与上游逐点同构**：cache 引用不归臂管——PTE 引用落掉后条目即
  cache-only，try_to_unuse() 自己的 entry 循环 `folio_free_swap()` 收尸，
  设备清零（swapoff 的既有机制，零新增消费者）。
- **锁序**：folio lock > desc write lock > ptl（驱动/换入同向，DEV-13 零新
  边）；运行于 unuse_mm 的 mmap_read 下（arena 生命周期稳定）；无设备 I/O
  （cached folio 已 uptodate——pull 成本 = 装载，非换入读）。
- **账目**：`corten_nr_swapins` +1（与直换入 pull 同账："arm pull 统一计、
  legacy unuse_pte 不计"的双口径对账不变，branch B 的 guest 判据由此可断言
  swapins 前进）；PGMAJFAULT 不计（cache hit 无设备读，do_swap_page 同口径）。

### 2.2 KUnit 锚：`corten_fault_test_unuse_windows_cache`（+100 行）

真链复现 branch-B 形状：`ft_swap_out`（真 ttu→驱动路由）换出窗口槽
（zram → cache-less）→ **`read_swap_cache_async()` 把 cache 成员种回去**
（= try_to_unuse 预载/预读形：folio + SWAP_HAS_CACHE + 内容回读，条目恰为
真盘 keep 形）→ `corten_arena_test_unuse_mm()` 一次调用断言：PTE present、
内容逐位（0x5c 两点）、meta CORTEN_MAPPED + FT_PERM_RW + payload 归零、
MM_SWAPENTS 1→0、swapins 台账恰 +1、盲账零前进 → 再亲手走 try_to_unuse 的
entry 循环形（swap_cache_get_folio + lock + `folio_free_swap`）断言条目死亡
（swap_duplicate < 0）。无 swap 双 skip 门约定与姊妹锚一致（QEMU 自动 skip，
guest 套件复跑自动实跑）。

### 2.3 分工边界（红线核对）

| 红线 | 本片执行 |
|---|---|
| INV6（窗口 PTE 写必经事务） | cache-pull 的一切窗口 PTE+meta 写在 `corten_lock_range` 事务体内（ptl 嵌套 desc(W) 内）；扫描只读；零新裸写点；白名单 #2/#3 原样 |
| M6/W1.a-e 本体零触碰 | `corten_arena_swap_in`、驱动、rmap 逐字节原样——本片全部改动在 unuse 块单 hunk（+408，对拍 r07-w1f.diff 同位） |
| =n 折叠 | =n 构建 RC=0，`nm vmlinux` 零 corten 符号（build-n.log；仅基线 objtool cpuidle + modpost memblock 两警告） |
| 不 commit | 树上 5 文件 modified，未暂存未提交 |

## 3. branch A 环境修正（mve_s3_swapoff.sh + mve_workload）

环境根因两条：① `hold` 模式只 map 4MiB、touch 1 页，而脚本等 32MiB VmSwap
——**目标不可达**；python hog 70% 水位 + 2MiB 间隔 touch 压力不稳；② 真读回
+ 内容校验缺位（旧 guest 工件的 s3 模式 segfault 后早退假阳性，§1.5）。

修正（bench/share/mve-battery/，**guest 侧工件随判据更新——需重同步**）：

- **mve_workload.c 新 `s3 <secs> <mb>` 模式**（静态产物已重建，gcc -static
  -O2 -Wall -Wextra 零警告）：MODE on + 一段 <mb> MiB 窗口 RW 区，逐页页索引
  pattern 填充（word 0x5c5c0000|idx + 尾字节 idx echo），打印
  `S3 ready pid= base=0x… pages=… sum=0x…`；SIGUSR1 重走窗口逐页校验，答
  `S3 verify PASS/FAIL sum=…`——换出/读回往返的内容证明。main 分发与 usage
  同步；既有 brk/hold/j4 零触碰。
- **mve_s3_swapoff.sh 按 W1.f2 判据重写**：
  - swap 源：默认 **SWAPFILE=/root/bench/s3swap**（真盘异步形状 = branch-B
    复现前提，文件头注明）；SWAPDEV=/dev/vdX 块设备可选。
  - 压力段（参照 W1.e2 gate 形）：`sync; echo 3 > drop_caches`（slab shrink
    一腿直达 corten shrinker）+ **全触碰** anon hog（bytearray 逐 MiB 构造即
    memset 全写 + b[0]=1 防惰性零页，90% MemAvailable）轮次驱动 kswapd；
    **swapped_out 台账必须前进、VmSwap 达 3/4 窗口目标——"below target,
    continuing" 静默降级判据删除**（换硬 FAIL）。
  - branch A：fill → 压力 → `dd if=/proc/pid/mem`（GUP 慢道窗口面）整窗读回
    + **swapins 必须前进** + SIGUSR1 **verify PASS**（内容）→ holder exit →
    swapoff 干净 PASS + 盲账不动 → 重新 swapon。
  - branch B：fill → 压力 → 不读回直接 swapoff：10s 采样**不得 D 态**、
    HOLD_SECS 内**自行完成**、期间 **swapins 前进（两形状 pull 的证据）**、
    一次成功（若设备要靠 holder exit + retry 收口 = **FAIL**——V-D 预案的
    升级答案，不再是 PASS 注记）、盲账恒零。
  - 判据收口：PASS iff branch A（swapped_out>0 ∧ swapins>0 ∧ verify PASS ∧
    swapoff rc==0）∧ branch B（无 D ∧ rc==0 一次过 ∧ swapins 前进 ∧ 设备脱）
    ∧ 全程 unuse_blind_mms 恒零。

## 4. 验证结果

| 门 | 结果 |
|---|---|
| make -j8（=y） | RC=0（#159→#161→#163 终镜像；触碰文件零警告；仅基线 objtool cpuidle + modpost memblock 两件，r07 各片 in-file） |
| KUnit on1（corten=on） | **corten 24/0/1 · corten_arena 104/0/0 · corten_fault 34/0/5** 全绿（+1 skip = 新锚无 swap 门约定；W1.f 基线 34/0/4） |
| KUnit on2（flake 复跑） | 同数全绿 |
| KUnit off1（corten=off） | **25/0/0 · 24/0/80 · 7/0/32** 全绿（+1 = 新锚 off skip；基线 7/0/31） |
| WARNING/BUG 指纹对拍 | on1 vs W1.f on1：20 行 pgtables_bytes 恒同 + 10 条 WARNING 同族同数（corten_arena.c 三处源行号 +7 = 本片插入位移，其余恒同），零 lockdep/oops/GPF 签名；三日志零 "not ok" |
| 终镜像复跑（=y 干净重建 #163） | **24/0/1 · 104/0/0 · 34/0/5** 全绿同数，零 "not ok"，pgtables_bytes 20 行恒同（kunit-on-final.log） |
| =n 折叠 | RC=0；vmlinux nm **零 corten 符号**（CORTEN_MM/ARENA/三 KUNIT_TEST 全 =n）；仅基线两警告 |
| checkpatch --strict（patch 模式） | **0E / 0W / 0C**，922 行 "ready for submission"（checkpatch-w1f2.txt；diff 对拍导出后源零改动） |
| guest 门（留主会话） | ① mve_s3_swapoff.sh 重跑（branch B：swapoff 一次成功不 D 态、swapins 前进、盲账恒零；branch A：swapped_out>0 + swapins>0 + verify PASS）——**先重同步 guest 工件：mve_s3_swapoff.sh + 重建的静态 mve_workload**；② 新锚 unuse_windows_cache 在 guest 有 swap 时自动实跑（zram 上与直换入锚互补） |

终镜像复跑补注：=n 验证后已恢复 =y config 重建（build-y-final.log），终镜像
复跑 on1 确认与 #161 位形一致（全绿同数）。

## 5. 登记与披露

- **W1.e1 异步形状的滞留件（既有事实，非本片缺口）**：真盘上驱动的写回 keep
  会留一个不入 LRU 的 swapcache folio，直到该条目 PTE 引用消失（unuse/exit/
  fault 消费）。W1.f2 的 cache-pull 消费它后即由 try_to_unuse 收尸，swapoff
  场景闭环；非 swapoff 场景该 folio 随条目在 fault/exit 消费时
  free_swap_and_cache 清理。驱动本体若要在异步设备上自行收尾（如 keep 前等待
  写回 + __remove_mapping），是 W1.e1 范围的独立立项，本片红线不动驱动。
- **branch B 判据收紧**：旧判据"exit walk + retry PASS"算 PASS；W1.f2 判据下
  那是 FAIL（正臂必须一次收敛）——防线仍在（exit walk 兜底 + signal 契约），
  但不再是合格形状。
- **直换入腿的 5 秒 -EEXIST 空转保留**：fork 并发换入的串行化机制本身正确
  （cache 真在途时等待），本片把"cache 滞留"形状从它面前分道后它不再暴露于
  unuse 路径。
- **规格注**：task 预案 "~60 行" 估的是枚举臂本体（W1.f 已落）；W1.f2 的实授
  范围是形状对齐 pull——实落生产 +240（纯码核 ≈85 行 + 契约注释），测试
  +100，脚本/workload 另计（bench 件不入内核 diff）。两波合并 diff 922 行，
  W1.f 增量部分与 r07-w1f.diff 逐行同（对拍 hunk 位）。
- **gate 工件代际注**：w1f/s3.log 由旧判据 guest 工件产生（其字符串与本片
  bench/share 刷新版不同源）——该 log 的四观测仍为 W1.f 内核的真行为，本片
  根因链据此闭合；刷新版脚本在本片完成前未经 guest 实跑，其 branch A 压力段
  缺陷（hold 4MiB/目标 32MiB 不可达）为代码级论证，已随重写消除。

## 6. 文件清单

- /home/ppw/linux-6.18-mva/mm/corten_arena.c（unuse_cache_pull + pull 循环
  形状分道 + W1.f 臂注释 W1.f2 段，单 hunk :14022-14430）
- /home/ppw/linux-6.18-mva/mm/corten_arena.h（unuse_windows 声明注释 W1.f2 段）
- /home/ppw/linux-6.18-mva/mm/swapfile.c（unuse_mm 挂入点注释一行）
- /home/ppw/linux-6.18-mva/mm/corten_fault_test.c（unuse_windows_cache 锚 +
  注册，:3409 区）
- /home/ppw/cortenmm/bench/share/mve-battery/mve_s3_swapoff.sh（W1.f2 判据重写）
- /home/ppw/cortenmm/bench/share/mve-battery/mve_workload.c（s3 模式；
  + 静态产物 mve_workload 已重建）
- /home/ppw/cortenmm/patches/r07-w1f2.diff（导出，922 行）
- 日志：/home/ppw/cortenmm/results/r07/w1f2/（build-y/y2/y-n/y-final、
  kunit-on1/on2/off1、checkpatch-w1f2.txt）
