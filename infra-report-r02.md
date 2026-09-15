# M9 infra 报告 — arm64 基线 + corten 移植失败点实证 + syzkaller (r02 night)

日期: 2026-09-14 CST；本段执行窗口 07:33–08:15（timegate 07:34:07 放行，免费窗口）。
约束: 08:30 硬停 / 08:25 收工。
更新: 2026-09-15 00:15–00:31 续传窗口完成 Round A（timegate 00:15:41 放行）——§1 判定已翻绿为 PASS。

环境: wt /home/ppw/linux-6.18-m9, 分支 m9-arm64, HEAD=e911b31adb9c (M3a.F1)。
口径说明: 中断前记录的 "HEAD=1284a235" 为本支父提交 1284a235f751（已验证是 e911b31 的祖先），同支一致，无漂移。
配置: arm64 defconfig + MGLRU/zram/lz4（见 results/r02/m9-baseline-config.log；末行
`CONFIG_CORTEN_MM= <absent>`，符合预期——mm/Kconfig:1432 `depends on MMU && X86_64` 在 arm64 上裁掉）。
工具链: /home/ppw/tools/aarch64-toolchain/bin/aarch64-linux-；make -j6。

## 1. Round A — M9 arm64 基线构建（增量续传）

**判定: PASS（2026-09-15 00:24:45 CST `arch/arm64/boot/Image` 产出，make 退出码 0，全日志 0E/0W）。**

- 产物: `arch/arm64/boot/Image` — 42,031,616 B（≈40.1 MiB），sha256
  `24f8f716726556d9dfda886f2636cb3fd0b73a075fc4c7c8ac26dc361d8f6204`，产出时刻 2026-09-15 00:24:45 CST。
- 中断续传说明: 前段会话在 3054 行处被杀；07:34:07 窗口 `make -j6` tee -a 增量续传，无缝衔接；
  09-15 00:15:41 timegate 放行后再次幂等续传（`nohup make ARCH=arm64 CROSS_COMPILE=... -j6 >>` 同一日志），至完成无重编冲突。
- 进度轨迹: 07:36→3118 行 / 07:55→6093 / 08:04→7301 / 08:13→8459 行（drivers 尾段），硬停回收时 11241 行
  （mtime 实证: 会话1 末个 .o `drivers/clocksource/mmio.o` @09-14 08:34:06）；09-15 续传首个 .o @00:17:56 →
  vmlinux @00:24:44 → Image @00:24:45 → 末个 mod.o（net/qrtr）@00:30:21，make exit 0，日志终 14921 行。
- 警告数: **0**（`warning:` 全日志计 0）；错误数: **0**（`error:`/`Error N` 计 0）。日志: results/r02/m9-baseline-build.log。
- 结论: arm64 defconfig 基线 Image 构建完成（0E/0W），M9 gate（M4.T4）所需基线 Image 就绪；
  曾记录的"时间窗内未完成链接"仅为窗口体量问题，无任何配置/代码错误。

## 2. Round B — corten 移植失败点实证（ARM64_PORTING.md P1 章回填）

**判定: 完成。核心 `mm/corten.o` 在 arm64 零错误编译通过；全部真实移植阻塞点（2 个错误）集中在
`mm/corten_test.o` 的 x86 页表位宏依赖。**

### 执行方式偏差（如实记录）
任务原定在 m9 worktree 内 `checkout -b` + sed Kconfig。因 Round A 的 make 正在同一树运行，
就地改 mm/Kconfig 会触发 syncconfig 污染在跑构建，故改用一次性 throwaway worktree:
`git worktree add /home/ppw/linux-6.18-m9-trial -b m9-arm64-corten-trial`（=e911b31，零提交）。
其余步骤与任务一致: sed 前 `depends on MMU && X86_64` 位于 **mm/Kconfig:1432**
（备份: results/r02/m9-trial-Kconfig.bak）→ sed 为 `depends on MMU` → 克隆 m9 .config 并追加
`CONFIG_CORTEN_MM=y` → `olddefconfig` → `make -k ARCH=arm64 CROSS_COMPILE=... mm/corten.o mm/corten_test.o`。
清理已完成: worktree remove --force + `branch -D m9-arm64-corten-trial`（was e911b31），m9 树未受任何扰动。

### 失败清单归类表（ARM64_PORTING.md P1 实证回填）

