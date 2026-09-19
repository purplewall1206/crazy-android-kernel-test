# CortenMM → Linux 6.18 移植 · 最终报告（M8）

- 版本: v0.9 骨架（2026-09-19 起草；数据截止 = r07 班 + M4.T5 run4）
- 内核树状态: android17-6.18，HEAD `802ff7551bd0`（tag `corten-r06-perf1`），项目提交 20 个、tag 19 个
- 唯一权威状态源: `STATE.md`（现场）/ `docs/ROADMAP.md`（任务级）；本报告为 M8.T3 交付物的骨架，
  待完成项以 `<!-- PENDING: ... -->` 显式标注，其余全部数字均有 `results/` 证据路径可复核
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

### 1.2 当前达成（截至 2026-09-19 晚）

| 阶段 | 状态 | 一句话结论 |
|---|---|---|
| M0-M2 | ✓ | android17-6.18 基线 + M1 基线全矩阵落盘 + 内核骨架/协议 3 提交，KUnit 16/16（results/r01/） |
| M3 (M3a+M3b) | ✓ | DoD 4/4 PASS；头条主张实证：**perf 367K 样本 `find_vma`/`mmap_lock` 零命中**（results/r03/final-smoke/m3-verdict-final.md） |
| M4 | **代码完成**；T5 判定进行中 | T0 透明接管 + T1/T2 杂志批处理 + T1c 常驻池 + perf1 TLB 风暴修复全部在树；**M4.T5 run4: G1 首次 MET（2/4）**，G3/G4 未全过（§4.3，results/r06/t5-run4-report.md） |
| M5 | T1a ✓ 提交；T1b/T2' 代码完成+验证全绿，**待 commit** | 忠实 fork（冻结窗+meta 镜像+COW）；fork_roundtrip 1000 轮全绿（results/r06/m5t1a-final-verify.md；results/r07/m5t1b-verify.md） |
| M6 | 未开始 | rmap/swap/回收（ROADMAP §3） |
| M7 | 首轮数据点已取得 | lockdep 首检零 splat（results/r07/m7-preheat.md）；syzkaller 17.8h / 1.42M exec **零内存安全 crash**（results/r07/syz-report.md）；KCSAN 未跑 |
| M8 | 本报告 = 骨架 | 全矩阵 5 次口径重跑 + perfetto G2 + 终稿待做（§8） |
| M9 | P1 ✓ | **CortenMM KUnit 25/25 首次在 arm64 全绿** + 全量 Image（+196 KiB）（results/r06/m9-p1/m9-p1-verify.md） |

### 1.3 核心数字亮点（全部同 boot BASE vs T0 口径，除注明外）

| 项 | 数字 | 出处 |
|---|---|---|
| unmap-virt（虚拟 unmap，PTE-less 窗）低竞争 t4 / t8 | **+1156% / +2792%**（run4，两臂 CV ≤7.3；high 侧 t16 高达 +7034%） | results/r06/t5-run4-report.md §1/§2 |
| unmap（预映射区）低竞争 t4 / t8 | **+33.5% / +143.5%**（run4，CV ≤10.6） | 同上 |
| mmap 全组（MODE 空间分配） | +119% ~ +278%（run4 维持，T1c 池生命周期税消除） | results/r06/t5-run4-report.md §2 |
| dedup_eq tcmalloc 档 t8 | 谱系最优 **+11.3% ~ +16.1%**（run2 +11.32 / run3 +16.14，三 run 无重叠；run4 +6.33 正向收窄，§4.3 如实登记） | results/r06/t5-run2-report.md §2；results/r06/t5-run3-report.md §3；results/r06/t5-run4-report.md §3 |
| arena 生命周期税（T1c 池） | mmap+munmap 对 17.4µs → **1.3µs（13× 消除）**，probe 首次反超 legacy 4.1×；池命中率 99.9%+ | results/r07/t1c-verify.md §0/§5.3 |
| D15 GP-free drain | MODE munmap 单次 8554µs → **227.8µs**（62×）；dedup tcmalloc 从 -91.84% → +19% | results/r06/t5-overhead-diagnosis.md §3.1/§3.2 |
| perf 头条主张 | churn 4t 运行中 367K 样本，`find_vma\|mmap_lock` 六符号宽口径 **0 命中** | results/r03/final-smoke/m3-verdict-final.md DoD#4 |
| arm64 移植证据 | **KUnit 25/25 全绿**（-smp 4 复跑）+ CORTEN_MM=y 全量 Image 42,232,320 B（+196 KiB） | results/r06/m9-p1/m9-p1-verify.md §2/§3 |
| syzkaller 首轮 | **1,416,524 execs / 17.8h，4 crash 全部为环境类，0 内存安全类** | results/r07/syz-report.md §1/§3 |
| lockdep 首检 | PROVE_LOCKING 全家族构建 + KUnit/guest 压测 **零 splat** | results/r07/m7-preheat.md |
| KUnit 全套终态（committed HEAD） | corten 25 + corten_arena 33 + corten_fault 24 = **82 用例 on 臂 0 fail**（+1 设计 skip）；m5t1b 树上 89 用例全绿 | results/r06/kunit-perf1-final.log；results/r07/m5t1b-kunit-on5-final.log |

