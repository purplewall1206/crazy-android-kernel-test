# PR-0 =on 默认进场回归 —— commit 定罪与 config 依赖诊断（2026-10-07 凌晨, 主会话）

## 0. 判定一句话

**PR-0（2d3febc401e1, bss implant adoption）在非 lockdep 生产形 config 上破坏 =on
默认进场世界：任何 init 在 exec 后首个用户写即 SIGSEGV → kernel panic**
（exitcode=0xb, "Run /sbin/init" 后 ~30ms）。全部既有 =on verdict（MV3.d 三腿
7216s 零 panic 在内）均为 **mva lockdep 形 config** 的结论——生产形 config 的
=on 世界在本发现之前从未被验证过。

## 1. 定罪矩阵（bzImage 级 boot 实验, 2026-10-07 00:00-04:50）

| 构建 | commit | config | 镜像 | =on 结果 |
|---|---|---|---|---|
| bzImage-w3fix5 | 47cc6cfb (W-3fix5, PR-0 前) | mva (lockdep) | w6v2 | **OK**（login prompt） |
| bzImage-w3fix6-y | 6fd01509 (W-3fix6, 含 PR-0) | mva (lockdep) | w6v2 | OK ×2（全新 overlay 首 boot 均净） |
| S2 自建 | dd3300f0f267 | mva (lockdep) | w6v2 | **崩**（S2 报告, console-head-defentry-crash） |
| 主树自建 #100 | dd3300f + madv/flake | **主树（非 lockdep）** | w6v2 | **崩**（5.84s, regress3） |
| fb01d99bc16b | dd3300f 纯净（无 S1/S2） | 主树（非 lockdep） | w6v2 | **崩**（5.86s） |
| fb01d99bc16b | **2d3febc4 单点（PR-0）** | 主树（非 lockdep） | w6v2 | **崩**（5.93s, "Run /sbin/init" 后 30ms） |
| fb01d99bc16b | 2d3febc4, `init=/bin/bash`（动态链接, 非 static-PIE） | 主树 | w6v2 | **崩**——非 static-PIE 特异 |
| fb01d99bc16b | 2d3febc4 | 主树 | **mv3d.img** | **崩**——非镜像特异 |
| 47cc6cfb 自建 | 47cc6cfb (W-3fix5) | 主树（非 lockdep） | w6v2 | **OK**（login prompt） |

结论: 崩溃变量 = **PR-0 的有无 × config 的 lockdep 族有无**; 镜像与 init 二进制
形态无关。W-3fix5（PR-0 前）在生产 config 上干净。

## 2. 机制画像（现场事实, 待根修轮确认）

- 崩溃 = init 用户态 SIGSEGV（无内核 Oops/WARN）; dmesg 静默至 panic。
- 时间形状: "Run /sbin/init as init process" 后 ~30ms —— exec 准入完成后
  首个用户写（.data/.bss/GOT 候选）未被服务。
- 候选面（按 PR-0 diff 的触点）: vm_brk_flags bss 腿 → corten_bss_declare_route
  adoption 臂（"early return, no populate"）与 exec 镜像 FILE record 的 W-7
  同帧共存（commit message 自述 "W-7 same-frame bucket eats and exec-image
  FILE record boundary frame overlap"）; fault 路径对收编 bss 的 fill/MAPERR
  判定。
- **lockdep 掩蔽机制**: mva config 与主树 config 全量 diff = 28 行, 全部为
  lockdep/PROVE/DEBUG_ATOMIC_SLEEP/TRACE_IRQFLAGS/DEBUG_*{MUTEX,SPINLOCK,RWSEMS}
  族——语义无关、时序巨变。竞态窗在慢速构建下不触发。
- 佐证: S2 片（flake-pie）独立发现同一崩形（其 head-crash 为 mva config 自建
  ——非 lockdep 变体? 待其工件复核; 主会话的 w3fix6-y 两次"干净"首 boot 曾误导
  假阴性方向, 系 lockdep 构建掩蔽）。

## 3. 处置（2026-10-07 主会话裁决）

- **登记发运, 不凌晨抢修**: W-7 同帧机械的抢修错一把即损 J1/J2 头牌不变量;
  本诊断链已把修复面收窄到 PR-0 diff 触点（bss adoption × exec FILE record
  同帧 × fault fill 判定）, 交下一轮满预算根修。
- 优先级: **P1 首项**（=on 生产 config 世界不可启动; prctl 进场世界与 =off
  世界不受影响——两者在本诊断中全绿）。
- 验收门（根修轮）: 生产 config =on 全系统 boot ×3 + 三腿电池复跑 + 本文件
  定罪矩阵全表转绿。

## 4. 证据

- results/r07/pr0-regression/（6 份 console 原件: 定罪矩阵 6 行的现场）
- 相关: flake-pie-dev-report.md（S2 独立发现 + static-PIE 探针红绿闭环）、
  mv3e-dev-report.md §1.2 C 组（PR-0 靶面原文）
