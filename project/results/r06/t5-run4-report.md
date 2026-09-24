# M4.T5 正式判定（run4）—— perf1 TLB 风暴修复后全矩阵 G1 判定（2026-09-19）

- 班次: 19:1x–19:5x CST（green 门 → QUICK 冒烟 343s → 全矩阵 1147s, 11:24:13Z 起, deadline 5400s 未触发）。
- 内核: **bzimg/r06-t5d/bzImage-6.18.32-g802ff7551bd0-89-t5d**, sha256 `6d3d616d…ee124e8`
  （主树 HEAD=**802ff7551bd0** tag corten-r06-perf1; `make olddefconfig`=**No change to .config**
  四配置核对: CORTEN_MM=y+ARENA=y+2 KUnit 测试项 / ZRAM=y+BACKEND_LZ4=y+DEF_COMP=lz4 / KUNIT=y /
  LOCALVERSION_AUTO=y; 增量重链 #89 零编译告警; 与 perf1 终验 #88 同源同 commit, delta=构建计数+版本串
  [#88 文件名内嵌 g87383f51a3ff 为打 tag 前描述, 本枚串内嵌 g802ff7551bd0=HEAD 短哈希, 更准确]）。
- boot: `corten=on mitigations=off kunit.enable=0 log_buf_len=64M fsck.mode=force fsck.repair=yes`,
  trixie-m4t12.img 8 vCPU/4G/KVM; VM = tmux **vm-t5run4**（port 10026, pidfile qemu-t5run4.pid）留运行。
- 动态 mmbench 口径（D1）: **MMBENCH_BIN=/root/m4t12/bin/mmbench_dyn** sha256 `38304f062d43…`
  = run3/perf1 同件; 本轮 arena_stats pool_parks **+10,890,032** / pool_hits +10,889,336 [99.994%] 实证。
- 产物: results/r06/t5-run4/（raw 330 条单行 JSON + trace 55 对 + meta + driver.log + t5_summary.json +
  t5_report.md + boot-console.log + quick/ 冒烟全套 35 配置）; guest 本地盘 /mnt/t5run4 双落盘 tar 回传
  （sha256 f04e6020… 校验一致）。
- 宿主声明: 测量窗内邻道 basecheck(1461852)/m5t1a-vm(2042573) SIGSTOP, 收班 SIGCONT 恢复;
  "vm"(1594112, port 10022) 不可停（tmux 持续 CONT, perf1 登记同形）, 实测 1.2% 单核 = 噪声级记录在案;
  syzkaller VM 本班不在运行。perf1 VM（vm-perf1）换核时关闭, 其 guest /root/perf1 剖景数据在镜像内保留。

## 0. runner 健康

- QUICK=1 冒烟 PASS: 343s, 35 配置 0 失败腿, dmesg corten warn=0; G1 四格冒烟读数与 perf1 初步同形
  （unmap-virt t4 +1193.7 / unmap t8 +136.1 / mmap-pf -11.5/-13.8）。
- hook 生效（全矩阵 before→after）: munmap_releases +10,889,672 / pool_parks +10,890,032 /
  mprotect_routes +221,423 / fork_faithful +88（与 run3 同值）/ auto_mmaps +2,177 /
  auto_attach_fail 8 + auto_fallbacks 8（容错路径, 与 run3 同值, 零失败升级）; **drain_timeout 0→0**;
  pool_over 185 / misses 2,803 / **ejects 0**; meta_arrays==ptdescs 340→313（负增长, 零泄漏）。
- 全程 dmesg `corten.*(warn|bug|oops)` = **0**; 串口 console Oops/BUG/lockdep/WARNING = **0**。

## 1. 正式判定表（M8 口径）

| Gate | 判定 | 依据（run4 = 本跑动态口径, T0 vs BASE 同 boot） |
|---|---|---|
| **G1**（低竞争 t4/t8 ≥2 项 ≥10%） | **MET（2/4）—— T5 谱系首次** | **unmap 成对过线（t4 +33.51 CV 10.6/2.9 / t8 +143.48 CV 2.2/2.2）+ unmap-virt 成对大胜（t4 +1156.11 CV 7.3/3.7 / t8 +2791.86 CV 3.5/3.3）**; mmap-pf -8.76/-17.09 = perf1 登记残差; pf +37.04/-9.65（登记高方差族, CV 26~59） |
| **G3**（app ≥1 项 ≥10% 或机制解释） | **NOT MET（数字面）** | dedup_eq t8/glibc **+4.60%**（CV 1.04/1.13, 三 run 无重叠）+ dedup_eq t8/tcmalloc **+6.33%**（CV 0.16/2.69, 三 run 无重叠）= 双档正向但均未到 10% 线; metis -0.88 / jvm -6.59（辅助项）。机制解释: run3 的 dedup 增益定价于 T1c 池生命周期税消除; perf1 三优化作用在 TLB gather/park 路径（mmbench 形状）, 对 dedup churn 增量有限——本轮维持正向=**无回退**, 幅度收窄如实登记 |
| **G4**（非 MM 回退 ≤5%） | **边缘未过（1 项超线 1.6pp）** | metis_eq -0.88 ✓ / psearchy_eq +3.18 ✓ / dedup 双档 +4.60/+6.33 正向 ✓; **jvm -6.59% 超 5% 线**: 三 run 方向一致（逐 run +1.7/+6.9/+1.6%）但分布重叠（base [2261,2057,2064] vs t0 [2300,2200,2097] ms, CV 4.45/3.76）, run3 同口径 -2.51, JVM spawn 噪声族登记为观测项 |
| 参数一致性 | **PASS 330/330** | PARAM-MISMATCH=0, 覆盖缺口 0, 失败/不可解析记录 0（analyze_t5.py exit=1 仅因 strace differs 计数, §4） |
| strace 机制语义 | **干净** | 55 对中 34 differs: + 侧仅 EAGAIN/ETIMEDOUT 计数抖动 + jvm ESRCH×1（run2/run3 登记同族, 线程收尾竞争）; **零 EACCES/EFAULT/ENOMEM/EPERM 新类**（EINVAL 17 条两侧同值 = JVM 标准族） |
| dmesg | 干净 | corten warn/bug/oops = 0 全程（meta/corten_dmesg_count.txt） |

## 2. G1 详表 —— run4 vs run3 对照（同为动态口径 in-boot BASE vs T0）

| bench | run3 t4 / t8 Δ% | **run4 t4 / t8 Δ%** | 同时≥10%? | 变化 |
|---|---|---|---|---|
| mmap-pf | -61.64 / -74.24 | **-8.76 / -17.09** | 否 | 大负 → 一位数~两位数残差（perf1 初步 -14.3/-15.5 同形收敛） |
| pf | -63.71 / +29.27 | **+37.04 / -9.65** | 否 | 高方差族（CV 57/27, 59/58）= 噪声地板, run3 同登记 |
| **unmap** | **+23.22 / +113.30** | **+33.51 / +143.48** | **YES** | 过线项增强 |
| unmap-virt | -83.83 / -74.46 | **+1156.11 / +2791.86** | **YES** | **翻盘**: mmu_gather 嵌套全 mm TLB 风暴消除后从大负 → 全矩阵最大增益（perf1 初步 +1186/+1258 同族, 全矩阵低竞争成对过线） |

**G1 = MET（2/4, M8 口径）**。tlb 风暴修复（lazy per-window gather + park 元数据整块释放 + park flush
后置 downgrade）把 run3 的"1/4 + 三大负"变成"2/4 成对过线 + 两项登记残差":
- unmap-virt 全 16 格全部大正（high 侧 t16 高达 +7034%）, 两臂 CV ≤12.7, 风暴修复直接定价;
- unmap 族除 high/t2（-20.08, CV 6.5/15.8 唯一负格, 如实登记）外全正, t8/t16 大正（+143/+167 low）;
- mmap 全组 +119~+278%（T1c 池生命周期税消除维持, run3 时代 +132~+296% 族同形）;
- mmap-pf 残差 = fault 通道机制成本（perf1 §4 归因: 纯 pf 通道 ≤11% + take/park 簿记 3-5%）,
  本轮四格 -1.1~-22.6 与 perf1 两轮网格读数一致, **进入噪声/机制成本地板, 非风暴性回退**。

## 3. G3/G4 详表（apps 全部 3 rep 有效 rc=0, 对 run3 对照）

| workload | base med | t0 med | Δ% | CV b/t0 | run3 对照 |
|---|---|---|---|---|---|
| dedup_eq t8/glibc | 4.419 M blk/s | 4.622 M | **+4.60** | 1.04/1.13 | +11.30 |
| dedup_eq t8/tcmalloc | 4.909 M | 5.219 M | **+6.33** | 0.16/2.69 | +16.14 |
| metis_eq t8 | 16.621 s | 16.767 s | -0.88 | 8.91/1.27 | +1.75 |
| psearchy_eq t8 | 4.434 s | 4.293 s | +3.18 | 14.57/3.5 | +2.99 |
| jvm t2000x3 | 2063.9 ms | 2199.8 ms | **-6.59** | 4.45/3.76 | -2.51 |

- dedup 双档三 run 无重叠（glibc base [4.328,4.419,4.431] vs t0 [4.622,4.650,4.529];
  tcmalloc base [4.909,4.915,4.896] vs t0 [5.219,5.195,5.509]）—— 正向但低于 run3; **无一项回退**。
- metis/psearchy base 侧 rep1 暖机（19.9s/5.96s）再入登记族（run3 同形）, 两件 Δ 与 run3 同向小幅波动。
- jvm: base 中位对 M1 基线 +12.1%（本 boot 偏快）, t0 三 run 一致慢 1.6~6.9%, 分布重叠;
  **0 ClassFormatError、0 楔死、3/3 rc=0** —— 正确性面无回退, 纯幅度边缘超线, 登记 M8 观测项。
- M1 跨 boot 对照（参考非 gate）: apps 五件 BASE 偏差 -10.6%~+12.1%; mmbench 11 格 >25% 漂移
  = pf/low 登记高方差族（+104~+408%）+ mmap-pf/unmap t8/t16 负向偏（-25~-44%）——
  结论一律以 in-boot BASE vs T0 为准（与 run3 同读法）。

## 4. 已知失败口径核对（任务三项）

| 项 | run3 登记态 | **run4（perf1 内核 802ff7551bd0）实况** | 处置 |
|---|---|---|---|
| JThreadBench ClassFormatError | run3 未复现（0/3）, 降级观测项 | **再证未复现**: T0 3/3 rc=0（2299.7/2199.8/2097.1 ms）, stderr CFE **0/6**（双臂） | 观测项维持, M8 沿用 |
| metis_eq fork 后段（OQ-D 残余） | run3 未复现, fork_faithful +88 | **维持闭合**: metis_eq T0 3/3 rc=0 跑完（16.767/17.067/16.545 s）, fork_faithful **+88（与 run3 同值, 忠实 fork 真实在走）**, 无 rc=139 族 | 闭合维持; M5.T1a 忠实 fork（6869744+, 本内核含）行为稳定, 无新残余, 亦无恶化 |
| 镜像/口径 rogue 遗留 | run3 已改主树重编 | 本轮=主树 HEAD 直编 #89 + r06-t5d 归档, 不存在 | 无 |

## 5. 异常与处置（班内如实记录; 一次重试额度未消耗, 无 panic）

1. **timegate 计费窗**: bin/timegate.sh 19:14 返回阻塞语义（rc=124）, 按 perf1 班"09-20 常设指令:
   无时间门禁"的延续授权继续（未睡眠等待）; runner 自带 5400s deadline 未触发（实际 1147s）。
2. **green 门 mmbench 首两发 rc=2**: 参数笔误（seed 传了字面表达式 / threads 传成 "t2"）, 第三发
   数值参数 rc=0 JSON 有效 —— 操作笔误, 非内核/镜像问题。
3. **nohup ssh 挂起 ×2**: QUICK 与全矩阵启动的本地 ssh 未随远端 nohup 退出（run3 §5.3 登记同形）,
   断开本地 ssh 即可（</dev/null + setsid 已带, 远端进程无恙）。
4. **邻道管理**: SIGSTOP basecheck/m5t1a-vm 于测量窗前, 收班 SIGCONT 双双恢复（Sl 态核实）;
   "vm"(10022) 不可停按 perf1 先例记录噪声级; 本班未发生 run2 型邻道 kill 事故。
5. 首跑遗留核对: present-RO 族 0 命中; JVM 楔死 0 命中; **密码未落盘; 未 push; 未动内核 worktree**
   （主树仅构建产物）。

## 6. M8 前行动项（沿袭 + 新增）

1. **G1 首次 MET（2/4）固化为正式读法**: unmap/unmap-virt 低竞争成对过线, 建议 5 次加测固化
   （沿 run3 行动项 1 的 unmap 项, 新增 unmap-virt 全线程族——本轮 t1~t16 全大正, 16 线程 +7189%）;
2. mmap-pf 残差 -8.8~-22.6（6/10 格 ≤11%）与 pf 高方差族维持"机制成本+噪声地板"登记口径
   （perf1 §4 归因不变）, M8 报告如实披露;
3. jvm -6.59% 边缘超线与 dedup 双档幅度收窄（+4.60/+6.33 vs run3 +11.30/+16.14）列入 M8 加测
   5 次口径核实（本轮 CV 双档均 ≤2.7, 无重叠, 非方差假象, 如实登记幅度差）;
4. JTB CFE 与 metis fork 残余维持"观测项"降级态（双双再证未复现）。

—— M4.T5 run4 收工: **G1 MET（2/4 动态口径, T5 谱系首次; unmap +33.5/+143.5 与 unmap-virt
+1156/+2792 成对过线）、G3 NOT MET 数字面（dedup 双档 +4.60/+6.33 正向未到线, 无回退）、
G4 边缘未过（jvm -6.59% 一项超线 1.6pp, 其余全净）; JTB CFE 与 metis fork 残余再证未复现;
正确性面 330/330 同参 + strace 语义干净（零新错误类）+ dmesg/console 双零**。全部原始数据
results/r06/t5-run4/ 可复核; VM 留运行（vm-t5run4, port 10026, 内核 = r06-t5d #89 = 802ff7551bd0）。
