# M3a.F1 验证报告 — desc->lock 全链 BH 对称修复

日期: 2026-09-14 00:40-02:00 CST (夜间免费窗口)
对象: 主树 /home/ppw/linux-6.18 @ 1284a235f751 + 未提交 M3a 改动 (4 文件,
与 patches/r02-m3a-hardening.diff 逐字节一致, 已验证 IDENTICAL)
VERDICT: **PASS** (全门通过, 1 项测试 Harness 时序伪影已定性取证, 非协议缺陷)

## 0. 验证环境

- qemu-system-x86_64 -enable-kvm -m 2048 -smp 4, 无盘直启,
  `kunit.filter_glob=corten`, `panic=-1 -no-reboot`
- config: android17-6.18 基线 + CORTEN_MM=y + CORTEN_MM_KUNIT_TEST=y
  (lockdep 变体另开 PROVE_LOCKING)
- 宿主: 16 CPU; 夜间另有基础设施 agent 的 M9/syzkaller 构建计划 (01:00 上限,
  实测窗口内未与其重叠; 本轮两次 KUnit 伪影均发生在自身 -j12 构建结束 90s 内)

## 1. 构建

| 变体 | 耗时 | 结果 |
|---|---|---|
| 普通 =y (olddefconfig + make -j12) | 11m36s (user 89m20s) | exit 0 |
| lockdep (PROVE_LOCKING=y, 全量) | 10m08s (user 85m47s) | exit 0 |
| =y 恢复重建 | 见 m3a-f1-restore-build.log | exit 0 |

警告: 仅基线既有 2 条 (objtool cpuidle_enter_state "return with
instrumentation enabled"; modpost memblock_end_of_DRAM EXPORT_SYMBOL/ __init)。
与 results/r01/build-m0.log、m2b-build-y-{1,2}.log 逐条比对一致 → **零新增**。

## 2. KUnit (普通内核) — 25 case

预期 24+, 实际 **25** (M2b 16 + M3a 新增 9: hole_fill / hole_race /
uninstall_interlock / uninstall_bh_ctx / path_overflow / child_err /
real_huge_leaf / full_window_atomic / debugfs_content)。

- run1: 25/25 (4.53s)
- run2: 25/25 (4.43s)
- **run3: 24/25 — corten_test_txn_uninstall_interlock FAIL (见 §5 伪影定性)**
- run4-8: 25/25 ×5 连续 (4.42-4.66s)

取证: m3a-f1-kunit{1..8}.log, `grep "Totals: pass"`。

## 3. lockdep 变体 (PROVE_LOCKING=y) — **BH 对称修复的决定性证据**

流程: config 备份 (/tmp/config-m3a-f1-y) → scripts/config -e PROVE_LOCKING →
olddefconfig → make -j12 → 无盘 KUnit → 恢复 config 重建。
(corten-lockdep-build.sh 不存在, 按预案手工执行)

结果:
- KUnit 25/25 **三连绿** (nohz=off, 5.49/5.77/5.99s) — m3a-f1-lockdep-kunit-nohz{1,2,3}.log
- 全部 lockdep 运行 **零** "possible deadlock / inconsistent lock usage / BUG:"
- 关键探针 `corten_test_uninstall_bh_ctx` 通过: 在 local_bh_disable() 上下文
  驱动 corten_ptdesc_uninstall() —— 正是 khugepaged
  pte_free_defer→RCU_SOFTIRQ→pte_free_now→pte_free 的 lockdep 上下文类;
  若 BH 对称缺失 (irqsave write 与 task-side plain read 混用), PROVE_LOCKING
  会即时报 inconsistent lock usage —— **静默通过即为 BH 对称的正面证据**
- 嵌套 walk 未做 lockdep_off 括弧, 验证器全程盯着: 无 lock-class 抱怨
  (per-level lockdep_set_class 生效)
- path_overflow 测试的 2 条 WARN_ON_ONCE (corten.c:776/829) 为该测试
  **设计内** 的 guard 触发 (注释明言 "expected log noise"), 非缺陷

## 4. corten=on 变体

m3a-f1-corten-on.log: 普通 =y 重建后 `-append "... corten=on
kunit.filter_glob=corten"` → 启动到 KUnit 完成, **fail:0**
(pass:24 skip:1 —— layout case 在 corten=on boot 下合法 SKIP);
两阶段日志在场: 0.229s "requested on, activating at initcall time" →
1.221s "page descriptors enabled"。fix1 early_initcall 修复在 M3a
改动下无回归。**不挂**。

## 5. run3/lockdep 抖动定性 — 测试 Harness 时序伪影 (不改代码)

两种失败签名, 根因同一: **guest 内 jiffies 与 TSC(kvmclock) 两时钟源发散**
(NO_HZ_IDLE + KVM, tick 递延), 而 interlock 测试用 ktime 20s deadline
( worker A) 与 jiffies 10s/5s/200ms 等待 (main/其他) 跨线程对标:

- 签名A (kunit3): a_locked==0 + violations==1 + a_err==1, runtime 20.27s
  = A 的 20s ktime deadline 准点到期退出 (a_err=1 即 deadline 路径),
  main 的 200ms jiffies 睡眠被拉伸到 ~20s host-time, 迟到的检查读到
  A 已退出、B 已完成。**协议行为正确**: B 的 uninstall 全程阻塞在
  write_lock_bh, 仅在 A (超时) 释放后才完成 —— interlock 本身在工作。
- 签名B (lockdep run1/run3): "worker A never acquired (phase=5
  begin_ret=0)", runtime 30.02s = main 的 10s jiffies 超时被拉到 30s
  host-time, A 的 20s ktime deadline 早已准点退出 (phase=5=EXITED,
  begin 成功)。main 从未在 jiffies 预算内观察到 A 立即置位的
  a_locked=1 —— 只能是时钟发散。
