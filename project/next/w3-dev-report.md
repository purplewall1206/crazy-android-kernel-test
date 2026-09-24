# W3 开发报告 · MV2 W-3: 委托域迁移四件（brk region 化落地 + 线程栈白名单翻转；主栈/exec/vdso 判定落章）

- 片: MV2 W-3（MV2_FULL_REMOVAL_SPEC.md 切片表 W-3；裁决依据 mve-dev-report.md §2.1 brk 三档阈值判据 + STATE D28 执行令）
- 基线: mv-a0 @ f5848730958e（W-2 完成，`git merge --ff-only` 已同主树 HEAD）· 未 commit · worktree /home/ppw/linux-6.18-mva
- diff: /home/ppw/cortenmm/patches/r07-w3.diff · 5 文件 **+835/−10**
  （生产：mm/corten_arena.c +548、mm/mmap.c +48、mm/corten_arena.h +43、include/linux/corten_arena.h +5；
  测试：mm/corten_arena_test.c +201）
- 验证: make -j8 RC=0 零新增警告（仅基线 objtool cpuidle + memblock modpost 两处既有噪声）·
  三套件 KUnit **on1 ×3（第 1 轮 core 套 txn_uninstall_interlock 计时 flake，复跑连续绿）** ·
  **on2 全绿（24/0/1 · 105/0/0）** · **off1 全绿（25/0/0）** ·
  **corten_arena 套 105/0/0（W-2 基线 104/0/0 + 本片新锚 1 例）** ·
  =n 十四对象 RC0 零警告 nm 零 corten 符号 · checkpatch --strict **0E/0W/0C（992 行）** ·
  终镜像 bzImage **#191**（sha256 前 16 位 14d6e5adaddf3a0f）已出 · guest 门留主会话
- 日志: /home/ppw/cortenmm/results/r07/w3/（kunit-on1[.run3]/on2/off1、build-y、build-n）

---

## 0. 四件判定（task 第一指令：先判定，代价 vs 违 D28 程度）

**先决事实（决定三件判定走向）**：`corten_mode` 是 per-mm 位，execve 装 `bprm->mm`
（mm_init 清零）后 **MODE 不跨 exec**——fork 才镜像（corten_arena_fork_begin 的
MODE-bit inheritance）。因此 **binfmt_elf / vdso 安装 / 主栈 VMA 的诞生全部发生在
MODE 入场之前**，它们对 D28 操作判据（"MODE mm 的 vma 分配恒 0"，W-2 §0.4 口径）
的贡献为零；入场后仍在产 VMA 的委托域只有三个：**sys_brk GROW/SHRINK、主栈
expand_downwards、线程栈 mmap**。这就是 W-3 的真实靶面。

### 0.1 件 1 brk region 化（V-E.2 复活）——**迁移，本片实施（干净段）**

| 判据 | 结论 |
|---|---|
| 违 D28 程度 | **高**——入场后每次 malloc 扩堆都走 do_brk_flags 的树操作；glibc 电池下 brk 流量是持续 vma 生产者 |
| 迁移代价 | **中**——ABI 面已被 V-E.1 裁决闭合（sbrk(0)/MORECORE 连续性 = mm->start_brk/brk 两个数，路由不动它们）；fault/route/渲染机器全部地址无关（fault tiers、S-5 双源、帧表 xarray），唯一新机器是"legacy 地址 + 页粒度边界"这一种 region 形状 |
| 降级段 | **入场前已写脏的堆**（resident PTE）不收——declare 的 [C1] 空段红线（resident PTE 无 metadata = MAPERR 自家数据）不可破；计数 `brk_legacy` 披露，W-4 的迁移事务机重开（见 0.3/§4） |

落地形状：sys_brk 双臂路由（§1.1/1.2）。**glibc 无关性成立**：min_brk/RLIMIT_DATA/
check_brk_limits/guard-gap next-VMA 四道闸全在 sys_brk 原位先跑，路由继承其裁决；
`mm->brk`/`mm->start_brk` 语义逐位不变；堆恒为一段连续 RW span。

### 0.2 件 2 栈——**半迁移：线程栈本片实施，主栈增长臂降级 W-3b**

