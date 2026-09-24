# CortenMM M4.T5 中期对比报告 (MODE-process T0 vs in-boot BASE)

- 生成: results/r06/t5-run3/t5_report.md (analyze_t5.py)
- 协议: docs/EVAL.md sec1-3（权威）；M1 参数锚: publish/baseline/baseline_meta.json
- kernel: 6.18.32-g87383f51a3ff  host: syzkaller  utc: 2026-09-19T09:01:08Z
- cmdline: `console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount corten=on mitigations=off kunit.enable=0 log_buf_len=64M fsck.mode=force fsck.repair=yes`
- mmbench sha256: `38304f062d4334cbec705cd08ff25d1902bad41aa45a56d4a4c684ecb1acce1b`（M1: `038ed5b3…` 为 bzImage，二进制漂移见 meta/env.txt）
- 记录: 330 条单行 JSON（失败/不可解析 0+0）

## 1. mmbench（论文 Table 3 口径，D6 语义声明见 bench/mmbench/README.md）

| bench | cont | t | base median | t0 median | Δ%(T0 更优为正) | verdict | CV base/t0 |
|---|---|---|---|---|---|---|---|
| mmap | high | 1 | 0.204068 | 0.532468 | 160.93 | ≥ +160.93% | 3.17 / 2.53 |
| mmap | high | 2 | 0.0875341 | 0.210623 | 140.62 | ≥ +140.62% | 2.0 / 2.47 |
| mmap | high | 4 | 0.0305846 | 0.0967649 | 216.38 | ≥ +216.38% | 5.22 / 3.31 |
| mmap | high | 8 | 0.0133735 | 0.0484742 | 262.46 | ≥ +262.46% | 10.65 / 3.58 |
| mmap | high | 16 | 0.0035592 | 0.0111521 | 213.33 | ≥ +213.33% | 0.6 / 3.29 |
| mmap | low | 1 | 0.184576 | 0.492427 | 166.79 | ≥ +166.79% | 6.98 / 8.73 |
| mmap | low | 2 | 0.0898022 | 0.208504 | 132.18 | ≥ +132.18% | 1.77 / 1.25 |
| mmap | low | 4 | 0.0340413 | 0.0984183 | 189.11 | ≥ +189.11% | 3.05 / 2.3 |
| mmap | low | 8 | 0.0118412 | 0.0469192 | 296.24 | ≥ +296.24% | 0.34 / 0.3 |
| mmap | low | 16 | 0.00342605 | 0.0117073 | 241.71 | ≥ +241.71% | 8.79 / 2.14 |
| mmap-pf | high | 1 | 0.026205 | 0.0180592 | -31.08 | ≤ -31.08% | 13.68 / 8.51 |
| mmap-pf | high | 2 | 0.00390816 | 0.00250065 | -36.01 | ≤ -36.01% | 3.04 / 5.34 |
| mmap-pf | high | 4 | 0.00230244 | 0.00100908 | -56.17 | ≤ -56.17% | 16.97 / 2.19 |
| mmap-pf | high | 8 | 0.00123032 | 0.000355947 | -71.07 | ≤ -71.07% | 2.19 / 1.39 |
| mmap-pf | high | 16 | 0.000628789 | 0.000194721 | -69.03 | ≤ -69.03% | 2.56 / 1.83 |
| mmap-pf | low | 1 | 0.0265023 | 0.0192542 | -27.35 | ≤ -27.35% | 6.76 / 6.66 |
| mmap-pf | low | 2 | 0.00366848 | 0.00269352 | -26.58 | ≤ -26.58% | 1.87 / 6.32 |
| mmap-pf | low | 4 | 0.00262677 | 0.00100751 | -61.64 | ≤ -61.64% | 3.32 / 5.79 |
| mmap-pf | low | 8 | 0.00142188 | 0.000366296 | -74.24 | ≤ -74.24% | 1.89 / 2.03 |
| mmap-pf | low | 16 | 0.000561298 | 0.000188652 | -66.39 | ≤ -66.39% | 5.13 / 1.19 |
| pf | high | 1 | 0.0397638 | 0.0359635 | -9.56 | ≤ -9.56% | 5.45 / 5.91 |
| pf | high | 2 | 0.024089 | 0.0202985 | -15.74 | ≤ -15.74% | 59.57 / 58.45 |
| pf | high | 4 | 0.0415182 | 0.0364262 | -12.26 | ≤ -12.26% | 48.9 / 28.67 |
| pf | high | 8 | 0.0389964 | 0.0313843 | -19.52 | ≤ -19.52% | 4.4 / 7.63 |
| pf | high | 16 | 0.0188224 | 0.0162467 | -13.68 | ≤ -13.68% | 6.69 / 3.14 |
| pf | low | 1 | 0.00875225 | 0.0091611 | 4.67 | ≥ +4.67% | 51.71 / 14.07 |
| pf | low | 2 | 0.00965626 | 0.0296287 | 206.83 | ≥ +206.83% | 62.72 / 46.22 |
| pf | low | 4 | 0.0239905 | 0.00870687 | -63.71 | ≤ -63.71% | 54.19 / 79.13 |
| pf | low | 8 | 0.0285118 | 0.0368573 | 29.27 | ≥ +29.27% | 15.31 / 2.44 |
| pf | low | 16 | 0.027298 | 0.0244977 | -10.26 | ≤ -10.26% | 8.97 / 2.41 |
| unmap | high | 1 | 0.0629198 | 0.12511 | 98.84 | ≥ +98.84% | 6.84 / 4.22 |
| unmap | high | 2 | 0.00577718 | 0.00557556 | -3.49 | ≤ -3.49% | 2.06 / 5.73 |
| unmap | high | 4 | 0.00443843 | 0.00420589 | -5.24 | ≤ -5.24% | 12.3 / 0.64 |
| unmap | high | 8 | 0.00185009 | 0.00312916 | 69.14 | ≥ +69.14% | 4.68 / 0.67 |
| unmap | high | 16 | 0.000721508 | 0.00245038 | 239.62 | ≥ +239.62% | 2.57 / 1.24 |
| unmap | low | 1 | 0.0591442 | 0.13711 | 131.82 | ≥ +131.82% | 6.77 / 2.98 |
| unmap | low | 2 | 0.00450916 | 0.00681542 | 51.15 | ≥ +51.15% | 13.17 / 5.38 |
| unmap | low | 4 | 0.00365474 | 0.00450347 | 23.22 | ≥ +23.22% | 9.07 / 1.21 |
| unmap | low | 8 | 0.00147592 | 0.00314814 | 113.3 | ≥ +113.30% | 15.58 / 0.6 |
| unmap | low | 16 | 0.000729298 | 0.00246991 | 238.67 | ≥ +238.67% | 1.86 / 2.12 |
| unmap-virt | high | 1 | 0.250874 | 0.597235 | 138.06 | ≥ +138.06% | 8.91 / 4.14 |
| unmap-virt | high | 2 | 0.112967 | 0.010906 | -90.35 | ≤ -90.35% | 1.14 / 5.27 |
| unmap-virt | high | 4 | 0.0521332 | 0.00834146 | -84.0 | ≤ -84.00% | 6.16 / 5.24 |
| unmap-virt | high | 8 | 0.0206457 | 0.00556869 | -73.03 | ≤ -73.03% | 6.63 / 14.4 |
| unmap-virt | high | 16 | 0.00621557 | 0.00411053 | -33.87 | ≤ -33.87% | 6.07 / 2.39 |
| unmap-virt | low | 1 | 0.24063 | 0.642226 | 166.89 | ≥ +166.89% | 6.37 / 3.04 |
| unmap-virt | low | 2 | 0.092585 | 0.0119603 | -87.08 | ≤ -87.08% | 17.56 / 5.09 |
| unmap-virt | low | 4 | 0.0535747 | 0.00866377 | -83.83 | ≤ -83.83% | 6.06 / 3.87 |
| unmap-virt | low | 8 | 0.0231932 | 0.00592387 | -74.46 | ≤ -74.46% | 1.74 / 5.47 |
| unmap-virt | low | 16 | 0.00801105 | 0.00406067 | -49.31 | ≤ -49.31% | 6.83 / 3.74 |

