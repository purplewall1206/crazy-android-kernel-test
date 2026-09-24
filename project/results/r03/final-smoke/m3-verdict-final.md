# M3 DoD 终判冒烟 (r03 final, 2026-09-16 05:25–06:1x CST)

- **VERDICT: M3 里程碑 PASS** — DoD 四条 4/4 PASS, 对象 commit `96466df24387`
  (tag corten-r02-m3b-s46-fix1, 基座 7bba3b9f7390 + A/B/C 三轮缺陷修复 + fix3r
  review 增量)。bzImage `bzimg/r02-m3b-s46-fix1`
  sha256=`1f0cb1b65a8adf3ef4ef754cd5f8b86c38e6a15d8a68dfc5df68c6ec57cb7038`。
- r03 首轮判定 2/4 FAIL 的全部失败模式（A 新鲜页 MAPERR / B RELEASE drain 挂死 /
  C churn 内容漂移）在本核上零复现; 三个独立 on-boot + 7 次压测全程 **零 panic /
  Oops / BUG / WARNING / D 状态**。
- 纪律: 未改码; 未碰 m3b46/m3b/m9 worktree; 未 push; 密码未落盘。
- 配置: CORTEN_MM / CORTEN_MM_ARENA / CORTEN_MM_ARENA_KUNIT_TEST /
  CORTEN_MM_ARENA_FAULT_KUNIT_TEST / ANON_VMA_NAME / KUNIT 全 =y
  （olddefconfig 后逐一核实）。构建 exit 0, 警告仅上游既有
  （objtool cpuidle_enter_state）。

## DoD 逐项判定

| # | DoD 条目 | 判定 | 证据 |
|---|---|---|---|
| 1 | arena_stress 压测器零 panic + 校验和正确 | **PASS** | 5 项判据全部 exit 0 + errors:0 + op_errors:0 + RELEASE ok + checksum 产出（见下表）; 全程 dmesg 零 panic/Oops/BUG/WARNING, 无 D 状态残留 |
| 2 | /proc/pid/maps 正常 | **PASS** | maps_check.sh exit 0 `RESULT: PASS (pid 683)`: 全部行解析、零重叠、arena 恰 1 条 `10000000-14000000 rw-p [anon:corten_arena]`（mixed 25s 运行中快照, maps-check-final.log） |
| 3 | kselftests/mm 冒烟 off/on 无确定性回归 | **PASS** | 同一 bzImage 双口径: off 10 pass + 1 skip + **1 fail**（va_high_addr_switch, 基线既有——r03 已证 m3a-f1 基线同断言失败）; on 10 pass + 1 skip + **1 fail_soft**（同一 va_high）。fail-set 完全一致, 零新 fail。mremap_test 双臂均 pass（r03 的 flake 未复现） |
| 4 | perf 无 find_vma/mmap_lock 符号 | **PASS** | churn 4t 运行中 `perf record -a -e cycles:k` 30s: 367K 样本, `find_vma\|mmap_lock` = **0 命中**; r03 六符号宽口径（find_vma/lock_vma_under_rcu/vma_start_read/lock_mm_and_find_vma/mmap_read_lock/mas_walk + mmap_write_lock）= **0 命中**; `corten_arena_lookup_get` 1.87% 在榜, `do_user_addr_fault` 11.64% / `__send_ipi_mask` 24.28%（构成与 r03 首轮 PASS 时一致）。perf 同窗口压测自身 errors:0 + RELEASE ok |

## DoD#1 压测矩阵（判据: exit 0 + errors:0 + op_errors:0 + RELEASE ok + checksum）

| 项 | 参数 | 结果 | 证据文件 |
|---|---|---|---|
| ① touch | 4t / 30s / 64MB / seed101 | exit 0, errors:0, op_errors:0, RELEASE ok, checksum=97e9d4209babf873, ops=12.8 亿 | stress-touch4t.log |
| ② churn --fixed --verify | 4t / 30s / 64MB / seed102 | exit 0, errors:0, op_errors:0, RELEASE ok, checksum=800c5ab717050a5b, minflt 920 万 | stress-churn4t.log |
| ③ mixed --verify | 4t / 30s / 64MB / seed103 (+40s seed104 复跑) | exit 0, errors:0, op_errors:0, RELEASE ok | stress-mixed4t.log |
| ④ touch | 8t / 30s / 128MB / seed106 | exit 0, errors:0, op_errors:0, RELEASE ok, ops=23.2 亿 | stress-touch8t.log |
| ⑤ churn 小 arena --verify | 4t / 30s / **16MB** / seed107 | exit 0, errors:0, op_errors:0, RELEASE ok, minflt 906 万 | stress-churn16mb.log |
| perf 同窗口 churn | 4t / 38s / 64MB / seed111 | exit 0, errors:0, op_errors:0, RELEASE ok（perf record 并行 30s） | perf-stress.json, perf-final.log |

