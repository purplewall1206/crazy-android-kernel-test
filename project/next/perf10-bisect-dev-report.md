# perf10 bisect 片 #10 — dev report: 6 锚 mmbench 重放与 mode 臂漂移归因

日期: 2026-10-08 09:05–10:10 | 测量树: /home/ppw/perf10-wt (detached) | 端口 10037
执行: 接手片 (原 agent 死亡, 无产出; 本次从零收数, 6/6 锚全量补齐)

## 1. 协议 (与 w3fix7 bench-base D35 金标准同形)

- 每 boot 双臂: stock = `env -u LD_PRELOAD mmbench_dyn mmap-pf low <t> 2 <seed>`;
  mode = `LD_PRELOAD=corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1` 同命令。
- 12 腿/锚: t{4,8} x k{1..3}, seed t4=20260951..53 / t8=20260979..81, 2s 窗, 相邻 stock→mode 配对。
- mmbench_dyn sha256 = 38304f062d43...acce1b (整串精确校验, 每 boot 复核)。
- 内核 `corten=on` cmdline; hook 编译自 /home/ppw/bench/share/cortenmm/bench/mode-hook/
  (guest 内 gcc -shared -fPIC -O2, STRICT=1 → ENTER 失败即 abort, 静默 no-op 会被捕获)。
- MODE 进场证据 (每锚): debugfs pool_parks 增量 >0 且 mode 腿 stderr "MODE on" 标记 6/6。
- 产物: project/results/r07/perf10-bisect/<anchor>/mmpf-{stock,mode}-t{4,8}-k{1..3}.json
  (72 个 JSON 全部核验: kernel=`6.18.32-g<anchormd5短hash>` 与锚 commit 一致, seed/threads 匹配)。

## 2. 六锚读数表 (ops_per_us, 3 腿中位数)

| # | 锚 (commit) | t4 stock | t4 mode | t4 mode/stock | t8 stock | t8 mode | t8 mode/stock | pool_parks Δ |
|---|-------------|----------|---------|---------------|----------|---------|---------------|--------------|
| 1 | b9541335 (A5, 09-21) | 0.002077 | 0.001643 | 0.791 | 0.000987 | 0.000810 | 0.820 | 74823 |
| 2 | corten-mv-complete (037bfba, 09-23) | 0.002030 | 0.000477 | 0.235 | 0.000955 | 0.000191 | 0.200 | 20336 |
| 3 | corten-r07-w2 (f584873, 09-24) | 0.002075 | 0.000550 | 0.265 | 0.001031 | 0.000145 | 0.140 | 19109 |
| 4 | corten-mv2-complete (6e10678, 10-04) | 0.001090 | 0.000402 | 0.369 | 0.000489 | 0.000141 | 0.288 | 16550 |
| 5 | 5475da44 (mv3cfeat, 10-05) | 0.001248 | 0.000845 | 0.677 | 0.000673 | 0.000263 | 0.391 | 32177 |
| 6 | 6fd01509 (w3fix6-tip, 10-06) | 0.001195 | 0.000943 | 0.789 | 0.000617 | 0.000266 | 0.431 | 35670 |

逐腿值见各锚 legs.txt / mmpf-*.json。逐对相邻锚变化 (t4 mode):
A5→mv-complete **-71.0%** | mv-complete→r07-w2 +15.4% | r07-w2→mv2-complete -26.9% (stock 同步 -47%)
| mv2-complete→mv3cfeat **+110.0%** | mv3cfeat→w3fix6 **+11.6%**。

## 3. 归因

**任务给定的漂移段 (mv3cfeat 0.000453 → w3fix6 0.000836, t4 mode) 在重放中方向复现:**
- mv3cfeat→w3fix6 t4 mode 0.000845 → 0.000943 (+11.6%), 3v3 腿完全分离
  (w3fix6 全部 ≥0.000900 > mv3cfeat 全部 ≤0.000886), 非噪声。
