# M4.T5 第二跑（r06-rogue 重跑）—— 首个真实全矩阵数字（2026-09-18）

- 班次: ~13:10–14:35 CST（QUICK 冒烟 → 全矩阵 05:42–06:04 UTC, 1260s, deadline 5400s 未触发）
- 内核: **bzimg/r06-rogue/bzImage**, tag corten-r06-rogue, HEAD=e219920b0923 (D16)。
  bzImage sha256 `c767ab30…50c061b`。
  **版本串异常（不影响内容）**: 镜像 `file`/uname 报 `6.18.32-gba77046c78fe-dirty #42 (12:46:02)`
  ——12:46 从 dirty worktree 构建（当时 D16 尚未提交, 12:51:28 才 commit）, 版本串为陈旧锚。
  **内容已验证 = D16**: extract-vmlinux 后 D16 新增 kallsyms 符号
  `corten_arena_test_fork_demote_perm` / `corten_fault_test_zap_keep_perm` /
  `corten_fault_test_zap_keep_perm_downgrade` 三者全在; 后继提交 025756094542 (arm64,
  13:04) 的新串 "construction is arch-specific" 不在 —— 镜像即 e219920 快照。
- boot: `corten=on mitigations=off kunit.enable=0`（与首跑逐字同参 + fsck 修复参, 见 §6 异常）,
  trixie 8vCPU/4G/KVM; 宿主 i9-13900HX。
- KUnit 记录: 本测 boot 按 T5 口径 kunit.enable=0; r06-rogue 镜像的 KUnit 证据 =
  share/r06-rogue-kunit-on.log（同 #42 镜像, kunit.enable=1 boot）: 3 suite（corten /
  corten_arena / corten_fault）合计 **73 ok / 0 not ok**, 含 D16 新增
  fork_demote_perm、zap_keep_perm、zap_keep_perm_downgrade; 4 条 WARNING 均为通过中
  负路径用例的故意断言（corten_txn_begin ×2, corten_arena_fork_demote rwsem 断言 ×2）。
- 协议: bench/t5/README.md（EVAL §1-3 权威; ABAB; 3 中位; M1 参数锚）。参数审计
  **327/327 PASS, PARAM-MISMATCH=0, 覆盖缺口 0**（analyze_t5.py exit=1 为数据项, 见 §4/§5）。
- 产物: results/r06/t5-run2/（raw 330 条单行 JSON + trace 55 对 + meta + driver.log +
  t5_summary.json + t5_report.md）; 冒烟件 share/t5quick3-rogue.tgz（QUICK, 391s,
  35 配置, 仅 runner 健康口径不可外引）。
- 宿主声明: 本班与另一 agent 共享宿主/VM（见 §6）, 测量窗（05:42–06:04 UTC）内本 VM
  独占 8 vCPU; 邻道 agent 在宿主侧的构建活动无法排除, t=1/t=2 组 CV 偏高或受其影响。

## 0. 环境与 hook 生效（ runner 健康 ）

- 冒烟轮 QUICK=1 PASS: 35 配置 0 崩溃腿（JVM T0 为登记 ClassFormatError, §3）,
  dmesg 0 warn; 全矩阵同。
- hook 生效（全矩阵 arena_stats before→after）: auto_mmaps 0→**155,523**,
  mprotect_routes 0→**186,484**, munmap_releases 0→**154,958**, fork_demotes 0→**563**,
  rearm_recovered 0→**634** —— T0 臂 MODE 路径真实在工作。
- 全程 dmesg `corten.*(warn|bug|oops)` = **0**（meta/corten_dmesg_count.txt）。

## 1. G1 预判定: **NOT MET（1/4, 较首跑 0/4 逼近一档）**

低竞争 t∈{4,8} 需 ≥2 项 t4/t8 同时 ≥10%（EVAL sec 6）:

| bench | t4 Δ% | t8 Δ% | 同时≥10%? | 首跑对照 (t4/t8) |
|---|---|---|---|---|
| mmap-pf | **+13.89** | +0.53 | 否（t8 平） | -2.65 / **+17.90** |
| pf | +1.06† | -0.97 | 否 | +258.70††(方差否决) / -0.52 |
| unmap | -6.01 | -13.68 | 否 | -5.83 / +7.77 |
| unmap-virt | **+48.27**† | **+11.41** | **YES** | **+17.13** / +5.57 |

† 噪声警示: unmap-virt/low/t4 base CV 24.6%、pf/low/t4 base CV 29.0%（登记的 pf/low 高
方差族延续, M8 需 5 次加测口径）; unmap-virt 过线对中 t8 一侧 +11.41 干净（CV 10.3/13.2）。

**逐项对照首跑的三个信号**:
- mmap-pf low t8: 首跑 +17.9 → 本轮 **+0.53（信号消失）**; 但 mmap-pf low t4 由 -2.65
  翻为 **+13.89**（新信号）。
- unmap-virt low t4: 首跑 +17.1 → 本轮 **+48.27**, 首次与 t8（+11.41）**成对过线** ——
  该组成为本轮唯一 qualifying 项。
