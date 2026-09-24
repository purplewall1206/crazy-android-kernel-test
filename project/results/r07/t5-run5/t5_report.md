# CortenMM M4.T5 中期对比报告 (MODE-process T0 vs in-boot BASE)

- 生成: results/r07/t5-run5/t5_report.md (analyze_t5.py)
- 协议: docs/EVAL.md sec1-3（权威）；M1 参数锚: publish/baseline/baseline_meta.json
- kernel: 6.18.32-gb9541335a554  host: syzkaller  utc: 2026-09-20T22:16:12Z
- cmdline: `console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount corten=on mitigations=off kunit.enable=0 log_buf_len=64M fsck.mode=force fsck.repair=yes`
- mmbench sha256: `38304f062d4334cbec705cd08ff25d1902bad41aa45a56d4a4c684ecb1acce1b`（M1: `038ed5b3…` 为 bzImage，二进制漂移见 meta/env.txt）
- 记录: 330 条单行 JSON（失败/不可解析 0+0）

## 1. mmbench（论文 Table 3 口径，D6 语义声明见 bench/mmbench/README.md）

| bench | cont | t | base median | t0 median | Δ%(T0 更优为正) | verdict | CV base/t0 |
|---|---|---|---|---|---|---|---|
| mmap | high | 1 | 0.166815 | 0.375513 | 125.11 | ≥ +125.11% | 5.21 / 6.78 |
| mmap | high | 2 | 0.0830533 | 0.161186 | 94.08 | ≥ +94.08% | 3.33 / 0.26 |
| mmap | high | 4 | 0.0328507 | 0.0775355 | 136.02 | ≥ +136.02% | 3.89 / 4.66 |
| mmap | high | 8 | 0.0115439 | 0.0425872 | 268.92 | ≥ +268.92% | 4.87 / 2.64 |
| mmap | high | 16 | 0.00314361 | 0.00813967 | 158.93 | ≥ +158.93% | 1.6 / 3.66 |
| mmap | low | 1 | 0.155674 | 0.420547 | 170.15 | ≥ +170.15% | 10.15 / 6.98 |
| mmap | low | 2 | 0.0722657 | 0.163757 | 126.6 | ≥ +126.60% | 4.5 / 3.72 |
| mmap | low | 4 | 0.034236 | 0.0803617 | 134.73 | ≥ +134.73% | 2.95 / 1.67 |
| mmap | low | 8 | 0.0126727 | 0.0424909 | 235.29 | ≥ +235.29% | 1.8 / 1.75 |
| mmap | low | 16 | 0.00306546 | 0.0085918 | 180.28 | ≥ +180.28% | 1.2 / 4.09 |
| mmap-pf | high | 1 | 0.0208271 | 0.0185572 | -10.9 | ≤ -10.90% | 7.68 / 6.16 |
| mmap-pf | high | 2 | 0.00453385 | 0.00367406 | -18.96 | ≤ -18.96% | 5.74 / 2.91 |
| mmap-pf | high | 4 | 0.0026642 | 0.00215272 | -19.2 | ≤ -19.20% | 2.19 / 5.38 |
| mmap-pf | high | 8 | 0.00119833 | 0.000940541 | -21.51 | ≤ -21.51% | 6.01 / 6.63 |
| mmap-pf | high | 16 | 0.000513983 | 0.000414554 | -19.34 | ≤ -19.34% | 8.53 / 7.38 |
| mmap-pf | low | 1 | 0.0237457 | 0.0170684 | -28.12 | ≤ -28.12% | 1.55 / 13.75 |
| mmap-pf | low | 2 | 0.00423654 | 0.00381027 | -10.06 | ≤ -10.06% | 3.02 / 1.04 |
| mmap-pf | low | 4 | 0.00281557 | 0.0022112 | -21.47 | ≤ -21.47% | 5.65 / 6.86 |
| mmap-pf | low | 8 | 0.00108221 | 0.000951824 | -12.05 | ≤ -12.05% | 2.91 / 6.13 |
| mmap-pf | low | 16 | 0.000499226 | 0.000439619 | -11.94 | ≤ -11.94% | 4.32 / 3.24 |
| pf | high | 1 | 0.030398 | 0.0309986 | 1.98 | ≥ +1.98% | 13.35 / 19.57 |
| pf | high | 2 | 0.0233371 | 0.0273638 | 17.25 | ≥ +17.25% | 3.25 / 3.05 |
| pf | high | 4 | 0.0383359 | 0.0309046 | -19.38 | ≤ -19.38% | 4.25 / 4.06 |
| pf | high | 8 | 0.0262343 | 0.0254988 | -2.8 | ≤ -2.80% | 18.18 / 6.08 |
| pf | high | 16 | 0.0145068 | 0.0127282 | -12.26 | ≤ -12.26% | 3.13 / 6.68 |
| pf | low | 1 | 0.00796415 | 0.00731617 | -8.14 | ≤ -8.14% | 22.7 / 75.53 |
| pf | low | 2 | 0.00941097 | 0.00968394 | 2.9 | ≥ +2.90% | 73.07 / 64.14 |
| pf | low | 4 | 0.028233 | 0.00855466 | -69.7 | ≤ -69.70% | 52.51 / 69.89 |
| pf | low | 8 | 0.0275871 | 0.0271998 | -1.4 | ≤ -1.40% | 8.11 / 2.57 |
| pf | low | 16 | 0.0173258 | 0.016513 | -4.69 | ≤ -4.69% | 16.07 / 8.72 |
| unmap | high | 1 | 0.0529823 | 0.103357 | 95.08 | ≥ +95.08% | 3.9 / 3.4 |
| unmap | high | 2 | 0.00531053 | 0.00531194 | 0.03 | ~ +0.03% (flat) | 4.23 / 6.54 |
| unmap | high | 4 | 0.00332778 | 0.00407845 | 22.56 | ≥ +22.56% | 2.47 / 4.53 |
| unmap | high | 8 | 0.00164775 | 0.00301765 | 83.14 | ≥ +83.14% | 10.18 / 2.82 |
| unmap | high | 16 | 0.000798613 | 0.00164651 | 106.17 | ≥ +106.17% | 3.35 / 1.12 |
| unmap | low | 1 | 0.0594269 | 0.120317 | 102.46 | ≥ +102.46% | 3.18 / 3.24 |
| unmap | low | 2 | 0.00449489 | 0.00566378 | 26.0 | ≥ +26.00% | 6.91 / 10.88 |
| unmap | low | 4 | 0.0026225 | 0.00422702 | 61.18 | ≥ +61.18% | 17.7 / 3.86 |
| unmap | low | 8 | 0.00140006 | 0.00334148 | 138.67 | ≥ +138.67% | 5.66 / 10.07 |
| unmap | low | 16 | 0.000580952 | 0.00166016 | 185.77 | ≥ +185.77% | 8.47 / 0.82 |
| unmap-virt | high | 1 | 0.206485 | 0.526939 | 155.19 | ≥ +155.19% | 2.43 / 2.91 |
| unmap-virt | high | 2 | 0.0922294 | 0.463543 | 402.6 | ≥ +402.60% | 4.63 / 2.94 |
| unmap-virt | high | 4 | 0.0411776 | 0.439577 | 967.51 | ≥ +967.51% | 7.25 / 1.02 |
| unmap-virt | high | 8 | 0.0186127 | 0.448995 | 2312.3 | ≥ +2312.30% | 2.16 / 2.98 |
| unmap-virt | high | 16 | 0.0057077 | 0.38696 | 6679.61 | ≥ +6679.61% | 4.34 / 3.75 |
| unmap-virt | low | 1 | 0.204349 | 0.65182 | 218.97 | ≥ +218.97% | 2.28 / 1.25 |
| unmap-virt | low | 2 | 0.101574 | 0.567676 | 458.88 | ≥ +458.88% | 1.66 / 2.51 |
| unmap-virt | low | 4 | 0.0453983 | 0.570268 | 1156.14 | ≥ +1156.14% | 7.12 / 1.7 |
| unmap-virt | low | 8 | 0.0206165 | 0.500265 | 2326.53 | ≥ +2326.53% | 9.83 / 12.55 |
| unmap-virt | low | 16 | 0.0067302 | 0.469157 | 6870.92 | ≥ +6870.92% | 12.25 / 1.24 |

