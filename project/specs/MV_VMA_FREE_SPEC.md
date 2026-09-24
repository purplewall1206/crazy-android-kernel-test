# MV_VMA_FREE_SPEC · M-V: MODE 进程完全脱离 VMA 层（零 VMA 运行）设计规格
创建: 2026-09-21 深夜（架构升级规格班, D20 授权; 规划者视角/论文作者视角）
授权: **STATE.md D20（2026-09-21 用户直接指令·架构升级）**——"移除整个 VMA 层——
从 D1 的 opt-in shadow-VMA 升级为论文完整形态: MODE 进程零 VMA 运行（maple tree 空,
全部内存经 corten metadata/事务）"。VMA **层代码**保留服务非 MODE 进程（单内核双 MM）;
"移除"指 corten 管理的内存完全脱离 VMA 语义。
基座: 主树 `/home/ppw/linux-6.18` HEAD = **4ca6ef1048f0**（M3b + M4.T0/T1/T2/T1c +
M5 全量 + M6.T1/T2/T3/T4 + perf2a + A5 + defguard + m9-arm64, 主线 27 项目提交 + 2 merge,
tag corten-r07-integrated, 31 提交）。本文所有 `file:line` 为该 HEAD 实测（无未提交增量）。
规范链: PAPER_SPEC(PS-x) > DESIGN(DEV 表) > D20 > **本文** > 既有三规格
(M4T0_SPEC/M5_FORK_SPEC/M6_RMAP_SPEC, 作为实现史与语义基线被本文**演进而非废止**——
shadow-VMA 机制本身在非 MODE 路径(MODE-targeted DECLARE)继续存活)。

> **多夜声明（先于一切）**: 本文是 V-A→V-E **分阶段多夜实施**的规格, 总量约
> 4,700 行内核 diff + ~1,300 行测试（§5 切片表）, 按 D8 惯例每片独立 review+commit,
> 合计 **9 个切片 ≈ 9–12 个实施夜**。不存在一夜实现形态; 每一片收口时内核必须
> 处于"=n 折叠 + =y/off 零扰动 + on 全绿"的可运行态（铁律, §4.1）。
> 本文是纯设计产出（只读审计 + 本文档）; 内核树未被触碰。

> **实施进度（2026-09-22 v1.5 校准维护, 入库记录见 STATE）**: V-A.0 ✓
> （34f1ae661be3 region record）| V-A.1 ✓（d40eae59ba76 de-VMA parked windows,
> results/r07/mva1-verify.md; 已知边界 B-1 rmap 锚→A.2b / B-2 上级表页→V-D）|
> A.2a/A.2b 代码完成（carrier detached VMA + fork 拷贝迁移, patches/r07-mva2.diff
> 2709 行, worktree mva 未入库, 09-22 23:00 夜验矩阵自动排程 bin/mva2-night.sh）|
> A.3-V.E 未开始。基座行（上方"HEAD=4ca6ef1048f0"）为规格撰写时点锚, 实施中内核树
> HEAD 已前进至 d40eae59ba76（33 提交/29 tag）。

---

## 0. 现状基线一句话（本文的出发点）

今天（HEAD 4ca6ef1048f0）, MODE 进程的每一段 arena 都以**一个普通匿名 VMA + 两个位**
（`VM_CORTEN|VM_NOHUGEPAGE`, shadowize mm/corten_arena.c:577-618）的形式寄生在 maple
树里; 热路径（fault: arch/x86/mm/fault.c:1362 快钩、mm/memory.c:6561 慢钩）已经零
VMA 树走查（per-mm 2M 帧 xarray + 事务）, 但 **VMA 仍然是 arena 的 rmap 锚、fork 的
copy_page_range 载体、exit 的拆表通道、/proc/maps 的渲染源、GUP-slow 的查询单位、
park 池的"PROT_NONE 预约"残骸**（park 时 shadow-VMA 退化为普通 PROT_NONE 匿名 VMA
继续占树, mm/corten_arena.c:5994-6001）。M-V 的全部工作 = 把这六条寄生关系逐一
切断, 让 maple 树退出 MODE 进程的 corten 管辖内存。

---

## 1. 目标与语义（终态定义）

### 1.1 终态一句话

**MODE 进程的 corten 窗口域内零 VMA**: 全部匿名与私有文件内存以"区域记录 +
per-PTE metadata + 事务"存在; maple 树里只剩下**白名单登记的委托遗留 VMA**
（exec 期映射、栈、vdso、brk 堆、植入映射）; 所有 VMA 消费者对窗口域地址要么
结构性触达不了、要么已改走 metadata 路径; 非 MODE 进程一行代码未变。

### 1.2 域（Dominion）划分 —— 本规格的核心诚实化决策

"MODE 进程 maple tree 空"在物理上不可达: MODE 位由运行时 prctl 置位
（kernel/sys.c:2887-2890 → corten_prctl_mode, mm/corten_arena.c:2449-2476）,
**ENTER 不跨越 exec**（exec 重建 mm, M4T0_SPEC §6 已证）, 所以每个 MODE 进程
必然带着 exec 期建立的映射进入 MODE（ELF/ld.so/interpreter 的 file 映射、
setup_arg_pages 的主栈、vdso/vvar）; 同理 MAP_FIXED 植入（JVM CDS, D-G'' 路由
mm/corten_arena.c:6546-6705）与 brk 堆是 ABI 层的显式地址操作。因此终态以
**域**定义, 而非字面空树:

| 域 | 范围 | 终态 |
|---|---|---|
| **窗口域（corten dominion）** | `[CORTEN_MODE_WINDOW_START, CORTEN_MODE_WINDOW_END)` = `[0x1000_0000_0000, 0x4000_0000_0000)`（include/linux/corten_arena.h:203-204） | **零 VMA**（唯一例外: "植入 VMA", 见下行）。全部内存 = 区域记录 + metadata + 事务 |
| 植入例外 | 窗口内的 file `MAP_FIXED`（D-G'' punch 路由的产物, JVM CDS 依赖） | 保留**一个**普通 legacy VMA（punch 后由 mmap_region 正常安装）; 带白名单标记, 计数可观测 |
| **委托遗留域** | 窗口外全部: exec 期 file 映射、主/线程栈（MAP_STACK 自 T0 起在 auto 白名单之外, M4T0_SPEC §3.1）、vdso/vvar/vsyscall、brk 堆（V-E）、MAP_SHARED 文件/shm/perf ring buffer | 照常作为 VMA 存在（VMA 层代码继续服务它们）; 每个可被审计断言"非窗口域" |

窗口域与委托域无地址交叠（窗口在 16T–64T, mmap_base/brk/vdso/栈在低端;
窗口发放的 obstacle 扫描保证窗口内无外来 VMA, mm/corten_arena.c:2110-2152,
find_vma_intersection 检查在 :2134）。V-A 后窗口内唯一可能的 VMA 是植入例外;
V-A.3 的审计不变量把它变成可断言的封闭集合。

### 1.3 终判据（全部可测量, V 系列收口 = 四条全过）

- **J1（find_vma 零调用）**: MODE 进程全生命周期, 对**窗口域地址**的
  `find_vma`/`find_vma_intersection`/`find_vma_and_prepare_anon`/
  `lock_vma_under_rcu` 调用数为 **0**。度量: (a) 审计计数器——`find_vma` 系
  入口加 `corten_enabled_static() && mm->corten_mode && 窗口判定` 的命中计数
  （非 MODE 进程付一次 static-branch 读, 与现有全部钩子同型; kernel/fork.c:1059
  注记的布局惯例）; (b) guest 侧 bpftrace/kprobe 独立复核（`find_vma` 第一参数
  addr ≥ 窗口起点即报）。
- **J2（树形态不变量）**: MODE mm 的 maple 树中每个 VMA 满足
  `地址 ⊄ 窗口域`（委托域）或 `是登记植入`（植入例外）; 违例 = INV-MV2 断言
  （KUnit 合成 mm + guest debugfs 审计入口）。
- **J3（/proc 枚举真源切换）**: `/proc/<pid>/maps`、smaps、`PROCMAP_QUERY`
  对窗口域条目全部由区域记录枚举（V-C）, 与 shadow-VMA 时代基线做**逐字节
  oracle diff**（地址一致、prot/offset/名字一致; parked 消失属登记语义变更,
  §3.1）。
- **J4（GUP 无 VMA 走查）**: GUP-slow 对窗口域地址不再经由
  `find_vma`（mm/gup.c:1973-1975/2041）与 `check_vma_flags`（gup.c:1213）,
  改走 metadata 判定 + 事务 fault（V-C）; GUP-fast 本就无 VMA（页表直走）,
  零改动, 只回归验证。

### 1.4 与 D20 字面的两处偏差（需主 agent 批准, 已给论证）

1. **"maple tree 空" → "窗口域零 VMA + 委托白名单封闭"**（§1.2）。字面空树
   要求消灭 exec 期映射/栈/vdso/brk, 而它们**先于** MODE 位存在（prctl 不跨
   exec）, 消灭它们 = 重新发明 exec/ABI, 收益为零、风险为论文全部已知边界。
   D20 注记"移除指 corten 管理的内存完全脱离 VMA 语义"——本规格即按此口径
   执行; 委托域 VMA 不是 corten 管理的内存。
2. **"零 VMA" ≠ "零 `struct vm_area_struct`"**: corten 保留一个**不入树的**
   detached VMA 对象作为每区域的 rmap/i_mmap/PTE-API 载体（§2.3）。理由:
   `pte_mkwrite(vma)`（pkey 位）、`folio_add_new_anon_rmap(vma)`、
   `copy_page_range(vma)`、`anon_vma_prepare(vma)` 全部以 vma 为参数;
   全盘自造这些语义（论文的页描述符 rmap, PS-B5）= 重写 fork/COW/GUP-pin/
   mapcount 家族（M5/M6 已验收代码的全部依赖面）, 与"论文完整形态"的工程
   路径矛盾。**树与锁与消费者才是 VMA 层**; 载体对象是 metadata 的宿主。
   此口径与 D20 注记一致, 但必须显式登记, 防止 review 用字面"无 vma 结构"
   判 FAIL。

### 1.5 单内核双 MM 不变

