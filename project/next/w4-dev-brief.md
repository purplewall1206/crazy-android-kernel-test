# W-4 开发任务书 · MV2 入场扫入（resident VMA→region 迁移事务机 = W-3c）

授权: MV2_FULL_REMOVAL_SPEC.md W-4 行 + MV2_REMAINING_SPECS.md W-4 节 + STATE D28/D29。
基座: worktree /home/ppw/linux-6.18-mva @ e6afdde09eda（= 主树 d7bd0dd9b5c9 内核代码
逐字节一致, W-3fix 已含）。**只在该 worktree 开发, 不 commit**（主会话验证后入库）。
规格依据: next/w3-dev-report.md §4.2（W-3c 设计稿, 权威）。

## 目标

prctl MODE ENTER 时对当前 mm 的全部既有 VMA 逐条判定迁移（一次性事务,
mmap_write 持有全程——EXIT 同级大锁窗, fault/GUP/ttu 全部被挡, 无并发面）:

- 匿名私有 VMA → anon region（declare_locked novma + 逐 present PTE 元数据
  补记 + rmap 锚从 anon_vma 换到 novma 家族 = W-2 的 folio_add/remove_anon_
  rmap_novma 形状）。
- file 私有 VMA → FILE region（corten_arena_file_attach + declare(file) 既有臂;
  驻留页元数据 FILE_MAPPED + rfile/rpoff 折算——pagecache 锚天然在, 无 folio 手术）。
- file 共享（VM_SHARED）/ 特殊（arch_vma_name）/ 主栈 GROWSDOWN / 窗口域 / 
  VM_HUGETLB|VM_IO|VM_PFNMAP|VM_MIXEDMAP / VM_DONTEXPAND? 不——只排 hugetlb/io/
  pfnmap/mixedmap/locked? → **结构性保留**（跳过+计数, wl 桶披露）。
  主栈跳过 = W-3b 移交维持; VM_LOCKED/VM_DONTEXPAND 等普通标志匿名 VMA **照收**
  （region 记录 rflags 已有位）。

## 核心设计（裁决, 勿翻案）

1. **入口**: corten_arena_mode_enter() 内、MODE 位置位后、mmap_write 之下调用
   corten_arena_mode_sweep(mm, state)。失败不回滚 MODE（fail-open: 未迁的 VMA
   留 legacy, 计数披露）。
2. **两相事务（无 unwind 需求）**: 先全部可失败步骤（fill_upper + meta array
   ensure + declare_locked——任何失败 → 该 VMA 放弃迁移留 legacy, 已 declare 的
   span 立即 release 回卷）, 后不可失败步骤（锁内手术 + 树摘除）。
3. **file 形先行**（w3 报告建议: pagecache 锚风险低一档）, anon 形其次。
4. **anon folio 手术（关键创新, 按 W-1.a 家族语义）**: 驻留页当前 mapping=
   anon_vma、_mapcount=0。目标: mapping=NULL、mapcount 不变、swapbacked 保持、
   AnonExclusive 保持。手术分两步锁形（W1.e1 驱动的 picks 模式）:
   - ptl 下（desc 写锁内）: 读 PTE, present 非零页 → folio_get + 记 (addr, folio)
     到 picks 列表; PTE 本身**不动**。
   - 放锁后: folio_lock(folio)（可睡, 合法——锁已放）→ 复跑 corten_lock_range+
     ptl 验证 PTE 仍是同页（mmap_write 下无合法竞态, 复验是防御性形式）→
     `anon_vma = folio_anon_vma(folio); folio->mapping = NULL;`（WRITE_ONCE）+
     anon_vma_put(anon_vma)（解除 mapping 持有的引用）→ meta 槽置 MAPPED+perm
     （corten_mark）→ 放 folio_lock, 放 txn。
   - 零页 PTE（read-fault 未写匿名）: 不碰 folio, meta 置 FRESH 语义（Invalid+
     perm, fault 门自会合成）——PTE 保留零页映射即可? **否**——保留零页 PTE 而
     meta 为 Invalid, GUP follow 腿会答零页 ✓ fault 门不触发 ✓ 可接受（登记）。
   - swap entry PTE: 元数据 SWAPPED + W1.f 条目编码直抄（pte_to_swp_entry 折算
     type/offset）, PTE 不动。
