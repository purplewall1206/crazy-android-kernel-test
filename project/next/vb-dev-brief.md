# V-B 开发包：FILE 区 region 化（窗口域内文件映射的零 VMA 落地）

创建: 2026-09-22（V-B 切片准备班, 只读审计产出）
基座: 主树 `/home/ppw/linux-6.18` HEAD = **d40eae59ba76**（V-A.1 收口; region record = A.0
34f1ae661be3, de-VMA park = A.1）。本文所有 `file:line` 为该 HEAD 实测。
前置: **V-A.2a/A.2b**（carrier + fork PTE 复制）——代码在 worktree
`/home/ppw/linux-6.18-mva`（未入库, ~2709 行, 夜验中）。本文挂点清单以主树 HEAD
描述, 涉及 A.2b 已改动面处显式标注 worktree 现状。V-B 开工前提 = A.2b 入库。
规范链: MV_VMA_FREE_SPEC.md §3.2（V-B 全文）+ §2.2/§2.3（region/carrier）+ §2.6
（VM 位表）+ §4.1（铁律/INV-MV3）+ §6 OQ-MV-2/OQ-MV-10。

现状一句话: 路由分类器在 `corten_arena_auto_mmap_classify`（mm/corten_arena.c:2165）
对 `file` 参数无条件回 `CORTEN_MMAP_LEGACY`; dispatch 对 `CORTEN_FILE_MAPPED`
回 `CORTEN_DISP_STUB`（:4183, "M4+ producers: unreachable"）; KUnit 里 FILE 相关
断言恰好三条, 全是"必须拒绝/必须 STUB"的负向锚（corten_arena_test.c:1230-1232
classify、corten_fault_test.c:285-286 dispatch STUB、:5555-5568 region record
rfile 恒 NULL）。V-B = 把这三处负向锚全部翻正向, 并补齐 file 生命周期七面
（classify / attach / fault 双臂 / COW / truncate 门 / i_mmap / put+fork）。

---

## 1. 目标与不做

### 1.1 收（spec §3.2.1）

- MODE 进程 post-ENTER 的 `mmap(NULL, len, prot, MAP_PRIVATE, fd, off)`
  （addr==0、类型位恰为 MAP_PRIVATE、无白名单外 flag 位）→ `CORTEN_REGION_FILE`
  region: 2M 圆整窗口 + `rfile=get_file(file)` + `rpoff=pgoff` + carrier(FILE 形态,
  不置 VM_SHARED) + 全区段 `corten_mark(INVALID→FILE_MAPPED)` 虚拟分配。
- 典型 workload: dlopen 的 .so 文本/数据段、JVM 附加库、locale/gconv 文件。
- fault 语义（§3.2.2）:
  - read fault: query=FILE_MAPPED → 解锁 → 锁外 pagecache 取页
    （filemap_get_folio, 缺页走 readahead/同步读, 可睡）→ 重锁 re-query →
    ptl 内 `set_ptes`（clean 页, perm 编码）→ **meta 保持 FILE_MAPPED**
    （FILE_MAPPED 即驻留形态; `__resv` 不需要——pgoff 由 `region.rpoff + 页内偏移`
    推导, 页级无 slot 负载）。
  - write fault: COW → 复用 `cow_write`（:4717）拷贝分支语义: 私有 folio +
    `copy_user_highpage` + meta 迁移 FILE_MAPPED→MAPPED（状态机 "any→MAPPED"
    合法, include/linux/corten.h:408-410 注记; 经 `corten_map(FORCE)`）。
  - beyond EOF: filemap 口径 **SIGBUS**（`index >= DIV_ROUND_UP(i_size)>>PAGE`,
    mm/filemap.c:3573-3575 的 arena 版）——需要新交付 action（见 §2.3）。
- zap/unmap/mprotect/release: `zap_window` 通用（私有 clean 页 folio_put 即可,
  无脏回写——MAP_PRIVATE 语义; MM_FILEPAGES/MM_ANONPAGES 拆分已在
  `corten_zap_release_page`, :5923-5935）; mprotect 纯 meta 沿 protect_window
  通用臂; RELEASE/park: file put + carrier 出 i_mmap。
- fork: `copy_page_range` 对 FILE_MAPPED 页 = 普通 file COW 复制（白名单 #1
  形状不变, A.2b 的 carrier 对, worktree mm/corten_arena.c:4048）; meta 镜像 +
  子 region `get_file`。

### 1.2 不做（显式边界, spec §3.2.4 + §1.2 + OQ 清单）

- **MAP_SHARED 一切**: 文件/shm/perf ring buffer/共享库真共享形态——永不入窗
  （ipc/shm.c 走 legacy; 共享一致性/i_mmap 写侧/msync/writeback 是第二卷）。
- **MAP_FIXED file = 植入例外（OQ-MV-2 边界）**: 窗口内 file MAP_FIXED 照旧走
  D-G'' punch 路由（`corten_arena_mmap_punch_route`, :7480-7543）, punch 后由
  mmap_region 安装**普通 legacy VMA**, 带白名单标记, 计数可观测
  （`corten_nr_mmap_punches`, :192）。V-B **不**把 CDS 型 MAP_FIXED file
  region 化——那需要 i_mmap 语义满配（写侧共享）而收益低; 该例外由 V-A.3 的
  J2 白名单审计 walker 断言为封闭集合, 任何新植入形态必须打进来。V-B 的
  classify 放行只对 `addr==0`（显式地址/hint 一律 legacy, 与 ANON 同口径）。