VMA 层全部代码保留: 非 MODE 进程、MODE-targeted DECLARE（prctl 79,
kernel/sys.c:2887）、委托域、植入映射、全部其他内核 mm 用户照走原路径。
CONFIG_CORTEN_MM=n 时本系列全部改动折叠为零（每片过 =n 八对象 + nm 零符号,
STATE r07-integrated 口径）。已建立的双门惯例
`static_branch_unlikely(&corten_enabled_key) + mm->corten_mode`（先例:
mm/mmap.c:427、mm/memory.c:6561、mm/gup.c:1234）是每一处新钩子的强制形态。

---

## 2. 区域记录（Region Record）设计

### 2.1 概念模型

```
应用 mmap() 一次
   └─ Region Record（区域记录）        ← 应用可见的映射单元（本文新增, 取代 VMA 语义）
        start/end (2M 圆整, 与 arena 同界)
        class: ANON | FILE | RESERVED
        prot / may_prot（语义权限 + MAY 上界）
        file + pgoff（FILE 类）
        pieces（punch 后的多片形态, 渲染与所有权用）
        carrier（detached vma, §2.3）
        └─ 2M 窗格 × N（arena 描述符 + xarray 帧槽 + per-PTE metadata array）
             └─ 512 × struct corten_pte_meta（唯一真源, PS-B2）
```

- **Region 是窗格的父概念**: 一个 region 与一个 arena 描述符 **1:1**
  （DEV-12 的 per-mmap auto-arena 形态自 M4.T0 起, mm/corten_arena.c:2206-2301）;
  region 的 [start,end) 就是 arena 的 [ar->start,ar->end)（PMD 圆整, 应用尾差
  是内核私有 padding, release 规则覆盖, :6272-6284）。因此 region **内嵌于
  `struct corten_arena`**, 不新设第二棵结构（§2.4 论证）。
- **权限分层不变**: 页级权限在 metadata（mprotect 路由纯 meta 改写 +
  PTE 重编码, mprotect.c:917 路由先例）; region 的 `prot/may_prot` 只是
  **上界契约**（取代 shadow-VMA 的 R/W/X 位 + MAY 位: VMA 时代
  `ar->prot` 由 `corten_arena_prot_from_vma` 从 VMA 位提取, :517-529;
  零 VMA 后改由 mmap 的 prot 参数直接记录, MAY 位 = prot 的超集规则
  对齐 do_mmap 的 calc_vm_prot_bits 语义）。
- **三类**:
  - `CORTEN_REGION_ANON`: MODE auto-mmap 的匿名私有映射（V-A 全覆盖）;
  - `CORTEN_REGION_FILE`: MODE 进程 post-ENTER 的 `MAP_PRIVATE` file 映射
    （dlopen/JVM 附加库/locale; V-B）; **MAP_SHARED 永不入窗**（§3.2.4）;
  - `CORTEN_REGION_RESERVED`: **特殊区**——park 池预约窗（内容已清零、
    无 VMA、渲染上不可见, V-A.1 起）与杂志 sentinel 帧
    （CORTEN_FRAME_RESERVE, corten_arena.h:214 起）的概念归口。
    委托域 VMA 不是 region（§1.2）。

### 2.2 `struct corten_region`（签名级; 内嵌进 corten_arena）

```c
/* include/linux/corten_arena.h, struct corten_arena 追加（V-A.0） */
	enum corten_region_class rclass;   /* ANON / FILE / RESERVED */
	u8                      may_prot;  /* CORTEN_PERM_* 上界（MAY 语义） */
	u32                     rflags;    /* CORTEN_RF_* 见 §2.6 */
	struct file            *rfile;     /* FILE 类: 引用计数持有（get_file） */
	loff_t                  rpoff;     /* FILE 类: 映射起始页偏移 */
	unsigned int            npieces;   /* >1 = 被 punch 过的多片形态 */
	struct list_head        rpieces;   /* {start,end} 片段表; npieces<=1 时空 */
	struct vm_area_struct  *carrier;   /* §2.3: 不入树的 vma 载体 */
```

约定:
- `rclass==ANON/FILE` 时 `ar->prot` 语义不变（现有 perm 上界, DECLARE 时由
  attach 从 mmap prot 写入——V-A.2 后不再读 VMA 位）。
- `rflags` 初值来自 mmap flags 白名单反射; 现有白名单
  （corten_arena.c:2231 classify + M4T0_SPEC §3.1）不变, 只是**记录**余下的
  语义位供消费者查询（WIPEONFORK/DONTFORK 等, §2.6）。
- 生命周期 = arena 生命周期（DECLARE 建 / RELEASE 拆 / park 转 RESERVED /
  reactivate 转回）; fork 时子侧深拷贝（fork_commit 既有镜像扩展
  mm/corten_arena.c:3078）: file 引用 `get_file`、pieces 复制、carrier 由子侧
  新造（§3.1 V-A.2b）。
- `may_prot` 的引入同时闭合 M4T0 时代的一个已知空档: mprotect 路由对
  "超过 DECLARE prot 的升级"的 -EACCES 门从"ar->prot 猜"变为显式 MAY 上界
  （protect_range, mm/corten_arena.c:7002-7145, 现以 ar->prot 判定）。

### 2.3 载体对象（carrier）—— 零 VMA 的可行化关键

```c
/* mm/corten_arena.c, V-A.2b 新增 */
struct vm_area_struct *corten_arena_carrier_alloc(struct mm_struct *mm,
						  u8 prot, u32 rflags,
						  struct file *file,
						  loff_t pgoff);
	/* vm_area_alloc() + 手工初始化: vm_mm=mm, vm_start/end=region 界,
	 * vm_flags = VM_CORTEN|VM_NOHUGEPAGE|MAY 位|ANON|NORESERVE
	 *           （FILE 类再置 vm_file=rfile, 不置 VM_SHARED）,
	 * anon_vma_prepare()（rmap 锚, 现由 shadowize 做, :594）。
	 * **绝不** vma_link / 不进 mm_mt; mmap_lock(W) 下创建/销毁。
	 */
```

- **职责**: (a) rmap 锚——arena folio 的 `folio_add_new_anon_rmap(folio,
  vma, …)`（map_anon, :3473 附近）与 anon_vma 链需要 vma; (b) PTE 编码参数——
  `pte_mkwrite(pte, vma)`（pkey 位, map_anon :3734）、
  `corten_arena_perm_pgprot(vma, perm)`（:555-567, 现从 vma 取 base flags
  ——改从 carrier 的 MAY 位取, 语义等价）; (c) fork 的 `copy_page_range`
  （mm/memory.c:1096 起, DEV-14 胶水白名单第 1 处）按 vma 遍历; (d)
  folio 预分配的 gfp 上下文（`corten_arena_folio_prealloc`, :3381）;
  (e) V-B 的 i_mmap 参与（§3.2.5）。
- **稳定性**: 沿用 [FAIL-2] 论证（corten_arena.h:173-180）——事务持
  `ar->active` 引用期间 carrier 不可被释放（写方都在 mmap_write +
  drain 之后）。carrier 的 vm write mark（`vma_start_write`）在创建/销毁/
  fork-dup 时照常使用; 事务内只读字段（vm_flags 的 MAY 位、vm_mm、
  vm_page_prot 派生）不需要锁（与今天读 ar->vma 同一契约）。
- **不是什么**: 不在 maple 树、不被 find_vma 触达（J1）、不参与 vma_merge/
  split/munmap 漏斗（那些以树为前提）、不承担 VMA 锁的角色（mmap_lock +
  per-VMA lock 体系对窗口域不存在）。**它是 metadata 的宿主结构**, 数量
  = 区域数（池上限 16 + 活跃区域, CORTEN_ARENA_POOL_MAX=16,
  corten_arena.h:349）。

### 2.4 每 mm 区域注册表: 数据结构选择与论证

**选择: 扩展既有 per-mm 2M 帧 xarray（`state->arenas`）为区域注册表;
枚举 = `xa_for_each` + 指针去重。不新设 interval tree / 链表 / maple。**

| 候选 | 判定 | 论证 |
|---|---|---|
| **帧索引 xarray（选定）** | ✅ | region 与 arena 1:1（§2.1）⇒ 注册表已存在; 按地址查询 = `xa_load` O(1)（GUP-slow/mincore/审计的查询形态）; RCU+`kfree_rcu` 发布契约已验收（corten_arena.c:29-32）; 枚举 = `xa_for_each`+指针去重, 两个先例在生产路径（fork_unfreeze :2505-2518、mm_exit :1550-1573）。代价: 枚举复杂度 O(声明帧) 而非 O(区域)——sentinel 密度上界 = 杂志已领段（每 CPU 1GiB = 512 帧, corten_arena.h:206-212）; JVM+8vCPU 量级 ~10⁴, 冷路径可接受（mm_exit 已同型跑） |
| interval tree (rbtree) | ❌（记为演进路径） | 需要它唯一理由是**页粒度 region 边界**; 但本设计 region 界 = arena 界（2M 圆整）, 页级语义已由 metadata 承载（mprotect 不分片是 M4.T0 既有契约, maps 保真口径 = 全区段粒度, 与 shadow-VMA 时代逐字节相同——今天的 shadow-VMA 同样不被路由 mprotect 分片, M4T0_SPEC §3.3 "一个 shadow-VMA 不能携带子 VMA 权限"）。新建第二棵树 = 双结构一致性负担（INV7 ×2）零收益。**若未来要页粒度 maps 保真（OQ-MV-13）, augment rbtree 是加法升级**: 注册表接口收敛在 `corten_region_{lookup,next}` 两函数后, 换实现不动消费者 |
| 有序链表 | ❌ | 查询 O(n)（GUP-slow/审计每次全走）; JVM churn 下区域数百~数千 |
| maple tree | ❌（自我否定） | 那就是要移除的 VMA 层本体（D12: mm/mmap.c+vma.c+maple_tree ≈12.5k 行）; 用它 = 换皮不换骨 |
| 页粒度 xarray | ❌ | 1GB 映射 = 256K 槽 × 8B ≈ 2MB 纯指针, 内存形状不可接受 |

**注册表接口（V-A.0 起, 全部消费者只经这两个函数, 便于未来换内部实现）**:

```c
struct corten_region_iter { unsigned long frame; struct corten_arena *last; };
/* 按地址升序产出 region（帧槽同指针去重）; 持 mmap_read 或 RCU+lookup_get */
struct corten_arena *corten_region_next(struct mm_struct *mm,
					struct corten_region_iter *it);
struct corten_arena *corten_region_lookup(struct mm_struct *mm,
					  unsigned long addr); /* = 既有 lookup */
```

### 2.5 与既有结构的关系（一句话账）

- **帧槽/xarray**: 不变, 继续承载 arena 指针 + sentinel; region 字段搭车。
- **per-PTE metadata（corten_pte_meta, include/linux/corten.h:179-188）**:
  仍是唯一真源（PS-B2）; region 只记录"页级状态之外"的映射级语义
  （类/file/pgoff/上界/特殊位）——正是 VMA 曾承担的那部分。
- **park 池**: `ar->idle`（corten_arena.h:143-157）⇔ `rclass=RESERVED`;
  reactivate 改 `rclass` 并刷新 prot（V-A.1 起无 VMA 手术）。
- **委托域 VMA**: 与注册表无关; 由 J2 白名单断言封闭（§1.3）。

### 2.6 VMA 位语义编码完备性表（INV-MV3 的封闭清单）

零 VMA 后每个 `VM_*` 位必须有下落之一: **RF**（记入 rflags 供消费者查）、
**PERM/MAY**（perm/may_prot）、**REJECT**（auto 白名单拒绝, 维持现状）、
**DELEG**（只在委托域 VMA 上存在, 不需要编码）。任何新 VM_* 上游合并时
必须补此表（登记为评审 checklist 项）。

| VMA 位 | 下落 | 说明 |
|---|---|---|
| VM_READ/WRITE/EXEC | PERM | metadata perm（页级）+ region.prot（上界） |
| VM_MAYREAD/MAYWRITE/MAYEXEC | PERM | region.may_prot; mprotect 升级合法域 |
| VM_SHARED | REJECT | MAP_SHARED file/shm 永不入窗（shm 走 ipc/shm.c:1658-1662 legacy） |
| VM_ANON | PERM（恒有） | ANON/FILE 类由 rclass 表达 |
| VM_NORESERVE / VM_ACCOUNT | PERM（恒 NORESERVE） | DEV-12 既有; OVERCOMMIT_NEVER 整体降级 legacy（:2226-2229） |
| VM_SOFTDIRTY | RF（可观测） | 现状即有已登记分歧: arena fault 不置 PTE soft-dirty（map_anon :3730-3734 无 soft-dirty 位）——V-C 显式文档化, 行为不变（OQ-MV-5） |
| VM_NOHUGEPAGE | PERM（恒有） | carrier 恒置; THP/khugepaged 结构性排除升级为"窗口域无 VMA ⇒ 结构不可达"（khugepaged.c:910/2417 的 for_each_vma 在空树窗口无所获） |
| VM_DONTCOPY / VM_WIPEONFORK | RF | fork 镜像读取（fork_copy_window :2838 起）; 白名单现拒绝映射带此位, RF 为将来放行预留 |
| VM_DROPPABLE | REJECT | T0 白名单既有（M4T0_SPEC §3.1） |
| VM_LOCKED/VM_LOCKONFAULT | REJECT | mlock 路由拒绝（mlock.c:645/788 range_overlaps）+ def_flags VM_LOCKED 拒绝（pool_take :6116-6120） |
| VM_IO/VM_PFNMAP/VM_MIXEDMAP | REJECT | 非常规映射不入窗 |
| VM_HUGETLB | REJECT | 白名单既有 |
| VM_UFFD_WP/VM_UFFD_MINOR | REJECT | OQ3 既有（shadowize 白名单 :535、uffd 路由拒绝） |
| VM_PKEY_* | REJECT | arch 快钩已旁路 PK fault（fault.c:1352-1355 注记） |
| VM_SHADOW_STACK | REJECT | 白名单既有 |
| VM_SEQ_READ/VM_RAND_READ | RF | file 区提示位; madvise hints 档同型 no-op（:6913-6916） |
| VM_GROWSDOWN/GROWSUP | DELEG | 只属栈/brk（委托域） |
| VM_SEALED | REJECT | mseal 路由拒绝（mseal.c:177）; region 不携带 seal 语义（OQ-MV-9） |
| VM_SVM / 厂商位 | REJECT | Android 厂商位不入窗（白名单穷举形态已覆盖: 未列位即拒） |

### 2.7 锁序（无新锁, INV2 不扩边）

region 字段与 carrier 的全部写方在 `mmap_lock(W)` 之下（do_mmap 路由、
munmap/mprotect/mremap 路由、fork begin/commit、exit）; 全部读方要么持
`mmap_lock(R)`（GUP-slow/proc/mincore/mseal/mlock 检查）, 要么走 RCU +
`lookup_get` + `active` 引用（fault/枚举）——**区域记录不是锁, mmap_lock
就是它的锁**。既有序列
`mmap_write > ctl_lock > drain-wait > [folio_lock] > [swap 锁] > fill_lock
> desc->lock(W,BH) > ptl`（M6_RMAP_SPEC D4）零新增边。carrier 的 vm write
mark 只在 mmap_write 下获取（与 shadowize 今天对 shadow-VMA 的做法一致,
:602）。

---

## 3. 分阶段规格（V-A → V-E）

每阶段统一栏目: 接口签名 / 锁序 / KUnit 锚 / guest 判据 / diff 预估 / 风险,
并附该阶段对语义保持矩阵（§3.0）的增量说明。

### 3.0 语义保持矩阵（全部 VMA 消费者 × 现状 × 终态 × 生效阶段）

"现状"= HEAD 4ca6ef1048f0; "终态"= V-E 收口后; 生效阶段 = 该行定性改变的
最早切片。file:line 除注明外为上述基座实测。

