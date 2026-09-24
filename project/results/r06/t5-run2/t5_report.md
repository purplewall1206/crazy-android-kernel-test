# CortenMM M4.T5 中期对比报告 (MODE-process T0 vs in-boot BASE)

- 生成: /home/ppw/cortenmm/results/r06/t5-run2/t5_report.md (analyze_t5.py)
- 协议: docs/EVAL.md sec1-3（权威）；M1 参数锚: publish/baseline/baseline_meta.json
- kernel: 6.18.32-gba77046c78fe-dirty  host: syzkaller  utc: 2026-09-18T05:42:42Z
- cmdline: `console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount corten=on mitigations=off kunit.enable=0 fsck.mode=force fsck.repair=yes`
- mmbench sha256: `2a9c066c6e5fe6970f31c40aa1a1df75a61314313c6fba35ee0e10610797bf6f`（M1: `038ed5b3…` 为 bzImage，二进制漂移见 meta/env.txt）
- 记录: 330 条单行 JSON（失败/不可解析 3+0）

## 1. mmbench（论文 Table 3 口径，D6 语义声明见 bench/mmbench/README.md）

| bench | cont | t | base median | t0 median | Δ%(T0 更优为正) | verdict | CV base/t0 |
|---|---|---|---|---|---|---|---|
| mmap | high | 1 | 0.258321 | 0.250838 | -2.9 | ≤ -2.90% | 11.75 / 6.92 |
| mmap | high | 2 | 0.088548 | 0.070548 | -20.33 | ≤ -20.33% | 7.95 / 11.36 |
| mmap | high | 4 | 0.0383966 | 0.0357419 | -6.91 | ≤ -6.91% | 13.94 / 12.43 |
| mmap | high | 8 | 0.0152594 | 0.0160067 | 4.9 | ≥ +4.90% | 10.87 / 8.56 |
| mmap | high | 16 | 0.00370976 | 0.00366194 | -1.29 | ≤ -1.29% | 4.77 / 3.32 |
| mmap | low | 1 | 0.232761 | 0.211641 | -9.07 | ≤ -9.07% | 11.0 / 10.1 |
| mmap | low | 2 | 0.0753884 | 0.0797671 | 5.81 | ≥ +5.81% | 7.92 / 8.8 |
| mmap | low | 4 | 0.0291186 | 0.0291329 | 0.05 | ~ +0.05% (flat) | 14.08 / 11.87 |
| mmap | low | 8 | 0.0154957 | 0.01549 | -0.04 | ~ -0.04% (flat) | 11.56 / 15.08 |
| mmap | low | 16 | 0.00349386 | 0.00378665 | 8.38 | ≥ +8.38% | 11.32 / 2.83 |
| mmap-pf | high | 1 | 0.0294035 | 0.0302196 | 2.78 | ≥ +2.78% | 1.84 / 2.99 |
| mmap-pf | high | 2 | 0.00350661 | 0.00344698 | -1.7 | ≤ -1.70% | 3.35 / 5.11 |
| mmap-pf | high | 4 | 0.00272195 | 0.0026826 | -1.45 | ≤ -1.45% | 9.45 / 10.46 |
| mmap-pf | high | 8 | 0.00142281 | 0.00131807 | -7.36 | ≤ -7.36% | 5.33 / 4.61 |
| mmap-pf | high | 16 | 0.000632321 | 0.000693281 | 9.64 | ≥ +9.64% | 8.39 / 3.92 |
| mmap-pf | low | 1 | 0.0303639 | 0.0290895 | -4.2 | ≤ -4.20% | 8.51 / 11.72 |
| mmap-pf | low | 2 | 0.00334595 | 0.00299672 | -10.44 | ≤ -10.44% | 5.38 / 8.23 |
| mmap-pf | low | 4 | 0.00249137 | 0.00283751 | 13.89 | ≥ +13.89% | 8.59 / 11.03 |
| mmap-pf | low | 8 | 0.0012643 | 0.00127102 | 0.53 | ~ +0.53% (flat) | 6.95 / 8.14 |
| mmap-pf | low | 16 | 0.000696317 | 0.000653472 | -6.15 | ≤ -6.15% | 5.44 / 10.13 |
| pf | high | 1 | 0.0429018 | 0.042026 | -2.04 | ≤ -2.04% | 3.73 / 4.24 |
| pf | high | 2 | 0.0442825 | 0.0406263 | -8.26 | ≤ -8.26% | 4.86 / 10.32 |
| pf | high | 4 | 0.0462225 | 0.0457394 | -1.05 | ≤ -1.05% | 9.87 / 1.5 |
| pf | high | 8 | 0.0405584 | 0.0376332 | -7.21 | ≤ -7.21% | 9.43 / 1.3 |
| pf | high | 16 | 0.0210916 | 0.022526 | 6.8 | ≥ +6.80% | 2.15 / 8.18 |
| pf | low | 1 | 0.0425312 | 0.0493969 | 16.14 | ≥ +16.14% | 47.68 / 7.32 |
| pf | low | 2 | 0.0423546 | 0.0479717 | 13.26 | ≥ +13.26% | 32.91 / 11.62 |
| pf | low | 4 | 0.0444577 | 0.0449271 | 1.06 | ≥ +1.06% | 29.02 / 4.27 |
| pf | low | 8 | 0.0407435 | 0.040349 | -0.97 | ~ -0.97% (flat) | 3.37 / 7.46 |
| pf | low | 16 | 0.0295441 | 0.0279505 | -5.39 | ≤ -5.39% | 5.47 / 3.44 |
| unmap | high | 1 | 0.0719415 | 0.0751101 | 4.4 | ≥ +4.40% | 3.69 / 5.21 |
| unmap | high | 2 | 0.00607155 | 0.00601127 | -0.99 | ~ -0.99% (flat) | 3.95 / 3.22 |
| unmap | high | 4 | 0.00420183 | 0.00432149 | 2.85 | ≥ +2.85% | 6.18 / 9.99 |
| unmap | high | 8 | 0.00213299 | 0.00216118 | 1.32 | ≥ +1.32% | 2.35 / 2.55 |
| unmap | high | 16 | 0.00101956 | 0.000972397 | -4.63 | ≤ -4.63% | 7.48 / 5.25 |
| unmap | low | 1 | 0.0687436 | 0.0659594 | -4.05 | ≤ -4.05% | 3.96 / 7.01 |
| unmap | low | 2 | 0.00418344 | 0.00545336 | 30.36 | ≥ +30.36% | 7.09 / 9.15 |
| unmap | low | 4 | 0.00409045 | 0.0038446 | -6.01 | ≤ -6.01% | 5.69 / 3.57 |
| unmap | low | 8 | 0.00201561 | 0.00173996 | -13.68 | ≤ -13.68% | 7.0 / 6.11 |
| unmap | low | 16 | 0.0007762 | 0.000801008 | 3.2 | ≥ +3.20% | 5.55 / 7.08 |
| unmap-virt | high | 1 | 0.275255 | 0.299146 | 8.68 | ≥ +8.68% | 5.52 / 7.03 |
| unmap-virt | high | 2 | 0.132045 | 0.131476 | -0.43 | ~ -0.43% (flat) | 5.25 / 5.54 |
| unmap-virt | high | 4 | 0.0436976 | 0.0416292 | -4.73 | ≤ -4.73% | 2.77 / 1.89 |
| unmap-virt | high | 8 | 0.023213 | 0.0229936 | -0.95 | ~ -0.95% (flat) | 7.9 / 13.48 |
| unmap-virt | high | 16 | 0.00591458 | 0.00569693 | -3.68 | ≤ -3.68% | 2.88 / 6.23 |
| unmap-virt | low | 1 | 0.212678 | 0.226266 | 6.39 | ≥ +6.39% | 7.25 / 5.44 |
| unmap-virt | low | 2 | 0.0966049 | 0.0964423 | -0.17 | ~ -0.17% (flat) | 2.01 / 10.14 |
| unmap-virt | low | 4 | 0.0341266 | 0.0505991 | 48.27 | ≥ +48.27% | 24.64 / 11.11 |
| unmap-virt | low | 8 | 0.0186451 | 0.0207732 | 11.41 | ≥ +11.41% | 10.3 / 13.24 |
| unmap-virt | low | 16 | 0.00621823 | 0.00581167 | -6.54 | ≤ -6.54% | 4.07 / 6.46 |

