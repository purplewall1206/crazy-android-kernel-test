# M4.T5 run5 终验判定 —— 最终内核（A5 惰性 registry）全矩阵 + G5 门复核（2026-09-21 通宵收官班）

- 班次: 05:40–07:4x CST（构建 11 min 含一次重试 → off 冒烟 → corten=on 重启 → QUICK 469 s → 全矩阵 1244 s → G5 复核 ~12 min → 回归双腿 → 报告; 全程 timegate 免费窗内）。
- 内核: **bzimg/r07-t5run5/bzImage-6.18.32-gb9541335a554-93-t5run5**, sha256 `24a33880…5ab33`
  = 主树 HEAD **b9541335a554**（tag corten-r07-a5, **干净树直编 #93**, 版本串 `6.18.32-gb9541335a554`
  与 HEAD 精确一致, 消除 #91 "g2639d3294b9d-dirty" 串）。`make olddefconfig` = **No change to .config**
  （四配置核对: CORTEN_MM=y + CORTEN_MM_ARENA=y + KUnit 测试项 ×3 / ZRAM=y+BACKEND_LZ4=y+DEF_COMP=lz4 /
  KUNIT=y / LOCALVERSION_AUTO=y）; 增量构建**零新增告警**（仅登记基线 objtool cpuidle + modpost memblock 2 条）。
- boot: `corten=on mitigations=off kunit.enable=0 log_buf_len=64M fsck.mode=force fsck.repair=yes`,
  trixie-m4t12.img 8 vCPU/4G/KVM; VM = tmux **vm-t5run5**（port 10026, pidfile qemu-t5run5.pid）**留运行**。
- off 臂 green 门（t5final 同形 append, 无 corten 参数）: **GREEN** —— 登录 OK + zram lz4 2G prio100
  + gssh/9p OK + mmbench_dyn rc=0 JSON 有效 ops_per_us=0.00378719（mmbench_dyn sha `38304f062d43`
  = run3/run4/t5final 动态口径同件）+ dmesg corten 行=0、Oops/BUG/WARNING/lockdep=0。
  证据 off-smoke/ + bzimg/r07-t5run5/green.txt。
- 口径: run_t5_compare.sh 全矩阵（M1 参数锚, threads 1-16 × 3 reps × ABAB-双臂 × 35 配置, strace 每配置,
  deadline 5400 s **未触发, 实际 1244 s**）; MMBENCH_BIN=/root/m4t12/bin/mmbench_dyn（D1 动态口径）;
  判定一律 in-boot BASE vs T0。邻道管理: basecheck/m5t3-vm/m6t2-vm 测量窗 SIGSTOP、收班 SIGCONT 恢复;
  "vm"(10022) tmux 持续 CONT 不可停 = 登记噪声级（run4/t5final 同形）。

## 0. runner 健康（全部过线）

| 断言 | 结果 |
|---|---|
| deadline | 1244 s / 5400 s, 无 truncation, 35 配置 × 2 臂 × 3 reps 全跑 |
| 失败腿 | **0**（"FAIL:" 计 0; driver FAIL=1 仅 strace differs 计数） |
| T0 ENTER marker | 全矩阵 **110/110** t0 腿 stderr 含 "MODE on"（missing=0）; QUICK 70/70 同 |
| hook 生效（QUICK 起 → 全矩阵后） | munmap_releases +11.22M / pool_parks +11.22M（命中 99.998%）/ mprotect_routes +301K / auto_mmaps +2801 / fork_faithful 88; **drain_timeout 0→0**; pool_misses 2807 / over 186 / **ejects 0**; **meta_arrays 331→305、ptdescs==meta_arrays 双端相等（零泄漏, 负增长）**; swapped_out 0 / shrink_scans 0（无压窗, 按设计） |
| dmesg / console | `corten.*(warn|bug|oops)` = **0**; Oops/BUG/WARNING/lockdep = **0**（console -S3000 全窗扫描同零） |
| 参数一致性 | **330/330 PASS**（analyze_t5.py: seed 公式/线程/语料/时长/allocator/JVM 形参全同 M1 协议） |
| strace 语义 | 55 对: 29 equal / 26 differs / 0 skip; differs 逐条核对 **+侧仅 EAGAIN×18、ETIMEDOUT×2 抖动 + jvm ESRCH×1（线程收尾竞争, run2/3/4 登记同族）**; **零 EACCES/EFAULT/ENOMEM/EPERM 新类** |