## KUnit（guest 真机自跑, 两个 on-boot 均复现）

| 口径 | corten | corten_arena | corten_fault |
|---|---|---|---|
| off（本核） | 25/0/0 | 8/0/1 | 3/0/10 |
| on boot#1 | 24/0/1（skip=layout 设计内） | **9/0/0** | **13/0/0** |
| on boot#2（终态 VM） | 24/0/1 | **9/0/0** | **13/0/0** |

- fault 13/13 含本轮新增的 fresh_fault（缺陷 A 盲区用例）、churn_repro
  （缺陷 C 转正用例）、untracked_drift（缺陷 C 注入用例, -EOPNOTSUPP 语义）——
  三条修复的真链路用例首次在终判里全绿。
- off 臂 dmesg 仅 2 条 WARNING = `corten_test_txn_path_overflow` KUnit
  fail-alloc 注入设计路径（ok 21 PASS）, 与 r03 基线 dmesg 逐条一致, 非新增。

## off 臂不变式（corten 无参数, 缺省关）

- boot clean; 启用日志零条（`requested on` 计数=0, 红线保持）。
- zram: /dev/zram0 lz4 2G swap on; lru_gen enabled=0x0007; 9p mount OK。
- mmbench 非 arena 进程 3 配置（mmap-low t1 / pf-high t2 / unmap-low t4, 1s）
  全部有效 JSON 零异常, kernel 字符串=6.18.32-g96466df24387（mmbench-off-final.log）。

## debugfs 计数（/sys/kernel/debug/corten/stats, on 臂全量冒烟后）

```
enabled             1
ptdescs             359
meta_arrays         360
desc_alloc_fail     3      (KUnit fail_alloc 设计内, 恒定)
meta_alloc_fail     1      (同上)
free_untracked      5      (r03 起恒定, KUnit 注入残留, 非运行期新增)
reinstalled         1      (幂等补装防线, 会话内触发 1 次并自愈)
legacy_drift        1      (untracked 窗口防线, 触发 1 次即闭合, 零内容错误)
```

- drain_timeout: 全局 stats 无此行（CORTEN_ARENA_STAT_DRAIN_TIMEOUTS 为 per-mm
  统计, debugfs 聚合接线留 S8）。行为学证据: 7 次 RELEASE + 2 次 KUnit
  全程零 10s 超时、零 D 状态——降级路径本次零触发。

## 与 r03 首轮 FAIL 的对照

| r03 失败模式 | 首轮表现 | 本轮 |
|---|---|---|
| A 新鲜页 fault MAPERR | touch/mixed 100% 首页 SIGSEGV exit 139 | touch/mixed 全过, 新鲜页合成路径 + fresh_fault 用例绿 |
| B RELEASE/exit drain 挂死 | D 状态不可杀 ×4+ | 7 次 RELEASE 全秒回, 配对修复 + 10s 超时降级在位未触发 |
| C churn/mixed 内容错误 | errors 54599 / probe3 96 失败 | --verify 全过 errors:0, probe3 防线计数仅 legacy_drift=1 自愈 |

## M3 里程碑总判定: **PASS**

四条 DoD 全过, 头条主张（arena 内核热路径脱离 VMA 树/mmap_lock 且内核照常
运行）在真 guest 双口径（off 不变式 + on 全功能）下成立。

非阻塞遗留项（移交, 不影响本判定）:
1. per-mm arena stats（含 DRAIN_TIMEOUTS）debugfs 聚合接线 → M3b.S8。
2. `corten_test_txn_path_overflow` 注入路径 2 条设计内 WARNING 与 interlock
   时钟源统一（M3a §5 遗留, 非阻塞）。
3. MEM_SOFT_DIRTY=n 导致 ksmoke soft-dirty skip（config 可选项, 与 Corten 无关）。
4. va_high_addr_switch 为基线/环境既有 fail（m3a-f1 基线复现）, 上游因素。
5. ksmoke 二进制为 01:18 预编译（share/ksmoke-build）, 本轮经 9p 重放于
   /tmp/ktree 口径与 r03 一致。

## 产物索引（results/r03/final-smoke/）

- 构建: bzimg/r02-m3b-s46-fix1 + bzimg-r02-m3b-s46-fix1.sha256（host）
- off 臂: dmesg-off-full.txt, kunit-off-guest.txt, mmbench-off-final.log,
  env-off-final.log, ksmoke-off-final.{json,log}
- on 臂: dmesg-on-full.txt, ksmoke-on-final.{json,log},
  stress-{touch4t,churn4t,mixed4t,touch8t,churn16mb}.log,
  maps-check-final.log, perf-final.log, perf-report-final.txt,
  perf-stress.json, mixed-bg{,2}.json, debugfs-final.log
- VM 终态: tmux `vm` = bzimg/r02-m3b-s46-fix1, corten=on, 无僵尸, 留运行。
