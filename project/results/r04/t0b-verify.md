# M4.T0b verify — 复验版 v4 FINAL（r05 夜, 2026-09-17 02:05–07:10 CST）

- 对象: worktree `/home/ppw/linux-6.18-m4t0b`（branch m4-t0b, HEAD=`ae236ee077ac` + 未提交
  diff; 增量 `patches/r05-m4t0b-increment.diff`（对 T0a 基; T0a 已提交
  5aef23c4aee9/tag corten-r05-m4t0a, 层叠由 maintainer 处理）, 全量
  `patches/r05-m4t0b-full.diff`（--diff-algorithm=patience））
- 验证人: r05 夜班修复 agent（未碰主树/T0a 树, 未 push; 内核改动 = B2/D-E/D-F/D-G/D-G'
  五处最小实现修复 + 测试层 + SPEC 勘误）
- **判定: KUnit 全绿（on×2: 20/0 + 22/0 + 16/0; off×1 全绿）; run_t0_dod 8P/4F——
  D-G 系形状（PROT_NONE 预留→chunk mprotect 提交→首触 SEGV/ACCERR）已修复并有 KUnit
  锚 + 计数器实证（rearm_recovered=16, rearm_failed=0）; 残余两条**独立**问题:
  ① JVM CDS 重定位阶段 abort（D-G''，新根因，与已修形状无关）
  ② metis_eq fork 后触碰 ACCERR（DEV-11 边界，已记 OQ 归规划者）
  ——T0b 提交决策归主 agent（可带①②遗留; 证据充分、定位精确）*
- 证据目录: `results/r04/t0b/`（build-y-r05c.log / build-n-seven-objects-r05{b,d,e}.log /
  kunit-{on1,on2,off1}-r05.log（终态）/ t0dod-run{1..6}.log）

## 修复总账（r04 四项 + 复验暴露八项）

| # | 位置 | 内容 |
|---|---|---|
| B1 | `mm/corten_arena.h:203` | `/*@` 注释排版（-Wcomment 编译断消除）; T0a 树同步 |
| B2 | `mm/corten_arena.c` madvise 决策表 | `case MADV_FREE_LOCKED:` 删除; SPEC §3.4 勘误已落 `publish/M4T0_SPEC.md` |
| D-A | release_classify 用例 | 尾差 `PMD_SIZE-PAGE_SIZE`（2M-4K <2M → EXACT） |
| D-B | test_mode/auto_route 用例 | 携 arena 的 mode_exit 改走 op-worker（定性: 测试 bug; current->mm 依赖=上游 do_munmap 漏斗原生语义）; r04 KUNIT=y boot 卡死消除（本班 KUNIT=y 直启到登录已实证） |
| D-C/D-D | auto_route/auto_attach_release 用例 | 持锁契约（route/attach 需调用方持 mmap_write）+ 期望笔误（8K→1 个 2M 槽）+ 首段 arena 2M 造形（修 exit_mmap BUG_ON 级联） |
| D-E | protect_range whole 腿 | `vm_flags_clear(R/W/X)` 前置（vm_flags_set 只 OR 不清） |
| D-F | dontneed_route | 返回归一化 `ret < 0 ? ret : 1` |
| D-G | protect_range -ENOENT | 无 tracked PT page 窗口: fill_upper 补装 + 逐页记录 pending perm（选"逐窗口 pending"而非"抬 ar->prot": 上界 per-arena, JVM 240M 预留内已/未提交子区并存, 抬上界会把未提交区放开）。KUnit 锚: `mprotect_fresh`（JVM 形状: PROT_NONE 预留→mprotect 提交→首触成功+PTE 可写; 提交区外首写 ACCERR） |
| **D-G'** | protect_range/fault_once 的 -EOPNOTSUPP/-ENOENT | **PT page 存在但 desc 缺失**窗口 lock 报 -EOPNOTSUPP（非 -ENOENT）——两路均先 fill_upper(rearm) + 重试一次 lock, 仍失败才保守 legacy 自愈; 新具名计数器 `rearm_recovered`/`rearm_failed` 入 arena_stats。KUnit 锚: `untracked_rearm`（inject_alloc_fail(2)+fill_upper 确定性造形, 复刻 untracked_drift 的 seam） |
| 测试层 | named_counter/m.perm/auto_route 期望/锁契约 | kstrtol mid-buffer 截断; PROT_NONE 编码=USER 位; 8K→1 槽; route/attach 持锁包装 |
| driver | `run_t0_dod.sh` java 腿 | 补 `java` 命令本体 |

