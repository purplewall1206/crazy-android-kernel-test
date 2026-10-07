# 交接计划（handoff plan）— 2026-10-07 06:40 CST，删除账执行夜收官

> 新 agent 接手前必读。本文与 STATE.md（唯一权威状态源）配合使用：
> STATE 记"发生了什么"，本文记"接下来做什么、怎么做、怎么验收"。
> 项目常驻目标：彻底移除（窗口域 VMA 职责的完全退役 + 实删兑现）。

---

## 0. 交接快照（一切以实查为准，验证命令在括号里）

| 项 | 值 |
|---|---|
| 内核统一链 android17-6.18 | `cdd1f47d4601`（已推 GitHub） |
| corten-github 分支 | 同上（已推） |
| tag | `corten-pr-sequence`=34a103e2、`corten-report-d36`=cdd1f47d（均已在线）；**55 个旧 corten-* tag 未补推**（指向 filter-branch 重写前历史，补推需传整棵老树，网络慢，登记不阻塞） |
| 发布仓 /home/ppw/cortenmm master | `ae0611879`（= 内核链 project/ 同步 + TECH-REPORT 终稿，已推） |
| 删除账 | **PR-0..4 全序列落地**（REPORT §11.4 有逐枚表） |
| 台账 | 已闭 #1-#6/#8/#9/#11/#13(裁决a)/#14/#16(PR序列)；开放 #7(记账已交付)/#10(在制)/#12(在制)/#15(登记)/E2(二期) |
| **P1 首项** | **PR-0 =on 默认进场回归**（§T1，本交接的头号任务） |
| 最终报告 | REPORT.md §11（MV3 章+§11.9 回归披露）+ §12（D36 成绩单） |

---

## T1（P0，头号任务）: PR-0 =on 回归根修

**现象**: 非 lockdep 生产形 config 上，`corten=on corten_mode_default=on` 世界
任何 init（bash 动态链接亦然）在 `Run /sbin/init` 后 ~30ms 首个用户写
SIGSEGV → kernel panic（exitcode=0xb）。w6v2 与 mv3d 双镜像皆崩。
prctl 进场世界与 =off 世界不受影响（10-07 终电池全绿为证）。

**已定罪事实（不要重复劳动，直接读）**:
- 诊断报告: `project/next/pr0-defentry-regression-dev-report.md`（定罪矩阵 +
  机制画像 + 处置裁决）
- 七份 console 原件: `project/results/r07/pr0-regression/`
- 定罪: W-3fix5（47cc6cfb，PR-0 前）生产 config =on 干净；**PR-0 单点
  （2d3febc4）必崩**；lockdep 掩蔽是**概率性**的（w3fix6-y 两次干净是运气
  样本；最终链 lockdep boot 也崩——console-final-chain-lockdep-crash.log）
- config 全量 diff = 28 行 lockdep/DEBUG 族（语义无关、时序巨变）——
  **任何 =on 声明必须用生产 config boot 验证，lockdep 构建的 =on 读数不可信**

**候选面（PR-0 diff 的三个触点，按嫌疑排序）**:
1. `corten_bss_declare_route`（mm/corten_arena.c:14024）→ `corten_bss_declare1`
   adoption 臂（"0 = region answered, early return, **no populate**"）:
   exec 镜像 bss 走 vm_brk_flags 进来被收编成 region，但 init 首个写未被
   fault-fill 服务。PR-0 commit message 自述 "W-7 same-frame bucket eats and
   exec-image FILE record boundary frame overlap"——bss 声明与 exec 镜像
   FILE record 同帧（2M frame）共存是 MV3.c 时代新形状，W-7 多记录机械对
   "FILE record + 后到 bss record" 的 fill 路径可能有洞。
2. fault 路径的 live/idle 判定（`corten_fault_window_maperr`,
   mm/corten_arena.c:3313 附近）: "无 live（非 idle）arena → MAPERR"。
   若收编后的 arena 状态使 fill 被判 MAPERR（parked/idle 形），首写即死。
   **warm park（MV3.c）对 none PT 域的保温语义**在这里交叉。
