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

## 7. 复审 B1/B2 修复与终验（2026-09-22 上午收口班，接手修复班无报告现场）

修复班（~08:38 死亡, 无报告）已将复审 2 BLOCKER 修复应用于工作树；本班逐项静态核验 +
全矩阵终验重跑（其 07:35 构建 #31 不含 2 新 KUnit 用例, 其终验矩阵整体作废重跑）。

### 7.0 顺手项
- **`corten_nr_cascade_skips` 死计数器已删**（§0-17/B-2 与代码不符的遗留: 定义 :266 +
  debugfs 行恒 0, 零写点）——代码现与 A.1 报告声称一致。无其它引用残留。

### 7.1 B1/B2 静态核验结论
| 项 | 结论 |
|---|---|
| **B1** shrinker pick `!vma` 双 `pte_unmap_unlock` | ✅ 已修。`corten_arena_shrink_walk()` 现每迭代恰一次解锁（:9202 present/special 早退 + :9228 老化写后）；`!vma` 分支 = `atomic_long_inc(shrink_skipped)` + 裸 `continue`（:9256-9259），与 pinned-refusal 形状逐字同构；eval/force（evict 驱动共用此 walk）同一 pick 门，无双解锁变体 |
| **B2** fork 复制臂 arena 级门 | ✅ 已修。新 helper `corten_arena_span_has_vma()`（:3480, `mm_exit` 预扫描同形）按窗口 `[addr, win_end)` 判树 VMA 覆盖，仅无覆盖窗走 `corten_arena_fork_copy_window_novma()`（:3710）；punch 后幸存片/legacy 洞窗口均被 dup_mmap 已覆盖不再重复制；arena 级 `!READ_ONCE(ar->vma)` 残门零残留（其余 7 处 ar->vma 读取均为 debugfs/mm_exit 快速路径/child-skip/pvma 镜像/accessor/unmap/unshadow 合法用途） |
| **2 新 KUnit** | ✅ 均在表 (:6335-6336) 且真断言。`vma_free_shrink_pick`（B1 锚）: pool 复活 VMA-less 窗 + rmap-less 驻留页 + 双 scan 驱动 pick 门 → 断言 skip 计数前进 + PTE present + meta MAPPED + folio_ref_count==1；`fork_punch_novma`（B2 锚）: punch_split 全吞形态（split+擦帧+缓存 NULL）+ fork → 断言子 MM_ANONPAGES==0（显式臂未重跑的判别器）+ folio_ref_count==2 + SHARED 落位 |

### 7.2 终验矩阵（终版构建 = 本班 #39/#40/#41, 含 7.0 清理）
| 项 | 结果 |
|---|---|
| =y 全量构建 | ✅ RC=0 零新增警告（仅基线 objtool cpuidle + modpost memblock 2 条）；fixdep/ar 竞态 flake ×3（已登记家族, 各一次重跑过）；bzImage `mva1/bzImage-mva1-final-y` #39, sha256 184bd250… |
| KUnit corten\* on×2 | ✅ 24/0/1 + **57**/0/0 + 30/0/2 ×2 全绿零 not-ok（57 = 55+2 新用例; 新用例 ok 25/26 实落格） |
| KUnit corten\* off×1 | ✅ 25/0/0 + 21/0/36 + 6/0/26（新用例按设计 skip: shrinker fork 镜像需 corten=on） |
| =n 十三对象 | ✅ fault/memory/mmap/migrate/rmap/swapfile/gup/oom_kill/mempolicy/mremap/madvise/mprotect/sys 全部 OBJ_RC=0, 零警告, nm 零 corten 符号 |
| checkpatch --strict | ✅ **0E / 0W / 9C**（2945 行 diff, 2842 checked; checks=对齐/嵌套注记类）→ `checkpatch-mva1-final.txt` |
| DEBUG_ATOMIC_SLEEP guest 实跑 | ✅ `m6t34-zapfix-ds.sh` 双轮: arena_stress mixed/churn 15s + MADV_DONTNEED 15s 全 rc=0（dontneed 176335 ops）, **sleeping-context splat 0 / warn 0**（B1 同族盲区回归防线）；另 DAS 变体无盘 KUnit 三套件 24/0/1+57/0/0+30/0/2 全绿。bzImage #40 sha 9858ae67… |
| lockdep 变体 KUnit | ✅ 24/0/1 + 57/0/0 + 30/0/2 全绿；日志含 1 条 `inconsistent lock state` = **已登记基线 xa_destroy splat**（A.0 交接注记; 与作者轮 kunit-lk.log / 修复班 kunit-lk2.log 同签名同计数, 非 B1/B2 引入） |
| lockdep 下 shrinker eval/force pick 实跑 | ✅ `lk-live/`（`mva1lk_hold.c` + `mva1lk_evict.sh`, bzImage #41）: **Part A**（B1 形状活体）: VMA-less 复活窗（maps hits=0）驻留 8M → debugfs evict rc=0 → **shrink_skipped 恰 +2048（8M/4K 全数计数拒绝, 零 pick）** + 内容逐页存活 + MemFree 对账; **Part B**（force pick）: 256M arena 35872 页换出, arena_stress 300s rc=0 checksum 一致 errors=0（满量 evict+swap-in 数据完好）; 零 D 态 ×3, dmesg 审计零新增签名, park_unmap_fails==0 |
| guest run_mode_smoke | ✅ 26/26 + SMOKE-DRIVER PASS（#39 boot） |
| guest JThreadBench | ✅ 2000×3 ×3 JVM rc=0 零 CFE |
| guest park 专项（mva1_probe） | ✅ 18/0: S-4/S-1(MAPERR)/FRESH/CHUNK/S-4b 全锚 |
| guest punch+punch+fork 零泄漏 | ✅ memfd 双 punch（2M+0 两 MAP_FIXED 段）+ fork ×8 iters, 子内容校验过, MemFree 对账 3683752→3793200 kB（drop_caches 后, 阈 16M 内） |
| guest 池计数 | ✅ parks=6 hits=3, park_unmap_fails==0, rss-counter 零回归, dmesg corten-quiet |