| # | 消费者（入口, 6.18 事实） | 现状处理 | 终态处理 | 阶段 |
|---|---|---|---|---|
| C1 | 用户 fault 快路径: `do_user_addr_fault`（arch/x86/mm/fault.c:1362-1382 钩; :1386 `lock_vma_under_rcu`→find_vma） | 窗口地址经快钩查帧表, 零树走查; miss→FALLBACK→legacy | 不变（钩子已是 metadata 驱动）; park 区间 fault: xarray 有帧但 `idle`→lookup 不可见（corten_arena.h:150-152）→FALLBACK→无 VMA→`bad_area_nosemaphore` MAPERR（**登记语义变更 S-1**: 原 PROT_NONE 预约=ACCERR, 现=MAPERR, 与真 munmap 一致且更正确） | V-A.1 |
| C2 | fault 慢路径: `handle_mm_fault` 钩（mm/memory.c:6561-6582, VM_CORTEN 位门） | shadow-VMA 命中→事务 | 载体门: 调用方传入的 vma 即 carrier（GUP-slow/ptrace 经 V-C 的 MODE 分支拿到 carrier）; 无变化在读侧 | V-A.2b |
| C3 | mmap 正面: `do_mmap`（mm/mmap.c:427-454 auto 路由; :460 `__get_unmapped_area`; :465 MAP_FIXED_NOREPLACE 查树; :490-498 MAP_FIXED 路由; :659-660 attach） | 路由改写 MAP_FIXED→mmap_region 建 shadow-VMA→attach | cret==1 分支不再调 mmap_region: 路由内置验证清单（security_mmap_file/may_expand_vm 口径/mlock_future_ok→fallback）+ 直接 declare（carrier+帧+meta）+ 返回 addr; cret==2（池）不变; MAP_FIXED 植入路由不变 | V-A.2a |
| C4 | munmap 三层: `__vm_munmap`（mmap.c:1209 路由）、`do_vmi_align_munmap`（vma.c:1599; guard mmap.c 守卫; `vms_gather_munmap_vmas` vma.c:1414）、`corten_arena_munmap_vma_guard`（corten_arena.c:6390-6405） | EXACT→park/RELEASE（park 留 PROT_NONE VMA）; CHUNK→事务 zap | EXACT→park **删 VMA**（§3.1.1）; RELEASE 删 VMA 同型; CHUNK/穿透守卫不变（对无 VMA 窗口, guard 的 for_each_vma 空转=放行, 正确: 无 VMA 可被 legacy zap） | V-A.1 |
| C5 | mprotect/pkey: `do_mprotect_pkey`（mprotect.c:895 写锁; :916-931 路由/拒） | 路由: protect_window 改 meta+PTE 重编码（`corten_arena_protect_window` :6884, perm_pgprot 读 shadow-VMA base） | 同一路由; perm_pgprot 改读 carrier MAY base（纯函数改参）; 升级合法性由 may_prot 判定 | V-A.3 |
| C6 | mremap: `do_mremap`（mremap.c:2005-2012 路由） | move=新窗+copy_to_user+RELEASE 旧区（`corten_arena_mremap_move` :7273）; shrink 尾 zap; VMA 手术仅经 punch | move/shrink 已 metadata 化; grow/shrink 不再有 VMA 邻接检查可依赖——mremap 内部 do_munmap/mmap 对窗口的兜底守卫（munmap_vma_guard）空转=放行, 正确性由"窗口无 VMA+帧表所有权"承载; 植入例外照旧 | V-A.3 |
| C7 | madvise: `do_madvise`（madvise.c:1940-1961 路由; hints 档 corten_arena.c:7661-7664, 默认拒 :7697） | DONTNEED/FREE 路由 zap 留 VA; hints no-op; 其余拒 | 不变（全 metadata）; DONTFORK/WIPEONFORK 对 region 改 RF 位（原生无 VMA 可改）→ 由拒绝改 RF 记录（行为等价: fork 镜像读 RF）; OQ-MV-8（PAGEOUT→shrinker 路由, 依赖 OQ-M6-7） | V-A.3 |
| C8 | brk: `sys_brk`（mmap.c:123）/`do_brk_flags`/`vm_brk_flags`（:1345） | legacy + 窗口 guard 后备 | **委托域保留**（V-E.1 推荐, §3.5）; guard 后备继续服务越界假设 | V-E.1 |
| C9 | mlock/mlockall: mlock.c:645/788 `range_overlaps` 拒 | 拒绝（VMA-free 检查, 已成立） | 不变; 零 VMA 使"mlock-on-window→-ENOMEM"假象不出现（检查先于 find_vma）——保持 -EOPNOTSUPP 口径, strace 等价 | 不变 |
| C10 | mseal: mseal.c:177 拒 | 拒绝 | 不变（同 C9 形态） | 不变 |
| C11 | mincore: mm/mincore.c:225/250 walk_page_range（pagewalk.c:495/511 find_vma 界定） | shadow-VMA 存在→正常枚举 | MODE 分支: 按 region + PT 直走（复用 shrink walk 形态 :8247）; 或登记 -ENOMEM 语义变更（OQ-MV-11 先走登记, V-C 收口路由） | V-C |
| C12 | userfaultfd: mm/userfaultfd.c:42/87/127/1572（全部 find_vma 系）+注册（:2006 经 mseal/madvise 形态拒绝） | 拒绝（白名单+路由） | 不变; 空树使 uffd 查询对窗口天然 -ENOENT; 保持显式拒绝口径（strace 等价） | 不变 |
| C13 | /proc/maps + PROCMAP_QUERY: fs/proc/task_mmu.c:313 m_start（maple 遍历）、:538 show_map、:682 查询 ioctl | shadow-VMA 被当普通匿名 VMA 渲染 | **双源合并渲染**: maple（委托+植入）∪ 区域注册表（窗口）; 共享条目渲染核心; smaps/rollup 用 corten PT 走查器聚合 RSS/PSS/Swap（复用 :8247 shrink walk 形态） | V-C |
| C14 | /proc/pid/clear_refs（soft-dirty 重置, task_mmu.c 同文件族） | 对 shadow-VMA 范围走 PT | 窗口: 无 VMA→跳过（PTE 本无 soft-dirty 位, 现状分歧 S-2 保持）; 登记文档 | V-C |
| C15 | GUP-slow: mm/gup.c:1973-1975/2041 find_vma 系、:1213/:1420/:1441 check_vma_flags、:1234 corten_own 门 | corten_own 让 GUP 信任 shadow-VMA R/W 位+fault 门下放 | **MODE 分支**: find_vma miss 且窗口→region 解析+check 语义仿真（perm/may/IO/PFN 表）→fault 经 :6561 钩（carrier ctx）→按 **metadata** 的 can_follow_write 形态抓 folio（镜像 gup.c:598 can_follow_write_common, 判据从 VMA 位换 meta perm+SHARED） | V-C |
| C16 | GUP-fast: `internal_get_user_pages_fast` 页表直走（无 VMA, gup.c:2857 起） | 天然工作（M5_FORK_SPEC §4.1 已证） | 不变; 回归矩阵（io_uring/9p/process_vm_readv 四态） | 不变 |
| C17 | rmap/ttu: try_to_unmap_one 守卫（rmap.c:1895-1911, `corten_rmap_unmap_one` :7803/`corten_rmap_swap_out` :7864）+ migrate 守卫（:2351-2357）+ syscall 拒（migrate.c:2647-2650） | shadow-VMA 上 VM_CORTEN 位触发守卫 | carrier 带 VM_CORTEN ⇒ 守卫逐字工作（anon_vma 链仍经 carrier）; **不破不修**; NUMA balancing（fair.c:3629 change_prot_numa）对窗口结构不可达（无 VMA）→ V3 违规自熄 | 不变（A.2b 验证） |
| C18 | swap out/in: shrinker（corten_arena.c:8482/8545/8659）、swapin 同步（swapfile.c:2210-2224 `corten_swapin_sync_meta`）、swapoff unuse（swapfile.c:2436/2457-2468 unuse_vma/unuse_mm） | shrinker 走 PT 不走 VMA; unuse 只对有 VMA 范围 | shrinker 不变; **unuse: 窗口 swap PTE 的换回**需 MODE 分支（unuse_mm 对无 VMA 范围盲）→ V-B/V-C 后置切片复测; 现 swapoff 对窗口=盲（行为同今天 shadow-VMA? 不, 今天 unuse 经 shadow-VMA 走到）→ **登记 V-A.1 起的语义变更 S-3**: swapoff 期间窗口换入改由 fault 慢车道兜底（do_swap_page 系经 fault 门; unuse 的提前换回优化丢失, 计数披露）——OQ-MV-6 深化 | V-A.1 登记, V-D 复测 |
| C19 | OOM: killer/`__oom_reap_task_mm`（oom_kill.c:577-581 skip 钩; :590 unmap） | 逐 VMA skip | reaper 树遍历只见委托域→照常 reap 委托页, 窗口零触碰（与 skip 等价且更干净）; SIGKILL→exit 走 V-D | V-A.1 |
| C20 | THP/mTHP/khugepaged: huge_mm.h:335 门、khugepaged.c:910/2417 | VM_NOHUGEPAGE 单开关 | 结构性排除（窗口无 VMA）; fill_upper 的 pmd_leaf 防御探测保留（:3257 附近, 防委托越界假设） | V-A.2a 验证 |
| C21 | KSM: ksm.c:1192/2625 for_each_vma | MADV_MERGEABLE 被拒（:6949） | 不变; 空树零扫描成本 | 不变 |
| C22 | hwpoison: memory-failure.c:1595 `corten_arena_hwpoison_check`（:7706 WARN+拒） | 拒绝 | 不变（folio 驱动, 非 VMA 驱动）; ttu_hwpoison 守卫臂继续拒绝（rmap.c:1895 家族） | 不变 |
| C23 | fork: dup_mmap（kernel/fork.c:1275 for_each_vma + copy_page_range）; 钩子 mmap.c:1902 fork_begin / :2024 fork_commit / :2039 abort | PTE 层靠 shadow-VMA 在树内被 copy_page_range 复制; meta 层镜像 | **PTE 复制搬进 corten**: fork_commit 内按区域 carrier 对调 `copy_page_range(dst_mm, dst_carrier, src_carrier)`（白名单 #1 形状不变, 锁序 mmap_write 下不变）; 树循环只见委托域（照常复制）; 子侧 carrier/fork_register_child 扩展（:2743） | V-A.2b |
| C24 | exec: binfmt_elf `elf_map`/setup_arg_pages（exec 期, ENTER 前） | legacy mmap | 委托域不变; ENTER 后 exec 重建 mm → MODE 位消失（既有语义, M4T0_SPEC §6） | 不变 |
| C25 | perf: `perf_mmap`（kernel/events/core.c:7144, 调用者自身 mm 的 ring buffer, MAP_SHARED file）; perf 工具读 /proc/maps | ring buffer=legacy; 工具读 shadow-VMA 渲染 | ring buffer 不变（MAP_SHARED 永不入窗）; 工具读 V-C 双源渲染——perf 对 MODE 目标的符号/映射保真 = C13 的下游 | V-C（工具验证） |
| C26 | shm: ipc/shm.c:1658-1662（shmat→do_mmap file） | legacy | 永不入窗（MAP_SHARED file, §3.2.4）; shmat 落委托域 | 不变 |
| C27 | teardown: exit_mmap（mmap.c:1393; :1407 mm_exit; :1427 unmap_vmas; :1439 free_pgtables; mmu_gather 家族） | shadow-VMA 在树内→unmap_vmas/free_pgtables 拆表 | **V-D**: mm_exit 扩展为纯 PT 走查（逐窗 zap + 上层表释放 + carrier 释放）; 树循环只拆委托域; mm_users==0 carve-out 维持 | V-D |
| C28 | vmscan/MGLRU: vmscan.c:3623 folio_lru_gen 门、:3717 young 清位、shrink_folio_list 输入=LRU（:1822 隔离） | arena folio 永不入 LRU（DEV-10） | 不变; lru_gen 的 mm walk（pagewalk.c:495 find_vma 界定）对窗口零 VMA→零访问（今天亦零, folio 门先灭）; shrinker 专属通道不变 | 不变 |
| C29 | PR_SET_VMA/mbind/move_pages/process_madvise | range_overlaps 系拒绝（migrate.c:2650 等） | 不变（全部 VMA-free 检查, 零改动满足终态） | 不变 |
| C30 | 外部观察者: seccomp（syscall 级, 与 VMA 无关）、gVisor/调试器（GUP+proc）、strace | proc/GUP 保真 | C13/C15 终态 + 登记分歧集 S-1/S-2/S-3; bpftrace oracle | V-C |

登记的**语义变更集**（每条都有 strace/观察者可见面, 全部走 REPORT 披露）:
- **S-1**（V-A.1）: park 区间访问 ACCERR→MAPERR（与真 munmap 对齐; si_addr 不变）。
- **S-2**（既有, V-C 文档化）: 窗口页 PTE 无 soft-dirty 位——clear_refs/增量脏跟踪
  对窗口不可见（今天已如此, 只补文档）。
- **S-3**（V-A.1 登记, V-D 复测）: swapoff 提前换回对窗口盲——换入走 fault 慢车道;
  swapoff 完成性由 shrinker+fault 双通道保证（swap entry 引用计数闭合）。
- **S-4**（V-A.1）: /proc/maps 中 parked 预约消失（今天渲染为 PROT_NONE 匿名段;
  应用视角=已 munmap 的区间, 消失才是正确渲染; J3 oracle 的 parked 例外档）。

---

### 3.1 V-A: 零 VMA 匿名内存

#### 3.1.0 V-A.0 准备片: 区域记录落位（零行为变更）

- **接口**: §2.2 struct/§2.4 迭代器; `rclass` 初值映射: 活跃=ANON、
  `idle`=RESERVED（park/declare 两处写点）; debugfs `arenas` 渲染增列
  （rclass/rflags/pieces）。
- **锁序**: 无新锁（§2.7）; 写点全在既有 mmap_write 下写点旁。
- **KUnit**: region 编/解码与 rflags 真值表; 迭代器去重（合成注册表:
  多帧同指针 + sentinel 跳过 + 空表）; INV-MV2 形态的白名单审计 walker
  （委托/植入/窗口三分, 合成 mm 注入三类样本）。
- **guest 判据**: 全套既有回归零变化（=y KUnit 三套件、=n 八对象、run13
  fails=0、26/26 smoke）; debugfs 新列可见。
