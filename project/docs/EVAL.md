# EVAL · 评测与稳定性协议（v3, 2026-09-13 对齐 docs/PAPER_SPEC.md §F）
> 一切数字的采集口径以本文件为准; M1 基线与 M8 终测必须同协议同件同参。
> 每一节标注对应的 PS-F 条款; 工具路径与 VM 操作见 bin/env.sh。

## 1. 实验环境纪律（PS-F1/F2）
- 性能数字只出自 **KVM** guest(8 vCPU/4G); TCG 只用于功能验证。
- **两臂同加 `mitigations=off`**(PS-F2: 论文为公平关闭 Linux 全部缓解; 经由
  launch_vm.sh EXTRA_APPEND 注入, on/off/legacy 三组一致)。论文本身也在 VM 里测
  (PS-F1), QEMU 方法学站得住。
- guest 负载绑核 taskset 0-7; host 同时只跑一个 qemu; 测量时段构建任务暂停。
- 时间戳/内核版本/bzImage sha256 随结果落盘。

## 2. 对照实验设计（核心方法论）
1. **同镜像开关对照**: 性能对比用**同一个 bzImage**, boot 参数 `corten=on/off`
   切换(消除构建非确定性)。arena 路径只比较 on 且 workload 走 arena 的组。
2. **arena/非 arena 同进程对照**: 微基准 harness 提供 `--arena`(prctl 声明后
   全部槽位落在 arena)与 `--legacy`(等价槽位在普通地址空间)两种模式, 同 boot
   交错执行 A/B/A/B(继承 bandit 项目教训: 同 boot 交错 ctl 消除 boot 间漂移
   ±2%)。
3. 重复: 每配置 ≥3 次, 报中位数; 附 min/max。CV(标准差/均值)>5% → 加测至 5 次
   并标注; 仍大 → 报告写明噪声源。
4. 线程扫描: 1,2,4,8 (+16 仅高竞争, 超订观察崩溃点用, 标注 overcommit)。

## 3. 测量矩阵（= PS-F3..F9 复刻, guest 8 vCPU 规模化）
| 组 | 工具 | 参数 | PS 条款 |
|---|---|---|---|
| 微基准 | bench/mmbench (D6 口径已声明与 Table 3 的偏差) | 5 bench × low/high × 1,2,4,8(+16 超订标注) × ≥3 | F3 |
| 枚举型 | lmbench lat_proc | fork / exec / shell | F6 |
| JVM 线程 | JThreadBench: **N 线程各绑一核, 测 spawn→线程初始化完成窗口**(非总吞吐), N=1..8 | MODE-process 下零改动运行 | F7 |
| 真实应用 | metis_eq / dedup_eq / psearchy_eq, 线程 1/4/8, MODE-process 零改动 | **× 分配器维度 {glibc ptmalloc, LD_PRELOAD libtcmalloc}**(dedup_eq/psearchy_eq 必跑双档; tcmalloc 档需记录 RSS ~2× 预期, PS-F8) | F7/F8 |
| 内存开销 | smaps+debugfs/corten: PT 字节+meta 字节 vs 理论满配上界 vs 基线 | metis_eq 规模化 | F9 |
| 非 MM 回归 | guest 内 kernel 小编译 + stream | 固定 60s 窗, 回退≤5% | F7(其余 PARSEC≈平) |
**8 vCPU 不可测清单(报告必须声明, PS-F5 边界)**: ①64 线程高竞争平台期
②2270×/1489× 量级差距 ③384 核线性段。可测: 1-8 线程低/高竞争斜率与拐点前移、
单线程五项方向性(F4: mmap 允许 -3%~-19%, 其余应正)、竞争消退 trace 证据。
每项 on/off × arena(process-mode)/legacy × 线程全组合; 基线(M1)已完成组不得改参。

## 4. 稳定性协议（M7, 伴随式）
1. **kselftests/mm**: guest 内编译 tools/testing/selftests/mm, 全量跑; 与基线
   (corten=off)的 fail 集合做差; 新 fail 清零或逐条豁免(豁免理由写夜报)。