## 2. 真实应用等价件 + JVM（PS-F7/F8；EVAL sec 3）

| workload | config | base median | t0 median | Δ%(T0 更优为正) | verdict | CV base/t0 |
|---|---|---|---|---|---|---|
| dedup_eq | t8/glibc | 4.43116e+06 | 4.93187e+06 | 11.3 | ≥ +11.30% | 0.79 / 1.51 |
| dedup_eq_tcmalloc | t8/tcmalloc | 5.02189e+06 | 5.83258e+06 | 16.14 | ≥ +16.14% | 0.64 / 0.57 |
| jvm | t2000x3 | 2434.14 | 2495.22 | -2.51 | ≤ -2.51% | 4.03 / 7.92 |
| metis_eq | t8 | 16.722 | 16.429 | 1.75 | ≥ +1.75% | 15.73 / 0.64 |
| psearchy_eq | t8 | 4.308 | 4.179 | 2.99 | ≥ +2.99% | 15.89 / 1.14 |

方向说明: metis_eq/psearchy_eq/jvm 为 lower-is-better（秒/毫秒），表中 Δ 已归一——正值一律表示 T0（MODE）更优；dedup_eq/mmbench 为 higher-is-better（ops/s per µs）。

## 3. G1 预判定（EVAL sec 6: 低竞争 t∈{4,8}，{mmap-pf,pf,unmap,unmap-virt} 中 ≥2 项（t4 与 t8 同时）中位数提升 ≥10%）

