# M3 DoD 冒烟判定 (r03 夜, 2026-09-16 00:20–02:00 CST)

- **VERDICT: FAIL** — M3 DoD 四条中 2 PASS / 2 FAIL。头条项「arena_stress 压测器
  零 panic + 校验和正确」未能达成: arena **新鲜页 fault 路径确定性 MAPERR** +
  **RELEASE/exit 路径 drain 挂死（D 状态, 不可杀）**。两类缺陷均 100% 复现、
  跨 4 次独立 boot（含修复 ANON_VMA_NAME 配置后的终版镜像）。
- 对象: commit `7bba3b9f7390` (tag corten-r02-m3b-s46), 主树构建,
  `bzimg/r02-m3b-s46` sha256=`9351100b2cb60e21f9fd847742950067e0ee47fbbab4095a9a3413d51f9c9228`
  （ANON_VMA_NAME=y 修正后终版; 首版 `cf5c4e80...` 因主树 .config 缺
  ANON_VMA_NAME 被替换, 已重验 off 不变式）。
- 配置: CORTEN_MM / CORTEN_MM_ARENA / CORTEN_MM_ARENA_KUNIT_TEST /
  CORTEN_MM_ARENA_FAULT_KUNIT_TEST / ANON_VMA_NAME 全 =y; KUNIT=y。
  构建 exit 0, 警告仅上游既有 2 条（objtool cpuidle_enter_state /
  modpost memblock_end_OF_DRAM, 与 s46-verify 记录一致）。
- 纪律: 未改内核码; 未碰 m3b46/m3b/m9 worktree; 未 push; 密码未落盘。

## DoD 判定表

| # | DoD 条目 | 判定 | 证据 |
|---|---|---|---|
| 1 | 压测器零 panic + errors:0 + JSON 正常 | **FAIL** | 见下「失败模式 A/B」; 内核侧全程零 panic/Oops/BUG, 但压测器无法完成任一 touch/mixed/churn 全程; JSON 因挂死/段错误从未产出 |
| 2 | /proc/pid/maps 正常 | **PASS** | maps_check.sh exit 0: 33 行全解析、无重叠、arena 恰 1 条映射覆盖 `[10000000,20000000)` 且名为 `[anon:corten_arena]`（maps-check3.log, churn 运行中快照） |
| 3 | kselftests/mm 冒烟 off/on | **PASS**（附注） | off: 10 pass/1 skip/1 fail; on: 同 fail-set + 2 fail_soft（详见 §3）; on 无新增确定性 fail |
| 4 | perf 无 find_vma/mmap_lock 符号 | **PASS** | perf 6.12, `cycles:k` system-wide 213K samples（churn 运行中）: `find_vma\|lock_vma_under_rcu\|vma_start_read\|lock_mm_and_find_vma\|mmap_read_lock\|mas_walk` **0 命中**; `corten_arena_{lookup_get,fill_upper,user_fault,zap_window}` 均在榜（perf-report.txt）。ftrace 替代口径不可用: 本 config 无 FUNCTION_TRACER（available_tracers=blk nop） |

## 失败模式 A: 新鲜页 fault → 确定性 SEGV_MAPERR（DoD#1 主因）

- 现象: DECLARE 成功后, 对 arena 内**从未触过**的页的第一次 fault（读/写同）
  返回 SIGSEGV si_code=1 (MAPERR), 5/5 次重试永不映射（pagemap present=0）。
  arena_stress `--mode touch` 首页即死 exit 139; 多线程时各线程首页即死
  （内核 segfault 记录 error 6 = user+write+not-present）。
- 复现: 4 次独立 boot（00:41 首版, 00:47, 01:10 终版 off→on）× 多参数
  （1/4 线程 × 16/64/256MB × 多 seed）全 100%。