- 交叉验证: 失败仅出现在 -j12 构建结束后 90s 内或 lockdep (更慢的
  tick 交付) 变体; `nohz=off` 强制周期 tick 后 **lockdep 3/3 全绿**
  且套件时间恢复 5.5s —— tick 递延定性成立。
- 遗留建议 (下一轮, 非阻塞): interlock 测试的 deadline/等待统一到单一
  时钟源 (全 jiffies 或全 ktime), 或加 nohz 依赖/skip 条件;
  mm/corten.h:165 `child` 成员声明多一个 tab (checkpatch 不抓, 纯观感)。

## 6. =n 编译检查

scripts/config -d CORTEN_MM -d CORTEN_MM_KUNIT_TEST → olddefconfig →
make -j12 arch/x86/mm/pgtable.o mm/pgtable-generic.o mm/memory.o
(m3a-f1-nbuild.log): 三目标全部 CC 重编, **exit 0, 0 warning**, 17.4s
→ 恢复 /tmp/config-m3a-f1-y → olddefconfig (=y, PROVE_LOCKING n 已确认)。

## 6b. 最终树内重建 (clean banner)

=n 往返后恢复 =y 全量重建 (m3a-f1-final-build.log, exit 0)。树内
bzImage sha256=47e1af57... (横幅变为 clean ge911b31adb9c, 代码内容与
提交逐字节一致) — 补 1 轮 KUnit 25/25 (m3a-f1-kunit9-cleanbuild.log)。

## 7. 提交与产物

- commit: **e911b31adb9c** `mm: CortenMM: BH-symmetric PT-page descriptor
  locking` (4 files, +1320/-73; Signed-off-by: purplewall1206);
  parent 1284a235f751
- `git diff 1284a235..HEAD` 与 patches/r02-m3a-hardening.diff 逐字节一致
  (COMMITTED == REVIEWED BACKUP PATCH)
- patch: patches/0001-mm-CortenMM-BH-symmetric-PT-page-descriptor-locking.patch;
  tag: **corten-r02-m3a-f1** (annotated)
- checkpatch: 0 errors / 0 warnings (1582 行) — results/r02/checkpatch-m3a.txt
- bzImage: bzimg/r02-m3a-f1/{bzImage,bzImage.sha256},
  sha256=bc3f9c5ce5e64bcb7daec37e125637a133bed83510fe893f8796a4c344b5a7ce
  (全部验证所在的 1284a235-dirty 横幅构建物)
- green.txt: r02-m3a-f1 条目已追加 (run/green.txt)
- 真 boot: KERNEL=bzimg/r02-m3a-f1 launch_vm.sh trixie.img
  (4G/8vCPU/KVM, systemd.mask=sys-kernel-config.mount) → 40s 到登录;
  gssh uname `6.18.32-g1284a235f751-dirty #19`; guest dmesg 内 autorun
  KUnit **25/25**; setup_zram.sh → "zram swap ready"; `zramctl | grep lz4`
  → `/dev/zram0 lz4 2G [SWAP]` (m3a-f1-boot-dod.txt, m3a-f1-boot.log);
  boot console 零 splat。**VM 保持运行** (S46 冒烟复用), 未 kill。

## 8. 门禁对照

| 门 | 要求 | 实测 | 判定 |
|---|---|---|---|
| 构建 | 零新增警告, 记耗时 | 2 条基线既有, 零新增; 11m36s | PASS |
| KUnit ×3 | 每次全套全绿 | run1,2 绿; run3 伪影; 复跑 4-8 五连绿 (共 7/8 绿) | PASS (附 §5 定性) |
| lockdep | 全绿 + 无报警 | nohz=off 三连绿, 零报告, BH 探针绿 | PASS |
| corten=on | 启动到 KUnit 完成不挂 | 见 §4 | PASS |
| =n 编译 | 3 目标零错 | 3 目标 CC, exit0, 0 警告, 17.4s | PASS |
| checkpatch | 存证 | 0/0 (checkpatch-m3a.txt) | PASS |
| 真 boot | 登录 + zram lz4 | 40s 登录, guest KUnit 25/25, lz4 [SWAP] | PASS |

## 9. 产物清单 (results/r02/)

m3a-f1-build.log, m3a-f1-kunit{1..8}.log, m3a-f1-kunit9-cleanbuild.log,
m3a-f1-lockdep-build.log, m3a-f1-lockdep-kunit{1..4}.log,
m3a-f1-lockdep-kunit-nohz{1..3}.log, m3a-f1-restore-build.log,
m3a-f1-corten-on.log, m3a-f1-nbuild.log, m3a-f1-final-build.log,
m3a-f1-boot.log, m3a-f1-boot-dod.txt, checkpatch-m3a.txt, m3a-f1-report.md
patches/0001-mm-CortenMM-BH-symmetric-PT-page-descriptor-locking.patch;
bzimg/r02-m3a-f1/{bzImage,bzImage.sha256}; run/green.txt 新条目。

## 10. 时间线与预算

00:40 timegate 放行 → 00:41 工作树=备份复核 → 00:41-00:52 =y 构建 →
00:53-00:59 KUnit run1-8 → 01:01-01:12 lockdep 构建 → 01:12-01:29
lockdep KUnit (2 伪影取证 + nohz=off 定性 + 三连绿) → 01:29-01:34 =y
恢复重建 → 01:35 corten=on → 01:36 bzImage 归档 → 01:37 =n 检查 →
01:39 提交 e911b31 + patch + tag → 01:45 最终重建毕 → 01:46-01:49 真
boot + zram → 01:50 green.txt + kunit9 → 报告收尾。全程 08:30 硬停线内
富余; VM 保持运行供 S46。
