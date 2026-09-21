# M-V A.1 验证报告 · park 去 VMA 手术（VMA 移除系列第二片）

- 日期: 2026-09-22（夜班 D14 精神延续）
- worktree: /home/ppw/linux-6.18-mva（分支 mv-a0, 基座 34f1ae661be3 = A.0, **未 commit**）
- 合同: MV_VMA_FREE_SPEC.md §3.1.1 + §1 Dominion（D20-a）+ §2 区域记录（A.0 已落）
- 内核构件: `results/r07/mva1/bzImage-mva1-y`（6.18.32-g34f1ae661be3-dirty, 构建号 #29,
  sha256 2442d785…8c42c3）
- **不 commit**（行为变更切片, 留 review/maintainer）

## 0. 手术清单（实现面）

| # | 位置 | 手术 |
|---|---|---|
| 1 | `corten_arena_pool_park_locked` | park 后**不再重铺 PROT_NONE 预约 VMA**: zap→tlb_finish→idle 发布→register(RESERVED, 保留存活 may/rflags)→`corten_arena_park_unmap_vma()`=do_munmap 摘除预约 VMA 并经常规漏斗退休空 PT 页（同时回冲 total_vm 记账）；失败(内存压力)→真 RELEASE 兜底, 计数 `park_unmap_fails`。VMA-less 再 park: 手工回冲 take 的 total_vm |
| 2 | `corten_arena_pool_parkable` | 去除 VMA 必存在前提: vma==NULL（池复活形态）可 park; 有缓存指针则校验边界 |
| 3 | `corten_arena_pool_take` | 删除 vma_lookup/validate/shadowize/vm_flags 重编码全部手术: 复活=**纯 metadata 翻转**（rclass=ANON + ar->prot=本次 mmap prot + 发布）; [C1] 判空保留; RLIMIT_AS/total_vm 记账入 `corten_arena_pool_reactivate` |
| 4 | `corten_arena_pool_reactivate` | 同上; DECLARE 侧（prctl 路径无 prot 输入）取中性 RWX |
| 5 | `corten_arena_pool_prepare_locked` | VMA 校验臂删除, 换 [C1] 纯 PT 判空（池命中=纯 metadata 复活） |
| 6 | `corten_arena_fault_owned` | Tier 1b: VMA-less arena 所有权=**纯 metadata**（描述符边界 + 帧槽==ar）; punched 形态帧已擦除照旧正确拒绝 |
| 7 | fault 体五臂（map_anon/zero/restore/cow_write/swap_in） | vma==NULL 全臂可用: perm→pgprot 纯函数、`corten_pte_mkwrite`(novma)、rmap 添加/移除对称跳过（A.1 边界: 无 VMA 无 rmap 锚）、flush/set_access_flags 的 VMA-free 形态 |
| 8 | 预分配 | `corten_arena_folio_alloc_novma`（folio_alloc_mpol_noprof + 任务默认策略, arena 无 mempolicy 故与 vma 路径等价） |
| 9 | zap（tracked + untracked） | vma==NULL 可运行; `corten_zap_release_page()` 统一 rmap/计数拆分（无 rmap 页不回减 mapcount; FILE 判别只信 vma!=NULL 时的 folio_test_anon） |
| 10 | mprotect 路由 | VMA-less arena = **纯 metadata perm 改写**（modify-prot 的 VMA-free 形态、flush_tlb_mm_range、 notifier 以 mm 为参、whole 臂只动 ar->prot） |
| 11 | mmap MAP_FIXED mark 路由 | 去除 vma==NULL 拒绝门（mark 本就纯 metadata） |
| 12 | `corten_arena_release_arena_locked` | 零 VMA 拆除臂: zap + `corten_arena_free_ptes_novma`（PT 页退休, M2a 卸载）+ total_vm 反记账（仅 live 形态） |
| 13 | `corten_arena_mm_exit` | 无 VMA arena 前置清扫（zap + PT 退休）, 在 unpublish 之前、ctl_lock 之外（锁序不倒挂） |
| 14 | fork | `corten_arena_fork_copy_window_novma`: dup_mmap 不走无 VMA 窗 → 显式 PT 复制臂（子侧表分配 + COW wrprotect 双侧 + swap 项复制 + AnonExclusive 清位）, ④-1 门放行无 piece 且无 vma 的 arena |
| 15 | shrinker | 老化臂 vma-free; **pick 门控**: 无 VMA 窗的页不可被 reclaim pick（rmap 缺失 → ttu 报成功 → UAF; A.1 边界, 计数 shrink_skipped） |
| 16 | INV-MV3 checker | `corten_region_record_ok`（may⊇prot + idle⇔RESERVED）作为 register() 写点 tripwire; `corten_region_invariants_ok(mm)` 注册表走查（KUnit/审计入口, 头文件导出, =n 桩返回 true） |
| 17 | debugfs | 新计数行 park_unmap_fails / cascade_skips（终版 cascade 已移除, 计数保留恒 0→实际已删, 见边界 3） |