3. 退化合同的时序窗: PR-0 的三守卫（VM_LOCKED/OVERCOMMIT_NEVER/may_expand_vm）
   逐项降级发生在 declare 路径内，与 exec 阶段的 mm 构建并发形状。

**建议根修路径（按成本递增）**:
- 步骤 1（30 分钟）: 读 `corten_bss_declare1` 全体 + `corten_fault_window_maperr`
  的 live/idle 判定 + fill 入口，画 bss 写的完整路径（declare→fault→fill）。
- 步骤 2（1 小时）: 定位实验——在 pr14-wt（已在最终链 34a103e2，mva config）
  加临时 printk（init 收 SIGSEGV 时强制打印 siginfo ip/addr，或
  `show_unhandled_signals` 强开），生产 config 构建，boot 复现，拿 faulting
  ip → `./scripts/faddr2line bzImage 符号` 定点。
- 步骤 3（对照实验, 30 分钟）: 把 `corten_bss_declare_route` 在 exec 期
  （`mm != current->mm` 判别，load_elf_binary 期 current->mm 还是旧 mm/NULL）
  强制走退化臂（return 1）——若 =on boot 转绿即锁定 exec 期收编为根因，
  且得到一个可辩护的临时修复形状（注意: 这会让 KUnit bss_adopt 锚变红，
  锚的 run_op 不满足 mm!=current->mm 前提，需同步修锚或换判别器）。
- 步骤 4: 真修 + 验收门。

**验收门（不可妥协）**:
1. 生产 config（主树 .config，非 lockdep）`corten_mode_default=on` 全系统
   systemd boot **×3 连续**（全新 qcow2 overlay，别复用——旧 overlay 会引入
   二次 boot 变量）+ `Run /sbin/init` 后 120s 无 panic。
2. KUnit 三套件全绿（锚不许删，合同变了就修锚并把理由写进 commit）。
3. 三腿电池复跑（mv3d-battery.sh 形状）: P1-off / P2-on / P3-journal。
4. j1/j2/wl delta 恒零 + gate_pass=1。
5. checkpatch 0E/0W + =n 零符号。

**预算**: 满预算一轮（历史上这类同帧机械修复 0.5-1 天）。若 4 小时只推进
到"定位但未修"，按项目惯例登记发运（更新 pr0-defentry-regression-dev-report.md
的机制画像节），不硬修。

---

## T2: #12 w3fix7（arena fill 温快路径）续接落地

**现状**: r09 接力会话 2026-10-06 23:57 后停摆（VM 死、进程消失）。
其 diff **未 commit**，躺在 `/home/ppw/linux-6.18-mva`（detached @ d36b317，
5 文件 +735/−73，`git status` 可见）。KUnit 已绿（on3: 26/0/1+146/0/0+34/0/5，
run1/run2 曾红两锚后被接力修绿）；mmbench 配对只完成 base 腿
（`project/results/r07/w3fix7/bench-base/` 12 个 JSON，w3fix6 基线）+
bench-after 只落了 1 个 JSON。

**接手步骤**:
1. 与 relay 会话确认所有权（若其复活）；确认死亡则接管。
2. `cd /home/ppw/linux-6.18-mva && git diff` 通读 diff（核心 =
   `corten_ptdesc_tracked()` lockless membership test，fill 温路径免
   mutex+re-arm）。
3. 补齐验收门: checkpatch、=n、guest smoke/metis/audit、mmbench 配对
   （协议: mmpf stock/mode × t4/t8 × k1-3，2s 窗，seed 20260951-53/079-81，
   驱动参考 w3fix7/bench-base 的产生方式与 /home/ppw/bench/share/bin/
   run_mmbench.sh；**mmbench sha256 须对 38304f06 前缀**）。
