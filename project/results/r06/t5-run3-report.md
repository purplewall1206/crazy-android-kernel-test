# M4.T5 正式判定（run3）—— T1c 主树内核全矩阵（2026-09-19）

- 班次: 16:4x–17:4x CST（green 门 → QUICK 冒烟 372s → 全矩阵 1167s, 09:01:08–09:20:35 UTC）;
  deadline 5400s 未触发, 22:00 CST 前收工。
- 内核: **bzimg/r06-t5c/bzImage-6.18.32-g87383f51a3ff-83**, sha256 `f04564eb…7538c62`
  （主树 HEAD=87383f51a3ff tag corten-r07-t1c; `make olddefconfig`=No change to .config,
  与 r07-t1c #82 bb692cea 同源同 config, delta=构建计数; 2 条 warning=登记基线
  objtool cpuidle + modpost memblock）。**rogue=e219920 旧镜像未使用** —— 本跑即任务
  要求的"主树重编 T1c"（含 M5.T1a 忠实 fork）。
- boot: `corten=on mitigations=off kunit.enable=0 fsck.mode=force fsck.repair=yes`
  （+log_buf_len=64M）, trixie-m4t12.img 8 vCPU/4G/KVM, 宿主 i9-13900HX;
  VM = tmux **vm-t5run3**（port 10026, pidfile qemu-t5run3.pid）留运行。
- 动态 mmbench 口径（D1 教训落实）: **MMBENCH_BIN=/root/m4t12/bin/mmbench_dyn**
  sha256 `38304f062d43…` = m4t12/t1c 初步读数同件（动态链接, hook 实证进入 MODE;
  本轮 arena_stats pool_parks **+13,728,110** / pool_hits +13,727,280 [99.994%] 实证）。
- 产物: results/r06/t5-run3/（raw 330 条单行 JSON + trace 55 对 + meta + driver.log +
  t5_summary.json + t5_report.md + boot-console.log + quick/ 冒烟全套）; guest 侧
  双落盘 tar 校验后经 9p 回传（/mnt/t5run3 = guest 本地盘, 数据面隔离沿 g1-consol 教训）。
- 宿主声明: 测量窗（17:01–17:21 CST）内 3 个邻道 VM（basecheck/vm/m5t1a-vm）+
  syzkaller VM（2975482, 289% CPU）全部 SIGSTOP, 收班已全部 SIGCONT 恢复。

## 0. runner 健康

- QUICK=1 冒烟 PASS: 35 配置 0 失败腿, dmesg corten warn=0, hook 全部 T0 腿生效
  （MODE 计数池 parks/hits 即时爆炸性增长）; 冒烟 10 对 strace differs 全为
  EAGAIN/ETIMEDOUT/ESRCH 噪声族（预演全矩阵同结论, §4）。
- hook 生效（全矩阵 before→after）: mprotect_routes +319,508 / munmap_releases
  +13,727,795 / fork_faithful +88 / rearm_recovered +1,462 / auto_mmaps +2,792 /
  auto_attach_fail 8 + auto_fallbacks 8（容错路径, 零失败升级）; **drain_timeout 0→0**;
  pool_over 187 / misses 2,800 / **ejects 0**。
- 全程 dmesg `corten.*(warn|bug|oops)` = **0**; Oops/BUG/lockdep = 0。

## 1. M4.T5 正式判定表