- **diff 预估**: ~+350（arena.h/arena.c/test）。
- **风险**: 纯结构搬运, 低; 唯一注意 `idle`⇔RESERVED 双写点的一致性
  （park_locked :5994 与 reactivate :5808/pool_take :6180）。

#### 3.1.1 V-A.1 park 去残留: PROT_NONE 预约 VMA 消失

- **语义**: munmap EXACT 的 park 不再保留任何 VMA——zap+meta 复位后
  （`corten_arena_unmap_chunk_flags`, :5982）, 把 shadow-VMA **从树中删除**
  （复用 RELEASE 的拆除下半场: unshadow 后 `do_munmap`, 形状同
  release_arena_locked :1040-1080（do_munmap 1058）, 但**不擦帧、不 free 描述符**）; 窗口
  留下"纯预约": 帧槽（idle 描述符）+ 无 VMA + 无 PT 页。
- **接口**:
```c
/* mm/corten_arena.c */
static int corten_arena_park_unmap_vma(struct mm_struct *mm,
				       struct corten_arena *ar);
	/* 前置: ar 已 idle、drain 完成窗口已栅栏; mmap_write + ctl_lock 下。
	 * do_munmap 删预约 VMA; 失败=退化为真 RELEASE（计数）。
	 */
```
  pool 侧同步手术: `pool_parkable`（:5915）与 `pool_take/reactivate`
  （:6103/:5792）删除全部 vma_lookup/shadowize/vm_flags 手术——
  reactivate = `rclass=ANON + ar->prot 刷新 + 发布`; `pool_prepare`
  （:5842）的 VMA 校验臂换成 `[C1]` 纯 PT 判空（:729 已是 PT 驱动,
  删除 vma_lookup 依赖）。`do_mmap` 的 cret==2 早退路径（mmap.c:433-446）
  文本不变, 语义自然变为"纯 metadata 复活"。
- **消费面连锁**（矩阵 S-1/S-3/S-4 生效）: fault 对 park 区间→MAPERR（C1）;
  oom_reaper 树循环天然不见（C19）; unuse 盲区登记（C18）; J3 oracle 记
  parked 例外。
- **锁序**: park 序列已在 `mmap_write > ctl_lock` 下（pool_release :6033-6034）,
  do_munmap 为其既有形状（release_arena_locked 同位）; 无新边。
- **KUnit**: park→断言 maple 树无该区间 VMA（合成 mm 用 mt_test 形态枚举）、
  帧槽存活、fault 分派 MAPERR 臂、pool_prepare 判空路径、eject（:5751）
  无 VMA 残留假设、`[C1]` 真空窗复用。
- **guest 判据**: ds_dontneed 三形态 churn（STATE §7.3 口径）+ run13 fails=0;
  debugfs `pool_parks==pool_hits` 对账; dmesg 静默; maps 差异抽样（parked
  消失=S-4 预期形状）。
- **diff 预估**: ~+450/−280（arena.c 为主, 测试 +250 计入切片表）。
- **风险**: do_munmap 失败臂（内存压力）必须落真 RELEASE（已预案）; unuse
  盲区（S-3）登记+swapoff 回归用例。

#### 3.1.2 V-A.2 auto-attach 不建 VMA（A.2a 验证与路由 / A.2b 载体与 fork）

**A.2a —— 正面建窗零 VMA**:
- **语义**: `corten_arena_auto_mmap_route` ret==1 分支（:2296-2300）不再要求
  do_mmap 走 MAP_FIXED/mmap_region; do_mmap 在 :447-453 处直接:
```c
/* mm/mmap.c, do_mmap() */
	if (corten_auto_arena) {
		/* V-A.2a: placement+validation+declare 已在路由内完成
		 * （carrier+帧+meta）; 无 VMA、无 mmap_region。 */
		return addr;
	}
```
- **移植的验证清单**（原由 mmap_region 承担, 逐项收口; 全部 VMA-free）:
  1. `security_mmap_file(NULL, prot, flags)`（LSM; 铁律——SELinux/AppArmor
     的 mmap 钩不可因零 VMA 失明; do_mmap 原路径在 :508 前 file 分支,
     anon 路径今天也没调——**修正既有缺口**, 计数披露）;
  2. RLIMIT_AS: `may_expand_vm` 口径（mmap.c:1472-1495）手工等价
     （total_vm += len2>>PAGE_SHIFT + vm_stat_account 同位记账）;
  3. `mlock_future_ok`/def_flags VM_LOCKED → fallback（pool_take :6116 同款）;
  4. MAP_FIXED_NOREPLACE/显式地址: 不可达（白名单要求 addr==0）;
  5. pkey/hugetlb/shadow-stack 位: 白名单既有拒绝;
  6. OVERCOMMIT_NEVER: 既有整体降级（:2226-2229）。
  清单做成纯函数 `corten_auto_validate()`（KUnit 真值表）。
- **A.2b —— carrier 与 fork**（§2.3）:
  - carrier_alloc（签名 §2.3）; DECLARE/attach/declare_locked（:779）改用;
  - fault ctx: `ctx.vma` 全部改指 carrier（get_vma :3663、map_anon :3701、
    restore/cow/swapin 同族）——`fault_owned`（:3305）的 F-A VMA 自检改为
    "帧在册 ∧ 地址⊂窗口"（植入映射已被 punch 擦帧, 自检前提消失, 简化）;
  - **fork PTE 复制**:
```c
/* mm/corten_arena.c, fork_commit 内（:3078 现位之后、解冻前） */
static int corten_arena_fork_copy_ptes(struct mm_struct *mm,
				       struct mm_struct *oldmm);
	/* 逐区域（父侧 xa_for_each）: copy_page_range(mm, child_carrier,
	 * parent_carrier)。前置: fork_begin 冻结窗已静止 PTE（DEV-15
	 * 不变）; copy_page_range 的 wrprotect=胶水白名单 #1（DEV-14
	 * 形状逐字保持, mm/memory.c:1096-1099）; 委托域由 dup_mmap
	 * 树循环照常（kernel/fork.c:1275）。失败: 既有 loop_out→
	 * fork_abort unwind（:2533）+ 子侧走 MMF_UNSTABLE 清场。 */
```
  语义对拍: 与 M5_FORK_SPEC §1.3 逐位等价（GUP-pinned 父页给子拷贝、
  PageAnonExclusive 清除、软脏/uffd 位处理全部白得——因为复用同一
  copy_page_range, 只是驱动 vma 从"树内 shadow"换成"carrier"）。
- **锁序**: 全部在 dup_mmap/auto 路由的 mmap_write 下; carrier 的
  anon_vma_prepare 在 DECLARE（今 shadowize 同位 :594）; fork 侧父/子
  desc 树锁不嵌套（M5_FORK_SPEC §1.3 ④-4 纪律不变）。
- **KUnit**: corten_auto_validate 真值表（≥16 例）; carrier 生命周期
  （declare/release/park 三态）+ [FAIL-2] 稳定性注入; fork_mirror 扩展:
  合成窗 fork→子 meta/PTE/RF 三方对拍（M5 判据 1 的 KUnit 版）; dispatch
  在 ctx.vma=carrier 下四态（fresh/restore/cow/swapin stub）。
- **guest 判据**: 26/26 smoke + JThreadBench 3×rc=0 + metis_eq checksum
  三方同值; strace diff 除地址外零新错误（T0 DoD-4 口径）; lat_proc G5
  复测（预期 mmap/fork 微改善, 非门）; `auto_mmaps>0 ∧ attach_fails==0`。
- **diff 预估**: A.2a ~+450/−120; A.2b ~+600/−260。
- **风险**: 最大单项 = fork PTE 复制搬家（R2, §4.2）; LSM 钩子引入是行为
  修正（披露）; carrier 的 anon_vma 链与 ttu 守卫联动需一次专项走查
  （C17, rmap.c:1895 家族在 carrier 上逐 flag 对拍）。

#### 3.1.3 V-A.3 路由去 VMA 化扫尾

- protect_window/perm_pgprot 改 carrier/MAY base（C5）; mremap 路由的
  VMA 假设清理（C6）; madvise RF 位（C7）; 植入白名单审计钩（J2 walker
  挂 debugfs 触发）; J1 审计计数器（find_vma 系入口, §1.3）。
- **KUnit**: perm_pgprot 纯函数矩阵; 白名单审计 walker 注入违例样本;
  J1 计数器开合。
- **guest**: run13 + churn + `find_vma` 窗口命中计数==0（bpftrace 双验证）
  ——此片即 J1/J2 的首次全绿点。
- **diff 预估**: ~+400/−150。
- **风险**: find_vma 审计计数器的取址开销（仅 static-branch 下, MODE mm
  才做窗口比较; legacy 零付——测量后若可测回退, 降级为 kprobe-only 口径）。

---

### 3.2 V-B: file 映射非 VMA 化

#### 3.2.1 范围与白名单（先划界）

- **收**: MODE 进程 post-ENTER 的 `mmap(NULL, len, prot, MAP_PRIVATE, fd,
  off)`（addr==0 匿名路由的 file 镜像）→ `CORTEN_REGION_FILE`。典型:
  dlopen 的 .so 文本段、JVM 附加库、locale/gconv 文件。
- **永不收**: `MAP_SHARED` 一切（shm、perf ring buffer、共享库真共享形态）
  ——论文有共享状态（PS-B2 SharedFileMapped）但共享一致性（i_mmap 写侧、
  msync、clean/dirty 回写）是第二卷工程; **MAP_FIXED file**照旧植入路由
  （D-G''）; `MAP_EXEC` 可写文件（denywrite 语义, mmap.c:534-539）拒绝;
  DAX/fsdax（FOLL_LONGTERM 拒, gup.c:1242 同源）拒绝。
- **MAY 边**: file 区 may_prot = prot ∧ file 打开模式允许集（镜像
  mmap_region 的 file 校验链, mmap.c:508-539 的 MODE 版纯函数）。

#### 3.2.2 数据与事务

- region: `rfile=get_file(file)`, `rpoff=pgoff`; carrier: `vm_file=rfile`
  （**不置** VM_SHARED）。
- **fault 分派**（`CORTEN_FILE_MAPPED`, include/linux/corten.h:101 状态点亮;
  今为 STUB, M6_RMAP_SPEC §1.1 "无生产者"）:
```
read fault : 事务内 query=FILE_MAPPED → 解锁 → 锁外 pagecache 取页
             （filemap_get_folio + 页内读缺失时 readahead/同步读, 锁外可睡）
             → 重锁 re-query → ptl 内 set_ptes(clean, perm 编码)
             → meta 不变（FILE_MAPPED 即驻留形态; __resv 不需要: pgoff 可
               由 region.rpoff+页内偏移推导, 页级无 slot 负载）
write fault: COW → 复用 cow_write 拷贝分支（:3979）: 私有 folio +
             copy_user_highpage + meta 迁移 FILE_MAPPED→MAPPED
             （状态机"any→MAPPED"合法, corten.h:408 注记）
beyond EOF : filemap 口径 SIGBUS（VM_FAULT_SIGBUS→MAPERR 族登记）
```
- **zap/unmap/mprotect/release**: zap_window 通用（私有 clean 页 folio_put
  即可, 无脏回写——MAP_PRIVATE 语义）; mprotect 纯 meta（PTE 重编码沿
  protect_window 通用臂）; RELEASE/park: file put（最后一个引用）。
- **fork**: copy_page_range 对 FILE_MAPPED 页=普通 file COW 复制（白名单 #1
  形状不变）; meta 镜像加 `get_file`（子 region 引用）。

#### 3.2.3 truncate/invalidation —— V-B 的红线要点

6.18 的 `vma_link→__vma_link_file`（mm/vma.c:1881-1894, :256-265）把**所有**
file VMA（含 MAP_PRIVATE）插入 `mapping->i_mmap`; `unmap_mapping_range`
（truncate/evict/invalidate 路径）会沿 i_mmap 命中并 zap——**包括私有 COW 页**
（even_cows）。因此:
- FILE 区 **carrier 必须参与 i_mmap**（i_mmap_lock_write + interval_tree
  insert/update/remove, 全在 mmap_write 下的 region 变更点）;
- 由此 `unmap_mapping_range` 成为**新的窗口 PTE 写者家族**——违反 INV6
  （胶水白名单 3/3 已满, STATE M6.T2 收口登记）。**必须路由, 不得裸写**:
  在其 zap 漏斗（`zap_page_range_single`, mm/memory.c）前置
  `corten_enabled_static() && 窗口重叠` 门 → 转发
  `corten_arena_unmap_chunk(mm, ar, start, len)`（事务 zap, content-drop
  语义 = truncate 的 VA 保留; KEEP_PERM=false 使 re-fault 重读文件——
  截断后再访问 re-fault→EOF 外 SIGBUS, 与 legacy 逐位一致）。
  此门与 D-G'' `__mmap_prepare` backstop（mmap.c:482-488 注记）同型,
  记 INV6 **route-only 扩展**（非第 4 处白名单写点）。

#### 3.2.4 不做（显式边界）

MAP_SHARED 文件/shm/msync/writeback 回写通道（委托域）; 共享匿名
（CORTEN_SHARED_ANON 仍无生产者）; WQ/糞 file_ops 特例（prctl/设备,
白名单拒绝链不变）。

#### 3.2.5 栏目

- **接口**: `corten_arena_auto_mmap_route` classify 扩展（file+PRIVATE+
  addr==0）、`corten_arena_file_attach`（get_file/carrier i_mmap 注册/
  declare）、dispatch FILE_MAPPED 两臂、truncate 门、release/park 的
  file put。
- **锁序**: pagecache I/O 全在 desc 锁外（M6 D4 同构: "I/O 与分配在锁外,
  事务内只 PTE/meta"）; i_mmap_lock 嵌在 mmap_write 下、desc 锁外
  （truncate 门先于任何 desc 锁; 路由转发时 desc 锁内不持 i_mmap——
  对拍 shrinker 先例）。
- **KUnit**: FILE_MAPPED 分派矩阵（read/write/EOF/COW/双 fault 竞争合成）;
  truncate 门路由臂（注入 unmap_mapping_range 形状→断言事务路径计数、
  零裸写）; file 引用计数对账（declare/release/fork/park 四态）; classify
  扩展真值表。
- **guest 判据**: JVM 完整启动含 CDS（D-G'' 关闭态不回退）+ dlopen 繁重
  workload（psearchy_eq/locale 繁多形态）checksum; 截断-重读矩阵（open/
  mmap/read/truncate/读→SIGBUS）与 legacy 臂 diff; perf2a churn 回归。
- **diff 预估**: ~+900/−80。
- **风险**: pagecache 锁外取页与并发 truncate 的经典竞态=do_read_fault
  同构（锁外取页+锁内 re-query, 图 7 契约）; i_mmap 参与使 carrier 进入
  rmap-walk 视野→C17 守卫逐 flag 复核（HWPOISON/迁移拒绝臂必绿）。

---

### 3.3 V-C: /proc/maps + GUP + perf 从 metadata 枚举（MODE 进程）

#### 3.3.1 /proc 双源渲染

- `m_start/m_next`（task_mmu.c:313）改为对 MODE mm 的**两源归并游标**:
  maple 流（委托+植入）∪ `corten_region_next` 流（窗口）, 按 vm_start 归并;
  条目渲染核心抽出 `procmaps_render_entry(seq, start, end, prot, off,
  flags, file, name)`（shadow/委托/region 三源共用, 消除三份格式化代码）。
- region 条目形态与今天 shadow-VMA 渲染**逐字节对齐**（匿名段:
  偏移 0、`rw-p`、dev 00:00、ino 0、名字 `[anon:corten_arena]` 沿用
  CORTEN_ARENA_VMA_NAME, arena.c:87-89）; FILE 区带真 file/off/路径。
  parked 不渲染（S-4）; 植入/委托照旧。
- smaps/smaps_rollup: 窗口范围由 corten PT 走查器聚合（RSS/PSS/
  Anonymous/Swap: 驻留=MAPPED PTE、Swap=SWAPPED meta 计数——复用
  `corten_mm_state_pages`（:8193）与 shrink walk（:8247）的行进骨架,
  新增 PSS 桶）。
- `PROCMAP_QUERY`（:682 起）: 覆盖/下一条解析进归并游标。

#### 3.3.2 GUP-slow MODE 分支

```c
/* mm/gup.c, __get_user_pages() find_vma 臂前置 */
struct vm_area_struct *corten_gup_probe(struct mm_struct *mm,
					unsigned long addr,
					unsigned int gup_flags);
	/* MODE ∧ 窗口: region 解析 + check_vma_flags 语义仿真
	 * （perm/may、VM_IO/PFNMAP=恒否、FOLL_FORCE 语义 = M5.T3/D12
	 * 既有裁决: RO 契约外部写→响亮 EFAULT）; 命中返回该区 carrier
	 * （后续 fault 钩/follow 全部吃 carrier）; 未命中返回 NULL →
	 * legacy。返回 ERR_PTR 遵循 check_vma_flags 错误码。 */
```
- 抓页判定: can_follow_write 从 VMA 位换 **meta**（perm 写位 ∧ !SHARED ∨
  PTE exclusive）——镜像 can_follow_write_common（gup.c:598-620）的
  接受集, M5.T3 已证两者对齐; faultin 走 :6561 慢钩（carrier ctx）。
- fixup_user_fault/process_vm_readv/proc-pid-mem: 全部经此分支, 无独立路径。

#### 3.3.3 perf 与工具面

- 内核侧零改动（perf_mmap=MAP_SHARED file, core.c:7144; 采样符号化靠
  C13 的 maps 保真）。交付 = 工具链验证: perf record/report 对 MODE JVM
  （JIT 段在窗口）符号率与 shadow 时代基线对齐; bpftrace J1 复核;
  top/pmap/smaps sanity。

#### 3.3.4 栏目

- **KUnit**: 归并游标（合成 maple+region 交叠/相接/空洞 8 形态→期望序列）;
  render_entry 字节对拍 fixture; corten_gup_probe 判定矩阵（R/W/PIN/
  FORCE × perm 四态, M5.T3 状态锚复用）; PROCMAP_QUERY 解析。
- **guest 判据**: **maps 逐字节 oracle**（同一 workload: shadow 内核
  bzimg/r07-integrated vs 本片, maps 输出 diff=空, parked 例外档）;
  io_uring/9p/process_vm_readv 四态矩阵（M5.T3 判据复用）; bpftrace
  find_vma 窗口命中==0; J1 计数器==0。
- **diff 预估**: 内核 ~+750/−60（task_mmu.c 是首个非 mm/ 核心的改动面,
  review 焦点）; bench 侧 oracle 脚本 ~200。
- **风险**: proc 渲染格式的字节级回归（R3）; GUP 分支与 FOLL_FORCE 的
  D12 分叉保持（不引入新放行）; mmap_read 下 region 枚举的锁面复核
  （task_mmu 持 mmap_read——xa_for_each 只读帧槽, 安全）。

---

### 3.4 V-D: exit/teardown 纯 PT 走查释放

- **语义**: mm_users==0 时（carve-out 与今天 mm_exit 相同, mmap.c:1407→
  arena.c:1515）, 窗口的 PT 拆除不再依赖树内 shadow-VMA 被 unmap_vmas
  （mmap.c:1427）/free_pgtables（:1439）扫到:
```c
/* mm/corten_arena.c, corten_arena_mm_exit() 扩展（drain 之后、返回之前） */
static void corten_arena_exit_walk(struct mm_struct *mm,
				   struct corten_mm_state *state);
	/* fullmm tlb_gather_mmu; 逐区域逐窗: zap（unmap_chunk_flags 全清
	 * 语义, 含 swap entry 清理/计数, :5597 通用臂）→ tracked PT 页经
	 * M2a uninstall 漏斗释放 → fill_upper 造的上层表（pmd/pud）经
	 * pte_free 同族+tlb_remove 批量释放（今天由 free_pgtables 以
	 * VMA 界定完成, 零 VMA 后必须自拆）→ carrier 释放。
	 * 账目闭合: MM_ANONPAGES/MM_SWAPENTS/pgtable_bytes/
	 * hiwater_rss 与 zap 家族既有记账同源。 */
```
- fork 失败 unwind（MMF_UNSTABLE 子 mm 清场, M5_FORK_SPEC §1.3 ⑤）自动
  收敛到同一走查（今天靠 exit_mmap 拆 shadow-VMA 的路径消失）。
