# W2 开发报告 · MV2 W-2: carrier 消灭（匿名锚退役，fork/COW/GUP 元数据化）

- 片: MV2 W-2（MV2_FULL_REMOVAL_SPEC.md 切片表；规格依据 W1_NATIVE_RMAP_SPEC.md §3.1/§3.5/§6.2 + MV_VMA_FREE_SPEC.md §2.3/§3.3.2）
- 基线: mv-a0 @ a9913afd1af0（W-1 全系列完成）· 未 commit · worktree /home/ppw/linux-6.18-mva
- diff: /home/ppw/cortenmm/patches/r07-w2.diff · 8 文件 **+1384/−977**
  （生产：mm/corten_arena.c、mm/corten_arena.h、include/linux/corten_arena.h、mm/rmap.c +37、mm/gup.c +40 改形、fs/proc/task_mmu.c、mm/memory.c 仅注释；测试：mm/corten_arena_test.c +492 行改形）
- 验证: make -j8 RC=0 零新增警告（仅基线 objtool cpuidle 噪声）· 三套件 KUnit
  **on1 ×3 / on2 全绿（24/0/1 · 104/0/0 · 34/0/5 = W1.f2 基线逐套件同数）** ·
  **off1 全绿（25/0/0 · 24/0/80 · 7/0/32）** · =n 十四对象 RC0 零警告 nm 零 corten 符号 ·
  checkpatch --strict **0E/0W/0C（3658 行）** · 终镜像 bzImage #178+ 已出 · guest 门留主会话
- 日志: /home/ppw/cortenmm/results/r07/mva2/（kunit-on1/on2/off1、build-n、build-y）

---

## 0. 方案判定（task 第一指令）：**Y（删壳）+ 匿名锚"半翻"**，X 否决

### 0.1 X（子侧不复制 PTE，首 fault 走原生路径）——**否决，对匿名页不无损**

匿名窗口页既无 swap entry 也无 pagecache 挂点：子侧槽位若留空，首个访问没有任何
"原生"来源可 fault（swap-in 需要 entry，file read 需要 (mapping,index)，两者匿名页
都没有）。要让 X 成立必须发明跨 mm 的"父侧驻留"编码（子槽指向父 mm+addr），那是
一条新的 lazy-dup 状态机——比被替换的 PTE 拷贝贵得多，且父进程 exit 的并发回收面
无法闭合。file 页确实可以 lazy-refault（pagecache 天然在），但拷贝臂本来就要为匿名
页走查同一张 PT——lazy 化 file 槽不减少任何 vma 依赖，纯加 churn。结论：**X 在
匿名族上语义不无损、在 file 族上无收益，整体否决**。

### 0.2 Y（carrier 降为纯元数据锚）——**采纳，且实码证明"W1.a-e 已拆完全部职责，W-2 只剩删壳"**

以实码逐点核对 carrier 的每个消费面，结论是 W-1 系列已经把 vma 依赖拆完：

| carrier 职责 | W-1 后状态 | W-2 动作 |
|---|---|---|
| anon rmap mapcount（order-0 数值） | W1.a wrapper 与 vma 形逐位等价（F1），OQ-W1-6 接口层已预答 | 翻转调用点（R1/R2/R5/R6/R7/R8） |
| file rmap | W1.c 已 novma | 无 |
| file i_mmap / 失效枚举 | W1.b 注册表 | 无 |
| ttu file 路由 / 匿名守卫 | W1.d / W1.e2 | 无 |
| swap-out 地址再发现 | W1.e 驱动自带地址 | 摘 `!vma` pick 门 + R8 + flush 形 |
| fault/protect/zap/unuse 的 vma 依赖 | **V-A.1 起 `if (vma)` 双臂并存**（perm_pgprot_pure、novma flush、NULL-tolerant zap/protect/prealloc） | anchor 置 NULL 即自动走 vma-less 臂 |
| fork 的 PTE 复制 | 依赖 copy_page_range(carrier 对) | corten 自有窗口拷贝臂（本片主件） |
| GUP-slow 应答 | 依赖 carrier 作 vma ctx | metadata 判定 + follow/fault 臂（本片主件） |
| 壳本身（vm_area_alloc + anon_vma + 生命周期） | —— | 删除 |