| Gate | 判定 | 依据（run3 = 本跑动态口径） |
|---|---|---|
| **G1**（低竞争 t4/t8 ≥2 项 ≥10%） | **NOT MET（1/4）** | T0 vs BASE 同 boot: 仅 **unmap 成对过线（t4 +23.22 / t8 +113.30, CV t0 0.6~1.2）**; mmap-pf -61.6/-74.2, pf -63.7/+29.3, unmap-virt -83.8/-74.5。**读法 A**（MODE@T1c vs MODE@de8a6853, 上版 MODE 配对）**3/4 ≥+10%**（unmap low t8 +79.9% / mmap-pf low t8 +40.9% / unmap-virt low t8 +11.4%）与 T1c 初步读数同形——两口径如实并列见 §2 |
| **G3**（app ≥1 项 ≥10% 或机制解释） | **MET（2 项过线）** | dedup_eq t8/glibc **+11.30%**（CV 0.79/1.51, 三 run 无重叠）+ dedup_eq t8/tcmalloc **+16.14%**（CV 0.64/0.57, 三 run 无重叠）; 机制侧: JVM ClassFormatError 未复现 + metis/psearchy 全部跑完（忠实 fork 在树）见 §4 |
| **G4**（非 MM 回退 ≤5%） | **MET（全项干净）** | metis_eq +1.75 / psearchy_eq +2.99 / jvm -2.51（均 \|Δ\|≤5%）; dedup glibc +11.30 与 tcmalloc +16.14 为正向。run2 的 psearchy -40% 方差未决项本轮消除（§3） |
| 参数一致性 | **PASS 330/330** | PARAM-MISMATCH=0, 覆盖缺口 0, 失败/不可解析记录 0（analyze_t5.py exit=1 仅因 strace differs 计数, §4） |
| strace 机制语义 | **干净** | 55 对中 31 differs: 29 对纯 EAGAIN 计数抖动 + dedup_tcmalloc ETIMEDOUT 同族计数（30→37/10→7）+ jvm ESRCH×1（线程收尾竞争, run2 登记同形）; **零 EACCES/EFAULT/ENOMEM/EPERM/EINVAL 新类** |
| dmesg | 干净 | corten warn/bug/oops = 0 全程（meta/corten_dmesg_count.txt） |

## 2. G1 详表 —— run2/run3 两口径如实并列 + 读法 A

**口径声明（D1）**: run2 的 mmbench 是静态链接, LD_PRELOAD hook 从未进入其 mm,
其 mmbench "T0" 臂 = legacy-vs-legacy 慢漂移, **mmbench 侧口径作废**（apps 臂不受影响）。
run3 全部 mmbench T0 腿 hook 实证进入 MODE（池计数 +13.7M parks）。

### 2.1 run3 动态口径 T0 vs BASE（G1 判据口径, EVAL sec 6）

| bench | t4 Δ% | t8 Δ% | 同时≥10%? | run2 对照（口径作废, 仅存档） |
|---|---|---|---|---|
| mmap-pf | -61.64 | -74.24 | 否 | t4 +13.89† / t8 +0.53（legacy-vs-legacy 假象） |
| pf | -63.71（CV 54/79††） | +29.27（CV 15.3/2.4） | 否（t4 大负） | +1.06 / -0.97 |
| **unmap** | **+23.22** | **+113.30** | **YES** | -6.01 / -13.68 |
| unmap-virt | -83.83 | -74.46 | 否 | +48.27† / +11.41（legacy-vs-legacy 假象） |

† run2 高 CV 采样假象（g1-consol 加测已裁定 +7.0/+6.6 小效应）; †† pf/low 登记高方差族。

**G1 = NOT MET（1/4）**。qualified 项 unmap 的成对过线为本口径首次（run2 该组 -6.0/-13.7）,
且 t8 侧 +113.30% 为全矩阵最大 MODE 增益、两臂 CV 干净（base 15.6 / t0 0.6）——
T1c 池把"unmap 预映射区"定价从 run2 的回退拉成大正, 是 M8 最值得 5 次加测固化的组。

### 2.2 读法 A 对照（MODE@T1c/run3 vs MODE@de8a6853 [m4t12-verify §4.1]; 同件 mmbench_dyn）

