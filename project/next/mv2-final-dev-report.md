# MV2 终局收官 dev report · 残留台账 #1/#3/#4 清偿 + 性能三刀之刀 2/3 首期 + 台账 #2 形变收官

agent: mv2-final（MM-dev 收官轮）。基线: mv-a0 @ 673ba95521de（与主链内核码面一致;
`git merge --ff-only 669a33f84279` = Already up to date, 分支已超前该基点, 内核码
与主树 HEAD 逐字节一致——`git diff 673ba95521de afd86b5c8a6c -- mm/ include/` 为空）。
授权: 用户指令「MV2 终局收官三项工作」。**未 commit**（红线: 主会话验证后统一入库）。
diff: 内核 +1241 行（worktree `/home/ppw/linux-6.18-mva`, 5 文件）; 项目侧
`project/results/r07/mv3d/mv3d-gate.sh`（SSH 韧性）与 `project/results/r07/mv2final/`
（本轮全部证据）。

---

## 0. 判定总览

| 项 | 判定 |
|---|---|
| 工作一 台账 #1（brk 路由 PT 页 UAF, P1） | **落**（读侧修复 + M2a 排他 + KUnit 竞窗锚 + **DPA 复测零签名**; brk 接管率 2636:0/4593:0, 台账"2:2882 失衡"消失） |
| 工作三 台账 #3（SSH 韧性, P1） | **落**（mv3d-gate.sh: 电池改 guest 内脱跑 + 短连轮询 + 分段取数 + sha256 逐件校验） |
| 工作三 台账 #4（wl_brk 多 brk 形, P1） | **落**（实测定 = 选项二: 多 brk 形入 `wl_brk_multi` 桶, `wl_brk_anomalies` 转结构性零 dead-man's switch; 现场读数 2/55027 benign 支撑） |
| 工作二 刀 2（脏域 rec_lo/hi 有界 park reset, 台账 #11） | **落**（widen×3 + zap 夹脏域 + 全窗 zap 塌缩 + KUnit 锚 dirty_range_park） |
| 工作二 刀 3（per-fault 簿记批化首期, 台账 #12） | **落（首期, 如实报）**（stats 合并: 2 条 fault 热路径全局 atomic → per-cpu; corten_map 双 meta 查找合一; **desc 锁粒度重构未动**, 登记余下） |
| 工作二 量测（台账 #10 前置基线） | **落**（mmbench_dyn 配对 boot 前后对照: mmap-pf t1 +27.2%/t4 +12.9%/t8 +3.8%, pf +2.4~8.8%, unmap +2.5~4.9%; 单配对轮如实披露） |
| 附产: 台账 #2（arena_stats 首读挂, P1） | **读侧收官**（挂死三重定界: registry xa_find 单调用内旋死 → 观测台账循环 → registry pin 轮; 三道预算化 + 形变改读观测台账; 首读 0.04s/0.05s; **腐坏生产者未定罪**, 登记余下） |
| 总验证 | **全绿**（三套件 =on 24/139/34 全零 fail + =off 25/28/7 全零 fail, 2 次 interlock flake 复跑绿; =n 17 对象含 mincore/msync 零 corten 符号; checkpatch 全 diff **0E/0W**; guest 双 boot 电池全判据过 + DPA 变体零签名） |

---

## 1. 工作一: 台账 #1 — brk 路由 PT 页生命周期 UAF（P1）

### 1.1 根因（两句）

`corten_arena_check_empty_locked()`（及 `corten_arena_frame_ptes_empty()`）是**全树唯二
不持 M2a 描述符锁就走查 PT 页内存的读者**: pmd_present 过了就走 `pte_offset_map_lock`
翻页即读 pte[]——而 M2a 退休协议（`corten_ptdesc_uninstall`: desc 写锁内置 stale、
之后才允许页内存消失, paper Fig.7）的排他只覆盖**持有 desc 锁的**事务层, 探针没持锁,
于是 pmd 读到 present 的那一刻与页内存已退休/复用（DPA unmap、pfn 重用）之间没有
任何屏障——DPA boot 86s 的 `check_empty_locked+0xb6`（`pte_present(ptep[pte_index(a)])`
= `test 0x101`, RCU 临界区内取已 unmap 页）即此窗。