- **线程栈（MAP_STACK）＝迁移，本片实施**。违 D28 程度高（每 pthread_create 一条
  VMA）；代价≈0：OQ-B 的前提（mprotect 路由能伺服 guard 页）T0b 起就是闭的，
  白名单翻转一行（§1.5），guard 页 mprotect = region 前缀 CHUNK 事务，join munmap =
  EXACT/pool 臂，fork 走既有镜像。glibc `MAP_PRIVATE|MAP_ANONYMOUS|MAP_STACK
  (+NORESERVE)` 形状即白名单正例。
- **主栈 GROWSDOWN 增长臂（expand_downwards 路由）＝不硬塞，W-3b**。理由：
  (a) 主栈 VMA 携 env/argv 驻留页——收编要跨 [C1] 空段红线，依赖 W-4 的迁移事务机
  （与件 3 同一台机器）；
  (b) 增长臂在**信号帧/深层调用**的全进程关键路径上，fault 侧要新开"below-start
  增长判据"（guard gap、RLIMIT_STACK、acct_stack_growth 的 locked_vm 面逐项重答），
  一处错 = 全系统信号路径不可用，违反"每件收口可运行"铁律的风险收益比不成立；
  (c) 主栈 VMA 在入场前已存在，post-entry 只是**就地扩**（vma_expand，非 vma 分配），
  对 D28 操作判据的即时压力远小于 brk/线程栈。设计稿见 §4.1。

### 0.3 件 3 exec 镜像——**迁移，但承接者是 W-4 入场扫入；本片不改 binfmt**

决定性事实（§0 先决）：binfmt 跑在 mode=false 的新 mm 上，load 段是"入场前 VMA"。
对 MODE 进程，exec 镜像的 region 化 **不是** do_mmap 路由问题，而是 W-4 扫入时
"存量 file VMA → FILE region"的转换问题——V-B 的 file 机器
（`corten_region_register_file`/`corten_arena_file_attach`/declare(file) 的
record+registry 臂）就是现成承接面。逐项核对 ELF 形状可表出性：

| binfmt 产物 | file region record 表出 | 备注 |
|---|---|---|
| load 段（text/data/interp） | rfile + rpoff(=p_offset+p_vaddr 页对齐) + prot/MAY | 与 dlopen 库同构，V-B 已测 |
| 插段对齐（两段同页交错） | 每段一 region、页粒度边界 | ELF_PAGEALIGN 语义在 binfmt 侧已完成，region 不再见插段 |
| padzero（text 尾页清零） | 入场前发生，页已驻留 | 转换时按"resident 页迁移"记档，与堆脏页同一条 W-4 事务 |
| set_brk 的 bss VMA | 件 1 的 adopt 臂直接消费（PTE 空段） | **本片已接**：入场后第一次 brk 增长即收编 |
| reloc 后 vm_mprotect（RW→RX） | T0b protect 事务（FILE region perm 重写） | V-A.3 既有 |

结论：件 3 本片**零 binfmt 改动**是正确动作；新机器只剩一台——**带驻留页的
VMA→region 迁移事务**（PT descriptor 补建 + metadata 补记 + folio 锚定），它同时
是 0.2 主栈驻留段与 W-4 全量扫入（~40 条）的公共前置。切片建议见 §4.2。

### 0.4 件 4 vdso/vvar——**结构性排除 + 计数披露（"系统 VMA 遗留、非应用映射"）**

arch 依赖深度实测（6.18 树）：

1. **安装**：`arch/x86/entry/vdso/vma.c` map_vdso 走 `_install_special_mapping` ×2/3
   （text `VM_READ|VM_EXEC|…|VM_SEALED_SYSMAP`；vvar `VM_IO|VM_DONTDUMP|VM_PFNMAP|
   VM_SEALED_SYSMAP`；vclock 页 `VM_PFNMAP`）——绕开 do_mmap 全链，region 化要为
   "special 页数组 fault"新开一类 record + fault 臂；
2. **内核侧 VMA 形状消费**（region 化即破坏）：
   `lib/vdso/datastore.c:115 vdso_join_timens()` **全树走查**
   `vma_is_special_mapping(vma, &vdso_vvar_mapping)` → `zap_vma_pages()`——time
   namespace 加入要重刷 vvar 页，region 化后此走查失明；
   `vdso_mapping.mremap = vdso_mremap`（vma.c:80/125）——用户态 mremap vdso 是
   受支持 ABI（CRIU 依赖），special `.mremap` 钩子无 region 对应物；
   VM_SEALED_SYSMAP 语义同理；
