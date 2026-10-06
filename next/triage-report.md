# w3fix4 triage 报告 —— MV2 终局收官·工作二（残留台账 #3/#4 快件）

- 日期: 2026-10-06
- 配套: w3fix4-dev-report.md（工作一）；采证在 worktree /home/ppw/linux-6.18-mva
  project/results/r07/{mv3d,w3fix4}/ 下。

## 1. 台账 #3: mv3d-gate.sh SSH 韧性（已修，两路径实测）

- 位置: worktree `project/results/r07/mv3d/mv3d-gate.sh`（/tmp 副本已失，正本在此）。
- 修形: 新增 `gfetch <guest-path> <local-path>`——①直接 scp ×3 重试（间隔 3s，每次带 600s
  超时）；②持续失败回落 16M 分段臂: guest 侧 `split -b 16M` + md5sum，逐段 scp ×3 重试，
  宿主侧拼接后 md5 验收，验收通过清理 guest 侧分片；`vm_run` 的工件取回改走 gfetch，失败显式
  say 告警不再静默吞。`bash -n` 过。
- 实测: 直接路径（活体 P2 VM 读取）; 分段路径 20M drill（2×16M 段，重组 md5
  `e0d5431c771e3c53…` 与 guest 侧精确一致）。原始故障形（66 字节
  "timeout: failed to run command 'g'" 破损件）不可再静默发生。

## 2. 台账 #3 采证补全: P2 audit_gate / counters 快照

- 现场: 活体 P2 VM（port 10031, trixie-mv3d.img, 内核 #365 = 电池原装, world = p3-journal
  遗留态）。原 P2 相位两件读数均损坏（audit_gate 66B 破损、counters rc=127）。
- **audit_gate: 已补**。`project/results/r07/mv3d/p2-audit-gate.txt`（21 行，读 rc=0）:
  gate_pass=1, wl_violations=0, wl_brk_anomalies=0, wl_brk_vmas=1, tree_entries=0,
  wl_delegated_vmas=105919——V-E 门零违规维持。
- **counters: 不可读（旧内核），处置记录落盘**。arena_stats 在该内核即残留台账 #2 的挂起:
  六个 16:41 起的 R 态不可杀 grep（各 130+ min CPU, SIGKILL 免疫）+ 新读
  `timeout 12 cat` 同挂；sysrq-t 栈 `xas_find/xas_load ← corten_mm_state_pages+0x108 ←
  corten_arena_stats_report`（mv3b §3 签名同形）→ `p2-stats-hang-sysrq.txt`（活体标本）+
  `p2-counters.txt`（处置记录）。补偿读数 = w3fix4 修复内核 DPA boot 的
  `results/r07/w3fix4/dpa-arena-stats.txt`（rc=0×2, 120 行）——台账 #2 修复的 after 证据。
- 注: 六个僵尸 grep 属上次电池失败遗留，SIGKILL 不可达（内核自旋态），留档披露；VM 未重启
  （主会话资产）。

## 3. 台账 #4: wl_brk_anomalies 多 brk 形预期修正（已落地，随代码 diff）

- 修形: 白名单扫描的 brk 谓词不再把"一次走查 >1 个 heap VMA"计 anomalies——实测合法形
  （全系统电池 heap-with-holes: 55027 次走查中 2 例，零违规、成员全 in-span 私有匿名）计入
  新独立桶 `wl_brk_multi`（计数器 + arena_stats/audit_gate 渲染行 +
  `corten_arena_test_wl_brk_multi` 锚）; `wl_brk_anomalies` 保持"当前谓词下结构性为零"的
  dead-man's switch（计数器块注释与 gate_pass 注释同步更新该预期）。
- KUnit 锚: wl 直方图用例断言双 heap VMA 走查 → `wl_brk_multi` 恰 +1、anomalies 不动、
  audit_gate 渲染含 `wl_brk_multi` 行。
- 读数: =on 全绿运行 `wl_brk_anomalies=0`（P2 补读 + DPA boot 双确认）; dpa-audit-gate.txt
  中 `wl_brk_multi=0` 行在位（新内核渲染）。

## 4. MV3.d verdict 表落 REPORT（已落）

- `project/REPORT.md` 新增 §10「MV3.d 全系统电池三腿 verdict 表」: P1/P2/P3 三腿
  PASS/FAIL/GAP 逐格（boot/mode 探针/LTP 构建与运行/smoke 面metis/dmesg 完整性/工件取回），
  每格带证据路径与采证缺口登记; w3fix4 补偿采证以〔w3fix4 补〕标注（P2 audit_gate 补读、
  counters 经 DPA boot 补偿、台账 #3 gfetch 修复）。
- 原始执行记录: BATTERY-STATE（P0-P3 四相位全程）+ rerun/RERUN-STATE（P2-on 7220s 重跑）。

## 5. 状态与移交

- 代码面（台账 #1/#2/#4/#5 修复 + 测试锚）全部在 worktree 未提交 diff:
  /home/ppw/cortenmm/patches/r07-w3fix4.diff（mm/ + include/，与 worktree `git diff mm/ include/`
  一致）。ops 面（gate 脚本、REPORT.md、采证文件）在 worktree 项目树内，随下批入库。
- 全套验证门（KUnit 三套件 flake 复跑、=n 16 对象零符号、checkpatch 0E/0W/1C、guest 双 boot
  =on 接管率/=off 回归、DPA 复测 PASS）见 w3fix4-dev-report.md §3/§2。