"小结构"最终就是 region record 本身（ar 已有 start/end/prot/rclass/rfile/rpoff），
carrier 的 vma 载荷没有一项不可替代：flags→perm 派生、anon_vma→随翻转退役、
vm_file→rfile、写标记→随 copy_page_range 消亡。

### 0.3 半翻（关键收敛决策）：shadow 域保持锚定，carrier 域全 vma-less

fault 臂的 anon rmap 翻转采用 `vma ? folio_add_new_anon_rmap(...) :
folio_add_anon_rmap_novma(...)` 而非无条件 novma——**shadow（targeted DECLARE 树
VMA）的匿名 folio 保持锚定**。理由：F7 是硬的——一旦 shadow 页也 unanchor
（mapping==NULL），dup_mmap 的 copy_present_ptes 对它们走 file 臂，子侧 rss 记错族
（fork 对拍必炸），连带要把 shadow 的 piece 结构（DONTCOPY/WIPEONFORK 逐 piece
保真）、copy_page_range 的 VM_CORTEN 门一起重写——那是 W-4（树清零）的活，不是
carrier 消灭的活。shadow VMA 是 MODE 入场前的用户 mmap 转形，**不是 MODE mm 的
vma 分配**，D28 判据不受影响；hwpoison 探针/mlock 判定/ttu backstop 对 shadow 继续
verbatim 工作（比 unanchor 后的"结构性排除+计数"披露更强）。W-4 树清零时
`if (vma)` 臂自然翻成无条件 novma，台阶干净。

### 0.4 D28 验收

`vm_area_alloc` 在 mm/corten_arena.c 剩 **0 处**（原 carrier_alloc 整体删除）；MODE
mm 的 vma 分配恒 0，detached 含在内。`corten_nr_carriers` 计数与 debugfs `carriers`
行退役（渲染行位由披露计数 `declare_notes` 接替）。

---

## 1. 实施内容（对照 task 清单）

### 1.1 carrier 生命周期删除

- `corten_arena_carrier_alloc/carrier_free` 删除；`corten_arena_anchor_vma` 退化为
  `READ_ONCE(ar->vma)`（auto 恒 NULL）。DECLARE novma 臂不再分配锚对象
  （`arena->auto_shape = true`），unwind 不再释放；RELEASE/mm_exit/mode_exit 的
  carrier free 臂删除；park→reactivate 从"无条件重武装"退化为纯 metadata 翻转。
- fork_register_child：子 carrier 分配 + anon_vma_fork 删除，镜像 = 纯 record 拷贝
  （rfile/rpoff/auto_shape）。
- region_file_teardown/disarm：carrier vm_file 复位臂删除（rfile+fput+registry
  unlink 保留）；INV-MV3 record check 的 carrier 配对项删除（rfile 单侧保留）。
- `corten_nr_carriers` 删除；debugfs `carriers` 行退役。RELEASE 手退（total_vm）的
  判别键从 carrier 换成新增 `ar->auto_shape`（精确区分 auto/shadow 的记账来源）。
- perf_event_mmap(carrier)（auto DECLARE 的 C25 保真 PERF note）随之消亡——登记为
  披露项，debugfs `declare_notes` 计数可观测；/proc/maps 双源渲染不受影响。

### 1.2 rmap 翻转（spec §3.1 R1/R2/R5/R6/R7/R8）

- R1 map_anon / R2 COW 新页 / R5 file-COW 私有拷贝 / R6 swap-in / unuse cache-pull：
  `if (vma) 锚定调用 else folio_add_anon_rmap_novma()`；R6/R5 的非 exclusive 形态
  补 `ClearPageAnonExclusive`（novma add 无 flags 参数，读侧语义对齐 rmap_flags）。
