# CortenMM M4.T5 中期对比报告 (MODE-process T0 vs in-boot BASE)

- 生成: /home/ppw/cortenmm/results/r06/t5-run4/t5_report.md (analyze_t5.py)
- 协议: docs/EVAL.md sec1-3（权威）；M1 参数锚: publish/baseline/baseline_meta.json
- kernel: 6.18.32-g802ff7551bd0  host: syzkaller  utc: 2026-09-19T11:24:13Z
- cmdline: `console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount corten=on mitigations=off kunit.enable=0 log_buf_len=64M fsck.mode=force fsck.repair=yes`
- mmbench sha256: `38304f062d4334cbec705cd08ff25d1902bad41aa45a56d4a4c684ecb1acce1b`（M1: `038ed5b3…` 为 bzImage，二进制漂移见 meta/env.txt）
- 记录: 330 条单行 JSON（失败/不可解析 0+0）

## 1. mmbench（论文 Table 3 口径，D6 语义声明见 bench/mmbench/README.md）

| bench | cont | t | base median | t0 median | Δ%(T0 更优为正) | verdict | CV base/t0 |
|---|---|---|---|---|---|---|---|
| mmap | high | 1 | 0.216159 | 0.538063 | 148.92 | ≥ +148.92% | 4.75 / 6.88 |
| mmap | high | 2 | 0.0961738 | 0.212501 | 120.96 | ≥ +120.96% | 3.08 / 6.09 |
| mmap | high | 4 | 0.033996 | 0.0933149 | 174.49 | ≥ +174.49% | 3.18 / 2.07 |
| mmap | high | 8 | 0.0119582 | 0.045143 | 277.51 | ≥ +277.51% | 2.98 / 0.31 |
| mmap | high | 16 | 0.00357727 | 0.0103838 | 190.27 | ≥ +190.27% | 0.69 / 14.55 |
| mmap | low | 1 | 0.1627 | 0.432316 | 165.71 | ≥ +165.71% | 1.21 / 7.43 |
| mmap | low | 2 | 0.0918889 | 0.201741 | 119.55 | ≥ +119.55% | 2.4 / 3.33 |
| mmap | low | 4 | 0.0376313 | 0.0943003 | 150.59 | ≥ +150.59% | 2.16 / 2.6 |
| mmap | low | 8 | 0.0127674 | 0.043635 | 241.77 | ≥ +241.77% | 6.87 / 7.71 |
| mmap | low | 16 | 0.00346432 | 0.0101605 | 193.29 | ≥ +193.29% | 3.3 / 4.62 |
| mmap-pf | high | 1 | 0.0284078 | 0.0222243 | -21.77 | ≤ -21.77% | 2.25 / 7.12 |
| mmap-pf | high | 2 | 0.00383669 | 0.0037934 | -1.13 | ≤ -1.13% | 1.3 / 3.88 |
| mmap-pf | high | 4 | 0.00259699 | 0.00236953 | -8.76 | ≤ -8.76% | 10.25 / 1.57 |
| mmap-pf | high | 8 | 0.00113214 | 0.00108127 | -4.49 | ≤ -4.49% | 8.29 / 3.31 |
| mmap-pf | high | 16 | 0.000608617 | 0.000505518 | -16.94 | ≤ -16.94% | 5.04 / 1.56 |
| mmap-pf | low | 1 | 0.0276915 | 0.0214403 | -22.57 | ≤ -22.57% | 3.34 / 3.03 |
| mmap-pf | low | 2 | 0.00359921 | 0.00364302 | 1.22 | ≥ +1.22% | 1.45 / 3.22 |
| mmap-pf | low | 4 | 0.00249902 | 0.00228022 | -8.76 | ≤ -8.76% | 3.15 / 1.02 |
| mmap-pf | low | 8 | 0.00126454 | 0.00104842 | -17.09 | ≤ -17.09% | 3.37 / 5.11 |
| mmap-pf | low | 16 | 0.000653988 | 0.000536999 | -17.89 | ≤ -17.89% | 4.19 / 1.21 |
| pf | high | 1 | 0.0395088 | 0.0378304 | -4.25 | ≤ -4.25% | 5.72 / 2.03 |
| pf | high | 2 | 0.032436 | 0.0381337 | 17.57 | ≥ +17.57% | 21.4 / 14.16 |
| pf | high | 4 | 0.0469242 | 0.0409093 | -12.82 | ≤ -12.82% | 4.26 / 5.9 |
| pf | high | 8 | 0.0198936 | 0.0326358 | 64.05 | ≥ +64.05% | 36.3 / 58.76 |
| pf | high | 16 | 0.0207811 | 0.0173365 | -16.58 | ≤ -16.58% | 5.2 / 3.35 |
| pf | low | 1 | 0.0196303 | 0.00972597 | -50.45 | ≤ -50.45% | 39.85 / 77.09 |
| pf | low | 2 | 0.0355682 | 0.0112064 | -68.49 | ≤ -68.49% | 53.04 / 25.29 |
| pf | low | 4 | 0.0198505 | 0.0272024 | 37.04 | ≥ +37.04% | 57.32 / 26.71 |
| pf | low | 8 | 0.031488 | 0.0284503 | -9.65 | ≤ -9.65% | 59.35 / 57.61 |
| pf | low | 16 | 0.028587 | 0.0278891 | -2.44 | ≤ -2.44% | 3.11 / 7.8 |
| unmap | high | 1 | 0.0574287 | 0.11499 | 100.23 | ≥ +100.23% | 12.37 / 4.17 |
| unmap | high | 2 | 0.00586029 | 0.00468374 | -20.08 | ≤ -20.08% | 6.45 / 15.81 |
| unmap | high | 4 | 0.00399567 | 0.00409615 | 2.51 | ≥ +2.51% | 0.36 / 3.47 |
| unmap | high | 8 | 0.0017784 | 0.0033529 | 88.53 | ≥ +88.53% | 7.03 / 0.79 |
| unmap | high | 16 | 0.000792501 | 0.00170268 | 114.85 | ≥ +114.85% | 6.66 / 1.99 |
| unmap | low | 1 | 0.0593653 | 0.121033 | 103.88 | ≥ +103.88% | 9.67 / 3.47 |
| unmap | low | 2 | 0.00426185 | 0.00650602 | 52.66 | ≥ +52.66% | 17.21 / 1.99 |
| unmap | low | 4 | 0.00348494 | 0.00465271 | 33.51 | ≥ +33.51% | 10.62 / 2.87 |
| unmap | low | 8 | 0.00143463 | 0.00349298 | 143.48 | ≥ +143.48% | 2.23 / 2.2 |
| unmap | low | 16 | 0.000662085 | 0.00177017 | 167.36 | ≥ +167.36% | 4.47 / 2.05 |
| unmap-virt | high | 1 | 0.239028 | 0.643477 | 169.21 | ≥ +169.21% | 1.93 / 0.72 |
| unmap-virt | high | 2 | 0.106565 | 0.575609 | 440.15 | ≥ +440.15% | 0.89 / 0.72 |
| unmap-virt | high | 4 | 0.0483594 | 0.543764 | 1024.42 | ≥ +1024.42% | 3.79 / 1.56 |
| unmap-virt | high | 8 | 0.020344 | 0.495614 | 2336.17 | ≥ +2336.17% | 7.77 / 3.46 |
| unmap-virt | high | 16 | 0.00645752 | 0.460726 | 7034.72 | ≥ +7034.72% | 4.22 / 2.22 |
| unmap-virt | low | 1 | 0.213228 | 0.746209 | 249.96 | ≥ +249.96% | 4.94 / 1.4 |
| unmap-virt | low | 2 | 0.101041 | 0.706254 | 598.98 | ≥ +598.98% | 12.65 / 3.04 |
| unmap-virt | low | 4 | 0.0590912 | 0.742252 | 1156.11 | ≥ +1156.11% | 7.3 / 3.7 |
| unmap-virt | low | 8 | 0.0219892 | 0.635897 | 2791.86 | ≥ +2791.86% | 3.47 / 3.32 |
| unmap-virt | low | 16 | 0.00788315 | 0.574652 | 7189.62 | ≥ +7189.62% | 2.38 / 6.49 |