## 2. 真实应用等价件 + JVM（PS-F7/F8；EVAL sec 3）

| workload | config | base median | t0 median | Δ%(T0 更优为正) | verdict | CV base/t0 |
|---|---|---|---|---|---|---|
| dedup_eq | t8/glibc | 4.02835e+06 | 4.15346e+06 | 3.11 | ≥ +3.11% | 2.22 / 2.33 |
| dedup_eq_tcmalloc | t8/tcmalloc | 4.23349e+06 | 4.60319e+06 | 8.73 | ≥ +8.73% | 2.66 / 1.92 |
| jvm | t2000x3 | 3077.91 | 3218.59 | -4.57 | ≤ -4.57% | 6.87 / 12.39 |
| metis_eq | t8 | 20.923 | 19.071 | 8.85 | ≥ +8.85% | 6.64 / 3.64 |
| psearchy_eq | t8 | 4.766 | 4.666 | 2.1 | ≥ +2.10% | 15.95 / 3.46 |

方向说明: metis_eq/psearchy_eq/jvm 为 lower-is-better（秒/毫秒），表中 Δ 已归一——正值一律表示 T0（MODE）更优；dedup_eq/mmbench 为 higher-is-better（ops/s per µs）。

## 3. G1 预判定（EVAL sec 6: 低竞争 t∈{4,8}，{mmap-pf,pf,unmap,unmap-virt} 中 ≥2 项（t4 与 t8 同时）中位数提升 ≥10%）