- R7 zap_release：**家族判别器从 folio_test_anon 换成 folio 自身状态**
  （新 helper `corten_folio_is_filemap()` = `!anon && !swapcache && folio_mapping`）——
  unanchored anon 无 mapping，`folio_test_anon` 会把它误判成 file（spec R7 预告的
  "分族依据改为 meta/region class"的 folio 侧等价实现；zap 走查"按 PTE 内容判定"
  的哲学不变）。file 臂 novma（W1.c 原样），anon 臂 `vma ? 锚定 : novma`。
- COW（R2/R3）：`old_is_file = corten_folio_is_filemap(old)`，移除臂三分
  （file novma / shadow 锚定 / auto novma）。reuse 判定 `mapcount==1` 对 auto 页从
  此是真实账（novma 计数）。
- R8 swap-out 事务：`vma ? folio_remove_rmap_ptes : novma`；签名把 `mm` 从
  `vma->vm_mm` 解耦成显式参数（vma 可 NULL）；flush 形：shadow 保
  flush_cache_range/flush_tlb_range，auto 用 ptep_get_and_clear +
  corten_tlb_flush_page_novma（W1.e2 登记的"W-2 重开"项兑现）。
- W1.e2 的 `!vma` pick 门删除；驱动 mlock 判定仅对 shadow 锚读取（carrier 的
  VM_LOCKED 位从结构上不可置——mlock(2) 走树，对 non-tree vma 无路径——实测确认
  为死位，auto 无需替代判定）。ttu_file_one 同形（`!vma` WARN 臂退役）。
- 驱动 pick 门：`folio_test_anon()` 项删除（auto 页 unanchor 后恒假），保留
  swapbacked+order-0+unpinned（meta MAPPED+!SHARED 已锁 anon 窗口页族）。

### 1.3 fork 的 corten 自有窗口拷贝臂（spec W-2 "fork_copy_ptes 纯 metadata 化"）

`corten_arena_fork_copy_ptes(dst_mm, src_mm, addr, win_end)`——替代
`copy_page_range(child_carrier, parent_carrier)`，仅 auto 形（auto_shape）调用；
shadow 继续走 dup_mmap 原生拷贝（0.3 半翻的直接收益）。逐槽语义镜像
copy_present_ptes/copy_nonpresent_pte 的 order-0 形：

- present 非特殊：父 wrprotect（ptep_set_wrprotect）+ 子继承 RO 翻译 +
  folio_get + novma dup（mapcount+1、清 exclusive）+ 子 MM_ANONPAGES+1；
  file 页（含 shmem）= 裸 mapcount bump + mm_counter_file 族（W1.c 数值等价）。
- **GUP-pinned anon**：copy_present_page 答案——子得私有拷贝
  （folio_copy + novma add + ANONPAGES+1），父保持可写+exclusive 原页——
  fork_mark_window 的 pinned-private 判定（可写+exclusive 幸存 → 不标 SHARED）
  原样识别，INV7 无漂移。拷贝页的写位跟随父 PTE 编码（比上游 vma 级
  maybe_mkwrite 更保守，RO 父的首写经 COW reuse 自愈）。
- present 特殊（共享零页）：PTE 裸拷贝（mkold），无 rmap 无 COW。
- swap entry：swap_duplicate + mmlist + 双侧清 exclusive 位（copy_nonpresent_pte
  原形）+ 子 MM_SWAPENTS+1；migration/device/markers 结构性排除于窗口。
- 锁形 = copy_pte_range 同形：双 mm 写锁（fork_commit 断言）、子 PT 锁先父锁
  nested（SINGLE_DEPTH_NESTING）、父 write_protect_seq 写段 + PROTECTION_PAGE
  notifier 窗（copy_page_range 的 is_cow 包络对齐）；子 PT 页按父表存在性
  pte_alloc——punched hole 免费跳过，④-4 子侧重放臂（child-PMD 门、MAPPED 槽
  页身份取子 PTE、SWAPPED 槽 entry 比对）零改动衔接。