### 1.4 诚实边界（与亮点并存的负向/未决项，M8 不隐藏）

1. **mmap-pf（映射区 page-fault）残余回退**: run4 低竞争 t4/t8 = **-8.8% / -17.1%**。
   perf1 已归因为机制成本地板：纯 fault 通道（每 fault 一事务：fill_upper + lock_range +
   xa_load + query + mark）≤11% + take/park 簿记 3-5%，进入 IPI/调度噪声地板，非风暴性
   回退（results/r06/perf1.md §4）。这是论文 Fig.5/8 per-fault transaction 语义本身的价格。
2. **G3 数字面 NOT MET（run4）**: dedup 双档 +4.60/+6.33 正向但未到 10% 线（run3 曾 +11.30/
   +16.14，跨轮幅度收窄如实登记，三轮无重叠非方差假象）（results/r06/t5-run4-report.md §1/§3）。
3. **G4 边缘未过（run4）**: jvm -6.59% 单项超 5% 线 1.6pp（三 run 方向一致但分布重叠，
   JVM spawn 噪声族登记为观测项）（同上 §1）。
4. **G1 判定史与口径演化**: 首跑 0/4（被 present-RO 缺陷族遮蔽）→ run2 1/4 → g1-consol
   加测 0/4 → **mmbench 静态链接口径缺陷（D1）使 run2/g1-consol 全部 mmbench 读数作废** →
   run3（动态口径）1/4 → perf1 TLB 风暴修复后 **run4 = 2/4 MET**。三口径并列呈现在 §4.1，
   不以单轮数字下结论。
5. **JThreadBench ClassFormatError**: 已由 gupfix（0719bc6ae74e）修复，run3/run4 双重再证
   未复现（0/6），降级为观测项维持（results/r06/gupfix.md §3；results/r06/t5-run4-report.md §4）。
6. **metis_eq fork 后段（OQ-D 残余）**: 忠实 fork 在树后 run3/run4 维持闭合（3/3 rc=0，
   fork_faithful +88 同值），观测项维持，无恶化（results/r06/t5-run4-report.md §4）。