| 类别 | 错误 | 位置 | 定性 |
|---|---|---|---|
| x86 头依赖（页表位宏） | `_PAGE_PSE` undeclared | mm/corten_test.c:1910（同款用法 :1916 `set_pud(pudp, __pud(_PAGE_PSE \| _PAGE_PRESENT))`） | **真实阻塞**。x86 pgtable_types.h 位宏；arm64 无此符号（已 grep arch/arm64/include/asm/pgtable*.h 确认为空）。修复方向: 大页 PMD/PUD 构造抽象为架构中立 helper 或按 CONFIG_X86_64 门控 |
| x86 头依赖（页表位宏） | `_PAGE_PRESENT` undeclared | mm/corten_test.c:1910 | 同上（同一行成对出现） |
| 层级假设 | — | — | **本切片未发现真实层级类错误**（核心事务/锁/MM 代码无 arch 假设泄漏） |
| 门控伪影（方法论记录，非移植失败） | `corten_test_inject_alloc_fail` / `corten_test_render_dbg` / `CORTEN_DBG_STATS/TXN/DUMP` implicit-declaration（第一轮 5 个错误） | mm/corten_test.c:1154,2024,2034,2043 | **非真实错误**: 这些声明在 mm/corten.h:211–227 `#ifdef CONFIG_CORTEN_MM_KUNIT_TEST` 内；该符号 `depends on CORTEN_MM && KUNIT`、`default KUNIT_ALL_TESTS`，defconfig 无 KUNIT → 符号被裁。单目标 `make mm/corten_test.o` 绕过 obj-$(…) 门控强编译才触发。补 `CONFIG_KUNIT=y`+`CONFIG_CORTEN_MM_KUNIT_TEST=y` 重跑（第二轮）即消失。P1 章勿将其计入移植工作量 |

两轮证据: results/r02/m9-trial-build.log（第一轮 7 错 + `=== ROUND2 08:12:39 KUNIT=y rerun ===` 分隔 + 第二轮 2 错）、m9-trial-config.log。

**P1 结论一句话: 移植面 = 测试文件里 2 个 x86 页表位宏；核心 corten.o（115,304 字节对象，CC 行 08:09）
arm64 干净通过——与 "gate 在 M4.T4、今晚仅实证" 的预期一致，移植成本低。**

### 附注
- mm/Makefile:156–157 `obj-$(CONFIG_CORTEN_MM) += corten.o` / `obj-$(CONFIG_CORTEN_MM_KUNIT_TEST) += corten_test.o`。
- x86 主树对照: /home/ppw/linux-6.18/.config:1135–1136 `CONFIG_CORTEN_MM=y`、`CONFIG_CORTEN_MM_KUNIT_TEST=y`。
- `syz-manager version` 子命令无效（FATAL no config，属正常 CLI 行为），版本以下述构建日志 GIT_REVISION 为准。

## 3. syzkaller 构建核验

**判定: PASS。**

- bin/: `syz-manager`(70,979,746B), `syz-mutate`, `syz-prog2c`, `syz-repro`, `syz-db`, `syz-sysgen`, `syz-upgrade` — 7 个，
  另 `bin/linux_amd64/`: `syz-execprog`, `syz-executor` — 2 个，共 **9 个可执行**（bin/ 顶层 7 + GOOS 目录 2；均 9月14 01:54–01:59 产出）。
- 版本: GIT_REVISION=70a60e12a1aa155e837bf0ad723487d72b1d2916（executor 链接命令行内嵌，见构建日志）。
- 构建: results/r02/syzkaller-build.log（23 行）以 `check-syzos.sh` + `make: Leaving directory '/home/ppw/tools/syzkaller'` 正常收尾；
  仅 1 条良性 ld 警告（static-pie 下 gethostbyname/glibc 提示）。syzkaller-build.rc 为空文件，无 rc 记录，判定以日志为准。

## 4. 总耗时与合规

- 本次收尾段: 07:33–08:15 ≈ 42 min（Round A 续传 07:34 起至收工仍在跑；Round B 实际 ~7 min，含两轮）。
- Round A 累计（mtime 推算, make -j6 wall）: 会话1 09-14 01:59:15（kernel.release）→08:34:06（末个 .o，硬停回收）
  ≈ **6h35m**；09-15 续传段 00:15:55（make 启动）→00:30:22（exit 0）≈ **14.5 min**；合计 ≈ **6h50m** wall，
  中间空窗 15h42m（08:34→次日 00:15），续传段零重编（make 幂等）。
- 合规: 未 push；未触碰主树与 m3b*/m3b46 worktree（worktree list 复核: linux-6.18 / m3b / m3b46 / m9 原状）；
  timegate 放行（07:34:07 / 09-15 00:15:41 均免费窗口内）；无密码落盘。
- 遗留: 无——Round A 已完成（PASS）；Image 出来后 M9 Round A 已翻绿，无需重做。