## 2. 真实应用等价件 + JVM（PS-F7/F8；EVAL sec 3）

| workload | config | base median | t0 median | Δ%(T0 更优为正) | verdict | CV base/t0 |
|---|---|---|---|---|---|---|
| dedup_eq | t8/glibc | 4.5175e+06 | 4.581e+06 | 1.41 | ≥ +1.41% | 2.94 / 3.7 |
| dedup_eq_tcmalloc | t8/tcmalloc | 5.0667e+06 | 5.64045e+06 | 11.32 | ≥ +11.32% | 0.49 / 1.73 |
| jvm | t2000x3 | 2340.71 | - | - | n/a (missing/failed leg) | - / - |
| metis_eq | t8 | 16.516 | 16.8 | -1.72 | ≤ -1.72% | 19.85 / 4.79 |
| psearchy_eq | t8 | 4.27 | 5.98 | -40.05 | ≤ -40.05% | 37.52 / 20.04 |

方向说明: metis_eq/psearchy_eq/jvm 为 lower-is-better（秒/毫秒），表中 Δ 已归一——正值一律表示 T0（MODE）更优；dedup_eq/mmbench 为 higher-is-better（ops/s per µs）。

## 3. G1 预判定（EVAL sec 6: 低竞争 t∈{4,8}，{mmap-pf,pf,unmap,unmap-virt} 中 ≥2 项（t4 与 t8 同时）中位数提升 ≥10%）