3. **arch 面**：x86_64 之外 arm64/compat 各有独立 installer——多架构爆炸半径；
4. **无关项澄清**：`get_gate_vma`（vsyscall 门 VMA）是 init_mm 静态对象、不在进程
   树内，region 化与否均不影响——判据里不构成 vdso 的依赖证据。

裁定：vdso/vvar 是**系统映射、非应用映射**，两个 VMA/进程，白名单分类器
`CORTEN_WL_SPECIAL`（arch_vma_name 谓词，V-E 已有）持续披露；wl 直方图给组成行。
**D28 会计问题移交**：终判据"maple 树条目数 == 0"在 vdso 保持 VMA 形状时不可达，
W-4/W-6 必须二选一：给"系统 VMA 对（[vdso]+[vvar]）"开判据豁免口径，或立项
special-region 类（timens 钩子 + special fault + mremap 语义 + 多架构）——本片
登记，不越权裁定。

---

## 1. 实施内容（对照判定）

### 1.1 sys_brk GROW 臂路由（`corten_brk_grow_route`，mm/mmap.c GROW 臂、
check_brk_limits 与 next-VMA guard 之后）

三段式，全部在 mmap_write 持有下运行（DEV-13，ctl_lock 内嵌）：

- **seed**（`oldbrk == start_brk`，binfmt 没产 bss VMA）：直接
  `declare_locked(novma)` 一段 `[start_brk, newbrk)`，perm R|W（对位
  do_brk_flags 的 `VM_DATA_DEFAULT_FLAGS` 形状）；
- **adopt**（有 brk VMA，[C1] 空段）：declare `[start_brk, oldbrk)`（novma，
  失败则 VMA 纹丝不动）→ `do_vmi_align_munmap` 摘除空 VMA（sys_brk SHRINK 的
  原生形状，uf 穿透保 uffd Unmap 事件；plain VMA 不碰 VM_CORTEN funnel guard）→
  落入 extend。declare 先行的次序保证任何失败点都回到 legacy 而不留半截堆；
- **extend**（region 已在，`ar->end == oldbrk` 校验）：纯 metadata——新帧
  xa_store（GFP_KERNEL_ACCOUNT，先查后写、失败回卷）+ `vm_stat_account` + 收边界
  `ar->end = newbrk`。零树操作。

记账对偶：adopt 的 declare take == 摘 VMA 的 refund（净零），extend/trim 逐页
对称；release（shrink 到 start_brk）走 `auto_shape` 退还全段。

### 1.2 sys_brk SHRINK 臂路由（`corten_brk_shrink_route`）

`newbrk <= start_brk` → 整段 release（novma 纯 metadata 拆除腿）；否则 = trim：
`corten_arena_unmap_chunk`（routed-munmap 同款事务，窗口级、metadata 驱动，边界
帧的存活页与共享邻接一概不碰）→ 整帧擦除（`[ceil(newbrk/PMD), oldbrk>>S - 1]`，
边界帧保留——tier-1 bounds 与事务 [F-B seal] 都读帧槽）→ PT 页退役
（free_ptes_span 边界守卫）→ 逐页退款。失败（-ENOMEM）与 legacy 同口径：brk 回滚，
半程 zap 接受（上游 do_vmi_align_munmap 失败语义的对偶）；legacy 臂随后找不到
brk VMA 自然走 REJECT，无 funnel 违约。

### 1.3 邻接精度件（`corten_route_hit`，本片的隐藏主件）

堆 region 活在 legacy 地址、**页粒度边界**——首帧几乎必与 data 段共享 PMD 帧。
七处范围路由全部是帧粒度 lookup + 分类拒绝，窗口隔离时代"帧命中 ⇒ 重叠"的隐含
前提被打破：不修，则**邻接合法空间操作全部 -EOPNOTSUPP 硬失败**
（mprotect/munmap data 段、ld.so 的 MAP_FIXED、mremap、madvise）。
修法：帧命中但字节不重叠（`end <= ar->start || start >= ar->end`）⇒ 丢弃命中跑
legacy——这正是分类器文档里 OUTSIDE 的本义（"none-of-ours"），窗口时代不可达而
已。落点七处：`munmap_route`、`munmap_guard`、`mprotect_route`、`mremap_route`、
`madvise_route`（DONTNEED 臂 + hints 臂）、`mmap_punch_route`、`mmap_route` MARK 臂；
引用配对逐点核对（drop 即 put，既有 out: 标签 NULL-safe）。PARTIAL（真跨界）保持
拒绝语义不变。