边界（A.1 登记不修, 按归口移交）:
- **B-1**: VMA-less 窗的页**无 rmap 锚**（SPEC §2.3 carrier=A.2b 交付）→ (a) reclaim/shrinker 不可 pick（内存压力下这些页滞留到 munmap/exit）; (b) GUP-slow 对窗 EFAULT（无 VMA 可走; GUP-fast 页表直走不受影响）; (c) fork 走显式 PT 复制臂。
- **B-2**: 无 VMA 窗的 PUD/PMD 上级表不回收（4K×2/窗残留至 mm 死亡; V-D exit walk 整体接管; 首版级联在 KUnit 真链路复现非规范页表态 GPF——本 `pmd_offset` 基址错位形态——故按"先对后收"原则整体移除, `cascade_skips` 计数随之删除）。
- **B-3**: chunk munmap 语义不变（Fig.8 L9-13: 保 VA 的内容丢弃 + KEEP_PERM）; 探针按此口径断言。

## 1. 语义变更清单落地（SPEC §3.0 登记集）

| 项 | 状态 | 证据 |
|---|---|---|
| **S-1** park 访问 ACCERR→MAPERR | ✅ 落地+实证 | guest `mva1_probe`: 对 parked 窗写访问的子进程 `si_code == SEGV_MAPERR`（活体 si_code 实测, 非推测）; KUnit: `corten_arena_user_fault` 对 parked 窗 FALLBACK（legacy 无 VMA→bad_area） |
| **S-2** 窗 PTE 无 soft-dirty | 不变（V-C 文档化） | 既有 |
| **S-3** swapoff 提前换回盲区 | ✅ 登记（V-D 复测） | unuse 走 VMA; 换入走 fault 慢车道（swapin 臂 vma-free 可用）|
| **S-4** parked 从 maps 消失 | ✅ 落地+实证 | guest: `maps_hits(parked 窗)==0`（原 PROT_NONE 预约段消失）; KUnit `EXPECT_NULL(vma_lookup)` 双断言更新+新增 |

## 2. 验证矩阵