## 2. 真实应用等价件 + JVM（PS-F7/F8；EVAL sec 3）

| workload | config | base median | t0 median | Δ%(T0 更优为正) | verdict | CV base/t0 |
|---|---|---|---|---|---|---|
| dedup_eq | t8/glibc | 4.41912e+06 | 4.62246e+06 | 4.6 | ≥ +4.60% | 1.04 / 1.13 |
| dedup_eq_tcmalloc | t8/tcmalloc | 4.9086e+06 | 5.21922e+06 | 6.33 | ≥ +6.33% | 0.16 / 2.69 |
| jvm | t2000x3 | 2063.92 | 2199.84 | -6.59 | ≤ -6.59% | 4.45 / 3.76 |
| metis_eq | t8 | 16.621 | 16.767 | -0.88 | ~ -0.88% (flat) | 8.91 / 1.27 |
| psearchy_eq | t8 | 4.434 | 4.293 | 3.18 | ≥ +3.18% | 14.57 / 3.5 |

方向说明: metis_eq/psearchy_eq/jvm 为 lower-is-better（秒/毫秒），表中 Δ 已归一——正值一律表示 T0（MODE）更优；dedup_eq/mmbench 为 higher-is-better（ops/s per µs）。

## 3. G1 预判定（EVAL sec 6: 低竞争 t∈{4,8}，{mmap-pf,pf,unmap,unmap-virt} 中 ≥2 项（t4 与 t8 同时）中位数提升 ≥10%）