- mm/rmap.c 新增 order-0 novma dup 双 wrapper（`folio_dup_anon_rmap_novma` /
  `folio_dup_file_rmap_novma`）：镜像 __folio_try_dup_anon_rmap / __folio_dup_file_rmap
  的 order-0 分支，去掉 vma 形参与 folio_test_anon 家族 WARN（unanchor folio 无
  mapping 可查）；pin 的 share-vs-copy 裁决归调用方；无 stat（dup 与上游同）。
- **无任何 memory.c 行为改动**：copy_page_range 对 shadow 照旧；auto 窗口不在树
  上，dup_mmap 循环根本看不见它们，无需门。

### 1.4 GUP-slow 窗口臂（"corten_gup_probe carrier 应答"的替换）

`corten_gup_window(mm, addr, gup_flags, pagep)`：__get_user_pages 的 vma-lookup
块前置（gup.c 唯一新增钩；原 corten_gup_probe 两处调用及其定义删除，
gup_vma_lookup 恢复上游形）。三腿：

1. **probe**（check_vma_flags 仿真）：记录 perm = 访问位、region class =
   anon/file 分裂（FOLL_ANON 必须 miss FILE region）、私映射契约 = FOLL_FORCE
   cow 条款；parked/储备/洞 = 响亮 -EFAULT（find_vma 自己的 errno，J1 恒零）；
   树锚 arena 与 implant = 返回 1 交还 legacy 走查。
2. **follow**：follow_page_pte 的 order-0 镜像——present/protnone 门、
   can_follow_write 仿真（FOLL_FORCE 仅对 exclusive 页放行；PG_anon_exclusive
   只在 anon 页上存在，上游 PageAnon() 项隐含成立）、gup_must_unshare 镜像
   （PIN + anchored anon + 非 Exclusive → -EMLINK）、try_grab_folio 抓页、
   FOLL_PIN 的 arch_make_folio_accessible、FOLL_TOUCH 记账。零页特殊形回零页
   （上游同判）；其余特殊形（PFN 族，窗口结构性排除）响亮拒绝。
3. **faultin**：直调 `__corten_arena_handle_mm_fault(mm, NULL, ...)`（慢钩本体
   重构为 (mm, vma) 双参，vma=NULL 即 V-A.1 vma-less 语义；route 从不掉 mmap
   锁、从不返回 RETRY（P2-6）），follow/fault 循环 8 轮封顶，错误映射 =
   vm_fault_to_errno 同族（OOM/EHWPOISON/EFAULT）。

调用点（gup.c __get_user_pages）：`ret != 1 → 0 填页 goto next_page / 负值出栈`；
`== 1` 落回 find_vma。实测语义保持：process_vm 四态（活窗读/PROT_NONE 拒/parked
拒/写往返）行为等价——PROT_NONE 从 perm 仿真更早拒绝（原经 fault+ACCERR，终态同）。
KUnit `mvc_gup_probe` 锚整体重写为逐腿断言（读答→零页、写答→真实 exclusive 页、
parked/洞/-EFAULT、implant/窗外地=1、FOLL_ANON×FILE=-EFAULT、计数器增量精确）。

### 1.5 /proc 双源渲染去 carrier（fs/proc/task_mmu.c）

- m_start 的行应答返回 NULL（.show 的 row 臂从 record 渲染，carrier 仅是旧 ctx）；
  query_merge_corten_row / query_matching_vma 行应答同（row_hit 旗标照旧）。
- `corten_row_flags(row)`/`corten_row_file(row)`：record 派生的 flag 字与回补文件；
  do_procmap_query 的 vma_start/end/flags/page_size/offset/dev/ino/name/build_id
  全部 row-aware（build_id 越过 teardown 需要 get_file 引用，取 rfile）。
  show_smap 的 VmFlags 行改 `show_smap_vma_flags_word()`（word 参数化，行侧传 0
  pad）；smaps/pagemap 走查器的页识别去 vm_normal_page（PTE-direct：present 非
  special → pte_page；PFNMAP/MIXEDMAP 窗口内结构性不存在，数值同判），
  anon/file 分族用 `corten_folio_is_filemap`（folio_test_anon 对 unanchor 页失效）。