| bench | t4 Δ% | t8 Δ% | qualifies |
|---|---|---|---|
| mmap-pf | -61.64 | -74.24 | no |
| pf | -63.71 | 29.27 | no |
| unmap | 23.22 | 113.3 | YES |
| unmap-virt | -83.83 | -74.46 | no |

**G1 预判定: NOT MET (中期数据; M8 终测口径不变)**（qualifying: 1/4）

## 3b. G3 预判定（EVAL sec 6: {metis_eq, dedup_eq ptmalloc 档, JVM 线程} 中 ≥1 项 t=8 提升 ≥10% 或 trace 级机制解释）

| item | Δ%(T0 更优为正) | counts |
|---|---|---|
| dedup_eq (t8/glibc) | 11.3 | YES |
| jvm (t2000x3, M1 口径 2000 线程 spawn 窗口, 辅助) | -2.51 | aux |
| metis_eq (t8) | 1.75 | no |

**G3 预判定: MET**

## 4. strace 抽样（错误返回 multiset diff，run_t0_dod.sh trace_pair 口径，5s 窗）

| trace pair | status |
|---|---|
| mmbench_mmap_low_t1 | equal (rc 0/0) |
| mmbench_mmap_low_t2 | differs (rc 0/0) |
| mmbench_mmap_low_t4 | differs (rc 0/0) |
| mmbench_mmap_low_t8 | differs (rc 0/0) |
| mmbench_mmap_low_t16 | differs (rc 0/0) |
| mmbench_mmap_high_t1 | equal (rc 0/0) |
| mmbench_mmap_high_t2 | differs (rc 0/0) |
| mmbench_mmap_high_t4 | differs (rc 0/0) |
| mmbench_mmap_high_t8 | equal (rc 0/0) |
| mmbench_mmap_high_t16 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t1 | equal (rc 0/0) |
| mmbench_mmap-pf_low_t2 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t4 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t8 | equal (rc 0/0) |
| mmbench_mmap-pf_low_t16 | equal (rc 0/0) |
| mmbench_mmap-pf_high_t1 | equal (rc 0/0) |
| mmbench_mmap-pf_high_t2 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t4 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t8 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t16 | differs (rc 0/0) |
| mmbench_pf_low_t1 | equal (rc 0/0) |
| mmbench_pf_low_t2 | equal (rc 0/0) |
| mmbench_pf_low_t4 | differs (rc 0/0) |
| mmbench_pf_low_t8 | equal (rc 0/0) |
| mmbench_pf_low_t16 | differs (rc 0/0) |
| mmbench_pf_high_t1 | equal (rc 0/0) |
| mmbench_pf_high_t2 | differs (rc 0/0) |
| mmbench_pf_high_t4 | differs (rc 0/0) |
| mmbench_pf_high_t8 | differs (rc 0/0) |
| mmbench_pf_high_t16 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t1 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t2 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t4 | differs (rc 0/0) |
| mmbench_unmap-virt_low_t8 | differs (rc 0/0) |
| mmbench_unmap-virt_low_t16 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t1 | equal (rc 0/0) |
| mmbench_unmap-virt_high_t2 | equal (rc 0/0) |
| mmbench_unmap-virt_high_t4 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t8 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t16 | differs (rc 0/0) |
| mmbench_unmap_low_t1 | equal (rc 0/0) |
| mmbench_unmap_low_t2 | equal (rc 0/0) |
| mmbench_unmap_low_t4 | equal (rc 0/0) |
| mmbench_unmap_low_t8 | differs (rc 0/0) |
| mmbench_unmap_low_t16 | equal (rc 0/0) |
| mmbench_unmap_high_t1 | equal (rc 0/0) |
| mmbench_unmap_high_t2 | differs (rc 0/0) |
| mmbench_unmap_high_t4 | differs (rc 0/0) |
| mmbench_unmap_high_t8 | differs (rc 0/0) |
| mmbench_unmap_high_t16 | differs (rc 0/0) |
| metis_eq | equal (rc 137/137) |
| psearchy_eq | equal (rc 137/137) |
| dedup_eq | equal (rc 137/137) |
| dedup_eq_tcmalloc | differs (rc 137/137) |
| jvm | differs (rc 137/137) |