- **MAP_DENYWRITE / MAP_EXECUTABLE / MAP_POPULATE / MAP_LOCKED / MAP_STACK /
  MAP_HUGETLB / MAP_GROWSDOWN 等**: 维持单 bit 白名单拒绝（同 :2177-2185 形态）。
- **DAX/fsdax**（FOLL_LONGTERM 拒, mm/gup.c:1242 同源）: classify 拒绝
  （`vma_is_fsdax` 等价的 `file->f_mapping` aops 判定入 `corten_file_may`）。
- **fault-around**（`do_fault_around` mm/memory.c:5685 / `filemap_map_pages`
  mm/filemap.c:3937）: 首版不做（单页取装）; PTE 批装是后续优化, 不在 DoD。
- **GUP-slow 的 FILE 区 pin**: 落在 V-C 的 `corten_gup_probe`（C15 行）;
  V-B 只预置 carrier 的 flag 形状使 `writable_file_mapping_allowed`
  （gup.c:1195, 调用点 :1253）在 V-C 喂 carrier 时逐字生效——即 OQ-MV-10 的
  FOLL_LONGTERM × pagecache 拒绝口径。V-B 期间窗口地址 GUP 行为与 A.2b 后的
  ANON 区一致（find_vma miss）, 不是 FILE 区新退步。
- **共享匿名**（CORTEN_SHARED_ANON 仍无生产者）、WQ/特殊 file_ops
  （prctl/设备, `can_mmap_file` 拒绝链）。

### 1.3 MAY 边（§3.2.1 "file 区 may_prot = prot ∧ file 打开模式允许集"）

`corten_file_may(file, prot)` 纯函数 = do_mmap file 校验链（mm/mmap.c:509-578）
的 MODE 版收口:
- `file_mmap_ok`（:514, size 溢出 -EOVERFLOW）;
- FMODE_READ 门（:554, 无读权限 -EACCES）;
- `path_noexec` ∧ EXEC → -EPERM 且 MAY 去 EXEC（:556-560）;
- `can_mmap_file`（:562, -ENODEV）; GROWSDOWN/GROWSUP（:564, 不可达——白名单已拒）;
- memfd seals（:576 `memfd_check_seals_mmap` 的 MODE 版等价: sealed memfd 拒绝）;
- may_prot = `calc_vm_prot_bits` 的 CORTEN_PERM 镜像: READ 恒经 FMODE_READ,
  WRITE 恒 0（MAP_PRIVATE file 的写全部走 COW, MAYWRITE 语义保留给 mprotect
  升级合法性判定——镜像 legacy 的 `vm_flags &= ~VM_MAYWRITE`? **不**: legacy
  MAP_PRIVATE file 的 VM_MAYWRITE 照常置位（:414 do_mmap 全局置 MAY 三位）,
  写权限由 COW 满足。故 may 记全三位, 与 ANON 的 `corten_region_may_full()`
  （:777-780）同型; file 打开模式只收 EXEC（noexec）一位）。

---

## 2. 数据结构增量（对照 A.0 的 struct 定义, 字段级草案）

### 2.1 `struct corten_arena` 现有 region record 字段——零新增

include/linux/corten_arena.h:229-276 的 A.0 落位**已经把 FILE 需要的载荷全部
建好**（当时注释 "no producer yet"）:

| 字段 | A.0 现状（:266-273） | V-B 用法 |
|---|---|---|
| `rclass` | ANON/FILE/RESERVED 枚举已定义（:128-132） | FILE 有了第一个生产者 |
| `may_prot` | 无读者 | §1.3 的 MAY 边; mprotect 升级门（沿 V-A.3 接线） |
| `rflags` | CORTEN_RF_* 已定义（:144-151） | file 区补记 RF_SEQ_READ/RF_RAND_READ（§2.6 表的两行 RF 下落, madvise hint 位） |
| `rfile` | 恒 NULL（`corten_region_register` :823 清零） | `get_file(file)`; put 点见 §3-H9 |
| `rpoff` | 恒 0 | 映射起始页偏移; 页 pgoff = `rpoff + ((addr - ar->start) >> PAGE_SHIFT)` |
| `npieces`/`rpieces` | 恒 1/空 | V-B 不动（file 区 punch 仍走 D-G'' 植入, 不产生 file 多片） |
| `carrier` | 恒 NULL | A.2b 产物; V-B 给 FILE 形态（见 §2.2） |

结论: **无新 rclass、无新 struct 字段**; 增量全部在"给字段生产者 + 新函数"。

### 2.2 carrier 的 FILE 变体（A.2b `carrier_alloc` 的扩展参数）

worktree `corten_arena_carrier_alloc(mm, start, end, perm)`（worktree
mm/corten_arena.c:789-842）当前只造 private-anon 形态
（`VM_MAY*|VM_NORESERVE|VM_CORTEN|VM_NOHUGEPAGE` + `vma_set_anonymous` +
`anon_vma_prepare`）。FILE 变体（建议同函数加 `struct file *file, pgoff_t pgoff`
参数, 或薄包装 `corten_arena_carrier_alloc_file`）:

```c
/* FILE 形态差异（对 worktree 现有实现的三处改动） */
	vma_set_range(vma, start, end, pgoff);		/* 替换 pgoff=0 */
	/* 不调 vma_set_anonymous(); 置: */
	vma->vm_file = get_file(file);			/* 引用由 region 持有,
							 * carrier 与 rfile 同源同放 */
	/* flags 增 VM_MAYREAD|VM_MAYWRITE|VM_MAYEXEC 照旧, 不置 VM_SHARED;
	 * anon_vma_prepare() 保留(私人 COW 页的 rmap 锚仍需要——wp_page_copy
	 * 形态的 folio_add_new_anon_rmap 需要 avc);
	 * i_mmap 挂钩由 file_attach 单独做(§2.4), 不在 alloc 里。 */
```

### 2.3 fault 交付/action 枚举的两处小增量

- `enum corten_fault_action`（include/linux/corten_arena.h:442-448）增
  `CORTEN_FAULT_BUS`（EOF 交付 SIGBUS/BUS_ADRERR——注意不是 MAPERR: legacy
  beyond-EOF 是 SIGBUS, S 系语义变更登记）。
- `enum corten_fault_status`（mm/corten_arena.c:4351-4358）对应增 `CORTEN_F_BUS`。
- `enum corten_disp`（mm/corten_arena.h:61-96）增 `CORTEN_DISP_FILE_READ`
  （FILE_MAPPED read 臂）。write 臂复用 `CORTEN_DISP_COW_MAYBE`/
  `CORTEN_DISP_ACCERR`（cow_write 内部出 file 分支, 见 §3-H6）。

### 2.4 `corten_region_register` 的 FILE 写侧变体

```c
/* mm/corten_arena.c, 新增（A.0 的 :809 是 ANON/RESERVED 写点） */
void corten_region_register_file(struct corten_arena *ar, u8 may_prot,
				 u32 rflags, struct file *file,
				 unsigned long pgoff, unsigned long len);
	/* = register(FILE, may, rflags) + rfile=get_file(file) + rpoff=pgoff;
	 * 不做 i_mmap 挂钩(carrier 尚未存在), 不做 mark(mark 由 file_attach
	 * 在 fill_upper 之后做——纯 record 写点保持无锁序扩张)。
	 * 调用方持本 mm mmap_write(与 :1097/:6607/:6872 同一纪律)。 */
```

### 2.5 INV-MV3 的 FILE 扩展（铁律 3 (d) 条的落地）

`corten_region_record_ok`（:795-807）加两行:
```c
	if (ar->rclass == CORTEN_REGION_FILE ?
	    (ar->rfile && ar->carrier && ar->carrier->vm_file == ar->rfile) :
	    !ar->rfile)
		return false;
	/* pgoff 派生一致性: carrier->vm_pgoff == ar->rpoff */
```
KUnit 走 `corten_arena_test_inv_mv3`（:6333 既有用例）扩负样本。

### 2.6 VMA 位表核对（§2.6 完备性表在 file 区的增量行）

| VMA 位 | file 区下落 | 备注 |
|---|---|---|
| VM_SHARED/MAYSHARE | REJECT（恒无） | MAP_SHARED 不入窗 |
| VM_SEQ_READ/RAND_READ | RF（已有位定义） | file attach 时反射记录 |
| VM_DENYWRITE | REJECT | `deny_write_access` 不做（classify 拒 MAP_DENYWRITE） |
| VM_SOFTDIRTY | RF | 沿 ANON 现状（S-2 分歧不因 file 扩大: file 页 PTE 同样不置 soft-dirty） |
| 其余 | 沿 §2.6 表不变 | 上游新位必须补表（checklist 常设项） |

---

## 3. 挂点清单（主树 HEAD d40eae59ba76 实测 file:line）

格式: 挂点 → 现状行为 → 目标行为 → 风险。A.2b 已改面标注 worktree。

### H1. mmap 路由分类器放行 file

- **mm/corten_arena.c:2162-2188** `corten_arena_auto_mmap_classify`——
  现状: `:2165-2166 if (file) return CORTEN_MMAP_LEGACY;` 一刀切; 且
  `:2174-2175` 要求 MAP_ANONYMOUS。目标: file 参数分支放行
  `MAP_PRIVATE | addr==0 | 白名单余位 == MAP_NORESERVE`（签名加 `prot`/
  校验延后到 `corten_file_may`; MAP_FIXED file 不在此臂——见 OQ-MV-2）。
  风险: 低（纯函数, KUnit 表先行）。**注意 classify 的调用点**
  mm/mmap.c:427 `if (!file && addr == 0)` 门要放宽为 `addr == 0`
  （file 与否交给 route/classify）。
- **mm/mmap.c:427-456** do_mmap auto 路由门——现状 `!file` 前置; 目标去掉
  `!file`（A.2a 的 cret==1 直返形态在 worktree 已就位, file 分支同型接入）。
  风险: 中——file 校验链（mm/mmap.c:509-578）必须在 route 内等价重放
  （§1.3 的 `corten_file_may`）, 否则 -EACCES/-EPERM/-ENODEV/-EOVERFLOW
  语义丢失。