### 1.4 `corten_arena_free_ptes_span` 边界守卫

原实现按窗口整页退役 PT 页——对窗口 arena（恒帧对齐）等价，对页粒度 region 会
free 掉含存活 PTE 的 PT 页（trim 的保留堆页、data 段邻居）。加"整帧才退役"守卫
（首/尾不满帧 skip），窗口路径逐位不变。

### 1.5 MAP_STACK 白名单翻转（件 2a）

`corten_arena_auto_mmap_classify` anon 臂掩码 `+ MAP_STACK`（file mirror 保持
排除——file 请求无 MAP_STACK 语义）；pthread_create 形状即正例；guard 页走 T0b
CHUNK 事务。同步 mm/corten_arena.h 文档注释（OQ-B 结案）。

### 1.6 白名单分类器同步：委托桶收缩＝迁移的伴生事实，谓词零改

设计裁定：`corten_whitelist_classify` 的 BRK/STACK/FILE 谓词**不动**——(a) region
化交付的收缩方式是"这些域不再产树 VMA"（novma region 对 J2 树走查天然不可见），
桶收缩是事实不是配置；(b) 谓词必须保留：degraded 堆（brk_legacy>0 的进程）仍是
BRK 形状，删谓词会把它打成 VIOLATION 假警报。收缩的可验证形状由 KUnit 锚承接：
region 化后 `find_vma_intersection(heap)` 为空、`wl_brk_vmas` delta==0（guest 判据
升级版，§5）。

### 1.7 计数/debugfs

`brk_region_adopts`（declare+摘 VMA 或 seed）、`brk_region_grows`（纯 metadata
扩展）、`brk_region_shrinks`（trim/release）、`brk_legacy`（降级披露：每 1 是一条
没被收走的堆 VMA）四行入 arena_stats，紧邻 V-E 的四臂五行；测试访问器
`corten_arena_test_brk_region(which)`。V-E 四臂 note 原样保留（region 臂同样计
GROW/SHRINK——接线证明判据不受影响）。

## 2. KUnit 锚（corten_arena_test.c，+201）

- **`corten_arena_test_brk_region_route`**（op worker 上跑，锁形对齐 sys_brk）：
  adopt（declare+VMA 摘除，`find_vma_intersection` 空断言 + total_vm 净零）→
  extend（越帧，帧槽断言）→ trim（全帧出注册表、边界帧与堆尾存活）→ shrink 到
  start_brk（整段 release、total_vm 退还）→ 降级（VM_LOCKED mm：路由答 1、
  brk_legacy+1、零 declare）。region 形状断言：`ar->start==start_brk`、
  `ar->end==brk`、`!idle`、`auto_shape`。
- **`corten_arena_test_auto_classify`**：MAP_STACK 两正例（裸 + NORESERVE 组合），
  file 镜像坏位表保持含 MAP_STACK。

开发期修复两枚（对报告诚实记录）：(1) adopt 在注册表未建时解引用 NULL state
（state_create 提前）；(2) 测试 op 未持 mmap_write 违反路由契约（funnel assert）；
另 KUnit 断言自身一处 `find_vma` 误用（"下一个高于"语义）改为 intersection 判定
——内核路径本身无恙。

## 3. 验证

- make -j8 RC=0；零新增警告（`vmlinux.o: objtool cpuidle` 与
  `modpost memblock_end_of_DRAM EXPORT_SYMBOL init` 为基线既有噪声，不在本 diff）。
- 三套件 KUnit（filter_glob=corten*）：**on1 ×3**（第 1 轮 core 套
  `corten_test_txn_uninstall_interlock` 计时 flake ——20.3s 并发竞速例，与本片改动
  无交集且复跑即绿；arena 套四轮全绿）· **on2 绿** · **off1 绿**；
  **corten_arena 105/0/0**（基线 104 + 本片 1）· core 套 24/0/1 与基线同数。
- =n：十四对象 RC=0 零警告，nm 零 corten 符号（`corten_brk_*_route` 以 inline
  stub 折叠进 mmap.c，常量分支）。
