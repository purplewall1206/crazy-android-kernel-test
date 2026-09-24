# CortenMM M4.T5 中期对比报告 (MODE-process T0 vs in-boot BASE)

- 生成: /home/ppw/bench/share/t5run/t5_report.md (analyze_t5.py)
- 协议: docs/EVAL.md sec1-3（权威）；M1 参数锚: publish/baseline/baseline_meta.json
- kernel: 6.18.32-g452ad7b9d91e  host: syzkaller  utc: 2026-09-17T22:35:37Z
- cmdline: `console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount corten=on mitigations=off kunit.enable=0`
- mmbench sha256: `2a9c066c6e5fe6970f31c40aa1a1df75a61314313c6fba35ee0e10610797bf6f`（M1: `038ed5b3…` 为 bzImage，二进制漂移见 meta/env.txt）
- 记录: 330 条单行 JSON（失败/不可解析 12+0）

## 1. mmbench（论文 Table 3 口径，D6 语义声明见 bench/mmbench/README.md）

| bench | cont | t | base median | t0 median | Δ%(T0 更优为正) | verdict | CV base/t0 |
|---|---|---|---|---|---|---|---|
| mmap | high | 1 | 0.1931 | 0.197209 | 2.13 | ≥ +2.13% | 2.24 / 2.45 |
| mmap | high | 2 | 0.0706599 | 0.0761561 | 7.78 | ≥ +7.78% | 2.68 / 5.76 |
| mmap | high | 4 | 0.0301133 | 0.0303288 | 0.72 | ~ +0.72% (flat) | 1.37 / 1.62 |
| mmap | high | 8 | 0.0128697 | 0.0127776 | -0.72 | ~ -0.72% (flat) | 3.06 / 2.84 |
| mmap | high | 16 | 0.00335073 | 0.00335499 | 0.13 | ~ +0.13% (flat) | 2.54 / 5.04 |
| mmap | low | 1 | 0.195713 | 0.18747 | -4.21 | ≤ -4.21% | 1.73 / 3.76 |
| mmap | low | 2 | 0.0700478 | 0.0705405 | 0.7 | ~ +0.70% (flat) | 1.76 / 0.25 |
| mmap | low | 4 | 0.0301956 | 0.0314596 | 4.19 | ≥ +4.19% | 1.93 / 1.74 |
| mmap | low | 8 | 0.0131267 | 0.0122649 | -6.57 | ≤ -6.57% | 5.62 / 3.82 |
| mmap | low | 16 | 0.00329457 | 0.00342719 | 4.03 | ≥ +4.03% | 2.4 / 4.89 |
| mmap-pf | high | 1 | 0.0247248 | 0.0231722 | -6.28 | ≤ -6.28% | 4.35 / 2.24 |
| mmap-pf | high | 2 | 0.00333738 | 0.0032849 | -1.57 | ≤ -1.57% | 5.71 / 5.74 |
| mmap-pf | high | 4 | 0.00240294 | 0.00251426 | 4.63 | ≥ +4.63% | 6.02 / 4.29 |
| mmap-pf | high | 8 | 0.001196 | 0.00122184 | 2.16 | ≥ +2.16% | 2.67 / 11.35 |
| mmap-pf | high | 16 | 0.000586924 | 0.000592584 | 0.96 | ~ +0.96% (flat) | 3.45 / 1.77 |
| mmap-pf | low | 1 | 0.0232743 | 0.0209065 | -10.17 | ≤ -10.17% | 12.57 / 40.47 |
| mmap-pf | low | 2 | 0.00314483 | 0.00307859 | -2.11 | ≤ -2.11% | 6.6 / 3.56 |
| mmap-pf | low | 4 | 0.00243524 | 0.0023706 | -2.65 | ≤ -2.65% | 7.92 / 3.44 |
| mmap-pf | low | 8 | 0.00112565 | 0.00132716 | 17.9 | ≥ +17.90% | 2.81 / 3.91 |
| mmap-pf | low | 16 | 0.00055312 | 0.000609113 | 10.12 | ≥ +10.12% | 6.07 / 2.01 |
| pf | high | 1 | 0.0367554 | 0.039557 | 7.62 | ≥ +7.62% | 2.18 / 2.33 |
| pf | high | 2 | 0.0360916 | 0.0397363 | 10.1 | ≥ +10.10% | 19.12 / 1.1 |
| pf | high | 4 | 0.037656 | 0.0382198 | 1.5 | ≥ +1.50% | 3.73 / 3.55 |
| pf | high | 8 | 0.0343003 | 0.0367244 | 7.07 | ≥ +7.07% | 3.58 / 1.42 |
| pf | high | 16 | 0.0177507 | 0.0179322 | 1.02 | ≥ +1.02% | 2.9 / 4.1 |
| pf | low | 1 | 0.00993873 | 0.0271585 | 173.26 | ≥ +173.26% | 65.99 / 52.07 |
| pf | low | 2 | 0.00975955 | 0.0357435 | 266.24 | ≥ +266.24% | 10.46 / 47.88 |
| pf | low | 4 | 0.00966727 | 0.0346769 | 258.7 | ≥ +258.70% | 50.94 / 50.14 |
| pf | low | 8 | 0.0327547 | 0.032584 | -0.52 | ~ -0.52% (flat) | 16.66 / 3.75 |
| pf | low | 16 | 0.0233141 | 0.0262061 | 12.4 | ≥ +12.40% | 4.18 / 6.8 |
| unmap | high | 1 | 0.0586808 | 0.0611137 | 4.15 | ≥ +4.15% | 1.36 / 0.94 |
| unmap | high | 2 | 0.00590917 | 0.00579672 | -1.9 | ≤ -1.90% | 4.15 / 2.42 |
| unmap | high | 4 | 0.00348951 | 0.00414929 | 18.91 | ≥ +18.91% | 3.78 / 2.54 |
| unmap | high | 8 | 0.0017942 | 0.00189561 | 5.65 | ≥ +5.65% | 2.75 / 8.11 |
| unmap | high | 16 | 0.00086225 | 0.000762422 | -11.58 | ≤ -11.58% | 7.17 / 2.71 |
| unmap | low | 1 | 0.0595444 | 0.061062 | 2.55 | ≥ +2.55% | 9.56 / 9.35 |
| unmap | low | 2 | 0.00410304 | 0.00437566 | 6.64 | ≥ +6.64% | 17.31 / 18.63 |
| unmap | low | 4 | 0.00355672 | 0.00334929 | -5.83 | ≤ -5.83% | 16.87 / 13.36 |
| unmap | low | 8 | 0.00132533 | 0.0014283 | 7.77 | ≥ +7.77% | 4.03 / 2.76 |
| unmap | low | 16 | 0.00068378 | 0.000727521 | 6.4 | ≥ +6.40% | 0.24 / 4.81 |
| unmap-virt | high | 1 | 0.245491 | 0.235401 | -4.11 | ≤ -4.11% | 4.09 / 5.45 |
| unmap-virt | high | 2 | 0.10445 | 0.106074 | 1.55 | ≥ +1.55% | 0.91 / 1.77 |
| unmap-virt | high | 4 | 0.0469553 | 0.0469251 | -0.06 | ~ -0.06% (flat) | 4.89 / 3.63 |
| unmap-virt | high | 8 | 0.0209718 | 0.0210692 | 0.46 | ~ +0.46% (flat) | 5.25 / 1.82 |
| unmap-virt | high | 16 | 0.0060126 | 0.00585819 | -2.57 | ≤ -2.57% | 3.96 / 4.84 |
| unmap-virt | low | 1 | 0.181967 | 0.203785 | 11.99 | ≥ +11.99% | 5.25 / 4.4 |
| unmap-virt | low | 2 | 0.0824126 | 0.0897111 | 8.86 | ≥ +8.86% | 10.17 / 8.17 |
| unmap-virt | low | 4 | 0.0407271 | 0.0477039 | 17.13 | ≥ +17.13% | 11.1 / 3.55 |
| unmap-virt | low | 8 | 0.020447 | 0.0215857 | 5.57 | ≥ +5.57% | 3.3 / 10.22 |
| unmap-virt | low | 16 | 0.00595174 | 0.00615388 | 3.4 | ≥ +3.40% | 2.83 / 6.32 |