| 格 | de8a6853 MODE | T1c 初步 MODE | **run3 MODE** | run3 vs de8a6853 | 与 T1c 初步一致? |
|---|---|---|---|---|---|
| unmap-virt low t4 | 0.00862 | 0.0085 | 0.008664 | +0.5% | ≈0% 两侧一致 |
| unmap-virt low t8 | 0.00532 | 0.0061（+15.2%） | 0.005924 | **+11.4%** | 同侧过线 ✓ |
| unmap low t8 | 0.00175 | 0.00247（+41.5%） | 0.003148 | **+79.9%** | 同侧过线且更强 ✓ |
| mmap-pf low t8 | 0.00026 | 0.00032（+23.5%） | 0.000366 | **+40.9%** | 同侧过线 ✓ |

**读法 A: 3/4 ≥+10%（unmap t8 / mmap-pf t8 / unmap-virt t8）= 与 T1c 初步读数（t1c-verify
§5.4 读法 A 3/4）同形且幅度更强**; run3 MODE 四格对 T1c 初步 MODE 自身: unmap t8 +27.5% /
mmap-pf t8 +14.5% 更优, unmap-virt 两格 ±3% 平。机制归因沿 t1c-verify §5.4: mmap-pf 残差
= arena fault 通道 ~2× legacy（M8 fault 域杠杆）; unmap-virt CHUNK 形状差与池正交。
**两读法并存说明**: 池确实消除了生命周期税（mmap 全组 +132%~+296%、probe mpl 4.1×、
unmap 组转正）, 但 G1 判据（同 boot MODE vs BASE）下 16KB 粒度四格中 CHUNK 之外两格仍负。

## 3. G3/G4 详表（apps 全部 3 rep 有效, 对 run2/T1c 初步对照）

| workload | base med | t0 med | Δ% | CV b/t0 | run2 对照 | T1c 初步对照 |
|---|---|---|---|---|---|---|
| dedup_eq t8/glibc | 4.431 M blk/s | 4.932 M | **+11.30** | 0.79/1.51 | +1.41 | n/a |
| dedup_eq t8/tcmalloc | 5.022 M | 5.833 M | **+16.14** | 0.64/0.57 | +11.32 | +9.4%（独立 3×: 6.13M） |
| metis_eq t8 | 16.722 s | 16.429 s | +1.75 | 15.73/0.64 | -1.72 | n/a |
| psearchy_eq t8 | 4.308 s | 4.179 s | +2.99 | 15.89/1.14 | -40.05†（暖机） | n/a |
| jvm t2000x3 | 2434.1 ms | 2495.2 ms | -2.51 | 4.03/7.92 | soft-fail（CFE） | MODE rc=0（JTB 复跑） |

- dedup 双档三 run 无重叠（glibc base [4.457,4.431,4.373] vs t0 [4.932,4.789,4.956];
  tcmalloc base [5.00,5.02,5.07] vs t0 [5.90,5.82,5.83]）—— 本轮 G3 过线项比 run2
  （仅 tcmalloc +11.32）**多一项且 tcmalloc 幅度更强**; T1c 初步的独立 dedup 6.13M 与
  本轮 in-matrix 5.83M 为不同窗形态, 不矛盾（D15 后不得回退线 5.94M: 本轮 in-matrix
  t0 5.83M 略低于该独立窗读数, 如实登记）。
- metis/psearchy base 侧 rep1 暖机（22.7s/5.9s）再入登记族（run2 psearchy 同形;
  g1-consol 已证暖机主导）, 本轮两件 Δ 为正, 不构成回退证据; M8 沿 5 次加测口径。
- M1 跨 boot 对照（参考非 gate）: apps 五件 BASE 偏差 -7.7%~+1.9%（run2 同形 ≤6.5%,
  app 基线可复现）; mmbench 10 格 >25% 漂移 = pf/low 登记高方差族 + mmap-pf/unmap
  t8/t16 负向偏（结论一律以 in-boot BASE vs T0 为准）。

## 4. 已知失败口径核对（任务 §5 三项）