## 1. M4.T5 终验判定表（本跑正式数 vs run4 对照）

| Gate | 判据 | **run5 终验（b9541335a554）** | run4 对照（802ff755） | 判定 |
|---|---|---|---|---|
| **G1** | 低竞争 t4&t8 同时 ≥+10% × ≥2 项 | **unmap +61.18 / +138.67**（CV 17.7/3.9, 5.7/10.1）+ **unmap-virt +1156.14 / +2326.53**（CV 7.1/1.7, 9.8/12.6）成对过线 = **2/4** | MET 2/4: unmap +33.51/+143.48, unmap-virt +1156.11/+2791.86 | **MET（维持）** |
| **G3** | {metis, dedup-glibc, jvm(aux)} ≥1 项 ≥+10% 或机制解释 | metis **+8.85**（CV 6.6/3.6）/ dedup glibc **+3.11** / tcmalloc 对照档 **+8.73**（三 run 无重叠）/ jvm -4.57（aux） | NOT MET 数字面: dedup +4.60/+6.33, metis -0.88 | **NOT MET 数字面（apps 全正无回退, 幅度 <10%）** |
| **G4** | 非 MM 回退 ≤5% | metis +8.85 ✓ / psearchy +2.10 ✓ / dedup 双档 +3.11/+8.73 ✓ / **jvm -4.57%（线内）** | 边缘未过: jvm -6.59% 超线 1.6pp | **MET（A5 残差收窄兑现: -6.59 → -4.57, 收窄 2.0pp 入线）** |
| **G5** | lat_proc 三件套 ×3 中位, MODE ≤+30% | **fork -10.65% / fork+exec +3.77% / shell +0.52%（worst +3.77%, 全线 ≤30%）** | （A5 验证班 -0.3/+14.5/+12.8 同族） | **MET（A5 后正式数, fork 转负）** |

## 2. G1 详表 —— run5 vs run4 vs t5final（动态口径 in-boot BASE vs T0, 中位）

| 格 | run4（3 rep） | t5final（5 轮固化） | **run5（3 rep 终件）** | 读法 |
|---|---|---|---|---|
| unmap low t4 | +33.51（CV 10.6/2.9） | +45.38（28.5/5.0） | **+61.18（17.7/3.9）** | 过线项持续走强 |
| unmap low t8 | +143.48（2.2/2.2） | +141.99（4.8/5.5） | **+138.67（5.7/10.1）** | +140% 族稳定 |
| unmap-virt low t4 | +1156.11（7.3/3.7） | +1142.34（5.0/1.6） | **+1156.14（7.1/1.7）** | 与 run4 逐位吻合 |
| unmap-virt low t8 | +2791.86（3.5/3.3） | +2583.03（1.4/2.0） | **+2326.53（9.8/12.6）** | +2300~2800 族 |
| mmap-pf low t4 | -8.76 | -17.33 | -21.47（5.7/6.9） | fault 通道机制成本家族（登记口径） |
| mmap-pf low t8 | -17.09 | -22.29 | -12.05（2.9/6.1） | 同上, 幅度与本族历史轮次同带 |
| pf low t4 / t8 | +37.04 / -9.65 | -7.18 / -3.82 | -69.70（CV 52/70）/ -1.40 | 高方差噪声族（三班同登记, 中位不判读） |

- 全 30 格横览: unmap 族 low/high 全正（high t16 +106%）, unmap-virt 全 16 格大正（high t16 **+6680%**）,
  mmap 全族 +94~+269%（T1c 池生命周期税消除维持）; **MODE 侧 unmap-virt 绝对值 ~0.50-0.57 ops/µs
  与 run4/t5final 同族（~0.64/0.66 → 本轮两臂同步小幅下移, 比值稳定 = boot 级共同漂移, in-boot 判读不变）**。
- **G1 三轮演化终态**: T1c 池（1/4→池化）→ perf1 TLB 风暴修复（2/4 首次 MET）→ A5 惰性 registry（**2/4 维持,
  无 A5 交互回归**）。unmap/unmap-virt 两族 × 三班 5+5+3 轮全部正号, MET 结论固化。

## 3. apps 详表（全部 3 rep rc=0）

