# W-3fix5 dev report（脏域有界化的 park reset —— 性能刀 2，台账 #11）

agent: w3fix5-dev（CortenMM kernel-MM; MV2 W-3fix5 片）。
基线: worktree /home/ppw/linux-6.18-mva @ b4b425245193（分支 mv-a0, MV2 W-3fix4 收口态）。
不 commit。工件 results/r07/w3fix5/（本树）+ /home/ppw/cortenmm/patches/r07-w3fix5.diff。
VM 命名空间 w3fix5（PORT 10033, qemu-w3fix5.pid, tmux w3fix5-vm）——与 mv3d(10031)/
mv3cf(10032) 全隔离; mv3d 长 battery VM 未触碰。

---

## 0. 判定总览

| 项 | 判定 |
|---|---|
| corten_unmap() 脏域夹取（meta 层 O(dirty) 收口） | **绿（落地, 逐输入语义等价, KUnit 锚红→绿）** |
| park 路径 PTE 级走查点裁决（实施内容 2） | **绿（裁决=保持全幅; 依据+解锁条件在案, §3）** |
| widen_dirty 调用点对称性核对（实施内容 4） | **绿（无缺口, §4）** |
| KUnit on×2 + off | **绿: 26/0/1 + 141/0/0 + 34/0/5 ×2 逐格一致; off skip 对账 +1 恰=新 arena 锚** |
| =y / =n 构建 | **绿: 零新增警告（唯一=基线 objtool cpuidle）; =n rc=0, 消费对象零 corten 符号** |
| checkpatch --strict 全量 diff | **0E/0W/0C（390 行, "no obvious style problems"）** |
| mmbench 动态协议 | **跑通（dyn+hook 判据件; 判定=噪声持平, §6 如实报）** |

**一句话**: W-3fix5 把脏域有界化收口到 meta 层最后一环——`corten_unmap()` 自身的
validate/reset 两个循环对未覆盖区间改为 O(1) 答案（与全幅走查逐输入等价），并以
3 个新 KUnit 锚锁死契约面。park 热路径的 meta 读自 perf2a/台账 #11 落地起已是
O(dirty)（本次核实），因此动态协议读数持平是**分析预期的如实结果**，不是回归;
残余的 park 成本在 PTE 级全幅走查（有意保留，§3 给出不能夹的证明与解锁条件）。

---

## 1. 改动统计（5 文件, +335/−4）

| 文件 | 行数 | 内容 |
|---|---|---|
| mm/corten.c | +41 | `corten_unmap()` 两循环脏域夹取（O(1) 未覆盖拒绝 + 注释内的完整 INV6 论证） |
| include/linux/corten.h | +9/−4 | `corten_unmap()` 契约文档补脏域夹取语义（原文仍逐字为真） |
| mm/corten_arena.c | +11 | zap 侧 Ledger #11 注释收口（meta 层已闭合）+ PTE 级不夹的证明与登记 |
| mm/corten_test.c | +159 | 两锚: `corten_test_unmap_dirty_clamp` / `corten_test_unmap_dirty_empty` |
| mm/corten_arena_test.c | +115 | 一锚: `corten_arena_test_dirty_range_scattered`（4 散槽含双 u16 端点） |

## 2. 为什么 O(dirty) 安全 —— INV6 论证（实施前成文, 逐字入码）

**不变式（INV6, 台账 #11 落地时已依赖的同一信任级——zap 侧 indirty 门
corten_arena.c:11928 的既有论据）**: 在 covering 描述符写锁下, 任何时刻,
`[rec_lo, rec_hi]` 之外的槽必然 `CORTEN_INVALID`。

证明三段:
1. **每个非 INVALID 态都加宽脏域**: 状态机的全部内容写入 = `corten_map()`
   (mm/corten.c:1239) / `corten_mark()` (:1303) / `corten_swap_replay()`
   (:1419), 三者都在 covering 写锁内调 `corten_desc_widen_dirty()`; arena 侧
   的 sweep-adopt 回填、fork 镜像、MV3.c 批 mark 也全部经这三口（§4）。