## 2. 真实应用等价件 + JVM（PS-F7/F8；EVAL sec 3）

| workload | config | base median | t0 median | Δ%(T0 更优为正) | verdict | CV base/t0 |
|---|---|---|---|---|---|---|
| dedup_eq | t8/glibc | 4.16825e+06 | - | - | n/a (missing/failed leg) | - / - |
| dedup_eq_tcmalloc | t8/tcmalloc | 4.56096e+06 | 372217 | -91.84 | ≤ -91.84% | 1.01 / 0.89 |
| jvm | t2000x3 | 2718.51 | - | - | n/a (missing/failed leg) | - / - |
| metis_eq | t8 | 17.066 | - | - | n/a (missing/failed leg) | - / - |
| psearchy_eq | t8 | 3.826 | - | - | n/a (missing/failed leg) | - / - |

方向说明: metis_eq/psearchy_eq/jvm 为 lower-is-better（秒/毫秒），表中 Δ 已归一——正值一律表示 T0（MODE）更优；dedup_eq/mmbench 为 higher-is-better（ops/s per µs）。

## 3. G1 预判定（EVAL sec 6: 低竞争 t∈{4,8}，{mmap-pf,pf,unmap,unmap-virt} 中 ≥2 项（t4 与 t8 同时）中位数提升 ≥10%）

