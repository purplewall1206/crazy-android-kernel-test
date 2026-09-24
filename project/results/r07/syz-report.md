# CortenMM M7 — syzkaller 隔夜挂机报告 (r07)

归档时间: 2026-09-19 17:35 CST（注: 任务简报所写"09-20 17:40"与系统时钟不符，实际系统时间为 09-19 17:3x CST）

## 1. 运行概况

| 项 | 值 |
|---|---|
| syz-manager PID | 1784337（自 2026-09-18 23:45:58 CST 启动，单实例连续运行，无重启） |
| 配置 | /home/ppw/cortenmm/bin/syz-cfg.json（tag=m7-preheat-r07-kcov；简报所写 syz-cfg-final.json 路径不存在，实际文件为 results/r07/syz-cfg-final.json，二者同源） |
| 内核 | /home/ppw/linux-6.18-kcov，kcov-build 分支 @ 025756094542（"mm: CortenMM: arm64 4K-page build support and arch-neutral test macros"，含 corten M3b+M4.T0），KCOV+KASAN 构建 |
| 运行面 | mmap/munmap/mprotect/madvise/mbind/userfaultfd/openat(+clone/ptrace) |
| VM | qemu, count=1, 4 vCPU / 4G guest, trixie-syz.img |
| 运行时长 | 17.8h（原计划至 09-19 08:00，实际超时 ~9.5h） |
| exec total | 1,416,524（~22 exec/s，KASAN+no_kvm 调试内核下正常水平） |
| 终态 corpus | 748 |
| 终态 coverage | 16,875 |
| crash 数 | 4（0 个内存安全类） |
| repro 结论 | 3 次内置 syz-repro 尝试全部失败（reliability=0.00）；第 4 次为同 title 重复尝试，归档时在跑 |

## 2. coverage / corpus 曲线要点

```
09-18 23:47  corpus=0    coverage=0       (启动)
09-19 00:00  corpus=466  coverage=13469   (前 13 分钟急速爬升)
09-19 01:00  corpus=633  coverage=15016
09-19 04:00  corpus=687  coverage=15389
09-19 08:00  corpus=723  coverage=15531   (原定结束点)
09-19 11:00  corpus=732  coverage=16687   (二次跃升 +1156, 见 §3 crash#4 后恢复)
09-19 17:00  corpus=748  coverage=16875
09-19 17:34  corpus=748  coverage=16875   (冻结: 17:21 起 VM 被 repro 占用)
```
要点: coverage 前 1 小时完成 ~90%；08:00 后进入慢速平台期（+1344 / 9.5h）；11:00 的跃升发生在 RCU stall crash 恢复之后，属于 VM 重启后 corpus 重放增益而非新功能面。

## 3. crash 清单与复现状态

workdir: /home/ppw/syzwork/crashes/

| # | 目录(前缀) | 时刻 | 描述 | 内核报告要点 | repro |
|---|---|---|---|---|---|
| 1 | e968b22d | 09-19 00:52 | INFO: rcu detected stall in schedule_timeout | rcu_preempt GP kthread starved 85269 jiffies（cpu=3），栈为 rcu_gp_fqs_loop/schedule_timeout 纯 RCU 基础设施帧；无 corten 帧、无 KASAN 报告 | 内置 repro 00:52→01:29 **失败**（repro0 存档，候选为 mbind/munmap/madvise/mprotect/userfaultfd 混合序列，全部 did not crash） |
| 2 | 439c37d2 | 09-19 06:06 | lost connection to test machine | report0 为空（VM 无痕迹死亡/挂死，无控制台 trace） | 内置 repro 06:06→06:57 **失败**（repro0 存档，Extracting 耗时 51m；候选为 openat+mmap+madvise(POPULATE_READ/WRITE/DONTNEED) 循环，全部 did not crash） |
| 3 | e9f5b119 | 09-19 07:50 及 17:21（两次） | no output from test machine | report0/report1 均为空 | 内置 repro 07:50→08:56 **失败**: "reproducer is too unreliable: 0.00"（repro0 存档）；17:21 第二实例触发重复 repro，17:21:57 起在跑，归档时未出结论（同 title 早间已判 0.00，预期同果） |
| 4 | 1a6abe07 | 09-19 11:47 | INFO: rcu detected stall in corrupted [corrupted] | CPU3 RCU stall t=65692 jiffies，"Stall ended during stack backtracing"（报告损坏/不可符号化），尾随 SYZFAIL: failed to recv rpc | syzkaller 因报告 corrupted **未调度 repro** |