2. **脏域不丢活状态**: 唯一收缩 = 全窗 park 塌缩（corten_arena.c:12243）, 而
   塌缩只发生在一次把窗内全部已记录槽 reset 完毕的 zap 之后（已记录 ⇒ 在域内
   ⇒ indirty 门已访问）; 不存在"塌缩后有槽仍非 INVALID"的形态。
3. **逆否**: 槽 ∉ [rec_lo, rec_hi] ⇒ 自上次塌缩以来从未被记录（或已被塌缩前
   的 reset 抹回 INVALID）⇒ state == INVALID（数组 kzcalloc 清零, 未写槽读 0）。

**对 `corten_unmap()` 的推论（逐输入语义等价）**: 设子范围 [start, end) 的
首/末槽 PTE-index 为 i0/i1。
- 若 [i0, i1] ⊄ [rec_lo, rec_hi]: 区间内存在必然 INVALID 的槽 ⇒ 旧代码的
  validate 循环**必然**在该槽（或之前）返回 -ENOENT 且零写入（validate-then-
  apply 原子性）。新代码在循环前直接答 -ENOENT: 同一裁决、同一副作用（无）、
  O(1) 而非 O(区间)——两个循环被完整跳过, 正是"空走查"的消除。
- 若 [i0, i1] ⊆ [rec_lo, rec_hi]: 夹取交集=区间本身, 循环与旧代码逐迭代相同;
  且非 INVALID 槽必在域内, **任何 reset 工作都不会被跳过**。

头文件契约（"Every page in the sub-range must be recorded, else -ENOENT and
nothing is changed"）保持逐字为真——域外槽=未记录=-ENOENT, 只是答案从 O(n) 走
查变为 O(1) 判定。现有测试逐一复核不受影响（txn_atomic_validate 的 [8,10) 跨
域案例: 脏域 [8,9] 由首轮 mark 加宽后 unmap 不收缩, 覆盖仍成立 ⇒ -ENOENT 保持;
full_window_atomic 的 [0,512) 案例同理）。KEEP_PERM 遗留 perm 的 INVALID 槽:
validate 只查 state ⇒ -ENOENT 判定不变; reset 本就到不了它。

**u16 安全**: `pte_index() < PTRS_PER_PTE` 恒 fit u16（widen 处同款 cast;
PTRS_PER_PTE ≤ 8192 even 64K 页）; `corten_lock_range()` 钉死子范围在单 PT 页
内 ⇒ 首/末 index 单调不回绕。跨迭代一致性: reset 不收缩脏域, 塌缩只发生在
全窗 zap 成功后 —— park 后再 take, 新 mark 在空域上逐字取首槽（
`corten_test_unmap_dirty_empty` 的塌缩-再 mark 腿 + arena 锚的 [3,3] 断言）。

## 3. 实施内容 2 的裁决: park 路径 PTE 级走查点保持全幅

排查了 zap 驱动（corten_arena_unmap_chunk_flags :12416 while 主循环 +
corten_arena_zap_window 内环）与全部 pte_none/pte_present 全幅扫描点:
`:12025`（tracked zap 的 any_pte 预扫, 仅 !tlb 臂）、`:12326`（untracked 窗
zap）、`:2302`（check_empty 探针, DECLARE 臂——非 park 路径）。结论: **三处
都不能安全夹取**, 维持全幅（现注释已把该裁决与依据写入 :11928 段）:

1. **r03 缺陷 C 纪律（既有, 本片复核成立）**: PTE 可以存在于 meta 之外——
   punch implant（窗域内合法 tree-VMA）经 legacy funnel 写纯 PTE, 不经事务、
   不加宽脏域。夹取 PTE 走查 = 漏清 implant 的翻译 = r03 arena_stress 魔数
   读取复现。meta 层无此洞（implant 不写 meta, INV6 只约束 meta）。
2. **force-resume 补窗洞（本片新分析, 登记为前置）**: 批溢出轮间写锁下放,
   已 zap VA 上的补窗 fault 其 mark 会加宽真实 rec 但轮 1 的 dirty 快照看不
   见; 塌缩与 indirty 门对该槽的既有处理已依赖"补窗落在已扫区"的形状。PTE 级
   夹取会把该竞态从"meta 残留"放大为"活翻译穿 park"。