- 定位（只读码, 未改）: `do_user_addr_fault` 钩子 → `corten_arena_user_fault`
  → `corten_arena_fault_once`: fill_upper → lock_range → `corten_query` 返回
  **state=CORTEN_INVALID(=0, 零初始化元数据)** → `corten_arena_dispatch()`
  default 分支 → `CORTEN_DISP_MAPERR`（mm/corten_arena.c:833）→
  fault.c bad_area_nosemaphore。
- 根因指向: fault_once **缺少「arena 内新鲜页 → 视作 PrivateAnon 虚拟分配」
  的合成步骤**（论文语义 PrivateAnon=virtual alloc on access; churn 路径靠
  mmap mark 显式写 PRIVATE_ANON 绕过此问题——churn 线程本体全绿恰好反证
  事务引擎其余部分正常）。KUnit corten_fault 10/10 全过是因为用例自行播种
  元数据, 未覆盖「query 到 INVALID 的新鲜页」真链路。
- 波及: `--mode touch` / `--mode mixed` 必死; churn 线程本体不受影响
  （仅在 mark 过的 chunk 内触页）。16MB 小 arena 边界同病（mini_probe2 8MB）。

## 失败模式 B: RELEASE / 进程退出 drain 挂死（DoD#1 次因）

- 现象: churn --fixed 跑满设定秒数到达收尾 `prctl RELEASE` 后, 进程入
  **D 状态永不返回**; `timeout`/SIGTERM/SIGKILL 均无法终止（SIGKILL 后转
  exit_mmap 路径继续挂）。4+ 次复现（两次独立 boot + 终版镜像）。
- 挂点: `corten_arena_release+0xff` → `corten_arena_drain()` →
  `wait_for_completion(&arena->drained)`（percpu_ref kill 后等待归零确认;
  mm/corten_arena.c:155-159）; SIGKILL 路径挂点 `corten_arena_mm_exit+0x88`
  ← exit_mmap ← mmput ← do_exit（同 drain 类）。
- 根因指向: churn 的 mark/unmap 事务路径泄漏 arena->active percpu_ref
  （或 confirm 回调路径未触发）, drain 永不完成 → **arena 进程成为不可杀
  僵尸**。desc_alloc_fail/meta_alloc_fail 恒为 1（KUnit fail_alloc 设计内）,
  free_untracked=5 恒定, 无递增——非分配失败所致。
- 波及: 任何到收尾的 churn 压测; arena 进程无法终止（系统级健康风险）。

## §3 kselftests 冒烟细节（DoD#3 PASS 的附注）

- off（KSMOKE_ON=0）: 12 项 = 10 pass + soft-dirty skip（内核无
  MEM_SOFT_DIRTY, rc=4 设计内）+ **va_high_addr_switch fail**。
  va_high 在 m3a-f1 基线内核（6.18.32-g1284a235f751）上**同样失败、同一
  断言**（mmap MAP_FIXED at addr_switch_hint）→ 基线/环境既有, 非 S4-S7
  回归。
- on（CORTEN_ON=1）: 9 pass + soft-dirty skip + va_high fail_soft（与 off
  同 fail-set）+ **mremap_test fail_soft**。mremap_test 于同 boot 即刻复跑
  3/3 rc=0（not ok 26 "move multiple invalid vmas" 为时序 flake, exit 码
  抖动）→ 判 flake 非 corten 回归; 如实记录。
- 结论: on 相对 off **无确定性新 fail**, 符合「同 fail-set」判据（以 3 次
  复跑定性 mremap 为 flake）。

## KUnit（guest 内开机自动跑, 非 host 无盘）

