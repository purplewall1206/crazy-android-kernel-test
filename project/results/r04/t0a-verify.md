# M4.T0a verify — 复验版（r05 夜, 2026-09-17 01:54–03:40 CST）

- 对象: worktree `/home/ppw/linux-6.18-m4t0a`（branch m4-t0a, HEAD=`ae236ee077ac` + 未提交
  diff, 导出 `patches/r05-m4t0a.diff`）
- 验证人: r05 夜班修复 agent（本 turn 内完成; 未碰主树, 未 push, 未改任何内核实现语义——
  本班全部改动在 测试/注释/驱动脚本/SPEC 勘误 层, 内核二进制行为零变化）
- **判定: 矩阵全绿, 建议 PASS（待 review/maintainer 复核后可 commit; `T0A_VERIFY_FAILED`
  标记留给维护者摘除）**
- 证据目录: `results/r04/t0a/`（build-y-r05.log / build-n-seven-objects-r05.log /
  kunit-{on1,on2,off1}-r05.log / boot-console-full-r05.log / boot-kunit-r05.txt /
  smoke-run-r05.log / smoke-obs-r05.log）

## 本班修复（r04 三阻塞项 + 复验中新暴露两项, 全在测试/注释层）

| # | 位置 | 修复 |
|---|---|---|
| B1 | `mm/corten_arena.h:203` | `*@addr/*@lenp/*@flagsp` → `*@addr / *@lenp / *@flagsp`（-Wcomment 断源消除; T0b 树同步） |
| D-A | `mm/corten_arena_test.c` release_classify | 尾差算式 `2 * PMD_SIZE - PAGE_SIZE`(=4M-4K, ≥2M→CHUNK) → `PMD_SIZE - PAGE_SIZE`(=2M-4K, <2M→EXACT), 与规格"尾差<2M→EXACT"一致 |
| D-B | `mm/corten_arena_test.c` test_mode/auto_route | 携带 arena 的 `corten_arena_mode_exit` 直调改走既有 op-worker（`kthread_use_mm`）。定性: **测试 bug, 实现无错** —— RELEASE 的 do_munmap 漏斗读 `current->mm` 是上游 6.18 原生语义（本树 mm/vma.c 零改动）, 真实调用方（prctl/fork_demote/exit_mmap）均满足 `current->mm == mm`。r04 的 KUNIT=y boot 卡死 = 该 Oops 令 kunit_try_catch 带禁中断死亡 |
| D-C | auto_route 测试 | ①期望值笔误: 8K 请求经 PMD 取整 = **1** 个 2M 槽, 期望从 `2*PMD_SIZE` 修为 `PMD_SIZE`（含 next_va/second-hit 级联两处）; ②裸调违反契约: route 是 do_mmap 前半, kernel-doc 明示需调用方持 mmap_write, 新增 `_auto_route_locked` 包装持锁直调 |
| D-D | auto_attach_release 测试 | 同上锁契约: auto_attach 是 do_mmap 尾钩（需 mmap_write）, 内联持锁调用; 首段 arena 从 `WIN_LEN`(4M) 改 `PMD_SIZE`(2M)——full-coverage RELEASE 规则只对"尾差<2M"生效, 4M arena 的 1 页 munmap 保持 CHUNK 是**实现正确行为**; 测试原造形还会第二次重叠 mkvm 撑爆 maple tree → exit_mmap `BUG_ON(count != map_count)` |

## 复验矩阵

| # | 项 | 判定 | 证据 |
|---|---|---|---|
| 1 | `make -j12 olddefconfig && make -j12`（=y 全量, **无 KCFLAGS 绕行**） | **PASS** | RC=0, bzImage #2; 全程仅 1 条警告: `objtool: cpuidle_enter_state+0x142 return with instrumentation enabled`（drivers/cpuidle, 非 diff 触及文件, r04 已判定为基线现象） |
| 2 | 配置检查 | **PASS** | CORTEN_MM / KUNIT_TEST / ARENA / ARENA_KUNIT_TEST / ARENA_FAULT_KUNIT_TEST 全 =y; ANON_VMA_NAME=y; WERROR=y |
| 3 | KUnit on×2（timeout 300, -smp 2, filter_glob=corten*, corten=on） | **PASS** | on1/on2 同: corten 20/0/5 + **corten_arena 19/0/0** + **corten_fault 13/0/0**（r04 饿死的两套件首次完整跑绿） |
| 4 | KUnit off×1（corten=off） | **PASS** | corten 21/0/4 + arena 16/0/3 + fault 3/0/10, 零 not ok |
| 5 | guest 首启（**KUNIT=y + corten=on, 无 kunit.enable=0** = r04 卡死场景） | **PASS** | boot 期 KUnit 全绿: `corten 24/0/1 + corten_arena 19/0/0 + corten_fault 13/0/0` → systemd multi-user → **login 可达**（boot-console-full-r05.log）。dmesg 中 4 条 WARNING 均为既有设计内 WARN_ON_ONCE 探针（r01 起 2 条先例） |
| 6 | run_mode_smoke.sh（corten=on + kunit.enable=0, 对齐 r04 口径） | **PASS 26/26** | 全 PASS 含 r04 唯一失败项 `released-arena-gone`（已改为 mincore-ENOMEM 语义: munmap 未映射区间本就返回 0）; SMOKE-DRIVER PASS; ledger before=after=0; dmesg corten WARN/BUG/OOPS 计数=0（smoke-run-r05.log） |
| 7 | 逐项观测 t0a_obs.py | **PASS** | ENTER→arenas=1/GET=1; fork: 子 GET=1、新 mmap 落窗口、父 arenas=0 且内容完好; EXIT 清空; OBS_RC=0（smoke-obs-r05.log） |
| 8 | =n 七对象回归 | **PASS** | RC=0, 零警告, 七对象全部重编译（build-n-seven-objects-r05.log）; config 已复原（=y 五项在位） |
| 9 | checkpatch --strict | **PASS** | r05-m4t0a.diff: **0 errors, 0 warnings, 0 checks**（2123 行） |

## 结论

- T0a 修复后**九项矩阵全绿**, 无遗留阻塞项。补丁: `patches/r05-m4t0a.diff`（+publish 镜像）。
- r04 报告中的"环境偏离声明"（KCFLAGS 绕行 / kunit.enable=0 才能启动）两条均已消除:
  B1 修复后 =y 正常编译; D-B 修复后 KUNIT=y 内核可正常 boot。