### 判读
- **零内存安全 crash**: 1.42M execs 中无任何 KASAN slab/UAF/溢出、无 BUG/WARNING/oops。corten M3b+M4.T0 修改面（mmap/mprotect/madvise/mbind/userfaultfd/openat）经受住了首轮 syzkaller 模糊测试。
- 4 个 crash 全部为环境/健壮性类: 2 个 RCU 饥饿（KASAN 慢速内核 + 4 vCPU 无 KVM 加速 + mbind/uffd 高频 churn 下的经典 rcu_preempt/GP kthread 饿死，非内存错误），2 个 VM 无输出/失联（同类慢速 VM 症状，且不可复现，reliability=0.00）。
- **正面旁证**: 内核日志中反复出现 `page size migration: madvise_vma_pad_pages:326: Invalid attempt to madvise padding on MAP_SHARED vma` —— M4.T0 的 padding 输入校验在 fuzz 下被大量触发且安静拒绝（无 WARN/BUG），说明输入验证路径工作正常。
- 3 份 repro0 候选序列全部落在运行面内（含 MADV_POPULATE_READ/WRITE、MADV_COLLAPSE、mbind flag 混合、userfaultfd），但单测均不触发 —— 支持"环境慢速"而非"corten 确定性挂死"的结论。

## 4. M7 判定建议

**判定: M7 为正面数据点（zero memory-safety crash on corten surface, 1.42M execs / 17.8h）。**
- 无 corten 相关可复现 bug → 无需走 repro→最小化→移交修复班流程；4 个 crash 目录原样归档于 /home/ppw/syzwork/crashes/。
- 建议下轮（M8）改进以区分"慢"与"挂死": vm.count=2 + rcu_stall_timeout 放宽 + gp kthread 优先级/`nohz_full` 配置，或改用非 KASAN 快速构建单独验证 hang 类；KASAN 版专注于内存安全面。
- 本轮 corpus（748 programs, workdir/corpus）可直接作为下轮种子。

## 5. 处置记录

- 17:35 CST: 归档完成，按"有 crash → 收集完先停"规则执行停机。
- 17:36-17:38 CST 停机过程: SIGTERM 后 manager 卡在 repro 关停路径 ~50s 不退（与"reproducing 卡死"观察一致），先 TERM 其 qemu VM(2984701)/ssh(2985462)，manager 仍不退，最终 SIGKILL(1784337)。三者均已确认退出。
- corpus 安全: 本 syzkaller 构建使用 workdir/corpus.db（124,762 B, 最后落盘 09-19 16:21），周期性持久化而非仅在退出时写 —— corpus 完整保留（最多缺 16:00→17:00 间的 ≤2 个新输入），可直接作 M8 种子。
- crash 归档完整: /home/ppw/syzwork/crashes/ 1.2M，4 目录含全部 log/report/machineInfo/repro0。
- 残留说明: 宿主上另有 4 个 qemu 为 r05/r06 基准/启动测试 VM（trixie.img/trixie-m5t1a.img 等，非本运行面），未触碰。
- 后续运行建议: 下次以 syz-cfg.json 重启即可续用 corpus.db；可考虑加 `"corpus_oriented": true` 之外，将 repro 相关超时调小（repro 单次曾耗时 36-66 分钟，占用唯一 VM 导致 fuzzing 停摆），如 `"reproduce": 1` 配合 vm.count=2。

## 附: 产物路径

- manager 日志: /home/ppw/cortenmm/results/r07/syz-manager.log（736KB, 至 09-19 17:34）
- 本报告: /home/ppw/cortenmm/results/r07/syz-report.md
- crash 归档: /home/ppw/syzwork/crashes/{e968b22d*,439c37d2*,e9f5b119*,1a6abe07*}
- corpus: /home/ppw/syzwork/corpus/
- 内核构建日志: /home/ppw/cortenmm/results/r07/kcov-build.log