| bench | t4 Δ% | t8 Δ% | qualifies |
|---|---|---|---|
| mmap-pf | -21.47 | -12.05 | no |
| pf | -69.7 | -1.4 | no |
| unmap | 61.18 | 138.67 | YES |
| unmap-virt | 1156.14 | 2326.53 | YES |

**G1 预判定: MET**（qualifying: 2/4）

## 3b. G3 预判定（EVAL sec 6: {metis_eq, dedup_eq ptmalloc 档, JVM 线程} 中 ≥1 项 t=8 提升 ≥10% 或 trace 级机制解释）

| item | Δ%(T0 更优为正) | counts |
|---|---|---|
| dedup_eq (t8/glibc) | 3.11 | no |
| jvm (t2000x3, M1 口径 2000 线程 spawn 窗口, 辅助) | -4.57 | aux |
| metis_eq (t8) | 8.85 | no |

**G3 预判定: NOT MET (如为负/平: 先查 glibc brk 主导=DEV-6, tcmalloc 档对照定位)**

## 4. strace 抽样（错误返回 multiset diff，run_t0_dod.sh trace_pair 口径，5s 窗）

| trace pair | status |
|---|---|
| mmbench_mmap_low_t1 | equal (rc 0/0) |
| mmbench_mmap_low_t2 | equal (rc 0/0) |
| mmbench_mmap_low_t4 | differs (rc 0/0) |
| mmbench_mmap_low_t8 | differs (rc 0/0) |
| mmbench_mmap_low_t16 | differs (rc 0/0) |
| mmbench_mmap_high_t1 | equal (rc 0/0) |
| mmbench_mmap_high_t2 | equal (rc 0/0) |
| mmbench_mmap_high_t4 | differs (rc 0/0) |
| mmbench_mmap_high_t8 | differs (rc 0/0) |
| mmbench_mmap_high_t16 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t1 | equal (rc 0/0) |
| mmbench_mmap-pf_low_t2 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t4 | equal (rc 0/0) |
| mmbench_mmap-pf_low_t8 | differs (rc 0/0) |
| mmbench_mmap-pf_low_t16 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t1 | equal (rc 0/0) |
| mmbench_mmap-pf_high_t2 | equal (rc 0/0) |
| mmbench_mmap-pf_high_t4 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t8 | differs (rc 0/0) |
| mmbench_mmap-pf_high_t16 | differs (rc 0/0) |
| mmbench_pf_low_t1 | equal (rc 0/0) |
| mmbench_pf_low_t2 | differs (rc 0/0) |
| mmbench_pf_low_t4 | differs (rc 0/0) |
| mmbench_pf_low_t8 | differs (rc 0/0) |
| mmbench_pf_low_t16 | equal (rc 0/0) |
| mmbench_pf_high_t1 | equal (rc 0/0) |
| mmbench_pf_high_t2 | equal (rc 0/0) |
| mmbench_pf_high_t4 | equal (rc 0/0) |
| mmbench_pf_high_t8 | differs (rc 0/0) |
| mmbench_pf_high_t16 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t1 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t2 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t4 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t8 | equal (rc 0/0) |
| mmbench_unmap-virt_low_t16 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t1 | equal (rc 0/0) |
| mmbench_unmap-virt_high_t2 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t4 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t8 | differs (rc 0/0) |
| mmbench_unmap-virt_high_t16 | differs (rc 0/0) |
| mmbench_unmap_low_t1 | equal (rc 0/0) |
| mmbench_unmap_low_t2 | differs (rc 0/0) |
| mmbench_unmap_low_t4 | equal (rc 0/0) |
| mmbench_unmap_low_t8 | differs (rc 0/0) |
| mmbench_unmap_low_t16 | equal (rc 0/0) |
| mmbench_unmap_high_t1 | equal (rc 0/0) |
| mmbench_unmap_high_t2 | equal (rc 0/0) |
| mmbench_unmap_high_t4 | differs (rc 0/0) |
| mmbench_unmap_high_t8 | equal (rc 0/0) |
| mmbench_unmap_high_t16 | equal (rc 0/0) |
| metis_eq | equal (rc 137/137) |
| psearchy_eq | equal (rc 137/137) |
| dedup_eq | equal (rc 137/137) |
| dedup_eq_tcmalloc | differs (rc 137/137) |
| jvm | differs (rc 137/137) |