| bench | t4 Δ% | t8 Δ% | qualifies |
|---|---|---|---|
| mmap-pf | -8.76 | -17.09 | no |
| pf | 37.04 | -9.65 | no |
| unmap | 33.51 | 143.48 | YES |
| unmap-virt | 1156.11 | 2791.86 | YES |

**G1 预判定: MET**（qualifying: 2/4）

## 3b. G3 预判定（EVAL sec 6: {metis_eq, dedup_eq ptmalloc 档, JVM 线程} 中 ≥1 项 t=8 提升 ≥10% 或 trace 级机制解释）

| item | Δ%(T0 更优为正) | counts |
|---|---|---|
| dedup_eq (t8/glibc) | 4.6 | no |
| jvm (t2000x3, M1 口径 2000 线程 spawn 窗口, 辅助) | -6.59 | aux |
| metis_eq (t8) | -0.88 | no |

**G3 预判定: NOT MET (如为负/平: 先查 glibc brk 主导=DEV-6, tcmalloc 档对照定位)**

## 4. strace 抽样（错误返回 multiset diff，run_t0_dod.sh trace_pair 口径，5s 窗）

| trace pair | status |
|---|---|
| mmbench_mmap_low_t1 | equal (rc 0/0) |
| mmbench_mmap_low_t2 | differs (rc 0/0) |
| mmbench_mmap_low_t4 | differs (rc 0/0) |
| mmbench_mmap_low_t8 | equal (rc 0/0) |
| mmbench_mmap_low_t16 | differs (rc 0/0) |
| mmbench_mmap_high_t1 | equal (rc 0/0) |
| mmbench_mmap_high_t2 | differs (rc 0/0) |
| mmbench_mmap_high_t4 | differs (rc 0/0) |
| mmbench_mmap_high_t8 | differs (rc 0/0) |
| mmbench_mmap_high_t16 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t1 | equal (rc 0/0) |
| mmbench_mmap-pf_low_t2 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t4 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t8 | equal (rc 0/0) |
| mmbench_mmap-pf_low_t16 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t1 | equal (rc 0/0) |
| mmbench_mmap-pf_high_t2 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t4 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t8 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t16 | differs (rc 0/0) |
| mmbench_pf_low_t1 | equal (rc 0/0) |
| mmbench_pf_low_t2 | differs (rc 0/0) |
| mmbench_pf_low_t4 | equal (rc 0/0) |
| mmbench_pf_low_t8 | differs (rc 0/0) |
| mmbench_pf_low_t16 | equal (rc 0/0) |
| mmbench_pf_high_t1 | equal (rc 0/0) |
| mmbench_pf_high_t2 | equal (rc 0/0) |
| mmbench_pf_high_t4 | differs (rc 0/0) |
| mmbench_pf_high_t8 | differs (rc 0/0) |
| mmbench_pf_high_t16 | differs (rc 0/0) |
| mmbench_unmap-virt_low_t1 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t2 | differs (rc 0/0) |
| mmbench_unmap-virt_low_t4 | differs (rc 0/0) |
| mmbench_unmap-virt_low_t8 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t16 | equal (rc 0/0) |
| mmbench_unmap-virt_high_t1 | equal (rc 0/0) |
| mmbench_unmap-virt_high_t2 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t4 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t8 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t16 | differs (rc 0/0) |
| mmbench_unmap_low_t1 | equal (rc 0/0) |
| mmbench_unmap_low_t2 | differs (rc 0/0) |
| mmbench_unmap_low_t4 | equal (rc 0/0) |
| mmbench_unmap_low_t8 | differs (rc 0/0) |
| mmbench_unmap_low_t16 | differs (rc 0/0) |
| mmbench_unmap_high_t1 | differs (rc 0/0) |
| mmbench_unmap_high_t2 | equal (rc 0/0) |
| mmbench_unmap_high_t4 | differs (rc 0/0) |
| mmbench_unmap_high_t8 | differs (rc 0/0) |
| mmbench_unmap_high_t16 | differs (rc 0/0) |
| metis_eq | equal (rc 137/137) |
| psearchy_eq | equal (rc 137/137) |
| dedup_eq | equal (rc 137/137) |
| dedup_eq_tcmalloc | differs (rc 137/137) |
| jvm | differs (rc 137/137) |