| bench | t4 Δ% | t8 Δ% | qualifies |
|---|---|---|---|
| mmap-pf | 13.89 | 0.53 | no |
| pf | 1.06 | -0.97 | no |
| unmap | -6.01 | -13.68 | no |
| unmap-virt | 48.27 | 11.41 | YES |

**G1 预判定: NOT MET (中期数据; M8 终测口径不变)**（qualifying: 1/4）

## 3b. G3 预判定（EVAL sec 6: {metis_eq, dedup_eq ptmalloc 档, JVM 线程} 中 ≥1 项 t=8 提升 ≥10% 或 trace 级机制解释）

| item | Δ%(T0 更优为正) | counts |
|---|---|---|
| dedup_eq (t8/glibc) | 1.41 | no |
| metis_eq (t8) | -1.72 | no |

**G3 预判定: NOT MET (如为负/平: 先查 glibc brk 主导=DEV-6, tcmalloc 档对照定位)**

## 4. strace 抽样（错误返回 multiset diff，run_t0_dod.sh trace_pair 口径，5s 窗）

| trace pair | status |
|---|---|
| mmbench_mmap_low_t1 | equal (rc 0/0) |
| mmbench_mmap_low_t2 | differs (rc 0/0) |
| mmbench_mmap_low_t4 | equal (rc 0/0) |
| mmbench_mmap_low_t8 | equal (rc 0/0) |
| mmbench_mmap_low_t16 | differs (rc 0/0) |
| mmbench_mmap_high_t1 | equal (rc 0/0) |
| mmbench_mmap_high_t2 | equal (rc 0/0) |
| mmbench_mmap_high_t4 | equal (rc 0/0) |
| mmbench_mmap_high_t8 | differs (rc 0/0) |
| mmbench_mmap_high_t16 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t1 | equal (rc 0/0) |
| mmbench_mmap-pf_low_t2 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t4 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t8 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t16 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t1 | equal (rc 0/0) |
| mmbench_mmap-pf_high_t2 | equal (rc 0/0) |
| mmbench_mmap-pf_high_t4 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t8 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t16 | differs (rc 0/0) |
| mmbench_pf_low_t1 | equal (rc 0/0) |
| mmbench_pf_low_t2 | equal (rc 0/0) |
| mmbench_pf_low_t4 | differs (rc 0/0) |
| mmbench_pf_low_t8 | differs (rc 0/0) |
| mmbench_pf_low_t16 | differs (rc 0/0) |
| mmbench_pf_high_t1 | equal (rc 0/0) |
| mmbench_pf_high_t2 | equal (rc 0/0) |
| mmbench_pf_high_t4 | differs (rc 0/0) |
| mmbench_pf_high_t8 | equal (rc 0/0) |
| mmbench_pf_high_t16 | differs (rc 0/0) |
| mmbench_unmap-virt_low_t1 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t2 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t4 | differs (rc 0/0) |
| mmbench_unmap-virt_low_t8 | differs (rc 0/0) |
| mmbench_unmap-virt_low_t16 | equal (rc 0/0) |
| mmbench_unmap-virt_high_t1 | equal (rc 0/0) |
| mmbench_unmap-virt_high_t2 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t4 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t8 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t16 | differs (rc 0/0) |
| mmbench_unmap_low_t1 | equal (rc 0/0) |
| mmbench_unmap_low_t2 | equal (rc 0/0) |
| mmbench_unmap_low_t4 | equal (rc 0/0) |
| mmbench_unmap_low_t8 | differs (rc 0/0) |
| mmbench_unmap_low_t16 | differs (rc 0/0) |
| mmbench_unmap_high_t1 | equal (rc 0/0) |
| mmbench_unmap_high_t2 | equal (rc 0/0) |
| mmbench_unmap_high_t4 | equal (rc 0/0) |
| mmbench_unmap_high_t8 | differs (rc 0/0) |
| mmbench_unmap_high_t16 | differs (rc 0/0) |
| metis_eq | equal (rc 137/137) |
| psearchy_eq | equal (rc 137/137) |
| dedup_eq | equal (rc 137/137) |
| dedup_eq_tcmalloc | differs (rc 137/137) |
| jvm | differs (rc 137/137) |