> **WARN**: 26 对 trace 在 MODE 下出现新的错误返回 —— 逐条核对 `raw/trace/<tag>.err.diff`（mmap 返回地址差异属设计内，不携带错误；此处只看 `-1 ERRNO` 对）。

## 5. M1 基线同参核对与数值对照


### 5.1 参数一致性审计（逐 raw 记录 vs M1 协议; 通过 330/330）

全部记录与 M1 协议同参（seed 公式/线程数/语料规模/时长/allocator/JVM 形参）。

### 5.2 数值对照（T5 BASE 臂 vs M1 基线中位；跨 boot 仅参考，非 gate）

| config | M1 median | T5 base median | 偏差% | 备注 |
|---|---|---|---|---|
| dedup_eq t8/glibc | 4801431 | 4.02835e+06 | -16.1% |  |
| dedup_eq_tcmalloc t8/tcmalloc | 5162927 | 4.23349e+06 | -18.0% |  |
| jvm t2000x3 | 2347.33 | 3077.91 | -31.1% **>25% 漂移** |  |
| metis_eq t8 | 15.772 | 20.923 | -32.7% **>25% 漂移** |  |
| mmbench mmap/high/t1 | 0.205918 | 0.166815 | -19.0% |  |
| mmbench mmap/high/t16 | 0.00379847 | 0.00314361 | -17.2% |  |
| mmbench mmap/high/t2 | 0.0825004 | 0.0830533 | +0.7% |  |
| mmbench mmap/high/t4 | 0.03629 | 0.0328507 | -9.5% |  |
| mmbench mmap/high/t8 | 0.0160272 | 0.0115439 | -28.0% **>25% 漂移** |  |
| mmbench mmap/low/t1 | 0.190526 | 0.155674 | -18.3% |  |
| mmbench mmap/low/t16 | 0.00356994 | 0.00306546 | -14.1% |  |
| mmbench mmap/low/t2 | 0.0826706 | 0.0722657 | -12.6% |  |
| mmbench mmap/low/t4 | 0.0361035 | 0.034236 | -5.2% |  |
| mmbench mmap/low/t8 | 0.0160509 | 0.0126727 | -21.0% |  |
| mmbench mmap-pf/high/t1 | 0.0245287 | 0.0208271 | -15.1% |  |
| mmbench mmap-pf/high/t16 | 0.00092601 | 0.000513983 | -44.5% **>25% 漂移** |  |
| mmbench mmap-pf/high/t2 | 0.00472667 | 0.00453385 | -4.1% |  |
| mmbench mmap-pf/high/t4 | 0.00310167 | 0.0026642 | -14.1% |  |
| mmbench mmap-pf/high/t8 | 0.00203699 | 0.00119833 | -41.2% **>25% 漂移** |  |
| mmbench mmap-pf/low/t1 | 0.0245319 | 0.0237457 | -3.2% |  |
| mmbench mmap-pf/low/t16 | 0.000791689 | 0.000499226 | -36.9% **>25% 漂移** |  |
| mmbench mmap-pf/low/t2 | 0.00397015 | 0.00423654 | +6.7% |  |
| mmbench mmap-pf/low/t4 | 0.00335439 | 0.00281557 | -16.1% |  |
| mmbench mmap-pf/low/t8 | 0.00159097 | 0.00108221 | -32.0% **>25% 漂移** |  |
| mmbench pf/high/t1 | 0.0327937 | 0.030398 | -7.3% |  |
| mmbench pf/high/t16 | 0.0210678 | 0.0145068 | -31.1% **>25% 漂移** |  |
| mmbench pf/high/t2 | 0.0215158 | 0.0233371 | +8.5% |  |
| mmbench pf/high/t4 | 0.0436225 | 0.0383359 | -12.1% |  |
| mmbench pf/high/t8 | 0.0418696 | 0.0262343 | -37.3% **>25% 漂移** |  |
| mmbench pf/low/t1 | 0.004185 | 0.00796415 | +90.3% **>25% 漂移** |  |
| mmbench pf/low/t16 | 0.0335499 | 0.0173258 | -48.4% **>25% 漂移** |  |
| mmbench pf/low/t2 | 0.0070048 | 0.00941097 | +34.4% **>25% 漂移** |  |
| mmbench pf/low/t4 | 0.00970738 | 0.028233 | +190.8% **>25% 漂移** |  |
| mmbench pf/low/t8 | 0.038444 | 0.0275871 | -28.2% **>25% 漂移** |  |
| mmbench unmap/high/t1 | 0.0580928 | 0.0529823 | -8.8% |  |
| mmbench unmap/high/t16 | 0.00094581 | 0.000798613 | -15.6% |  |
| mmbench unmap/high/t2 | 0.00702397 | 0.00531053 | -24.4% |  |
| mmbench unmap/high/t4 | 0.00497333 | 0.00332778 | -33.1% **>25% 漂移** |  |
| mmbench unmap/high/t8 | 0.00230889 | 0.00164775 | -28.6% **>25% 漂移** |  |
| mmbench unmap/low/t1 | 0.0602691 | 0.0594269 | -1.4% |  |
| mmbench unmap/low/t16 | 0.000963858 | 0.000580952 | -39.7% **>25% 漂移** |  |
| mmbench unmap/low/t2 | 0.00553709 | 0.00449489 | -18.8% |  |
| mmbench unmap/low/t4 | 0.00446821 | 0.0026225 | -41.3% **>25% 漂移** |  |
| mmbench unmap/low/t8 | 0.00213514 | 0.00140006 | -34.4% **>25% 漂移** |  |
| mmbench unmap-virt/high/t1 | 0.218624 | 0.206485 | -5.6% |  |
| mmbench unmap-virt/high/t16 | 0.00656029 | 0.0057077 | -13.0% |  |
| mmbench unmap-virt/high/t2 | 0.126328 | 0.0922294 | -27.0% **>25% 漂移** |  |
| mmbench unmap-virt/high/t4 | 0.0557513 | 0.0411776 | -26.1% **>25% 漂移** |  |
| mmbench unmap-virt/high/t8 | 0.025576 | 0.0186127 | -27.2% **>25% 漂移** |  |
| mmbench unmap-virt/low/t1 | 0.193832 | 0.204349 | +5.4% |  |
| mmbench unmap-virt/low/t16 | 0.00699679 | 0.0067302 | -3.8% |  |
| mmbench unmap-virt/low/t2 | 0.109961 | 0.101574 | -7.6% |  |
| mmbench unmap-virt/low/t4 | 0.0531767 | 0.0453983 | -14.6% |  |
| mmbench unmap-virt/low/t8 | 0.0225235 | 0.0206165 | -8.5% |  |
| psearchy_eq t8 | 4.009 | 4.766 | -18.9% |  |