> **WARN**: 34 对 trace 在 MODE 下出现新的错误返回 —— 逐条核对 `raw/trace/<tag>.err.diff`（mmap 返回地址差异属设计内，不携带错误；此处只看 `-1 ERRNO` 对）。

## 5. M1 基线同参核对与数值对照


### 5.1 参数一致性审计（逐 raw 记录 vs M1 协议; 通过 330/330）

全部记录与 M1 协议同参（seed 公式/线程数/语料规模/时长/allocator/JVM 形参）。

### 5.2 数值对照（T5 BASE 臂 vs M1 基线中位；跨 boot 仅参考，非 gate）

| config | M1 median | T5 base median | 偏差% | 备注 |
|---|---|---|---|---|
| dedup_eq t8/glibc | 4801431 | 4.41912e+06 | -8.0% |  |
| dedup_eq_tcmalloc t8/tcmalloc | 5162927 | 4.9086e+06 | -4.9% |  |
| jvm t2000x3 | 2347.33 | 2063.92 | +12.1% |  |
| metis_eq t8 | 15.772 | 16.621 | -5.4% |  |
| mmbench mmap/high/t1 | 0.205918 | 0.216159 | +5.0% |  |
| mmbench mmap/high/t16 | 0.00379847 | 0.00357727 | -5.8% |  |
| mmbench mmap/high/t2 | 0.0825004 | 0.0961738 | +16.6% |  |
| mmbench mmap/high/t4 | 0.03629 | 0.033996 | -6.3% |  |
| mmbench mmap/high/t8 | 0.0160272 | 0.0119582 | -25.4% **>25% 漂移** |  |
| mmbench mmap/low/t1 | 0.190526 | 0.1627 | -14.6% |  |
| mmbench mmap/low/t16 | 0.00356994 | 0.00346432 | -3.0% |  |
| mmbench mmap/low/t2 | 0.0826706 | 0.0918889 | +11.2% |  |
| mmbench mmap/low/t4 | 0.0361035 | 0.0376313 | +4.2% |  |
| mmbench mmap/low/t8 | 0.0160509 | 0.0127674 | -20.5% |  |
| mmbench mmap-pf/high/t1 | 0.0245287 | 0.0284078 | +15.8% |  |
| mmbench mmap-pf/high/t16 | 0.00092601 | 0.000608617 | -34.3% **>25% 漂移** |  |
| mmbench mmap-pf/high/t2 | 0.00472667 | 0.00383669 | -18.8% |  |
| mmbench mmap-pf/high/t4 | 0.00310167 | 0.00259699 | -16.3% |  |
| mmbench mmap-pf/high/t8 | 0.00203699 | 0.00113214 | -44.4% **>25% 漂移** |  |
| mmbench mmap-pf/low/t1 | 0.0245319 | 0.0276915 | +12.9% |  |
| mmbench mmap-pf/low/t16 | 0.000791689 | 0.000653988 | -17.4% |  |
| mmbench mmap-pf/low/t2 | 0.00397015 | 0.00359921 | -9.3% |  |
| mmbench mmap-pf/low/t4 | 0.00335439 | 0.00249902 | -25.5% **>25% 漂移** |  |
| mmbench mmap-pf/low/t8 | 0.00159097 | 0.00126454 | -20.5% |  |
| mmbench pf/high/t1 | 0.0327937 | 0.0395088 | +20.5% |  |
| mmbench pf/high/t16 | 0.0210678 | 0.0207811 | -1.4% |  |
| mmbench pf/high/t2 | 0.0215158 | 0.032436 | +50.8% **>25% 漂移** |  |
| mmbench pf/high/t4 | 0.0436225 | 0.0469242 | +7.6% |  |
| mmbench pf/high/t8 | 0.0418696 | 0.0198936 | -52.5% **>25% 漂移** |  |
| mmbench pf/low/t1 | 0.004185 | 0.0196303 | +369.1% **>25% 漂移** |  |
| mmbench pf/low/t16 | 0.0335499 | 0.028587 | -14.8% |  |
| mmbench pf/low/t2 | 0.0070048 | 0.0355682 | +407.8% **>25% 漂移** |  |
| mmbench pf/low/t4 | 0.00970738 | 0.0198505 | +104.5% **>25% 漂移** |  |
| mmbench pf/low/t8 | 0.038444 | 0.031488 | -18.1% |  |
| mmbench unmap/high/t1 | 0.0580928 | 0.0574287 | -1.1% |  |
| mmbench unmap/high/t16 | 0.00094581 | 0.000792501 | -16.2% |  |
| mmbench unmap/high/t2 | 0.00702397 | 0.00586029 | -16.6% |  |
| mmbench unmap/high/t4 | 0.00497333 | 0.00399567 | -19.7% |  |
| mmbench unmap/high/t8 | 0.00230889 | 0.0017784 | -23.0% |  |
| mmbench unmap/low/t1 | 0.0602691 | 0.0593653 | -1.5% |  |
| mmbench unmap/low/t16 | 0.000963858 | 0.000662085 | -31.3% **>25% 漂移** |  |
| mmbench unmap/low/t2 | 0.00553709 | 0.00426185 | -23.0% |  |
| mmbench unmap/low/t4 | 0.00446821 | 0.00348494 | -22.0% |  |
| mmbench unmap/low/t8 | 0.00213514 | 0.00143463 | -32.8% **>25% 漂移** |  |
| mmbench unmap-virt/high/t1 | 0.218624 | 0.239028 | +9.3% |  |
| mmbench unmap-virt/high/t16 | 0.00656029 | 0.00645752 | -1.6% |  |
| mmbench unmap-virt/high/t2 | 0.126328 | 0.106565 | -15.6% |  |
| mmbench unmap-virt/high/t4 | 0.0557513 | 0.0483594 | -13.3% |  |
| mmbench unmap-virt/high/t8 | 0.025576 | 0.020344 | -20.5% |  |
| mmbench unmap-virt/low/t1 | 0.193832 | 0.213228 | +10.0% |  |
| mmbench unmap-virt/low/t16 | 0.00699679 | 0.00788315 | +12.7% |  |
| mmbench unmap-virt/low/t2 | 0.109961 | 0.101041 | -8.1% |  |
| mmbench unmap-virt/low/t4 | 0.0531767 | 0.0590912 | +11.1% |  |
| mmbench unmap-virt/low/t8 | 0.0225235 | 0.0219892 | -2.4% |  |
| psearchy_eq t8 | 4.009 | 4.434 | -10.6% |  |

