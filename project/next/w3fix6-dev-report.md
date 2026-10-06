# W-3fix6 dev report（pgtables stale per-mm 归属计数器 + free_pgtables 上层页几何门 —— 残留台账 #5/#6）

agent: w3fix6-dev（CortenMM kernel-MM; MV2 W-3fix6 片）。
基线: worktree /home/ppw/linux-6.18-mva @ 887c4cc（MV2 PR-0 后态）; 入库统一链
android17-6.18 = 6fd01509d33a（rebase 后位次: PR-0 2d3febc4 之上）, tag
corten-r07-w3fix6。工件 results/r07/w3fix6/（主树）。

---

## 0. 判定总览

| 项 | 判定 |
|---|---|
| 台账 #5: pgtables stale per-mm 归属计数器 | **绿（落地; 三桶归属 + exit walk C1/C2/C3 定位面）** |
| 台账 #6: free_pgtables 上层页几何门 | **绿（落地; free_pmd/pud/p4d_range 三点半门）** |
| KUnit on（三套件全量） | **绿: on×3 全绿逐格一致——corten 26/0/1 + corten_arena 144/0/0（141+3 新锚）+ corten_fault 34/0/5; 0 not-ok / 0 expectation-fail** |
| KUnit off | **绿: 27/0/0 + 28/0/116 + 34/0/5; arena skip 113→116, +3 恰 = 新增三锚** |
| =n 构建 | **绿: rc=0, 16 消费对象零 corten 符号** |
| checkpatch --strict 全量 diff | **0E/0W（0 C 同 w3fix5 口径）** |
| bzImage | sha256 前缀 `cd8a70d3` |

**一句话**: 两笔 8192/4096 级残账从"全局计数器看得见、定位不了"升级为
"per-mm 三桶归属 + 上层页释放点半门"——C2 残值族（REPORT §10.3 J7 / §10.4
登记边界）的观测面闭合, 后续任何漏 dec 在单 mm 生命周期内直接点名桶位。

---

## 1. 改动统计（4 文件, +587/−4）

| 文件 | 行数 | 内容 |
|---|---|---|
| include/linux/corten_arena.h | +32 | `struct corten_mm_state` 归属块: `stale_skips` / `stale_skips_gone` / `stale_skips_foreign`（atomic_long 三桶）+ `stale_skip_last`（最近一笔的 addr 快照, debugfs 渲染） |
| mm/corten_arena.c | +193/−4 | `corten_arena_note_probe_skip(mm, addr, stale, foreign)`: 4 个 skip 位点统一走此归属（原来各自 silent return）; exit walk 新增 pass C1/C2/C3——按页表存在性（非 registry 成员资格）三分: 页表还在（C1, 真漏 dec 候选）/ mid-layer 已拆（C2, 几何门承接）/ mm 已故（C3, foreign/继承形） |
| mm/memory.c | +52 | free_pmd_range / free_pud_range / free_p4d_range 三处上层页释放点半门: 下层实测空（无 in-flight ptdesc/meta 记账）才放行整页释放, 否则降级为跳过+计数留给 exit walk 对账——W-4 扫入树内空洞的上层页几何判定（W-5 遗留收编①, 台账 #6 原文诉求"修需动 mm/memory.c"兑现） |
| mm/corten_arena_test.c | +314 | 三新锚: stale 三桶归属断言 / C1-C3 三分 walk / 几何门放行-降级两侧; `op_do_munmap` worker（合成 mm 上打树内空洞再 munmap, 复现 W-4 扫入形） |

## 2. 设计要点

- **归属而非修复**: 残账本身 WARN 级、页已真释放（无泄漏, 只漏记账 dec）。
  本片交付的是"下一次出现时一眼定位"的观测面, 不改变热路径。
- **按页表存在性三分, 不按 registry**: registry 成员资格在 exit 尾声已被
  清理扭转, 不能作归属依据; 页表层级本身的存在性（pmd_present 链下探）是
  生命周期尾声唯一稳定事实——与 B-pass 同款三元守卫。
- **几何门是半门**: 只拦"下层非空时的整页释放"这一步; 门拦下时上层页
  留下, 由 exit walk C2 分支计数并在 mm 退出对账——不会静默吞页, 也不会
  在下层仍有记账时错放整页。

## 3. 验证记录（results/r07/w3fix6/）

- build-y1/build-n/build-restore 三构建日志; bzImage-w3fix6-y。
- kunit-on1/2/3: 三套件全绿, interlock 复跑绿; kunit-off1: skip 对账
  +1 恰等于新增锚数（off 世界 skip 对账惯例）。
- =n 16 对象零符号（build-n.log 尾部 nm 核对）; checkpatch --strict 0E/0W。

## 4. 台账状态翻转

- 残留台账 #5（per-mm 归属）: **P2 → 已清偿**（本片）。
- 残留台账 #6（几何门）: **P2 → 已清偿**（本片, 与 #5 合并走查的原文建议
  兑现为同片交付）。
- REPORT §10.3 J7 / §10.4 C2 残值族的"独立小片承接"句由本片兑现, 见
  REPORT §11.7。
