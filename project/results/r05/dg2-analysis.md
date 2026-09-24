# D-G'' 根因分析：MODE 接管下 JVM CDS relocation SIGSEGV ACCERR @ 0x100086000010

- 日期: 2026-09-15（r05 修复班取证）
- 内核: `/home/ppw/linux-6.18` HEAD=9d74b22a1348（T0a+T0b 已合入）
- 证据: `hs_err_pid524/567/611.log`（`/home/ppw/bench/share/t0dod/mode-smoke/`），
  `java.trace.on`（`/home/ppw/bench/share/t0dod-results-r05close/`，崩溃进程 pid 612 的 strace 真迹），
  `t0dod-run6.log`/`t0dod-run7.log`，`mm/corten_arena.c`、`mm/mmap.c`、`mm/vma.c`、`arch/x86/mm/fault.c`
- 结论等级: **根因已定案**（strace 真迹 + 寄存器 + 代码锚三方一致），非推测排序

---

## 0. 摘要（结论先行）

崩溃不是 D-G/D-G'（mprotect 提交路径 pending-perm/fill_upper）机制的残留——`rearm_recovered=16,
rearm_failed=0` 恰恰证明该机制工作正常。真凶是**第三种形状**：

JDK 21 的 CDS map_archive() 用 **mmap(classes.jsa, MAP_FIXED, PROT_READ|PROT_WRITE) 把文件映射
直接打进了 auto-arena 的 VA 区间**。这条 mmap 按 `corten_arena_mmap_classify(file=true)` 走 legacy
`mmap_region()`，其 overlap-removal（`__mmap_prepare` → `vms_gather_munmap_vmas`，vma.c:2469）
**不经过** `do_vmi_align_munmap` 里的 arena 守卫（vma.c:1618）——shadow-VMA 被无声打洞，文件 VMA
落位成功；但 arena 的 xarray frames 只在 RELEASE/demote 时擦除（corten_arena.c:1573-1576、727-731），
于是 fault 热钩子（x86 fault.c:1344 → `xa_load(frame)` 纯地址路由，corten_arena.c:1016-1028）仍然
把这个地址判给 arena。fault 进入事务层后元数据为 CORTEN_INVALID，FRESH 门取
`ar->prot`（attach 时的 PROT_NONE 预留 → `CORTEN_PERM_USER`，全程无 mprotect 抬过它），
读被拒 → `CORTEN_F_ACCERR` → `force_sig_fault(SIGSEGV, SEGV_ACCERR)`（fault.c:1348-1351）。

一句话：**地址键控的 arena 拦截了一个它不该拥有的（file-backed）VMA 的 fault；ACCERR 是 FRESH 门
按旧上界拒绝的"良性"表现——即使把门放开，arena 也只会给文件映射发 fresh 匿名零页（静默数据损坏）。**

---

## 1. 崩溃指令与访问类型（任务 1）

来源：`hs_err_pid524.log`（pid480/472/517/567/611 六份同形，si_addr 全部 0x100086000010）。

| 项 | 值 | 含义 |
|---|---|---|
| 线程 | `JavaThread "Unknown thread"` id=525，`_thread_in_vm` | 主线程 VM 初始化期（universe_init → global_initialize → map_archives） |
| pc | `libjvm.so+0x7b0389` = `FileMapInfo::relocate_pointers_in_core_regions+0x279` | CDS 指针重定位循环体 |
| si_code | 2 = SEGV_ACCERR @ 0x100086000010 | 页"存在但权限不符"的软件判定 |
| ERR（页错误码） | 0x4 = U=1, **W=0（读）**, **P=0（非 present）** | 用户态读、PTE 从未装上 |
| 崩溃指令 | `48 01 1c ca` = `add %rbx,(%rdx,%rcx,8)` | **读-改-写**，fault 报在读分量（ERR.W=0） |
| 关键寄存器 | RDX=0x100086000000, RCX=2, RBX=0x00000ff886000000 | RDX+RCX*8 = 0x100086000010 ✓ |

**RBX 即重定位 delta**：`0x100086000000 − 0x0000000800000000 = 0xff886000000`——把归档内指针从
SharedBaseAddress(0x800000000) 平移到实际落位基址 0x100086000000。崩溃点是循环处理的第一个非零
指针槽（偏移 0x10；槽 0/1 为 0 被前面的 test/循环展开跳过，反汇编序列 `48 85 f6`/`40 f6 c6 01` 可证）。

**ACCERR 与 ERR=0x4 的组合是指纹**：x86 原生路径上，"VMA 存在但权限不符"才会给 ACCERR；而
/proc/maps 显示 si_addr 落在 `rw-p` 文件 VMA 内（`100086000000-100086c9c000 rw-p classes.jsa`），
对 rw-p VMA 的读，`access_error()` 必然放行、filemap 正常供页。能同时给出
"rw-p VMA + 读 + ACCERR + P=0"的只有一条路：**fault 在进入 VMA 权限检查之前就被软件路径短路**。
即 x86 fault.c:1342 的 corten 热钩子（位于 `lock_vma_under_rcu()`/`access_error()` 之前）
返回 `CORTEN_FAULT_ACCERR`，由 `force_sig_fault(SIGSEGV, SEGV_ACCERR)` 直接投递（fault.c:1348-1351），
硬件 ERR 位原样保留（0x4）。这与"arena 事务层拒绝"完全吻合。

附带证据（同家族）：崩溃后 hs_err 错误报告器自身的 `read(5, 0x1000000a6000, 4096) = -1 EFAULT`
（trace 内可见）——arena 区间内的地址经 GUP→`corten_arena_handle_mm_fault` 慢路径吃闭门羹，
对应 run6 的 `FAIL: java new error returns under MODE`。

---

## 2. 事实链（strace 真迹，pid 612，逐条 syscall）

```
mmap(NULL, 1107296256, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE) = 0x100085600000
    → T0a auto-arena：1 个 arena [0x100085600000, 0x1000c7600000)，528 个 2M window，
      ar->prot = prot_from_vma(预留VMA) = CORTEN_PERM_USER（全 0）   (corten_arena.c:607,296-308)