### H2. `corten_arena_auto_mmap_route` 的 file 臂

- **mm/corten_arena.c:2686-2781**——现状: 只服务 anon（classify 门在 :2711）。
  目标: classify==AUTO_FILE 时走 `corten_file_may` 校验 → pool_take **跳过**
  （FILE 区不进 resident pool: 复用窗是 ANON 语义; pool 命中判 rclass）→
  `window_place` → 调 `corten_arena_file_attach`（新, 替代 :2776-2778 的
  flags 重写——A.2a 后该形态已变, 直连 declare）。
  风险: 低-中。注意 `sysctl_overcommit_memory==OVERCOMMIT_NEVER` 整体降级
  （:2706-2709）对 file 区同样保守沿用（file 区无 VM_ACCOUNT 需求, 可放宽,
  但首版沿保守口径, 登记为后续微调）。

### H3. `corten_arena_file_attach`（新函数, attach 三件套）

- 位置: mm/corten_arena.c declare 族（对照 `declare_locked` :1010 与 A.2a 后
  的 worktree 直连形态）。职责:
  1. `get_file` + `corten_region_register_file`（§2.4）;
  2. carrier(FILE)（§2.2）+ **i_mmap 挂钩**: `i_mmap_lock_write(mapping)` →
     `flush_dcache_mmap_lock` → `vma_interval_tree_insert(carrier,
     &mapping->i_mmap)` → unlock 两把（逐字对照 `__vma_link_file`,
     mm/vma.c:256-265; 不调 `mapping_allow_writable`——VM_SHARED 未置）;
  3. `fill_upper` 全窗（对照 mmap_route :7630-7639 的循环）+
     `corten_mark(FILE_MAPPED, perm)` 全区段虚拟分配（mm/corten.c:1123-1132
     的 INVALID→FILE_MAPPED **已合法, 事务层零改动**）。
  风险: 中——mark 需要每窗事务（fill 先行）, 与 mmap_route 同型; i_mmap 挂钩
  锁序 `mmap_write > i_mmap_rwsem`（内核既有序）, 不与 desc 锁嵌套。

### H4. dispatch 与 fault_once 的 FILE 双臂接线

- **mm/corten_arena.c:4183-4190** dispatch `CORTEN_FILE_MAPPED →
  CORTEN_DISP_STUB`——目标: read+perm_ok → `CORTEN_DISP_FILE_READ`;
  write ∧ perm WRITE → `CORTEN_DISP_COW_MAYBE`; 写无权（含 EXEC-only 写）→
  `CORTEN_DISP_ACCERR`。风险: 低（纯函数, corten_fault_test.c 表改三行）。
- **mm/corten_arena.c:5368-5405** fault_once 的 FRESH 合成臂——现状: 无条件
  合成 `PRIVATE_ANON`。目标: `rclass==FILE` 时合成 `FILE_MAPPED`（perm 沿
  KEEP_PERM 残留或 ar->prot）。**这是 DONTNEED/截断后 re-fault 的语义命门**:
  FILE 区内容被 chunk-zap 后 slot 回 INVALID+perm, re-fault 若合成
  PRIVATE_ANON, 截断重读变成匿名零页——静默内容损坏。风险: 中（漏改即
  数据损坏, KUnit 必须有对拍用例, §5）。
- **mm/corten_arena.c:5443-5456 后** 新 case `CORTEN_DISP_FILE_READ` →
  `corten_arena_file_read(ctx, &m)`（新, 自管解锁/重锁——swapin 同款
  "owns its lock cycles" 形态, :5443 注记同型）。

### H5. read 臂 `corten_arena_file_read`（新函数, V-B 最大新代码块）

- 形态（对拍 `filemap_fault` mm/filemap.c:3562-3660 + `do_read_fault`
  mm/memory.c:5746-5780, 去掉 vm_fault 包装）:
  ```
  事务解锁（持 active 引用）→
    pgoff = rpoff + (addr - start)>>PAGE;
    max_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
    if (pgoff >= max_idx) return CORTEN_F_BUS;		/* EOF: filemap.c:3573-3575 同判 */
    folio = filemap_get_folio(mapping, pgoff);		/* 命中: async readahead 可省首版 */
    miss → __filemap_get_folio(FGP_CREAT|FGP_FORFMAP)	/* 同步读, 可睡 */
    folio_lock → 截断复查 folio->mapping != mapping → retry	/* filemap.c:3628-3633 */
  → 重锁 corten_lock_range → txn_owned + re-query(state==FILE_MAPPED) →
    ptl: set_ptes(folio_mk_pte + perm 编码, clean);
    folio_add_file_rmap_pte(folio, page, carrier, false);	/* rmap 挂 i_mmap 找回 */
    add_mm_counter(mm, MM_FILEPAGES, 1);			/* 记账对齐 legacy */
    meta 不写（FILE_MAPPED 即驻留形态; nr_mapped 不含 file 页
    → shrinker 天然不把 pagecache 页当 swap 候选——正是想要的行为）
  ```
  风险: **高**（MV-9: 锁外取页 × 并发 truncate/回写; do_read_fault 同构的
  "锁外取页+锁内 re-query" 图 7 契约; folio 引用收支三出口: 成功转 PTE/
  retry 放/fallback 放）。
