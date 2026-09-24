# W1.a 开发报告：vma-free rmap wrapper 四函数（MV2 W-1 第一片，纯加法零行为变化）

切片: MV2 W1.a（W1_NATIVE_RMAP_SPEC.md §5 表首行: rmap.c ~90 + corten_arena.h ~30 + 测试 ~120）
worktree: `/home/ppw/linux-6.18-mva`（分支 mv-a0 = 主树 037bfbaed020 fast-forward 同步 + 本片未提交增量）
日期: 2026-09-22/23
patch: `/home/ppw/cortenmm/patches/r07-w1a.diff`（= git diff HEAD，3 文件 **+251/−0**：
生产码 mm/rmap.c +100、mm/corten_arena.h +30，测试 mm/corten_arena_test.c +121）
状态: **达成** —— =y 构建零新增警告、KUnit 三套件全绿（corten=on 两连跑 on1/on2 判 flake +
corten=off off1 零扰动）、=n 十四对象（含 mm/rmap.o）零警告零 corten 符号、checkpatch
--strict 0E/0W/0C、INV6 红线实测成立（diff 零 PTE 写点、wrapper 全树零调用点）。未 commit。

## 0. 结论一句话

四个 novma wrapper（`folio_{add,remove}_{anon,file}_rmap_novma`）落 mm/rmap.c，逐行镜像
`__folio_add_rmap`/`__folio_remove_rmap` 的 order-0 PTE 分支，把 vma 依赖面（`__folio_set_anon`
的 mapping/index、mlock_vma/munlock_vma、地址范围告警、VM_DROPPABLE 项）留给调用方事务，
vma 无关的位/计数（mapcount、lruvec stat、swapbacked、AnonExclusive）归 wrapper 所有；
lruvec 桶为**显式入参**而非 `__folio_mod_stat` 的 `folio_test_anon()` 分派——novma folio
的 mapping 合法为 NULL（W-2 unanchor 形态），沿分派会错记 NR_FILE_MAPPED。合成 folio
KUnit 锚四判据（①roundtrip 计数 ②位语义 ③free 过检 ④=n 折叠）全绿，本片零调用点，
回归集原样。

## 1. 改动点清单

### 1.1 mm/rmap.c +100（`#ifdef CONFIG_CORTEN_MM_ARENA` 内，紧随 folio_remove_rmap_pud）

| 函数 | 语义（对照 spec §2.2 签名注释） |
|---|---|
| `folio_add_anon_rmap_novma(folio)` | `folio_add_new_anon_rmap` 去 vma 项：`!swapbacked → __folio_set_swapbacked`（VM_DROPPABLE 是 vma 属性，窗口无此项）+ `atomic_set(&_mapcount, 0)`（new-folio 契约，预态 -1 有 VM_WARN）+ `SetPageAnonExclusive` + NR_ANON_MAPPED +1。不碰 mapping |
| `folio_remove_anon_rmap_novma(folio)` | order-0 remove 分支：`atomic_add_negative(-1)` 最后一次时 NR_ANON_MAPPED −1 **并清 AnonExclusive**（判据②的"最后摘除清位"） |
| `folio_add_file_rmap_novma(folio)` | `atomic_inc_and_test` 首映射时 NR_FILE_MAPPED +1；保留 `__folio_add_file_rmap` 的 `folio_test_anon` 家族守卫 |
| `folio_remove_file_rmap_novma(folio)` | 对称 −1 |

共同形态与取舍（rmap.c banner 注记在案）：
- 复用 `__folio_rmap_sanity_checks(folio, &folio->page, 1, PGTABLE_LEVEL_PTE)`（rmap.h:397，
  本就不收 vma——本片可行性根据）+ 自加 `folio_test_large` VM_WARN（仅 order-0）。
- **为什么不全量调 `__folio_add_rmap`**：其 `__folio_mod_stat` 按 `folio_test_anon()`（读
  folio->mapping 低两位）分派 stat 桶；novma 匿名页 W-2 后 mapping==NULL 会错记 file 桶。
  故镜像 order-0 分支算术（逐行同源）、桶显式传参。这同时把 spec OQ-W1-6（anon unanchor
  后 NR_ANON_MAPPED 维持）在接口层预答：W-2 复用本 wrapper 不需要任何改动。
- AnonExclusive 清位点：上游 remove-rmap 从不清此位（fork/GUP/swap-encode 各消费者自清）；
  novma 世界没有这些 vma 侧消费者，spec 判据② 直接要求 wrapper 清。PG_anon_exclusive =
  PG_owner_2（owner 复用位），最后一次摘除清位给下一任 owner 确定初值。
- `mod_mthp_stat`（order>0 才生效，huge_mm.h）与 `deferred_split_folio`（large only）在
  order-0-only wrapper 语义外，无需镜像（banner 注记）。
- 上游 rmap 任何既有函数零改动——diff 纯加法（−0 行）机器可证。

### 1.2 mm/corten_arena.h +30（照既有惯例折叠）

- =y 块（corten_rmap_* 原型簇后）四个原型 + 语义 banner（配对契约、桶归属、位归属）。
- `#else` 块四个空 `static inline` 桩（紧随 corten_rmap_swap_out 桩）——=n 全折叠，
  mm/rmap.c 本就 `#include "corten_arena.h"`（:78），零新增 include。