munmap(0x100085600000, 10485760)  = 0   ← JDK 修整 reserve 头部 10MB
munmap(0x1000c7000000, 6291456)   = 0   ← 修整尾部 6MB
    → 两笔都命中 arena：start==ar_start / end==ar_end，class=CHUNK（尾>2M 不触发 release）
      → 事务化 chunk-zap：清内容、**保留 VA 与全部 xarray frames**（Fig.8 L9-13，
        unmap_chunk 无任何 xa_erase）——hs_err maps 里两侧仍是 ---p [anon:corten_arena] 即此
mmap(0x100086000000, 4722688, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED, fd=4, 0x1000) = 成功
mmap(0x100086481000, 8499200, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED, fd=4, 0x482000) = 成功
    → file-backed MAP_FIXED 打进 arena：classify(file)→CORTEN_MMAP_LEGACY (corten_arena.c:3068-3069)
      → legacy mmap_region → __mmap_prepare 的 vms_gather_munmap_vmas (vma.c:2469) —— 无守卫！
      → shadow-VMA 被拆成头/尾两片，中段拆除，文件 VMA 落位（两段合并成
        [0x100086000000,0x100086c9c000) rw-p，与 hs_err maps 逐字一致）
    → xarray frames 未动：xa_load(0x100086000000>>21) 仍返回该 arena (corten_arena.c:1028)
