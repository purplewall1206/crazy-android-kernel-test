# M4.T1+T2 收口报告 — per-cpu 2M-frame VA 杂志 + munmap 批处理（r07, 2026-09-19）

- worktree: /home/ppw/linux-6.18-m4t12（分支 m4-t12），基座 = 主树 HEAD **68697442097e**
  （M3b+M4.T0+M5.T1a 全量；fork_demote 已删、fork_mirror/fork_begin/commit 在树）。
  **未 commit**（review/maintainer 后续班次）；全量 diff 备份 /home/ppw/cortenmm/patches/r07-m4t12.diff
  （1713 行，3 文件 +1122/-221：include/linux/corten_arena.h / mm/corten_arena.c / mm/corten_arena_test.c）。
- 班次结构: 前班（取消）完成诊断 + T1/T2 主体实现；本班清点后续作（重建否决：
  半成品与诊断 D3 逐条对应，且含一轮已生效的 boot 验证修复），补 4 处正确性修复 +
  全链验证 + 性能收口。

## 0. 清点结论（brief 第一问）

前班 worktree = 基座对齐（log 顶 = 68697442097e）+ 半成品 diff（staged+unstaged,
1112 行）+ 诊断完成（results/r06/m4t12/DIAGNOSIS.md，含 D1 基建缺陷推翻 G1 旧读数）。
实现主体（T1 杂志 + T2 批处理）与 D3 重心调整逐条对应，最后一轮 unstaged 修复
（mode_exit 防活锁单调扫描、zap ptep++ 步进、KUnit 钩子加锁、mag 用例cpu选择）已在
11:48 bzImage 上通过 smoke+JTB。**判定: 续作**（checkout 重来只会丢已验证状态）。

## 1. 诊断节（引用 + 本班复核）

前班 results/r06/m4t12/DIAGNOSIS.md 三条结论（本班全盘认收）:
- **D1**: mmbench 是静态链接，LD_PRELOAD hook 从未进入其 mm —— t5-run2/g1-consol
  的全部 mmbench "T0" 臂从未进入 MODE，G1 "+7% 稳定小效应" 是 legacy-vs-legacy
  慢漂移，**该口径作废**。apps 臂（G3/G4）不受影响。
- **D2**: 动态二进制下的真实 MODE 画像: 写锁排队（osq/rwsem）为两臂共同瓶颈;
  MODE 额外成本 = xarray 节点生灭（mpl 画像 ~35%）+ munmap zap 与 refill 两侧
  各付一次 mmu_gather 收尾 flush（IPI 链 ~54%）。
- **D3**: 实现重心 = T1 带真回收（marker 复位 + recycle list）+ T2 单查找 fast path/
  单 ptl 锁段/单一 flush/force_flush 惯用法; 测量基建换动态 mmbench。

本班未重跑 perf 画像（前班 15s×12 腿画像已足够指引实现; 本班资源优先给验证链与
ABAB 收口——D2 的符号级结论被本班 probe 复核数据间接印证，见 §4.2）。

## 2. 实现（最终形态）

### 2.1 T1: per-cpu 2M-frame VA 杂志（PS-E1, DESIGN §1 E1 行; 替换 T0 全局 cursor 主路径）
- `corten_va_seg_claim()`: 为当前 cpu 从全局 cursor 认领 1GiB 段
  （CORTEN_VA_SEG_FRAMES=512 帧 × 2M），claim 时一次性做障碍扫描
  （find_vma_intersection 跳 legacy VMA / xa_load 跳 arena 与外来 marker），
  整段打 CORTEN_VA_RESERVE sentinel（corten_va_reserve_sentinel，真对象非 IS_ERR）。
  窗口 [16T,64T)/障碍跳跃语义原样保留; 耗尽回退全局 cursor 路径
  （corten_arena_window_place_global = T0 原逻辑，>1 段的大请求也走它）。
- `corten_va_mag_alloc_cpu()`: 回收 list 优先 → 本 cpu 段私有 bump（每次发放仍做
  marker 校验 + VMA 障碍校验，T0 契约不放松）→ 段耗尽回 claim。cpu<0 取 running
  cpu（get_cpu_ptr 段内自洽; 全部写方持本 mm mmap_write，percpu 布局服务地址
  局域性与 xarray 子树不相交，非并发）。