- 委托域照旧走 exit_mmap 树循环（exec/栈/brk/vdso 仍在树内, :1449-1457）。
- **锁序**: 无并发（mm_users==0）; 保持 ctl_lock 单边（:1548 既有）+
  fullmm gather 的 mmu_gather 语义（sleep-correct zap 形态, STATE §7.3
  zapfix 的锁外 tlb_finish 纪律在此强制）。
- **KUnit**: 合成 mm 全生命周期账（fault N 页+fork+swap N'+exit →
  ptdescs==meta_arrays、零 folio 泄漏、pgtable_bytes 归零）; 上层表
  释放计数; drain-timeout 泄漏降级臂不回归。
- **guest**: fork_roundtrip 1000（既有件）; memcg 压力 OOM-kill 大 MODE
  进程（reap+exit 干净, C19/C27 合流）; swapoff→exit 的 swap entry 对账
  （S-3 复测收口）。
- **diff 预估**: ~+420/−60。
- **风险**: 上层表自拆是全新代码路径（free_pgtables 从未以非 VMA 界定
  跑过窗口）——用 KUnit 账目断言 + guest `/proc/meminfo`
  PageTables 稳定性双向验证; RCU kfree_rcu 尾巴与 exit 顺序。

---

### 3.5 V-E: brk/heap 策略

- **裁决: V-E.1（采用）——brk 保留委托域 legacy 单 VMA**。论证:
  1. **ABI 约束**: brk 返回值必须与原 break 连续（sbrk(0)/MORECORE 契约）,
     堆不可搬窗; "brk 增长改 mmap 窗"= 对应用说谎（glibc 的 trim 语义
     M_TRIM_THRESHOLD 依赖 shrink 原址）。
  2. **账目与消费面**: RLIMIT_DATA（is_data_mapping, mmap.c:1477）、
     `[heap]` 注记消费面（gdb/pmap/PSI 分类）、VM_GROWSUP 扩展语义
     （acct_stack_growth 族）全部已实现且零回归风险。
  3. **收益面**: 堆 fault 的 find_vma 成本只对 glibc-brk 型分配器显著
     （静态链接 mmbench 口径, G1 口径更正条目）; 目标 workload
     （dedup/metis/psearchy/JVM+tcmalloc/dm_malloc）走 mmap 窗,
     brk 流量近零。**先测量后决策**（OQ-MV-7）。
- **交付**: J2 白名单把 brk VMA 显式登记; heap fault 路径计数
  （legacy find_vma 窗外命中, 观测不优化）; V-E.2（不实施, 登记）:
  若未来要"maple 字面空", 方案 = glibc 无关的内核侧 brk→region 翻译
  （brk 语义退化为 region 增缩, do_brk_flags 路由化 ~400 行）——仅当
  brk 压倒性热点时立项。
- **diff 预估**: ~+150（白名单断言/计数/文档）。
- **guest**: mmbench 动态口径（G1 口径更正的教训）对照 brk 计数; 委托
  白名单 live 断言（debugfs）。

---

## 4. 风险与红线

### 4.1 铁律（每片 review 逐条打勾）

1. **非 MODE 进程零感知**（=n 折叠 + =off 冒烟 + 三套件双口径; 每片必测;
   STATE r07-integrated 口径）。所有新钩子 = 双门形态（§1.5）。
2. **INV6 真源纪律**: 窗口 PTE 写必经事务; 胶水白名单 **3/3 已满**——
   V-B truncate 门、V-D exit 走查全部以"路由"或"mm_users==0 carve-out"
   形态存在, **不得新增第 4 个白名单写点**; 违 = 设计错误。
3. **INV7 扩展到区域记录**（INV-MV3）: 调试构建 checker 新增——
   (a) region 界 ≡ arena 界; (b) region.may_prot ⊇ 区域内每页 meta.perm;
   (c) rclass/idle/池三态互恰; (d) FILE 区 rfile 引用>0 且 pgoff 派生一致;
   (e) INV-MV2: 委托/植入白名单封闭（§1.3 J2）。
4. **VMA 位编码完备性**（§2.6 表）: 未列位=拒绝; 上游新位必须补表
   （review checklist 常设项）。
5. **语义变更集披露**（S-1..S-4, §3.0）: 全部落 REPORT; strace 等价性
   测试逐项带例外清单。
6. **外部观察者**: seccomp 不受影响（syscall 级）; gVisor/调试器保真 =
   C13/C15; J3 oracle 是回归门。

### 4.2 最大三个风险

| # | 风险 | 机制 | 缓解 |
|---|---|---|---|
| **R1** | **消费者清单遗漏** = 某条路径仍以窗口地址调 find_vma/裸假设 VMA 存在, 静默破坏（空树把"漏 guard"从'有 VMA 兜底'变成'空转放行'或'假 -ENOMEM'） | VMA 消费者 30+ 处（§3.0）; 6.18 演进期上游还会新增 | J1 审计计数器+bpftrace 双口径（负向探针: 计数>0 即 FAIL）; §2.6 完备性表; OQ-MV-2 的白名单封闭断言把"漏"变"响"; 每片 strace/行为等价测试 |
| **R2** | **carrier 语义漂移**: detached vma 上的 rmap/anon_vma/COW/GUP-pin/fork-copy 假设破裂（vma→mm 反链、anon_vma 链、pkey 位、寿命竞态）——M5/M6 已验收代码的全部依赖面 | fork PTE 复制搬家（A.2b）; carrier 参与 i_mmap（B）; ttu 守卫依赖 VM_CORTEN 位在 carrier 上 | 复用而非重写（copy_page_range/守卫逐字吃 carrier）; M5 判据 1/2 的 KUnit+guest 全量复跑; [FAIL-2] 寿命论证扩展到 carrier; C17 逐 TTU-flag 对拍走查 |
| **R3** | **外部观察保真度**: /proc/maps 双源渲染字节级回归 / GUP-slow 分支错误 → JVM CDS relocation、perf 符号化、gVisor、io_uring pin 断 | proc 格式是 de facto ABI; GUP 是 9p/O_DIRECT/io_uring 的数据通道 | J3 逐字节 oracle（shadow 基线 diff）; GUP 四态矩阵（M5.T3 件复用）; PROCMAP_QUERY/smaps/rollup 三口全测 |

### 4.3 完整风险表

| ID | 风险 | 概率/影响 | 缓解 |
|---|---|---|---|
| MV-4 | LSM（SELinux）对 anon mmap 的失明既成缺口（今天 auto 路由不调 security_mmap_file） | 低/中 | A.2a 显式补钩+计数披露; guest 无 LSM 时零行为差 |
| MV-5 | park 删 VMA 后 unuse/swapoff 盲区（S-3）在 swapoff 高频场景漏换回 | 低/中 | fault 慢车道兜底闭合性论证（entry 引用计数）; V-D 复测矩阵 |
| MV-6 | J1 审计计数器对 legacy find_vma 的开销可测 | 低/低 | static-branch 前置; 降级 kprobe-only 口径预案 |
| MV-7 | 上层表自拆（V-D）泄漏/重释 | 中/高 | KUnit 账目断言+PageTables 稳定性; 首版可保守: exit 保留"临时委托"路径对比（不采用, 仅作 review 对照） |
| MV-8 | 池/reactivate 与 region 状态机交互回归（eject/prepare 的 VMA 假设残码） | 中/中 | A.1 全臂 KUnit; perf2a churn 台账对账（munmap_releases==pool_parks） |
| MV-9 | V-B pagecache 竞态（锁外取页 vs truncate/回写） | 中/高 | do_read_fault 同构+re-query; truncate 门路由化; EOF SIGBUS 矩阵 |
| MV-10 | 植入例外被未来代码路径放大（更多 MAP_FIXED 入窗形状） | 低/中 | 白名单封闭+计数; J2 断言 |
| MV-11 | 多夜序列中"半 V-A"混合态（部分区域有 VMA 部分无）被消费者错误假设统一 | 高/中 | 混合态为一等公民: 全部消费面以帧表为准（今天已是）; 每片 DoD 含混合态用例 |

---

## 5. 切片表（V-A.x → 终态）

每片 ≤1 夜 + review; 依赖严格线性（B 依赖 A.2b; C 依赖 A.2b+B; D 依赖 C;
E 任意靠后）。diff 含测试。

| 片 | 内容 | diff 预估 | 测试锚 | 依赖 |
|---|---|---|---|---|
| **V-A.0** | region 记录落位+迭代器+debugfs+INV-MV3 基础（零行为变更） | ~+350 | region 编码/迭代/审计 walker KUnit | 无（HEAD 即可） |
| **V-A.1** | park 去 VMA 残留+池全臂手术+S-1/S-3/S-4 登记 | ~+450/−280(+250 测试) | park 无树断言/MAPERR 臂/判空复用/eject | A.0 |
| **V-A.2a** | auto-attach 零 VMA+`corten_auto_validate`（含 LSM 补钩）+记账口径 | ~+450/−120(+200 测试) | validate 真值表/declare 三态/THP 结构排除验证 | A.1 |
| **V-A.2b** | carrier 对象+fault ctx 迁移+**fork PTE 复制搬家**（fork_copy_ptes） | ~+600/−260(+300 测试) | carrier 寿命/fork_mirror 三方对拍/dispatch 四态 | A.2a |
| **V-A.3** | 路由扫尾（perm_pgprot/mremap/madvise RF）+J1 计数器+J2 审计钩 | ~+400/−150(+150 测试) | 纯函数矩阵/白名单注入/J1 开合 | A.2b —— **A 系出口: J1/J2 首次全绿** |
| **V-B** | FILE 区（classify/attach/fault 双臂/COW/EOF/truncate 门/i_mmap 参与/file put/fork 引用） | ~+900/−80(+350 测试) | FILE_MAPPED 矩阵/truncate 路由臂/引用对账 | A.2b（A.3 可并行 review） |
| **V-C** | proc 双源渲染+PROCMAP_QUERY+smaps PT 聚合+GUP-slow MODE 分支+J3 oracle | ~+750/−60(+250 内核测试+200 脚本) | 归并游标/render 对拍/gup_probe 矩阵 | A.2b+V-B |
| **V-D** | exit 纯 PT 走查+上层表自拆+carrier/region 终拆+swapoff 复测收口 | ~+420/−60(+200 测试) | 全生命周期账/上层表计数/RCU 尾巴 | V-C |
| **V-E** | brk 委托裁决+白名单 live 断言+brk 计数+文档/REPORT | ~+150 | 白名单断言/mmbench 动态口径对照 | V-D |