- **arch/x86/mm/fault.c:1362-1381** 快钩 switch——目标: 加
  `case CORTEN_FAULT_BUS: force_sig_fault(SIGBUS, BUS_ADRERR, addr); return;`
  （与 ACCERR 臂同款 no-lock 交付形态）。风险: 低; 慢钩
  （mm/memory.c:6561-6582）走 vm_fault_t, `VM_FAULT_SIGBUS` 自然贯通,
  只需 `corten_arena_handle_mm_fault` 的 status 映射补 BUS 行。

### H6. COW 写臂 `cow_write` 的 file 分支

- **mm/corten_arena.c:4717-4879**——现状: 假定 old folio 是 anon（reuse 判定
  `folio_mapcount==1 ∧ PageAnonExclusive`, :4765-4772; 计数只动
  MM_ANONPAGES, :4843/:4856）。目标（meta=FILE_MAPPED ∧ PTE present 的写）:
  1. **reuse 恒否**: pagecache folio 的多 mapcount 是常态（他进程共享同一
     pagecache 页）, `folio_test_anon(old)` 为假直接走 copy 分支;
  2. copy 分支补 file 差额: `add_mm_counter(MM_ANONPAGES, +1)`（新私有页）+
     `add_mm_counter(MM_FILEPAGES, -1)`（旧 file 页, 拆分对照
     `corten_zap_release_page` :5923-5935 的既有拆分）; old folio 的
     `folio_remove_rmap_pte(old, page, carrier)` + `folio_put`（对拍
     wp_page_copy mm/memory.c:4137-4167 的 file 形状——file rmap 走
     同一 remove 函数）;
  3. meta: `corten_map(txn, addr, 新页, perm, FORCE)`——FILE_MAPPED→MAPPED
     （"any→MAPPED 合法"）, flags 清 SHARED;
  4. 从未驻留页的首写（PTE 空 ∧ FILE_MAPPED）: 先 file_read 取页再 copy
     （`do_cow_fault` mm/memory.c:5782-5822 同构: fetch+copy 一步到位）。
  风险: 中——folio 收支与计数的 file/anon 双形态是本臂全部难点; GUP-pin 的
     pagecache 页恒走 copy（wp_can_reuse 拒 pin 同理）。

### H7. truncate/invalidation 路由门（INV6 route-only 扩展, 红线）

- **mm/memory.c:4153-4158** `unmap_mapping_range_vma`——现状: 直接
  `zap_page_range_single` 裸写。目标: 前置
  `corten_enabled_static() && (vma->vm_flags & VM_CORTEN)` 门（carrier 恒带
  VM_CORTEN）→ 转发 `corten_arena_unmap_chunk(vma->vm_mm, ar, start, len)`
  （:6477; KEEP_PERM 语义 = truncate 的 VA 保留; 截断后 re-fault →
  FRESH→FILE_MAPPED→EOF 外 SIGBUS, 与 legacy 逐位一致）→ 返回不落
  zap_page_range_single。调用点形态: `unmap_mapping_pages`（:4227-4243,
  i_mmap_lock_read 持有中）与 `unmap_mapping_folio`（:4192-4213,
  invalidate/回收单页路径, ZAP_FLAG_DROP_MARKER 形态）**两处都会把 carrier
  送进来, 门必须在 vma 层一刀切**。
- **mm/memory.c:2177-2184** `zap_page_range_single`——防御性第二门（对拍
  D-G'' `__mmap_prepare` backstop 先例, mmap.c:470-499 注记形态）: WARN+计数
  当 vma 是 carrier（VM_CORTEN ∧ 不在树）——正常路径永远到不了, 到了就是
  漏网新调用者。
- 锁序: 门内转 `unmap_chunk` 纯事务（`munmap_route` :7106 的无锁形态先例）,
  与 i_mmap_lock_read 的嵌套 = `i_mmap_read > desc > ptl`——不与 attach 的
  `mmap_write > i_mmap_write` 成环（desc 锁内绝不取 i_mmap, 对拍 shrinker
  先例纪律）。风险: **高**（新窗口 PTE 写者家族; 门漏一个调用点 = 裸写 =
  设计错误, §6 红线 2）。
- **截断-重读语义**: chunk zap 用 KEEP_PERM（:6375 `unmap_chunk_flags` 的
  既有 flag 语义: 内容清、VA/perm 留）→ re-fault 重读文件——spec §3.2.3
  原文口径。

### H8. i_mmap 挂钩的拆除面

- attach 插入（H3）对应拆除点:
  - `corten_arena_release_arena_locked`（:1259-1396）;
  - `corten_arena_pool_park_locked`（:6802, park→RESERVED 时**同时 file put**:
    复用窗 reactivate 是 ANON 语义, rfile 留着是悬挂引用);
  - `corten_arena_mm_exit`（:1894-2046, mm_users==0 清场）;
  - `corten_arena_mode_exit`（:2847, 逐 arena RELEASE 复用上一行）;
  - punch/植入路径若打在 FILE 区上（未来）: punch_split 同步手术。
  拆除统一收敛到 `corten_region_file_teardown(ar)`:
  `i_mmap_lock_write → interval_tree_remove → flush_dcache_mmap_unlock →
  fput(rfile) → rfile=NULL`（carrier 本身交 A.2b 的 free 通道）。
  风险: 中——**漏一处 = fput 缺失（file 引用泄漏, 文件无法回收）或
  i_mmap 悬挂节点（UAF）**; §5 的引用对账矩阵按这五点逐个出用例。