> **WARN**: 28 对 trace 在 MODE 下出现新的错误返回 —— 逐条核对 `raw/trace/<tag>.err.diff`（mmap 返回地址差异属设计内，不携带错误；此处只看 `-1 ERRNO` 对）。

## 5. M1 基线同参核对与数值对照


### 5.1 参数一致性审计（逐 raw 记录 vs M1 协议; 通过 327/327）

全部记录与 M1 协议同参（seed 公式/线程数/语料规模/时长/allocator/JVM 形参）。

### 5.2 数值对照（T5 BASE 臂 vs M1 基线中位；跨 boot 仅参考，非 gate）

| config | M1 median | T5 base median | 偏差% | 备注 |
|---|---|---|---|---|
| dedup_eq t8/glibc | 4801431 | 4.5175e+06 | -5.9% |  |
| dedup_eq_tcmalloc t8/tcmalloc | 5162927 | 5.0667e+06 | -1.9% |  |
| jvm t2000x3 | 2347.33 | 2340.71 | +0.3% |  |
| metis_eq t8 | 15.772 | 16.516 | -4.7% |  |
| mmbench mmap/high/t1 | 0.205918 | 0.258321 | +25.4% **>25% 漂移** |  |
| mmbench mmap/high/t16 | 0.00379847 | 0.00370976 | -2.3% |  |
| mmbench mmap/high/t2 | 0.0825004 | 0.088548 | +7.3% |  |
| mmbench mmap/high/t4 | 0.03629 | 0.0383966 | +5.8% |  |
| mmbench mmap/high/t8 | 0.0160272 | 0.0152594 | -4.8% |  |
| mmbench mmap/low/t1 | 0.190526 | 0.232761 | +22.2% |  |
| mmbench mmap/low/t16 | 0.00356994 | 0.00349386 | -2.1% |  |
| mmbench mmap/low/t2 | 0.0826706 | 0.0753884 | -8.8% |  |
| mmbench mmap/low/t4 | 0.0361035 | 0.0291186 | -19.3% |  |
| mmbench mmap/low/t8 | 0.0160509 | 0.0154957 | -3.5% |  |
| mmbench mmap-pf/high/t1 | 0.0245287 | 0.0294035 | +19.9% |  |
| mmbench mmap-pf/high/t16 | 0.00092601 | 0.000632321 | -31.7% **>25% 漂移** |  |
| mmbench mmap-pf/high/t2 | 0.00472667 | 0.00350661 | -25.8% **>25% 漂移** |  |
| mmbench mmap-pf/high/t4 | 0.00310167 | 0.00272195 | -12.2% |  |
| mmbench mmap-pf/high/t8 | 0.00203699 | 0.00142281 | -30.2% **>25% 漂移** |  |
| mmbench mmap-pf/low/t1 | 0.0245319 | 0.0303639 | +23.8% |  |
| mmbench mmap-pf/low/t16 | 0.000791689 | 0.000696317 | -12.0% |  |
| mmbench mmap-pf/low/t2 | 0.00397015 | 0.00334595 | -15.7% |  |
| mmbench mmap-pf/low/t4 | 0.00335439 | 0.00249137 | -25.7% **>25% 漂移** |  |
| mmbench mmap-pf/low/t8 | 0.00159097 | 0.0012643 | -20.5% |  |
| mmbench pf/high/t1 | 0.0327937 | 0.0429018 | +30.8% **>25% 漂移** |  |
| mmbench pf/high/t16 | 0.0210678 | 0.0210916 | +0.1% |  |
| mmbench pf/high/t2 | 0.0215158 | 0.0442825 | +105.8% **>25% 漂移** |  |
| mmbench pf/high/t4 | 0.0436225 | 0.0462225 | +6.0% |  |
| mmbench pf/high/t8 | 0.0418696 | 0.0405584 | -3.1% |  |
| mmbench pf/low/t1 | 0.004185 | 0.0425312 | +916.3% **>25% 漂移** |  |
| mmbench pf/low/t16 | 0.0335499 | 0.0295441 | -11.9% |  |
| mmbench pf/low/t2 | 0.0070048 | 0.0423546 | +504.7% **>25% 漂移** |  |
| mmbench pf/low/t4 | 0.00970738 | 0.0444577 | +358.0% **>25% 漂移** |  |
| mmbench pf/low/t8 | 0.038444 | 0.0407435 | +6.0% |  |
| mmbench unmap/high/t1 | 0.0580928 | 0.0719415 | +23.8% |  |
| mmbench unmap/high/t16 | 0.00094581 | 0.00101956 | +7.8% |  |
| mmbench unmap/high/t2 | 0.00702397 | 0.00607155 | -13.6% |  |
| mmbench unmap/high/t4 | 0.00497333 | 0.00420183 | -15.5% |  |
| mmbench unmap/high/t8 | 0.00230889 | 0.00213299 | -7.6% |  |
| mmbench unmap/low/t1 | 0.0602691 | 0.0687436 | +14.1% |  |
| mmbench unmap/low/t16 | 0.000963858 | 0.0007762 | -19.5% |  |
| mmbench unmap/low/t2 | 0.00553709 | 0.00418344 | -24.4% |  |
| mmbench unmap/low/t4 | 0.00446821 | 0.00409045 | -8.5% |  |
| mmbench unmap/low/t8 | 0.00213514 | 0.00201561 | -5.6% |  |
| mmbench unmap-virt/high/t1 | 0.218624 | 0.275255 | +25.9% **>25% 漂移** |  |
| mmbench unmap-virt/high/t16 | 0.00656029 | 0.00591458 | -9.8% |  |
| mmbench unmap-virt/high/t2 | 0.126328 | 0.132045 | +4.5% |  |
| mmbench unmap-virt/high/t4 | 0.0557513 | 0.0436976 | -21.6% |  |
| mmbench unmap-virt/high/t8 | 0.025576 | 0.023213 | -9.2% |  |
| mmbench unmap-virt/low/t1 | 0.193832 | 0.212678 | +9.7% |  |
| mmbench unmap-virt/low/t16 | 0.00699679 | 0.00621823 | -11.1% |  |
| mmbench unmap-virt/low/t2 | 0.109961 | 0.0966049 | -12.1% |  |
| mmbench unmap-virt/low/t4 | 0.0531767 | 0.0341266 | -35.8% **>25% 漂移** |  |
| mmbench unmap-virt/low/t8 | 0.0225235 | 0.0186451 | -17.2% |  |
| psearchy_eq t8 | 4.009 | 4.27 | -6.5% |  |