> **WARN**: 以下配置 T5 BASE 臂与 M1 基线偏差 >25%（跨 boot 噪声/不同 mitigations/宿主负载；先用 in-boot BASE vs T0 结论，再排查漂移源）。

- jvm t2000x3: base vs M1 -31.1%
- metis_eq t8: base vs M1 -32.7%
- mmbench mmap/high/t8: base vs M1 -28.0%
- mmbench mmap-pf/high/t16: base vs M1 -44.5%
- mmbench mmap-pf/high/t8: base vs M1 -41.2%
- mmbench mmap-pf/low/t16: base vs M1 -36.9%
- mmbench mmap-pf/low/t8: base vs M1 -32.0%
- mmbench pf/high/t16: base vs M1 -31.1%
- mmbench pf/high/t8: base vs M1 -37.3%
- mmbench pf/low/t1: base vs M1 +90.3%
- mmbench pf/low/t16: base vs M1 -48.4%
- mmbench pf/low/t2: base vs M1 +34.4%
- mmbench pf/low/t4: base vs M1 +190.8%
- mmbench pf/low/t8: base vs M1 -28.2%
- mmbench unmap/high/t4: base vs M1 -33.1%
- mmbench unmap/high/t8: base vs M1 -28.6%
- mmbench unmap/low/t16: base vs M1 -39.7%
- mmbench unmap/low/t4: base vs M1 -41.3%
- mmbench unmap/low/t8: base vs M1 -34.4%
- mmbench unmap-virt/high/t2: base vs M1 -27.0%
- mmbench unmap-virt/high/t4: base vs M1 -26.1%
- mmbench unmap-virt/high/t8: base vs M1 -27.2%