- **真回收**: RELEASE 对段内帧复位 marker（xarray 子树保温、无节点 churn）并按
  LIFO 块推 va_free（上限 2GiB 帧，超限退 T0 行为，计数可见）; 下次发放**同址复用**。
  debugfs arena_stats 新增 seg_claims / mag_skips / va_recycles。
- sentinel 语义: lookup/query 读作"无 arena"（fault 落 legacy 漏斗）; overlaps/mode_exit/
  mm_exit/fork_unfreeze/fork_begin/fork_commit 全部跳 sentinel——**与 M5.T1a
  fork_mirror/fork_begin/commit 共存核对过**（子注册只 xa_store arena 自身帧范围，
  子杂志全新，child release 走 plain erase，无 marker 泄漏路径）。
- KUnit: corten_arena_test_mag_recycle（release→同址复用→计数闭合）、mag_marker
  （sentinel 读作无 arena + DECLARE 拒绝）、auto_route 扩展（显式 cpu 发放的确定性
  断言 + 全 online cpu 两两不相交 + 超大段回退全局 + 耗尽计数）。

### 2.2 T2: munmap 批处理
- `corten_arena_munmap_route()` 单查找 fast path: start 与 end-1 同 2M 帧时省第二次
  RCU lookup+pin; 引用记账按"实际 pin 了几个"（跨帧同 arena = 两个引用，EXACT 分支
  各 put 一次）——修复 r03 DoD-B 引用泄漏形状在 fast path 下的复发（JVM 64MB heap
  free drain 超时即此形状，KUnit 回归锚 corten_arena_test_auto_attach_release 尾块）。
- `corten_arena_zap_window()` / `zap_untracked_window()`: 整窗一次 pte_offset_map_lock
  （窗宽 ≤1 PMD，ptep++ 步进），删除显式 flush_tlb_range（与 mmu_gather 收尾重复——
  一次 zap 一次 shootdown）; __tlb_remove_page_size 批满走 zap_pte_range 的
  force_flush 惯用法（旧代码忽略返回值 = 潜在越界）。unmap_chunk 的 gather 一次
  收尾 = 多窗事务一次 drain 一次 flush。
- punch 路由: 复用 unmap_chunk/分类器，T2 改动自动覆盖; mmap_punch 的帧擦除与
  杂志 marker 丢失路径（mag_skips）闭环。

### 2.3 本班 4 修复（前班半成品之上的正确性收口）
1. **recycle 发放路径补 VMA 障碍检查**: RELEASE 与再发放之间，显式地址 legacy mmap
   可占用已复 marker 的帧——旧码只查 marker 不查 VMA，MAP_FIXED 落点会毁外部映射
   （违反 T0 "发放范围绝不携带外部映射"契约）。现回收块含 VMA 即整块丢弃（保守，
   计数可见）。
2. **va_nrfree 记账漂移**: 部分消费回收块时未减计数（仅整块消费时减）→ 界限检查
   提前饱和 + debugfs 数字失真。改为每次发放按 frames 减。
3. **punch 洞帧不再恢复 marker**: release 循环对 xa_load==NULL（punch 洞，其上有
   legacy VMA）的帧直接跳过 release_frame——旧码会给洞帧打回 sentinel，使回收路径
   误判"可发放"。live-arena 帧才进回收。
4. **pinned_end 未用变量删除**（-Wunused-but-set 形状; 记账语义由 ar_end==NULL 表达）
   + 1 处杂散空行。checkpatch --strict 复跑 0E/0W/0C。

## 3. 验证矩阵（全链）