7. 8 vCPU vs 论文 384 核：只做方向性验证；64 线程平台期、2270×/1489× 量级差距、384 核
   线性段不可测（docs/EVAL.md §3 不可测清单；G8 正式声明见 §8 PENDING）。

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
│  T1b/T2' (待提交): INV7 checker / F2 子侧 pmd 门 / FOLL_FORCE  │
│  /UNSHARE 事务化 / OQ-4/5 裁决                                 │
└────────────────────────────────────────────────────────────────┘
corten=off / 非 MODE 进程: 零感知（=n 编译折叠 + static branch + 等价测试三重保障）
```

### 2.4 代码量现状

主树 6 个 corten 文件合计 **15,125 行**（mm/corten.c 1550 / mm/corten_arena.c 6713 /
mm/corten_test.c 2113 / mm/corten_arena_test.c 3685 / include/linux/corten.h 543 /
include/linux/corten_arena.h 521；HEAD 802ff7551bd0 实测 `wc -l`）。opt-in 路线
"先加后减"（D12）：终态账（若 arena 成默认可删的 VMA 热路径代码量 vs corten 增量）留 M8.T3。

<!-- PENDING: LoC 终态账 (D12②: mm/mmap.c+vma.c+maple_tree ≈12.5k 行 vs corten 15.1k 行的终态对照表, M8.T3 交付) -->

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

### M4 事务化空间操作 — **代码完成**；T5 性能判定进行中
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
- **M4.T5 正式判定（run4）: G1 MET（2/4）**；G3 数字面 NOT MET；G4 边缘未过（jvm
  -6.59%）。全文见 §4.2；原始数据 results/r06/t5-run4/。
- 遗留: G3/G4 收口（5 次加测口径）→ §8。

### M5 fork/COW/GUP — T1a ✓ 提交；T1b/T2' 待提交
- **M5.T1a 忠实 fork**: 68697442097e（冻结窗 + copy_page_range 照常 + fork_commit 事务
  meta 镜像 + COW 核心 + fork_demote 废止）；证据 results/r06/m5t1a-final-verify.md（终态
  补跑全绿）+ results/r06/m5t1a/。登记 D17（DEV-14）/D18（DEV-15）。
- 关键数字: fork_roundtrip **1000 轮 PASS**（checksum 076534f9ba241483 稳定，逐 100 轮
  MemFree 平稳 = COW-reuse 泄漏修法闭合）；fork_isolation 双臂 checksum
  e1c81840e4123903 跨臂等价；metis_eq MODE 全量三方同值 8a8db99075665220 + fork-probe OK
  （OQ-D 闭环落终态代码）；对账 fork_faithful=1005、fork_demotes=0。
- **M5.T1b + T2'**（INV7 checker / F2 子侧 pmd 门 / F3 drain 注入 / FOLL_FORCE 与
  UNSHARE 事务化 / OQ-4/5 按上游代码裁决关闭 / Fig.8 L26-38 对拍锚 / cow_collision 压测）:
  5 文件 +1127/-13，patches/r07-m5t1b.diff 在档；验证全矩阵全绿（KUnit on3/on4/on5-final
  89 用例 0 fail、off 全绿、**lockdep 变体零 splat 下 roundtrip 1000 轮 + mtlock 1000
  fork×8 线程**、ksmoke 9/9、cow_collision 双臂 200 轮零互串）；
  证据 results/r07/m5t1b-verify.md + results/r07/m5t1b-*/。
- 遗留: **未 commit**（review/maintainer 班次处理）；残余三项登记（窗粒度 DONTCOPY 过度
  SHARED 良性自愈 / FORCE 边界 perm-RO+VM_WRITE EIO 响亮拒绝 / fork_battery ksmoke
  --kdir 参数），见 results/r07/m5t1b-verify.md §3。

### M6 rmap/回收/swap — 未开始
- 依赖 M5 COW 位（已具备）；切片表 docs/ROADMAP.md §3 M6.T1-T4。

### M7 稳定性 — 伴随式进行中，首轮数据点已取得
- lockdep 首检（M7 预热）: PROVE_LOCKING 构建 + KUnit + guest 压测零 splat；interlock
  用例在 lockdep 开销下 1 次时序 flake 复跑绿（M7 清单）；证据 results/r07/m7-preheat.md、
  results/r07/{lockdep-build.log, lockdep-kunit.log, lockdep-kunit-run2.log}。
- syzkaller 首轮挂机: 17.8h / 1,416,524 execs / corpus 748 / coverage 16,875 / 4 crash
  全部环境类（2×RCU 饥饿 + 2×VM 无输出，repro reliability=0.00）、**零内存安全类**；
  证据 results/r07/syz-report.md、results/r07/{m7-preheat-syz.md, syz-manager.log,
  kcov-build.log}。
- 遗留: KCSAN 未跑；syz "连续 2 夜无可复现 crash" 的 G6 完整口径需第二夜；lockdep
  corten=on 口径复跑全套件（T1c 登记的 add_mm_counter-under-ptl preempt_nested WARN
  裁定）。

### M8 终测报告 — 本报告（骨架）
- M8.T1 全矩阵 5 次口径重跑 / M8.T2 perfetto 深度分析（G2）/ M8.T3 终稿: §8 计划。

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
g1-consol 0/4（口径作废）→ run3 1/4（动态口径，读法 A 3/4）→ **run4 2/4 MET**。

### 4.2 M4.T5 run4 正式判定（最新，全文转引 results/r06/t5-run4-report.md §1）

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

<!-- PENDING: G1 五次加测固化（unmap / unmap-virt 全线程族 / dedup 双档 / jvm，run4 §6 行动项 1-3）——加测完成后本节更新为固化读数 -->

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

**G3（真实应用）— run4 数字面 NOT MET；机制面成立**。谱系: run1 无有效 T0 数字
（present-RO 遮蔽）→ run2 tcmalloc +11.32% → run3 双档 +11.30/+16.14（G3 MET）→ run4
双档 +4.60/+6.33（正向无重叠但未到线）。run4 机制解释被 T5 判定采信: dedup 增益主要
定价于 T1c 池（run3 已入），perf1 三优化作用在 mmbench 形状的 TLB 路径，对 dedup churn
增量有限。**无一项回退**；五 app 全部 3 rep rc=0。
<!-- PENDING: dedup 双档 5 次加测定案幅度（run4 §6-3） -->

**G4（非 MM 回退 ≤5%）— run4 边缘未过（1 项超线 1.6pp）**。metis -0.88 / psearchy +3.18 /
dedup 双档正向全净；jvm -6.59% 超线（分布重叠的 spawn 噪声族）。历史: run2 的 psearchy
-40.05% 已由 g1-consol 5 轮 ABAB 定案为暖机方差（排除暖机后中位 -0.07%，apps 臂不受
D1 影响，裁决有效，results/r06/g1-consolidation.md §4）；run3 全项干净。
<!-- PENDING: jvm spawn 5 次加测定案（run4 §6-3） -->

**G5（枚举代价 lat_proc）**: M1 基线 fork/fork+exec/shell = 1298/2969/9874 µs 在档
（publish/baseline/）；MODE 臂复测未做（属 M5.T4）。
<!-- PENDING: G5 lat_proc fork/fork+exec/shell MODE 臂复测（M5.T4；论文口径允许 fork 单线程回退 ≤30%） -->

**G2（竞争消退 perfetto）/ G6（稳定性）/ G7（内存开销）/ G8（诚实性）**: G6 部分
数据点已取得（§5）；G2/G7 未启动；G8 的原始数据落盘与 3 中位纪律已全程执行。
<!-- PENDING: G2 perfetto mmap_lock_contention/fault_latency/sched_breakdown SQL 对照（M8.T2） -->
<!-- PENDING: G7 PT+metadata 内存开销 vs 论文理论上界（M6.T4） -->
<!-- PENDING: G6 syzkaller 第二夜（"连续 2 夜无可复现 crash"完整口径）+ KCSAN 短跑 -->

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
| **HEAD 802ff7551bd0（perf1 终验）** | **24/0/1 + 33/0/0 + 24/0/0 = 81 pass + 1 设计 skip, 0 fail**（×2 复跑） | results/r06/kunit-perf1-final.log |
| m5t1b 树（待提交） | 24/0/1 + **38**/0/0 + **26**/0/0 = 88 pass + 1 skip, 0 fail（普通 + lockdep 变体双口径） | results/r07/m5t1b-kunit-on5-final.log；results/r07/m5t1b-kunit-on1-lockdep.log |
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
| lmbench lat_proc | 部分 | M1 基线在档（fork 1298µs 等，publish/baseline/） | **MODE 臂复测未做（M5.T4/G5）** |
| 非 opt-in 进程零感知 | ✓ | =n 编译折叠（nm 零符号）+ corten=off 冒烟（18/26 过、8 失败全为 mode 门 EOPNOTSUPP 设计行为）+ legacy 一致性面全过（results/r06/gupfix.md §3） | 无 |

MODE-process 的登记观测差（T1c 池结构性代价，t1c-verify.md §6）: parked 期间
/proc/maps 显示为普通 PROT_NONE 匿名预约（pre-T1c 为无 VMA）→ si_code 为 ACCERR 非
MAPERR；mprotect/mlock 作用于预约范围由 ENOMEM 变为 legacy 成功/拒绝族。应用契约
（access-faults/内容干净/MAP_FIXED 复用/fork 清洁）全部保持。

---

## 7. 已知问题与遗留（分级）

### A. 阻塞 M4 收口的（M4.T5 判定线闭合所需）

| # | 问题 | 根因锚 | 建议 |
|---|---|---|---|
| A1 | G1 加测固化缺失（run4 判定基于单轮 3 中位） | run4 §6-1/2（unmap/unmap-virt 全线程族/pf 高方差族） | 5 次 ABAB 加测固化后改写 §4.3 为固化读数（估 1 夜） |
| A2 | G3 幅度跨轮收窄未定案（+16.1→+6.3） | run4 §6-3（dedup 双档 CV ≤2.7 无重叠，非方差假象） | dedup 双档 5 次加测 + 机制面复核（池命中 vs TLB 路径占比） |
| A3 | G4 jvm -6.59% 边缘超线未定案 | run4 §1（三 run 方向一致但分布重叠，CV 4.45/3.76） | jvm spawn 5 次加测；若固化仍超线，按 G8 机制级解释+偏离声明收口 |
| A4 | M5.T1b/T2' 未 commit | results/r07/m5t1b-verify.md（验证全绿，patches/r07-m5t1b.diff 在档） | review/maintainer 班次 commit+tag（估半夜） |

### B. 登记观测的（不阻塞，持续盯）

| # | 问题 | 根因锚 | 状态 |
|---|---|---|---|
| B1 | JThreadBench CFE（已修复未复现） | gupfix.md（内核根因=GUP/access_error 门提前拒绝） | 观测项，run3/run4 双证 0 命中 |
| B2 | metis_eq fork 后段（OQ-D） | 忠实 fork 闭环（run3/run4 fork_faithful +88 同值） | 观测项维持 |
| B3 | interlock/_mutex_disjoint KUnit 时序 flake | m4t12-verify §3 注（宿主多 VM 抢占） | M7"首跑容忍/复跑判定"清单 |
| B4 | zap 路径 add_mm_counter-under-ptl preempt_nested WARN | t1c-verify §8-3（仅 corten=on lockdep boot 暴露，上游同模式） | M7 门复跑裁定 |
| B5 | find_vma_intersection RCU WARN（java 高频 42 条/班） | rogue-fix §6-2（tier-2 RCU walk 合法，噪音级） | 改 lockless 变体或压掉 assert（下片） |
| B6 | pf/low 家族高方差（CV 15~79%） | perf1 §4-3（单发 5s 窗摆动 -21%~+29%） | 5 次网格口径永久登记 |
| B7 | 杂志回收列表注释"LIFO"实为 FIFO+尾块扩展；auto_route 测试 addrs[8] 封顶 | STATE r07 收口条（非阻塞备注） | 文档/测试件顺手修 |

### C. 后续切片的（设计级杠杆，非缺陷）

| # | 方向 | 根因锚 | 归属 |
|---|---|---|---|
| C1 | mmap-pf fault 通道批量化/percpu 化（残差 -8.8~-17.1 的最后一段） | perf1 §4-1/2（每 fault 一事务 + take/park 两次写锁段） | M8 fault 域重构（D19 反面账） |
| C2 | unmap/unmap-virt CHUNK 形状差（池正交） | t1c-verify §8-2 | CHUNK 快路径（视 run4 过线后优先级下调） |
| C3 | M6 rmap/swap/回收（Stage1 wired + Stage2 chunk-VMA + swap 编码） | docs/ROADMAP.md §3 M6.T1-T4 | M6（2-3 夜） |
| C4 | M9 P2/P3: arm64 热钩子接线 + contpte/BBM 决议（OQ1/P3） | ARM64_PORTING.md（publish/）；gupfix §5-3 | M9 |
| C5 | mremap_move PROT_NONE 窗 perm 取值 | gupfix §5-1（无 workload 触发） | 下片 |
| C6 | FORCE 边界: perm-RO + VM_WRITE 的 FOLL_FORCE = EIO 响亮拒绝 | m5t1b-verify §3-2 | T2''（语义裁决已登记） |
| C7 | F2 窗粒度 DONTCOPY 页级过度 SHARED（良性自愈） | m5t1b-verify §3-1 | T2''（+~10 行页粒度门可选） |
| C8 | mmbench 动态口径永久化 + meta 记 sha256 | D1 口径更正（STATE） | 已在 T5 runner 执行；M8 全矩阵沿用 |

---

## 8. 剩余工作与计划

| 序 | 工作 | 内容/DoD | 估计 |
|---|---|---|---|
| 1 | M4.T5 收口 | A1-A3 三组 5 次加测（unmap/unmap-virt 全线程族、dedup 双档、jvm）→ M4 最终判定改写；M4 里程碑判定登记 | 1 夜 |
| 2 | M5 收口 | T1b/T2' review+commit+tag（A4）；OQ-4/5/F2/F3 关闭回填 STATE | 半夜 |
| 3 | M5.T3/T4 | GUP 互操作（process_vm_readv/ptrace/io_uring 注册页）+ lat_proc/JVM MODE 臂（G5） | 1 夜 |
| 4 | M6.T1-T4 | wired 确认 → chunk-VMA rmap 可见性 + 回收/换出 PTE 写路由进事务 → swap zram 编码 → 内存开销测量（G7） | 2-3 夜 |
| 5 | M7 周期 | syzkaller 第二夜（G6 完整口径，corpus 种子续用）+ KCSAN 构建短跑 + lockdep corten=on 复跑全套件（B4 裁定） | 伴随 2 夜 |
| 6 | M8.T1 | 全矩阵重跑（EVAL §2-§4，与 M1 同参同件，CV>5% 配置 5 次） | 1 夜 |
| 7 | M8.T2 | perfetto 深度分析: mmap_lock_contention / fault_latency / sched_breakdown SQL 固化（G2） | 1 日窗 |
| 8 | M8.T3 | 本报告终稿: G1-G8 全 gate 表（过/机制解释+偏离声明双轨）、LoC 终态账（D12）、G8 诚实性声明（8vCPU 不可测清单）+ ARM64_PORTING.md 定稿 | 1 日窗 |
| 9 | （并行） | publish 仓库 push 节奏化（R9: 内核树凭据另议，不阻塞） | 伴随 |

关键路径 = 1→2→4→6→8；3/5/7 可并行插入。总计约 **6-8 个夜窗 + 2 个日窗**。

---

## 9. 附录

### 9.1 提交清单（20 个项目提交 / 19 tag；主树 android17-6.18，均未 push）

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
| 13 | ba77046c78fe | —（D15，未打 tag） | born-atomic transaction refs, GP-free drain |
| 14 | e219920b0923 | corten-r06-rogue | carry routed permissions across content drops and fork demotion (D16) |
| 15 | 025756094542 | corten-r06-m9p1 | arm64 4K-page build support and arch-neutral test macros |
| 16 | 0719bc6ae74e | corten-r06-gupfix | defer GUP and kernel-mode fault gates to arena metadata |
| 17 | 68697442097e | corten-r07-m5t1a | faithful fork (frozen window, metadata mirroring, COW) |
| 18 | de8a685370bb | corten-r07-m4t12 | per-cpu VA magazine and munmap fast path (M4.T1/T2) |
| 19 | 87383f51a3ff | corten-r07-t1c | resident arena pool for MODE processes (T1c) |
| 20 | 802ff7551bd0 | corten-r06-perf1（=HEAD） | skip TLB gathers on PTE-less windows, park flush post-downgrade |
| （待） | —（patches/r07-m5t1b.diff，5 文件 +1127/-13） | — | M5.T1b/T2'（INV7/F2/F3/FOLL_FORCE/UNSHARE），已验证待 commit |

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
  results/r06/t5-run3-report.md、**results/r06/t5-run4-report.md（最新正式判定）**；
  原始数据各 t5-run*/
- G1 加测与方差裁决: results/r06/g1-consolidation.md + results/r06/g1-consol/
- M5.T1b/T2': results/r07/m5t1b-verify.md + results/r07/m5t1b-* + results/r07/m5t1b-guest/
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
  （G2 待固化，PENDING）。

---

## 附: 本报告自检状态

- 全部 §引用的 results/publish/docs/log 路径已 `ls` 核对存在（起草时点 2026-09-19）。
- G1-G8 gate 对账: G1 run4 MET 2/4（待固化）/ G2 PENDING / G3 run4 数字面 NOT MET
  （机制解释已采信，待加测）/ G4 边缘未过 1 项（待定案）/ G5 PENDING / G6 部分
  （syz 1 夜 + lockdep ✓ + KUnit ✓；2 夜口径与 KCSAN PENDING）/ G7 PENDING / G8
  本报告口径声明已内嵌（8vCPU 方向性 D4、三口径 §4.1、DEVIATION 清单 §2.2）。
