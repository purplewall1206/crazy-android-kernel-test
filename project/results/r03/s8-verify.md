# M3b.S8 验证记录 (debugfs 观测面收口 + kselftests 接线, 2026-09-16 06:00-07:3x CST)

- 基座: worktree /home/ppw/linux-6.18-m3b46 reset 到 `96466df24387`
  (tag corten-r02-m3b-s46-fix1) + `git clean -fd`, 从头干净切片。
- bzImage: `bzimg/r03-m3b-s8`
  sha256=`61c1d0d951a6b7b29b096ad6fa2b60426f06b3177543cc6419b008466ef67066`。
- 补丁: `patches/r02-m3b-s8.diff` (5 文件, +299/-7, checkpatch --strict 0E/0W/0C)。
- **未 commit**（按任务指示, 留主 agent maintainer 流程）。

## 变更清单

| 文件 | 内容 |
|---|---|
| include/linux/corten_arena.h | `struct corten_arena` 增 `obs` list 节点（S8 观测 ledger, 仅观测, 故障路径零接触）; 声明 `corten_arena_arenas_report/`corten_arena_stats_report`（!ARENA 内联桩）; KUnit 钩子声明 `corten_arena_test_inject_drain_timeout`/`corten_arena_test_drain_timeouts` |
| mm/corten_arena.c | 全局 `corten_arena_list`（list_add_rcu/del_rcu + `corten_arena_list_lock`, 写侧仅 DECLARE 末步/RELEASE·mm_exit 注销点, 读侧 RCU）; `corten_arena_nr_drain_timeouts` 全局镜像（`corten_arena_note_drain_timeout()` 统一记录 per-mm + 全局, 冷路径）; 两个渲染器（arenas 逐行: mm/vma/[start,end)/prot/status active\|dying; arena_stats: live arenas 计数 + drain_timeout 汇总行） |
| mm/corten.c | stats 行渲染抽出 `corten_stats_lines()`（stats 与 arena_stats 共用）; 新增 debugfs 文件 `arenas`/`arena_stats`（0444, 挂既有 `/sys/kernel/debug/corten/`）; render 测试驱动扩 2 个枚举 case |
| mm/corten.h | `enum corten_dbg_file` 增 `CORTEN_DBG_ARENAS`/`CORTEN_DBG_ARENA_STATS` |
| mm/corten_arena_test.c | +2 KUnit: `corten_arena_test_obs_ledger`（DECLARE→渲染含 "[800000,1000000)"+"active"→RELEASE→消失）; `corten_arena_test_drain_timeout_stat`（注入 1 次→全局镜像恰 +1→arena_stats 渲染含新值行） |
| bench/arena-stress/ksmoke.sh（项目侧, 非内核树） | `dbgfs_snapshot()`：CORTEN_ON=1 时跑前/跑后各抓 stats/arenas/arena_stats 到 OUTDIR（debugfs-{pre,post}-*.txt）; bash -n 通过 |

## 设计要点（最小合理方案）

- **全局 arenas 枚举**: per-mm xarray 无法从 debugfs 遍历 mm, 故按任务书用
  DECLARE/RELEASE 时的全局 list 双账。锁: 写侧 `corten_arena_list_lock`
  （比任务书的 ctl_lock 更正确——ctl_lock 是 per-mm, 无法串行化跨 mm 的全局
  list 写者）, 读侧 rcu_read_lock, arena 本体 kfree_rcu 释放天然 RCU 安全。
  ledger 仅观测: 登记点是 DECLARE 全部失败路径之后（不会出现半注册项）,
  注销点在 xa_erase 后（RELEASE 返回前必已消失, 冒烟实证）。
- **drain_timeout 聚合**（final-smoke 遗留项 1）: per-mm 状态同样不可枚举,
  取"记录点全局镜像"而非读时聚合——drain 超时是 kernel-bug 冷路径, 零热路径
  代价（热路径 stat_add 一行未动）。arena_stats = 协议层 9 行（stats 同源）
  + `arenas <n>` + `drain_timeout <n>`; reinstalled/legacy_drift 已在协议层行内。
- 热路径: fault 路径不碰 list/不碰新锁; 唯一新增热路径代码 = 0。

## 验证矩阵

| 项 | 判定 | 证据 |
|---|---|---|
| make -j6 =y 全量 | **PASS** 零新增警告（唯一 objtool cpuidle_enter_state=上游既有, 与 final-smoke 记录一致） | /tmp/s8-build-y{,2}.log; 中途修 1 处缺 include（corten.c→corten_arena.h） |
| checkpatch --strict | **PASS** 0E/0W/0C, 438 行 | patches/r02-m3b-s8.diff |
| KUnit off×1（guest, kunit.filter_glob=corten*） | **PASS** corten 25/0/0, **corten_arena 10/0/1**（+2 新用例 ok, skip=既有 overlap-gate）, corten_fault 3/0/10 | s8-kunit-off-console.txt |
| KUnit on×1（同 bzImage, corten=on） | **PASS** corten 24/0/1（skip=layout 设计内）, **corten_arena 11/0/0**, corten_fault 13/0/0 | s8-kunit-on-console.txt |
| =n 回归（五对象） | **PASS** CORTEN_MM=n 全量构建完成, 唯一警告=上游 objtool cpuidle_enter_state 既有项; `mm/corten*.o` 五对象全部未重建（config 中 CORTEN 全部折叠） | /tmp/s8-build-n.log |
| guest 冒烟 | **PASS** 见下 | s8-smoke-arenas-mid.txt, s8-smoke-stress.log, s8-debugfs-final.log |

## guest 冒烟细节（bzimg/r03-m3b-s8, corten=on）

- arena_stress 4t/15s/64MB touch --verify (seed 4242)：ops=615,570,957,
  errors:0, op_errors:0, checksum=29c7bf9638d37b7f, **RELEASE ok**。
- **运行中** `/sys/kernel/debug/corten/arenas` 恰 1 行:
  `ffff8dca42d012c0 ffff8dca45b52200 [10000000,14000000) 0b active`
  （mm 指针、shadow-VMA、区间、prot、liveness 全部呈现）。
- **RELEASE 返回后**: arenas 表回空（arena 消失）。
- `arena_stats`: enabled=1, reinstalled=1, legacy_drift=1,
  `drain_timeout 1`（= 本 boot KUnit 注入恰好 1 次, 与新用例断言闭环）,
  arenas=0; **drain_timeout/reinstalled/legacy_drift 三项遗留观测需求全接上**。
- dmesg 全程零 panic/Oops/BUG; 2 条 WARNING=corten_test_txn_path_overflow
  设计内注入（final-smoke 基线逐条一致）。

## 遗留 / 移交

1. 本切片未 commit——diff 在 patches/r02-m3b-s8.diff, 由主 agent 派 maintainer。
2. 工作树收尾态: .config 已恢复 =y（CORTEN_MM=y/CORTEN_MM_ARENA=y, olddefconfig 核实）;
   =n 回归曾把树内构建产物翻成 =n, 收尾 =y 全量重建已于 07:14 跑完（唯一警告仍=上游
   objtool 既有项）, 树内 bzImage 与 diff 状态一致。
3. VM 终态: tmux `vm` = bzimg/r03-m3b-s8, corten=on, 冒烟后留存运行。
4. KUnit 注入 drain_timeout 走记录函数直注（构造真实泄漏 ref 会故意泄漏描述符
   并污染后续运行, 不适合周期性 KUnit）——drain 超时行为学证据仍以 r03
   final-smoke 的 7 次 RELEASE 零超时为准。
5. final-smoke 其余非阻塞项（txn_path_overflow WARNING 时钟源统一/soft-dirty
   skip/va_high 既有 fail/ksmoke 预编译口径）不在 S8 范围, 维持原状。
6. ksmoke.sh 的 debugfs 快照为 on 口径自动带出（CORTEN_ON=1 才触发）, off 口径
   行为零改动。