- unmap high t4: 首跑 +18.9 → 本轮 **+2.85（信号消失）**; unmap 组整体转负
  （low t8 -13.68 为本组最大回退）。

结论: G1 仍 NOT MET（1/4）, 但**项形态从首跑的"单项孤信号"变为"成对过线"**;
unmap-virt 组跨两轮方向一致（t4 均大幅正, t8 由 +5.6 升至 +11.4）, 是 M8 前最值得
5 次加测固化的组; mmap-pf/unmap 组信号不稳定, 归 M1 基线同源的跨 boot 噪声候选
（§5: 12 项 BASE vs M1 >25% 漂移, pf/low 三格 +358%~+916% 为登记方差族）。

## 2. G3 预判定: **MET —— dedup_eq tcmalloc 档 +11.32%（本轮 app 首次全部可跑完）**

| workload | base med | t0 med | Δ% | 首跑对照 |
|---|---|---|---|---|
| dedup_eq t8/tcmalloc | 5.067 M blk/s | 5.640 M blk/s | **+11.32** (CV 0.49/1.73) | 首跑 **-91.84%** 崩溃级回退（D15/D16 前后对照） |
| dedup_eq t8/glibc | 4.518 M blk/s | 4.581 M blk/s | +1.41 | 首跑 n/a（T0 3/3 rc=139） |
| metis_eq t8 | 16.52 s | 16.80 s | -1.72 | 首跑 n/a（T0 3/3 rc=139） |
| psearchy_eq t8 | 4.27 s | 5.98 s | -40.05† | 首跑 n/a（T0 3/3 rc=139） |
| jvm t2000x3 | 2340.7 ms | n/a（登记 soft-fail） | - | 首跑 T0 3/3 rc=137（>480s 楔死） |

† **不可引用**: psearchy 两臂 rep1 均为暖机污染态（base [8.73, 4.11, 4.27] / t0 [7.05,
5.98, 4.25]; 逐 run 配对: run1 base 8.73 vs t0 7.05 为 T0 更优）, CV base 37.5% / t0
20.0%, 触发 EVAL §2.3 加测口径 —— 按协议归**方差未决**, 非 MODE 回退证据; QUICK 轮同
 workload 方向相反（base 14.2 vs t0 5.2）佐证暖机主导。M8 前 5 次加测。

**G3 判据落点**:
1. **数字过线**: dedup_eq tcmalloc 档 **+11.32%**（≥10%, 两臂 CV <2%, 三 run 无重叠:
   base [5.066, 5.119, 5.067] / t0 [5.746, 5.508, 5.640]）——brief 预期 "+19%", 实测
   +11.32%, 仍过线。首跑 -91.84% 崩溃彻底消除, D15+D16 修复在 app 级坐实。
2. **机制解释（present-RO 族闭合的直接证据）**: 首跑三件 glibc 档 T0 腿 9/9 rc=139
   （libc segfault, "RW 提交区装 present-RO PTE"）→ 本轮 **9/9 rc=0 全部跑完**, 全程
   dmesg 零 warn —— 登记缺陷族在本内核上未复现。
3. JVM（soft-fail 口径）: base 3/3 正常（2340.7 ms, 与 M1 2347.3 ms 偏差 +0.3%）; T0
   3/3 在**跑完 2000 线程×3 rep 之后**于收尾 printf 处死（`ClassFormatError: Unknown
   constant tag 0 in class file sun/text/resources/cldr/FormatData_en`, 登记;
   hook fork-probe 3/3 OK）。与首跑"MODE 下 2000 线程 spawn >480s 楔死"相比, spawn
   与执行已完全收敛（每腿 ~5 s）, 仅存登记的收尾 CDR 资源类加载失败。

## 3. G4 非 MM 回退 ≤5% 检查: **MET（1 项方差未决, 如实记录）**

- metis_eq -1.72%、dedup_eq glibc +1.41%、dedup_eq tcmalloc +11.32%（改善）—— 全部
  |Δ|≤5% 或正向, ✓。
- psearchy_eq 中位 -40.05% 表面超限, 但如 §2 所述为两臂 rep1 暖机 + CV>5% 的方差未决
  项, 协议口径下**不构成可引用的回退证据**; M8 以 5 次加测定案。
- JVM 为登记 soft-fail, 不参与。

## 4. strace 抽样（55 对, 5s 窗 multiset diff）

- 严格口径: 27 equal / **28 differs**（首跑 28/27, 总体同形）。
- **28 对 differs 逐一核过 .err.diff**: 26 对仅 EAGAIN 计数抖动（futex 竞争 5s 窗采样
  噪声, 与首跑同族）; dedup_eq_tcmalloc 差 1 条 ETIMEDOUT 计数（23→24, 同类抖动）;
  jvm 多 1 条 ESRCH（线程收尾 kill 竞争, 首跑已登记同形）。