3. **untracked 窗无域可夹**: 无描述符即无 rec 域。

**解锁条件（后续刀的登记）**: ①implant 门（park 路径在 mmap_write 下做
`corten_implant_covers` 逐窗检查, 无 implant 才夹）; ②force-resume 的 epoch
处理（轮间重读 dirty 或塌缩前重扫）。两者齐备前, park 的 O(512) 残余 = PTE 级
none-槽 xchg 空走查, 与 meta 层无关。

## 4. 实施内容 4: widen_dirty 调用点对称性核对 —— 无缺口

| meta 写入形态 | 路径 | 加宽 |
|---|---|---|
| 单页 map（fault FRESH/COW 装页） | `corten_map()` :1239 | ✓ |
| 范围 mark（虚拟分配/sweep/镜像/批 mark） | `corten_mark()` :1303 | ✓ |
| swap 槽回放（fork 镜像/sweep 回放） | `corten_swap_replay()` :1419 | ✓ |
| MAPPED→SWAPPED（换出） | `corten_swap_out()` | 无需: 槽已记录（MAPPED ⇒ 在域内; 域只在全窗塌缩收缩, 塌缩前槽已 reset） |
| arena 侧全部 meta 写（sweep-adopt 回填 :7039/:7052/:7065、fork 镜像 :7150/:7205/:8701-8742、批 mark :10019/:10114、file 臂 :2391/:10467） | 全部经 `corten_map/mark/swap_replay` | ✓（无一处绕过 txn API 直写 meta） |

## 5. KUnit 锚清单（新增 3, 全绿; on1/on2 逐格一致）

| 锚 | 套件 | 锁死的契约 |
|---|---|---|
| `corten_test_unmap_dirty_clamp` | corten (ok 25) | 4 散槽（7/400/511）roundtrip: 域下单页 reset=0、域下单 u16 端点(511) reset=0、域外(0)=-ENOENT、跨 rec_lo 下沿原子失败且内容不动、域内空洞跨距原子失败、覆盖式多页整段 reset=0（O(dirty) 赢面形态）、rec_lo=0 低端点 |
| `corten_test_unmap_dirty_empty` | corten (ok 26) | 空域（新描述符）单页/整窗=-ENOENT 零写入; park 塌缩形状（手塌 rec）后 reset 槽再 unmap=-ENOENT; 新 mark 逐字接管域（[300,300], 跨迭代一致性） |
| `corten_arena_test_dirty_range_scattered` | corten_arena (ok 135; =off 正确 SKIP) | 真 fault 臂 4 散槽（0/7/505/511——双 u16 端点）: rec=[0,511]+nr_mapped=4 → park → nr_mapped=0+域塌缩（reset 只碰已记录少数）→ take 后内点首 mark=[3,3]（无 [0,511] 残留）→ 低端点加宽跨它=[0,3] |

既有锚 `corten_arena_test_dirty_range_park`（2 散槽+塌缩+再 take）继续绿
（ok 134）。off 轮 skip 对账: 恰 +1（新 arena 锚, 理由串 "requires corten=on"）,
两协议锚 =off 照常运行且绿。

## 6. mmbench 动态协议（如实报: 噪声持平, 与分析一致）

协议: mmbench **dyn**（`/mnt/cortenmm/bench/mmbench/mmbench_dyn`——静态件吃不进
LD_PRELOAD, 首轮 6 格假读数已废弃重跑, mv3cfeat §2.1 同款 artifact 再现并规避）
+ `corten_mode_hook.so` 进场 + seed 公式（20260951-53 / 20260979-81, 与 committed
基线逐 seed 配对）+ min_seconds=2 + 三遍取中位。=on 判 boot: `pool_parks=41892`
（≈1 park/op, MODE 路径实走）; stock 臂 `pool_parks=0`（真 legacy）; 两 boot
dmesg 零内核签名。