> **WARN**: 以下配置 T5 BASE 臂与 M1 基线偏差 >25%（跨 boot 噪声/不同 mitigations/宿主负载；先用 in-boot BASE vs T0 结论，再排查漂移源）。

- mmbench mmap/high/t8: base vs M1 -25.4%
- mmbench mmap-pf/high/t16: base vs M1 -34.3%
- mmbench mmap-pf/high/t8: base vs M1 -44.4%
- mmbench mmap-pf/low/t4: base vs M1 -25.5%
- mmbench pf/high/t2: base vs M1 +50.8%
- mmbench pf/high/t8: base vs M1 -52.5%
- mmbench pf/low/t1: base vs M1 +369.1%
- mmbench pf/low/t2: base vs M1 +407.8%
- mmbench pf/low/t4: base vs M1 +104.5%
- mmbench unmap/low/t16: base vs M1 -31.3%
- mmbench unmap/low/t8: base vs M1 -32.8%

## 6. CV 告警（EVAL sec 2.3: >5% 应加测至 5 次）

- metis_eq t8: cv_base=8.91% cv_t0=1.27%
- mmbench mmap/high/t1: cv_base=4.75% cv_t0=6.88%
- mmbench mmap/high/t16: cv_base=0.69% cv_t0=14.55%
- mmbench mmap/high/t2: cv_base=3.08% cv_t0=6.09%
- mmbench mmap/low/t1: cv_base=1.21% cv_t0=7.43%
- mmbench mmap/low/t8: cv_base=6.87% cv_t0=7.71%
- mmbench mmap-pf/high/t1: cv_base=2.25% cv_t0=7.12%
- mmbench mmap-pf/high/t16: cv_base=5.04% cv_t0=1.56%
- mmbench mmap-pf/high/t4: cv_base=10.25% cv_t0=1.57%
- mmbench mmap-pf/high/t8: cv_base=8.29% cv_t0=3.31%
- mmbench mmap-pf/low/t8: cv_base=3.37% cv_t0=5.11%
- mmbench pf/high/t1: cv_base=5.72% cv_t0=2.03%
- mmbench pf/high/t16: cv_base=5.2% cv_t0=3.35%
- mmbench pf/high/t2: cv_base=21.4% cv_t0=14.16%
- mmbench pf/high/t4: cv_base=4.26% cv_t0=5.9%
- mmbench pf/high/t8: cv_base=36.3% cv_t0=58.76%
- mmbench pf/low/t1: cv_base=39.85% cv_t0=77.09%
- mmbench pf/low/t16: cv_base=3.11% cv_t0=7.8%
- mmbench pf/low/t2: cv_base=53.04% cv_t0=25.29%
- mmbench pf/low/t4: cv_base=57.32% cv_t0=26.71%
- mmbench pf/low/t8: cv_base=59.35% cv_t0=57.61%
- mmbench unmap/high/t1: cv_base=12.37% cv_t0=4.17%
- mmbench unmap/high/t16: cv_base=6.66% cv_t0=1.99%
- mmbench unmap/high/t2: cv_base=6.45% cv_t0=15.81%
- mmbench unmap/high/t8: cv_base=7.03% cv_t0=0.79%
- mmbench unmap/low/t1: cv_base=9.67% cv_t0=3.47%
- mmbench unmap/low/t2: cv_base=17.21% cv_t0=1.99%
- mmbench unmap/low/t4: cv_base=10.62% cv_t0=2.87%
- mmbench unmap-virt/high/t8: cv_base=7.77% cv_t0=3.46%
- mmbench unmap-virt/low/t16: cv_base=2.38% cv_t0=6.49%
- mmbench unmap-virt/low/t2: cv_base=12.65% cv_t0=3.04%
- mmbench unmap-virt/low/t4: cv_base=7.3% cv_t0=3.7%
- psearchy_eq t8: cv_base=14.57% cv_t0=3.5%

## 7. 结论速览

- PARAM-MISMATCH: 0（exit 2 口径）
- 覆盖缺口: 0
- 失败/不可解析记录: 0
- strace differs: 34
- G1 预判定: MET
- 报告退出码: 1（0=干净, 1=数据问题, 2=参数不一致）

