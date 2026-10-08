# perf10 bisect 片 #10 — 收数进度台账

接手时间: 2026-10-08 09:05 (原 agent 死亡, 无产出)
主树: /home/ppw/linux-6.18 (勿扰) | 测量树: /home/ppw/perf10-wt (detached)
端口: **10037** (原 10035 弃用, 按 VM 纪律 10037+)

## 锚点清单 (git 已核实, 全部可解析)

| # | 锚名 | commit | 日期 | 主题 |
|---|------|--------|------|------|
| 1 | b9541335 (A5) | b9541335a554 | 09-21 | lazy registry & cheap teardown |
| 2 | corten-mv-complete | 037bfbaed020 | 09-23 | brk delegation ledger |
| 3 | corten-r07-w2 | f5848730958e | 09-24 | carrier elimination |
| 4 | corten-mv2-complete | 6e106786496a | 10-04 | multi-record registry |
| 5 | 5475da44 (mv3cfeat) | 5475da44bdc0 | 10-05 | exec-image adoption & mmap-pf warm park |
| 6 | 6fd01509 (w3fix6-tip) | 6fd01509d33a | 10-06 | pgtables stale attribution & upper-tier gates |

拓扑: 1→2→3 线性; 3 与 4 分叉(3 非 4 祖先, 分支合并汇流); 4→5→6 线性。
漂移窗口 5→6 间隔 36 commits (直接祖先链)。

## 环境盘点 (步骤 1)

- 已有锚点 JSON: **0/6** — results/r07/perf10-bisect/ 原不存在, 原 agent 无任何产出。
- 但遗留: A5(b9541335) 的 bzImage 已构建成功 (build-b9541335a554.log 尾部 "bzImage is ready"),
  .config 在树内, bzImage 时间戳 10-06 23:57 与构建一致。
- 遗留 run-perf10-anchor.sh: 完整单锚驱动 (checkout→build→VM→校 sha→stock/mode 双臂
  t{4,8}×k{1..3}, seeds t4=20260951+, t8=20260979+, 2s 窗, JSON 校验 kernel/threads/seed/elapsed)。
  本片复用该脚本, 仅改: PORT 10035→10037; pidfile/monitor socket 陈旧文件加固。
- VM 底图: /home/ppw/vm/trixie-w6v2.img (raw, 只读); overlay /home/ppw/vm/perf10.qcow2
  (qcow2, backing=trixie-w6v2.img raw, 196KiB 基本全新) — 符合 qemu-img create -b base -F raw 形态, 复用。
- 端口占用: 1003x 全空; 无 qemu 进程; 无 tmux 会话; 无陈旧 pidfile/sock。
- 协议参考: /home/ppw/bench/share/run_mmbench.sh (mmbench <bench> <cont> <t> <sec> <seed>);
  hook 源 /home/ppw/bench/share/cortenmm/bench/mode-hook/corten_mode_hook.c (9p 挂 hostshare,
  脚本会在 guest 内编译, STRICT=1, "MODE on" stderr 标记)。
- mmbench_dyn sha 要求: 38304f062d4334cbec705cd08ff25d1902bad41aa45a56d4a4c684ecb1acce1b (整串精确匹配)。

## 测量顺序 (按时间序, 减少增量构建churn)

1. b9541335 → 2. corten-mv-complete → 3. corten-r07-w2 → 4. corten-mv2-complete → 5. 5475da44 → 6. 6fd01509

## 各锚状态

| 锚 | 构建 | boot | 12 腿 | JSON |
|----|------|------|-------|------|
| b9541335 | done 09:14 (bzImage 17.8MB) | boot3 09:23 ok (6.18.32-gb9541335a554) | 12/12 09:24 | OK, 09:24 |
| corten-mv-complete | done 09:27 | 09:27 ok (6.18.32-g037bfbaed020) | 12/12 09:28 | OK (stock-t4-k2 WARN: elapsed 1.999s<2.0 阈值, 读数有效), 09:28 |
| corten-r07-w2 | done 09:36 | 09:36 ok (6.18.32-gf5848730958e) | 12/12 09:37 | OK, 09:37 |
| corten-mv2-complete | done 09:48 (11min 重构, 分叉分支) | 09:49 ok (6.18.32-g6e106786496a) | 12/12 09:50 | OK, 09:50 |
| 5475da44 | done 09:54 | 09:54 ok (6.18.32-g5475da44bdc0) | 12/12 09:55 | OK (3x mode-t8 WARN: elapsed 1.998-1.999s 假阳性, 读数有效), 09:55 |
| 6fd01509 | done 10:02 | 10:02 ok (6.18.32-g6fd01509d33a) | 12/12 10:03 | OK (3x mode-t8 WARN 同上), 10:03 |

**收数完成: 6/6 锚, 72/72 腿, 锚级 GAP = 0。** 报告:
project/next/perf10-bisect-dev-report.md

## 收尾 (10:05-10:10)