> **WARN**: 31 对 trace 在 MODE 下出现新的错误返回 —— 逐条核对 `raw/trace/<tag>.err.diff`（mmap 返回地址差异属设计内，不携带错误；此处只看 `-1 ERRNO` 对）。

## 5. M1 基线同参核对与数值对照


### 5.1 参数一致性审计（逐 raw 记录 vs M1 协议; 通过 330/330）

全部记录与 M1 协议同参（seed 公式/线程数/语料规模/时长/allocator/JVM 形参）。

### 5.2 数值对照（T5 BASE 臂 vs M1 基线中位；跨 boot 仅参考，非 gate）

| config | M1 median | T5 base median | 偏差% | 备注 |
|---|---|---|---|---|
| dedup_eq t8/glibc | 4801431 | 4.43116e+06 | -7.7% |  |
| dedup_eq_tcmalloc t8/tcmalloc | 5162927 | 5.02189e+06 | -2.7% |  |
| jvm t2000x3 | 2347.33 | 2434.14 | -3.7% |  |
| metis_eq t8 | 15.772 | 16.722 | -6.0% |  |
| mmbench mmap/high/t1 | 0.205918 | 0.204068 | -0.9% |  |
| mmbench mmap/high/t16 | 0.00379847 | 0.0035592 | -6.3% |  |
| mmbench mmap/high/t2 | 0.0825004 | 0.0875341 | +6.1% |  |
| mmbench mmap/high/t4 | 0.03629 | 0.0305846 | -15.7% |  |
| mmbench mmap/high/t8 | 0.0160272 | 0.0133735 | -16.6% |  |
| mmbench mmap/low/t1 | 0.190526 | 0.184576 | -3.1% |  |
| mmbench mmap/low/t16 | 0.00356994 | 0.00342605 | -4.0% |  |
| mmbench mmap/low/t2 | 0.0826706 | 0.0898022 | +8.6% |  |
| mmbench mmap/low/t4 | 0.0361035 | 0.0340413 | -5.7% |  |
| mmbench mmap/low/t8 | 0.0160509 | 0.0118412 | -26.2% **>25% 漂移** |  |
| mmbench mmap-pf/high/t1 | 0.0245287 | 0.026205 | +6.8% |  |
| mmbench mmap-pf/high/t16 | 0.00092601 | 0.000628789 | -32.1% **>25% 漂移** |  |
| mmbench mmap-pf/high/t2 | 0.00472667 | 0.00390816 | -17.3% |  |
| mmbench mmap-pf/high/t4 | 0.00310167 | 0.00230244 | -25.8% **>25% 漂移** |  |
| mmbench mmap-pf/high/t8 | 0.00203699 | 0.00123032 | -39.6% **>25% 漂移** |  |
| mmbench mmap-pf/low/t1 | 0.0245319 | 0.0265023 | +8.0% |  |
| mmbench mmap-pf/low/t16 | 0.000791689 | 0.000561298 | -29.1% **>25% 漂移** |  |
| mmbench mmap-pf/low/t2 | 0.00397015 | 0.00366848 | -7.6% |  |
| mmbench mmap-pf/low/t4 | 0.00335439 | 0.00262677 | -21.7% |  |
| mmbench mmap-pf/low/t8 | 0.00159097 | 0.00142188 | -10.6% |  |
| mmbench pf/high/t1 | 0.0327937 | 0.0397638 | +21.3% |  |
| mmbench pf/high/t16 | 0.0210678 | 0.0188224 | -10.7% |  |
| mmbench pf/high/t2 | 0.0215158 | 0.024089 | +12.0% |  |
| mmbench pf/high/t4 | 0.0436225 | 0.0415182 | -4.8% |  |
| mmbench pf/high/t8 | 0.0418696 | 0.0389964 | -6.9% |  |
| mmbench pf/low/t1 | 0.004185 | 0.00875225 | +109.1% **>25% 漂移** |  |
| mmbench pf/low/t16 | 0.0335499 | 0.027298 | -18.6% |  |
| mmbench pf/low/t2 | 0.0070048 | 0.00965626 | +37.9% **>25% 漂移** |  |
| mmbench pf/low/t4 | 0.00970738 | 0.0239905 | +147.1% **>25% 漂移** |  |
| mmbench pf/low/t8 | 0.038444 | 0.0285118 | -25.8% **>25% 漂移** |  |
| mmbench unmap/high/t1 | 0.0580928 | 0.0629198 | +8.3% |  |
| mmbench unmap/high/t16 | 0.00094581 | 0.000721508 | -23.7% |  |
| mmbench unmap/high/t2 | 0.00702397 | 0.00577718 | -17.8% |  |
| mmbench unmap/high/t4 | 0.00497333 | 0.00443843 | -10.8% |  |
| mmbench unmap/high/t8 | 0.00230889 | 0.00185009 | -19.9% |  |
| mmbench unmap/low/t1 | 0.0602691 | 0.0591442 | -1.9% |  |
| mmbench unmap/low/t16 | 0.000963858 | 0.000729298 | -24.3% |  |
| mmbench unmap/low/t2 | 0.00553709 | 0.00450916 | -18.6% |  |
| mmbench unmap/low/t4 | 0.00446821 | 0.00365474 | -18.2% |  |
| mmbench unmap/low/t8 | 0.00213514 | 0.00147592 | -30.9% **>25% 漂移** |  |
| mmbench unmap-virt/high/t1 | 0.218624 | 0.250874 | +14.8% |  |
| mmbench unmap-virt/high/t16 | 0.00656029 | 0.00621557 | -5.3% |  |
| mmbench unmap-virt/high/t2 | 0.126328 | 0.112967 | -10.6% |  |
| mmbench unmap-virt/high/t4 | 0.0557513 | 0.0521332 | -6.5% |  |
| mmbench unmap-virt/high/t8 | 0.025576 | 0.0206457 | -19.3% |  |
| mmbench unmap-virt/low/t1 | 0.193832 | 0.24063 | +24.1% |  |
| mmbench unmap-virt/low/t16 | 0.00699679 | 0.00801105 | +14.5% |  |
| mmbench unmap-virt/low/t2 | 0.109961 | 0.092585 | -15.8% |  |
| mmbench unmap-virt/low/t4 | 0.0531767 | 0.0535747 | +0.7% |  |
| mmbench unmap-virt/low/t8 | 0.0225235 | 0.0231932 | +3.0% |  |
| psearchy_eq t8 | 4.009 | 4.308 | -7.5% |  |

