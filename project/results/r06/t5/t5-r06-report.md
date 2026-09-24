# M4.T5 首跑 —— 第一组 MODE-process on/off 性能对比（r06 夜, 2026-09-18）

- 班次: 05:30–07:57 CST（timegate 放行全程; 08:25 硬停线内收工）
- 内核: 主树 HEAD=452ad7b9d91e（tag corten-r06-m4dg2）主树重编 #27,
  6.18.32-g452ad7b9d91e, bzImage sha256 18ad5ffd…351e5（bzimg/r06-t5/, green.txt 已登记）
- boot: corten=on mitigations=off kunit.enable=0, trixie 8vCPU/4G/KVM
- 协议: bench/t5/README.md（EVAL §1-3 权威; ABAB 交错; ≥3 中位; M1 参数锚
  publish/baseline/baseline_meta.json; 参数审计 318/318 PASS, PARAM-MISMATCH=0）
- 产物: 本目录 t5run/（raw 330 条单行 JSON + trace 55 对 + meta + summary + report）,
  quick/{t5quick-hang, t5quick2}, runner-fix-r06t5.md; 快速轮 t5quick2 供 runner 健康佐证。
- 宿主声明: 共享 i9-13900HX, 测量时段 05:35–07:34 宿主无其他 qemu/构建（M9-P1 arm64
  KUnit 05:39 前已结束）; t=1/pf 组方差大见 §3。

## 1. mmbench 主对照（in-boot BASE vs T0, 3 中位, Δ 正=T0 优）

每配置 base_median vs t0_median + ratio（t0/base）+ verdict（完整 50 行见 t5_report.md §1）。
CV>5% 标 †（EVAL §2.3 加测候选）。

| bench | cont | t | base med | t0 med | ratio | Δ% | verdict |
|---|---|---|---|---|---|---|---|
| mmap | low | 4 | 0.03020 | 0.03146 | 1.042 | +4.19 | T0 优 |
| mmap | low | 8 | 0.01313 | 0.01226 | 0.934 | -6.57 | BASE 优 |
| mmap | high | 4 | 0.03011 | 0.03033 | 1.007 | +0.72 | flat |
| mmap | high | 8 | 0.01287 | 0.01278 | 0.993 | -0.72 | flat |
| mmap-pf | low | 4 † | 0.002435 | 0.002371 | 0.973 | -2.65 | BASE 优 |
| mmap-pf | low | 8 | 0.001126 | 0.001327 | 1.179 | +17.90 | T0 优 |
| mmap-pf | high | 4 † | 0.002403 | 0.002514 | 1.046 | +4.63 | T0 优 |
| mmap-pf | high | 8 | 0.001196 | 0.001222 | 1.022 | +2.16 | T0 优 |
| pf | low | 4 †† | 0.009667 | 0.03468 | 3.587 | +258.70 | T0 优†† |
| pf | low | 8 † | 0.03275 | 0.03258 | 0.995 | -0.52 | flat |
| pf | high | 4 | 0.03766 | 0.03822 | 1.015 | +1.50 | flat+ |
| pf | high | 8 | 0.03430 | 0.03672 | 1.071 | +7.07 | T0 优 |
| unmap | low | 4 † | 0.003557 | 0.003349 | 0.942 | -5.83 | BASE 优 |
| unmap | low | 8 | 0.001325 | 0.001428 | 1.078 | +7.77 | T0 优 |
| unmap | high | 4 | 0.003490 | 0.004149 | 1.189 | +18.91 | T0 优 |
| unmap | high | 8 | 0.001794 | 0.001896 | 1.057 | +5.65 | T0 优 |
| unmap-virt | low | 4 † | 0.04073 | 0.04770 | 1.171 | +17.13 | T0 优 |
| unmap-virt | low | 8 | 0.02045 | 0.02159 | 1.056 | +5.57 | T0 优 |
| unmap-virt | high | 4 | 0.04696 | 0.04693 | 0.999 | -0.06 | flat |
| unmap-virt | high | 8 | 0.02097 | 0.02107 | 1.005 | +0.46 | flat |
| mmap | low | 1 | 0.1957 | 0.1875 | 0.958 | -4.21 | BASE 优 |
| pf | low | 1 †† | 0.00994 | 0.02716 | 2.733 | +173.26 | T0 优†† |
| pf | low | 2 †† | 0.00976 | 0.03574 | 3.663 | +266.24 | T0 优†† |
| mmap-pf | high | 2 † | 0.003337 | 0.003285 | 0.984 | -1.57 | flat- |