5. **file 驻留页**: 无 folio 手术。meta FILE_MAPPED + rpoff = vma pgoff + 
   (addr - vma_start)>>PAGE 折算; PTE 不动。**B.2 失效门与 W1.b registry 已备**。
6. **树摘除**: 手术成功后逐 VMA: unlink_file_vma / unlink_anon_vmas（anon_vma
   链随末引用消亡——页的 mapping 已 NULL, 不欠）→ vma_iter_clear(&vmi, start,
   end) → vm_area_free。total_vm/data_vm 记账: declare take 与 VMA refund 恰好
   对消（核对 vm_stat_account 双侧）。RLIMIT_AS 不变（total_vm 不变）。
7. **declare 的 may/perm**: may=corten_region_may_full(), perm=从 vma->vm_flags
   折算（VM_READ/WRITE/EXEC → CORTEN_PERM_*）; rflags=corten_vma_rflags(vma)
   （既有 helper, :1077 附近）。
8. **计数**: sweep_anon_adopts / sweep_file_adopts / sweep_skip_{stack,special,
   shared,window,other} / sweep_pages_resident（手术页数）入 arena_stats +
   测试访问器。
9. **=n 折叠**: sweep 在 mode_enter 内（已 CONFIG 门控）, 无新外露符号。
10. **J2 影响**: 迁移后树内只剩 skip 桶——whitelist_scan 的 BRK/FILE/ANON 桶
    自然收缩（谓词不动, W-3 §1.6 同一裁定）。

## KUnit 锚（corten_arena_test.c, ~250 行）

- `sweep_anon_resident`: mkvm(RW) + 手动 fault 两页（fill_window 形或
  corten_arena_handle_mm_fault）+ mode_enter(带 sweep) → 断言: 树内该 span 无
  VMA、region 在册、两页 meta MAPPED+RW、内容可读回（页还在）、folio 家族判定
  corten_folio_is_arena_anon==true、mapcount==1。
- `sweep_file_resident`: shmem file + mkvm(private, file) + fault 读一页 +
  mode_enter → FILE region 在册、meta FILE_MAPPED、rpoff 折算正确、页可读。
- `sweep_skip_taxonomy`: 主栈 mkvm(GROWSDOWN) + special（无法直接造——用
  VM_SHARED file）+ shared → 三桶计数、不迁。
- `sweep_empty_anon`: 纯 declare 形（无驻留页）→ 全迁, 树空。
- `sweep_swapent`: 换出页 VMA（造形难可 skip 登记, 若驱动 evict 可在 KUnit 用
  corten_arena_test 既有 swap 工具）。
- 既有 mode_enter 用例全部不红（sweep 对空树 no-op）。

## 红线

- INV6: PTE 写仅经事务; 手术的 mapping=NULL 是 folio 字段不是 PTE。
- mmap_write 全程; 不得取 mmap_read、不得睡在 desc/ptl 临界区内（folio_lock
  只在放锁后, W1.e1 同款）。
- 每步收口可运行: 失败路径全部 fail-open。
- 零 binfmt/内核外改动; 只动 mm/corten_arena.c + .h + mm/corten_arena_test.c
  （include/linux/corten_arena.h 若需测试访问器）。
- checkpatch --strict 0E/0W。

## 验证（自验到 guest 门为止的主会话侧）

make -j8 零新增警告; 三套件 on1/on2/off（filter_glob=corten*）; =n 十四对象;
checkpatch。报告落 next/w4-dev-report.md（含红绿记录与遗留）。guest 门留主会话。