- **零 arena 语义新错误**: 无 EACCES/EFAULT/ENOMEM/EPERM/EINVAL 新类/新形状
  （EINVAL/ENOENT/ENOTTY/EEXIST 出现处均为两臂同在的计数抖动）。机制语义口径干净,
  与首跑结论一致（建议的窗长/类别集判据改进继续有效）。

## 5. M1 跨 boot 对照（参考非 gate）

- BASE 臂 vs M1 中位: app 四件全部 |偏差|≤6.5%（dedup -5.9% / tcmalloc -1.9% /
  metis -4.7% / psearchy -6.5%）, jvm +0.3% —— app 基线高度可复现。
- mmbench 12 项 >25% 漂移, 集中 pf/low（t1/t2/t4 = +916%/+505%/+358%, base CV
  47.7/32.9/29.0）—— M1 已登记 pf/low 高方差族 + 首跑同形; 其余为 mmap-pf/unmap-virt
  个别 t2/t4 格。结论一律以 in-boot BASE vs T0 为准。
- CV>5% 告警 47 项（清单见 t5_report.md §6）, 与首跑 28 项同因（t1/t2 小格宿主噪声 +
  pf/low 族）。

## 6. 异常与处置（班内如实记录）

1. **镜像版本串异常**: bzimg/r06-rogue 版本串为 ba77046-dirty #42（构建于 D16 commit
   前 5 分钟的 dirty 树）。经 extract-vmlinux 符号级验证内容 = e219920（§0 顶部三条
   D16 符号在、后继 arm64 提交串不在）。**建议规划者: 后续镜像打包应在 commit 后重编
   或记录 -dirty 语义, 避免审计歧义。**
2. **邻道 agent 冲突（一次重试用尽）**: 首次全矩阵（13:33 起, ~2 min）被另一 agent 的
   `launch_vm.sh`（其 tmux kill-session + 重建 "vm" 会话, 13:36:00 起 qemu, 跑
   025756094542 #43 = arm64 提交）杀死, 9p writeback 缓存未落盘, 首跑数据全失。重试
   措施: 会话改名 vm-t5run2（脱离其 kill 范围）+ 占用 qemu.pid（其 launch 因 "VM
   already running" 拒绝双起）+ 9p cache=none（腿级结果即时落盘, 可抗 qemu 暴毙）+
   fsck.mode=force fsck.repair=yes（其被杀 boot 留下脏 rootfs 曾使其陷入 emergency）。
   重试成功, 1260s 完整收工。
3. **runner 空 symlink FATAL（重试前自伤, 已修）**: 我 `rm -rf` 目标目录导致
   /mnt/t5run2 符号链悬空, runner mkdir 失败 FATAL 一次; 重建目标目录后启动成功。
   非脚本缺陷, 操作教训。
4. 首跑遗留项核对: metis/psearchy/dedup glibc T0 rc=139（present-RO 族）**本轮 0 命中**
   （登记缺陷未复现, 即其修复态）; JVM ClassFormatError（登记既有）如约出现, soft-fail
   口径; JThreadBench 在 MODE 下 spawn/执行已正常（首跑的 >480s 楔死未复现）。

## 7. 判定汇总

| Gate | 判定 | 依据 |
|---|---|---|
| G1（低竞争 t4/t8 ≥2 项 ≥10%） | **NOT MET**（1/4） | 仅 unmap-virt 成对过线（+48.27†/+11.41）; mmap-pf t4 +13.89 但 t8 平; 较首跑 0/4 逼近 |
| G3（app ≥1 项 ≥10% 或机制解释） | **MET** | dedup_eq tcmalloc **+11.32%**（CV<2%）; 且 glibc 三件 9/9 rc=0（首跑 9/9 崩溃）, present-RO 族闭合 |
| G4（非 MM 回退 ≤5%） | **MET** | metis -1.72 / dedup glibc +1.41 / tcmalloc +11.32; psearchy -40.05 为方差未决（rep1 暖机, CV 37.5/20）, 协议下不可引用 |
| 参数一致性 | PASS 327/327 | PARAM-MISMATCH=0, 覆盖缺口 0 |
| 机制语义（strace） | 干净 | 28 differs 全部 futex/ETIMEDOUT/ESRCH 采样噪声, 零 arena 语义新错误 |
| dmesg | 干净 | 全程 corten warn/bug/oops = 0 |

## 8. M8 前行动项（沿袭 + 新增）

1. unmap-virt low {t4,t8} 5 次加测固化（两轮方向一致的最佳 G1 候选）;
2. psearchy_eq 5 次加测（本轮 -40.05% 方差未决, 需定案是否真实 MODE 开销）;
3. pf/low 全组沿用 5 次口径（+358%~+916% M1 漂移与首跑同族）;
4. JVM 收尾 ClassFormatError（登记既有）在 M8 口径内维持 soft-fail 或单独归档;
5. 镜像打包流程: commit 后重编, 消除版本串 -dirty 歧义（§6.1）。

—— 时间盒内收工; 未 push、未动 worktree（读侧验证除外）; 密码未落盘; VM 留运行
（会话 vm-t5run2, pidfile /home/ppw/vm/qemu.pid, 内核 = bzimg/r06-rogue）。