### 1.2 修法

`mm/corten_arena.c` check_empty_locked / frame_ptes_empty 三段式（照 W-1.e1/图 7 的
pin 协议）: ① `corten_ptdesc_get(pfn)` 钉住窗口描述符 → ② `read_lock_bh(&desc->lock)`
横跨数组走查（uninstall 的 write_lock_bh 因此等待, 页内存不可能在走查中消失）→
③ **present pmd 但无 live 同 mm 描述符（stale/已 erase/pfn 重用为外 mm 页）= 已退休形,
跳过该窗并计数**——corten=on boot 上所有用户 PT 页自 initcall 起无条件被钩子跟踪,
不存在"合法未跟踪驻留", 跳过零漏判（退休臂先 zap 后退休, 窗按合同无内容）。
计数器 `corten_nr_probe_stale_skips`（debugfs 行 `probe_stale_skips`）。

### 1.3 验证

- **KUnit 竞窗锚** `corten_arena_test_declare_probe_stale_pt`: 合成 mm + 手装 PT 页 +
  desc, DPA 竞窗的确定性重演（uninstall + 毒化页内存 + pmd 仍 present）; 修前该形读
  毒页得假 -EBUSY（红面）, 修后探针跳窗、declare 通过、skip 计数 +1; 对照臂
  （live desc + present PTE）仍正确 -EBUSY。
- **DPA 复测一轮**（`dpa-verdict.txt`）: DEBUG_PAGEALLOC=y, =on journal boot,
  journalctl + 2×heap-churn: brk_grow **4593** / brk_legacy **0** / adopts 216,
  **零 Oops/零 GPF**（唯一 dmesg 项 = C2 容差 pgtables 一笔）; mv3b 86s 的 oops 面不复现。
- **非 DPA 世界**: 双 boot 电池 brk 接管 **2636:0 / 2631:0**（grow:legacy）——台账
  记载的"2:2882 失衡"消失, brk 路由对 heap 流量全量接管。

---

## 2. 工作三（P1 快件）

### 2.1 台账 #3: mv3d-gate.sh SSH 韧性（重试/分段取数）

故障面 = 7216s 电池的 SSH 会话中断 → battery-on.log 截断 + 工件 tar 空壳
（p2-audit-gate.txt 等）。修（`project/results/r07/mv3d/mv3d-gate.sh`）:
① 电池改 **guest 内脱跑**（nohup + rc 落 .rc 标记, 幂等启动——重试不重播）; 主机侧
短连轮询标记（每轮 gssh_retry 3）; ② 短命令一律 `gssh_retry`; ③ 工件 tar **split
16M 分段取数**, 每段 sha256 校验、重组后对 guest 侧全件 sha 复核; 单文件取数同样
`scp_verified`（5 试 + 双端 sha256）。`bash -n` 过。

### 2.2 台账 #4: wl_brk_anomalies 多 brk 形白名单预期修正（实测定）

实测: mv3d P2 rerun audit-gate（`rerun/p2-audit-gate.txt`）= wl_walks 55027 中
**brk_anomalies 2**, wl_brk_vmas 12, violations/unclassified 全 0——多 brk 形真实、
良性、低频。两选项按测定选 **分类多 brk 桶**（谓词放宽无对象: 谓词本就逐 VMA 收
in-span anon 形）: 新桶 `corten_nr_wl_brk_multi`（debugfs `wl_brk_multi`）计
`brk_vmas > 1` 的 walk（命名观测形）; `wl_brk_anomalies` 转为**结构性零的
dead-man's switch**（现谓词下无 walk 可达, 门脚本 grep 行不变、恒 0 且"应当 0"）。
KUnit `corten_arena_test_whitelist_audit` 断言同步（split heap → multi +1 / anomaly
不动 + 渲染行锚）。

---

## 3. 工作二: 刀 2 + 刀 3 首期（先量后动）

### 3.1 基线量测（台账 #10 前置, D35 刷新）

协议 = mv3cfeat D35 形（mmbench_dyn, seed=20260913+b*1009+c*97+t*7+k, min_seconds=2,
k1..k3 取中位, =on 默认进场, 配对 boot）, 驱动与读数
`project/results/r07/mv2final/bench/`（before= 673ba95521de 内核 8653bd036c11,
after= 本轮内核; `COMPARISON.txt` 全表）:

| cell | before | after | Δ |
|---|---|---|---|
| mmap-pf low t1 | 0.019556 | 0.024879 | **+27.2%** |
| mmap-pf low t4 | 0.001041 | 0.001175 | **+12.9%** |
| mmap-pf low t8 | 0.000370 | 0.000384 | +3.8% |
| pf low t1/t4/t8 | 0.0417/0.0429/0.0419 | 0.0453/0.0451/0.0429 | +8.8/+5.0/+2.4% |
| unmap low t1/t4/t8 | — | — | +4.9/+2.5/+3.5% |
| unmap-virt low t1/t4/t8 | — | — | +6.9/**−3.7**/**−1.8%**（3 run 噪声带内） |

单配对 boot 如实披露（噪声带 ±3-5%）; unmap-virt 停车的是全空窗（原 walk 已是
便宜 NULL-array 形）, 方向中性。**D35 刷新后的当前地板基线（before 臂）已落档。**

### 3.2 刀 2（台账 #11）: 脏域 rec_lo/hi 有界 park reset

- `struct corten_ptdesc` 新增 `rec_lo/rec_hi`（闭区间 PTE 索引, lo>hi = 空;
  install 初始化空）。
- **加宽点 = 全部元数据产生型迁移**: `corten_map()`/`corten_mark()`/
  `corten_swap_replay()`（covering 写锁内 = 有序; swap_out/mprotect/swap-in 只在已
  mark 索引上操作, 无需加宽）。INV6 不变式（PTE ⊆ 已 mark 范围）由此机械成立。
- **zap 读侧夹域**: `corten_arena_zap_window` 逐槽 `corten_txn_slot` 查找只在
  [dirty_lo, dirty_hi] 内做（域外槽按不变式必 INVALID, FILE-event 臂与 reset 臂
  等价跳过）; **PTE 级扫描保持全窗**（r03 defect C 纪律: 存在无元数据的活 PTE——
  legacy fallback 写）。
- **塌缩点 = 全窗 zap 完成**（`zw->fullwin`, 首轮计算、force 复跑沿用）: 全窗走完
  即 range 置空（park reset 的 O(dirty)→O(0) 重置点）; 局部 zap（punch 裁剪窗）
  不塌缩（过覆盖安全方向）。
- KUnit 锚 `corten_arena_test_dirty_range_park`: 精确加宽（idx 7/400 → (7,400)）、
  park 全窗 zap 塌缩为空、reactivation 后 idx 250 精确 (250,250)。

### 3.3 刀 3（台账 #12）: per-fault 簿记批化——首期（stats 合并）

- **2 条 fault 热路径全局 atomic → per-cpu**（`corten_nr_swapins`、
  `corten_nr_file_read_faults`）: S5 swap-in/read 安装臂每 fault 写一条争用
  cacheline 的形态消除; 读侧（debugfs 渲染/KUnit）求和, 语义不变。
- `corten_map()` 双 meta 查找合一（数组存在时首查即终, ensure 只在 NULL 臂）。
- **如实报**: spec §2.4-2 的"desc 锁粒度"重构（覆盖写锁跨 PTE 安装的粒度问题）本轮
  **未动**——那是协议级变更, 需独立小片+独立量测; 本刀首期交付 stats 合并 +
  簿记微合并, 效果已并入 §3.1 的 after 读数。

---

## 4. 附产收官: 台账 #2 — arena_stats 首读挂（读侧）

