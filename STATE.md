
- **[2026-10-06 08:2x MV2 终局: W-3fix4 ✓ —— 台账 #1/#2/#3/#4 全消项（第十四片）]** 
  - 提交: cherry-pick **88ec127bbad3**（5 文件 +12866/−90, 含 battery 证据全集;
  tag **corten-r07-w3fix4**）。
  - **台账 #1 UAF 修复**: check_empty_locked 与 frame_ptes_empty（全树唯二不持
    M2a 描述符锁的 PT 页内存读者）收进 corten_ptdesc_get + read_lock_bh 的 pin
    协议（W1.e1 原形）。DPA boot brk 全速流量 ×300 轮 ×2 进程**零 oops**（原 86s
    定罪面）; 非 DPA: arena_stats 首读 rc=0 ×2（原挂死）——台账 #2 一并消项。
  - **台账 #3/#4**: mv3d-gate.sh SSH 韧性（分段 16M ×3 + md5）; wl_brk_multi
    独立桶（多 brk 形按观测计, anomalies 保持结构性零 dead-man's switch）。
    REPORT.md MV3.d verdict 表落（三腿 PASS/FAIL/GAP 逐格带证据）。
  - 验证: 三套件 =on x3/=off x3 全绿（含 declare_probe_stale 双世界锚）;
    =n 16 对象零符号; checkpatch 0E/0W; guest 双 boot =on 接管率 100%/=off 分离净。
  - MV2 终局台账状态: #1-#4 全消项; 剩 P2 级 #5-#9（pgtables stale 归属/几何门/
    flake/MADV_PAGEOUT/POPULATE）与 #10-12（性能三刀）+ P3 裁决件 #13-#16。