| 项 | run2 登记态 | **run3（T1c 主树内核）实况** | 处置 |
|---|---|---|---|
| JThreadBench ClassFormatError | T0 3/3 收尾 CDE soft-fail（"下一片——注意 T1c 后是否仍在"） | **未复现**: T0 3/3 rc=0（2584/2142/2495 ms）, stderr `ClassFormatError` **0/3**; base 3/3 rc=0 | 登记更新: T1c boot 上消失（与 gupfix 后 MODE JTB rc=0 谱系一致）; M8 维持观测项 |
| metis_eq fork 后段（OQ-D 残余） | rogue 时代 rc=139 族（M5.T1a 前后对照项） | **未复现**: metis_eq T0 3/3 rc=0 跑完, fork_faithful +88（MODE 忠实 fork 真实在走） | 闭合; OQ-D 忠实 fork 侧无新残余 |
| rogue 内核 = e219920 早于 M5.T1a | run2 用 rogue 镜像 | **已按任务改用主树重编 T1c #83**（忠实 fork 在树）, 镜像问题不再存在 | 归档 bzimg/r06-t5c + SHA256SUMS |

## 5. 异常与处置（班内如实记录; 一次重试额度未消耗, 无 panic）

1. **timegate 计费窗**: bin/timegate.sh 在 23:00 前为阻塞语义, 按任务"09-20 D14 精神
   延续可全天实验"的明确授权继续（未睡眠等待）; runner 自带 5400s deadline 未触发。
2. **9p cache=none 不可用**: 本 qemu 的 -fsdev/-virtfs 均拒绝 `cache` 参数（两次 FATAL,
   qemu 侧拒绝, 非 guest 问题）→ 回退 stock 9p（本 lineage r06-t5/t5quick/t1c 历次成功
   boot 同形）; 数据面隔离按 g1-consol 教训改 guest 本地盘 + 收班双落盘 tar（sha256
   6e539c98… 回传核对）。
3. **QUICK 启动 ssh 挂起**: nohup 后台件未接 </dev/null 使 ssh 等 stdin —— 断开本地
   ssh 即可（nohup 保护远端进程, 冒烟未中断）; 全矩阵启动即带 </dev/null。操作项, 非
   runner 缺陷。
4. **邻道管理**: 测量窗 SIGSTOP basecheck(1461852)/vm(1594112)/m5t1a-vm(2042570/73)
   + syzkaller VM(2975482, 16:51 起家 289% CPU); 收班 5 pid 全部 SIGCONT。run2 的
   邻道 kill 事故未复现（session 改名 vm-t5run3 + 独占 pidfile/monitor sock）。
5. 首跑遗留核对: present-RO 族（run1 9/9 rc=139）**0 命中**; JVM >480s 楔死 **0 命中**;
   密码未落盘; 未 push; 未动内核 worktree（主树仅构建产物）。

## 6. M8 前行动项（沿袭 + 新增）

1. **unmap low {t4,t8} 5 次加测固化**（run3 新过线项, t8 +113.30 为最大增益; unmap-virt
   的 g1-consol +7% 小效应结论维持）;
2. mmap-pf/unmap-virt t≥2 的 fault 通道残差与 CHUNK 形状差 = t1c-verify §8 原杠杆不变;
3. dedup tcmalloc 独立窗 vs in-matrix 窗的 6.13M/5.83M 差异注明口径（D15 线 5.94M 按
   独立窗口径维持）;
4. pf/low 家族维持 5 次口径（本轮 CV 47~79% 延续登记）;
5. JVM CFE 与 metis fork 残余双双未复现, M8 口径从"soft-fail/登记"降级为"观测项"。

—— M4.T5 run3 收工: **G1 NOT MET（1/4 动态口径; 读法 A 3/4 与 T1c 初步同形）、G3 MET
（dedup 双档 +11.30/+16.14）、G4 MET（全项 ≤5% 或正向）; JTB CFE 与 metis fork 残余
未复现; 正确性面 330/330 同参 + strace 语义干净 + dmesg 0**。全部原始数据
results/r06/t5-run3/ 可复核; VM 留运行（vm-t5run3, port 10026, 内核 = r06-t5c #83）。