> **WARN**: 以下配置 T5 BASE 臂与 M1 基线偏差 >25%（跨 boot 噪声/不同 mitigations/宿主负载；先用 in-boot BASE vs T0 结论，再排查漂移源）。

- mmbench mmap/low/t8: base vs M1 -26.2%
- mmbench mmap-pf/high/t16: base vs M1 -32.1%
- mmbench mmap-pf/high/t4: base vs M1 -25.8%
- mmbench mmap-pf/high/t8: base vs M1 -39.6%
- mmbench mmap-pf/low/t16: base vs M1 -29.1%
- mmbench pf/low/t1: base vs M1 +109.1%
- mmbench pf/low/t2: base vs M1 +37.9%
- mmbench pf/low/t4: base vs M1 +147.1%
- mmbench pf/low/t8: base vs M1 -25.8%
- mmbench unmap/low/t8: base vs M1 -30.9%

## 6. CV 告警（EVAL sec 2.3: >5% 应加测至 5 次）

- jvm t2000x3: cv_base=4.03% cv_t0=7.92%
- metis_eq t8: cv_base=15.73% cv_t0=0.64%
- mmbench mmap/high/t4: cv_base=5.22% cv_t0=3.31%
- mmbench mmap/high/t8: cv_base=10.65% cv_t0=3.58%
- mmbench mmap/low/t1: cv_base=6.98% cv_t0=8.73%
- mmbench mmap/low/t16: cv_base=8.79% cv_t0=2.14%
- mmbench mmap-pf/high/t1: cv_base=13.68% cv_t0=8.51%
- mmbench mmap-pf/high/t2: cv_base=3.04% cv_t0=5.34%
- mmbench mmap-pf/high/t4: cv_base=16.97% cv_t0=2.19%
- mmbench mmap-pf/low/t1: cv_base=6.76% cv_t0=6.66%
- mmbench mmap-pf/low/t16: cv_base=5.13% cv_t0=1.19%
- mmbench mmap-pf/low/t2: cv_base=1.87% cv_t0=6.32%
- mmbench mmap-pf/low/t4: cv_base=3.32% cv_t0=5.79%
- mmbench pf/high/t1: cv_base=5.45% cv_t0=5.91%
- mmbench pf/high/t16: cv_base=6.69% cv_t0=3.14%
- mmbench pf/high/t2: cv_base=59.57% cv_t0=58.45%
- mmbench pf/high/t4: cv_base=48.9% cv_t0=28.67%
- mmbench pf/high/t8: cv_base=4.4% cv_t0=7.63%
- mmbench pf/low/t1: cv_base=51.71% cv_t0=14.07%
- mmbench pf/low/t16: cv_base=8.97% cv_t0=2.41%
- mmbench pf/low/t2: cv_base=62.72% cv_t0=46.22%
- mmbench pf/low/t4: cv_base=54.19% cv_t0=79.13%
- mmbench pf/low/t8: cv_base=15.31% cv_t0=2.44%
- mmbench unmap/high/t1: cv_base=6.84% cv_t0=4.22%
- mmbench unmap/high/t2: cv_base=2.06% cv_t0=5.73%
- mmbench unmap/high/t4: cv_base=12.3% cv_t0=0.64%
- mmbench unmap/low/t1: cv_base=6.77% cv_t0=2.98%
- mmbench unmap/low/t2: cv_base=13.17% cv_t0=5.38%
- mmbench unmap/low/t4: cv_base=9.07% cv_t0=1.21%
- mmbench unmap/low/t8: cv_base=15.58% cv_t0=0.6%
- mmbench unmap-virt/high/t1: cv_base=8.91% cv_t0=4.14%
- mmbench unmap-virt/high/t16: cv_base=6.07% cv_t0=2.39%
- mmbench unmap-virt/high/t2: cv_base=1.14% cv_t0=5.27%
- mmbench unmap-virt/high/t4: cv_base=6.16% cv_t0=5.24%
- mmbench unmap-virt/high/t8: cv_base=6.63% cv_t0=14.4%
- mmbench unmap-virt/low/t1: cv_base=6.37% cv_t0=3.04%
- mmbench unmap-virt/low/t16: cv_base=6.83% cv_t0=3.74%
- mmbench unmap-virt/low/t2: cv_base=17.56% cv_t0=5.09%
- mmbench unmap-virt/low/t4: cv_base=6.06% cv_t0=3.87%
- mmbench unmap-virt/low/t8: cv_base=1.74% cv_t0=5.47%
- psearchy_eq t8: cv_base=15.89% cv_t0=1.14%

## 7. 结论速览

- PARAM-MISMATCH: 0（exit 2 口径）
- 覆盖缺口: 0
- 失败/不可解析记录: 0
- strace differs: 31
- G1 预判定: NOT MET
- 报告退出码: 1（0=干净, 1=数据问题, 2=参数不一致）