| bench | t4 Δ% | t8 Δ% | qualifies |
|---|---|---|---|
| mmap-pf | -2.65 | 17.9 | no |
| pf | 258.7 | -0.52 | no |
| unmap | -5.83 | 7.77 | no |
| unmap-virt | 17.13 | 5.57 | no |

**G1 预判定: NOT MET (中期数据; M8 终测口径不变)**（qualifying: 0/4）

## 3b. G3 预判定（EVAL sec 6: {metis_eq, dedup_eq ptmalloc 档, JVM 线程} 中 ≥1 项 t=8 提升 ≥10% 或 trace 级机制解释）

| item | Δ%(T0 更优为正) | counts |
|---|---|---|

**G3 预判定: NOT MET (如为负/平: 先查 glibc brk 主导=DEV-6, tcmalloc 档对照定位)**

## 4. strace 抽样（错误返回 multiset diff，run_t0_dod.sh trace_pair 口径，5s 窗）

| trace pair | status |
|---|---|
| mmbench_mmap_low_t1 | equal (rc 0/0) |
| mmbench_mmap_low_t2 | differs (rc 0/0) |
| mmbench_mmap_low_t4 | differs (rc 0/0) |
| mmbench_mmap_low_t8 | differs (rc 0/0) |
| mmbench_mmap_low_t16 | differs (rc 0/0) |
| mmbench_mmap_high_t1 | equal (rc 0/0) |
| mmbench_mmap_high_t2 | equal (rc 0/0) |
| mmbench_mmap_high_t4 | differs (rc 0/0) |
| mmbench_mmap_high_t8 | equal (rc 0/0) |
| mmbench_mmap_high_t16 | equal (rc 0/0) |
| mmbench_mmap-pf_low_t1 | equal (rc 0/0) |
| mmbench_mmap-pf_low_t2 | equal (rc 0/0) |
| mmbench_mmap-pf_low_t4 | equal (rc 0/0) |
| mmbench_mmap-pf_low_t8 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t16 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t1 | equal (rc 0/0) |
| mmbench_mmap-pf_high_t2 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t4 | equal (rc 0/0) |
| mmbench_mmap-pf_high_t8 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t16 | differs (rc 0/0) |
| mmbench_pf_low_t1 | equal (rc 0/0) |
| mmbench_pf_low_t2 | differs (rc 0/0) |
| mmbench_pf_low_t4 | equal (rc 0/0) |
| mmbench_pf_low_t8 | differs (rc 0/0) |
| mmbench_pf_low_t16 | differs (rc 0/0) |
| mmbench_pf_high_t1 | equal (rc 0/0) |
| mmbench_pf_high_t2 | differs (rc 0/0) |
| mmbench_pf_high_t4 | equal (rc 0/0) |
| mmbench_pf_high_t8 | differs (rc 0/0) |
| mmbench_pf_high_t16 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t1 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t2 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t4 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t8 | differs (rc 0/0) |
| mmbench_unmap-virt_low_t16 | equal (rc 0/0) |
| mmbench_unmap-virt_high_t1 | equal (rc 0/0) |
| mmbench_unmap-virt_high_t2 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t4 | equal (rc 0/0) |
| mmbench_unmap-virt_high_t8 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t16 | differs (rc 0/0) |
| mmbench_unmap_low_t1 | equal (rc 0/0) |
| mmbench_unmap_low_t2 | differs (rc 0/0) |
| mmbench_unmap_low_t4 | differs (rc 0/0) |
| mmbench_unmap_low_t8 | equal (rc 0/0) |
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