> **WARN**: 以下配置 T5 BASE 臂与 M1 基线偏差 >25%（跨 boot 噪声/不同 mitigations/宿主负载；先用 in-boot BASE vs T0 结论，再排查漂移源）。

- mmbench mmap/high/t1: base vs M1 +25.4%
- mmbench mmap-pf/high/t16: base vs M1 -31.7%
- mmbench mmap-pf/high/t2: base vs M1 -25.8%
- mmbench mmap-pf/high/t8: base vs M1 -30.2%
- mmbench mmap-pf/low/t4: base vs M1 -25.7%
- mmbench pf/high/t1: base vs M1 +30.8%
- mmbench pf/high/t2: base vs M1 +105.8%
- mmbench pf/low/t1: base vs M1 +916.3%
- mmbench pf/low/t2: base vs M1 +504.7%
- mmbench pf/low/t4: base vs M1 +358.0%
- mmbench unmap-virt/high/t1: base vs M1 +25.9%
- mmbench unmap-virt/low/t4: base vs M1 -35.8%

## 6. CV 告警（EVAL sec 2.3: >5% 应加测至 5 次）

- metis_eq t8: cv_base=19.85% cv_t0=4.79%
- mmbench mmap/high/t1: cv_base=11.75% cv_t0=6.92%
- mmbench mmap/high/t2: cv_base=7.95% cv_t0=11.36%
- mmbench mmap/high/t4: cv_base=13.94% cv_t0=12.43%
- mmbench mmap/high/t8: cv_base=10.87% cv_t0=8.56%
- mmbench mmap/low/t1: cv_base=11.0% cv_t0=10.1%
- mmbench mmap/low/t16: cv_base=11.32% cv_t0=2.83%
- mmbench mmap/low/t2: cv_base=7.92% cv_t0=8.8%
- mmbench mmap/low/t4: cv_base=14.08% cv_t0=11.87%
- mmbench mmap/low/t8: cv_base=11.56% cv_t0=15.08%
- mmbench mmap-pf/high/t16: cv_base=8.39% cv_t0=3.92%
- mmbench mmap-pf/high/t2: cv_base=3.35% cv_t0=5.11%
- mmbench mmap-pf/high/t4: cv_base=9.45% cv_t0=10.46%
- mmbench mmap-pf/high/t8: cv_base=5.33% cv_t0=4.61%
- mmbench mmap-pf/low/t1: cv_base=8.51% cv_t0=11.72%
- mmbench mmap-pf/low/t16: cv_base=5.44% cv_t0=10.13%
- mmbench mmap-pf/low/t2: cv_base=5.38% cv_t0=8.23%
- mmbench mmap-pf/low/t4: cv_base=8.59% cv_t0=11.03%
- mmbench mmap-pf/low/t8: cv_base=6.95% cv_t0=8.14%
- mmbench pf/high/t16: cv_base=2.15% cv_t0=8.18%
- mmbench pf/high/t2: cv_base=4.86% cv_t0=10.32%
- mmbench pf/high/t4: cv_base=9.87% cv_t0=1.5%
- mmbench pf/high/t8: cv_base=9.43% cv_t0=1.3%
- mmbench pf/low/t1: cv_base=47.68% cv_t0=7.32%
- mmbench pf/low/t16: cv_base=5.47% cv_t0=3.44%
- mmbench pf/low/t2: cv_base=32.91% cv_t0=11.62%
- mmbench pf/low/t4: cv_base=29.02% cv_t0=4.27%
- mmbench pf/low/t8: cv_base=3.37% cv_t0=7.46%
- mmbench unmap/high/t1: cv_base=3.69% cv_t0=5.21%
- mmbench unmap/high/t16: cv_base=7.48% cv_t0=5.25%
- mmbench unmap/high/t4: cv_base=6.18% cv_t0=9.99%
- mmbench unmap/low/t1: cv_base=3.96% cv_t0=7.01%
- mmbench unmap/low/t16: cv_base=5.55% cv_t0=7.08%
- mmbench unmap/low/t2: cv_base=7.09% cv_t0=9.15%
- mmbench unmap/low/t4: cv_base=5.69% cv_t0=3.57%
- mmbench unmap/low/t8: cv_base=7.0% cv_t0=6.11%
- mmbench unmap-virt/high/t1: cv_base=5.52% cv_t0=7.03%
- mmbench unmap-virt/high/t16: cv_base=2.88% cv_t0=6.23%
- mmbench unmap-virt/high/t2: cv_base=5.25% cv_t0=5.54%
- mmbench unmap-virt/high/t8: cv_base=7.9% cv_t0=13.48%
- mmbench unmap-virt/low/t1: cv_base=7.25% cv_t0=5.44%
- mmbench unmap-virt/low/t16: cv_base=4.07% cv_t0=6.46%
- mmbench unmap-virt/low/t2: cv_base=2.01% cv_t0=10.14%
- mmbench unmap-virt/low/t4: cv_base=24.64% cv_t0=11.11%
- mmbench unmap-virt/low/t8: cv_base=10.3% cv_t0=13.24%
- psearchy_eq t8: cv_base=37.52% cv_t0=20.04%

## 7. 结论速览

- PARAM-MISMATCH: 0（exit 2 口径）
- 覆盖缺口: 0
- 失败/不可解析记录: 3
- strace differs: 28
- G1 预判定: NOT MET
- 报告退出码: 1（0=干净, 1=数据问题, 2=参数不一致）

