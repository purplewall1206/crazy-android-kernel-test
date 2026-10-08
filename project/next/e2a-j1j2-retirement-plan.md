# E2-A 组（J1/J2 探针退役）—— D1 执行手术清单（2026-10-08 预制）

> 用户已批：浸泡即刻（结果/e2-soak/，≥24h）→ A 组落地。本文 = D1 的机械
> 执行清单：逐站点处置 + 锚去向 + 门序列。分支名建议 `pr-e2a`。

## 0. 范围裁定（A 组 = J1 计数对 + J2 审计 oracle；不含 wl/B 组与短路门）

**保留（非 A 组）**: PR-1 的窗口双比较短路（功能性守卫, E3 族）；wl 桶与
分类器（B 组, 用户已裁全删但独立 PR）；gup/reject 观测计数器（廉价
debugfs 观测, 渲染臂 E 组随行裁定）。

## 1. 站点清单（行号为 5464999 时点, D1 执行时重校）

### J1（计数对退役, 短路保留）
| 站点 | 处置 |
|---|---|
| mm/corten_arena.c:415-416 `corten_nr_j1_probes/hits` | 删 |
| :3336/:3340 递增点（slow-path terminus 内） | 删（短路分支与返回值不动） |
| :3697-3700 debugfs arena_stats 两行 | 删 |
| :4114-4121 `corten_arena_test_j1_probes/hits` | 删 + 头文件声明 |
| mm/corten_arena_test.c:8458-8459 读点（锚） | 锚改造：j1 断言 → **合成形不变量锚**（窗口地址 `vma_lookup()==NULL` + find_vma 走查返回 NULL——S-1 的直接证人，不再借计数器） |
| :15445-15452 audit_gate 渲染两行 | 删（gate_pass 计算式同步收窄, 见 §3） |

### J2（审计 oracle 整族退役）
| 站点 | 处置 |
|---|---|
| :124-125 前置声明 | 删 |
| :492-495 `corten_nr_j2_walks/violations/stale` + `j2_first_violation` | 删 |
| :14997-15160 `corten_audit_j2_covered/scan/sample/sample_locked`（~164 行） | 删 |
| 五个采样调用点（munmap_route :14378、release、punch、mode-exit、exit 族——grep `j2_sample` 全列） | 删调用, 注释里"V-A.3c hot-path sample"段同步删 |
| :3712-3723 debugfs 四行 | 删 |
| :4300-4337 四个测试访问器 | 删 + 头文件声明 |
| audit_gate 渲染面 :15430+（gate_pass 计算式 + j2 行） | j2 项删除; gate_pass 过渡态 = wl 桶单腿（B 组退役时该面整体交 E 组） |

### 锚去向（每锚一条理由, 纪律不变）
- 读 j1/j2 计数的 INV-MV2 walker 锚族 → **拆两半**：不变量半（zap 覆盖一致性）
  改合成形：test op 造 punch/park 形 → 直接断言 `corten_implant_covers()`
  与 slot 解码（现有 w7_frame_share_punch 锚族已具雏形, 补 J2 曾在运行时
  巡的两形：mode-exit 后窗净空、punch 后 record 重锚）；计数半随探针退役
  （commit message 逐锚列明）。
- j1 断言锚 → §J1 表的合成形。

## 2. 门序列（退役轮新口径, 用户已批）
1. 退役前基线 = 本日电池（results/r07/pr0-fix/battery/ + w3fix7 gate-takeover）
   ——已冻结, 不再重跑。
2. build 零新警告 → KUnit on3 全绿（锚去留逐条在上表）→ checkpatch 0E/0W
   → guest smoke 26/26 + metis 2d383eeed4ceb73b 精确同值 + dmesg corten 行
   零新增。**j1/j2/wl delta 口径废止**（oracle 即靶子）——以 §1 锚看守不变量。
3. 落地前置 = 浸泡读数收口（results/r07/e2-soak/soak.log 49 检查点, panic=0,
   gate 快照无漂移）+ 用户 D1 确认。

## 3. 过渡态披露
A 落地后至 B/E 落地前: audit_gate 面仅剩 wl 行（gate_pass=wl 单腿）; 电池
脚本的 audit 读数行照跑（wl 部分有效）。B/E 落地时该面整体退役, 电池脚本
同步改版（E 组 PR 内容）。