- window 流过滤器从 `carrier 存在` 改为 `!ar->vma`（auto=窗口流全部观众）。

### 1.6 KUnit 锚同步（全量保持对拍/file_lifecycle/J4 矩阵）

- `carrier_of`/`test_carriers` 助手退役 → `corten_arena_test_region_of`（回 record）；
  file_lifecycle/file_fork_mirror 的 carrier vm_file/vm_pgoff 断言改 record
  （rfile/rpoff 断言原本就在，删重复件）；teardown/disarm/registry 形状不变。
- zap/i_mmap backstop 的正样本改"合成 VM_CORTEN 树 VMA"（shadow-piece 形，
  mkvm+unmap 自清理）——carrier 退场后 backstop 的真实观众。
- `carrier_vma` 用例改写为 W-2 形：断言 **auto arena 零 vma 分配**（region 存在、
  auto_shape、vma NULL、不在树、total_vm 记账/退款对称）——即 D28 的 KUnit 面。
- `vma_free_shrink_pick`/`carrier_shrink_pick`：pick 放行形保留（W1.e2 门已摘），
  reactivate 断言改 auto_shape；`fork_redeclare_content` 成为主拷贝臂的回归锚
  （逐页 checksum + SHARED + 子侧 PT present）。
- `mvc_gup_probe` 重写（1.4）；`mvc_smaps_pagemap` 的注页改经窗口臂；
  exit_lifecycle/swap 手工腿的 rmap 移除改 novma wrapper。
- 既有 fork 对拍（file_fork_mirror/file_fork_cow/file_fork_pinned 的
  mapcount/ref/rss 族断言）、file_lifecycle 引用账、J4 矩阵**全量保持且全绿**。

---

## 2. 红线核对

| 红线 | 实测 |
|---|---|
| INV6（窗口 PTE 写必经事务） | diff 内新增 PTE 写点全部集中在 fork 拷贝臂（set_pte_at×3 形 + ptep_set_wrprotect）——dup_mmap 冻结包络（双 mm 写锁 + write_protect_seq 写段 + notifier 窗），与被替换的 copy_page_range 同包络同白名单地位（DEV-14 家族）；上游文件（gup.c/memory.c/rmap.c）零新增窗口 PTE 写；机器扫描 `set_pte*|ptep_*` 新增 6 行全部在拷贝臂 |
| D28 | corten_arena.c `vm_area_alloc` 0 处；`KUNIT_EXPECT` 面断言 auto arena 的 vma==NULL/auto_shape |
| =n/off 折叠（INV9） | =n 14 对象 RC0 零警告 nm 零 corten 符号；corten=off 三套件 25/0/0 · 24/0/80 · 7/0/32 全绿；gup 窗口臂 off 形返回 1（legacy 走查接管） |
| DEV-13 锁序 | 拷贝臂 dst 锁先 src 锁 nested（copy_pte_range 同形）；无新锁类 |
| 不 commit | HEAD 仍 a9913afd1af0，交付 = 工作区 + r07-w2.diff |

---

## 3. 验证结果（无盘 qemu，mva2-verify.sh；日志 results/r07/mva2/）

| 门 | 结果 |
|---|---|
| make -j8（=y） | RC=0，触碰文件零警告（全量仅基线 objtool cpuidle 一条） |
| KUnit on1 | **corten 24/0/1 · corten_arena 104/0/0 · corten_fault 34/0/5**（= W1.f2 基线同数，零用例增减） |
| KUnit on2（flake 判定） | 同数全绿 |
| KUnit off1（corten=off） | **25/0/0 · 24/0/80 · 7/0/32** 全绿 |
| =n 十四对象门 | RC=0、零警告、nm 零 corten 符号（.config 已恢复 =y 并重建终镜像） |
| checkpatch --strict | **total: 0 errors, 0 warnings, 0 checks, 3658 lines** |
| guest 门 | 留主会话：①J4 workload 六断言 + gup_probes delta>0（窗口臂现为唯一 GUP 应答）；②M6.T3 压力门复跑（RSS 降/swap.so 增/INV7 零漂移）；③并发 fault 压测 + fork 对拍 |