mmap(NULL, 253952, PROT_READ, MAP_PRIVATE, fd=4, 0xc9d000) = 0x7fc9da0d5000   ← 归档头（legacy 区，成功）
--- SIGSEGV {si_code=SEGV_ACCERR, si_addr=0x100086000010} ---                  ← 重定位第一读
```

后续本来还会有的 mprotect(RO)（remap read-only）**从未到达**——崩在 relocation 阶段。

fault 时的执行路径：
`do_user_addr_fault` → corten 热钩子（fault.c:1342，先于任何 VMA 查找）→
`corten_arena_lookup_get` 命中 → `corten_arena_fault_once`：
`lock_range` 首试 -ENOENT（该窗从未触碰、无 tracked PT page）→ `fill_upper` 补装 + 重试成功
（**这就是 rearm_recovered=16 的来源**：≈每次崩溃迭代武装一个窗，与 run4/5/6/7 的迭代数吻合）→
`corten_query` 得全零元数据 CORTEN_INVALID → FRESH 门 `gate.perm = READ_ONCE(ar->prot)`
（corten_arena.c:2258-2272）→ `perm_ok(PROT_NONE, read=false)` → 拒 → `CORTEN_F_ACCERR`。

关键：**这个 arena 从头到尾没有一次 mprotect**（trace 中 110 次 mprotect_routes 全部来自
metaspace/code-cache 等其它 arena）。既无 pending perm（`corten_arena_protect_window` 未运行过），
`ar->prot` 也从未被抬升（chunk 级 mprotect 本来也只记逐页 pending，只有 EXACT 才写 `ar->prot`，
corten_arena.c:3419-3430）——门是出厂时的 PROT_NONE 原样。

---

## 3. 候选根因排序（任务 2）

### (d) MAP_FIXED 归档映射落 arena —— **成立，即根因** ✅

拆成两个互相独立的缺陷（都需修，见 §5）：

- **D1（mmap 侧，守卫缺口）**：mmap.c:463-465 的注释假设"其余一切留在 legacy mmap_region 流程，
  其 overlap-removal 由 `corten_arena_munmap_vma_guard()` 把守"。该假设为假：守卫只挂在
  `do_vmi_align_munmap()`（vma.c:1618），而 MAP_FIXED 的 overlap-removal 走
  `__mmap_prepare()` 自己的 `vms_gather_munmap_vmas`（vma.c:2469），**绕过守卫**。
  后果：shadow-VMA 在未持 arena 事务锁的情况下被 zap/拆分（正是守卫注释里警告的
  "zap_pte_range() writes PTEs without the arena covering write lock" 形状），文件 VMA 无声落位。
  本次恰因窗口从未触碰（无 PTE、无 tracked PT page）而未触发锁竞争/损坏——但这是运气不是设计。
- **D2（fault 侧，地址键控）**：热钩子按 `xa_load(addr>>21)` 纯地址路由（corten_arena.c:1016-1028，
  fault.c:1342），不核对覆盖 VMA 是否仍是 `ar->vma`（shadow-VMA）。frames 只在
  RELEASE/demote 擦除（727-731/1573-1576），chunk-zap 与 legacy punch 均不擦 → 打洞后
  hole 内 fault 仍归 arena → anon-only 事务体 + FRESH 门按 `ar->prot` 拒读 → ACCERR。

成立需满足的观测（全部满足）：strace 中 file-MAP_FIXED 落位成功；hs_err maps 中文件 VMA 两侧为
`[anon:corten_arena]`；ERR=0x4+ACCERR 组合；`ar->prot` 无任何抬升来源；本 arena 零 mprotect。

### (a) mprotect 提交时 pending perm 部分覆盖 —— **不成立**（对本崩溃）

本 arena 全程无 mprotect：不存在"记了一半"的问题；si_addr 所在页是窗口第 0 页、
也是 mmap 区间第 0 页（4K 对齐边界内的首页），任何"off-by-something"都覆盖不到"完全没跑过"。
反向证据：若 pending perm 真覆盖了该页，fault 会**成功**并映射 fresh 匿名零页——表现为静默数据
损坏而非 ACCERR。签名对不上。（旁注：chunk-mprotect 不抬 `ar->prot` 的设计本身是对的——
t0b-verify.md 已论证"抬上界会把未提交区放开"——真正的问题在 (d)。）

### (b) RO 提交路径弄不一致 —— **不成立**

时序排除：JDK 21 是 map(RW) → relocate → remap(RO)；崩溃在 relocate，RO 步从未执行。
且 `corten_arena_protect_window` 的降级路径（3273-3287）自洽：zero-page 遇写降级时拆映射自愈。

### (c) 窗口/chunk 边界 off-by —— **不成立**（hex 几何直接否证）

- 0x100086000010 − 窗口基 0x100086000000 = **0x10**（窗口第 0 页、页内偏移 16B）
- 0x100086000010 − arena 基 0x100085600000 = 0xA00010（arena 偏移 10MB+16B，正是 JDK trim 后
  reserve 的起点+0x10）
- 距下一窗口界 0x100086200000 = 0x1FFFF0；第一段 mmap 长 0x481000=1153 页，fault 在第 0 页。
  完全不是"跨界第二页组"。

### (e) KVM 页表/host 页表不同步（mmu_notifier 缺口）—— **不成立**

si_code 是**guest 内核的软件决策**（force_sig_fault），EPT/host 同步问题只会改变硬件 ERR 位，
不可能凭空制造 ACCERR；且 T0b 的 protect_range 已包 mmu_notifier_invalidate_range_start/end
（corten_arena.c:3339-3341），本崩溃路径（fault 拒绝）根本不写 PTE，无失效可言。dmesg 零
WARN/BUG（run6/7 均 PASS 该项）。

**排序：d（D1+D2）≫ a > b > c > e（后四者被签名/时序/几何直接否证）。**

---

## 4. 判定实验（任务 3，今晚修复班可直接执行）

### E1 —— probe2 加一行：复现（**判定 d-D2**）

`/tmp/dg_probe2.c` 在现有 mprotect 成功后（line 63 之后）插入：

```c
int fd = open("/bin/true", O_RDONLY);          /* 任意真实文件 */
if (fd >= 0) {
    void *f = mmap(q, 4096, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_FIXED, fd, 0);   /* 文件映射盖在已提交窗口上 */
    fprintf(stderr, "[probe] file mmap=%p\n", f);
    fprintf(stderr, "[probe] file byte=0x%02x (expect 0x7f ELF)\n",
            *(volatile unsigned char *)(q + 0x10));
}
```

- 预期：MODE on → `SIGSEGV si_addr=q+0x10 code=2(ACCERR)`（与 JVM 同签名，且该页 pending perm
  已是 RW——证明不是 perm 覆盖问题）；MODE off(mode=3) → 打印 0x7f。
- 区分力：把 (d) 与 (a)/(c) 干净分离——fault 页 perm 已提交仍 ACCERR，只剩"文件 VMA 被拦截"可解释。
- 顺手 `strace -f -e mmap,munmap` 跑一遍，应复刻 §2 的四步序列。

### E2 —— E1 的对照组：匿名 MAP_FIXED（**排除 a，锁定"file"变量**）

E1 同位置把 `MAP_FIXED, fd` 换成 `MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1`。
预期：MODE on 下读写成功（走 CORTEN_MMAP_MARK 事务，r04 冒烟 mmap-fixed-rw 已过）。
E1 crash / E2 pass 的差分 = "file-backed"这一位。

### E3 —— 计数器取证：证明 fault 进了 arena（**区分"arena 拦截"vs"普通 PROT_NONE VMA"**）

E1 探针在崩溃前后各 dump `/sys/kernel/debug/corten/arenas` 与 arena_stats 的
`faults/accerr/fallbacks`（CORTEN_ARENA_STAT_*，corten_arena.h:72-82；segv handler 里 _exit 前 fprintf）。
预期：崩溃 run 的 `accerr` 恰 +1、`faults` +1；E2 run 的 `mmap_mark_txns`/`mapped` +1。
若是"VMA 本来就 PROT_NONE 被 x86 access_error 拒"，arena 计数器不会动——这是 D2 的直接观测。

### E4 —— 开门验证：证明"只放宽 FRESH 门"是错的修法（**排除 b 的近亲 + 暴露静默损坏面**）

E1 基础上，在 file mmap 之前对整 arena `mprotect(p, 128MB, PROT_READ|PROT_WRITE)`
（range==arena → EXACT → `ar->prot` 被抬成 RW，corten_arena.c:3419-3430）。
预期：**不崩**，但 `file byte=0x00` 而非 0x7f——arena 给文件映射发了 fresh 匿名零页
（CORTEN_DISP_MAP_ANON）。结论：ACCERR 是良性面，放宽门会变成 JVM 拿到全零归档，更糟。
判定实验 E4 同时验证 D1 的守卫缺口是唯一正确修法层级（mmap 侧路由，而非 fault 侧放行）。

### E5 —— 守卫缺口微测：NOREPLACE/FIXED 不对称（**判定 D1，F-B 的回归形状**）

活 arena 上对窗口地址（无需先 mprotect）：

```c
void *r1 = mmap(q, 4096, PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
/* 预期 -EEXIST（shadow-VMA 占位，find_vma_intersection 命中） */
void *r2 = mmap(q, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED, fd, 0);
/* 现状：成功（守卫缺口）；修复 F-C 后：应与 E1 语义一致（打洞成功且读得文件内容） */
```

预期现状：`r1=MAP_FAILED(EEXIST)`、`r2` 成功且随后读 ACCERR——记录下"拒绝与无声打洞并存"的
不对称，作为 F-B/F-C 的前后对照。

---

## 5. 修复设计草案（任务 4，≤100 行级别）

分层三件套；F-A 单独上线即可让 `java -version` 跑通，F-B 堵锁序险，F-C 是 T0 语义闭环。

### F-A（热修，~12 行）：fault 侧 shadow-VMA 覆盖自检

`corten_arena_fault_once` 在 FRESH 门之前（或 `corten_arena_user_fault` lookup 命中后）加：

```c
/* [D2 self-check] The arena owns [start,end) by frame table, but a legacy
 * MAP_FIXED punch may have carved a hole out of the cached shadow-VMA.
 * Zero maple walk: use the FAIL-2 cached pointer only.  A fault outside
 * the cached shadow-VMA is not ours -- hand it to the legacy funnel.
 */
vma = corten_arena_shadow_vma(ar);
if (unlikely(!vma || addr < vma->vm_start || addr >= vma->vm_end)) {
    corten_legacy_drift_inc();
    return CORTEN_F_FALLBACK;      /* → lock_vma_under_rcu → filemap 供页 */
}
```

- 效果：hole 内 fault 落回 legacy，文件数据正确，`java -version` 通过。
- 代价（文档化）：打洞后尾片 [hole_end, ar_end) 的 fault 会走 legacy 自愈（计数 drift）——
  interim 可接受，F-C 落地后消失（届时 lookup 即 NULL/各片帧仍归 arena 且缓存更新）。

### F-B（安全网，~10 行）：堵 mmap_region 守卫缺口

`__mmap_prepare()`（vma.c:2469 `vms_gather_munmap_vmas` 之前）或 mmap.c:469 legacy 落点处：

```c
if (corten_enabled_static() && corten_arena_munmap_vma_guard(mm, addr, addr + len))
        return -EOPNOTSUPP;    /* 与 do_vmi_align_munmap 同一 verdict */
```

- 效果：未路由处理的 MAP_FIXED∩shadow-VMA 组合一律干净拒绝（含部分跨界、SHARED、
  hugetlb 等所有 classify 不认识的形状），消灭"无锁 zap shadow-VMA"的悬置险。
- 与 F-C 的关系：能被 F-C 正确打洞的形状在 do_mmap 更早处返回，到不了这里。

### F-C（路由闭环，~60-70 行）：file-MAP_FIXED 打洞路由（T0 语义闭环）

1. `corten_arena_mmap_classify` 增列：`MAP_FIXED + file + 单 arena 内(CHUNK/EXACT)` →
   新类 `CORTEN_MMAP_PUNCH`；跨界/PARTIAL 仍 LEGACY（交给 F-B 拒绝）。
2. `corten_arena_mmap_route` PUNCH 分支（run 在 do_mmap、持 mmap_write）：
   a. 逐窗 `fill_upper`（与 MARK 分支同法，3138-3147）；
   b. 事务 zap [start,end) 内容与元数据（复用 `corten_arena_zap_window`）；
   c. **拆 shadow-VMA**：在 [start,end) 两端 split，中段按 legacy munmap 语义拆除
      （走 vms 机制，持写锁，等价 RELEASE 的 teardown 步骤但不动 frames 头尾）；
   d. **擦 frames**：`xa_erase(frame)` for [start>>PMD_SHIFT, end-1>>PMD_SHIFT]——
      hole 内 lookup 从此落空 → fault/mprotect/munmap 路由自然 legacy；
   e. 修正 `ar` 不变量：`ar->vma` 指向头片；若 frames 清空则走 RELEASE 尾半段（obs_remove +
      free）；返回 0 让 legacy 安装文件 VMA（此时 gather 已无 shadow-VMA 可撞）。
3. 不变量修复：头尾片各自 frames 保留 → 头尾 fault 继续享有 arena 服务；
   `corten_arena_range_overlaps`/路由 classify 以 frames 为准的口径写进 M4T0_SPEC 勘误。

### KUnit 锚（`mm/corten_arena_test.c` / `corten_fault_test.c` 形状）

| 用例 | 层 | 断言 |
|---|---|---|
| `mmap_punch_classify` | 纯 | file+FIXED+arena 内→PUNCH；PARTIAL 跨界→LEGACY；NOREPLACE→LEGACY |
| `fault_hole_fallback` | 纯(S4) | 带覆盖自检的 dispatch：addr 在缓存 shadow-VMA 之外 → FALLBACK（不得 ACCERR）——扩展 fault_test 契约注入 ar_coverage |
| `punch_frames_erased` | 表驱动 | 打洞后 `xa_load(hole frame)==NULL`，头尾 frame 仍命中；`range_overlaps(hole)==false` |
| guest smoke（E1/E2/E5 脚本化） | 端到端 | E1 读回文件字节 0x7f；E2 匿名照旧 PASS；E5 F-C 后 r2 成功且读得内容；`java -version` rc=0；metis_eq on-run rc=0 |

回归口径：`java -version`（默认 -Xshare:on）、metis_eq、mode-smoke 全绿 + 新增 4 KUnit +
dmesg 零 WARN；`arena_stats` 断言 accerr 不再增长、drift 仅在 F-A interim 阶段出现。

### 工作量预估

F-A ~12 行 + F-B ~10 行 + F-C ~60-70 行 ≈ **90 行**（含注释），KUnit/冒烟 ~150 行测试代码。

---

## 6. 三行结论

1. **最可能根因（已定案）**：JDK CDS 用 file-backed `mmap(MAP_FIXED)` 把 classes.jsa 打进
   auto-arena 区间——`mmap_region` 的 overlap-removal 绕过 `do_vmi_align_munmap` 的 arena 守卫
   （vma.c:2469 vs 1618）无声打洞，而地址键控的 fault 热钩子（fault.c:1344 → xa_load 帧命中）
   仍把 hole 判给 arena，FRESH 门按 attach 时 PROT_NONE 上界（无 mprotect、无 pending perm）
   拒读 → `force_sig_fault(SIGSEGV, SEGV_ACCERR)`，与 ERR=0x4+rw-p VMA 的指纹逐位吻合；
   RBX=0xff886000000=两基址 delta 是重定位上下文的封印证据。
2. **判定实验**：5 个（E1 probe2 加一行 file-MAP_FIXED 复现；E2 匿名对照；E3 arena 计数器取证；
   E4 整 arena 抬门证伪"放宽 FRESH 门"修法并暴露静默零页损坏；E5 NOREPLACE/FIXED 守卫缺口微测）。
3. **修复预估**：三层 ≈90 行——F-A fault 侧缓存 shadow-VMA 覆盖自检（热修，java 即通）、
   F-B `__mmap_prepare` 补守卫（堵无锁 zap 险）、F-C `CORTEN_MMAP_PUNCH` 打洞路由+帧擦除
   （语义闭环）；配 3 个 KUnit 锚 + E1/E2/E5 冒烟 + `java -version`/metis_eq 回归。