> **WARN**: 27 对 trace 在 MODE 下出现新的错误返回 —— 逐条核对 `raw/trace/<tag>.err.diff`（mmap 返回地址差异属设计内，不携带错误；此处只看 `-1 ERRNO` 对）。

## 5. M1 基线同参核对与数值对照


### 5.1 参数一致性审计（逐 raw 记录 vs M1 协议; 通过 318/318）

全部记录与 M1 协议同参（seed 公式/线程数/语料规模/时长/allocator/JVM 形参）。

### 5.2 数值对照（T5 BASE 臂 vs M1 基线中位；跨 boot 仅参考，非 gate）

| config | M1 median | T5 base median | 偏差% | 备注 |
|---|---|---|---|---|
| dedup_eq t8/glibc | 4801431 | 4.16825e+06 | -13.2% |  |
| dedup_eq_tcmalloc t8/tcmalloc | 5162927 | 4.56096e+06 | -11.7% |  |
| jvm t2000x3 | 2347.33 | 2718.51 | -15.8% |  |
| metis_eq t8 | 15.772 | 17.066 | -8.2% |  |
| mmbench mmap/high/t1 | 0.205918 | 0.1931 | -6.2% |  |
| mmbench mmap/high/t16 | 0.00379847 | 0.00335073 | -11.8% |  |
| mmbench mmap/high/t2 | 0.0825004 | 0.0706599 | -14.4% |  |
| mmbench mmap/high/t4 | 0.03629 | 0.0301133 | -17.0% |  |
| mmbench mmap/high/t8 | 0.0160272 | 0.0128697 | -19.7% |  |
| mmbench mmap/low/t1 | 0.190526 | 0.195713 | +2.7% |  |
| mmbench mmap/low/t16 | 0.00356994 | 0.00329457 | -7.7% |  |
| mmbench mmap/low/t2 | 0.0826706 | 0.0700478 | -15.3% |  |
| mmbench mmap/low/t4 | 0.0361035 | 0.0301956 | -16.4% |  |
| mmbench mmap/low/t8 | 0.0160509 | 0.0131267 | -18.2% |  |
| mmbench mmap-pf/high/t1 | 0.0245287 | 0.0247248 | +0.8% |  |
| mmbench mmap-pf/high/t16 | 0.00092601 | 0.000586924 | -36.6% **>25% 漂移** |  |
| mmbench mmap-pf/high/t2 | 0.00472667 | 0.00333738 | -29.4% **>25% 漂移** |  |
| mmbench mmap-pf/high/t4 | 0.00310167 | 0.00240294 | -22.5% |  |
| mmbench mmap-pf/high/t8 | 0.00203699 | 0.001196 | -41.3% **>25% 漂移** |  |
| mmbench mmap-pf/low/t1 | 0.0245319 | 0.0232743 | -5.1% |  |
| mmbench mmap-pf/low/t16 | 0.000791689 | 0.00055312 | -30.1% **>25% 漂移** |  |
| mmbench mmap-pf/low/t2 | 0.00397015 | 0.00314483 | -20.8% |  |
| mmbench mmap-pf/low/t4 | 0.00335439 | 0.00243524 | -27.4% **>25% 漂移** |  |
| mmbench mmap-pf/low/t8 | 0.00159097 | 0.00112565 | -29.2% **>25% 漂移** |  |
| mmbench pf/high/t1 | 0.0327937 | 0.0367554 | +12.1% |  |
| mmbench pf/high/t16 | 0.0210678 | 0.0177507 | -15.7% |  |
| mmbench pf/high/t2 | 0.0215158 | 0.0360916 | +67.7% **>25% 漂移** |  |
| mmbench pf/high/t4 | 0.0436225 | 0.037656 | -13.7% |  |
| mmbench pf/high/t8 | 0.0418696 | 0.0343003 | -18.1% |  |
| mmbench pf/low/t1 | 0.004185 | 0.00993873 | +137.5% **>25% 漂移** |  |
| mmbench pf/low/t16 | 0.0335499 | 0.0233141 | -30.5% **>25% 漂移** |  |
| mmbench pf/low/t2 | 0.0070048 | 0.00975955 | +39.3% **>25% 漂移** |  |
| mmbench pf/low/t4 | 0.00970738 | 0.00966727 | -0.4% |  |
| mmbench pf/low/t8 | 0.038444 | 0.0327547 | -14.8% |  |
| mmbench unmap/high/t1 | 0.0580928 | 0.0586808 | +1.0% |  |
| mmbench unmap/high/t16 | 0.00094581 | 0.00086225 | -8.8% |  |
| mmbench unmap/high/t2 | 0.00702397 | 0.00590917 | -15.9% |  |
| mmbench unmap/high/t4 | 0.00497333 | 0.00348951 | -29.8% **>25% 漂移** |  |
| mmbench unmap/high/t8 | 0.00230889 | 0.0017942 | -22.3% |  |
| mmbench unmap/low/t1 | 0.0602691 | 0.0595444 | -1.2% |  |
| mmbench unmap/low/t16 | 0.000963858 | 0.00068378 | -29.1% **>25% 漂移** |  |
| mmbench unmap/low/t2 | 0.00553709 | 0.00410304 | -25.9% **>25% 漂移** |  |
| mmbench unmap/low/t4 | 0.00446821 | 0.00355672 | -20.4% |  |
| mmbench unmap/low/t8 | 0.00213514 | 0.00132533 | -37.9% **>25% 漂移** |  |
| mmbench unmap-virt/high/t1 | 0.218624 | 0.245491 | +12.3% |  |
| mmbench unmap-virt/high/t16 | 0.00656029 | 0.0060126 | -8.3% |  |
| mmbench unmap-virt/high/t2 | 0.126328 | 0.10445 | -17.3% |  |
| mmbench unmap-virt/high/t4 | 0.0557513 | 0.0469553 | -15.8% |  |
| mmbench unmap-virt/high/t8 | 0.025576 | 0.0209718 | -18.0% |  |
| mmbench unmap-virt/low/t1 | 0.193832 | 0.181967 | -6.1% |  |
| mmbench unmap-virt/low/t16 | 0.00699679 | 0.00595174 | -14.9% |  |
| mmbench unmap-virt/low/t2 | 0.109961 | 0.0824126 | -25.1% **>25% 漂移** |  |
| mmbench unmap-virt/low/t4 | 0.0531767 | 0.0407271 | -23.4% |  |
| mmbench unmap-virt/low/t8 | 0.0225235 | 0.020447 | -9.2% |  |
| psearchy_eq t8 | 4.009 | 3.826 | +4.6% |  |

