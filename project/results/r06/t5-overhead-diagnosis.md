# T5 两大灾难性开销信号 · 根因诊断与修复 (r06, 2026-09-18)

- 班次: 09-18 全天窗 (D14 授权), 09:30–14:30 CST
- 诊断对象: results/r06/t5/ 首跑两信号 — ①dedup tcmalloc T0 档 -91.84% (0.372M vs 4.56M blocks/s, wall 311s vs ~23s); ②JVM 2000 线程 spawn >480s vs base 2.7s
- 内核: 诊断在 bzimg/r06-t5 (主树重编 #27, 452ad7b9d91e); 修复验证在主树 commit **ba77046c78fe** (构建 #28/#30), bzImage sha256 8aca322c…ce6894 (bzimg/r06-t5-fix1/)
- VM: trixie 8vCPU/4G KVM, boot `corten=on mitigations=off kunit.enable={0,1}`
- 结论一句话: **信号①根因 = RELEASE 的 drain 在 born-percpu 的 percpu_ref 上每枚 munmap 等满一个 RCU 宽限期 (实测 4.9–20ms), 且等待时持有 mmap_write(W)+ctl_lock; 已用 PERCPU_REF_INIT_ATOMIC (D15) 修复, MODE 恢复到 5.9M blocks/s (超过 base +19%), wall 311s→~18s。信号② (JVM spawn) 的"线程栈 arena 税"初始假设被证伪 (线程栈 MAP_STACK 白名单排除, spawn 微复现 MODE≈BASE); r06 的极慢形态与本 boot 的形态均被登记的 libc PTE 缺陷族 (rc=139) 遮蔽, 留待该缺陷闭合后复测 (详见 §7)。**

---

## 1. 信号①取证 (修复前内核 #27, 逐项原文)

### 1.1 单元探针 churn (mmap(NULL,2M)+touch+munmap 循环, /root/churn, 源码 share/r06t5diag/churn.c)

```
=== BASE churn 1000 iters ===
munmap: n=500 min=52.0us avg=264.5us max=2356.6us
mmap  : min=2.5us avg=20.8us max=2056.9us
=== MODE churn 1000 iters (修复前) ===
munmap: n=500 min=4865.4us avg=8554.5us max=19835.1us     ← 单次 arena RELEASE ≈ 8.55ms
mmap  : min=6.2us avg=35.7us max=755.5us                  ← DECLARE 侧仅 +15us, 便宜
```
- munmap 分布: <1k:0 <5k:2 <10k:409 <20k:89 — 无泄漏 (drain_timeout=0), 全部落在 GP 量级。
- rcu_expedited=1 (/sys/kernel/rcu_expedited 写 1 回读 1) 不改变延迟 (avg 9.46ms) — 排除"普通 GP 等待可加速"路径, 属 rcu_preempt 常规 GP 时长 + 回调调度。

### 1.2 RCU trace 直接对表 (tracepoints rcu:rcu_grace_period + percpu:percpu_alloc_percpu)

```
rcu_preempt-15 [000] 898.353172: rcu_grace_period: rcu_preempt 30421 start
rcu_preempt-15 [000] 898.353175: rcu_grace_period: rcu_preempt 30421 fqswait
rcu_preempt-15 [000] 898.356939: rcu_grace_period: rcu_preempt 30421 end     ← GP#30421 历时 ~3.8ms
```
churn 期间 GP 覆盖 kill→confirm→release→complete 全窗; 8 vCPU 忙闲波动下 GP 3.8–20ms 与 churn min/max (4.9–19.8ms) 一致。

### 1.3 放大机制: 为什么是 12 倍而非 1.03 倍

dedup_eq 的内存形态 (bench/apps/dedup_eq.c): 每 4096 ops 执行一次 `mmap(NULL, 256KB..1MB)+touch` 与最老一块的 `munmap`。8 线程 × 20s × 655360 ops/s ÷ 4096 = **25,600 对大块 mmap/munmap**。

- MODE 下每对 = auto-arena DECLARE (μs 级) + EXACT/release-rule → `corten_arena_release()` → `corten_arena_drain()` = percpu_ref_kill_and_confirm → **percpu→atomic 切换需一次 call_rcu 宽限期** → complete 才到达。
- drain 持 **mmap_write(W)** (DEV-13 修复后的锁序), 8 线程的大块 munmap 在此全串行: 单线程周期 = 4096 ops + 一次 (mmap+munmap); 实测 base 每 cycle ~6ms (全速), MODE 下 cycle ≈ 8×GP ≈ 68–88ms。
- 算术对表: 4096 ops ÷ 87ms/cycle ≈ 47k blocks/s/线程 × 8 = **376k blocks/s ≈ 实测 375,718** (r06 T5 run1)。311-23=288s ≈ 25,600 × 11.25ms。
- lib/percpu-refcount.c 语义确认 (本树): percpu 模式 kill → `call_rcu_hurry(switch_to_atomic_rcu)` → GP 后收集 percpu 计数 → count==0 → release → complete。**born-percpu 的 ref 每次 kill 必付一个 GP; born-atomic (INIT_ATOMIC) 的 ref kill 是同步 put, release 回调直接在 killer 上下文触发, 零 GP。**

### 1.4 debugfs 计数互证 (修复前, 一次完整 MODE dedup 20s 档)

```
auto_mmaps        3306 ->   28906   (+25,600 = blocks 104857600/4096, 精确)
munmap_releases   1426 ->   27026   (+25,600, 每笔大块 munmap 都是全量 RELEASE)
drain_timeout        0 ->       0   (零泄漏, 等待全部"健康"地等满 GP)
auto_attach_fail/exhausted/fallbacks 0; RSS 64MB 与 base 同
```
`munmap_releases` 计数器只统计 release-rule 改判 (tail rule) 的路径, 全量 EXACT 走 `corten_nr_munmap_releases` 之外的主 RELEASE 路径 — 数字仍精确吻合的原因: 本 workload 每笔都在改判集内 (整 arena+page-tail 形状)。

### 1.5 perf + PSI + vmstat (修复前, MODE dedup 运行中采样)

- perf record -F399 -g -p <dedup> (25s 窗) top: `worker 30.7%` (用户态真实工作), kernel 侧 `finish_task_switch 12.6%`, 其下 `schedule ← rwsem_down_write_slowpath ← down_write_killable ← vm_mmap_pgoff ← __mmap (6.3%)` 与 `down_write ← corten_arena_release ← corten_arena_munmap_route ← __do_sys_munmap (3.4%)` — **线程成群堵在 mmap_write 上** (持锁者睡在 drain)。
- PSI memory: some/full avg10=0.00 全零; vmstat: **idle 94–95%** — 完全 wait-bound, 排除内存压力与 CPU 饱和 (步骤 1a/b 的排除项落实)。
- strace -f 窗口 (15s, MODE): mmap 1494 / munmap 1419 / mprotect 16 / madvise 0 — ~93 对/秒 ≈ 25,600/280s; munmap 地址 0x100000000000 系 (MODE 窗口) 证实大块全部进了 arena 窗; tcmalloc 自身 MAP_FIXED 再提交在 0x7f… (legacy 区, 不经 arena)。

### 1.6 线程 spawn 微复现 (信号②第一证伪)

spawn.c: 纯 pthread_create/join 1000 次 ×2-3 reps:
```
BASE: 693/636/488 ms      MODE: 576/605/544 ms      (修复前内核; 差异<噪声)
auto_mmaps +0 — 线程栈 (glibc mmap MAP_STACK) 不进 arena, 无 DECLARE/RELEASE
```
→ **"2000 线程 = 2000×(8MB 栈 DECLARE+退出 RELEASE)"的初始假设不成立**。

---

## 2. 修复 (第 4 步授权, ≤50 行)

### 2.1 生产修复 (mm/corten_arena.c, DECLARE 描述符初始化)

```c
-	ret = percpu_ref_init(&arena->active, corten_arena_active_release, 0,
-			      GFP_KERNEL);
+	/* Born atomic (D15): a percpu-born ref forces percpu_ref_kill()'s
+	 * atomic switch through a full RCU grace period, and RELEASE drains
+	 * with mmap_write held -- every arena munmap paid one GP
+	 * (4.9-20ms measured, r06-t5), which ran the dedup_eq tcmalloc arm
+	 * at 12x its base wall time.  An atomic-born ref makes the kill
+	 * synchronous: the drain only ever waits for in-flight
+	 * transactions, never for grace. ...
+	 */
+	ret = percpu_ref_init(&arena->active, corten_arena_active_release,
+			      PERCPU_REF_INIT_ATOMIC, GFP_KERNEL);
```
语义保持: tryget_live 在 atomic 模式为 `atomic_long_inc_not_zero` (kill 后 DEAD 位同步置位 → tryget 同步失败); drain 仍等待在飞事务 (KUnit drain-sync 用例不变绿通过); 超时/泄漏路径不变。代价: fault/route 路径的 tryget/put 从 percpu 变共享 cacheline 原子操作 — 事务本来就串行在 covering desc write lock 上, 实测无回归 (§4.3 pf 微基)。

**Commit: ba77046c78fe "mm: CortenMM arena: born-atomic transaction refs, GP-free drain (D15)"** (主树 android17-6.18, 未 push, 未打 tag — 留主 agent 门)。

### 2.2 KUnit 锚 (mm/corten_arena_test.c, +1 用例)

`corten_arena_test_ref_born_atomic`: DECLARE 后立即 (未 kill、无切换) 经 `__ref_is_percpu()` 断言 ref 已处于 atomic 模式 + RELEASE 语义不变。锚住 D15: 将来若有人把 init flag 改回 percpu (或引入 percpu 重初始化), 用例即红。

### 2.3 并发 KUnit 用例同步化 (必要跟随修)

`corten_arena_test_concurrent{,_window}` 原"自由竞速"依赖旧 drain 的 GP 等待给 reader 一个 ~10ms 的窗口; drain 变 μs 级后 reader 必输 (实测 `gets==0` → not ok; 且 reader pin 偶发落入 drain 10s 超时窗造成分钟级 stall)。改为 completion 握手: worker 第 0 次 attach 后等 `c->got` (10s 上限), reader 首次 pin 成功时 `complete(&c->got)` 后短持即 put — **gets≥1 变为确定性**, drain-race 语义 (release 等在飞 pin) 保留。此为测试件修改, 非生产语义变更。

---

## 3. 修复后验证 (主树 #28/#30 = ba77046c78fe, boot corten=on mitigations=off kunit.enable=0)

### 3.1 单元 (churn 600 iters, 同 boot)

```
BASE munmap: avg=278.4us      MODE munmap: avg=227.8us    (修复前 MODE: 8554us → 62 倍改善)
MODE <1k bucket 245/300, max 1.4ms — GP 完全消失
```

### 3.2 dedup tcmalloc 完整档 (8 20 42, ×3 中位)

| 臂 | blocks/s (×3) | wall |
|---|---|---|
| BASE (tcmalloc, 无 MODE) | 4.96 / 5.01 / 4.99 M | ~21.0s |
| **T0/MODE (修复后)** | **5.94 / 5.81 / 6.12 M** | **~17.6s** |
| (对照) T0/MODE 修复前 | 0.372 / 0.372 / 0.368 M | 279–285s |

- 中位对比: MODE 5.94M vs base 5.00M = **+19%** (超 "±10% 内" 验收线, 方向为优); vs r06 T5 的 -91.84% = **16 倍恢复**。
- 计数互证: auto_mmaps 0→76,800 / munmap_releases 0→76,800 (800 churn + 3×25,600, 精确), drain_timeout=0, attach_fail=0, exhausted=0。
- MODE 反超 base 的机制候选 (非本班结论): arena 命中 FRESH/事务路径的 zap 与 zero-page 行为对 tcmalloc 的 DONTNEED-free churn 更友好; 留 M8 复核。

### 3.3 无回归 sanity

- spawn 1000×2: BASE 458ms vs MODE 329ms (MODE 不慢)。
- mmbench pf t8 (ops_per_us): low base 0.002/0.017 vs mode 0.038/0.038; high base 0.015/0.037 vs mode 0.036/0.038 — **fault 路径原子 tryget 无回归** (pf/low 高方差为 M1 已知登记形状)。

### 3.4 KUnit (kunit.enable=1, 3 次 boot)

| run | corten | corten_arena | corten_fault |
|---|---|---|---|
| 1 (host 噪声, TSC watchdog 抖动) | 24 pass + 1 fail(flake) | 23/23 | 19/19 |
| 2 | 24 pass + 1 skip | **23/23** | 19/19 |
| 3 | 24 pass + 1 skip | **23/23** | 19/19 |

- 新用例 `ok 6 corten_arena_test_ref_born_atomic`, 并发两用例 `ok 20/21`。
- run1 的 `txn_uninstall_interlock` violations==1: mm/corten_test.c 与本改动零耦合 (grep percpu/arena = 0 命中), 该 boot kunit 用时 25s vs 正常 5-8s (host 载荷), 判环境 flake; run2/3 复测全绿。dmesg 固有 WARNING (txn_path_overflow 注入 / fork_demote rwsem 探针) 与登记基线一致。

---

## 4. 信号② (JVM 2000 线程 spawn) — 现状与遗留

1. **证伪**: 线程栈不走 arena (MAP_STACK 白名单排除, §1.6), "栈 DECLARE/RELEASE 税"假设推翻。
2. 修复前内核上 r06 的 >480s 形态本班未能稳定复现: 本 boot 的 ASLR 掷骰下 JVM 在 MODE 启动期即触发**登记在案的 libc PTE 缺陷族** (`SIGSEGV libc.so.6+0xa2a63/0xa2b40`, error 7, 与 metis/psearchy/dedup-glibc 的 rc=139 同族, T5 报告 §6 已登记 "M8 前必须闭合该 PTE 形状") — 修复前 my 200/2000 线程尝试同样崩 (该缺陷与 D15 无关, 两内核同形)。修复后 3×2000 也全数触崩 (rc=137=超时杀+libc 崩退混合), 无一进入可测 spawn 段。
3. 计数侧证据 (MODE JVM, 每次尝试): auto_mmaps +55~293, mprotect_routes +235~337, munmap_releases ≤1, drain_timeout=0 — spawn 慢形态与 RELEASE/drain 无直接关联证据。
4. **结论/建议**: 信号②的两个可测成分 (spawn 微复现零差异; r06 慢形态复现被登记缺陷遮蔽) 都不再指向本修复的领域。遗留动作 = 按登记口径先闭合 T0b libc PTE 形状, 然后用同一 JThreadBench/strace -f -c 口径复测 (本班已在 guest 备好 /root/JThreadBench.class + jvm.strace 手法); 若复测仍 >2×base, 下一嫌疑按序: ① routed mprotect 的逐页元数据事务 (glibc arena 提交), ② arena fault 路径 desc 写锁在 spawn 风暴下的串行化。

## 5. 修复设计备选 (按诊断结果评估, 供 M8/规划者)

| 方案 | 内容 | 判定 |
|---|---|---|
| **A. born-atomic (已实施)** | INIT_ATOMIC, kill 同步, drain 只等在飞事务 (~1 行) | 采纳; 16 倍恢复, 零新语义 |
| B. 异步 drain (workqueue/RCU 延迟释放) | kill 后立即 do_munmap, 描述符经 callback 回收 | 需重构 ar->vma 生命周期与 [FAIL-2] 安全论证; 收益与 A 相同, 复杂度高数个量级 — 不采纳 |
| C. arena 池化/T1 magazine | 窗口 VA 复用 + per-cpu 槽, 消灭 DECLARE/RELEASE 次数 | 属 T1 计划; A 落地后 dedup 已 +19%, C 仅剩 µs 级 DECLARE 税可省 — 维持 M8 议题 |
| D. drain 移出 mmap_write | 先 do_munmap 后 drain | 破坏 [FAIL-2]/事务安全论证, 不采纳 |

若未来出现 percpu 模式才可满足的 fault 吞吐场景 (超大 arena 多线程高频 fault 的 cacheline 争用), 升级路径 = A + desc 级 percpu 计数聚合, 不必回退 D15。

## 6. 证据清单

- 探针源码与产物: share/r06t5diag/{churn.c,spawn.c}, guest /root/{churn,spawn,dedup.strace,dedup_t0.perf.data,psi_vmstat.log,st_dedup*.txt,f0/f1,j0/j1}
- KUnit: share/r06t5diag/kunit-run{2,3}.log; 构建: results/r06/t5-overhead-build{28,29-breadcrumb,30}.log
- 本报告即累积取证记录; 关键数字在 §1/§3 表格, 探针可一键复跑 (gcc + 两条命令)。

## 7. 收工状态

- 主树 HEAD = ba77046c78fe (1 commit on 452ad7b9d91e), 工作树 clean; 未 push、未动其他 worktree; bzimg/r06-t5-fix1/ 已登记 (sha256 8aca322c…ce6894) + green.txt 追加行。
- VM: 留 r06-t5-fix1 corten=on 运行 (tmux session `vm`)。
- 时间: 09:30–14:30 CST, 距 22:00 硬停余量充足; 密码未落盘。