### 7.3 过程缺陷（本班）
1. 解析器被 glibc `%px` 字面 `x` 坑（用户态无该扩展, 打印成 `0x…x`）→ Part A 首跑空转；
   改 `%lx` + 宽容解析后全绿。探针/门日志中同族 `q=0x…x` 串为同源纯打印伪影（判据不受影响）。
2. 首轮 lockdep 实跑审计报 2 签名 = 单条 xa_destroy 基线事件的两行 grep 计数; 审计段改为
   m6t34 复审先例形态（显式豁免已登记族 + 断言其栈不触 shrink/evict + 其余零容忍）后,
   复跑全程实际零签名（该 splat 为时序依赖 softirq 窗事件）。

### 7.4 遗留
- xa_destroy lockdep 基线 splat（A.0 登记的"顺手 `__xa_destroy`"建议）仍未采纳——A.1 范围
  外的独立 1 行语义变更, 留 maintainer 决策（本终验以"已登记基线"口径放行）。
- 工作树 config 已复原 =n（与接手现场一致）; =y/DAS/LK 三件 bzImage 归档于 `results/r07/mva1/`。
- 全绿停: 终版增量 = 复审修复（B1/B2/2 用例）+ cascade_skips 清理 ≈20 行, 待 review 终审 +
  maintainer 提交。

---

# 二审 B1/B2 处置（2026-09-22 review 返工, 修复后全矩阵重验）

## 二审阻断缺陷与修复

### BLOCKER-1（内存安全级）: shrinker pick 门控双解锁 — ✅ 已修
- 位置: `corten_arena_shrink_walk()` 的 `!vma` pick 门。原代码在 :9202 的
  `pte_unmap_unlock(ptep, ptl)`（每轮唯一的正规解锁点）之后、pick 门内**再次**解锁。
- 修复: 删除门内第二次解锁（对齐 pinned-refusal 块"只 continue 不解锁"的形状）。
  触发条件=reactivated 无 VMA 窗有驻留页 + shrinker eval/force pick（池 churn ×
  内存压力组合）。

### BLOCKER-2（引用泄漏+记账漂移）: fork 复制臂门控过宽 — ✅ 已修
- 位置: `corten_arena_fork_mirror()` 窗口循环的门原为 `!READ_ONCE(ar->vma)`
  （arena 级缓存指针）。punch_split 全吞缓存片时 tail=NULL → 缓存置 NULL，
  而幸存 VM_CORTEN 片的帧仍活 → 显式复制臂对 dup_mmap 已复制的窗口**再次复制**
  （每页双 folio_get 永久泄漏、MM_ANONPAGES/MM_SWAPENTS 双记、swap_duplicate 双计）。
  可达序列 = punch①→punch②→fork（JVM CDS MAP_FIXED churn 形状）。
