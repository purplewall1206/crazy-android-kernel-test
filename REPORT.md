# CortenMM → Linux 6.18 移植 · 最终报告（M8）

- 版本: **v1.0 终稿**（2026-09-21 定稿；数据截止 = r07 通宵班: M4.T5 五轮固化 + G5 门 + M6 全量）
- 内核树状态: android17-6.18，HEAD `2639d3294b9d`（tag `corten-r07-m6t34`），**项目提交 25 个、tag 24 个**
- 唯一权威状态源: `STATE.md`（现场）/ `docs/ROADMAP.md`（任务级）；本报告为 M8.T3 交付物终稿，
  全部数字均有 `results/` 证据路径可复核；无未决 PENDING——未及执行项一律转入 §7/§8
  的「计划中」显式登记（G2/G6-二夜/G7 定量/G5-fix），不以"待补"含糊带过
- 口径符号: **BASE** = 同一 bzImage 上 `corten` 未激活（或无 LD_PRELOAD hook）的对照臂；
  **T0/MODE** = `corten=on` + MODE 进程（LD_PRELOAD hook）臂；"同 boot" = 同一内核一次开机内
  ABAB 交错对照（EVAL §2，docs/EVAL.md）

---

## 1. 执行摘要

### 1.1 项目使命

把 CortenMM (SOSP'25) 的核心思想移植进 Linux 6.18：消除内存管理热路径上的软件层抽象
（VMA 树 / maple tree / mmap_lock），改用「页表页锁协议（covering PT-page lock）+
per-PTE metadata array + 事务接口」。在 QEMU x86_64（8 vCPU/4G/KVM，i9-13900HX 宿主）
环境用论文同款测试证明：**干掉 VMA 后内核照常运行**，且 mmap-PF/PF/unmap 等热路径取得
可测提升；fork/lmbench 等全地址空间操作允许论文量级的回退。ARM64 侧完成移植性设计文档 +
交叉编译验证（STATE.md「使命」节；8 vCPU vs 论文 384 核 = 方向性验证，D4，不承诺复现
论文 26× 量级数字）。

### 1.2 当前达成（截至 2026-09-21 晨）

| 阶段 | 状态 | 一句话结论 |
|---|---|---|
| M0-M2 | ✓ | android17-6.18 基线 + M1 基线全矩阵落盘 + 内核骨架/协议 3 提交，KUnit 16/16（results/r01/） |
| M3 (M3a+M3b) | ✓ | DoD 4/4 PASS；头条主张实证：**perf 367K 样本 `find_vma`/`mmap_lock` 零命中**（results/r03/final-smoke/m3-verdict-final.md） |
| M4 | **✓ 完成** | T0 透明接管 + T1/T2 杂志批处理 + T1c 常驻池 + perf1 TLB 风暴修复全在树；**M4.T5 终判: G1 = MET（2/4）且五轮 ABAB 固化成立**（results/r07/g1-final.md）；G3 机制面成立（数字面 run4 未到 10% 线，§4.3）；G4 边缘项定案 JVM 噪声族（§4.3） |
| M5 | **✓ 完成** | T1a 忠实 fork + T1b/T2'（INV7/F2/F3/FOLL_FORCE/UNSHARE）+ T3 GUP 互操作全提交；T4 G5 门实测 **NOT MET（MODE 生命周期退出 RCU GP，机制级根因已证，§4.3-4）**，JThreadBench MODE 回归 rc=0 零 CFE（results/r07/g5-gate/） |
| M6 | **✓ 完成（T1-T4）** | rmap 守卫 + swap out/in 全事务 + shrinker 压力通道 + 观测面全提交；guest 判据: **swap roundtrip 69164=69164 账目闭合、shrinker 自然换出 RSS 281→3.8MB、三方口径一致**（results/r07/m6t2-verify.md、m6t34-verify.md） |
| M7 | 首轮完成，二轮计划中 | lockdep 首检零 splat（results/r07/m7-preheat.md）；syzkaller 17.8h / 1.42M exec **零内存安全 crash**（results/r07/syz-report.md）；第二夜 + KCSAN = 计划中（§7/§8） |
| M8 | **本报告 = v1.0 终稿** | G1 五轮固化 / G3/G4 复核定案 / G5 实测 / G6-G8 对账（§4、§7）；G2 perfetto 定量与 syz 第二夜转计划中 |
| M9 | P1 ✓ | **CortenMM KUnit 25/25 首次在 arm64 全绿** + 全量 Image（+196 KiB）（results/r06/m9-p1/m9-p1-verify.md）；P2/P3 计划中（§7-C4） |

### 1.3 核心数字亮点（除注明外，全部同 boot BASE vs T0 口径；G1 行 = 五轮固化终读数）

| 项 | 数字 | 出处 |
|---|---|---|
| **unmap-virt（虚拟 unmap，PTE-less 窗）低竞争 t4 / t8（五轮固化）** | **+1142% / +2583%**（20/20 配对腿全正号，t0 CV ≤2.0） | results/r07/g1-final.md §1 |
| **unmap（预映射区）低竞争 t4 / t8（五轮固化）** | **+45.4% / +142.0%**（10/10 配对腿正号） | 同上 |
| mmap 全组（MODE 空间分配，informational） | 固化读数 +141.3% / +251.4%（run4 家族 +119%~+278% 同形） | results/r07/g1-final.md §1 |
| dedup_eq tcmalloc 档（G3 机制面） | run3 +16.14 / run4 +6.33（三轮无重叠）/ 固化班复核 +19.17——方向恒正、幅度跨轮收窄如实登记 | results/r06/t5-run3-report.md §3；t5-run4-report.md §3；g1-final.md §4 |
| arena 生命周期税（T1c 池） | mmap+munmap 对 17.4µs → **1.3µs（13× 消除）**，probe 首次反超 legacy 4.1×；固化班池命中率 **99.999%** | results/r07/t1c-verify.md §0/§5.3；g1-final.md §0 |
| D15 GP-free drain | MODE munmap 单次 8554µs → **227.8µs**（62×）；dedup tcmalloc 从 -91.84% → +19% | results/r06/t5-overhead-diagnosis.md §3.1/§3.2 |
| perf 头条主张 | churn 4t 运行中 367K 样本，`find_vma\|mmap_lock` 六符号宽口径 **0 命中** | results/r03/final-smoke/m3-verdict-final.md DoD#4 |
| M6 swap/回收闭环 | **swapped_out 69164 = swapins 69164（账目精确闭合）**；shrinker 自然触发换出 RSS **281MB→3.8MB**；冻结点三方口径一致（memory.swap.current == smaps Swap == ledger，Δ<0.05%） | results/r07/m6t2-verify.md §3；m6t34-verify.md §3 |
| arm64 移植证据 | **KUnit 25/25 全绿**（-smp 4 复跑）+ CORTEN_MM=y 全量 Image 42,232,320 B（+196 KiB） | results/r06/m9-p1/m9-p1-verify.md §2/§3 |
| syzkaller 首轮 | **1,416,524 execs / 17.8h，4 crash 全部为环境类，0 内存安全类** | results/r07/syz-report.md §1/§3 |
| lockdep 首检 | PROVE_LOCKING 全家族构建 + KUnit/guest 压测 **零 splat** | results/r07/m7-preheat.md |
| KUnit 全套终态（HEAD 2639d3294b9d） | **24/0/1 + 48/0/0 + 30/0/2 = 102 pass + 3 设计 skip, 0 fail**（on ×2 + lockdep 变体同数零签名） | results/r07/m6t34-kunit-on3.log；m6t34-kunit-lockdep.log |

### 1.4 诚实边界（与亮点并存的负向/未决项，M8 终稿口径）

1. **G5 NOT MET（本报告新增的实测负结果）**: lat_proc MODE 臂 fork/fork+exec/shell =
   **+516%/+172%/+354%**（门限: fork 回退 ≤30%）。机制级根因已证实且**与忠实 fork 镜像无关**
   （零 arena fork = E1 no-op，fork_faithful 全程不变）: prctl ENTER 急切建 shrinker registry +
   fork 急切复制子 registry + exit_mmap 每 mm 遥拆付一次 `synchronize_rcu()`（完整 RCU 宽限期）。
   隔离实验: `/bin/true` 带 hook 生命周期 **+50ms/进程**（≈1 GP）。暴露面 = 进程派生密集的 MODE
   工作负载；长寿命进程（T5 全部 apps/JTB）一次性摊销不可见。修复方向已登记（惰性入册 /
   空 state kfree_rcu 快速退出，§7-A5），本班未实施（禁改内核树约束）。
   results/r07/g5-gate/g5-gate.md。
2. **mmap-pf（映射区 page-fault）残余回退**: 固化终读数 t4/t8 = **-17.33% / -22.29%**（10/10 负号，
   五轮一致，非方差）。perf1 已归因为机制成本地板：纯 fault 通道（每 fault 一事务：fill_upper +
   lock_range + xa_load + query + mark）≤11% + take/park 簿记 3-5%，进入 IPI/调度噪声地板，非风暴性
   回退（results/r06/perf1.md §4）。这是论文 Fig.5/8 per-fault transaction 语义本身的价格。
3. **G3 数字面 NOT MET（机制面成立）**: run4 dedup 双档 +4.60/+6.33 正向但未到 10% 线
   （run3 曾 +11.30/+16.14，跨轮幅度收窄如实登记，三轮无重叠非方差假象）；固化班独立 boot 复核
   +8.83/+19.17 方向恒正。五次加测未按原计划执行（run4 §6-3），以"三轮 run + 一轮独立 boot 复核、
   方向恒正、幅度不定"定案为机制解释口径（§4.3-2）。
4. **G4 边缘项定案（JVM 噪声族）**: jvm run4 -6.59% 单项超 5% 线 1.6pp；跨 boot 方向不定
   （run3 -2.51 / g1 班 +6.5 / 本班 JTB -7.1），分布重叠——定案为 spawn 噪声族观测项，无系统性
   回退证据（§4.3-3）。
5. **G1 判定史与口径演化**: 首跑 0/4（被 present-RO 缺陷族遮蔽）→ run2 1/4 → g1-consol
   加测 0/4 → **mmbench 静态链接口径缺陷（D1）使 run2/g1-consol 全部 mmbench 读数作废** →
   run3（动态口径）1/4 → perf1 TLB 风暴修复后 run4 = 2/4 → **五轮 ABAB 固化（t5final）= MET 2/4
   成立且更稳**（§4.2，results/r07/g1-final.md）。三口径并列呈现在 §4.1。
6. **JThreadBench ClassFormatError**: 已由 gupfix（0719bc6ae74e）修复，run3/run4/g1 班/G5 班
   四次独立再证未复现（0 命中），降级为观测项维持（results/r06/gupfix.md §3；results/r07/g5-gate/）。
7. **metis_eq fork 后段（OQ-D 残余）**: 忠实 fork 在树后 run3/run4/g1 班维持闭合（rc=0、
   checksum 8a8db990… 三方同值、fork_faithful 前进），观测项维持，无恶化（g1-final.md §4）。
8. 8 vCPU vs 论文 384 核：只做方向性验证；64 线程平台期、2270×/1489× 量级差距、384 核
   线性段不可测（docs/EVAL.md §3 不可测清单；G8 声明 = 本报告内嵌口径: §1.1、§4.1 三口径、
   §2.2 DEVIATION 清单、本条）。

---

## 2. 背景与设计

### 2.1 CortenMM 思想（一段话）

CortenMM 论文主张：把"页表页"作为锁与元数据的载体——每张 PT 页配一个描述符（rwlock +
per-PTE metadata array），所有对地址空间内容的读写（fault/map/mark/unmap/COW）都经
**事务接口**（covering 写锁下原子），metadata 是唯一真源（PS-B2），页表只是硬件缓存；
VMA 树与 mmap_lock 从热路径上消失。论文原型独立 OS；本项目验证该思想能否 **retrofit**
进成熟 Linux（6.18/android17）并保住兼容性（docs/PAPER_SPEC.md 为论文唯一权威解读；
docs/DESIGN.md 为移植决策记录）。

### 2.2 移植姿态与关键决策（D1-D19 精简表，全文见 STATE.md「关键决策记录」）

| # | 决策 | 要点 |
|---|---|---|
| D1 | opt-in arena | prctl/boot param 划定地址空间区段，非全 mm 替换；shadow-VMA 保其余子系统可用 |
| D2 | C 语言 + 工程化正确性 | Rust/形式验证不做；以 KUnit 不变量测试 + lockdep + KCSAN + syzkaller 逼近论文 strong correctness |
| D3 | 先 CortenMM_rw（rwlock 版） | 打通全链路后再评估 RCU+DFS 升级 |
| D4 | 8 vCPU 方向性验证 | 不承诺 384 核数字；REPORT 明说（本文 §1.4-7） |
| D5 | zram=lz4 | 对齐论文与 M0 DoD |
| D6 | mmbench 语义偏差声明 | mmap=成对计 1 op 等；M1/M8 同二进制同参 |
| D7 | 等价 workload | metis_eq/dedup_eq/psearchy_eq 多线程等价件；JVM 用真 openjdk-21 |
| D9 | corten=on 两阶段启用 | early param 静态 key 翻转挂死修复；默认 off、基线零扰动红线 |
| D11 | PAPER_SPEC 唯一权威 | metadata 唯一真源；**MODE-process 透明接管**（真实应用零改动进 arena）；fork 默认论文忠实（后被 D17 修正落地形态） |
| D12 | T0 硬门 | 上层接口不可改/已编译程序照常运行 = 零改动回归集；mremap 必须路由；M8 增加 LoC 终态账 |
| D13 | 锁序修订 | mmap_write > ctl_lock > drain-wait > desc->lock > ptl（结构性消灭反向边，DEV-13） |
| D15 | born-atomic percpu_ref | RELEASE drain 免 RCU GP；8554µs→227.8µs（results/r06/t5-overhead-diagnosis.md） |
| D16 | present-RO 缺陷族修法 | perm 跨"内容丢弃与 fork demote"携带（e219920b0923） |
| D17 | 忠实 fork 定型 = DEV-14 | copy_page_range 照常（wrprotect=glue 白名单第 1 处）+ fork_begin/commit 事务 meta 镜像；"不走 copy_page_range"作废 |
| D18 | fork 冻结窗 = DEV-15 | 移植自造语义（论文无对应），REPORT 披露清单项 |
| D19 | T1c 常驻 arena 池提案 | G1 翻盘杠杆 = arena 生命周期成本而非 VA 分配器（已实施，corten-r07-t1c） |

DEVIATION 披露清单（REPORT 义务，docs/DESIGN.md DEV 表）: DEV-1 xarray 代 boot 数组 /
DEV-3 qrwlock / DEV-6 glibc brk 留 legacy / DEV-11 fork_demote（已废止删除，fork_demotes
计数留遥测恒 0）/ DEV-12 per-mmap N auto-arena / DEV-13 锁序 / DEV-14 忠实 fork 形态 /
DEV-15 fork 冻结窗 / MODE EXIT 全退场语义 / **glue 白名单 ≤3 已用 1**（copy_page_range
wrprotect，STATE D17）。

### 2.3 架构现状（组件与提交对应）

```
用户态
  │ prctl(PR_CORTEN_MODE=80) 或 PR_CORTEN_ARENA=79 显式划段
  ▼
┌────────────────────────── MODE 进程 ──────────────────────────┐
│ addr=0 匿名 mmap 四入口路由 (T0a/T0b: do_mmap/munmap/mprotect/ │
│ madvise/mremap) ── 显式地址/brk/文件映射留 legacy (DEV-6)      │
│                                                                │
│  auto-arena (DEV-12): 每 mmap 建 arena → T1c 常驻池 park/      │
│  reactivate（99.9% 命中，zero churn）                          │
│  VA 发放: per-cpu 2M-frame 杂志 (T1) + sentinel 障碍           │
│  shadow-VMA (VM_CORTEN bit43) + punch 打洞 (D-G'': CDS 兼容)   │
│                                                                │
│  事务引擎: corten_lock_range/query/map/mark/unmap              │
│    - covering PT-page 描述符锁 (BH 对称, M3a)                  │
│    - per-PTE metadata array = 唯一真源 (PS-B2)                 │
│    - born-atomic ref (D15), lazy gather/park flush (perf1)     │
│                                                                │
│  fault 双钩子: do_user_addr_fault 快门 + handle_mm_fault 慢门  │
│    - FRESH 分派 (defect A) / KEEP_PERM perm 门 (D16)           │
│    - GUP/access_error 门让位 metadata (gupfix)                 │
│                                                                │
│  fork (M5.T1a 忠实): 冻结窗 (fork_begin) → copy_page_range     │
│  (wrprotect=白名单#1) → fork_commit meta 镜像 (SHARED+COW)     │
│  T1b/T2' (已提交 1f8dfc78ae9f): INV7 checker / F2 子侧 pmd 门 / │
│  FOLL_FORCE/UNSHARE 事务化 / OQ-4/5 裁决                        │
└────────────────────────────────────────────────────────────────┘
corten=off / 非 MODE 进程: 零感知（=n 编译折叠 + static branch + 等价测试三重保障）
```

### 2.4 代码量现状与 LoC 终态账（D12②）

主树 corten 文件合计 **19,229 行**（mm/corten.c 1750 / mm/corten_arena.c 8877 /
mm/corten_test.c 2113 / mm/corten_arena_test.c 5298 / include/linux/corten.h 616 /
include/linux/corten_arena.h 575；HEAD `2639d3294b9d` 实测 `wc -l`，2026-09-21）。

**终态账**（"若 arena 成默认，VMA 热路径可删代码量" vs "corten 增量"，同 HEAD 实测）:

| 侧 | 文件 | 行数 |
|---|---|---|
| VMA 树/查找面（可删上限口径） | mm/mmap.c + mm/vma.c + lib/maple_tree.c | 2,086 + 3,366 + 7,268 = **12,720** |
| corten 生产代码 | corten.c + corten_arena.c + 两头文件 | **11,818** |
| corten 测试代码（KUnit 不变量资产） | corten_test.c + corten_arena_test.c | 7,411 |

诚实结论: **代码量维度本项目为净增**（生产代码 ≈ 可删上限的 93%，含测试净增 51%）——
且"可删上限"本身高估: file/SHLIB/HUGETLB/migrate 等路径的 VMA 树在任何形态下都必须保留
（D1 opt-in 架构下 shadow-VMA 与 legacy VMA 长期共存）。本移植的价值主张在**机制与性能**
（VMA/mmap_lock 热路径消失 = M3 头条主张；unmap 族 +45%~+2583%），不在代码量节省——
这是论文原型（独立 OS）与 retrofit 路线的结构性差异，如实声明。

---

## 3. 分里程碑进展

> 状态/证据/关键数字/遗留四元组。任务级细节在 docs/ROADMAP.md；夜叙事在 log/2026091*-*.md。

### M0 环境完备 — ✓（r01 夜）
- 证据: results/r01/{boot-m0-console.log, m0-dod-*.txt, config-android17-m0}；tag `corten-r01-m0`。
- 数字: android17-6.18（HEAD 68974e235117）构建 436s 零错误；VM 启动 65s（DoD ≤180s）；
  zram lz4 2G / MGLRU enabled=0x7 / 无 THP（4K 页基线）。
- 遗留: 无。

### M1 基线测量 — ✓（r01 夜）
- 证据: publish/baseline/（= results/r01/baseline/ 镜像；baseline_meta.json 全口径）。
- 关键数字（3 中位）: lat_proc fork/fork+exec/shell = 1298/2969/9874 µs；JVM 2000 线程
  2347 ms；metis_eq 15.77 s；psearchy_eq 4.01 s；dedup_eq glibc 4.80M、tcmalloc 5.16M
  blocks/s（log/20260913-r01.md §2）。
- 机制级发现（对 M8 关键）: 6.18 的 CONFIG_PER_VMA_LOCK 使缺页几乎不碰 mmap_lock
  （30s PF trace: 50 个 mmap_lock 事件 vs 1,013,798 次 page_fault_user）→ PF 系与
  mmap/unmap 系必须分开解读。
- 遗留: mitigations=off 基线补测组由 T5 各跑 in-boot 承担（PS-F2 对齐，两臂同加）。

### M2 内核骨架 — ✓（r01 夜）
- 提交: b8386e4e2467（m2a 骨架）/ 2fd4070e745c（m2b 覆盖页锁协议+事务 API）/
  1284a235f751（fix1: D9 启动修复）；tag corten-r01-m2{a,b,c-fix1}。
- 证据: results/r01/{m2-dod-off.md, m2-dod-on.md, m2fix-report.md, m2b-kunit-boot-*.log}。
- 数字: 3739 行；KUnit 16/16 双验证；=n 编译零扰动。
- 遗留: 无。

### M3 协议加固 + arena fault 绕 VMA — ✓（r02-r04，M3 PASS）
- M3a.F1: e911b31adb9c（desc->lock 全链 BH 对称，修 khugepaged RCU_SOFTIRQ 同 CPU 死锁）；
  证据 results/r02/m3a-f1-report.md（KUnit 25、lockdep 构建三连绿）。
- M3b.S1-S3: d040b61051af（prctl=79 + per-mm 2M-frame xarray + shadow-VMA）；
  证据 results/r02/s123-verify.md。
- M3b.S4-S8: 7bba3b9f7390（fault 双钩子+fill_upper+空间路由）+ 96466df24387（DoD 三缺陷
  修复 A/B/C，各带 KUnit 锚）+ ae236ee077ac（debugfs arenas/arena_stats）；证据
  results/r02/s46-verify.md、results/r03/。
- **M3 DoD 4/4 PASS**（results/r03/final-smoke/m3-verdict-final.md）: 压测 5+1 配置零
  panic 零错误（touch 4t 12.8 亿 ops 校验和正确）；maps 恰一条 shadow-VMA；kselftests
  fail-set 与基线一致；perf 367K 样本 VMA/mmap_lock 符号零命中。guest KUnit on: 24+9+13。
- 首轮 FAIL→修复记录（诚实链）: results/r03/m3-dod-verdict.md（A 新鲜页 MAPERR /
  B RELEASE drain 挂死 / C TLB range 缺失，三轮全部带 KUnit 回归锚修复）。
- 遗留: interlock 用例时序 flake（首跑容忍/复跑判定清单，M7 口径）；va_high_addr_switch
  基线既有 fail（非 corten）。

### M4 事务化空间操作 — **✓ 完成（T5 终判: G1 = MET 2/4，五轮固化）**
- **M4.T0 MODE 透明接管**: 5aef23c4aee9（T0a 锁序重构+auto-attach+fork demote）+
  9d74b22a1348（T0b mprotect/madvise/mremap 路由+计数器）；证据 results/r04/t0a-verify.md、
  results/r04/t0b-verify.md。T0 零改动 DoD 终态 7P/3F/2S，两条残余（D-G'' CDS 打洞、
  DEV-11 fork 边界）后继全部闭环（见 r06）。
- **D-G'' shadow-VMA 打洞**: 452ad7b9d91e（CDS file-MAP_FIXED 绕守卫打洞 + P1 vma-lock
  泄漏修复）；根因 results/r05/dg2-analysis.md，验证 results/r06/dg2-verify.md、
  publish/dg2-verify.md。判定更新: MODE 下 `java -version` rc=0、dedup/metis/psearchy
  全 rc=0（此前 3/3 rc=139，log/20260918-r06.md §2）。
- **M4.T1/T2 per-cpu VA 杂志 + munmap 批处理**: de8a685370bb；证据 results/r07/m4t12-verify.md。
  机制生效: va_recycles 25805≈99% 发放走回收 / 仪式税 49.5→17.4µs / probe CHUNK 形态
  MODE 3.0× 快于 legacy；同跑诊断 D1（mmbench 静态链接，G1 历史读数作废）+ D19 T1c 提案。
- **T1c 常驻 arena 池**: 87383f51a3ff；证据 results/r07/t1c-verify.md。池命中 99.93%
  （battery 窗 parks +107,964 / hits +107,886）、mpl 17.4→1.3µs（13×）、dedup tcmalloc
  独立窗 6.13M blocks/s（超 D15 记录 5.94M）。
- **perf1 TLB 风暴修复**: 802ff7551bd0；证据 results/r06/perf1.md。三优化（lazy
  per-window gather / park 元数据整块释放 / park flush 后置 downgrade）把 G1 三格从大负
  翻正（§4.4-1）。
- **M4.T5 正式判定（终态）**: run4 首过（802ff7551bd0，G1 2/4）→ **五轮 ABAB 加测固化
  （t5final, HEAD 2639d3294b9d, results/r07/g1-final.md）: G1 = MET（2/4）成立**——
  unmap +45.38/+141.99、unmap-virt +1142.34/+2583.03 成对过线，20/20 配对腿无一带正号例外；
  固化班有效性与遥测断言全过（109/109 腿 rc=0、池命中 99.999%、hookassert 0/54、
  seed 配对 50/50、dmesg 零 warn）。apps 复核: dedup 双档 +8.83/+19.17、JTB 双臂 rc=0。
- 遗留: G3 数字面与 G4 边缘项定案见 §4.3；mmap-pf 残差 = 机制成本地板登记（perf1 §4）。

### M5 fork/COW/GUP — **✓ 完成（T1a/T1b+T2'/T3 提交；T4 G5 实测 = 本报告 §4.3-4）**
- **M5.T1a 忠实 fork**: 68697442097e（tag corten-r07-m5t1a；冻结窗 + copy_page_range 照常 +
  fork_commit 事务 meta 镜像 + COW 核心 + fork_demote 废止）；证据 results/r06/m5t1a-final-verify.md
  （终态补跑全绿）+ results/r06/m5t1a/。登记 D17（DEV-14）/D18（DEV-15）。
- 关键数字: fork_roundtrip **1000 轮 PASS**（checksum 076534f9ba241483 稳定，逐 100 轮
  MemFree 平稳 = COW-reuse 泄漏修法闭合）；fork_isolation 双臂 checksum
  e1c81840e4123903 跨臂等价；metis_eq MODE 全量三方同值 8a8db99075665220 + fork-probe OK
  （OQ-D 闭环落终态代码）；对账 fork_faithful=1005、fork_demotes=0。
- **M5.T1b + T2'**: 1f8dfc78ae9f（tag corten-r07-m5t1b；INV7 checker / F2 子侧 pmd 门 /
  F3 drain 注入 / FOLL_FORCE 与 UNSHARE 事务化 / OQ-4/5 按上游代码裁决关闭 / Fig.8 L26-38
  对拍锚 / cow_collision 压测）；5 文件 +1127/-13。验证全矩阵全绿（KUnit on 89 用例 0 fail
  普通+lockdep 双口径、off 全绿、lockdep 变体零 splat 下 roundtrip 1000 轮 + mtlock 1000
  fork×8 线程、ksmoke 9/9、cow_collision 双臂 200 轮零互串）；
  证据 results/r07/m5t1b-verify.md + results/r07/m5t1b-*/。残余三项登记（窗粒度 DONTCOPY
  过度 SHARED 良性自愈 / FORCE 边界 perm-RO+VM_WRITE EIO 响亮拒绝 / fork_battery ksmoke
  --kdir 参数），见 results/r07/m5t1b-verify.md §3。
- **M5.T3 GUP 互操作**: 5c545359e856（tag corten-r07-m5t3）；证据 results/r07/m5t3-verify.md
  + results/r07/m5t3-gupmat-results.txt。fast 路径零改动证实（arena PTE 对 gup_fast 透明，
  guest 四形态全对）；slow 路径挖出并修复 T2' 遗留两真 bug（FOLL_FORCE 重定向 × 普通 GUP 写
  pin = 自旋死循环 → force 语义整体移除；UNSHARE→write 映射 × read-pin 误拒 → ACCERR 落回
  legacy unshare）+ `zap_pinned` 计数 + 5 个 KUnit 矩阵锚；KUnit 终态 24/0/1 + 43/0/0 +
  26/0/0 全绿（普通 + lockdep 双口径）。
- **M5.T4（G5 门）**: lat_proc MODE 臂实测 = **NOT MET（fork +516%/exec +172%/shell +354%），
  根因 = MODE 生命周期退出 RCU GP（急切 registry + 每-mm synchronize_rcu），与忠实 fork 镜像
  无关**；隔离实验 +50ms/进程生命周期；JThreadBench MODE 回归 rc=0 零 CFE。
  全文 results/r07/g5-gate/g5-gate.md + 本报告 §4.3-4；修复方向登记 §7-A5。
- 遗留: G5-fix 小 diff 切片（§7-A5，计划中）；m5t1b §3 三项残余维持登记。

### M6 rmap/回收/swap — **✓ 完成（T1/T2/T3+T4 全提交，guest 判据闭合）**
- **M6.T1 回收守卫**: 0e469cd9d055（tag corten-r07-m6t1）。V1（oom_reaper 裸写 arena PTE）=
  逐 VMA skip；V2（ttu walker 裸写）= 守卫拒绝 + `rmap_rejects` 计数（完整 swap-out 事务留 T2）。
  KUnit 24/0/1 + 43/0/0 + 28/0/0 全绿（真 try_to_unmap 端到端锚 + reaper 谓词注入锚）；
  guest OOM 钉桩击杀 audit=0 零撕裂；重要环境事实: freezer+OOM_REAPER_DELAY 使 reaper 在本
  负载形态从未进入 VMA 循环（守卫对所有形态零风险）。证据 results/r07/m6t1-verify.md。
- **M6.T2 swap out/in 全事务**: b51754002f2f（tag corten-r07-m6t2；8 文件 +1817/-93）。
  swap-out = try_to_unmap_one 的 mmu_notifier 窗内事务（镜像上游 swap-install 链 +
  corten_swap_out 元数据；glue 白名单第 2/3 处: ttu 换出 + unuse_pte）；swap-in =
  自持锁循环 + SWP_SYNCHRONOUS_IO 直读（zram 同步设备），拒绝"legacy 放行 + 事后同步"
  （PTE-none 形状静默数据丢失论证）；不进 LRU（DEV-10）。
  **Guest 判据: swapped_out=69164 = swapins=69164 账目精确闭合（+ zap_swap_frees=0）、
  RSS 276MB→1.8MB、读回 checksum/errors=0、退出零泄漏 zram used 回基线、OOM 回归 audit=0、
  非 arena 进程 swap 行为不变**（results/r07/m6t2-verify.md §3 + m6t2-guest/）。
  KUnit 24/0/1 + 44/0/0 + 30/0/2 全绿（lockdep 变体同数零签名）。
- **M6.T3+T4 shrinker 压力通道 + 观测面**: 2639d3294b9d（tag corten-r07-m6t34 = HEAD；
  5 文件 +1563/-126；顺带修 M5.T3 一行 folio_put 遗留）。per-mm RCU registry + 精确常驻计数
  （4 个状态迁移收口点维护 nr_mapped/nr_swapped）+ 两遍 young 位 aging 事务（ptep_clear_young_notify
  镜像 vmscan）+ 批量 victim 拾取（T2 的 ~1.4-6ms/页 → 82-350µs/页全含）+ memcg 感知注册。
  T4 观测: `shrink_scans/aging_passes/shrink_swapped/resident_pages/swapped_pages/
  swapout_rate/swapin_rate` 新行。
  **Guest 判据: shrinker 自然触发换出（无 debugfs 触发，memory.max 压窗）5s 内 945 次
  shrink_scans/59,356 页换出，RSS 281MB→3.8MB；读回 verdict PASS errors=0；
  swapins 69,356 = swapped_out 69,356 闭合；冻结点三方口径一致
  （memory.swap.current 69,388p == smaps Swap 69,388p == ledger 69,356p，Δ32 页 < 容差）**；
  批量 evict 10k 页暖态 822ms（12,150 页/s）。KUnit 终态 **24/0/1 + 48/0/0 + 30/0/2 = 102
  pass + 3 设计 skip, 0 fail**（on ×2 + lockdep 变体同数零签名）。
  证据 results/r07/m6t34-verify.md + m6t34-guest/ + m6t34-kunit-*.log。
- 遗留: G7 定量测量未执行（本片仅有三方口径一致性核对 + meta_bytes 1.37MB/335 阵列快照），
  登记 §7-C/§8 计划中；批量换出天花板 = zram 同步写逐页（上游 page_io 形态，非 corten 缺陷）。

### M7 稳定性 — **首轮完成；第二夜/KCSAN = 计划中**
- lockdep 首检（M7 预热）: PROVE_LOCKING 构建 + KUnit + guest 压测零 splat；interlock
  用例在 lockdep 开销下 1 次时序 flake 复跑绿（M7 清单）；证据 results/r07/m7-preheat.md、
  results/r07/{lockdep-build.log, lockdep-kunit.log, lockdep-kunit-run2.log}。
  后继每切片 lockdep 变体 KUnit 全绿零签名（m5t1b/m5t3/m6t1/m6t2/m6t34 各 verify §2）。
- syzkaller 首轮挂机: 17.8h / 1,416,524 execs / corpus 748 / coverage 16,875 / 4 crash
  全部环境类（2×RCU 饥饿 + 2×VM 无输出，repro reliability=0.00）、**零内存安全类**；
  证据 results/r07/syz-report.md、results/r07/{m7-preheat-syz.md, syz-manager.log,
  kcov-build.log}。corpus 748 programs 已保留作下轮种子（同上 §5）。
- interlock flake 登记: `corten_test_txn_uninstall_interlock` / `txn_mutex_disjoint`
  历史时序 flake 各 1-3 例，基线内核可复现（同签名同频），复跑全绿——M7 口径 =
  "首跑容忍/复跑判定"（results/r07/m6t2-verify.md §2: 同轮基线内核 4 跑亦 1 次同签名，
  机理 = host vCPU 饥饿，协议代码无交集）。
- 遗留（计划中）: KCSAN 未跑；syz "连续 2 夜无可复现 crash" 的 G6 完整口径需第二夜
  （corpus 种子已备）；B4（zap 路径 add_mm_counter-under-ptl preempt_nested WARN）裁定
  在 lockdep corten=on 复跑全套件时执行。

### M8 终测报告 — **本报告 = v1.0 终稿（2026-09-21）**
- M8.T1（G1 五轮加测固化）✓ = results/r07/g1-final.md（§4.2）；M5.T4（G5 门）✓ =
  results/r07/g5-gate/（§4.3-4）；M8.T2（perfetto G2 定量）= **计划中**（§8）；M8.T3 = 本报告。
- G1-G8 终态对账: **G1 MET 2/4（五轮固化）/ G2 计划中（机制地板定性已引用 perf1 §4）/
  G3 数字面 NOT MET·机制面成立 / G4 边缘项定案 JVM 噪声族 / G5 NOT MET（生命周期 GP 根因）/
  G6 部分（syz 1 夜 + lockdep ✓ + KUnit ✓；二夜口径与 KCSAN 计划中）/ G7 定性（机制成本地板 +
  M6 三方口径核对；定量计划中）/ G8 诚实性声明内嵌（§1.1/§1.4/§2.2/§2.4/§4.1）**。

### M9 ARM64 — P1 ✓ PASS
- 证据: results/r06/m9-p1/m9-p1-verify.md；提交 025756094542（tag corten-r06-m9p1）。
- 数字: mm/corten.o + corten_test.o arm64 双绿（r02 Round B 首证）；全量 Image
  42,232,320 B（基线 40.1MiB + 196 KiB）；**corten 套件 25 用例 arm64 首次全绿**
  （单 CPU 21 pass+4 设计 skip，-smp 4 复跑 25/0/0）。
- 遗留: P2/P3（热钩子接线 / contpte-fold 与事务共存 OQ1/P3 决议）未做；
  gupfix 的 access_error 等价门 arm64 侧需同型处理（results/r06/gupfix.md §5-3）。

---

## 4. 性能评测

### 4.1 T5 协议与三口径声明（口径演化，诚实呈现）

协议: bench/t5/README.md（docs/EVAL.md §1-3 权威）——ABAB 交错、每配置 ≥3 中位、CV>5%
标注/加测、M1 参数锚（publish/baseline/baseline_meta.json）、330 项参数审计、55 对 strace
multiset diff、dmesg 零 warn 门。环境: trixie 8 vCPU/4G/KVM，`corten=on mitigations=off
kunit.enable=0`。

**三个口径并列（M4.T5 谱系）**:
1. **判据口径（G1/G4 采用）**: 同 boot BASE vs T0（in-boot ABAB，消除构建/boot 间漂移）。
2. **读法 A（机制进步口径）**: MODE@新版 vs MODE@上版（如 run3 的 MODE 对比
   de8a685370bb 的 MODE，results/r06/t5-run3-report.md §2.2）——衡量机制演进，
   不构成 G1 判据。
3. **作废口径（存档警示）**: 首跑（results/r06/t5/t5-r06-report.md）的三 app rc=139
   由 present-RO 缺陷族遮蔽、dedup -91.84% 与 JVM >480s 由 D15 前内核产生；run2 的
   mmbench 臂因 **mmbench 为静态链接、LD_PRELOAD hook 从未进入其 mm**（D1 口径更正，
   证据链: ldd / 单 marker / auto_mmaps +160 vs 动态重编 +19365）全部作废——
   g1-consol 的 5 轮加测（results/r06/g1-consolidation.md）同样基于静态二进制，其
   "+7% 稳定小效应"判 legacy-vs-legacy 慢漂移，一并作废（results/r07/m4t12-verify.md
   §1-D1）。**apps 臂（dedup/metis/psearchy/JVM，动态链接，hook 双 marker 实证）不受
   D1 影响，G3/G4 结论维持**。run3 起全部 mmbench 腿改用动态 mmbench_dyn
   （sha256 38304f06…，池计数 +13.7M parks 实证 hook 进 MODE）。

G1 判定史一览（低竞争 t∈{4,8} ≥2 项同时 ≥10%）: 首跑 0/4（遮蔽）→ run2 1/4（口径作废）→
g1-consol 0/4（口径作废）→ run3 1/4（动态口径，读法 A 3/4）→ run4 2/4 → **五轮 ABAB 固化
（t5final）2/4 成立 = 终判 MET**（results/r07/g1-final.md）。

### 4.2 M4.T5 判定（run4 → 五轮固化终读数; run4 全文转引 results/r06/t5-run4-report.md §1）

内核 = 主树 HEAD 802ff7551bd0 直编（bzimg/r06-t5d，sha256 6d3d616d…ee124e8，四配置
olddefconfig=No change 核对）；动态 mmbench 口径（池 parks +10,890,032 / hits 99.994% 实证）；
330/330 参数一致；dmesg/console 双零。

| Gate | 判定 | 依据（run4 = 本跑动态口径, T0 vs BASE 同 boot） |
|---|---|---|
| **G1**（低竞争 t4/t8 ≥2 项 ≥10%） | **MET（2/4）—— T5 谱系首次** | **unmap 成对过线（t4 +33.51 CV 10.6/2.9 / t8 +143.48 CV 2.2/2.2）+ unmap-virt 成对大胜（t4 +1156.11 CV 7.3/3.7 / t8 +2791.86 CV 3.5/3.3）**; mmap-pf -8.76/-17.09 = perf1 登记残差; pf +37.04/-9.65（登记高方差族, CV 26~59） |
| **G3**（app ≥1 项 ≥10% 或机制解释） | **NOT MET（数字面）** | dedup_eq t8/glibc **+4.60%**（CV 1.04/1.13, 三 run 无重叠）+ dedup_eq t8/tcmalloc **+6.33%**（CV 0.16/2.69, 三 run 无重叠）= 双档正向但均未到 10% 线; metis -0.88 / jvm -6.59（辅助项）。机制解释: run3 的 dedup 增益定价于 T1c 池生命周期税消除; perf1 三优化作用在 TLB gather/park 路径（mmbench 形状）, 对 dedup churn 增量有限——本轮维持正向=**无回退**, 幅度收窄如实登记 |
| **G4**（非 MM 回退 ≤5%） | **边缘未过（1 项超线 1.6pp）** | metis_eq -0.88 ✓ / psearchy_eq +3.18 ✓ / dedup 双档 +4.60/+6.33 正向 ✓; **jvm -6.59% 超 5% 线**: 三 run 方向一致（逐 run +1.7/+6.9/+1.6%）但分布重叠, run3 同口径 -2.51, JVM spawn 噪声族登记为观测项 |
| 参数一致性 | **PASS 330/330** | PARAM-MISMATCH=0, 覆盖缺口 0 |
| strace 机制语义 | **干净** | 55 对中 34 differs: 仅 EAGAIN/ETIMEDOUT 计数抖动 + jvm ESRCH×1; **零 EACCES/EFAULT/ENOMEM/EPERM 新类** |
| dmesg | 干净 | corten warn/bug/oops = 0 全程 |

**G1 五轮加测固化（终判定, 2026-09-21, results/r07/g1-final.md）**: 内核 = 主树 HEAD
`2639d3294b9d`（tag corten-r07-m6t34）直编 #90（bzimg/r07-t5final, sha256 `97e90f34…f68f78`,
`make olddefconfig` = No change 四配置核对, 零新增告警）。10 格全跑（5 bench × low × {t4,t8}）,
每格 5 轮 ABAB（seed = `20260913 + bidx*1009 + cidx*97 + t*7 + k`, 同 seed 双臂配对）。
有效性断言全过: 动态 mmbench 同件 `38304f06…`、T0 臂 STRICT marker 54/54、debugfs 遥测
（pool_parks +3,140,704 / hits 99.999% / munmap_releases 精确对应 / meta_arrays==ptdescs 零泄漏 /
drain_timeout 0）、seed 配对 50/50、109/109 腿 rc=0、dmesg 零 warn。

| 项 | t4 Δ%（5 轮中位, CV b/t0） | t8 Δ% | 同时 ≥10%? | 逐轮符号 |
|---|---|---|---|---|
| mmap-pf | -17.33（4.1/5.2） | -22.29（2.9/7.6） | 否 | 10/10 负 = fault 通道机制成本地板（登记口径） |
| pf | -7.18（6.6/17.9） | -3.82（7.0/4.1） | 否 | 高方差族, 围绕 0 |
| **unmap** | **+45.38**（28.5/5.0, base 1 轮离群被中位吸收） | **+141.99**（4.8/5.5） | **YES** | 10/10 正 |
| **unmap-virt** | **+1142.34**（5.0/1.6） | **+2583.03**（1.4/2.0） | **YES** | 10/10 正 |
| mmap（informational） | +141.34 | +251.42 | — | 10/10 正 |

**G1 终判 = MET（2/4）**: 全部 20 个 unmap/unmap-virt 配对腿无一带正号例外（t0 侧 CV ≤5.5%）;
unmap-virt t0 绝对值跨线程规模稳定 ~0.65 ops/µs（MODE CHUNK 快路径不塌）, base 侧 t8 腰斩
即差距来源。mmap-pf 残差深端 -17/-22 与 perf1 网格（-14.3/-15.5）、run4（-8.76/-17.09）同族
演进——机制成本地板登记口径不变（本负载 shrink_scans=0, M6 路径零触碰, 非 M6 新回退）。

### 4.3 G1-G4 逐 gate 分析

**G1（热路径提升）— MET 2/4，与残差并存**。run4 vs run3 同口径对照
（results/r06/t5-run4-report.md §2）:

| bench | run3 t4/t8 Δ% | run4 t4/t8 Δ% | 同时≥10% |
|---|---|---|---|
| mmap-pf | -61.64 / -74.24 | **-8.76 / -17.09** | 否（收敛至机制成本地板） |
| pf | -63.71 / +29.27 | +37.04 / -9.65（CV 26~59 高方差族） | 否 |
| unmap | +23.22 / +113.30 | **+33.51 / +143.48** | **YES** |
| unmap-virt | -83.83 / -74.46 | **+1156.11 / +2791.86** | **YES** |

- unmap-virt 全 16 格全大正（high t16 +7034%），两臂 CV ≤12.7——TLB 风暴修复直接定价。
- unmap 族除 high/t2（-20.08，唯一负格如实登记）外全正。
- mmap 全组 +119~+278%（T1c 池生命周期税消除维持，run3 时代同形）。
- 读法 A 参考（机制演进，非判据）: T1c 使 MODE 四格对上版 MODE 3/4 ≥+10%（unmap +41.5% /
  mmap-pf +23.5% / unmap-virt t8 +15.2%，results/r07/t1c-verify.md §5.4）；
  perf1 再把 unmap-virt low t4 对比 run3 拉开 13× 形态逆转（results/r06/perf1.md §0）。

**G3（真实应用）— 数字面 NOT MET；机制面成立（终案）**。谱系: run1 无有效 T0 数字
（present-RO 遮蔽）→ run2 tcmalloc +11.32% → run3 双档 +11.30/+16.14（G3 MET）→ run4
双档 +4.60/+6.33（正向无重叠但未到线）→ **固化班独立 boot 复核（g1-final §4, ×1/臂）:
glibc +8.83 / tcmalloc +19.17**。四个独立 boot 全部正向、无一回退; 幅度不定的机制解释被 T5
判定采信: dedup 增益定价于 T1c 池生命周期税消除（run3 已入），perf1 三优化作用在 mmbench
形状的 TLB gather/park 路径，对 dedup churn 增量有限; 跨 boot 绝对值漂移（固化班 dedup
tcmalloc base 3.843M vs T1c 班 5.609M）按口径一律以 in-boot BASE vs T0 为准。
**定案声明: run4 §6-3 原计划的"dedup 双档 5 次加测"未执行**（夜窗产能让位于 G1 固化与 G5）;
G3 终案 = 「四 boot 方向恒正 + 幅度不定 + 机制归因」口径，10% 线未稳定跨越如实登记。
metis/psearchy/JTB 3 rep rc=0（run4）+ 固化班 rc=0。

**G4（非 MM 回退 ≤5%）— 边缘项定案（JVM spawn 噪声族）**。run4 终读数: metis -0.88 ✓ /
psearchy +3.18 ✓ / dedup 双档 +4.60/+6.33 正向 ✓; **jvm -6.59% 单项超线 1.6pp**。定案依据
（跨 boot 方向不定性）: run4 三 rep +1.7/+6.9/+1.6、run3 同口径 -2.51、g1 固化班 JTB t0 反向
快 +6.5%、G5 班 JTB t0 慢 -7.1%（宿主 4 VM 并行负载不同）——**无系统性回退证据, 定案为
spawn 噪声族观测项维持**。历史: run2 的 psearchy -40.05% 已由 g1-consol 5 轮 ABAB 定案为
暖机方差（排除暖机后中位 -0.07%，apps 臂不受 D1 影响，裁决有效，
results/r06/g1-consolidation.md §4）; 固化班 metis +35.92% 判冷缓存伪影剔除（两臂绝对值
都受冷缓存拖累, 先后顺序即方向; 有效读法 = rc=0 + checksum 8a8db990… 双臂同值）。

**G5（枚举代价 lat_proc）— 实测 NOT MET（M5.T4, 2026-09-21, results/r07/g5-gate/g5-gate.md）**。
M1 基线分母 1298/2969/9874 µs 在档（publish/baseline/）；MODE 臂于 HEAD 2639d3294b9d
（忠实 fork 在树）首次实测:

| op | BASE 中位 µs | MODE 中位 µs | Δ | 门限 ≤30% |
|---|---|---|---|---|
| fork | 1696.9 | 10454.1 | **+516.0%** | **NOT MET** |
| fork+exec | 4932.1 | 13402.2 | **+171.7%** | NOT MET |
| shell | 14261.5 | 64679.8 | **+353.5%** | NOT MET |

- **根因（机制级证实）与忠实 fork 无关**: 零 arena fork = E1 no-op（fork_begin 对无 state
  父 early-return; 全程 fork_faithful 34→34、fork_demotes=0、munmap_releases/pool 计数零前进）。
  回退全部来自 **MODE 生命周期成本**: prctl ENTER 急切建 shrinker registry（corten_arena.c
  `corten_arena_mode_enter`）→ fork 急切复制子 registry（fork_commit 预备）→
  exit_mmap 每 mm 遥拆付 `list_del_rcu + synchronize_rcu()`（完整 RCU 宽限期）。
- **隔离实验**: `/bin/true` ×20 带 hook（仅 ENTER+退出）1.135s vs plain 0.135s =
  **+50ms/进程生命周期**（当刻宿主 4 VM 并行, GP 偏慢; lat_proc 窗口折算 ≈ +8.8ms/次）。
  三 op 差值（+8.8ms/+8.5ms/+50ms）由每迭代进程退出次数 1/2/多次线性解释。
- **暴露面定性**: 进程派生密集的 MODE 工作负载（lat_proc 恰是）；长寿命进程一次性摊销不可见
  ——与 T5 全部 apps/JTB 无回退自洽。run1（hook atexit fork-probe 被子进程继承 + 9p stderr
  同步写污染）判废存档 raw/run1/，run2 干净口径（NOPROBE hook + /tmp 对称重定向）为正式数字。
- **JThreadBench MODE 回归（同 boot ×1/臂）**: 双臂 rc=0、CFE 0 命中（gupfix 零回归第四次
  独立再证: run3/run4/g1/G5 班, §1.4-6）; 幅度 -7.1% 落 JVM 噪声族（30 分钟前同 VM 反向 +6.5%）。
- **处置**: 修复方向登记 §7-A5（惰性入册 / 空 state kfree_rcu 快速退出, 小 diff）；本班禁改
  内核树未实施。**G5 判定如实记 NOT MET**, 不以机制解释折抵门限。

**G2（竞争消退 perfetto 定量）— 计划中（未执行）**: M8.T2 的
mmap_lock_contention/fault_latency/sched_breakdown SQL 对照未跑（夜窗产能让位）。已有的定性
替代: ①M3 头条主张（perf 367K 样本 VMA/mmap_lock 零命中, results/r03/final-smoke/）为竞争面
最强证据; ②M1 基线 PER_VMA_LOCK 发现（30s PF trace 50 mmap_lock 事件 vs 1,013,798 PF）仍
成立; ③perf1 §4 终态 profile 两臂同形、corten 侧仅 zap_window 1.84% 在榜（机制成本地板定量）。
**G7（PT+metadata 内存开销定量）— 定性在档, 定量计划中**: 已有: M6.T4 三方口径一致性核对
（memory.swap.current == smaps Swap == ledger swapped_out, Δ32 页 < 容差 1643,
m6t34-verify.md §3）+ 本班 arena_stats 快照（meta_bytes 1,372,160 B / 335 阵列 ≈ 4KB/阵列,
ptdescs 335）+ DEV-12 每-16KB-mmap 一 descriptor 的结构性上界（T1c 池后 descriptor 随
mmap+munmap 往返复用, 稳态驻留 = 窗口数）。vs 论文理论上界（PT 页同量级 metadata array）
的正式对照测量未执行, 登记 §8。
**G6（稳定性）— 部分（首轮完成, 二夜计划中）**: syzkaller 首夜 17.8h/1.42M execs 零内存安全
crash + lockdep 全家族首检零 splat + KUnit 102 用例 0 fail + interlock flake 登记（§5.3/§5.4/§3-M7）。
"连续 2 夜无可复现 crash"完整口径与 KCSAN 短跑 = §8 计划中（corpus 748 种子已备）。
G8（诚实性）: 本报告内嵌口径已齐（§1.1 8vCPU 方向性 D4、§1.4 负向清单、§2.2 DEVIATION、
§2.4 LoC 终态账、§4.1 三口径）。

### 4.4 机制归因（发现问题 → 根因 → 数字变化，三段式）

**① TLB 风暴（perf1，commit 802ff7551bd0）**
- 发现: run3 的 G1 三格大负（mmap-pf -74.2 / unmap-virt -83.8）与 MODE 侧 osq_lock/IPI
  占榜矛盾。
- 根因: 路由空间操作的 mmu_gather 并发重叠 → `mm->tlb_flush_pending > 1` →
  `tlb_finish_mmu()` 读 `mm_tlb_flush_nested()` 强制 `fullmm=1` → 即使零 PTE 清除也发
  全 mm TLB shootdown IPI → IPI 延迟拉长 gather 存活窗 → 重叠概率上升（自持风暴）。
  ftrace 判据级实证: unmap-virt t4 3s 窗 T0 = 82,942 次 tlb_flush 事件 vs BASE = 95；
  pages:-1 全 mm 形态占绝对主导（results/r06/perf1.md §1.3）。
- 修复与数字: ①lazy per-window gather（PTE 锁下先扫窗，零内容不开 gather）；②park 元
  数据整块释放（corten_txn_meta_drop，等价 pristine 态）；③park flush 后置到
  write→read downgrade（legacy `vms_complete_munmap_vmas()` 同位）。逐构建: unmap-virt
  t4 0.012→0.791 ops/µs；mmap-pf t8 0.00033→0.00114（-74%→-14%）（perf1.md §2 表）。
  失败尝试 perf1a-v1（write 锁序列化 gather）把 unmap t8 +113% 打成 -56%，完整回滚
  记录在案——touched 形态的 flush 并行性是 T0 反超 legacy 的第一来源（perf1.md §2/§4）。

**② RELEASE drain 的 RCU GP 放大（D15，commit ba77046c78fe）**
- 发现: T5 首跑 dedup_eq tcmalloc 档 -91.84%（wall 311s vs ~23s）。
- 根因: born-percpu 的 percpu_ref 每次 kill 必付一个 RCU 宽限期（实测 GP 3.8-20ms），
  而 RELEASE drain 持 mmap_write；"每 munmap 一 RELEASE"的形状 = O(GP×次数) 串行放大，
  算术对表 25,600 对大块 mmap/munmap ≈ 实测崩溃数字（t5-overhead-diagnosis.md §1）。
- 修复与数字: PERCPU_REF_INIT_ATOMIC（kill 同步、drain 只等在飞事务）→ MODE munmap
  单次 8554µs→227.8µs；dedup tcmalloc 恢复到 +19%（5.94M vs 4.96M blocks/s）；
  debugfs 计数精确对账 auto_mmaps/munmap_releases +25,600（同上 §3）。

**③ present-RO PTE 缺陷族（D16，commit e219920b0923）——正确性修复同时解锁性能测量**
- 发现: MODE 下 dedup/metis/psearchy 三 app T0 腿 9/9 rc=139（libc ACCERR SIGSEGV），
  遮蔽 T5 全部应用测量。
- 根因: 两个同根缺陷——"路由提交只活在 per-page metadata，任何把页/VMA 退出 arena 语义
  的路径必须把 perm 带走": ①zap 内容丢弃把槽擦成 INVALID+perm=0 → FRESH 门跌回 DECLARE
  的 PROT_NONE；②fork_demote 把 shadow-VMA 还原成只带 DECLARE flags 的 plain VMA，
  已提交 perm 对父子同时蒸发（glibc 退出期清理写 ACCERR）（rogue-fix.md §2）。
- 修复与数字: CORTEN_UNMAP_KEEP_PERM + FRESH 门 perm 优先 + demote 前 perm 物化
  （__split_vma 手术）。修复后: dedup 5/5、metis 3/3、psearchy 3/3、java -version rc=0；
  KUnit +3 锚全绿；同批闭环 OQ-D 主体（rogue-fix.md §4）。

**④ 生命周期税 → 常驻池（M4.T1/T2 + T1c，de8a685370bb + 87383f51a3ff）**
- 发现: m4t12 动态口径四格 -22.6%~-88.1% 全负，但 T1 杂志机制本身 99% 回收生效——
  说明杠杆不在 VA 分配器。
- 根因: 每 16KB mmap 建一整个 arena 的 T0 架构税（probe mpl 形态 ~12.6µs/op 全程在
  mmap_write 内，多线程排队放大）（results/r07/m4t12-verify.md §4.3）。
- 修复与数字: T1c 池（park/reactivate，xa 槽零 churn + early-take ret 2 原地复用
  parked VMA）→ mpl 17.4µs→1.3µs（13× 消除，首次反超 legacy 4.1×）；mmap 全组转正
  +119~+278%；dedup tcmalloc 独立窗破 D15 记录（t1c-verify.md §0/§5）。

### 4.5 机制成本地板（当前设计粒度下不可消除项，perf1.md §4 原文口径）

1. mmap-pf t8 残差 -14.3/-15.5%（网格两轮一致）= 纯 fault 通道 -7.1~-11%（论文 Fig.5/8
   per-fault transaction 语义本身）+ take/park 簿记 3-5%（parked 窗 FOLL_FORCE 防线，
   不可省）；终态 profile 两臂同形，corten 侧仅 zap_window 1.84% 在榜。
2. T1c 池每 op 两次 mmap_write 段（take+park），与 legacy 同量级但临界区含路由工作；
   进一步压缩需 mark 批处理（M8 fault 域重构，非小 diff）。
3. pf/low 家族高方差（登记 CV 15~79%），以 5 次加测网格口径登记。

---

## 5. 正确性验证

### 5.1 KUnit 总数统计（ suites: corten / corten_arena / corten_fault ）

| 时点 | 用例数（on 臂 pass/skip） | 证据 |
|---|---|---|
| M2 | 16/16（单套件） | results/r01/m2b-kunit-boot-pass.log |
| M3a | 25（+8 arena） | results/r02/m3a-f1-kunit*.log |
| M3 终判 | 24+9+13 = 46 on 臂全绿 | results/r03/s8-kunit-on-console.txt |
| M4.T0 | 20+22+16 = 58 | results/r04/t0b/kunit-on*-r05.log |
| M5.T1a | 24+27+24 = 75 | results/r06/m5t1a/kunit-on1-final18.log |
| HEAD 802ff7551bd0（perf1 终验） | 24/0/1 + 33/0/0 + 24/0/0 = 81 pass + 1 设计 skip, 0 fail（×2 复跑） | results/r06/kunit-perf1-final.log |
| m5t1b（1f8dfc78ae9f, 已提交） | 24/0/1 + **38**/0/0 + **26**/0/0 = 88 pass + 1 skip, 0 fail（普通 + lockdep 变体双口径） | results/r07/m5t1b-kunit-on5-final.log；results/r07/m5t1b-kunit-on1-lockdep.log |
| m5t3（5c545359e856, 已提交） | 24/0/1 + **43**/0/0 + **26**/0/0（GUP 四态锚 ×5; lockdep 变体同数零签名） | results/r07/m5t3-kunit-final-on1.log；results/r07/m5t3-kunit-on3-final.log |
| m6t1（0e469cd9d055, 已提交） | 24/0/1 + 43/0/0 + **28**/0/0（rmap 守卫 + reap skip 锚; lockdep 同数） | results/r07/m6t1-kunit-on3-final.log；results/r07/m6t1-lockdep-kunit.log |
| m6t2（b51754002f2f, 已提交） | 24/0/1 + **44**/0/0 + **30**/0/2（swap 事务 +4 用例; lockdep 同数零签名） | results/r07/m6t2-kunit-on4-final.log；results/r07/m6t2-kunit-lockdep-final.log |
| **HEAD 2639d3294b9d（m6t34 = 终态）** | **24/0/1 + 48/0/0 + 30/0/2 = 102 pass + 3 设计 skip, 0 fail**（on ×2 + lockdep 变体同数零签名） | results/r07/m6t34-kunit-on3.log；results/r07/m6t34-kunit-lockdep.log |
| **arm64** | corten 套件 **25/25 全绿**（-smp 4） | results/r06/m9-p1/kunit-arm64-smp4.log |
| =n 回归 | 每切片 CORTEN_MM=n 七~十对象 nm 零 corten 符号（r02→r07 各切片在档） | 例: results/r07/m5t1b-verify.md §2c |

已知 flake 登记（非回归，M7 清单）: `corten_test_txn_uninstall_interlock` / 
`txn_mutex_disjoint` 时序 flake 各历史 1-3 例，复跑全绿（results/r07/m4t12-verify.md
§3 注；results/r06/rogue-fix.md §5）。KUnit 固有 2 条 WARNING = txn_begin 注入设计探针
（基线同形同址，results/r06/perf1.md §3）。

### 5.2 kselftests/mm

- M3 终判: 同一 bzImage off/on 双口径 fail-set 完全一致（off 10 pass+1 skip+1 fail，
  va_high_addr_switch = 基线/环境既有；on 同 fail-set 零确定性新 fail）
  （results/r03/final-smoke/m3-verdict-final.md DoD#3）。
- m5t1b: ksmoke 8 件套（map_fixed_noreplace/mremap_test/mremap_dontunmap/madv_populate/
  cow/mkdirty/protection_keys/gup_longterm/gup_test）**9/9 pass 0 skip 0 fail**，且在
  lockdep 内核上复跑同结果（results/r07/m5t1b-verify.md §2e）。
- run_mode_smoke 26/26 + SMOKE-DRIVER PASS: T0b 起每切片回归锚（最近: results/r07/
  m5t1b-verify.md §2e、results/r06/t5-run4-report.md QUICK）。

### 5.3 lockdep（M7 首检）

- PROVE_LOCKING 全家族构建（仅 2 条上游基线警告）；无盘 KUnit + guest 压测零 splat；
  interlock 用例 lockdep 开销下 1 次时序 flake 复跑绿（results/r07/m7-preheat.md）。
- m5t1b lockdep 变体: KUnit 全绿 + **roundtrip 1000 轮 + mtlock 1000 fork×8 故障线程**
  零 deadlock splat（results/r07/m5t1b-verify.md §2e）。
- 登记观察: `zap_untracked_window` 的 add_mm_counter-under-ptl preempt_nested WARN 仅
  corten=on lockdep boot 暴露（上游 zap_pte_range 同模式，待 M7 裁定，
  results/r07/t1c-verify.md §8-3）。

### 5.4 syzkaller 首轮（M7.T1/T2 数据点）

- 1,416,524 execs / 17.8h 连续（KCOV+KASAN 构建 @025756094542，运行面 mmap/munmap/
  mprotect/madvise/mbind/userfaultfd/openat+clone/ptrace）；corpus 748、coverage 16,875。
- **4 crash 全部环境/健壮性类（2×RCU GP kthread 饥饿 + 2×VM 无输出），零 KASAN
  slab/UAF/溢出、零 BUG/WARNING/oops，repro reliability=0.00**；正面旁证: M4.T0 padding
  输入校验被大量触发且安静拒绝（results/r07/syz-report.md §1/§3）。
- corpus 748 programs 已保留作下轮种子（同上 §5）。

### 5.5 fork/COW 隔离与对账

- fork_isolation 双臂 checksum e1c81840e4123903 跨臂（corten=legacy）等价；
  fork_roundtrip 1000 轮 checksum 076534f9ba241483 稳定 + MemFree 平稳（COW 泄漏修法
  闭合证明）（results/r06/m5t1a-final-verify.md §1b）。
- metis_eq MODE 全量 checksum 8a8db99075665220 三方（T1a on 臂/legacy 臂/m5t1b）同值 +
  fork-probe OK（results/r07/m5t1b-verify.md §2e）。
- cow_collision: 每轮 fork 后双侧各 2 kthread 对撞改写全部页，corten+legacy 双臂 200 轮
  零互串（results/r07/m5t1b-verify.md §2e）。
- debugfs 对账: fork_faithful 增长与 fork 次数一致（2202 = lockdep 变体终态）、
  fork_demotes=0、drain_timeout=0、rearm_failed=0、legacy_drift=0（同上 §2e；
  results/r06/t5-run4-report.md §0: run4 全矩阵 ejects=0、meta_arrays==ptdescs 负增长
  零泄漏、munmap_releases==pool_parks 精确对账 10.89M）。

### 5.6 strace 机制语义（T5 55 对 ×4 轮）

四轮全矩阵一致结论: differs 全部为 EAGAIN/ETIMEDOUT/ESRCH futex 竞争采样噪声，
**零 EACCES/EFAULT/ENOMEM/EPERM 新类**（results/r06/t5-run4-report.md §1、
t5-run3-report.md §1、t5-run2-report.md §4、t5/t5-r06-report.md §3）。

---

## 6. 兼容性验证（D12 零改动 DoD：已编译程序不经修改在 MODE 下跑通）

| workload | 状态 | 关键证据 | 残余 |
|---|---|---|---|
| java（JVM 启动） | ✓ | `java -version` rc=0（D-G'' CDS 打洞后，results/r06/rogue-fix.md §4）；run_t0_dod java mprotect_routes 0→110 路由接管实证（results/r04/t0b-verify.md） | find_vma_intersection RCU 语境 WARN 噪音级（功能无害，rogue-fix.md §6-2） |
| JThreadBench（2000 线程×3） | ✓ | gupfix 后 rc=0 零 CFE（results/r06/gupfix.md §3"零改动 DoD 最后缺口闭合"）；run3/run4 再证 0/6 CFE、3/3 rc=0 | ClassFormatError 降级观测项（results/r06/t5-run4-report.md §4） |
| metis_eq | ✓ | rogue 修复后 3/3 rc=0 checksum 一致（rogue-fix.md §4）；run3/run4 各 3/3 rc=0 | fork 后段 OQ-D 残余=闭合维持观测项 |
| dedup_eq（glibc + tcmalloc 双档） | ✓ | run4 全 3 rep rc=0 双档（results/r06/t5-run4-report.md §3）；D15 崩溃级回退 -91.84% 已修复（t5-overhead-diagnosis.md §3.2） | 无 |
| psearchy_eq | ✓ | run3/run4 rc=0；-40% 方差定案暖机假象（results/r06/g1-consolidation.md §4） | 无 |
| lmbench lat_proc | **✓ 功能面**（性能面 = G5 NOT MET, §4.3-4） | MODE 臂 fork/fork+exec/shell 全部跑通 rc=0（STRICT ENTER + marker 断言 9/9, results/r07/g5-gate/）；正确性语义: 子进程完整存活、零 fork_demote、遥测零异常 | **性能回退 +516%/+172%/+354% = MODE 生命周期退出 RCU GP（根因已证, 修复 §7-A5）**；M1 分母在档（publish/baseline/） |
| 非 opt-in 进程零感知 | ✓ | =n 编译折叠（nm 零符号）+ corten=off 冒烟（18/26 过、8 失败全为 mode 门 EOPNOTSUPP 设计行为）+ legacy 一致性面全过（results/r06/gupfix.md §3） | 无 |

MODE-process 的登记观测差（T1c 池结构性代价，t1c-verify.md §6）: parked 期间
/proc/maps 显示为普通 PROT_NONE 匿名预约（pre-T1c 为无 VMA）→ si_code 为 ACCERR 非
MAPERR；mprotect/mlock 作用于预约范围由 ENOMEM 变为 legacy 成功/拒绝族。应用契约
（access-faults/内容干净/MAP_FIXED 复用/fork 清洁）全部保持。

---

## 7. 已知问题与遗留（终稿分级）

### 0. 已闭环（本终稿起移入历史档，仅留索引）

| 项 | 闭环证据 |
|---|---|
| D-G'' shadow-VMA CDS 打洞（原 A 级） | 452ad7b9d91e; MODE `java -version` rc=0、三 app 全 rc=0（results/r06/dg2-verify.md、rogue-fix.md §4） |
| present-RO 缺陷族（原 T5 遮蔽项） | e219920b0923（CORTEN_UNMAP_KEEP_PERM + FRESH 门 perm 优先 + demote 前 perm 物化）; 修复后 dedup 5/5、metis 3/3、psearchy 3/3 rc=0（rogue-fix.md §4） |
| OQ-D metis fork 后段主体 | 忠实 fork 落树后 run3/run4/g1 班维持闭合（checksum 三方同值, g1-final.md §4）; 残余降级 B2 观测 |
| JThreadBench ClassFormatError（原 B1） | gupfix 0719bc6ae74e 修复; run3/run4/g1 班/G5 班四次独立 0 命中（results/r06/gupfix.md §3; results/r07/g5-gate/） |
| GUP 短读/互操作（M5.T3 审计目标） | 5c545359e856; fast 零改动证实 + slow 两真 bug 修复 + guest 四态矩阵全绿（results/r07/m5t3-verify.md） |
| A1 G1 加测固化缺失 | **已执行**: 五轮 ABAB 固化 G1 = MET 2/4（results/r07/g1-final.md, §4.2） |
| A4 M5.T1b/T2' 未 commit | **已提交** 1f8dfc78ae9f tag corten-r07-m5t1b（git log 核对, §9.1） |

### A. 开放——需代码/测量动作的（按优先级）

| # | 问题 | 根因锚 | 建议 |
|---|---|---|---|
| A5 | **G5 NOT MET: MODE 生命周期退出 RCU GP**（fork +516%/exec +172%/shell +354%; 隔离实验 +50ms/进程） | g5-gate.md §2（ENTER 急切建 registry + fork 急切复制 + exit_mmap `synchronize_rcu`）; 与忠实 fork 无关（fork_faithful 全程不变） | 小 diff 切片二选一: ①registry join 惰性化（首个 arena declare 时入册, 零 arena MODE mm 退出免费）; ②空 state 退出 `kfree_rcu` 快速路径。完成后 G5 复测（估半夜+半小时） |
| A6 | G2 perfetto 定量（M8.T2）未执行 | §4.3-G2（定性替代已引用: M3 头条 + M1 PER_VMA_LOCK + perf1 §4 profile） | SQL 固化三件套（mmap_lock_contention/fault_latency/sched_breakdown），1 日窗 |
| A7 | G7 内存开销定量 vs 论文上界未执行 | §4.3-G7（定性 + 三方口径核对在档） | 常驻/换出两态 × 窗口数扫描（meta_bytes/ptdescs/desc 行数 vs 理论 array 上界），半天 |
| A8 | M7 第二夜 + KCSAN（G6 完整口径） | §3-M7（corpus 748 种子已备） | syz-manager 续跑一夜 + KCSAN 构建短跑（伴随） |

### B. 登记观测的（不阻塞，持续盯）

| # | 问题 | 根因锚 | 状态 |
|---|---|---|---|
| B2 | metis_eq fork 后段（OQ-D 残余） | 忠实 fork 闭环（g1 班 fork_faithful 前进 + checksum 同值） | 观测项维持 |
| B3 | interlock/_mutex_disjoint KUnit 时序 flake | m4t12-verify §3 注 + m6t2-verify §2（基线内核同签名同频, host vCPU 饥饿） | M7"首跑容忍/复跑判定"清单 |
| B4 | zap 路径 add_mm_counter-under-ptl preempt_nested WARN（tier-2, 仅 corten=on lockdep boot 暴露） | t1c-verify §8-3（上游 zap_pte_range 同模式） | 随 A8 lockdep 复跑裁定 |
| B5 | find_vma_intersection RCU WARN（java 高频 42 条/班，噪音级） | rogue-fix §6-2（tier-2 RCU walk 合法） | 改 lockless 变体或压掉 assert（下片） |
| B6 | pf/low 家族高方差（CV 15~79%） | perf1 §4-3（单发 5s 窗摆动 -21%~+29%） | 5 次网格口径永久登记（固化班 pf -7.18/-3.82 同族） |
| B7 | 杂志回收列表注释"LIFO"实为 FIFO+尾块扩展；auto_route 测试 addrs[8] 封顶 | STATE r07 收口条（非阻塞备注） | 文档/测试件顺手修 |
| B8 | run_mode_smoke `released-arena-unmapped/gone` 2 例既有失败 + 该二进制退出 stack-smash abort（m6t34 班新观察） | m6t2-verify §3（T1 基线内核逐字复现=既有）、m6t34-verify §3（二进制自身缺陷） | smoke 件维护切片 |
| B9 | ClassFormatError / JVM spawn 幅度族（G4 定案并入） | 跨 boot 方向不定（run3 -2.51 / run4 -6.59 / g1 班 +6.5 / G5 班 -7.1） | 噪声族观测维持（§4.3-3） |

### C. 后续切片的（设计级杠杆，非缺陷）

| # | 方向 | 根因锚 | 归属 |
|---|---|---|---|
| C1 | mmap-pf fault 通道批量化/percpu 化（固化终读数 -17.3/-22.3 的最后一段） | perf1 §4-1/2（每 fault 一事务 + take/park 两次写锁段） | fault 域重构（D19 反面账） |
| C2 | unmap/unmap-virt CHUNK 形状差（池正交） | t1c-verify §8-2 | CHUNK 快路径（G1 已 MET, 优先级下调） |
| C3 | zram 同步写批提交（批量换出天花板 82-350µs/页的大头） | m6t34-verify §3/§5-3（上游 page_io `swap_writepage_bdev_sync` 固有形态） | 上游/T5 性能化 |
| C4 | M9 P2/P3: arm64 热钩子接线 + contpte/BBM 决议（OQ1/P3） + gupfix access_error 等价门同型处理 | ARM64_PORTING.md（publish/）；gupfix §5-3 | M9 |
| C5 | mremap_move PROT_NONE 窗 perm 取值 | gupfix §5-1（无 workload 触发） | 下片 |
| C6 | FORCE 边界: perm-RO + VM_WRITE 的 FOLL_FORCE = EIO 响亮拒绝（M5.T3 已将 force 语义整体移除, 此为该裁决的登记面） | m5t3-verify §3.1 | 语义裁决在档 |
| C7 | F2 窗粒度 DONTCOPY 页级过度 SHARED（良性自愈） | m5t1b-verify §3-1 | +~10 行页粒度门可选 |
| C8 | mmbench 动态口径永久化 + meta 记 sha256 | D1 口径更正（STATE） | 已在 T5 runner/固化班执行；后续沿用 |

---

## 8. 剩余工作与计划（终稿口径: 已完成打勾, 未竟项显式登记编号）

| 序 | 工作 | 状态 | 说明 |
|---|---|---|---|
| 1 | M4.T5 收口 | **✓ 完成** | G1 五轮 ABAB 固化 = MET 2/4（results/r07/g1-final.md）；G3 机制面定案、G4 噪声族定案（§4.3）。原"三组 5 次加测"中 dedup/jvm 两组未执行, 以"run 谱系 + 固化班独立 boot 复核"定案（§4.3-2/3 如实声明） |
| 2 | M5 收口 | **✓ 完成** | T1b/T2' = 1f8dfc78ae9f tag corten-r07-m5t1b；T3 = 5c545359e856 tag corten-r07-m5t3（§3-M5） |
| 3 | M5.T3/T4 | **✓ 完成** | GUP 互操作全绿; G5 lat_proc MODE 臂实测 = NOT MET + 根因 + 修复方向（§4.3-4、§7-A5）; JVM MODE 臂 = JTB 回归 rc=0 零 CFE |
| 4 | M6.T1-T4 | **✓ 完成** | T1/T2/T3+T4 全提交; guest 判据闭合（69164/69356 roundtrip、RSS 281→3.8MB、三方口径一致）（§3-M6）; G7 定量除外 → A7 |
| 5 | M7 周期 | **首轮 ✓** | syz 1 夜 + lockdep 首检 + 每切片 lockdep 变体全绿; 第二夜/KCSAN/B4 裁定 → A8 |
| 6 | M8.T1（G1 固化） | **✓ 完成** | 与工作项 1 同夜窗合并执行 |
| 7 | M8.T2 perfetto G2 | **计划中 → A6** | 定性替代已在 §4.3-G2 引用 |
| 8 | M8.T3 报告终稿 | **✓ 本报告 v1.0** | G1-G8 全 gate 对账（§3-M8）、LoC 终态账（§2.4）、G8 声明内嵌（§1.4-8）; ARM64_PORTING.md 已定稿（publish/） |
| 9 | G5-fix 修复切片 | **计划中 → A5** | 惰性入册 / kfree_rcu 快速退出 + G5 复测 |
| 10 | （并行） | publish push | 待用户凭据（R9）; publish 本地已提交 |

关键路径 1→2→4→8 已走完; 开放项 A5-A8 为独立小切片, 无相互阻塞。

---

## 9. 附录

### 9.1 提交清单（25 个项目提交 / 24 tag；主树 android17-6.18，均未 push；HEAD `2639d3294b9d`）

| # | commit | tag | 内容 |
|---|---|---|---|
| 1 | 93d907a5ff2e | corten-r01-m0 | M0: android17-6.18 boots |
| 2 | b8386e4e2467 | corten-r01-m2a | CortenMM skeleton: PT-page descriptors, per-PTE metadata, KUnit |
| 3 | 2fd4070e745c | corten-r01-m2b | covering-PT-page lock protocol and transaction API |
| 4 | 1284a235f751 | corten-r01-m2c-fix1 | defer static key enable to initcall; review fixes (D9) |
| 5 | e911b31adb9c | corten-r02-m3a-f1 | BH-symmetric PT-page descriptor locking |
| 6 | d040b61051af | corten-r02-m3b-s123 | prctl registration, per-mm frame xarray, shadow-VMA |
| 7 | 7bba3b9f7390 | corten-r02-m3b-s46 | fault-path transactions, upper-table fill, spatial routing |
| 8 | 96466df24387 | corten-r02-m3b-s46-fix1 | fresh-fault dispatch, ref pairing, TLB-correct zap |
| 9 | ae236ee077ac | corten-r02-m3b-s8 | debugfs observability (arenas ledger, arena_stats) |
| 10 | 5aef23c4aee9 | corten-r05-m4t0a | MODE-process transparent takeover (lock-order refactor) |
| 11 | 9d74b22a1348 | corten-r05-m4t0b | MODE spatial routing (mprotect/madvise/mremap) and counters |
| 12 | 452ad7b9d91e | corten-r06-m4dg2 | shadow-VMA hole punching and vma-lock fix (D-G'') |
| 13 | ba77046c78fe | —（D15，唯一无 tag 提交） | born-atomic transaction refs, GP-free drain |
| 14 | e219920b0923 | corten-r06-rogue | carry routed permissions across content drops and fork demotion (D16) |
| 15 | 025756094542 | corten-r06-m9p1 | arm64 4K-page build support and arch-neutral test macros |
| 16 | 0719bc6ae74e | corten-r06-gupfix | defer GUP and kernel-mode fault gates to arena metadata |
| 17 | 68697442097e | corten-r07-m5t1a | faithful fork (frozen window, metadata mirroring, COW) |
| 18 | de8a685370bb | corten-r07-m4t12 | per-cpu VA magazine and munmap fast path (M4.T1/T2) |
| 19 | 87383f51a3ff | corten-r07-t1c | resident arena pool for MODE processes (T1c) |
| 20 | 802ff7551bd0 | corten-r06-perf1 | skip TLB gathers on PTE-less windows, park flush post-downgrade |
| 21 | 1f8dfc78ae9f | corten-r07-m5t1b | COW unshare, INV7 checker and fork test battery (M5.T1b/T2') |
| 22 | 5c545359e856 | corten-r07-m5t3 | GUP interop fixes and pin accounting (M5.T3) |
| 23 | 0e469cd9d055 | corten-r07-m6t1 | reclaim guards for oom_reaper and rmap (M6.T1) |
| 24 | b51754002f2f | corten-r07-m6t2 | swap out/in transactions (M6.T2) |
| 25 | 2639d3294b9d | **corten-r07-m6t34（=HEAD）** | reclaim shrinker, observability and sleep-correct zap (M6.T3/T4) |

补丁镜像: patches/（0001-*.patch 全序列 + 各切片 diff 快照）。bzImage 归档: bzimg/
（r01-m0-android17-baseline … r07-t1c/r06-perf1/r06-t5d，各带 .sha256 详 bzimg/ 目录）。
green 门台账: run/green.txt。

### 9.2 证据索引（results/ 路径）

- 环境与链路冒烟: results/r00/（perfetto 全链路，log/20260912-r00-smoke.md）
- M0-M2: results/r01/（boot-m0-console.log、m0-dod-*、m2-dod-{off,on}.md、m2fix-report.md、
  m2b-kunit-boot-*.log、baseline/、config-android17-m0）
- M1 基线（M8 分母）: publish/baseline/（baseline_meta.json + mmbench/apps/jvm/latproc
  summary + runner 脚本 + bench-src.tar.gz）
- M3a/M3b: results/r02/（m3a-f1-report.md、s123-verify.md、s46-verify.md、infra-report.md）
- M3 判定: results/r03/（m3-dod-verdict.md 首轮 FAIL、final-smoke/m3-verdict-final.md
  终判 PASS、s46-fix2/3.md、s8-verify.md）
- M4.T0: results/r04/t0a-verify.md、results/r04/t0b-verify.md、results/r05/t0b/
- D-G'': results/r05/dg2-analysis.md、results/r06/dg2-verify.md、results/r06/dg2-fix2.md
- T5 诊断与修复: results/r06/t5-overhead-diagnosis.md（= publish/t5-overhead-diagnosis.md）、
  results/r06/t5-overhead-build2*.log
- present-RO 族: results/r06/rogue-fix.md（探针内核取证 rogue/）
- JTB CFE 根因: results/r06/jtbcfe-investigation.md、results/r06/jtbcfe-final.md
- GUP 门修复: results/r06/gupfix.md + results/r06/gupfix/
- M5.T1a: results/r06/m5t1a-final-verify.md + results/r06/m5t1a/（guest-final/、
  roundtrip-1000-r2.log、fork_isolation/）
- M4.T1/T2+D1 诊断: results/r07/m4t12-verify.md + results/r06/m4t12/DIAGNOSIS.md +
  results/r07/abab/
- T1c 池: results/r07/t1c-verify.md + results/r07/t1c/
- perf1 TLB 风暴: results/r06/perf1.md + results/r06/perf1/ + results/r06/perf1.diff
- T5 四轮: results/r06/t5/t5-r06-report.md（首跑）、results/r06/t5-run2-report.md、
  results/r06/t5-run3-report.md、results/r06/t5-run4-report.md（run4 正式判定）；
  原始数据各 t5-run*/
- G1 加测与方差裁决（历史口径存档）: results/r06/g1-consolidation.md + results/r06/g1-consol/
- **G1 五轮固化（M8.T1 终件）**: results/r07/g1-final.md + results/r07/t5final/
  （raw 110 条单行 JSON + arena_stats.{before,midpoint,after} + analyze_g1final.py）
- **G5 门（M5.T4 终件）**: results/r07/g5-gate/g5-gate.md + results/r07/g5-gate/
  （raw/run1=判废存档、raw/run2=干净口径、jtb/、隔离实验、arena_stats 快照、runner 脚本）
- M5.T1b/T2': results/r07/m5t1b-verify.md + results/r07/m5t1b-* + results/r07/m5t1b-guest/
- M5.T3: results/r07/m5t3-verify.md + results/r07/m5t3-{kunit,gupmat,probe*}* + m5t3-guest/
- M6.T1: results/r07/m6t1-verify.md + results/r07/m6t1-*.log
- M6.T2: results/r07/m6t2-verify.md + results/r07/m6t2-guest/ + results/r07/m6t2-*.log
- M6.T3+T4: results/r07/m6t34-verify.md + results/r07/m6t34-guest/ + results/r07/m6t34-*.log
- M7: results/r07/m7-preheat.md、results/r07/m7-preheat-syz.md、results/r07/syz-report.md、
  results/r07/{lockdep-build.log, lockdep-kunit.log, lockdep-kunit-run2.log,
  kcov-build.log, syz-manager.log}
- M9: results/r06/m9-p1/m9-p1-verify.md + results/r06/m9-p1/（kunit-arm64*.log、
  image-build.log）、results/r02/m9-baseline-build.log（Round A/B）
- 设计/规范: docs/{PAPER_SPEC,DESIGN,EVAL,ROADMAP}.md；publish/{M3B_DESIGN,M4T0_SPEC,
  M5_FORK_SPEC,ARM64_PORTING}.md
- 夜叙事: log/20260912-r00-smoke.md、log/20260913-r01.md、log/20260914-r02.md、
  log/20260916-r04.md、log/20260917-r05.md、log/20260918-r06.md（r07 起 = STATE.md
  + publish/STATE-snapshot-20260919-r07.md）

### 9.3 复现指南（同参重跑）

环境: `source /home/ppw/cortenmm/bin/env.sh`（KDIR/BZIMG/VM 工具链路径）。
guest: `KERNEL=<bzImage> bash ~/bench/host/launch_vm.sh ~/vm/trixie.img
"corten=on mitigations=off kunit.enable=0 log_buf_len=64M"`（8 vCPU/4G/KVM）。

- **T5 全矩阵**（run4 同参）: 驱动 bench/t5/（QUICK=1 冒烟 35 配置 → 全矩阵 330 腿，
  5400s deadline）；动态口径 `MMBENCH_BIN=/root/m4t12/bin/mmbench_dyn`
  （sha256 38304f06…）；分析 `analyze_t5.py` → t5_summary.json + t5_report.md；
  判定表生成见 results/r06/t5-run4/（meta/ + raw/ 330 条 JSON + trace 55 对）。
- **单格 ABAB 加测**: `mmbench_dyn <bench> <cont> <t> 1 <seed>`，seed =
  `20260913 + bidx*1009 + cidx*97 + t*7 + k`（与 T5 驱动同式，results/r06/
  g1-consol/run_g1consol.sh）；BASE 臂 `env -u LD_PRELOAD`，T0 臂
  `LD_PRELOAD=<hook> CORTEN_MODE_HOOK_STRICT=1`。
- **KUnit**: 无盘 `qemu-system-x86_64 -enable-kvm -m 2048 -smp 4 -kernel bzImage
  -append "console=ttyS0 panic=-1 kunit.filter_glob=corten*" -nographic -no-reboot`；
  arm64: `qemu-system-aarch64 -M virt -cpu max -m 1024 -kernel arch/arm64/boot/Image
  -append "console=ttyAMA0 kunit.enable=1 kunit.filter_glob=corten*"`（results/r06/m9-p1/）。
- **smoke/兼容**: guest `run_mode_smoke`（26 项）+ `run_t0_dod.sh` + ksmoke 8 件套
  （预编译口径 `--kdir /root/ktree`，results/r07/m5t1b-verify.md §2e）。
- **fork 压测**: bench/arena-stress/{fork_arena_test.c, cow_collision.c, fork_battery.sh}
  （`/root/fork_battery.sh --prebuilt /root/ks-ship --outdir …`）。
- **syzkaller**: `syz-manager -cfg results/r07/syz-cfg-final.json`（KCOV+KASAN 内核
  /home/ppw/linux-6.18-kcov @025756094542，corpus 种子 /home/ppw/syzwork/corpus/）。
- **perfetto**: 采集/SQL 模板 results/r00/smoke_guest.cfg + docs/EVAL.md §5
  （G2 定量对照 = A6 计划中, 采集链路已在 r00 冒烟验证）。
- **G5 复测**（A5 修复后）: guest 侧 `bash /mnt/hostshare/g5gate/run_g5_clean.sh
  /mnt/hostshare/g5gate`（hook = bench/mode-hook/corten_mode_hook.c guest 现编;
  判废教训与干净口径见 results/r07/g5-gate/g5-gate.md §0——NOPROBE 变体 +
  两臂 /tmp 对称重定向）。

---

## 附: 本报告自检状态（v1.0 终稿, 2026-09-21）

- 全部 §引用的 results/publish/docs/log 路径已核对存在（终稿时点 2026-09-21; 本附录更新时
  再次抽样复核）。
- PENDING 占位清零: 原骨架 12 处 PENDING 全部处置——8 处数据位已填充
  （LoC 终态账 §2.4 / G1 固化 §4.2 / G3·G4 定案 §4.3 / G5 实测 §4.3 / 证据索引·复现指南）,
  4 处未竟项转为显式「计划中」登记（G2→A6 / G7 定量→A7 / G6 二夜+KCSAN→A8 / G5-fix→A5）,
  无"待补"式含糊表述残留。
- G1-G8 gate 终态对账: **G1 MET 2/4（五轮 ABAB 固化, unmap +45.4/+142.0、unmap-virt
  +1142/+2583）** / **G2 计划中（A6; 定性替代 = M3 头条 + M1 PER_VMA_LOCK + perf1 §4
  profile）/ G3 数字面 NOT MET·机制面成立（四 boot 方向恒正 + 幅度不定, 定案声明）/
  G4 边缘项定案 JVM 噪声族（跨 boot 方向不定, 观测维持）/ G5 NOT MET（+516%/+172%/+354%,
  生命周期退出 RCU GP 根因已证, 修复 → A5）/ G6 部分（syz 1 夜 + lockdep ✓ + KUnit
  102 用例 ✓; 二夜口径与 KCSAN → A8）/ G7 定性在档 + 三方口径核对, 定量 → A7 / G8
  诚实性声明内嵌（8vCPU 方向性 D4、三口径 §4.1、DEVIATION 清单 §2.2、LoC 净增声明 §2.4、
  负向清单 §1.4）**。
- 与 STATE.md 一致性: HEAD/tag/里程碑状态/M5-M6 判定与 STATE 及
  publish/STATE-snapshot-20260921-r07c.md 同源; G5 NOT MET 与 A5 登记为本终稿新增事实,
  已同步 STATE（r07 班 G5 条目）。
- 数字溯源: 本报告全部数字可经 §9.2 索引追溯至 results/ 原始件; G1 固化数字 =
  results/r07/g1-final.md（10 格 × 5 轮全表）; G5 数字 = results/r07/g5-gate/g5-gate.md
  （run2 raw + 隔离实验）; M6 数字 = m6t2-verify/m6t34-verify §3; KUnit 计数 = 各切片
  kunit-on*log 的 TAP 行计数。