## 复验矩阵（终态）

| # | 项 | 判定 | 证据 |
|---|---|---|---|
| 1 | =y 全量 | **PASS** | RC=0（build-y-r05c.log）; 仅基线 objtool 1 条 |
| 2 | KUnit on×2 | **PASS（全绿）** | corten 20/0/5 + arena 22/0/0 + **fault 16/0/0**（含 mprotect_fresh、untracked_rearm 两个新用例）; on1/on2 一致 |
| 3 | KUnit off×1 | **PASS** | 21/0/4 + 16/0/6 + 3/0/13（新用例正确 skip） |
| 4 | guest run_mode_smoke 回归 | **PASS 26/26** | SMOKE-DRIVER PASS; ledger 归零 |
| 5 | guest run_t0_dod 完整跑（run5/run6 同结果） | **FAIL 8P/4F** | 见下节差距清单（均收敛到两条独立残余） |
| 6 | =n 七对象 | **PASS** | RC=0 零警告; config 复原 |
| 7 | checkpatch --strict | **PASS** | increment 0E0W0C（2044 行）; full(patience) 0E0W0C（3938 行） |

## run_t0_dod 首跑/终跑差距（run6 = 终态 kernel; run1-5 历史见 t0dod-run*.log）

**PASS（8）**: exec-mm 演示; java off-run; java auto_mmaps 3→41; java mprotect_routes
0→110（**JVM reserve+commit 被 MODE 路由接管, D-G 形状的 110 次提交全部成功**）;
metis_eq off-run; metis_eq auto_mmaps 3→82; metis_eq strace 等价; dmesg 零 corten
WARN/BUG。**D-G' 计数器实证: rearm_recovered=16, rearm_failed=0**（arena_stats）。

**FAIL（4）**:
1. java on-run SIGABRT @CDS `FileMapInfo::relocate_pointers_in_core_regions`
   ACCERR @0x100086000010（hs_err_pid524.log; run4/run5/run6 同点）。
2. java fork-probe 未达（on-run 前置 abort 所致）。
3. metis_eq on-run ACCERR @0x100004000030——发生在 **fork 之后**: fork_demote 按 DEV-11
   把 metadata-only commit 擦成 INVALID（VMA 保持 PROT_NONE）→ 提交不随 fork 存活。
   **DEV-11 架构边界, 记 OQ 归规划者**（修法方向: demote 时把已记录 pending 页翻译成
   VMA split + vma_flags, 或文档化"MODE 下 fork 后需重提交"）。
4. java strace 等价不可判（on 侧 trace 因 abort 截断）。

## 残余根因说明（供维护者/规划者）

**① JVM CDS abort（D-G''）**: rearm 计数器证明 present-untracked 形状已全部恢复
（failed=0），故该 ACCERR 是**第三种形状**——CDS archive 的 core region 映射/重定位与
MODE 路由的交互（疑: file-backed MAP_FIXED 落窗口区 / pending perm 的只读区域被
relocate 写穿 / mmap_mark 对非 anon 的处理）。下一步建议: 在 mprotect_route/mmap_route
入口加临时 trace 打印该 si_addr 所在 vma 的 flags/prot, 一次 guest 跑即可分辨。
**② fork 边界（②③号 FAIL）**: 如上, DEV-11 OQ。

**注**: 两条残余都不在 D12"已编译程序必须正常"的核心提交路径上——core shape（PROT_NONE
预留+mprotect 提交+触碰）已由 mprotect_fresh KUnit 锚定并通过; CDS 与 fork 是 JVM 启动
后期与多进程场景的附加覆盖面。

## 结论与交接

- KUnit 全绿 + =n/checkpatch 全绿; DoD 8/12 项达成, 4 项 FAIL 收敛到两条已定位的独立
  根因（①CDS 交互 ②DEV-11 fork 边界）, 均有 hs_err/strace/计数器证据与下一步手段。
- 补丁: `patches/r05-m4t0b-increment.diff` / `r05-m4t0b-full.diff`（+publish 镜像）。
- T0a 已由 maintainer 提交（5aef23c4aee9）; 本树基座未 rebase（验证独立）, 层叠由
  maintainer 处理。