| workload | base med | t0 med | Δ% | CV b/t0 | run4 对照 |
|---|---|---|---|---|---|
| dedup_eq t8/glibc | 4.028 M blk/s | 4.153 M | **+3.11** | 2.2/2.3 | +4.60 |
| dedup_eq t8/tcmalloc | 4.233 M | 4.603 M | **+8.73** | 2.7/1.9 | +6.33（t0 三 run [4.588,4.603,4.785] 全高于 base 侧 max 4.458, 无重叠） |
| metis_eq t8 | 20.923 s | 19.071 s | **+8.85** | 6.6/3.6 | -0.88（本轮 rep1 base 22.2 s 暖机腿在族） |
| psearchy_eq t8 | 4.766 s | 4.666 s | +2.10 | 15.9/3.5 | +3.18 |
| jvm t2000x3 | 3077.9 ms | 3218.6 ms | **-4.57** | 6.9/12.4 | -6.59 |

- **apps 五件四正一负、无一回退**; A5 预期"apps 维持正收益"成立。dedup tcmalloc 双档无重叠正向、
  metis 转正（run4 小负 → +8.85, 接近 10% 线）、psearchy 同族。
- **jvm 残差收窄且入线（G4 焦点项）**: -6.59% → **-4.57%**。逐 run 符号混合（+19.6/-18.9/+14.2%）,
  分布重叠（base [2819,3078,3337] vs t0 [2708,3219,3680]）= JVM spawn 噪声族观测项维持, 但幅度
  收窄方向与 A5 消除 per-lifecycle GP 的机制预期一致（MODE 2000 线程生命周期 × 每退出一次 GP 的
  定价已消失, 残余为预载 + 簿记噪声）。**M1 跨 boot 对照**: 本 boot BASE 侧 jvm/metis 绝对值较 M1
  漂移 -31/-33%（登记的跨 boot 漂移族, 一律以 in-boot 为准, 与 run4 同读法）。

## 4. G5 门复核（同 boot, A5 后正式数）

协议: lmbench lat_proc 三件套 ×3 rep 臂级交错 × 两臂; **真 no-probe STRICT hook**
（share/a5fix/corten_mode_hook_true_noprobe.c 现编 `/root/g5hook_true_noprobe.so`
sha256 `5c38a02c…74b74`; 逐腿 "MODE on" marker 断言 + 逐腿零 fork-probe 行断言, 双双全过;
g5gate 旧 noprobe 件 = 已登记名不副实污染源, 未使用）。

| op | BASE 3 rep (µs) | 中位 | MODE 3 rep (µs) | 中位 | **Δ** | A5 验证班对照 | 判据 |
|---|---|---|---|---|---|---|---|
| fork | 1563.9 / 1820.2 / 1565.5 | 1565.5 | 1497.6 / 1398.7 / 1370.7 | 1398.7 | **-10.65%** | -0.3% | ≤30% ✓ |
| fork+exec | 4834.5 / 3982.0 / 4076.3 | 4076.3 | 3928.3 / 4523.1 / 4229.8 | 4229.8 | **+3.77%** | +14.5% | ≤30% ✓ |
| shell | 7779.5 / 9735.8 / 8506.2 | 8506.2 | 7776.1 / 8550.8 / 12094.7 | 8550.8 | **+0.52%** | +12.8% | ≤30% ✓ |

**G5 = MET（worst +3.77%, 全部远离 30% 线）**。A5 惰性 registry 在终件上复现: ENTER 零建 registry、
空 state 退出 call_rcu 异步释放, 每-MODE-生命周期 GP 定价消失; fork op 本轮转负（MODE 快于 BASE 10.7%）。

## 5. 已知失败口径复核（任务两项, 均闭合）

| 项 | 预期 | run5 实况 | 判定 |
|---|---|---|---|
| JThreadBench ClassFormatError（gupfix） | rc=0 零 CFE | 全矩阵 jvm 双臂 3/3 rc=0 零 CFE; **独立回归腿 MODE ×1: rc=0、CFE 0、fork-probe OK ×1、median 2756.9 ms（族内）** | 闭合维持 |
| metis_eq fork 残余（OQ-D） | rc=0 + checksum 双臂同值 + fork-probe OK | **回归双腿: BASE rc=0 checksum `8a8db99075665220` / MODE rc=0 checksum 同值（= lineage 登记值）+ fork-probe OK + fork_faithful 前进（矩阵 +72, 回归后再进）** | 闭合维持 |

## 6. 异常与处置（如实记录; 无 panic, 重试额度按序消耗）