4. 端口 10034/命名空间 w3fix7 归 relay——接管后沿用或换 10036+（10035
   被 bisect 占用到其完成）。
5. commit → merge 双分支 → tag corten-r07-w3fix7 → 推送 → 台账 #12 闭。

**注意**: 该片是台账 #12（per-fault 簿记批化）的邻接实现，性能目标 =
把 t8 −61.5% 的尾部差距再收一截。预期收益读数写进 w3fix7 的 bench-after。

---

## T3: #10 地板演化 bisect——收数与集成

**现状**: 主会话派出的测量 agent（general-purpose）仍在后台跑
（worktree `/home/ppw/perf10-wt`，端口 10035，overlay /home/ppw/vm/perf10.qcow2）。
任务: 在 6 锚点（b9541335 A5 / corten-r07-w2 / corten-mv-complete /
corten-mv2-complete / 5475da44 mv3cfeat / 6fd01509 w3fix6-tip）重放同协议
mmbench，归因 mode 臂吞吐漂移（mv3cfeat 0.000453 → w3fix6 0.000836 ops/µs t4）。

**接手步骤**:
1. 若 agent 已完成: 取其 SUMMARY + `perf10-bisect-dev-report.md`，核对
   JSON 的 kernel 字段与锚一致 → commit 报告 → REPORT §11.8 填归因 → 推送。
2. 若 agent 死亡/超时: 检查 `/home/ppw/perf10-wt` 的产物目录
   `project/results/r07/perf10-bisect/`，有多少锚算多少（测量片允许 GAP），
   自行补测缺失锚后同上集成。
3. 归因读数回填后，#10 闭，台账刷新。

---

## T4: E2 二期实删（真·LoC 兑现，需要用户裁决排期）

**这是"彻底移除"目标剩下的实质大头。** 删除账 PR-0..4 的口径是"边界件
收口"（短路/门控/断言化，净 +242 行），mv3e §1.3 明载实删大头 =
**E2 审计机械退役**: J1/J2 探针 + 白名单分类器 + corten 测试面 20,573 行的
一部分。前提条件（原文）: 全账兑现 + 浸泡期。
**建议**: P1（T1）修复 → 三腿电池复跑全绿 → 浸泡（≥1 天 =on 挂机）→
向用户提裁决 → 按五组 24 项清单逐项实施退役 PR，每枚验收门不变
（j1/j2/wl delta 恒零在全账兑现后应改为"机械退役后探针不复存在"的新口径——
需先写退役轮的验收门设计稿再动手）。

---

## T5: 杂项与卫生（低优先，随手做）

1. **55 tag 补推**: `cd /home/ppw/linux-6.18 && comm -23 <(git tag --list
   'corten-*' | sort) <(git ls-remote --tags github | sed 's|.*refs/tags/||'
   | grep -v '\^{}' | sort)` 列缺，分批 `git push github <tags>`。网络快时
   一次成；慢则登记不阻塞。
2. **stale pidfile 清理**: `/home/ppw/vm/qemu-perf2.pid`、`/home/ppw/vm/
   qemu-w3fix7.pid`（进程已死，文件残留）。**删 pidfile 前先 ps 确认**。
3. **worktree 卫生**（接手 T1/T2 前先看清状态，勿盲目 clean）:
   - `/home/ppw/pr14-wt`: @ 34a103e2 最终链 + **mva lockdep config**（=on
     工作可直接用；做生产 config 实验时临时换主树 .config，用完恢复）
   - `/home/ppw/small1-wt`: @ 00c07b1（PR-4），有 madv-pair 的 untracked
     残留工件（无害）
   - `/home/ppw/small2-wt`: @ **2d3febc4（PR-0 单点，崩溃构建）** + 主树
     config——T1 的复现机可直接用它（bzImage 现成 = fb01d99b 前缀），用完
     checkout 回主链
   - `/home/ppw/perf10-wt`: bisect agent 领地，T3 完成前勿动
   - `/home/ppw/linux-6.18-mva`: relay 领地（w3fix7 未 commit diff），
     T2 接管前勿动
