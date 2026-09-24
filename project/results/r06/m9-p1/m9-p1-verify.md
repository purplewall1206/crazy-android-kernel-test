# M9-P1 收口判定 (m9-p1-verify.md)

- 班次: r06 m9-p1 收尾班 (2026-09-18 04:48-05:3x CST)
- 树: /home/ppw/linux-6.18-m9, 分支 m9-arm64 @ e911b31adb9c (M3a.F1)
- 工作树补丁 (未 commit, 按 M9-P1 计划): `mm/corten_test.c` (test 位宏中立化)
  + `mm/Kconfig` (CORTEN_MM_KUNIT_TEST depends 落地)

## 1. §11.4-1 两对象双绿闭环 (前班完成, 本班核验)

results/r06/m9-p1/two-objects.log 末段:
- `CC mm/corten.o` + `CC mm/corten_test.o` — **零错误** (全 log 无 error;
  config-grep.log: ARM64_4K_PAGES=y / PAGE_SHIFT=12 / CORTEN_MM=y /
  CORTEN_MM_KUNIT_TEST=y / KUNIT=y)
- **判定: P1 闭环 ✅** (ARM64_PORTING.md §11.4 第 1 条收口)

## 2. arm64 全量 Image (P4 前半, image-build 判定)

- 命令: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux- -j6 Image` (增量续传,
  自 23:32 中断点恢复, 05:03-05:17, 约 14 分钟, RC=0)
- **零错误 / 零新增警告** (image-build.log 4811 行; "error" 命中 3 条均为
  文件名: acpica/uterror.o, acpica/utxferror.o, 9p/error.o)
- 产物: `arch/arm64/boot/Image` 42,232,320 B
  - new:  f9b0e0c6fbe74c3bd2fe638dea5e21e9df9819bd8f28a427ded5cf9233a59add
  - base: 24f8f716726556d9dfda886f2636cb3fd0b73a075fc4c7c8ac26dc361d8f6204
    (Round A 基线, 2026-09-15 00:24, 42,031,616 B)
  - **增量: +200,704 B (~196 KiB)** — 含 corten_test.o KUnit 测试代码链接入
    Image 及 P1 补丁增量; 量级与"一个测试对象 + 宏中立化"相符, 无异常膨胀
- 见 image-sha256.txt

## 3. CortenMM KUnit arm64 首跑 (M9 里程碑证据)

命令: `qemu-system-aarch64 -M virt -cpu max -m 1024 -nographic -kernel
arch/arm64/boot/Image -append "console=ttyAMA0 kunit.enable=1
kunit.filter_glob=corten*" -no-reboot` (TCG, 无 KVM 嵌套; boot→KTAP 完成
wall ~2.5 min, guest 14s)

### 首跑 (单 CPU): kunit-arm64.log
```
# Subtest: corten   (module: corten_test, 1..25)
# corten: pass:21 fail:0 skip:4 total:25
# Totals: pass:21 fail:0 skip:4 total:25
ok 1 corten
```
- 4 skip = `need 3 online CPUs for worker placement`
  (txn_mutex_overlap / txn_mutex_disjoint / txn_hole_race /
  txn_uninstall_interlock) — 与 x86 侧单 CPU 口径一致的设计性跳过

### -smp 4 复跑: kunit-arm64-smp4.log
```
# corten: pass:25 fail:0 skip:0 total:25
# Totals: pass:25 fail:0 skip:0 total:25
ok 1 corten
```
- **25/0/0 全绿** — 上述 4 用例 (ok 14/15/18/19) 真跑全过
- WARNING 恰 2 条 (mm/corten.c:829 与 :776, corten_txn_begin, 调用栈
  corten_test_txn_path_overflow) = **既有设计注入路径**, 与 r03/r05 口径
  完全一致 ("2 条 WARNING=txn_path_overflow KUnit 注入设计路径")
- 零 Oops / 零 BUG / 零 lockdep splat
- 末段 `VFS: Unable to mount root fs` panic = 无根 fs 的预期终止方式
  (KUnit 于 initcall 阶段已完成, -no-reboot 使 qemu 退出), 非缺陷

## 4. 收口判定

| Gate (ARM64_PORTING.md §11.4) | 判据 | 结果 |
|---|---|---|
| 第 1 条 (P1 收尾) | mm/corten.o + mm/corten_test.o 双绿 (KUNIT=y) | ✅ 前班完成, 本班核验 |
| 第 2 条前半 (P4 前半) | CORTEN_MM=y 全量 arm64 Image + qemu -M virt smoke | ✅ Image RC=0 + KUnit 25/0/0 |

**M9-P1 收口: PASS。** corten 套件 (25 用例) 在 arm64 首次全绿运行成立,
这是 CortenMM 继 x86 后第二个架构上的 KUnit 运行证据; P2/P3 (钩子/contpte
语义适配) 未在本切片范围, 仍为 §11.4 后续项。

## 5. 遗留 / 移交

- 未 commit (主 agent 决策; M9-P1 属 M2+M3a.F1 树补丁, 独立小提交可留明日)
- 未 push; 主树 / 其他 worktree 未触碰
- inflight.txt 已追加 r06 m9-p1 段