---

## 4. 登记与披露

- **C25 部分 保真回退**：auto DECLARE 的 PERF_RECORD_MMAP note 随 carrier 消亡；
  `declare_notes` 计数可观测。恢复路径 = 上游 perf_event_mmap() 签名的 metadata
  变体（不在 W-2 展开）。/proc/maps 双源渲染完整保留。
- **GUP 臂与上游 follow 的已登记差**：①can_follow_write 的 PageAnon() 项由
  "PG_anon_exclusive 仅存于 anon 页"隐含；②gup_must_unshare 只对 anchored anon
  生效（unanchor 页的 fork 一致性由写 COW 路径达成）；③protnone 形遵循
  FOLL_HONOR_NUMA_FAULT 门；④get_user_page_vma_remote（ptrace/access_remote_vm）
  的窗口短读行为与 V-C 基线**逐字相同**（其 vma_lookup WARN 形非本片引入）。
- **shadow 域保持锚定（半翻）**是显式决策而非遗漏：F7 fork 对拍保真 > 提前统一
  rmap 姿态；W-4 树清零时 `if (vma)` 臂自然翻无条件 novma。
- **flush_cache_range 的 vma-less 形**：窗口域跳过（x86 no-op；arm64 为
  ARM64_PORTING 登记面，与既有 novma flush 家族同一披露）。
- **首轮 on1 暴露并修复的本片缺陷**（记录在案）：①拷贝臂遗漏
  `pte_unmap_unlock(dst)` → 子 PT 锁滞留 → 重放臂 RCU stall 死锁（栈迹直指
  fork_copy_window，修复后消）；②shmem pagecache 被 folio_test_swapbacked 误分族
  进 anon dup（F7 极性反转面）→ 改 corten_folio_is_filemap；③GUP 臂 write 判定
  的 fault 携带位被循环头重置 → 8 轮耗尽 -EFAULT → 改 cross-iteration carry。
  三处均为新码缺陷，修复后 on1/on2/off1 全绿。
- **观测到一次 off1 的 `corten_test_txn_uninstall_interlock` 偶发失败**
  （worker A 10s 未获锁；mm/corten_test.c 本片零触碰，复跑即绿）——登记为该并发
  锚的既有 timing flake，非本片引入。
- 内存收益：每 auto arena 省 1 个 vm_area_struct 分配/释放 + anon_vma_prepare/
  fork 树维护；J4 面的窗口 GUP 从"probe+tree 兜底"收敛为单臂。

## 5. 文件清单

- /home/ppw/linux-6.18-mva/mm/corten_arena.c（carrier 删除、rmap 翻转、fork 拷贝臂
  `corten_arena_fork_copy_ptes`、GUP 窗口臂 `corten_gup_window`、调试面、家庭判别
  helper `corten_folio_is_filemap`）
- /home/ppw/linux-6.18-mva/mm/rmap.c（novma dup 双 wrapper）
- /home/ppw/linux-6.18-mva/mm/gup.c（__get_user_pages 窗口分支；gup_vma_lookup 恢复上游形）
- /home/ppw/linux-6.18-mva/mm/memory.c（注释同步）
- /home/ppw/linux-6.18-mva/fs/proc/task_mmu.c（row-aware 渲染）
- /home/ppw/linux-6.18-mva/mm/corten_arena.h、include/linux/corten_arena.h
  （carrier 字段退役、auto_shape、corten_gup_window/拷贝臂/dup wrapper 声明与 =n 桩）
- /home/ppw/linux-6.18-mva/mm/corten_arena_test.c（锚同步）
- /home/ppw/cortenmm/patches/r07-w2.diff（导出，3658 行）
