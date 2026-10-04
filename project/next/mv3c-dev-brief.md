# MV3.c 开发任务书 · exec 镜像收编 + mmap-pf 批 mark（两轴切片）

授权: 用户指令"MV3 开工，按 D29 路线走" + specs/MV2_REMAINING_SPECS.md §MV3.c +
STATE MV3.a 收口条目（in_execve 降级臂登记=本片轴一靶面）。基座: worktree
/home/ppw/linux-6.18-mva（HEAD 同步主树 9d3b3cb4+）。**两轴顺序执行, 轴间各自
完整验证门+报告分节**。

## 轴一: exec 镜像收编（正确性/完备性）
现状: MV3.a 的 in_execve 降级臂让 exec 镜像映射留 legacy-stock（ET_DYN 解释器
首段 addr==0 非 MAP_FIXED → 路由拒绝 → init ld.so SEGV 那次红）。W-7 multi-record
已拆除 frame-sharing 边界——收编完备性的架构前提已备。
- 门槛形: ET_DYN 解释器首段（addr==0 file mapping）+ ELF 各段（混合 file/anon、
  同帧多段=multi-record 已支持）。
- 路由件: addr==0 file 形 → auto file attach 既有臂核对（W-3 镜像 + V-B 机制
  在树; 缺口=接线不在 exec 语境）; in_execve 降级臂的替换=收编（非 MAP_FIXED
  的 exec 安装也走路由）。file_may 拒绝形状维持 fail-open legacy（登记计数）。
- 判据: =on 裸 boot 后 tree_entries 只剩三桶+brk 豁免（exec 镜像行全部入
  region; wl_file/wl_anon delta 与 ELF 段数对账）; ld.so/ET_DYN 程序正常执行
  （init/裸 smoke/metis 全链行为零回归）。
- 锚: ET_DYN 形（addr==0 file）路由 KUnit 直驱锚; 多段同帧收编锚（W-7 后应
  全收编）。

## 轴二: mmap-pf 批 mark（性能）
D29: fault 路径簿记合并, 目标 -17~-22% 地板收窄到个位数。
- 基线先行: mmap-pf 基准（bench 既有）在当前树跑三遍取中位, 数字入报告。
- 重构面: fault 路径逐页簿记（mark/计数/ledger）合并为批操作——设计稿先行
  （数据面: 哪些簿记可合并/锁域不变量/INV6 保持）, 短设计节经报告即可动码
  （本片设计复杂度低于 W-7, 不强制停等, 但设计节先写）。
- 判据: mmap-pf 中位数改善入报告（目标个位数地板; 未达=如实报差距+归因）。
- 零回归: 全 guest 门（同轴一）。

## 红线
INV6/锁形/fail-open/=n/checkpatch 0E/0W; 只动 mm/corten_arena.c+mm/mmap.c+
fs/exec.c（如轴一需）+头文件+测试; 兼容零破坏（=on/=off 双世界）; 不 commit。
工件 results/r07/mv3cfeat/（与 debug 轮 mv3c/ 分开）; 报告
next/mv3cfeat-dev-report.md。

## 坑清单
同前片（pidfile/-j12/glob 星号/9p/interlock 复跑绿/串行 make/命名空间轮次名/
stats 渲染挂死——host-side timeout 防护照 mv3c 先例）。
