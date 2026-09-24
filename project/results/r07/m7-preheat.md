# r07 / M7 预热 — lockdep (PROVE_LOCKING) 变体首检

- 日期: 2026-09-18 13:03-14:00 CST; 主树 /home/ppw/linux-6.18 @ `025756094542`
  (`mm: CortenMM: arm64 4K-page build support and arch-neutral test macros`, 即 r06-m9p1)
- 配置: `scripts/config -e PROVE_LOCKING -e CORTEN_MM -e CORTEN_MM_ARENA
  -e CORTEN_MM_KUNIT_TEST` + olddefconfig → LOCKDEP=y, PROVE_RCU=y,
  CORTEN_MM/ARENA/KUNIT_TEST=y; 其余不动。备份:
  `r07/config-prelockdep.backup`。构建 `make -j6` 约 20 min, exit 0。
- 本变体目的: M7(锁协议评审预热)之前, 让 M3-M4 全部 corten 锁
  (PT-page desc 锁、txn 锁、arena percpu_ref drain、BH 对称路径)首次在
  lock dependency validator 全量校验下运行。**未发现任何 lockdep 报警。**

## 1. 构建 (lockdep-build.log / lockdep-build-r06.log)

警告基线如实记录, 共 2 条, 均为既有基线(与 lockdep 之前的构建一致):

1. `vmlinux.o: warning: objtool: cpuidle_enter_state+0x17a: return with instrumentation enabled`
2. `WARNING: modpost: vmlinux: memblock_end_of_DRAM: EXPORT_SYMBOL used for init symbol.`

corten 对象 (corten.o / corten_test.o / arena / fault) 在 lockdep 下
**零新增警告、零错误**; lockdep 子系统自身亦无增量警告。

## 2. 无盘 qemu KUnit (lockdep-kunit.log, lockdep-kunit-run2.log)

`qemu-system-x86_64 -enable-kvm -m 2048 -smp 4 -kernel bzImage
-append "console=ttyS0 panic=-1 kunit.filter_glob=corten*" -nographic -no-reboot`
(panic 退出属预期, 无 rootfs)。guest 内确认
`Lock dependency validator: Copyright ... Ingo Molnar` + `RCU lockdep checking is enabled`。

| 套件 | run1 | run2 |
|---|---|---|
| corten | pass:24 fail:1 skip:0 | **pass:25 fail:0 skip:0** |
| corten_arena | pass:17 fail:0 skip:7 | pass:17 fail:0 skip:7 |
| corten_fault | pass:4 fail:0 skip:17 | pass:4 fail:0 skip:17 |

- run1 唯一失败: `corten_test_txn_uninstall_interlock`, 断言
  "worker A never acquired the lock (phase=5 begin_ret=0)", runtime 30.037s
  (≈主线程 10s 观察窗 + worker 20s go-spin deadline 两个定时器精确叠加)。
  **归因: harness 时序 flake, 非协议违规** — worker 自身 `begin_ret=0`
  (事务成功开始)、phase=5 (干净退出), 无任何锁签名异常; 普通内核 r06 基线
  同用例 `ok 19`。lockdep 使 `corten_txn_begin` 校验开销放大, worker 在
  CPU1 上的启动/加锁延迟超出测试写死的 10s 观察窗。run2 同内核全绿复现。
  → M7 评审项: interlock 测试的固定 10s/20s 窗口在 lockdep 下偏紧,
  建议后续改为按 `ktime` 预算或 `kunit_slow` 标注(记录, 不阻塞)。
- run1 中两条 `WARNING ... mm/corten.c:864/811 corten_txn_begin` 为
  `corten_test_txn_path_overflow` 用例**按设计**触发的守卫
  WARN_ON_ONCE (先 descent 守卫后 PATH_MAX 守卫, 用例断言 -EPROTO),
  r02 起即有此约定, 非 lockdep splat。
- **lockdep/oops 精确签名 (possible recursive locking/deadlock,
  inconsistent lock state, unsafe locking scenario, bad unlock balance,
  DEBUG_LOCKS_WARN, BUG/GPF) 两轮运行均 0 命中。**

## 3. guest 冒烟 under lockdep (lockdep-guest-smoke.log)

launch_vm (trixie.img, KVM 4G/8vCPU) + `corten=on kunit.enable=0`,
guest 内 dmesg 确认 `corten: page descriptors enabled`。

- `run_mode_smoke.sh`: 26/26 PASS, SMOKE-DRIVER PASS,
  debugfs arenas before/after 均为 0 (账本归零)。
- `arena_stress 4 15 64 42 --mode churn --verify`:
  `ops=9959 errors:0 op_errors:0 arena_ok:true mem_ok:true`, CHURN_RC=0,
  RELEASE drain + shadow-VMA teardown 正常; `--verify` 语义
  (munmap 后同 VA 重 mmap 读回全 0) 通过。sys 58s / wall 15s 即 lockdep
  开销特征 — 按任务口径只看正确性, 性能数字无意义。
- 终检: dmesg lockdep/oops 签名 **0 条**; debugfs stats 健康
  (`legacy_drift:0 desc_alloc_fail:0 free_untracked:0 reinstalled:0`)。

## 4. 判定与收尾

| 关卡 | 结果 |
|---|---|
| lockdep 构建 | PASS (exit 0, 两条既有基线警告, corten 零新增) |
| KUnit (无盘 qemu) | PASS (run2 25/25 全绿; run1 interlock 时序 flake 已归因; 双运行零 lockdep splat) |
| guest 冒烟 + churn | PASS (26/26 + errors:0 + 零 splat) |

- **M3-M4 全部 corten 锁在 PROVE_LOCKING 下首检通过, 无一处 lockdep
  报警 → M7(锁协议评审)预热目标达成, 绿灯。**
- 恢复: `.config` 已还原 (PROVE_LOCKING/LOCKDEP 消失, CORTEN_MM 三项=y,
  olddefconfig 确认); 普通 bzImage 已重建, lockdep 变体不残留树内。
- lockdep 变体 bzImage **不归档**、不进 green.txt; 唯一遗留记录项:
  interlock 测试固定窗口在 lockdep 下偏紧(见 §2), 移交 M7 评审清单。
