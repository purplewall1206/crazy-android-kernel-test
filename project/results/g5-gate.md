# G5 门测量 —— lat_proc fork/fork+exec/shell BASE vs MODE（M5.T4，r07 终件，2026-09-21 通宵班）

- 班次: 03:0x–04:0x CST（timegate 放行；VM 复用 g1 班的 vm-t5final, port 10026, 未重启）。
- 内核: **bzimg/r07-t5final = 主树 HEAD `2639d3294b9d`**（tag corten-r07-m6t34, 构建 #90, sha256
  `97e90f34…f68f78`）`6.18.32-g2639d3294b9d`, boot `corten=on mitigations=off kunit.enable=0`；
  trixie-m4t12.img 8 vCPU/4G/KVM。**M5.T1a 忠实 fork 已在树** —— 本次是 fork 语义修复后的首个正式 G5。
- 工具: guest `/usr/lib/lmbench/bin/x86_64-linux-gnu/lat_proc`（ldd 实证动态链接, LD_PRELOAD 可注入;
  与 M1 基线同路径同件）；`/var/tmp/lmbench/hello` 在位；hook = `bench/mode-hook/corten_mode_hook.c`
  guest 现编（ENTER+GET 断言, `CORTEN_MODE_HOOK_STRICT=1`, 逐跑 marker 断言）。
- 方法: M1 基线同款（results/r01/baseline/run_latproc.sh）——fork / exec(=fork+exec) / shell 三 op,
  ×3 rep 中位, **两臂 rep 级交错**（BASE=`env -u LD_PRELOAD` ↔ MODE=`LD_PRELOAD=… STRICT=1`,
  同 boot 内配对）。产物: `results/r07/g5-gate/`（本报告 + run1/run2 raw + jtb/ + arena_stats 快照）。

## 0. 测量有效性（两轮: run1 污染判废 → run2 干净定数）

**run1（判废存档, raw/run1/）**: MODE 臂 stderr 落 9p 文件, 且 hook 的 atexit fork-probe 被
fork 子进程继承（atexit 列表随 fork 复制）——每个被测子进程退出时多跑 fork+waitpid+fprintf,
stderr 又走 9p 同步写。读数 fork 24.9ms / shell 87.3ms 中位, 污染未排除, **判废留档**。

**run2（干净口径, raw/run2/）**: 两个修正 ①NOPROBE hook 变体（仅去 atexit 注册;
ENTER/GET STRICT 断言保留, probe 职能由 T0-DoD/t0dod 系列继续覆盖）②两臂 stdout+stderr
对称落 guest 本地 /tmp 后回拷。3 rep 全部 `MODE on` marker 断言过, rc=0。

- 诊断记录: run1 fork 臂 .err 仅 54 行（≈迭代数次 probe 行）, 排除"千行 fprintf"量级污染后
  run2 仍复现回退 → 回退为**内核真实成本**, 见 §2 根因。

## 1. 正式数字（run2, 3 rep 中位, 同 boot BASE vs MODE, 单位 µs）

| op | BASE 3 rep | **BASE 中位** | MODE 3 rep | **MODE 中位** | **Δ (MODE vs BASE)** | M1 基线（跨 boot 参考, publish/baseline/） |
|---|---|---|---|---|---|---|
| fork | 1349.6 / 1696.9 / 1924.4 | **1696.9** | 9979.5 / 10454.1 / 11289.5 | **10454.1** | **+516.0%** | 1298.4 |
| fork+exec | 4480.5 / 4932.1 / 5377.6 | **4932.1** | 13241.0 / 13402.2 / 14816.7 | **13402.2** | **+171.7%** | 2968.7 |
| shell | 14261.5 / 11472.0 / 17328.0 | **14261.5** | 62303.3 / 64679.8 / 76142.8 | **64679.8** | **+353.5%** | 9874.0 |

（BASE 侧跨 boot 对照 M1: fork +30.7% / exec +66.1% / shell +44.4% —— 本 boot 宿主 4 VM 并行 +
内核树含 25 个 corten 提交, 仅作环境记录; **G5 判据 = 同 boot MODE vs BASE**, 与 M1 无关。）

## 2. 判定与根因（机制级证实, 非猜测）

### G5 判定: **NOT MET**（论文口径 fork 单线程回退 ≤30%; 实测 +516%/+172%/+354% 全部超线）