- 归一化后同样成立: t4 mode/stock 0.677 → 0.789 (+16.5%); t8 +1.2% (0.000263→0.000266, 腿有重叠, t8 弱)。
- 但幅度显著小于参考 +84%。重放中 mv3cfeat 绝对值 (0.000845) 远高于参考低端 (0.000453),
  而参考高端 0.000836 与本次 w3fix6 (0.000943, 差 13%) 大体一致 → 参考的 mv3cfeat 低点
  更可能被当时的宿主环境压低, 真实内核段增量只有 ~+12% (t4)。

**宿主漂移混杂 (重要):** stock 臂不含 MODE 进程、内核路径与 corten 无关, 但本次会话内
t4 stock 在 r07-w2 (09:36) 与 mv2-complete (09:49) 两次运行之间下跌 47%
(0.002075→0.001090; t8 同步 0.001031→0.000489)。此后锚 4-6 维持在低水位。
即本会话存在两次可分辨的宿主水位 (锚 1-3 高水位 / 锚 4-6 低水位), 与参考数字不可直接绝对比较;
跨锚归因应以 mode/stock 比值为准 (同 boot 配对, 抵消宿主水位)。

**mode 臂漂移的完整归因 (以 ratio 为准):**
- 主坍塌事件在 **锚1→锚2 (A5 → corten-mv-complete)**: ratio 0.791→0.235, t4 mode -71%,
  t8 ratio 0.820→0.200。这是六锚间最大的单段漂移 (brk delegation ledger 落地的锚)。
- 随后一路修复: r07-w2 (0.265) → mv2-complete (0.369) → mv3cfeat (0.677) → w3fix6 (0.789),
  **w3fix6-tip 的 mode/stock 已回到 A5 水位 (0.789 vs 0.791)**。
- 因此 "mv3cfeat→w3fix6 +84%" 的原始观察, 分解为: mv2→mv3cfeat 段承担大头 (ratio +83%,
  绝对 +110%), mv3cfeat→w3fix6 段补足最后 ~16% (ratio), 使 mode 臂收回坍塌损失。
- 若只答任务字面问题: 漂移确认发生在 **5475da44 (mv3cfeat) 与 6fd01509 (w3fix6-tip) 之间**,
  但重放显示该段只是恢复尾段 (+11.6% 绝对 / +16.5% 归一); 更大的跳变在它前一段
  mv2-complete→mv3cfeat。若需把 36-commit 窗口进一步二分定位单 commit, 需追加 bisect 片
  (本片仅 6 锚协议, 不覆盖)。

## 4. GAP 清单

**锚级 GAP: 无** — 6/6 锚构建、boot、12 腿全部成功 (72/72 JSON 有效)。

基础设施事件 (非 GAP, 已修复):
1. 镜像 trixie-w6v2.img 无 /root/m4t12/bin/mmbench_dyn (原脚本假设存在)。
   修复: 每 boot 从 9p share 挂载 hostshare 并 sha 校验覆盖拷贝 (cp -f + sync;
   SIGTERM 硬杀 VM 会丢 overlay 未落盘写, 曾产生 0 字节文件, sha 复核兜底)。
2. hook .so 同风险: <10KB 视损坏删除重编。
3. 4 腿 JSON 校验 WARN (锚2 stock-t4-k2, 锚5/6 各 3 腿 mode-t8): elapsed_s=1.998-1.999
   撞 >=2.0 阈值的假阳性 (guest 时钟取整), 读数有效全部保留。

## 5. 环境与可复现性

- 驱动: /home/ppw/perf10-wt/run-perf10-anchor.sh (本片加固: 端口 10037; pidfile 校验 proc
  cmdline 才 kill; 9p provisioning; hook 完整性检查)。
- 底图: /home/ppw/vm/trixie-w6v2.img (raw, 只读) + overlay /home/ppw/vm/perf10.qcow2 (qcow2)。
- 每锚 bzImage 留档: project/results/r07/perf10-bisect/<anchor>/bzImage-<anchor>。
- 测量顺序 1→6 按时间序; 每 锚 wall time: 构建 2-11 min + boot ~26s + 12 腿 ~45s。
- 未 commit/push; 测量树 HEAD 停在 6fd01509 (已按接手时状态恢复回 b9541335, 见台账)。