| 项 | 结果 |
|---|---|
| =y 全量构建 | ✅ 零新增警告（仅基线 objtool cpuidle + modpost memblock, m9p2/mva0 构建日志同签名） |
| KUnit corten\* on×2 | ✅ 24/0/1 + 55/0/0 + 30/0/2 ×2 全绿零 Oops（新增 vma_free_reuse/inv_mv3/fork_vma_free 三用例绿） |
| KUnit corten\* off×1 | ✅ 25/0/0 + 21/0/34 + 6/0/26 全绿零 Oops |
| =n 八对象（+m5t3 五件复核） | ✅ memory/mmap/migrate/rmap/swapfile/gup/oom_kill/x86-fault/sys/mempolicy/mremap/madvise/mprotect 全部 RC=0, nm 零 corten 符号 |
| checkpatch --strict | ✅ **0E / 0W / 3C**（2539 行; checks=嵌套深度注记） |
| guest run_mode_smoke | ✅ **26/26** + SMOKE-DRIVER PASS（契约件更新: 见 §3 判据变更注记, sha f45157cf→6d20288d） |
| guest JThreadBench | ✅ 2000×3 ×3 JVM rc=0 零 CFE |
| guest metis_eq | ✅ ×2 rc=0 + MODE marker + checksum 自洽（65073/2d383eee） |
| guest park 专项（mva1_probe） | ✅ S-4/S-1(MAPERR si_code 活体)/FRESH 复用 8M 读写回/CHUNK 无 VMA 拆分/复活窗内容语义/S-4b 全锚 |
| guest 池计数 | ✅ parks=6>0, hits=3>0, **park_unmap_fails==0**, dmesg corten-quiet |
| DEBUG_ATOMIC_SLEEP 变体 | ✅ 实跑三套件 24/0/1 + 55/0/0 + 30/0/2 全绿零 Oops（B1 类回归防线） |
| lockdep 变体 | ✅ 实跑三套件同上全绿; 3 条 WARNING 均为已登记签名（corten.c:865/812 = interlock flake 家族 + drain 注入设计路径）, 无新 lockdep 边 |

## 3. 契约件判据变更注记（任务书授权项）

- `t1c/corten_mode_smoke`: case **released-arena-no-pages** 以 `mincore()` 判 parked 窗无驻留页。A.1 后 parked 窗**无 VMA** → mincore 正确返回 -ENOMEM（区间读作"已 munmap"）→ 判据更新为 "ENOMEM=PASS"（源码 `cortenmm/bench/mode-smoke/corten_mode_smoke.c` 已注释注明 S-4）。sha **f45157cf → 6d20288d**。此变更即任务书预告的"maps 检查口径变化"同族（mincore 亦为 VMA 消费者, V-C 收口）。

## 4. 过程缺陷（全部闭环）

1. reactivate 先 register 后清 idle → INV tripwire 触发: 翻转顺序修正（idle 先清）。
2. zap 计数拆分 `folio_test_anon` 对无 rmap 页恒假 → MM_FILEPAGES/-1、MM_ANONPAGES/+1 漂移（KUnit mm 撕毁告警暴露）: 拆分改由窗口 vma-ness 主导。
3. harness mkvm 从不记账 total_vm → A.1 park 反记账下溢 → take 的 may_expand_vm 拒绝 → pool_limit 假失败: mkvm 补记账 + drop_vma 补反记账。
4. fork 复制臂 `a++` 笔误（应为 `a += PAGE_SIZE`）→ 走出 PT 页 GPF: 修复。
5. 首版级联在真链路复现非规范页表态 GPF（pmd_offset 基址未对齐 PUD 跨界 + 陈旧页表态）: 整体移除, 归口 B-2/V-D。

## 5. 留给 A.2a 的边界

- VMA-less 窗 rmap 锚缺位 → reclaim 不可 pick / GUP-slow EFAULT（A.2b carrier 恢复）。
- pud/pmd 上级表残留（每窗 2 页, mm 死亡回冲缺位）→ V-D exit walk。
- fork 无 VMA 臂为软复制（无 rmap/WIPEONFORK 语义未含; 白名单未放行该 RF 位）。
- `corten_arena_test_region_record` 等三处 KUnit 断言已按 S-4 更新, J3 oracle 的 parked 例外档随 V-C 双源渲染收口。

## 6. 证据文件

- `results/r07/mva1/kunit-on1.log, kunit-on2.log, kunit-off1.log`（=y 三套件 ×2/off）
- `results/r07/mva1/kunit-das.log`（DEBUG_ATOMIC_SLEEP 变体）、`kunit-lk2.log`（lockdep 变体）
- `results/r07/mva1/guest-gate-final.log`（guest 16/0: smoke 26/26 + probe 全锚 + 计数）
- `results/r07/mva1/checkpatch-mva1.txt`（0E/0W/3C）、`patches/r07-mva1.diff`（2643 行）
- `results/r07/mva1/bzImage-mva1-y`（#29, sha256 2442d785…）