- checkpatch --strict：**0E/0W/0C**（992 行）。
- 终镜像 bzImage **#191**（14d6e5adaddf3a0f）——每件收口可运行铁律：brk 路由全部
  fail-open 到 legacy，任何降级路径内核照常启动。

## 4. 切片建议（不硬塞件的去处）

### 4.1 W-3b：主栈 GROWSDOWN region 增长臂（独立切片，中）

设计稿：入场后首次向下扩张时 adopt 主栈 VMA（驻留 arg/env 页 ⇒ 依赖 4.2 的迁移
事务机）；fault 侧在 region dispatch 加 below-start 臂：`start - addr` 过 guard-gap
检查（`stack_guard_gap` + `vm_start_gap` 对偶）→ `acct_stack_growth` 对偶
（RLIMIT_STACK/locked_vm/`may_expand_vm`）→ `ar->start` 下收 + 帧认领 + 计账，
然后按普通 region fault 供零页。旁压面逐项迁移：`find_extend_vma`（GUP 侧）走
defer-GUP 既有门；sigaltstack 不涉 VMA 形状；mprotect/madvise 已路由。
验收：信号storms + 深递归 + gdb 附加三件套 + J2 直方图 STACK 桶清零。

### 4.2 W-3c：resident VMA→region 迁移事务机（件 3 的落地点，W-4 的公共前置）

规格要点：对 [start,end) 建全量 PT descriptor（fill_upper 形）→ 逐 present PTE
补 metadata（anon 驻留页记 FRESH 形 + novma rmap 锚定对齐 W-2 的
`folio_add_anon_rmap_novma`；file 驻留页记 FILE_MAPPED + rpoff 折算）→ total_vm
转记（VMA refund + declare take）→ 原子窗口（mmap_write 持有）发布。C1 红线由
"补记"而非"空段"满足。落地次序建议：先 file 形（exec 镜像，件 3 目标），后 anon
形（堆脏页/主栈）——file 页有 pagecache 天然锚，风险低一档。

### 4.3 vdso special-region 类：挂 W-4/W-6 的 D28 会计裁定（§0.4），不单独立片。

## 5. guest 判据（主会话执行；标准电池 + maps 形状断言升级版）

1. **标准电池**：J1–J4 + brk + S-3 + 回归集，阈值表沿 mve_battery.sh；
   brk 四臂 delta 全 >0（接线证明）维持。
2. **brk 形状断言（升级版）**：MODE 进程 malloc 压测后
   (a) `wl_brk_vmas` delta == **0**（W-3 前为持续正计数）；
   (b) `/proc/self/maps` 树侧无 heap VMA 行、堆域读写存活（数据完整性）；
   (c) debugfs `brk_region_grows` > 0、`brk_region_adopts` ≥ 1、`brk_legacy` == 0
   （glibc 电池负载下不允许降级；>0 需回填负载画像）。
3. **线程栈断言**：pthread_create/join 循环 N 轮 → J2 树走查 delta==0、窗口
   region 数随线程数起落（pool 复用生效）、guard 页写 → SIGSEGV/ACCERR。
4. **委托域组成披露**（wl 直方图）：入场后树内仅剩 STACK（主栈，W-3b 前）+ FILE
   （exec 镜像，W-4 前）+ SPECIAL（[vdso]/[vvar]，本片口径的系统 VMA 对）+
   IMPLANT（显式 MAP_FIXED，W-5 前）；BRK 桶恒 0。
5. **回归**：heap mprotect/munmap/madvise（data 段邻接操作，精度件的面）在
   legacy 与 MODE 两侧均 rc==0。

## 6. 红线盘点

- **INV6**：全片零新增 PTE 侧写路径；trim/zap 走既有事务（desc 写锁内），审计
  走查（wl/j2）只读维持；`wl_violations` KUnit 断言 0。
- **=n 折叠**：路由 inline stub（return 1）+ sys_brk 调用点 #ifdef 双保险；
  十四对象 nm 零符号实测。
- **每件收口可运行**：brk 路由所有失败分支 fail-open legacy（1）或拒绝（-errno，
  REJECT 臂回滚）；测试四轮全套件 + 终镜像 #191 出片。
- 不 commit；diff 在 /home/ppw/cortenmm/patches/r07-w3.diff。