† = CV>5%（t5_report.md §6 共 28 项）; †† = pf/low t1/t2/t4 三格 base 侧 CV
66/10/51%、t0 侧 CV 52/48/50% —— M1 已知 pf 高方差组（M1 pf low t4 三 run
0.0066/0.0097/0.039 同形）, **方向性数字不可引用**, M8 须按加测至 5 次口径重测。

### G1 预判定（低竞争 t∈{4,8} 四组中 ≥2 项 t4/t8 同时 ≥10%）

| 组 | t4 Δ% | t8 Δ% | 同时≥10%? |
|---|---|---|---|
| mmap-pf | -2.65 | +17.90 | 否 |
| pf | +258.70†† | -0.52 | 否（t8 平） |
| unmap | -5.83 | +7.77 | 否 |
| unmap-virt | +17.13 | +5.57 | 否（t8<10%） |

**G1 预判定: NOT MET（0/4; 中期数据, M8 阈值不变）**。信号方向提示: unmap-virt t4
+17.1% 与 mmap-pf t8 +17.9% 为单项过线, 但无一组成对过线; pf/low 巨幅数字被高方差
否决（base 与 M1 基线偏差 +137%~+39% 同批漂移, 见 t5_report.md §5.2）。

## 2. 真实应用等价件 + JVM

| workload | base median | t0 median | verdict | 说明 |
|---|---|---|---|---|
| metis_eq t8 | 17.07 s | n/a | n/a（T0 腿崩） | 3/3 rc=139, dmesg segfault at 0x10003c000030 error 7 in libc —— **登记的 "RW 提交区 present-RO PTE" T0b 既有形状**（r06 夜 dg2-verify 同签名） |
| psearchy_eq t8 | 3.83 s | n/a | n/a（T0 腿崩） | 3/3 rc=139, segfault at 0x100034000030 error 7 in libc.so.6 —— 同上, 同族已知形状 |
| dedup_eq t8/glibc | 4.17 M blocks/s | n/a | n/a（T0 腿崩） | 3/3 rc=139 同族; BASE 臂 vs M1 -13.2% |
| dedup_eq t8/tcmalloc | 4.56 M blocks/s | 0.372 M blocks/s | **≤ -91.84%** | 唯一完整的 T0 应用档: MODE 下 tcmalloc 供体崩溃级回退（wall 311s vs base ~30s, 两轮 QUICK/全矩阵可复现）——tcmalloc 的 mmap/madvise 高频 churn 与 MODE 事务路径共振, DEV-6/机制解释的活样本 |
| JVM t2000x3 | 2718.5 ms | n/a | n/a（T0 腿超时） | 3/3 rc=137（480s T5_LEG_TIMEOUT 超时+KILL; base 11s 跑完）。MODE 下 JVM **不崩不挂**（QUICK2 抓证: S 态 futex_wait 正常运行, 仅极慢）, 2000 线程 spawn 在 MODE 下 >480s |

### G3 预判定: NOT MET（无任何应用项可得 T0 数字; 无法走 trace 级机制解释路径——
机制被登记缺陷遮蔽）。阻塞项 = 已登记 T0b 残余（非本班新失败）; glibc 档三件全部
崩溃使 DEV-6（brk 主导留 legacy）也无法在应用级验证。**M8 前必须闭合该 PTE 形状**。