三重定界（每步 sysrq 栈实证, 三个内核构建）: ① 挂点 = `corten_mm_state_pages` 的
`xa_find(&state->arenas...)` **单调用内部**（xas_find+0x187 永不返回——预算化调用
次数无效）; ② 弃 registry xarray、改**观测台账**（corten_arena_list——`arenas` 文件
同源、同 boot 读取正常）+ 代际去重（desc->stats_gen, W-7 同帧桶精确）→ 仍旋死于
**台账循环本身**（+0x7e = `ar->mm != mm` 比较处, head 判定永不到达）→ ③ 台账与
registry pin 轮全部**预算化**（手工 RCU 迭代, 台账 ≤1M 步 / 窗 ≤65536 / pin 轮 ≤64,
越界计 `stats_walk_truncs` 并在渲染行披露——totals 为 floor, 如实）。
**判据达成: 首读 FIRST_READ_SECONDS=0.04/0.05, RC=0, 双 boot 一致。**
KUnit 锚 `corten_arena_test_stats_walk_budget`（预算 2 + 断言截断计数/渲染行 +
恢复预算零截断）。
**如实登记**: 观测台账/registry 在 =on guest boot 上的**腐坏生产者未定罪**
（=on-only、KUnit 不可复现; 涉 list/xarray 节点级腐坏, 疑 freed-arena-或-state
滞链后被 slab 重用——pool_eject 注记的同族已知类; 全部现场=sysrq 三栈 + 复现配方
已入 mv2final/）。读者侧现对任意腐坏形有界。

---

## 5. 总验证一览（全部最终内核 e681109ce35f89cd 上）

| 门 | 结果 |
|---|---|
| 三套件 KUnit =on（filter_glob=corten*） | **corten 24/0 · corten_arena 139/0 · corten_fault 34/0**（两次 interlock flake 见下） |
| 三套件 KUnit =off | **corten 25/0 · corten_arena 28/0（111 skip）· corten_fault 7/0（32 skip）** |
| flake 复跑判定 | =on 第一轮 `file_fork_mirror`（folio ref 3 vs 4）复跑全绿; =on 第二轮 `corten_test_txn_uninstall_interlock` 复跑全绿; =off 首轮同件复跑全绿——**同台账 #7 flake 族（复跑绿姿态）, 判据面外** |
| =n 折叠 | CORTEN_MM/ARENA/3×KUNIT =n: 17 对象（mva2 清单 + mmap_lock）全编译 RC=0, `mm/built-in.a` **corten 符号 = 0** |
| checkpatch | 全 diff 1241 行 **0E/0W**（"ready for submission"） |
| guest 双 boot 电池（=on journal, DEBUG_PAGEALLOC=n） | boot1/boot2 双绿: brk_grow 2636/2631 vs brk_legacy **0**; **arena_stats 首读 0.04s/0.05s RC=0**; smoke **26/26**（FAIL=0）双 boot; metis ×2×2 boot checksum 全 = `2d383eeed4ceb73b`; dmesg 各 1 笔 = C2 容差 pgtables（mv3d 口径）; systemd degraded = redis-server failed——**与 mv3d P1/rerun failed-units 逐字同形**（基线自带, 非回归） |
| DPA 变体（CONFIG_DEBUG_PAGEALLOC=y, 台账 #1 定罪世界） | brk 流量全速: brk_grow 4593 / legacy 0, **零 oops/GPF**（唯一 dmesg = C2 容差）; oops 面不复现（`dpa-verdict.txt`） |
| mmbench_dyn 前后对照 | §3.1 表; mmap-pf t4/t8 +12.9%/+3.8%（D35 判据 cell）, 无回退 cell 超噪声带 |

日志/工件: `project/results/r07/mv2final/`（console-gate-boot1/2/5/6+dpa.log、
gate-boot5/6.log、bench/{before,after}/raw/ ×36 JSON + COMPARISON.txt、
dpa-verdict.txt、bench-dyn.sh、mv2final-guest-gate.sh）。

---

## 6. 明示不做 / 登记余下

1. **台账 #2 生产者定罪**: =on boot 的 list/xarray 节点腐坏源头未定位（读者已对任意形
   有界, 判据面已闭合; 定罪需 =on 长跑 + slab/RCU 观测, 独立小片）。
2. **刀 3 的 desc 锁粒度重构**: 协议级变更, 独立小片 + 独立量测（本轮只落 stats
   合并/簿记微合并, 效果并入 after 读数）。
3. **mmbench 单配对轮**: 表中 ±3-5% 噪声带内的 cell（unmap-virt t4/t8）不作结论;
   定论需 3 配对 boot（D35 bisect 全谱另行）。
4. **台账 #5/#6（C2 pgtables 残账）**: 电池/DPA 各 1 笔 = house 口径容差, 未动。
5. 全程未 commit（红线）; 内核改动在 worktree 工作区, 项目侧工件在
   `/home/ppw/cortenmm/project/`（cortenmm repo 工作区）。