| 项 | 结果 | 证据（results/r07/ 或 worktree） |
|---|---|---|
| =y 全量构建 | **零新增警告**（仅 2 条既有基线: cpuidle objtool + memblock modpost，与 r06 m5t1a build-y-full.log 逐字同形） | /tmp/m4t12-build-y.log, m4t12-y-final.log |
| KUnit corten* on×2 | **全绿 ×2**: corten 24+1skip / **corten_arena 29（=基线 27+mag_recycle+mag_marker，新用例全过）** / corten_fault 24 | kunit-on1.log, kunit-on2.log |
| KUnit off×1 | off1 一例 flake（txn_uninstall_interlock "worker A never acquired"，见下注）; **off2 复跑全绿** 25/25 + skip 分布符合 off 口径（arena 18+11skip / fault 4+20skip） | kunit-off1.log, kunit-off2.log |
| =n 七对象 | **PASS**: vmlinux nm 零 corten 符号; mmap/memory/mprotect/madvise/mremap/migrate/sys/fork/fault/mempolicy 十对象 nm 零 corten 符号; 构建零新增警告 | /tmp/m4t12-n-build*.log |
| checkpatch --strict | **0 errors / 0 warnings / 0 checks**（1661 行） | 本地复跑 |
| lockdep 变体（M7 联动） | PROVE_LOCKING=y 构建（仅 2 条基线警告）+ KUnit 全绿 25/29/24 + **零 lockdep/oops 签名** | kunit-lockdep.log |
| guest run_mode_smoke | **26/26 PASS**（#21 内核 + #25 最终件复跑，见下） | abab/battery.log |
| JThreadBench 回归 | **MODE rc=0**（median 1884.0ms）+ base rc=0（1958.2ms）——gupfix 成果未破坏; dmesg corten warn 全程 **0** | abab/jt.out, jtb.out |
| =y→=n→=y 往返 | 恢复构建零新增警告; KUnit 复绿（构建计数 #21→#25 为指纹差来源） | kunit-on3-restore.log |
| 最终件 guest 复证 | bzImage #25（与树终态一致）: boot + battery 复跑（结果见 §5 附录） | abab/battery.log |

> off1 flake 注: `corten_test_txn_uninstall_interlock` 位于 mm/corten_test.c（M2 协议套件，
> 本班 diff 零触碰）。失败形状 = 主线程 10s 轮询窗错过 worker 的 a_locked=1，而 worker
> 实际跑到 phase=EXITED（begin_ret=0）——CPU1 饥饿型调度 flake（宿主同刻 4 个 qemu VM）。
> 该测试在 r06/m5t1a 历史上有同形失败一次（kunit-on1-final.log）。复跑全绿，判 flake 不判回归。

## 4. 性能对照（动态 mmbench 口径; 判定 = 判据的诚实回答）

### 4.1 ABAB×3 四格（guest vm-m4t12, 8 vCPU/4G/KVM, boot corten=on mitigations=off
kunit.enable=0; 双臂同件 mmbench_dyn sha256 38304f062d43…（动态链接，hook 实证进入）;
同 seed 配对 = run2/g1-consol 驱动公式; BASE=env -u LD_PRELOAD / T0=hook STRICT=1;
每腿 3s×3 轮取中位; 数据 results/r07/abab/raw/ 24 条 JSON + guest 侧 raw.tgz）

| 格 | base med (ops/µs) | MODE med | 配对 Δ |
|---|---|---|---|
| unmap-virt low t4 | 0.07245 | 0.00862 | **-88.1%** |
| unmap-virt low t8 | 0.02827 | 0.00532 | **-81.2%** |
| unmap low t8 | 0.00227 | 0.00175 | **-22.6%** |
| mmap-pf low t8 | 0.00197 | 0.00026 | **-86.7%** |

### 4.2 机制证据（同 boot 配对 probe + 杂志计数器）

- **杂志计数器（arena_stats before→after，全 battery+ABAB 窗）**: auto_mmaps +26055,
  munmap_releases +25835, **va_recycles 25805（≈99% 发放走回收）**, seg_claims 88
  （26k 次发放摊薄到 88 次 1GiB 段认领）, mag_skips 0 —— T1 机制按设计工作，
  xarray 节点逐 op 生灭（D2 的 35%）已消除。
- **in-boot probe 配对（m4t12_probe, 8000 iters, 本班 #21 内核）**:
  - uv（arena 内 16KB chunk munmap+MAP_FIXED refill，不触页）: **MODE 1.62µs/op vs
    legacy BASE 4.83µs/op = 3.0× 快**（PS-F4 unmap-virt 方向在本移植首次在 boot 内
    配对成立; 前班旧内核 probe 同形 6.3 vs 13.0，跨 boot 仅作方向参考）。
  - mpl（mmap(NULL)+munmap 对，16KB）: MODE 17.4µs vs legacy 4.8µs —— 仪式税
    ~12.6µs/op。前班旧内核同 probe 49.5µs（MODE）→ 本班 17.4µs，**T1/T2 把仪式税
    砍 ~65%**（跨 boot 方向性; 幅度受邻道污染保守读）。