但**回退与忠实 fork 镜像无关**（零 arena fork 是 E1 no-op——源码 `corten_arena_fork_begin()`:
`old_state = smp_load_acquire(&oldmm->corten_state); if (!old_state) return 0;`, 且本次全程
`fork_faithful` 34→34 不变、`fork_demotes=0`）。根因 = **MODE 生命周期的每-mm 退出 RCU 宽限期**:

1. `corten_arena_mode_enter()`（mm/corten_arena.c:2292）在 prctl ENTER 时即经
   `corten_arena_get_state()` **急切建 registry**（"creating it here keeps the hot path
   allocation-free"）→ JOIN `corten_mm_registry`（M6.T3 shrinker 注册表）。
2. `corten_arena_fork_begin()` 对已有 state 的父 **急切复制子 registry**（fork_commit 预备）。
3. `corten_arena_mm_exit()`（exit_mmap 调用, mm/mmap.c:1407）对每个带 state 的 mm:
   `list_del_rcu` + **`synchronize_rcu()`**（mm/corten_arena.c:1502 起, "the only
   synchronize_rcu() on the exit path"）→ **每 mm 遥拆付一个完整 RCU 宽限期**。

**隔离实验（决定性, guest 实测）**: `/bin/true` ×20 对照——
plain 0.135s vs `LD_PRELOAD=hook`（仅 ENTER+退出）**1.135s** = 每 MODE 进程生命周期 **+50ms**
（当刻宿主 4 VM 并行, GP 偏慢; lat_proc 班更早窗口折算 ≈ +8.8ms/次, 同机制）。 lat_proc 三 op
差值逐项对表: fork op = 每迭代恰 1 次子退出（+8.8ms≈1 GP）; exec op = exec 换 mm 遥拆 + 子退出
（≈2 GP, +8.5ms）; shell op = sh→hello exec 链多次进程生命周期（+50ms@负载）。**三 op 差值全部
由"MODE mm 退出 GP"线性解释**。

**遥测对账**（run2 before→after, arena_stats）: `fork_faithful 34→34`（镜像零触碰）/
`fork_demotes=0` / `munmap_releases`、`pool_parks`、`mprotect_routes` 全部零前进（本基准无任何
arena 事务工作）/ `meta_arrays 338→337`（建=拆平衡, 零泄漏）/ dmesg corten warn=0。

### 定性: 这是 M6.T3 registry 退出设计与"急切建 registry"的组合机制成本, 暴露面 = **进程派生密集的
MODE 工作负载**（lat_proc 恰是该形状）。长寿命进程（T5 全部 apps/JTB/mmbench）一次性摊销不可见——
run4/g1-final 的 apps 无回退与此自洽。修复方向（登记, 未实施——本班禁改内核树）:
①惰性入册: registry join 从 ENTER 挪到首个 arena declare（零 arena MODE mm 永不入册, 退出免费）;
或 ②空 state 退出走 `kfree_rcu` 快速路径（免全 GP）。两改均为小 diff, 建议 M8 收口后作 G5-fix 切片。

## 3. JThreadBench MODE 回归（×1/臂, 同 boot 顺带）

| 臂 | median ms (runs) | rc | ClassFormatError |
|---|---|---|---|
| BASE | 3817.1 (3504.8/3817.1/3896.5) | 0 | 0 |
| MODE | 4086.6 (3600.5/4086.6/4141.0) | 0 | 0（`MODE on` marker 在） |

- **正确性回归 PASS: rc=0 双臂、CFE 0 命中**（gupfix 零回归第四次独立再证（run3/run4/g1-final/本班））。幅度 -7.1% 落在
  已登记 JVM spawn 噪声族（同 VM 30 分钟前 g1 班同口径测得 t0 反向 +6.5%; 本班宿主 4 VM 并行,
  绝对值只作回归watch 不作判定）。

## 4. 边界声明

- 本班未改内核树/未 push/密码未落盘（全程 ssh key）; VM 只读使用 + 跑 lat_proc/JTB。
- run1 判废数据、run2 干净数据、隔离实验原始输出全部在档可复核（raw/、jtb/、*.before/after）。
- G5-fix 未实施前, **进程派生密集场景的 MODE 回退为已知开放项**（本报告 §2 登记, REPORT.md 遗留
  分级同步）。

—— G5 收工: **NOT MET（+516%/+172%/+354%）, 根因=MODE 生命周期退出 RCU GP（急切 registry + 每-mm
synchronize_rcu）, 与忠实 fork 镜像无关（fork_faithful 全程不变）; 隔离实验 +50ms/lifecycle 定价;
JThreadBench MODE 回归 rc=0 零 CFE。修复方向已登记（惰性入册 / kfree_rcu 快速退出）。**