| 口径 | corten | corten_arena | corten_fault | 备注 |
|---|---|---|---|---|
| off（首版镜像 run1） | 24/1/0 | 8/0/1 | 3/0/7 | 唯一 fail=txn_uninstall_interlock, **M3a 报告 §5 签名A 原样**（a_locked=0+violations=1+a_err=1, runtime 20.4s; 时钟发散伪影, 已定性非协议缺陷）; 本 boot 恰在 make -j12 结束 90s 干扰窗内 |
| off（同镜像 run2） | **25/0/0** | 8/0/1 | 3/0/7 | 无负载复跑全绿, flake 闭环 |
| off（终版镜像） | **25/0/0** | 8/0/1 | 3/0/7 | 重验一致 |
| on（终版镜像） | 24/0/**1** | **9/0/0** | **10/0/0** | skip=corten_test_layout（设计内 "SKIP corten=on boot"）; **corten_fault 真链路 10/10 含 fill_upper_race/map_race/chunk_unmap 首次在真 guest 全绿** |

注: s46-verify 的 off×2（21/0/4）与 on（20/0/5）为 host 无盘 qemu `-smp 2`
口径——`num_online_cpus()<3` 使 4 个多核用例 skip; 真 guest 8 vCPU 下它们
首次实跑, 与本表差异全部由 skip 数差解释。

## 其余冒烟面（全绿）

- corten=off 基线不变式: boot 3x clean; dmesg 零 Oops/panic; zram lz4 OK;
  lru_gen enabled(0x0007); 9p mount OK; mmbench 非 arena 进程 3 配置
  （mmap-low t1 / pf-high t2 / unmap-low t4, 1s）全部有效 JSON 零异常。
- corten=on 启用日志: `corten: requested on, activating at initcall time` +
  `corten: page descriptors enabled`（D9 两段式）。
- debugfs: `/sys/kernel/debug/corten/{stats,txn,dump}` 在 =on 下读数正常
  （stats: ptdescs/meta_arrays 随 DECLARE/fill_upper 增长, desc_alloc_fail=1
  为 KUnit fail_alloc 设计内; per-fault 计数闭合按计划留 S8 接线）。
- perf 符号判定补充: 榜首 do_user_addr_fault 11.2%, __send_ipi_mask 19.7%
  （TLB shootdown 风暴, churn zap 所致, 记录备考）; rwsem_spin_on_owner 3.8%
  存在（mmap 事务路由的 rwsem 或其他锁, 无 mmap_read_lock 符号命中）。

## 产物索引

- smoke-off/: serial-boot.log, serial-off2.log, dmesg-off-full.txt, env-off.log,
  dmesg-off.log, mmbench-off.log, kunit-off-guest.log, kunit-off-run2-guest.log,
  kunit-interlock-fail.log, off-recheck-newsha.log, ksmoke-off.log,
  ksmoke-off.json
- smoke-on/: boot-on.log, boot-on-newsha.log, dmesg-on-full.txt,
  dmesg-on-final.txt, serial-on-boot.log, serial-on-final.log,
  serial-release-hang.log, probe.log, probe-final.log, crash1-probe.log,
  stress-touch.json(err), stress-churn-fixed.json(err), touch-final.log,
  release-hang-evidence{,2}.txt, maps-check.log, maps-check2.log,
  maps-check3.log, perf-symbols.log, perf-report.txt, debugfs-final.log,
  ksmoke-on.log, ksmoke-on.json, r03-ksmoke-logs-{off,on}/（share 内复制）
- 宿主: bzimg/r02-m3b-s46 + results/r03/bzimg-r02-m3b-s46.sha256
- 测试探针: bench/share/mini_probe.c（SIGSEGV-retry + si_code + pagemap 取证）

## 移交 dev 班次（不修码, 本班只定性）

1. 失败模式 A: fault_once 新鲜页 INVALID→PRIVATE_ANON 合成缺失（或
   DECLARE/fill_upper 应全帧播种）——需补「query INVALID 的新鲜页」真链路
   KUnit 用例。
2. 失败模式 B: churn mark/unmap 路径 percpu_ref 泄漏定位 + drain 加
   可诊断超时告警（不可杀僵尸的风险不可接受）。
3. interlock 测试时钟源统一（M3a §5 遗留建议, 非阻塞）。
4. ksmoke soft-dirty 依赖 MEM_SOFT_DIRTY（config 可选项, 与 Corten 无关）。