- **T2 单 flush/单查找**: zap 显式 flush_tlb_range 删除后每内容 unmap 一次 shootdown
  （代码路径见 §2.2; JThreadBench 的 64MB heap free 正常 rc=0，drain-timeout 锚全绿）。

### 4.3 G1 判定: **NOT MET（0/4, 动态口径）**——机制级解释

判据（≥2 项同时 ≥10%）下 0 项过线且全部大幅为负。这不是 T1/T2 未生效，而是
**mmbench 的 unmap-virt/unmap/mmap-pf 格在 16KB 粒度上 = mmap(NULL)+munmap 对 =
MODE 下每次操作一整个 arena 生命周期**: DECLARE（state ensure + ctl_lock + 校验链 +
shadow-VMA 化 + 逐帧 xa_store + obs 登记 + percpu_ref_init）+ RELEASE（drain + ref_exit +
shadow-VMA teardown + legacy do_munmap + kfree_rcu）。probe 的 mpl 形态把这层定价在
**~12.6µs/op 且全程在 mmap_write 之内**（多线程排队放大, t4/t8 塌得最狠 = 写锁串行化
点未消失——DEV-7 口径的 T1 完整愿景仍属后续切片）。T1 杂志优化的是生命周期**内部**
的 VA 发放（已做到 99% 回收 + 0 节点 churn），改不了"每 16KB mmap 建一个 arena"的
T0 架构事实（M4T0_SPEC DEV-12 per-mmap auto-arena）。

结构性结论（M8 素材）:
1. g1-consol 的 "+7% 稳定小效应" 系 legacy-vs-legacy 假象（D1），G1 的历史读数全部作废;
2. 在 MODE 进程口径下，microbench 四格的真实代价为 -22%~-88%，其中 ~-88% 的
   unmap-virt/mmap-pf 与 ~-21% 的 unmap 之差恰与 probe 的两形态（RELEASE 型 vs
   CHUNK 型）定价一致——CHUNK 型（glibc free() churn，dedup_eq）MODE 赢 3×，
   RELEASE 型输在生命周期;
3. **G1 翻盘的正确杠杆不是 VA 分配器，而是 arena 生命周期成本**: 候选 T1c = arena
   描述符缓存复用（percpu_ref/ctl_lock/obs/描述符 kalloc 池化）或 per-thread 常驻
   multi-chunk arena（16KB op 退化为 chunk mark/unmark，即 probe uv 形态）。后者
   预期把四格拉回 probe uv 的 3× 方向; 建议规划者立 OQ。

## 5. 附录: 最终件（#25）guest 复证

- boot corten=on mitigations=off kunit.enable=0; run_mode_smoke **26/26 PASS**;
  JThreadBench **MODE rc=0**（median 1952.5ms）+ base rc=0（同窗）;
  probe sanity 五形态跑通; **dmesg corten warn 0**（#21 首证 + #25 终件复证,
  两次 battery 独立全绿）。
- VM 留运行: tmux vm-m4t12（port 10026, pidfile /home/ppw/vm/qemu-m4t12.pid）,
  内核 = worktree 终态 bzImage（6.18.32-g68697442097e-dirty #25）。

## 6. 红线自查

- M4.T0 白名单矩阵/release 规则/migrate 钩子: 未触碰（route/classify/migrate 拒绝
  原样; auto_route KUnit 全绿）。
- M5.T1a fork 交互: fork_begin/commit/unfreeze 的 sentinel 跳过 + 子注册路径核对
  （§2.1）; fork smoke 用例（fork/waitpid/child-checks/parent-content）全过;
  fork 冻结窗内 munmap 路由的引用/drain 语义不变（born-atomic ref 两引用记账，
  KUnit 跨帧 EXACT 锚 + drain_timeouts 计数不动）。
- D12 兼容/g1-consolidation: g1 数字被 D1 判为口径作废（非本班推翻——前班诊断，
  本班 ABAB 为其动态口径复测）; g1-consolidation.md 原文未动。
- 未 commit、未 push、未碰主树/其它 worktree、密码未落盘。

—— r07 收工。判定汇总: 功能全绿; **G1 NOT MET（0/4, 动态口径, 机制级解释 + T1c 杠杆提案）**;
T1/T2 机制本身按设计生效（99% 回收率 / 仪式税 -65% / CHUNK 形态 3×）。