## 6. CV 告警（EVAL sec 2.3: >5% 应加测至 5 次）

- jvm t2000x3: cv_base=6.87% cv_t0=12.39%
- metis_eq t8: cv_base=6.64% cv_t0=3.64%
- mmbench mmap/high/t1: cv_base=5.21% cv_t0=6.78%
- mmbench mmap/low/t1: cv_base=10.15% cv_t0=6.98%
- mmbench mmap-pf/high/t1: cv_base=7.68% cv_t0=6.16%
- mmbench mmap-pf/high/t16: cv_base=8.53% cv_t0=7.38%
- mmbench mmap-pf/high/t2: cv_base=5.74% cv_t0=2.91%
- mmbench mmap-pf/high/t4: cv_base=2.19% cv_t0=5.38%
- mmbench mmap-pf/high/t8: cv_base=6.01% cv_t0=6.63%
- mmbench mmap-pf/low/t1: cv_base=1.55% cv_t0=13.75%
- mmbench mmap-pf/low/t4: cv_base=5.65% cv_t0=6.86%
- mmbench mmap-pf/low/t8: cv_base=2.91% cv_t0=6.13%
- mmbench pf/high/t1: cv_base=13.35% cv_t0=19.57%
- mmbench pf/high/t16: cv_base=3.13% cv_t0=6.68%
- mmbench pf/high/t8: cv_base=18.18% cv_t0=6.08%
- mmbench pf/low/t1: cv_base=22.7% cv_t0=75.53%
- mmbench pf/low/t16: cv_base=16.07% cv_t0=8.72%
- mmbench pf/low/t2: cv_base=73.07% cv_t0=64.14%
- mmbench pf/low/t4: cv_base=52.51% cv_t0=69.89%
- mmbench pf/low/t8: cv_base=8.11% cv_t0=2.57%
- mmbench unmap/high/t2: cv_base=4.23% cv_t0=6.54%
- mmbench unmap/high/t8: cv_base=10.18% cv_t0=2.82%
- mmbench unmap/low/t16: cv_base=8.47% cv_t0=0.82%
- mmbench unmap/low/t2: cv_base=6.91% cv_t0=10.88%
- mmbench unmap/low/t4: cv_base=17.7% cv_t0=3.86%
- mmbench unmap/low/t8: cv_base=5.66% cv_t0=10.07%
- mmbench unmap-virt/high/t4: cv_base=7.25% cv_t0=1.02%
- mmbench unmap-virt/low/t16: cv_base=12.25% cv_t0=1.24%
- mmbench unmap-virt/low/t4: cv_base=7.12% cv_t0=1.7%
- mmbench unmap-virt/low/t8: cv_base=9.83% cv_t0=12.55%
- psearchy_eq t8: cv_base=15.95% cv_t0=3.46%

## 7. 结论速览

- PARAM-MISMATCH: 0（exit 2 口径）
- 覆盖缺口: 0
- 失败/不可解析记录: 0
- strace differs: 26
- G1 预判定: MET
- 报告退出码: 1（0=干净, 1=数据问题, 2=参数不一致）