> **WARN**: 以下配置 T5 BASE 臂与 M1 基线偏差 >25%（跨 boot 噪声/不同 mitigations/宿主负载；先用 in-boot BASE vs T0 结论，再排查漂移源）。

- mmbench mmap-pf/high/t16: base vs M1 -36.6%
- mmbench mmap-pf/high/t2: base vs M1 -29.4%
- mmbench mmap-pf/high/t8: base vs M1 -41.3%
- mmbench mmap-pf/low/t16: base vs M1 -30.1%
- mmbench mmap-pf/low/t4: base vs M1 -27.4%
- mmbench mmap-pf/low/t8: base vs M1 -29.2%
- mmbench pf/high/t2: base vs M1 +67.7%
- mmbench pf/low/t1: base vs M1 +137.5%
- mmbench pf/low/t16: base vs M1 -30.5%
- mmbench pf/low/t2: base vs M1 +39.3%
- mmbench unmap/high/t4: base vs M1 -29.8%
- mmbench unmap/low/t16: base vs M1 -29.1%
- mmbench unmap/low/t2: base vs M1 -25.9%
- mmbench unmap/low/t8: base vs M1 -37.9%
- mmbench unmap-virt/low/t2: base vs M1 -25.1%

## 6. CV 告警（EVAL sec 2.3: >5% 应加测至 5 次）