**终态判据（V-E 收口 = 系列 DoD）**: §1.3 J1–J4 全绿 + 铁律 1–6 全过 +
零改动回归集全量（metis_eq/dedup_eq/psearchy_eq/JThreadBench/lmbench
checksum 三方同值）+ REPORT.md §"M-V" 章落盘（含 LoC 终态账增量: 窗口域
零 VMA 使 MODE 进程对 maple_tree/mmap_lock 的热路径依赖归零——与 D12 的
"VMA 热路径可删量"账呼应）。
KUnit 终局锚: 合成 MODE mm 走完 **mmap→fault→mprotect→madvise→mremap→fork
→swap→park→exit** 全生命周期后断言: maple 树仅含白名单成员 ∧
J1 计数==0 ∧ 账目三元组（ptdescs/meta_arrays/folios）闭合。

---

## 6. OQ 清单（全部显式, 提请主 agent/规划者裁决）

- **OQ-MV-1**（§1.4, 需批准）: "maple tree 空"→"窗口域零 VMA+委托白名单"
  的口径修正; carrier=detached vma 的"零 VMA"解释。两处都是 D20 字面与
  工程现实的和解, 拒绝则本规格不成立（需回到全自造 rmap/COW/fork 的
  论文原教旨路线, 估算 ×3 工程量）。
- **OQ-MV-2**: 植入例外（窗口内 file MAP_FIXED legacy VMA）是否长期容忍,
  还是 V-B+ 把 CDS 型 MAP_FIXED file 也 region 化（需要 i_mmap 语义满配,
  收益低）。
- **OQ-MV-3**: MAP_STACK/线程栈入窗（旧 OQ-B 复活）: guard-page mprotect
  路由已就绪（T0b 起）, 但 glibc pthread 栈复用缓存对"munmap 后 maps
  消失"的观察敏感度未知——JVM spawn 矩阵实测后裁。
- **OQ-MV-4**: JVM paranoid 地址布局: 压缩 OOP/heap 保留走 hint 地址
  （非 NULL addr）本就落委托域, 窗口不影响; 但 sanitizer（ASan shadow
  高段 0x1000_7fff_8000 起）与窗口 16T 的共存未测——MODE+ASan 矩阵
  （low-risk 但必须跑一次）。
- **OQ-MV-5**: S-2 soft-dirty: 是否给窗口 PTE 补 soft-dirty 位（增量脏跟踪
  工具面）——今天已缺, 补=写点扩张, 倾向文档化不补。
- **OQ-MV-6**: S-3 swapoff 深化: unuse 的 MODE 分支（提前换回优化）是否
  V-D 后补回（swapoff 非热路径, 倾向不补, 计数披露）。
- **OQ-MV-7**: V-E.2（brk region 化）触发条件: heap fault 的 find_vma
  占比测量（mmbench 动态口径）; 无阈值前不立项。
- **OQ-MV-8**: MADV_PAGEOUT 对窗口路由到 shrinker victim 通道（依赖
  OQ-M6-7; process_madvise 用户可见便利）。
- **OQ-MV-9**: mseal 对 region 的语义（现=拒绝）; 若上游 mseal 生态成熟
  再评（region 需自持 seal 位+检查点, ~+100）。
- **OQ-MV-10**: io_uring 注册缓冲区覆盖 FILE 区（FOLL_LONGTERM × pagecache
  页）: COW 后为 anon 可 pin; 裸 pagecache pin 的 LONGTERM 拒绝口径对齐
  legacy writable_file_mapping_allowed（gup.c:1252）——B 片矩阵必测。
- **OQ-MV-11**: mincore 窗口路由 vs 登记 -ENOMEM 变更（C11; 倾向 V-C 路由,
  ~+120）。
- **OQ-MV-12**: 外部观察者合同: gVisor/runsc、crash、ebpf-based security
  工具对"maps 双源"的假设测试矩阵（C30）; 不阻塞, 登记。
- **OQ-MV-13**: 页粒度 maps 保真（region 升级 interval tree）触发条件:
  真实消费者投诉/上游 maps 语义变更; 接口已收敛（§2.4）。
- **OQ-MV-14**: NUMA: 委托域不参与窗口 balancing（fair.c 结构不可达）,
  多节点机器上 MODE 进程的页放置策略=首次 touch 本地——与 DEV-10/PS-G1
  披露合并陈述。

---

## 附: 本文引用的关键 file:line 索引（评审复核用）

- corten 侧: mm/corten_arena.c — shadowize 577-618/anon_vma_prepare 594/
  perm_pgprot 555-567/prot 互转 517-544/白名单 653-655/C1 729-771/
  declare_locked 779/release_arena_locked 976-1089（do_munmap 1058）/
  mm_exit 1515-1606/lookup 1613/auto_place 1710/seg_claim 1783/
  window_place_global 2110-2152（find_vma_intersection 2134）/
  auto_mmap_route 2206-2301（OVERCOMMIT 2226, pool_take 2272）/auto_attach
  2303-2338/mode_enter 2340-2365/mode_exit 2367-2439/prctl 2449-2476/
  unfreeze 2505/fork_abort 2533/fork_begin 2548/register_child 2743/
  fork_copy_window 2838/fork_mirror 2979/fork_commit 3078/lookup_get 3199/
  shadow_vma 3242/fault_covered 3270/fault_owned 3305/perm_ok 3403/
  fill_upper 3518/ctx 3636/get_vma 3663/map_anon 3687/zero_page 3826/
  restore 3880/cow_write 3979/swap_in 4189/dispatch 4524/user_fault
  4753-4876/slow_handle 4884/zap_window 5147/zap_untracked 5467/
  unmap_chunk_flags 5597/unmap_chunk 5696/pool_eject 5751/reactivate 5792/
  pool_prepare 5842/parkable 5915/park_locked 5947-6010（PROT_NONE strip
  5994-6001）/pool_release 6017/pool_take 6103-6199（VM_LOCKED 6116）/
  unmap_chunk_retry 6211/munmap_route 6230-6335（release 规则 6272-6284）/
  munmap_guard 6346/munmap_vma_guard 6390-6405/punch_classify 6415/
  punch_split 6460/mmap_punch 6546/mmap_punch_route 6604/mmap_route 6705/
  protect_window 6884/protect_range 7002/mprotect_route 7145/mremap_move
  7273/mremap_route 7357/range_overlaps 7525/dontneed 7567/madvise_route
  7631（hints 7661-7664, 默认拒 7697）/hwpoison_check 7706/oom_skip 7775/
  rmap_unmap_one 7803/rmap_swap_out 7864/swapin_sync 8044/shrink walk 8247/
  state_pages 8193/shrink_mm 8482/registry_pin 8545/shrinker 8659-8689/
  evict 8732-8806。
- 数据结构: include/linux/corten.h — 状态枚举 98-104/perm 106-110/COW 位
  115-124/map flags 133-139/unmap flags 147-154/512 常量 161/meta
  struct 179-188/状态机注记 408-410/swap 原语 453-493; include/linux/
  corten_arena.h — arena struct 159-190/窗口 203-204/段几何 206-212/
  POOL_MAX 349/stats 71-105; include/linux/mm_types.h — 982-993。
- 上游钩子面: mm/mmap.c — auto 路由 427-454（pool 早退 433-446）/
  __get_unmapped_area 460/NOREPLACE 465/MAP_FIXED 路由 490-498/attach
  659-660/find_vma 1010/munmap 路由 1209/brk 123/vm_brk_flags 1345/
  exit_mmap 1393-1466（mm_exit 1407/unmap_vmas 1427/free_pgtables 1439）/
  may_expand_vm 1472-1495/vm_stat_account 1497; mm/vma.c — __vma_link_file
  256-265/vms_gather_munmap_vmas 1414/do_vmi_align_munmap 1599/
  vma_link_file 1881-1894; mm/memory.c — 慢钩 6561-6582/copy_page_range
  wrprotect 1096-1099; arch/x86/mm/fault.c — 快钩 1344-1382（门 1362）/
  lock_vma_under_rcu 1386; mm/gup.c — check_vma_flags 1213/corten_own
  1234/file 写 1252/FORCE 1258/LONGTERM-dax 1242/can_follow_write_common
  598/slow find_vma 1973-1975/2041/fast 2857 起; mm/mprotect.c — 895/898-932;
  mm/madvise.c — 1940-1961; mm/mremap.c — 2005-2012; mm/mlock.c — 645/788;
  mm/mseal.c — 177; mm/migrate.c — 2647-2650; mm/rmap.c — ttu 守卫
  1895-1911/migrate 守卫 2351-2357; mm/oom_kill.c — 577-581; mm/swapfile.c
  — sync 2210-2224/unuse_vma 2436/unuse_mm 2457-2468; mm/memory-failure.c
  — 1595; mm/khugepaged.c — 910/2417; mm/ksm.c — 1192/2625; mm/pagewalk.c
  — 495/511; mm/mincore.c — 225/250; mm/userfaultfd.c — 42/87/127/1572/
  2006; fs/proc/task_mmu.c — m_start 313/show_map 538/PROCMAP_QUERY 682;
  ipc/shm.c — 1658-1662; kernel/events/core.c — perf_mmap 7144;
  kernel/sched/fair.c — change_prot_numa 3629; kernel/fork.c — mm_init
  1058-1062/dup_mmap for_each_vma 1275; kernel/sys.c — 2887-2890。
- 既有规格（语义基线）: M4T0_SPEC（T0 路由/白名单/审计表）、M5_FORK_SPEC
  （DEV-14/15、冻结窗、COW、GUP T3）、M6_RMAP_SPEC（V1/V2/V3、D1-D7、
  白名单 3/3）、STATE D11-D20。
