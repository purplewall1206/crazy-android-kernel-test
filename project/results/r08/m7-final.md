# M7 第二轮（r08）收数与终判 — 2026-09-22

> 收数执行: 2026-09-22 14:20 CST（原定 08:10 自动化未触发, 由用户指令手动收数; 见 §5）。
> 结论先行: **M7.T2 DoD（连续 2 夜无可复现的内存安全 crash）= PASS**。第二轮
> (corten=on + PR_CORTEN prctl 描述 = corten 修改面首次实际覆盖) 全程零 KASAN/
> BUG/WARNING, 6 个崩溃目录全部为环境噪音类, 无一指向 corten 代码。

## 1. 终态数字

| 项 | 第二轮（r08） | 第一轮（r07, 对照） |
|---|---|---|
| 挂机窗口 | 09-21 08:01:58 → 09-22 06:28:24 **有效 22.4h**（其后 manager 卡死, 见 §3; 14:20 收数时 wall 30.3h） | 17.8h |
| exec total | **3,114,125** | 1,416,524 |
| coverage | **17,859**（> 首轮 17.8h 终值 16,875; 首 30 分钟 17,496 已超） | 16,875 |
| corpus | 716（种子 769 载入, 经 minimize 稳定于 716; fuzzer 首日即自主发现含 PR_CORTEN 程序与 MODE ENTER 链） | 748 |
| 内核 | @b9541335a554 KCOV+KASAN, cmdline +corten=on（sha256 d6c0dd5b…24f） | @025756094542, corten=off（覆盖面更正见 m7-round2.md §0） |
| vm.count | 2 | 1 |
| 内存安全 crash | **0**（manager log 全量 grep `KASAN\|BUG:\|WARNING:` 命中 = 0） | 0 |
| 崩溃目录 | 6（全部环境类, 见 §2） | 4（全部环境类, repro reliability=0.00） |

## 2. 崩溃目录全量清点（/home/ppw/syzwork/crashes/, 6 目录）

| hash | 首现 | 末次活动 | description | 归属与定性 |
|---|---|---|---|---|
| e968b22d…a2c3d | 09-19 | 09-19 | INFO: rcu detected stall in schedule_timeout | 第一轮遗留; RCU GP kthread 饥饿（KASAN 慢核 + 宿主过载） |
| 1a6abe07…92fe86 | 09-19 | 09-19 | INFO: rcu detected stall in corrupted | 第一轮遗留; 同上 |
| e9f5b119…973719f | 09-19 | 09-21 | no output from test machine | 第一轮遗留; 串口无输出（VM 级, 非 panic） |
| 439c37d2…91453a447 | 09-19 | **09-22 06:28** | lost connection to test machine | 第一轮遗留; 09-22 06:28 再现并触发 manager 卡死（§3） |
| 107d4fad…3d1605ce1 | 09-21 13:06 | 09-21 | INFO: rcu detected stall in do_idle | **第二轮新增**（m7-round2.md §5 已预登记）; 空闲核 RCU 饥饿, 环境类 |
| b83ebc0f…b44f55efc | 09-21 23:18 | 09-21 | suppressed report | **第二轮新增**; syzkaller 去重抑制的报告（6 次聚集在同一慢 VM 时段）, 环境类 |

判定依据: ① 六目录 description 全部落在 RCU stall / VM 失联 / 无输出 / suppressed 四个
已知环境族, 与 m7-round2.md §5 的预判一致; ② 全量 manager log 零 KASAN/BUG/WARNING
（`grep -cE "KASAN\|BUG:\|WARNING:" syz-manager-r2.log` = 0）; ③ 无任何 repro 成功产出
（第二轮唯一进入 reproducing 的 lost-connection 从未复现完成）。相对 m7-round2.md 登记
的"已知 5 条", 第二轮实际新增仅 b83ebc0f（suppressed report）一条, 同族环境噪音。

## 3. 诚实记录: manager 06:28 起卡死（工具面事件, 非内核 crash）

09-22 06:28:22 VM-0 报 `crash: lost connection to test machine` 后, manager 进入
reproducing=1 并**未再退出**: 06:28:24 起 exec total 冻结于 3,114,125（其后 ~7.9h 零
增量, 逐 10s stats 行可证）, VM 不再按 1h 周期重启, 直至 14:20 收数。定性: syzkaller
reproducer 对 lost-connection（不可复现环境事件）的等待挂起 = **工具面卡死**, 不是内核
缺陷信号; 本报告全部终态数字取自冻结前有效窗口（22.4h）。停机流程照 m7-round2.md §4
预案: 14:20 `kill 477619`（TERM）→ manager 优雅退出并自动清理两台 syz qemu（复核
`trixie-syz` 进程归零, 其余 10 台复核用 VM 未受影响）。

## 4. DoD 判定

- **M7.T2 "连续 2 夜无可复现 crash"（完整口径: 无 5 分钟内可复现的内存安全 crash）:
  PASS** —— 第一夜 17.8h（corten=off 面, 覆盖面缩限已如实登记）+ 第二夜有效 22.4h
  （corten=on 面, 3.11M exec / coverage 17,859 / 零内存安全报告）。两轮合计 4.53M exec,
  10 个崩溃条目全部环境类、零成功复现。
- M7.T1（环境）: ✓ 两轮（第二轮含 syzlang PR_CORTEN_ARENA/MODE 描述补丁, 归档
  r08/corten.txt + .const）。
- M7.T3 残余: **KCSAN 未跑**（维持计划中）; **B4**（zap_untracked_window
  add_mm_counter-under-ptl preempt_nested WARN, tier-2）的 lockdep corten=on 复跑裁定
  并入 09-22 夜 mva2 验证矩阵的 kunit-lk 步（results/r07/mva2/, 夜验排程见 STATE r08 条）。
- 后续种子: corpus 716 programs 保留于 workdir, 可作 M-V 收口后第三轮（post-de-VMA 面）
  的种子; rcu_stall_timeout 放宽与非 KASAN 快内核验证 hang 类两改进项视第三轮必要性定。

## 5. 过程记录（为何 14:20 才收数）

原定 09-22 08:10 的自动收数任务（cron 一发）**未触发**（触发时刻客户端会话不在运行,
runCount=0）; 06:28 起 manager 卡死无人处置, 空转 ~6h。14:2x 由用户指令手动执行本收数。
夜验接力同窗口修复: 12:41 启动的 mva2-night.sh 已死（原会话退出时被带走）, 14:22 以
setsid 完全脱离方式重启, timegate 休眠至 23:00 自动开跑 A.2a/A.2b 全矩阵。明晨 08:10
已另排自动收数。

## 6. 产物清单

- 本文件: results/r08/m7-final.md
- 全量日志: results/r08/syz-manager-r2.log（已入 git; stats 行含冻结期全程）
- 启动报告: results/r08/m7-round2.md（§0 覆盖面更正 + §4 停机预案 + §5 预判）
- 崩溃原始目录: /home/ppw/syzwork/crashes/（6 目录, 保留现场）