## 3. strace 抽样（55 对, 5s 窗 multiset diff）

- 严格口径（multiset 全等）: 28 equal / 27 differs。
- 27 对 differs 的构成（本班逐对核过 .err.diff）: 12 对仅同 errno 计数抖动
  （EAGAIN/ETIMEDOUT, futex 竞争 5s 截断窗采样噪声）; 14 对 t0 窗多出 EAGAIN
  （base 窗恰好 0 条——同为 futex 竞争采样差, 非 arena 语义）; 1 对 jvm t0 多出
  ESRCH（kill 中的 JVM 线程收尾 futex 竞争）。
- **零 arena 语义新错误**: 无 EACCES/EFAULT/EINVAL/EOPNOTSUPP/ENOMEM 类新形状。
- 结论: 在"错误返回不得新增（类别）"的机制语义口径下干净; 严格 multiset 口径的
  27 对差异全部可归因 futex 竞争采样, M8 建议窗长加倍或以错误类别集为判据。

## 4. M1 跨 boot 对照（参考非 gate; t5_report.md §5.2）

BASE 臂 vs M1 基线: 多数 ±20% 内; 15 项 >25% 漂移（集中在 mmap-pf/pf/unmap 组,
与 pf 高方差同源 + M1 基线 boot 的宿主状态差异）。结论一律以 in-boot BASE vs T0
为准（本报告 §1-2 即 in-boot 口径）。

## 5. QUICK 与全矩阵一致性观察

- QUICK 首跑（t5quick）暴露 runner 两缺陷已修（runner-fix-r06t5.md）:
  ①无腿级超时 → MODE JVM 挂死腿阻塞 driver; ②JVM 9p 退路楔死
  （v9fs_evict_inode D 态, SIGKILL 免疫, 证据 quick/v9fs-wedge-evidence.txt）。
  修后 selftest 19/0, t5quick2 + 全矩阵验证通过（JVM 腿本地化后不崩不挂可超时收敛）。
- 数字一致性: dedup tcmalloc t0 wall 两轮 QUICK 308s/311s vs 全矩阵 3 腿
  ~282-311s, 高度可复现; app 崩溃签名两轮逐字同地址族; pf/low 方差形状同 M1 登记。
- QUICK={1,4,8}×1 rep 按协议不得对外引用（COVERAGE-GAP 属预期）, 仅作 runner 健康。

## 6. 已知失败项核对（按登记口径, 不算新失败）

- metis_eq/psearchy_eq/dedup_eq T0 腿 SIGSEGV rc=139: 与 r06 夜 dg2-verify 登记
  的 "RW 提交区装 present-RO PTE"（libc 形状, error 7）逐字同族 → T0b 既有 ✓
- java: 本轮 JVM MODE 下形态变为"极慢可超时"（非 r05 的 CDS ACCERR——D-G'' 修复
  后已消除）, 归登记的 libc 残余族; fork 后 ACCERR（OQ-D）本轮未复测（JVM 腿无 fork
  探针; 归规划者口径不变）
- 新登记（非失败, 数据点）: MODE 下 tcmalloc 档 -91.8% 与 JVM 2000 线程 spawn
  >480s —— MODE 高频 mmap/madvise 路径开销的两个真实信号, 归 DEV-6 机制解释与
  M5/规划者议题。

## 7. 时间线与合规

05:30 起（杀残留 VM→主树重编→green 门 4/4→green.txt 登记→QUICK→修 runner→全矩阵
06:35–07:34→分析 07:38→归档）。全程 timegate 放行; 未 push、未动 worktree（bench/t5
两脚本修复为协议允许的 runner 修复, bench/ 可写）; 密码未落盘。VM 按 ≥07:30 关闭。

*遗留: G1/G3 均 NOT MET 为中期预判定, 不改 M8 阈值; M8 前提: 闭合 T0b 残余
PTE 形状 → 重测 apps/JVM 腿; pf/low 组按 5 次加测; strace 窗长/判据口径建议。*