| cell（mmap-pf low, ops/µs, 中位） | ① before=673 时代 committed（a5on JSON） | ② paired-now base（bzImage-mv3d-y, 本机本刻） | ③ after W-3fix5 | ③vs① | ③vs② |
|---|---|---|---|---|---|
| t4 | 0.0010228 | 0.00114782 | 0.000997495 | **−2.5%** | −13.1% |
| t8 | 0.000346119 | 0.000340322 | 0.00038118 | +10.1% | +12.0% |

| 税线（同 boot dyn stock 对 MODE） | stock | MODE | 税 |
|---|---|---|---|
| t4（W-3fix5 内核） | 0.00138566 | 0.000997495 | −28.0% |
| t8（同上） | 0.000711166 | 0.00038118 | −46.4% |
| （a3 时代对照） | 0.00290994 | 0.000456644 | −84.3% |

**判定: 噪声持平。** ②③同机同刻成对、方向相反（−13%/+12%）, 而宿主当下
load≈35（mv3d 长 battery 同宿主在跑）——读数带 ±13% 为宿主噪声带, 判据格
无显著位移。这与 §1/§3 的代码分析一致: park 热路径的 meta 读**早已** O(dirty)
（perf2a 的逐槽 slot 读 + indirty 门已落地）, 本片夹取的是 `corten_unmap()`
对多页调用的空走查（生产调用面=file-declare scrub 与协议层, park 的逐页调用
天然落在覆盖区=逐输入等价）, 不触碰该格。税线 −28%/−46% 优于 a3 的 −84%:
warm park（a5）+ 本机条件差异的合成, 跨时代 stock 绝对值不可比（披露）。

## 7. 验证门一览

| 门 | 结果 |
|---|---|
| =y 构建（clean 重建 #395 + =n 往返还原 #397） | rc=0; **零新增警告**（唯一=stock objtool cpuidle 基线, 在案） |
| KUnit on×2（filter_glob=corten*, diskless 直启） | 绿×2 逐格一致: 26/0/1 + **141/0/0** + 34/0/5（含 3 新锚, 无 flake 无复跑） |
| KUnit off | 绿: 27/0/0 + 28/0/113 + 7/0/32; skip 恰 +1=新 arena 锚; 两协议锚 =off 照常绿 |
| =n 构建 | rc=0; memory/mmap/exec/fork/migrate/rmap 六消费对象 `nm` 零 corten 符号; 陈旧 .o 归档 n-stale-objects/ |
| checkpatch --strict 全量 diff | **0E/0W/0C**（390 行, "ready for submission"） |
| mmbench 动态协议 | §6（噪声持平, 判据格 MODE 实走 41892 parks） |
| guest 冒烟 | =on boot 双轮 dmesg 零 BUG/WARNING/Oops 签名（grep 命中均为 ACPI/systemd 文案） |

## 8. 移交/待办

1. **PTE 级夹取刀**（§3 解锁条件: implant 门 + force-resume epoch）——park 的
   O(512) 残余真正归零的那一刀; 本片把裁决与前置写入 zap 注释与本文。
2. per-fault 簿记批化（mv3cfeat §2.4-2, 独立小片）。
3. mmbench 稳态读数建议在安静宿主窗重跑配对 A/B（本机 mv3d battery 同宿主
   运行中, ±13% 噪声带盖过判据格位移; 协议与 seed 已在 results/ 可复跑）。

## 9. 工件清单（project/results/r07/w3fix5/）

build-y-w3fix5-1/2.log, build-n-w3fix5.log, build-y-restore.log,
config-y-w3fix5.snapshot, n-stale-objects/, kunit-on1/on2/off.log,
kunit-totals.txt, console-w3fix5-on-bench.log, console-w3fix5-on-bench2.log,
console-w3fix5-base-bench.log, bzImage-w3fix5（sha256 前缀 b31898fc5ddd5307,
内核 6.18.32-gb4b425245193-dirty #395）, bench/（mmpf-mode-t{4,8}-k{1..3},
mmpf-stock-t{4,8}-k{1..3}, base-paired/mmpf-base-t{4,8}-k{1..3}）,
w3fix5-full.diff（= /home/ppw/cortenmm/patches/r07-w3fix5.diff）。