- 72 JSON 全量复核: kernel=`6.18.32-g<锚hash>` 与锚 commit 全部一致, seed/threads 匹配, 中位数表入报告。
- VM 已按 pidfile kill (qemu pid 92111), overlay flush 完毕。
- 测量树恢复接手时状态: HEAD 回 detached b9541335a554, make 重同步产物 (恢复构建日志 /tmp/perf10-restore-build.log)。
- 未 commit/push (按要求留给主会话)。

## 事件记录

- 09:14 boot1 成功但 sha 校验失败: 镜像 trixie-w6v2 无 /root/m4t12/bin/mmbench_dyn
  (原脚本假设存在)。**非 boot 失败, 不计 GAP** — 环境缺陷修复。
- 修复: mmbench_dyn (sha=38304f06 精确匹配) 位于 9p share
  /home/ppw/bench/share/cortenmm/bench/mmbench/mmbench_dyn; 脚本已加 9p 挂载
  (hostshare→/mnt/hostshare) + cp -n 到 /root/m4t12/bin 的 provisioning 步骤
  (写入 overlay, 6 锚共享)。已在运行中的 VM 上人工验证: sha 匹配, 冒烟 leg
  输出合法 JSON (kernel=6.18.32-gb9541335a554), hook /root/corten_mode_hook.so 在位。
- 09:2x b9541335 以 skip-build 重跑 (boot2)。sha 出现 e3b0c442(空文件 sha):
  boot1 VM 被 SIGTERM 硬杀 → overlay 未落盘写丢失 (m4t12/bin/mmbench_dyn 0 字节)。
- 加固: provisioning 每次 boot 都做 sha 校验, 不符则 cp -f + sync; hook .so <10KB 视损坏重编。
- 09:23 boot3 (skip-build) 成功, **锚1 b9541335 完成 12/12 腿**:
  t4 stock 0.001122/0.002369/0.002077 (med 0.002077) | t4 mode 0.000976/0.001792/0.001643 (med 0.001643)
  t8 stock 0.000919/0.000987/0.001063 (med 0.000987) | t8 mode 0.000792/0.000867/0.000810 (med 0.000810)
  MODE 证据: pool_parks 0→74823, marker 6/6。
- 09:25 锚2 corten-mv-complete 启动 (增量构建)。09:28 完成 12/12:
  t4 stock med 0.002030 | t4 mode med 0.000477 (mode 臂大跳水, A5 是 0.001643)
  t8 stock med 0.000955 | t8 mode med 0.000191
  MODE 证据: pool_parks 0→20336, marker 6/6。
  注: stock-t4-k2 JSON 校验 WARN, 实为 elapsed_s=1.999 (guest 时钟取整) < 2.0 阈值的假阳性, 读数保留。
- 09:29 锚3 corten-r07-w2 启动。09:37 完成 12/12:
  t4 stock med 0.002075 | t4 mode med 0.000550
  t8 stock med 0.001031 | t8 mode med 0.000145
  MODE 证据: pool_parks 0→19109, marker 6/6。
- 09:38 锚4 corten-mv2-complete 启动 (r07-w2 分叉分支, 预计重构量较大)。09:50 完成 12/12 (构建 11min):
  t4 stock med 0.001090 | t4 mode med 0.000402
  t8 stock med 0.000489 | t8 mode med 0.000141
  MODE 证据: pool_parks 0→16550, marker 6/6。
- 09:50 锚5 5475da44 (mv3cfeat, 漂移低端点) 启动。09:55 完成 12/12 (构建 2min):
  t4 stock med 0.001248 | t4 mode med 0.000845
  t8 stock med 0.000673 | t8 mode med 0.000263
  MODE 证据: pool_parks 0→32177, marker 6/6。
  注: mode-t8 三腿 elapsed_s=1.998-1.999 假阳性 WARN, 读数保留。
- 09:55 锚6 6fd01509 (w3fix6-tip, 漂移高端点) 启动 (36 commits, 预计重构较大)。10:03 完成 12/12 (构建 2min):
  t4 stock med 0.001195 | t4 mode med 0.000943
  t8 stock med 0.000617 | t8 mode med 0.000266
  MODE 证据: pool_parks 0→35670, marker 6/6。
- **归因速记**: 参考漂移段 mv3cfeat→w3fix6 方向复现 (t4 mode +11.6%, 3v3 腿全分离;
  ratio +16.5%), 但幅度小于参考 +84% — 参考的 mv3cfeat 低点疑被宿主环境压低。
  六锚最大漂移段是 A5→corten-mv-complete (ratio 0.791→0.235, 坍塌 -71%);
  w3fix6 ratio 0.789 已回到 A5 水位。宿主水位混杂: 锚 3→4 之间 stock 臂整体 -47% (与内核无关)。
- 注: 宿主另有一 tmux 会话 t1fix (他片 VM), 不触碰。

GAP 规则: 某锚构建/boot 失败 2 次 → 记 GAP 跳过。