1. **构建首跑失败（fs/nfs fixdep 竞态, Error 123/缺 .o.d）**: 版本串 dirty→clean 触发大范围重编时的
   瞬时 flake; 磁盘/内存正常、无 OOM。**消耗一次重试**: 复跑 make -j12 一次通过（#93）。非内核问题。
2. **off 冒烟首跑口径偏差**: 首个 off boot 未带 `mitigations=off kunit.enable=0`（KUnit 自测行 +
   2 条登记基线注入 WARNING 入 dmesg）。核对 t5final off-console 后按同形 append 重启复跑 → 全绿。
   内核本身零异常（首跑 dmesg 亦无 cortex 运行时行）。
3. **9p hostshare 未随 boot 挂载**: 重启后 /mnt/hostshare 为空 → guest 手动
   `mount -t 9p -o trans=virtio hostshare /mnt/hostshare` 后全通（历次班为手动挂, fstab 无此项;
   登记为环境已知项）。
4. **G5 脚本两次前台超时截断**（300 s/590 s timeout 参数不足, 18 腿实际需 ~12 min）: 第三次改 guest
   内 detached 重跑, 一次性干净跑完 18/18 腿。前两次截断件即弃（未混入终数）。
5. **G5 内嵌汇总 glob bug**: 脚本 python 汇总按 `rep?*` 招文件, 实际落盘为 `_1/_2/_3` → 汇总 median=None
   的空转 MET。**测量数据本身完整有效**; 宿主侧按 raw 文件重新汇总（g5/run5/g5_summary_fixed.json）,
   share 脚本 glob 已修正留档。
6. **本地 ssh 挂起 ×2**（QUICK 与 G5 detached 启动; run3/4 登记同形）: 断开本地 ssh 即可, 远端进程无恙。
7. 密码未落盘; 未 push; 未动内核 worktree（主树仅构建产物）; 判废/截断件均留档可审计。

## 7. M4 里程碑总判定建议（基于四门终态）

**建议 M4 = PASS（性能故事收官成立）**, 依据:

1. **G1 MET（2/4）三班固化**: run4 首 MET → t5final 五轮加固 → run5 终件维持。unmap +61/+139、
   unmap-virt +1156/+2327 成对过线, 三班跨 13 个测量轮次全部正号; mmap-pf 残差 -12~-21 保持在
   perf1 登记的"fault 通道机制成本"家族带宽内（非风暴性回退, profile 两臂同形归因不变）; pf = 噪声族。
2. **G4 MET**: 唯一超线项 jvm 从 run4 -6.59% 收窄至 **-4.57% 入线**, 方向与 A5 机制预期一致;
   apps 其余四件全正, 非回退面干净。
3. **G5 MET（正式数）**: 三 op ≤+3.77%（fork 转负）, MODE 进程生命周期税在终件上归零级。
4. **G3 NOT MET 数字面（不阻塞）**: metis +8.85 / dedup 双档 +3.11/+8.73 全正但无单项 ≥10%。
   机制解释维持: app 面增益定价于 T1c 池消生命周期税 + A5 消退出 GP（G5 的 fork -10.65% 即其
   lat_proc 投影）, churn 增量在 mmbench 形状（G1）兑现; dedup tcmalloc 三 run 无重叠 +8.73 为
   最接近线的真实增益。如实披露, 不以 G3 单独否决里程碑。
5. **正确性/语义面零新欠账**: 330/330 同参、strace 零新错误类、dmesg/console 双零、零泄漏台账
   （parks≈releases 99.998%、drain_timeout 0、ejects 0、meta_arrays 负增长）、两项已知失败口径
   （JTB CFE / metis fork）再证闭合。
6. 遗留登记（M8 观测项, 均非阻塞）: jvm 噪声族（分布重叠、符号混合）; mmap-pf 机制成本家族;
   pf 高方差族; A5 后 /mnt 旧 smoke binary 既有伪影（m6t34 登记, 非 A5）。

—— M4.T5 run5 收工: **终件 = 主树 b9541335a554 直编 #93（sha 24a33880…）, G1 MET（2/4, 维持）、
G3 NOT MET 数字面（apps 全正无回退）、G4 MET（jvm -4.57% 收窄入线）、G5 MET（worst +3.77%）;
正确性面 330/330 + strace 干净 + 台账零泄漏 + 两项已知失败闭合**。全部原始数据
results/r07/t5-run5/ 可复核; VM 留运行（vm-t5run5, port 10026, 内核 = r07-t5run5 #93 = b9541335a554）。