2. **syzkaller**: 
   - syz 构建: 已备 Go 1.23.4 + clone; `make TARGETOS=linux TARGETARCH=amd64`。
   - 内核 syzconfig: 基线配置 + KCOV=y KASAN_GENERIC=y(若 4G guest 内存吃紧降
     KASAN_LIGHT) DEBUG_INFO=y PROVE_LOCKING=n(syz 构建不开 lockdep, lockdep
     单独构建)。
   - manager cfg: qemu 后端, image=trixie.img 快照拷贝, 1 VM, ssh key 复用,
     workdir=$PROJ/syzwork; `enable_syscalls` 白名单: mmap,munmap,mprotect,
     madvise,mlock,munlock,mremap,brk,clone,clone3,fork,execve,prctl,ptrace,
     userfaultfd,process_madvise,mbind,move_pages,mincore,msync,munlockall。
   - **sy zlang 扩展**: 增加 `resource PR_CORTEN_ARENA` 常量描述, 让 fuzzer 能
     真正开 arena(M7.T1 交付物之一, 没有它 syzkaller 打不进新路径)。
   - 挂机: 每夜 ≥4h; crash 处理: 标题去重 → syz-repro → C reproducer →
     PLAN 加修复任务 → 修后回归该 repro。
3. **lockdep**: PROVE_LOCKING 构建, arena-stress + 微基准高竞争跑一遍,
   splat 清零。
4. **KCSAN**: KCSAN 构建, arena-stress 30 分钟, 报告清零或白名单(KCSAN 对
   无锁 fast 路径可能误报 ptl 语义, 逐条注释)。
5. 长跑: M8 前一整夜连续混合负载(微基准循环+apps+stress), 晨间检查
   oops/panic/内存泄漏(/proc/meminfo 趋势 + debugfs corten 计数)。

## 5. perfetto 协议（口径固化, 性能 agent 必须照抄）
- 采集: guest 内 tracebox, 事件集 = mmap_lock 三事件 +
  sched/sched_switch + (fault 分析轮) exceptions/page_fault_user/x86;
  每场景 10s, 与 workload 同起。cfg 模板 = results/r00/smoke_guest.cfg。
- 指标 SQL(存 $PROJ/eval/sql/, 禁改; 改 = 新版本号):
  - `mmap_lock_contention.sql`: start_locking→acquire_returned 间隔分布
    (按线程/操作聚合) —— on/off 对照的主证据(arena 操作应接近 0 事件)。
  - `fault_latency.sql`: page_fault 事件→返回的延迟直方图。
  - `sched_breakdown.sql`: kernel/user 时间分解(论文 Fig16/17 风格)。
- 查询入口: `$TP -q <sql> <trace>`; 原始事件在 `ftrace_event` 表(不是 slice)。
- trace 归档 results/rNN/trace/; 大于 50MB 的留 guest 侧不 publish。

## 6. 验收门（M8 DoD, 全过或机制级解释+用户可见的偏离声明）
| 门 | 阈值 |
|---|---|
| G1 热路径提升 | 低竞争 t∈{4,8}: {mmap-PF, PF, unmap, unmap-virt} 中 ≥2 项中位数提升 ≥10% (on-process vs off 同 boot) |
| G2 竞争消退 | mmap_lock_contention SQL: on 组 mmap_lock 事件 ≈ 0(<5% of off), 高竞争 unmap 不塌陷 |
| G3 真实应用(PS-F7/F8) | MODE-process 零改动下: {metis_eq, dedup_eq(ptmalloc 档), JVM 线程} 中 ≥1 项 t=8 提升 ≥10%; 或 trace 级机制解释(如 glibc brk 主导→记录为 DEV-6 局限) |
| G4 无回归 | 非 MM 组回退 ≤5% |
| G5 枚举代价 | lat_proc fork 回退 ≤30%(论文 17.7% 同向), fork+exec 允许净改善 |
| G6 稳定性 | kselftests 新 fail=0; syzkaller 2 夜无可复现 crash; lockdep/KCSAN 清 |
| G7 开销 | PT+metadata 总开销 ≤ 论文理论上界的同比例(8 vCPU 规模化说明) |
| G8 诚实性 | 全部数字 3 中位+原始输出路径; 8 vCPU vs 论文 384 核的方向性声明(D4)+不可测清单 |

## 7. 报告格式
- 每夜: log/YYYYMMDD-rNN.md(既有格式)。
- 终报 publish/REPORT.md 骨架: ①方法学(本文件 §1-§2 摘要+与论文差异) ②微基准
  曲线(论文 Fig13/14 同构) ③枚举型(Fig20 同构) ④应用与 JVM(Fig15/16/17 同构)
  ⑤内存开销(Fig22 同构) ⑥稳定性汇总 ⑦已知差异与妥协清单(§6.1 wired、mprotect
  maps 粒度、8vCPU 规模、等价 workload) ⑧结论。