### H9. file 引用计数与 fork 迁移

- `corten_arena_fork_register_child`（:3223-3296）——现状: `corten_region_
  register(child, rclass, may, rflags)`（:3247）只拷 class/may/rflags, 注释
  明言 "FILE payload always clear in V-A.0"。目标: rclass==FILE 时改调
  `register_file`（get_file 子引用 + rpoff 拷贝）+ 子 carrier(FILE 形态,
  pgoff 同源)。
- fork PTE 复制: A.2b 的 `copy_page_range(ccarrier, pcarrier)`（worktree
  :4048, 主树无——今天靠树内 shadow-VMA 被 dup_mmap 循环复制）对 FILE 页 =
  普通 file COW（wrprotect + file rmap dup + GUP-pin 拷贝兜底, copy_page_range
  mm/memory.c:1499-1559 逐字吃 carrier; `is_cow_mapping` 对无 VM_SHARED 的
  file carrier 为真 → wrprotect 生效）。meta 侧 fork_mirror/copy_window
  （:3328/:3619）对 FILE_MAPPED slot 走 mark(state→itself) 记 SHARED——
  已通用, 对拍补用例即可。
  风险: 中——file rmap dup 的 folio lock 依赖（folio_try_dup_file_rmap_pte
  走 copy_page_range 内部既有路径, 无需 arena 侧新代码, 但 KUnit 对拍必须
  含"父子共享 pagecache 页→双写各自 COW"链）。

### H10. 观测面（收尾）

- debugfs `arenas` 渲染（`corten_arena_arenas_report` :1506,
  `corten_region_class_name` :1478 已有 "FILE" 名）补 rfile/rpoff 列;
  新计数: `file_mmaps`/`file_read_faults`/`file_cow_copies`/`truncate_routes`
  （沿 :176-193 atomic_long_t 命名惯例 + `corten_arena_stats_report` :1542 列）。

---

## 4. 实施顺序（4 片, 每片 ≤300 行 diff 含测试; 依赖 A.2b 入库）

依赖总形: **B.1 → B.2 → B.3 → B.4 线性**; B 依赖 A.2b（carrier/anchor/fork
copy 形状, §"A.2b 依赖"）; A.3 可并行 review 不阻塞 B。每片收口 =n 折叠 +
=y/off 零扰动 + on 全绿（铁律 1）。

### V-B.1 classify + attach + 引用生命周期（~+300, 含测试 ~120）

- H1（classify file 放行 + do_mmap 门放宽）+ H2（route file 臂）+
  H3 前半（`corten_file_may`、`register_file`、carrier(FILE)、`file_attach`
  的 declare+mark）+ H8/H9 的 put 面（release/park/mm_exit/mode_exit 的
  `file_teardown`、fork 的子引用）。
- **i_mmap 挂钩不启用**: carrier 造好但不 insert——B.1 收口时 FILE 区无任何
  驻留页（fault 仍 STUB→MAPERR）, rmap 找不到 carrier 是无害的（无人问）。
  这样把"结构生命周期先绿"与"语义启用"切开, B.1 中间态完全可运行。
- 测试: classify 真值表扩展（含 file→AUTO_FILE 翻转 :1230 的负向锚改正向）+
  引用对账四态 + INV-MV3(d)。
- 风险: 低-中; 纯结构+路由, 无 PTE 新写者。

### V-B.2 i_mmap 挂钩 + truncate 路由门（~+260, 含测试 ~110）

- H3 后半（attach/teardown 的 interval_tree insert/remove）+ H7（两处
  unmap 入口门 + zap_page_range_single 防御门 + 截断-重读 KEEP_PERM 对拍）。
- 此片单独成片的原因: i_mmap insert 一旦发生, carrier 就进入
  `unmap_mapping_range` 的 walker 视野——门必须与 insert **同一片**落,
  否则中间态裸写窗口 PTE（INV6 违规, 不可出厂）。
- 测试: truncate 路由臂（注入 unmap_mapping_range_vma 形状断言事务计数、
  零裸写防御计数）; FILE_MAPPED 区 attach 后 i_mmap 可见性。
- 风险: 高（本片是红线片）; review 焦点 = 锁序声明与门覆盖完备性。

### V-B.3 fault 双臂（~+300, 含测试 ~130）

- H4（dispatch 三路 + FRESH 合成 rclass-aware）+ H5（`file_read` 全链 +
  EOF→CORTEN_F_BUS + arch/fault.c 交付 case + 慢钩映射）+ H6（cow_write
  file 分支 + 计数拆分 + 首写 fetch+copy）。
- 测试: FILE_MAPPED 分派矩阵（read/write/EOF/COW/双 fault 竞争合成）、
  FRESH 重合成用例（截断/DONTNEED 后）、BUS 交付。