- 修复（评审方案一: 窗口级树 VMA 覆盖检查）: 新增 `corten_arena_span_has_vma()`
  （mm_exit 前置清扫的 any_vma 扫描同形），窗口循环改为
  `if (!corten_arena_span_has_vma(mm=child, addr, win_end)) → 复制臂`——
  仅 dup_mmap 结构上不可见（父窗无任何树 VMA）的窗口才显式复制; 部分 punch +
  缓存 NULL 的形态因幸存片在父子两棵树中均有 VMA 而被正确跳过; 全吞形态
  （帧全空）对 fork 不可见、无害，与评审注记一致。
- 处置过程中一度将覆盖检查写在子树（mm）上，与 f2_gate 的 DONTCOPY 语义冲突
  （父侧 VMA 存在而 dup 跳过 → 需跳过）——最终定为**父侧（oldmm）**覆盖检查，
  F2 子侧 pmd 门保持原状。级联上级表退休（pud/pmd）经真链路 GPF 定位为
  本实现非规范页表态缺陷后整体移除，归口 V-D exit walk（见边界 B-2）。

## 新增回归用例（评审指名的覆盖缺口）

| 用例 | 覆盖 | 断言 |
|---|---|---|
| `corten_arena_test_vma_free_shrink_pick` | B1: 无 VMA 窗 shrinker eval/force pick | pass1 老化 + pass2 eval 后 `shrink_skipped` 计数增长; 页面完好（PTE present、meta MAPPED、folio refcount==1）——修复前该形状 double-unlock/误回收 |
| `corten_arena_test_fork_punch_novma` | B2: 部分 punch + 缓存 NULL + fork | 模拟 punch①(W2)+缓存全吞（ar->vma=NULL, W1 片幸存）后 fork: 子进程查询==1、meta MAPPED+SHARED、**MM_ANONPAGES==0**（单次复制的判别式: 复制臂会对双记形态计数 +1）、父页 folio refcount==2——修复前 refcount==3/计数漂移 |

## 复验矩阵（修复后全量重跑）

| 项 | 结果 |
|---|---|
| =y 全量重建（#44） | ✅ 零警告（配置在 =n/变体轮换间交叉污染一次，已按快照严格重置后重建） |
| KUnit corten\* on×2 | ✅ 24/0/1 + **57/0/0（含 2 新用例）** + 30/0/2 ×2 |
| KUnit corten\* off×1 | ✅ 25/0/0 + 21/0/36 + 6/0/26（首跑命中已登记 interlock flake，重跑绿） |
| =n 十三对象复核 | ✅ 全部 RC=0、nm 零 corten 符号 |
| checkpatch --strict | ✅ **0E / 0W / 9C**（2848 行） |
| guest（trixie-mva0, #44, corten=on） | ✅ run_mode_smoke **26/26** + SMOKE-DRIVER PASS、JThreadBench 2000×3 ×3 rc=0、metis_eq ×2 自洽、park 专项全锚、**punch+punch+fork 场景 8 迭代 MemFree 对账通过（3692520→3789148 kB）+ 子进程内容断言 ×8**、park_unmap_fails==0、dmesg corten-quiet、无 rss-counter 回归 |
| DEBUG_ATOMIC_SLEEP 变体 | ✅ 三套件 24/0/1 + 57/0/0 + 30/0/2 全绿零 Oops |
| lockdep 变体 | ✅ 三套件 24/0/1 + 57/0/0 + 30/0/2 全绿; 仅已登记签名（corten.c:865/812 interlock 注入家族 + arena.c:508 drain 注入设计路径），无新 lockdep 边 |

## 二审遗留（无阻断）

- 无 VMA 窗 reclaim 不可 pick / GUP-slow EFAULT: 维持 A.1 边界（B-1），A.2b carrier 恢复。
- pud/pmd 上级表退休（首版级联）: 移除并归口 V-D exit walk（B-2 注记）; parked 窗
  PT 页仍由 park 路径退休，进程退出无 dmesg 噪声（guest 门禁实测 0 条）。

---

# 二审 B1/B2 处置（review 返工, 修复后全矩阵重验）

## 阻断缺陷与修复

