# M5.T1a 忠实 fork — 终态补跑报告（r06/m5t1a-final，maintainer 收口班）

- 班次: 2026-09-19 02:0x–02:5x CST（通宵, D14+夜窗, 每步 timegate 放行）
- worktree: /home/ppw/linux-6.18-m5t1a, 分支 m5-t1a, 基线 0719bc6ae74e
- **终态核实（补跑前置门）**: `git status` 7 文件 modified 无其它改动;
  `git diff --stat` = **+2003/-439**（与前班 §5a-2 勘误后的终态口径一致）;
  `mm/corten_arena.c` mtime = 09-18 23:03:32 +0800 = 23:03 folio 所有权修复,
  **其后零修改** → 工作树即终态。
- 终态与备份 diff 的差异核实: 现树 vs patches/r06-m5t1a.diff（21:53 旧版）逐行
  diff 仅两组 hunk: ①fork_mirror 游标循环重排; ②**23:03 folio 所有权修复**
  （fault_once 成功 epilogue: 仅 MAP_ANON 把投机引用转移进 PTE; zero-page/restore/
  COW-reuse 引用留在调用方 epilogue folio_put()——原先的一律 ctx->folio=NULL 每
  次此类 fault 泄漏一个预分配页, 即 fork_roundtrip OOM 根因, 修复注释在树）。
  → 补跑就是对该修复闭合验证。

## 1. 补跑矩阵（全部落在终态代码 = 普通配置 bzImage #18, 01:09:37 CST）

构建: 沿用前班 01:09 的 =y 普通配置重建 #18（.config 与 config-y-plain 快照
逐项比对一致; 补跑前未再触碰源码 → 无需重建, `bzImage` mtime/sha256 见 §3）。

### 1a. KUnit（无盘 qemu, -m 4096 -smp 8, timeout 600 未触, 实测 ~20s/轮）
| 臂 | corten | corten_arena | corten_fault | splat | 日志 |
|---|---|---|---|---|---|
| **on**（corten=on） | 24 pass/0 fail/1 skip | **27/0/0** | **24/0/0** | 0 | kunit-on1-final18.log |
| **off**（参数缺省） | 25/0/0 | 18/0/**9 skip** | 4/0/**20 skip** | 0 | kunit-off1-final18.log |

两轮零 `not ok`、零 BUG:/lockdep:/possible deadlock; skip=off 臂设计性, 与
gupfix/rebase 基线口径完全一致。23:09 的 kunit-on1-final.log（kernel #16, 1 例
interlock 时序 flake）由本终态 on 轮取代为权威记录。

### 1b. guest（m5t1a-vm 重启进 #18, trixie-m5t1a.img, corten=on, 8 vCPU/4G KVM）
前班 #16（lockdep 变体）guest 于 02:05 下线, 全部终态判据在新 boot 上重出;
原始日志归档 **results/r06/m5t1a/guest-final/**（回填前班 §3c 缺档）。

| 判据 | 结果 | 日志 |
|---|---|---|
| **fork_isolation 双臂** | **PASS 双臂 rc=0**: corten 与 legacy 均 before=after=**e1c81840e4123903**（=前班同值, 跨臂等价）, child rewrote-own-copy | iso-corten-final18.log / iso-legacy-final18.log |
| **metis_eq MODE 全量**（8 线程, 1.6GB text1600, STRICT hook） | **rc=0**, checksum **8a8db99075665220** = 前班 on 臂与 legacy 臂同值（跨臂一致）; **fork-probe OK**（threads+arenas alive, child exit 42）→ **OQ-D 闭环落在终态代码** | metis-full-final18.log |
| **fork_roundtrip 1000 轮**（1k 页/轮: fork→子校验→子写→exit→父校验+父重触 COW） | **PASS 1000 轮 rc=0**, checksum **076534f9ba241483** 稳定（=前班同值）; 逐 100 轮 MemFree 2.07–2.08 GB 平稳 → **23:03 COW-reuse 泄漏修法闭合证明**（上轮 OOM 正死于此路径） | roundtrip-1000-final18.log |
| **JThreadBench 2000 线程 ×3 reps ×1 JVM**（MODE+STRICT hook） | **rc=0**, counter=2000×3, **零 ClassFormatError**, MODE on + **fork-probe OK**; median 16755ms（注: 本 boot 与 syzkaller M7 预热同宿, metis_eq 同受载 70.8s vs 前班 28.4s——时延非本轮判据, 回归判据=rc/counter/CFE 全过） | jtbc-final18.log |
| **run_mode_smoke** | **26/26 PASS**（0 FAIL）+ SMOKE PASS + [smoke] SMOKE-DRIVER PASS, rc=0 | smoke-final18.log |

对账（同 boot 累计）: `fork_faithful` 0→**1005**（roundtrip 1000 + metis/jtbc
fork-probe 各 1 + smoke/fork_isolation 若干, 全部有 arena 的 fork 计数闭合）;
`fork_demotes=0`（demote 已死）/ `fork_skips=0` / `drain_timeout=0` /
arenas ledger 回 0。（final-baseline.txt / r2-final-final18.txt）

dmesg: 零 BUG/零 Oops/零 panic; 仅 10 条 WARNING = find_vma_intersection
rwsem assert（rwsem.h:83, 全部 PID=JThreadBench java 类加载线程）= 主树既有
残留（rogue-fix.md §6-2 登记, tier-2 RCU walk 噪声, 功能无害, T1a diff 未触碰
该函数）。boot-console-final18.log 全程在档。

### 1c. 与前班报告的差异说明
1. 前班 §3a/§3c 证据产自 lockdep 变体内核（#16/#17, 前班 §5a 勘误1 自曝）;
   本轮全部判据复出在**普通配置 #18**（=y 全量、无 PROVE_LOCKING 家族）——
   即合入所需"普通 =y 内核上的绿证据链"由本轮补齐; lockdep 臂证据（零 splat）
   仍由前班 §3d/§5b 有效在档, 两套互补。
2. 23:03 folio 修复在前班报告定稿后落码, 其后仅有 #17 lockdep KUnit 一轮 +
   guest roundtrip（#16 lockdep 内核）; 本轮 = 普通内核上的首也是终验证,
   **五项判据 + KUnit on/off 全绿, 修复判定获终态代码证据支持**。
3. 校验和三方对齐: fork_isolation e1c81840e4123903、metis_eq 8a8db99075665220、
   roundtrip 076534f9ba241483 均与前班值逐位相同 → 终态代码与前班验证代码
   在判据输出上无行为漂移（23:03 修复只影响页引用所有权, 不影响内容语义）。
4. JThreadBench/metis_eq 时延受同宿 syzkaller 预热负载拉长（如实记录, 判据不涉时延;
   正式性能数按 EVAL 归 T4/T5 矩阵）。

## 2. 判定
**终态补跑 = 全绿 → 23:03 folio 修复闭环, PASS-with-conditions 的补跑条件满足,
M5.T1a 具备合入条件**（maintainer 提交 + 登记; F2/F3/F4 三条件转 T1b/T2 跟踪）。

## 3. 工件清单
- patches/r06-m5t1a.diff — 终态重导出（2837 行, +2003/-439, 含 23:03 修复;
  覆盖 21:53 旧版）
- guest-final/ — §1b 全部原始日志 + boot-console-final18.log（父目录）
- kunit-{on1,off1}-final18.log — §1a
- 本报告: results/r06/m5t1a-final-verify.md
- 终态 bzImage = worktree arch/x86/boot/bzImage（14,771,200 B, #18,
  sha256 2985384577e6bb7b...）→ 收口时归档 bzimg/r07-m5t1a