- 风险: 高（MV-9 竞态; folio 收支三出口）; KUnit 合成窗 + guest 截断-重读
  矩阵双口径。

### V-B.4 fork 对拍 + 观测 + 扫尾（~+230, 含测试 ~120）

- H9 对拍用例（fork 后父子 FILE_MAPPED 三方一致: PTE/meta/rfile 引用;
  双写 COW 独立性; `copy_page_range` file 形状回归）+ H10（debugfs 列 +
  计数）+ guest 判据全量: JVM 完整启动含 CDS **关闭态**（D-G'' 不回退）+
  dlopen 繁重 workload（psearchy_eq/locale）checksum + 截断-重读与 legacy
  臂 strace/si_code diff + perf2a churn 回归。
- 收口即 V-B DoD: spec §5 切片表 B 行四锚全绿。

片间依赖图: `B.1 (结构) → B.2 (i_mmap+门, 红线) → B.3 (语义) → B.4 (fork+
观测)`; B.2 与 B.3 理论可互换（B.3 的驻留页会立刻暴露在未开门的
unmap_mapping_range 下, 故门先行不可换）。

---

## 5. 测试矩阵（对照既有 KUnit 惯例的用例名草案）

惯例: 纯函数走 `mm/corten_fault_test.c` 表驱动（dispatch 表 :230-297 形态）;
带 mm 的走 `mm/corten_arena_test.c` 合成窗（`corten_arena_test_*` 命名,
用例表 :6311-6367）; 计数/注入钩子沿 `corten_arena_test_*` 的 debugfs-free
直调形态（:1701-1880 先例）。

### 5.1 classify/MAY 真值表（B.1）

- `corten_arena_test_auto_classify_file`——file+PRIVATE+addr==0 → AUTO;
  MAP_SHARED/VALIDATE file → LEGACY; MAP_FIXED file → LEGACY（植入路由）;
  MAP_DENYWRITE/POPULATE/STACK file → LEGACY; DAX 文件 → LEGACY。
- `corten_fault_test_file_may_matrix`（纯函数）——无 FMODE_READ → -EACCES;
  noexec∧PROT_EXEC → -EPERM 且 MAY 无 EXEC; `can_mmap_file` 假形状 →
  -ENODEV; pgoff+len 溢出 → -EOVERFLOW; 正常 → may 三位。

### 5.2 FILE_MAPPED 分派矩阵（B.3, corten_fault_test.c 表改写）

替换现 STUB 行（:285-286）为:
- `file-read` → `CORTEN_DISP_FILE_READ`;
- `file-write-writable` → `CORTEN_DISP_COW_MAYBE`;
- `file-write-ro` → `CORTEN_DISP_ACCERR`;
- `file-exec-instr` → `CORTEN_DISP_FILE_READ`（perm_ok 的 R|X 读通道）。

### 5.3 引用对账（B.1, file_count 计数锚）

- `corten_arena_test_file_refcount_lifecycle`——declare(+1)/release(-1)/
  park(-1)/reactivate(不再持有)/mm_exit(-1)/mode_exit(-1) 六态后
  `file_count() == 初始`; fork → +1, 子 exit → -1。
- `corten_arena_test_file_fork_mirror`——子 region rfile==父 rfile、rpoff
  相等、carrier->vm_pgoff 同源（INV-MV3(d) 三断言）。

### 5.4 truncate 路由臂（B.2）

- `corten_arena_test_truncate_route`——合成 FILE 区 + 驻留页, 直接调
  `zap_page_range_single(carrier-shaped vma)` 防御门 → 断言 WARN 计数 +
  零 PTE 变化; 模拟 `unmap_mapping_range_vma` 入口 → 断言
  `corten_nr_unmap_pages` 事务侧计数增长 + PTE 清 + meta INVALID+perm 留。
- `corten_arena_test_truncate_refault`——截断后 re-fault: EOF 内重读
  （内容=新文件内容）; EOF 外 → BUS; 与 legacy 臂同形 diff。

### 5.5 语义对拍（B.3/B.4）

- `corten_arena_test_file_read_install`——驻留后: PTE present∧clean、
  meta 仍 FILE_MAPPED（非 MAPPED）、MM_FILEPAGES 计数、rmap mapcount+1
  （经 i_mmap walk 可找到 carrier）。
- `corten_arena_test_file_cow`——首写: 私有页 + meta MAPPED + EXCLUSIVE +
  MM_ANONPAGES/FilePages 拆分 + 旧 pagecache folio mapcount-1; 二次写走
  reuse 臂（此时已 anon）。
- `corten_arena_test_file_fresh_resynth`——DONTNEED 后 re-fault 合成
  FILE_MAPPED（**不是** PRIVATE_ANON; 截断重读链的 KUnit 锚, H4 命门）。
- `corten_arena_test_file_double_fault_race`——合成并发: read 臂锁外窗口
  注入竞争 zap → re-query 拒绝 → retry 收敛（Fig.7 预算内）。
- guest: JVM+CDS(off)/dlopen 繁重 checksum; open-mmap-read-truncate-read→
  SIGBUS(BUS_ADRERR) 矩阵与 legacy diff; perf2a churn。

---

## 6. 红线核对表（每片 review 逐条打勾）