### 1.3 mm/corten_arena_test.c +121（`corten_arena_test_novma_rmap` + stat 读取 helper）

合成 order-0 folio（`folio_alloc(GFP_KERNEL, 0)`）直驱，零 arena 状态、零调用点（INV6：
纯基建，folio 本身即全部 fixture）。注册位在 `drain_timeout_stat` 之后、V-E 白名单对之前
（whitelist_audit 注记要求保持末位，其累计 gate 判定不受本测污染）。

## 2. KUnit 锚（判据 ①-④ 对账）

| 判据 | 落点 | 断言 |
|---|---|---|
| ① roundtrip | `corten_arena_test_novma_rmap` | `_mapcount -1→0→-1`；NR_ANON_MAPPED / NR_FILE_MAPPED 恰 ±1（启动期无其他 user mapping，期望精确值）；两族桶互相独立（anon add 时 file 桶不动，反之亦然） |
| ② 位语义 | 同上 | add 置 swapbacked + AnonExclusive；remove 最后一次清 AnonExclusive；swapbacked 保留（folio 类别非计数，`folio_add_new_anon_rmap` 语义镜像） |
| ③ free 过检 | 同上 + 全量日志 | `page_expected_state` 是 page_alloc.c 私有 static → 逐项断言其条款（mapcount==-1、mapping==NULL、refcount==1、`flags.f & PAGE_FLAGS_CHECK_AT_FREE == 0`——swapbacked 刻意不在 AT_FREE 内即本项的反证锚），再走真 `folio_put()`；三份运行日志 `Bad page state` 命中 0 |
| ④ =n 折叠 | mva2-verify n-objects 门 | `CONFIG_CORTEN_MM_ARENA=n` 下 mm/rmap.o 等 14 对象 RC 0、零警告、`nm` 零 corten 符号；调用面全经 mm/corten_arena.h（=y 原型 / =n 桩同形） |

**首跑教训（已修）**：`node_page_state()` 读的是 fold-lagged 全局（`pgdat->vm_stat`），
wrapper 的 per-cpu 增量（`__mod_node_page_state`）要等 vmstat worker 折叠才可见——首跑
①的 ±1 断言因此落空。fixture 改用精确读法：全局 + 逐 online CPU 求和
`per_cpu_ptr(pgdat->per_cpu_nodestats, cpu)->vm_node_stat_diff[item]`（CONFIG_SMP 内；
!SMP 全局本身同步），同步确定性成立后 on1 全绿。

## 3. 红线核对

| 红线 | 实测 |
|---|---|
| INV6（本片零 PTE 写点零调用点） | diff 内 `set_pte/ptep_/pte_clear/mk_pte` 命中 0；`rmap_novma` 全树符号仅 rmap.c（4 定义）/corten_arena.h（8 原型+桩）/corten_arena_test.c（4 调用），corten_arena.c 零触碰 |
| 不改上游 rmap 既有函数 | diff −0 行，rmap.c 仅有 `#ifdef CONFIG_CORTEN_MM_ARENA` 新块 |
| INV9（=n/off 折叠） | =n 零符号（§2 判据④）；corten=off 三套件全绿（off1），锚不依赖运行时 gate |
| 零行为变化 | 纯加法 + 零调用点；on1/on2 与 mvb4 时代基线形态逐套件同构（corten 24/0/1、fault 31/0/2，skip 均为既有 by-design） |

## 4. 验证结果（无盘 qemu，mva2-verify.sh；日志 `/home/ppw/cortenmm/results/r07/w1a/`）

| 项 | 结果 |
|---|---|
| `make -j8` | 零新增警告（全量日志仅 objtool cpuidle + modpost memblock 两条，均在 m6t34-rev/mva1 基线日志中同在；rmap/corten 相关 0 条） |
| KUnit corten=on（on1） | `corten 24/0/1 · corten_arena 100/0/0 · corten_fault 31/0/2` 全绿；`ok 98 corten_arena_test_novma_rmap` |
| KUnit corten=on 复跑（on2，flake 判定） | 同上全绿，两连跑零 flake |
| KUnit corten=off（off1） | 全绿（corten_arena 24 pass/76 skip、fault 7/26——既有 mode-dependent kunit_skip 形态），锚 `ok 98` 亦绿 |
| =n 十四对象门 | RC 0、零警告、`nm` 零 corten 符号（含 mm/rmap.o） |
| checkpatch --strict | `total: 0 errors, 0 warnings, 0 checks, 287 lines checked` |
| free 路径日志 | 三份日志 `Bad page state` 命中 0 |
| bzImage | #133 已出（=y 一致态重建），guest 门留主会话（本片零调用点，标准门即可） |

## 5. 遗留与下游接口备注

- 消费点改道按 spec §3.1 落在后续片：R3/R4/R7 file 臂（W1.c）、R8 swap-out（W1.e）、
  R9/R1/R2/R5/R6 匿名族（W-2，复用本 wrapper，OQ-W1-6 的桶语义已在接口层解决）。
- anon wrapper 的 `folio->mapping && !folio_test_anon` 与 file wrapper 的
  `folio_test_anon` VM_WARN 是家族混用守卫（R-W1-4 跨域污染类），DEBUG_VM=y 构建下生效。
- 首跑暴露的 `node_page_state()` fold-lag 属测试面知识：后续 W1.c/W1.e 若需锚 lruvec
  stat，直接复用 `corten_arena_test_node_stat()`。