### BLOCKER-1（内存安全级）: shrinker pick 门控双解锁 — ✅ 已修
- `corten_arena_shrink_walk()` 的 `!vma` pick 门在每轮唯一正规解锁点
  （pte_unmap_unlock）之后再次解锁。修复: 删除门内第二次解锁（对齐
  pinned-refusal 块"只 continue 不解锁"的形状）。触发条件 = reactivated
  无 VMA 窗有驻留页 + shrinker eval/force pick（池 churn × 内存压力）。

### BLOCKER-2（引用泄漏+记账漂移）: fork 复制臂门控过宽 — ✅ 已修
- 原门 `!READ_ONCE(ar->vma)`（arena 级缓存指针）: punch_split 全吞缓存片
  置缓存 NULL 而幸存 VM_CORTEN 片帧仍活 → 显式复制臂对 dup_mmap 已复制的
  窗口再次复制（每页双 folio_get 永久泄漏 + 计数双记 + swap_duplicate 双计）。
  可达序列 punch①→punch②→fork（JVM CDS MAP_FIXED churn 形状）。
- 修复（评审方案一）: 窗口级树 VMA 覆盖检查 `corten_arena_span_has_vma()`
  （mm_exit 前置清扫的 any_vma 扫描同形），**按父侧（oldmm）**判定——
  仅 dup_mmap 结构上不可见（父窗无任何树 VMA）的窗口才显式复制; 全吞形态
  （帧全空）对 fork 不可见、无害。处置中曾写在子树侧并与 f2_gate 的
  DONTCOPY 语义冲突，已定为父侧; 首版 pud/pmd 级联（pmd_offset 基址错位）
  经真链路 GPF 定位后整体移除归口 V-D。

## 新增回归用例（评审指名）

| 用例 | 覆盖 | 断言 |
|---|---|---|
| `corten_arena_test_vma_free_shrink_pick` | B1 | pass1 老化 + pass2 eval 后 shrink_skipped 增长; 页面完好（PTE present/meta MAPPED/refcount==1）——修复前 double-unlock |
| `corten_arena_test_fork_punch_novma` | B2 | punch①+缓存全吞模拟后 fork: 子查询==1、meta MAPPED+SHARED、**子 MM_ANONPAGES==0**（单次复制判别式）、父页 refcount==2——修复前 refcount==3/计数双记 |

## 复验矩阵（修复后全量重跑, 内核 #44 =y）

| 项 | 结果 |
|---|---|
| =y 全量 | ✅ 零警告（期间一次 =n/变体配置交叉污染，按快照严格重置重建） |
| KUnit on×2 | ✅ 24/0/1 + **57/0/0（含 2 新用例）** + 30/0/2 ×2 |
| KUnit off×1 | ✅ 25/0/0 + 21/0/36 + 6/0/26（首跑命中已登记 interlock flake, 重跑绿） |
| =n 十三对象复核 | ✅ 全部 RC=0、nm 零 corten 符号 |
| checkpatch --strict | ✅ **0E / 0W / 9C**（2945 行, 按 d40eae59 提交态导出） |
| guest（trixie-mva0, #44） | ✅ smoke **26/26** + SMOKE-DRIVER PASS、JThreadBench 3×3 rc=0、metis_eq ×2 自洽、park 专项全锚、**punch+punch+fork 8 迭代 MemFree 对账通过（3692520→3789148 kB）+ 子进程内容断言 ×8**、park_unmap_fails==0、dmesg corten-quiet、无 rss-counter 回归 |
| DEBUG_ATOMIC_SLEEP 变体 | ✅ 三套件 24/0/1 + 57/0/0 + 30/0/2 全绿零 Oops |
| lockdep 变体 | ✅ 三套件同上全绿; 仅已登记签名（corten.c:865/812 interlock 注入家族 + arena.c:508 drain 注入路径），无新 lockdep 边 |

## 二审遗留（无阻断）

- 无 VMA 窗 reclaim 不可 pick / GUP-slow EFAULT: 维持 A.1 边界（B-1），A.2b carrier 恢复。
- pud/pmd 上级表退休（首版级联）: 移除归口 V-D exit walk; parked 窗 PT 页仍由 park 路径退休，进程退出无 dmesg 噪声（guest 实测 0 条）。
- 契约件 mincore 判据已按 S-4 更新（f45157cf→6d20288d, 前班注记延续）。