| # | 不变量 | 在 file 路径的映射 | 验证锚 |
|---|---|---|---|
| 1 | 非 MODE 零感知（=n 折叠 + 双门形态） | H1/H7 的新钩子全部 `corten_enabled_static() && mm->corten_mode/state` 先行; zap 门在 vma 层 = `VM_CORTEN` 位门（VM_CORTEN 只在 corten=on 内核出现, rmap 守卫同款先例 mm/corten_arena.h:406-410 注记） | =n 八对象 + nm 零符号; =off 冒烟 |
| 2 | **INV6: 任何 arena PTE 写必经事务; 白名单 3/3 已满, 不得新增第 4 写点** | file 路径的三类新写者全部合规: (a) truncate/invalidation → **路由**（H7 门, route-only 扩展, 非 Vega 白名单）; (b) read 臂/COW 的 set_ptes → 事务内 ptl（H5/H6, desc write lock 持有中）; (c) attach 的 mark → 事务。`zap_page_range_single` 防御门把漏网变响 | 5.4 用例 + 防御计数恒 0 |
| 3 | INV-MV3 扩展（铁律 3）: (a) region≡arena 界——天然; (b) may_prot⊇每页 perm——`register_file` 构造保证 + protect_range 升级门读 may; (c) rclass/idle/池互恰——**park(FILE)→RESERVED 同时 file put**（复用窗 reactivate 恒 ANON, 不得带 rfile 复活）; (d) rfile>0 ∧ carrier->vm_file==rfile ∧ pgoff 派生一致——`record_ok` 扩展（§2.5） | `inv_mv3` KUnit 负样本 + 调试构建 checker |
| 4 | §2.6 VM 位完备表 | file 区新消费行已补（§2.6 表）; 未列位=拒 | review checklist 常设 |
| 5 | 语义变更披露 | S-FILE-1（登记新）: EOF 外访问 = SIGBUS BUS_ADRERR（与 legacy 一致, 但**经快钩交付**是新路径, si_code 对拍入 guest 矩阵）; S-FILE-2: FILE 区驻留不计入 nr_mapped/shrinker 候选（pagecache 页 reclaim 由 pagecache 侧拥有, 正确行为, 文档化） | REPORT.md V-B 章 + strace/si_code diff |
| 6 | 锁序不扩边（INV2/§2.7） | 新嵌套仅 `mmap_write > i_mmap_rwsem`（attach, 内核既有序）与 `i_mmap_read > desc > ptl`（truncate 门, desc 锁内不取 i_mmap——shrinker 纪律同款）; pagecache I/O 全在 desc 锁外（M6 D4 同构） | B.2 review 焦点声明 |
| 7 | [FAIL-2] carrier 寿命 | FILE carrier 与 ANON 同一 active-ref 栏栅; i_mmap 节点拆除全部在 mmap_write 下的 region 变更点 | 既有 carrier KUnit 复用 |
| 8 | 混合态一等公民（MV-11） | 同 mm 内 ANON+FILE+parked 并存: 全消费面以帧表为准（已成立）; 每片 DoD 含混合态用例 | 5.3/5.5 组合用例 |

---

## 附: 本文件的关键 file:line 索引（主树 HEAD, review 复核用）

- corten 侧: classify :2162-2188（file 拒 :2165）/auto route :2686-2781/
  register :809-835/record_ok :795-807/declare_locked :1010/release_arena_
  locked :1259-1396/mm_exit :1894-2046/mode_exit :2847/fork_register_child
  :3223-3296/fork_copy_window :3328/fork_mirror :3619/perm_ok :4127/dispatch
  :4146-4204（FILE STUB :4183）/fault_status :4351/fault_ctx :4360/fault_once
  :5285-5507（FRESH :5368）/cow_write :4717-4879/zap_release_page :5918/
  unmap_chunk_flags :6375/unmap_chunk :6477/park_locked :6802/punch_route
  :7480/mmap_classify :7553/mmap_route :7581。
- 事务层（零改动）: mm/corten.c mark 状态机 :1123-1132（INVALID→
  FILE_MAPPED 已合法）; include/linux/corten.h 状态枚举 :97-104/any→MAPPED
  注记 :408-410。
- legacy 对照: do_mmap file 链 mm/mmap.c:427/509-578/647; mmap_region
  mm/vma.c:2855; __vma_link_file mm/vma.c:256-265; do_read_fault
  mm/memory.c:5746; do_cow_fault :5782; do_fault_around :5685; filemap_fault
  mm/filemap.c:3562（EOF SIGBUS :3573-3575）; filemap_map_pages :3937;
  unmap_mapping_range_vma mm/memory.c:4153/unmap_mapping_folio :4192/
  unmap_mapping_pages :4227/zap_page_range_single :2177; copy_page_range
  mm/memory.c:1499; truncate 入口 mm/truncate.c:733/766; gup 拒绝链
  mm/gup.c:1195/1242/1253; arch 快钩 arch/x86/mm/fault.c:1362-1381。
- worktree（A.2b 只读参考）: carrier_alloc worktree mm/corten_arena.c:789/
  anchor_vma :861/fork copy_page_range :4048/do_mmap 直返形态 worktree
  mm/mmap.c:445-467。