4. **残留 overlay 清理**: /home/ppw/vm/{regress,regress2..7,final,ku,on,
   pr14,pr234,small1,small2}.qcow2 中确认无人用后可删（都是薄 overlay，
   底图 trixie-w6v2.img / trixie-mv3d.img 永远别删）。

---

## 纪律与陷阱（本项目 110+ 提交的教训浓缩，每条都踩过坑）

1. **=on 声明只用生产 config boot 验证**（lockdep 构建的 =on 是概率性掩蔽，
   T1 的教训）。
2. **每片过全门**: build rc=0 零新警告 → KUnit on 全绿（锚红不许删，修锚
   要写理由）→ =n 零符号 → checkpatch 0E/0W → guest smoke 26/26 + metis
   checksum `2d383eeed4ceb73b` 精确同值 + audit gate_pass=1。
3. **config 翻转后的陈旧 .o**: =n 实验（make CONFIG_x=... 或 scripts/config
   -d）之后必须完整恢复 .config 并重跑受影响对象——S1/S2 都被这个咬过
   （S2 的"HEAD 必崩"首轮读数即陈旧对象假阳性贡献）。
4. **VM 纪律**: 永远 qcow2 overlay（底图只读）；端口先查
   `ps aux | grep qemu`（10022/10031-10038 历史占用）；杀 VM 用 pidfile，
   **严禁 pkill 模式串**。
5. **登记不代落地**: 时间盒到点没过门 → 如实登记 GAP 收口，不硬修、不降
   验收门。relay 的 w3fix7 没被代落地就是这个原因。
6. **push 规范**: 内核链 commit 只在 android17-6.18，然后 merge 到
   corten-github 双推；project/ 文档变更同步发布仓
   （cp 到 /home/ppw/cortenmm/project/ 后 commit push master）。
   发布仓 HEAD 现挂在 master（10-07 已修复，历史上曾挂在 master-sync）。
7. **commit message 风格**: `mm: CortenMM arena: <一句话> (上下文)`，
   正文写机械与门读数；项目文档 commit 用 `project/: <一句话>`。
8. **权威状态**: 每班结束更新 STATE.md（ dated 条目，最新的在文件尾部）；
   本文件是路线图，STATE 是事实账。

---

## 关键文件索引

| 文件 | 内容 |
|---|---|
| project/STATE.md | 全部历史事实（尾部 10-07 条 = 删除账执行夜） |
| project/REPORT.md | §10 MV3.d verdict 表 / §11 MV3 章（§11.4 PR 表、§11.9 P1 披露）/ §12 D36 成绩单 |
| project/next/mv3e-dev-report.md | 删除账五组 24 项清单 + 残留台账权威表 |
| project/next/pr0-defentry-regression-dev-report.md | T1 的完整诊断输入 |
| project/next/w3fix5/6-dev-report.md, madv-pair, flake-pie, pr234-deletion | 各片机械与门读数 |
| project/results/r07/pr0-regression/, final-battery/, w3fix7/ | 证据原件 |
| TECH-REPORT-AI-BUILD.md（发布仓根） | 面向读者的技术报告（含诚实边界节） |
| /home/ppw/bench/share/ | guest 侧 harness: run_mode_smoke.sh、r6dg/metis_eq、bin/mmbench、mv3d-battery.sh（ssh 形状参考） |
| ssh: `/home/ppw/vm/trixie.id_rsa`，端口按 VM | guest root 登录 |

---

## 建议执行顺序（第一班）

```
T1 步骤1-2（读码+定位实验, ~2h）→ 若锁定根因: 步骤3-4 修复+验收（~3h）
   └─ 期间并行: T2 接管确认（读 w3fix7 diff, ~30min）
T1 落地 → 三腿电池复跑 → T3 收数集成 → T4 裁决提案（写给用户）→ 推送收口
```