- mmbench mmap/high/t16: cv_base=2.54% cv_t0=5.04%
- mmbench mmap/high/t2: cv_base=2.68% cv_t0=5.76%
- mmbench mmap/low/t8: cv_base=5.62% cv_t0=3.82%
- mmbench mmap-pf/high/t2: cv_base=5.71% cv_t0=5.74%
- mmbench mmap-pf/high/t4: cv_base=6.02% cv_t0=4.29%
- mmbench mmap-pf/high/t8: cv_base=2.67% cv_t0=11.35%
- mmbench mmap-pf/low/t1: cv_base=12.57% cv_t0=40.47%
- mmbench mmap-pf/low/t16: cv_base=6.07% cv_t0=2.01%
- mmbench mmap-pf/low/t2: cv_base=6.6% cv_t0=3.56%
- mmbench mmap-pf/low/t4: cv_base=7.92% cv_t0=3.44%
- mmbench pf/high/t2: cv_base=19.12% cv_t0=1.1%
- mmbench pf/low/t1: cv_base=65.99% cv_t0=52.07%
- mmbench pf/low/t16: cv_base=4.18% cv_t0=6.8%
- mmbench pf/low/t2: cv_base=10.46% cv_t0=47.88%
- mmbench pf/low/t4: cv_base=50.94% cv_t0=50.14%
- mmbench pf/low/t8: cv_base=16.66% cv_t0=3.75%
- mmbench unmap/high/t16: cv_base=7.17% cv_t0=2.71%
- mmbench unmap/high/t8: cv_base=2.75% cv_t0=8.11%
- mmbench unmap/low/t1: cv_base=9.56% cv_t0=9.35%
- mmbench unmap/low/t2: cv_base=17.31% cv_t0=18.63%
- mmbench unmap/low/t4: cv_base=16.87% cv_t0=13.36%
- mmbench unmap-virt/high/t1: cv_base=4.09% cv_t0=5.45%
- mmbench unmap-virt/high/t8: cv_base=5.25% cv_t0=1.82%
- mmbench unmap-virt/low/t1: cv_base=5.25% cv_t0=4.4%
- mmbench unmap-virt/low/t16: cv_base=2.83% cv_t0=6.32%
- mmbench unmap-virt/low/t2: cv_base=10.17% cv_t0=8.17%
- mmbench unmap-virt/low/t4: cv_base=11.1% cv_t0=3.55%
- mmbench unmap-virt/low/t8: cv_base=3.3% cv_t0=10.22%

## 7. 结论速览

- PARAM-MISMATCH: 0（exit 2 口径）
- 覆盖缺口: 0
- 失败/不可解析记录: 12
- strace differs: 27
- G1 预判定: NOT MET
- 报告退出码: 1（0=干净, 1=数据问题, 2=参数不一致）

