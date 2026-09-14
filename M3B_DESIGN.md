# M3b 设计文档 — CortenMM arena fault 路径绕过 VMA 的完整集成设计

- 目标树: /home/ppw/linux-6.18, HEAD=1284a235f751 + M3a 未提交工作树 (corten.h / corten.c / mm/corten.h / corten_test.c)
- 上游里程碑: MASTER_PROMPT §4 **M3** (fault 路径绕过 VMA, opt-in arena, shadow-VMA 互操作)
- 约束红线: **D1** opt-in arena (默认全 legacy); **D2(C)** 纯 C; **铁律 3** CONFIG_CORTEN_MM=n 时二进制路径零变化; `corten=off` 时 hook 退化为一条 static-branch nop
- 证据格式: `file:line` 均为 6.18 工作树实测 (2026-09-13)。凡未能树内核实者标 **OPEN QUESTION (OQ-x)**

> **版本注记 r2 (2026-09-13)**: 依据 design review PASS-with-conditions 结论修订。r1 事实抽查 40+ 处全部命中;
> r2 应用 4 个 P1 设计修正 (P1-1 投机分配出写锁 / P1-2 need_zero=true / P1-3 arena_ctl_lock 统一排空 /
> P1-4 mmap 路由门收窄) 与 P2 补行 (矩阵 4 行、R1 证据、R2 收敛、X86_64 依赖、percpu_ref_exit、
> check_stable_address_space、folio_page、计账声明、fork 对照口径、bench/arena-stress/ 路径、S4 测试降级)。
> 文中标注 [P1-x]/[P2-x] 即对应修正点。
- 论文引用: `/home/ppw/paper/corte/paper.txt` §4 (Fig.4 RCursor / Fig.5 locking protocol / Fig.6 unmap+stale / Fig.7 retry / Fig.8 fault handler)

---

## 0. 与 M3a 交付物的接口对齐 (本设计的基础, 全部来自工作树 git diff)

M3a 已经把"事务机器"造好 (工作树未提交, 1320 行):

| M3a 交付 | 形态 | M3b 如何消费 |
|---|---|---|
| `corten_lock_range(mm,start,len,txn)` | CortenMMrw 协议, 覆盖页写锁; hole→`ops->alloc` ensure-alloc 已接 (`corten_txn_fill_hole`, read→write 升级+升级窗重查) | arena fault = 对 `[addr, addr+PAGE_SIZE)` 开事务 |
| `corten_query/map/mark/unmap/unlock` | metadata 层; `corten_map` 仍只写 metadata (include/linux/corten.h:385-387 "2b scope") | M3b 在 arena 层补硬件 PTE 写入 + 记账 |
| uninstall 互锁 | staleness 在 desc 写锁下发布 (include/linux/corten.h:56-76 INVARIANT) → 事务体拥有覆盖 PT 页 | 锁序推导的基石 (§6) |
| BH 对称锁 | 一切 desc->lock 皆 `_bh` 变体 (include/linux/corten.h:503-514 红线 1) | arena 层所有新 desc->lock 触点必须同样 `_bh` (§6 红线) |
| real view `alloc` stub | `corten_real_alloc` 返回 -EOPNOTSUPP, 注释明言 "3b wires the real allocation here ... from inside the fault path" | 本设计 §4.4 给出接线方案 |
| 2b 遗留契约 | include/linux/corten.h:341-344: "caller must keep the walked page-table hierarchy alive (e.g. hold mmap_lock for read)" | §6.4 论证 arena 设计如何消除它 |

---

## 1. 总览: 用户旅程与路径对比

### 1.1 用户旅程

```
/* arena 压测器 (§7.1) 的骨架 */
#define ARENA_SIZE  (1UL<<30)              /* 1G, 2M 对齐 */
base = mmap(NULL, ARENA_SIZE, PROT_READ|PROT_WRITE,
            MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);   /* ① 大区 */
prctl(PR_CORTEN_ARENA, CORTEN_ARENA_DECLARE, base, ARENA_SIZE, 0); /* ② 声明 */
/* ③ 多线程 churn: */
chunk = base + off;
mmap(chunk, SZ, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                              /* ③a arena 内 mmap → corten_mark 事务 (§5.6) */
chunk[i] = x;                 /* ③b 触页 → arena fault 事务 (§4) */
munmap(chunk, SZ);            /* ③c arena 内 munmap → corten_unmap 事务 (§5.5) */
prctl(PR_CORTEN_ARENA, CORTEN_ARENA_RELEASE, base, ARENA_SIZE, 0); /* ④ 释放 */
```

### 1.2 内核路径对比图

```
            legacy PF (6.18, CONFIG_PER_VMA_LOCK)                arena PF (M3b, opt-in)
            ─────────────────────────────────────                ─────────────────────────
user fault                                                user fault
  │ do_user_addr_fault  fault.c:1207                        │ do_user_addr_fault
  │                                                         │  [hook, fault.c:1322 之前]
  │  lock_vma_under_rcu   mmap_lock.c:224                   │  mm->corten_state==NULL? ──是──► legacy
  │   rcu_read_lock + mas_walk (maple tree)  :232            │  xa_load(2M frame idx)      O(1)
  │   vma_start_read (per-VMA seqlock)       :238            │  arena_get(percpu_ref)
  │  access_error(vma)      fault.c:1329                     │  [PMD 缺?] fill_upper (§4.4, 稀有)
  │  handle_mm_fault        fault.c:1334                     │  [投机 alloc folio+charge, P1-1, 锁前]
  │   __handle_mm_fault     memory.c:6300                    │  corten_lock_range(PAGE_SIZE)   ← desc 写锁
  │    pgd/p4d/pud/pmd_alloc  :6317-6352                     │   corten_query → perm 校验
  │    PMD THP 门 thp_vma_allowable_order :6360-6361         │   pte_offset_map_lock → set_ptes (§4.6, 锁内 0 分配 0 睡眠)
  │    handle_pte_fault      :6391                           │   corten_map (metadata)
  │   do_pte_missing        memory.c:4364                    │  corten_unlock
  │    do_anonymous_page    memory.c:5166                    │  (全程: 0 次 find_vma / 0 次 mmap_lock /
  │     pte_alloc           :5183                            │   0 次 vma_start_read / 0 次 maple-tree walk)
  │     alloc_anon_folio    :5215                            │
  │     pte_offset_map_lock :5237                            │
  │     set_ptes+rmap+lru   :5259-5267                       │  并行度 = 每 2M 窗口一个 desc 写锁
  │  vma_end_read / mmap_read_unlock  fault.c:1336/1413      │  (legacy: 同一 VMA 的全部 PF 串行于
  │                                                          │   vma seqlock 读侧 + page_table_lock 竞争)
```

unmap 对比:

```
            legacy munmap                                         arena munmap (M3b, 区内 chunk)
            ─────────────                                         ─────────────────────────────
sys_munmap  mmap.c:1088                                           sys_munmap → [hook]
  do_vma_munmap  vma.c:1646                                          corten_arena_munmap(): 按 2M 窗口迭代:
   mmap_write_lock                                                    corten_lock_range(≤2M)
   vma isolation/split/merge (vma.c:1577-)                             ptl 内 pte_clear + rmap 移除 + 计账
   unmap_vmas → zap_pte_range → TLB batch                              corten_unmap(metadata)
   free_pgtables → pte_free funnels                                    corten_unlock
                                                                       TLB batch flush → folio_put (§5.5)
                                                                     (shadow-VMA 与 maple 树全程不动)
```

---

## 2. arena 注册与查找 (核心决策)

### 2.1 prctl 契约

**编号**: `PR_CORTEN_ARENA = 79`。6.18 uapi 最高数字 prctl 是 `PR_FUTEX_HASH = 78` (include/uapi/linux/prctl.h:385; kernel/sys.c:2882 是其 case)。数字命名空间 (非 magic-number 命名空间如 PR_SET_VMA 0x53564d41) 是内核惯例走向 (67-78 全部为数字)。

**签名** (include/uapi/linux/prctl.h 新增):

```c
#define PR_CORTEN_ARENA        79
#define CORTEN_ARENA_DECLARE   0    /* arg3=addr, arg4=len: 声明 [addr,addr+len) 为 arena */
#define CORTEN_ARENA_RELEASE   1    /* arg3=addr, arg4=len: 释放 (必须精确等于 DECLARE 区间) */
#define CORTEN_ARENA_QUERY     2    /* arg3=addr: 返回 1(在某 arena 内)/0(不在), 负值=错误 */
/* arg5 恒须为 0 (预留), 违者 -EINVAL */
```

**DECLARE 契约** (kernel/sys.c 新 case → `mm/corten_arena.c: corten_arena_declare()`):

1. `capable(CAP_SYS_ADMIN)` — M3 实验特性, 收紧权限 (OQ-1: 是否放松为无 cap)。
2. 门控: `corten_enabled_static()` 为假 → `-EOPNOTSUPP` (boot 未开 corten=on 时行为与 =n 一致, 可探测但无副作用)。
3. `addr` **PMD 对齐 (2M)**, `len > 0` 且 **PMD 整除** — arena 粒度 = covering PT 页粒度, 使一次事务永不跨两个 arena, 也让 §2.2 的 2M-frame xarray 索引无歧义。非对齐返回 `-EINVAL`。
4. 范围必须**恰好等于当前进程一个现存 VMA**: `vma_lookup(mm, addr)` 且 `vma->vm_start==addr && vma->vm_end==addr+len` (单一 VMA, 不允许跨多个/部分覆盖 — 多 VMA arena 的 split 语义推迟到 M4, OQ-2)。该 VMA 必须满足: 私有匿名 (`!vma->vm_file && !(vm_flags & VM_SHARED)`), `!(vm_flags & VM_ACCOUNT)` (调用方 mmap 时须 MAP_NORESERVE), 且无 `VM_SPECIAL/VM_PFNMAP/VM_MIXEDMAP/VM_IO/VM_HUGETLB/VM_UFFD_*/VM_PKEY_BIT*/VM_SHADOW_STACK/VM_SEQ_READ`… 即只允许 `VM_READ|VM_WRITE|VM_EXEC|VM_MAY*|VM_NORESERVE` 白名单位。
5. 与现存 arena 重叠 → `-EEXIST`。
6. 就地"改造"该 VMA 为 shadow-VMA: 持 `mmap_write_lock` + `vma_start_write`, `vm_flags` 增 `VM_CORTEN | VM_NOHUGEPAGE` (§3.1), 不 split 不 merge (§3.3)。
7. 创建 arena 描述符并发布到 per-mm xarray (§2.2/2.3); `vm_stat_account` 已在原 VMA 上 (total_vm 已计入, arena 内 mmap/unmap 不再动 VMA 记账, §5.6)。
8. `anon_vma_prepare(vma)`: shadow-VMA 将在 §4.6 被 `folio_add_new_anon_rmap()` 引用, anon_vma 必须先行就绪。

**RELEASE 契约**: `addr/len` 必须精确命中一个已声明 arena (按 start 查)。流程 (§6.3): 取 `state->ctl_lock` [P1-3] → `xa_erase` → `percpu_ref_kill_and_confirm(&active, confirm_cb)` (confirm 回调 `complete(&drained)`, percpu-refcount.h:129) → `wait_for_completion(&drained)` (drain 在 ctl_lock 下等待, 无环论证见 §6.3) → 取 `mmap_write_lock` 走 legacy `do_vma_munmap()` 摘除 shadow-VMA (清 `VM_CORTEN` 位后普通 munmap; PT 页经既有 pte_free funnels → M2a uninstall → 描述符消亡) → **`percpu_ref_exit()` 先于 `kfree_rcu(ar)`** [P2-8, init 时指定 release 回调] → 释放 ctl_lock。

**QUERY 契约**: 纯读, 无锁 (rcu), 返回 0/1/-ENOENT。

**挂点**: kernel/sys.c `_S(PR_CORTEN_ARENA, ...)` case (参照 kernel/sys.c:2882 `case PR_FUTEX_HASH` 的写法), 转调 `#include <linux/corten_arena.h>` 的 `corten_prctl_arena(op, arg3, arg4, arg5)`; `CONFIG_CORTEN_MM=n` 时该函数为空桩返回 `-EOPNOTSUPP` (铁律 3: case 标签本身编译进去, 但路径为 `return -EOPNOTSUPP;` 一条 — 与未知 prctl 同路径, 二进制行为零变化)。

### 2.2 per-mm 元数据: 2M-frame xarray (选定方案)

**决策: xarray, 索引 = VA 的 2M frame 序号 (`addr >> PMD_SHIFT`), 值 = `struct corten_arena *`。** 否决区间树 (interval tree): 查找是 O(log n) + 需要自旋锁保护 (增广 rb-tree), 而 xa_load 是无锁 RCU O(1), 且 arena 数量小 (每进程个位数)、每 arena 占 `len/2M` 个连续槽 — DECLARE 时批量 `xa_store`, 内存开销 = 指针数组 (1G arena = 512 槽 = 4KB, 与 metadata array 同量级, 可接受; 若 512G arena 则 1MB 指针表 — 记入 §8 风险 R6, M4 可换 sparse-cache)。

```c
/* include/linux/corten_arena.h (新文件) */
struct corten_arena {
    unsigned long           start;      /* PMD 对齐 */
    unsigned long           end;        /* PMD 对齐, start+len */
    u8                      prot;       /* 声明时的 CORTEN_PERM_* 上界 */
    struct mm_struct        *mm;        /* 反链 (诊断用) */
    struct percpu_ref       active;     /* 事务活性计数 (§6.3): fault 路径 tryget */
    struct completion       drained;    /* percpu_ref_kill_and_confirm 的 confirm
                                         * 回调 complete() — 6.18 无
                                         * percpu_ref_wait_for_zero, 用 confirm+completion
                                         * (percpu-refcount.h:129) */
    struct mutex            fill_lock;  /* 上层页表 ensure-alloc 互斥 (§4.4) */
    struct vma_iterator     pad;        /* 保留对齐, M4 用 */
};

struct corten_mm_state {               /* 惰性分配: 第一次 DECLARE 才建 */
    struct xarray           arenas;     /* 2M frame idx -> struct corten_arena * */
    refcount_t              nr;         /* 活跃 arena 数; 0 ⇒ 查找一跳短路 */
    struct mutex            ctl_lock;   /* [P1-3] DECLARE/RELEASE (含 drain) 串行化;
                                         * fault 路径只做 percpu_ref tryget/put, 永不取它 */
    /* 统计计数 (debugfs 用, relaxed) */
    unsigned long __percpu  *stats;     /* faults/sigsegv/fills/fallbacks/unmaps */
};
```

`struct mm_struct` 增一个字段 (include/linux/mm_types.h, 挂在 mm_struct:967 `struct maple_tree mm_mt;` 附近, **整段 `#ifdef CONFIG_CORTEN_MM` 包裹**):

```c
#ifdef CONFIG_CORTEN_MM
    struct corten_mm_state  *corten_state;   /* NULL = 无 arena, 一跳回 legacy */
#endif
```

`CONFIG_CORTEN_MM=n` ⇒ mm_struct 布局不变, 铁律 3 满足。

### 2.3 fault 路径的零-VMA 查找 (快/慢门)

```c
/* mm/corten_arena.c */
static inline struct corten_arena *corten_arena_lookup(struct mm_struct *mm,
                                                       unsigned long addr)
{
    struct corten_mm_state *st;
    /* 一跳否定: 绝大多数进程 (与 arena 进程的非 arena 访问) 走这里 */
    st = READ_ONCE(mm->corten_state);            /* 1 load */
    if (!st || !refcount_read(&st->nr))          /* 1 load */
        return NULL;
    return xa_load(&st->arenas, addr >> PMD_SHIFT);   /* RCU 无锁 O(1) */
}
```

- 全程不碰 `mm->mm_mt` (maple 树)、不碰 `find_vma`、不碰 `mmap_lock`、不碰 per-VMA seqlock — 满足"fault 路径零 find_vma/mmap_lock"。
- 快否定双门: 静态门 `corten_enabled_static()` (boot 参数, include/linux/corten.h:250-258) 放在 arch hook 入口; 动态门 `corten_state==NULL` 放在 lookup 第一行。两个门都是纯读。
- **RCU/引用生命周期**: `xa_load` 在 `rcu_read_lock()` 内完成; 命中后 `percpu_ref_tryget_live(&arena->active)` (失败= 正被 RELEASE, 返回"回 legacy"), `rcu_read_unlock()` 后再使用; 事务结束 (含 TLB flush) `percpu_ref_put`。`struct corten_arena` 经 `kfree_rcu` 释放。mm 侧: `exit_mmap()` (mm/mmap.c:1272) 早段插入 `corten_arena_mm_exit(mm)` (§5.2), mm 消亡先于 state 消亡, 故 descriptor 不会活过 mm。
- **fork**: 见 §5.1 (M3: fail-fast; 元数据复制策略为 M5 定案 — 本设计仍给出推荐方案)。

---

## 3. shadow-VMA

### 3.1 flags 选择: 新通用位 `VM_CORTEN` (CONFIG_64BIT bit 43) + `VM_NOHUGEPAGE`

6.18 `vm_flags_t = unsigned long` (include/linux/mm_types.h:666), x86_64 上 64 位宽。对 32 位字的审计 (include/linux/mm.h:279-329): 0x00000001…0x40000000 与 BIT(31) 全部占用, **无空位**; 高位区: `VM_HIGH_ARCH_BIT_0..6` = 32..38 (mm.h:332-338), x86 用 0-3 作 pkey (mm.h:348-357; arch/x86/Kconfig:1826 `ARCH_PKEY_BITS default 4`)、5 作 `VM_SHADOW_STACK` (mm.h:365-375) ⇒ **x86 上 bit 36 (VM_HIGH_ARCH_4) 恰好空闲** — 但它被 arm64 占为 `VM_MTE` (mm.h:404-405), M9 会撞车。

**选定方案: 仿 mseal 先例开 64 位通用位** (mm.h:446-450: `#ifdef CONFIG_64BIT #define VM_SEALED_BIT 42`):

```c
/* include/linux/mm.h, 紧随 VM_SEALED 块之后 */
#ifdef CONFIG_CORTEN_MM
#ifdef CONFIG_64BIT
# define VM_CORTEN_BIT  43
# define VM_CORTEN      BIT(VM_CORTEN_BIT)  /* CortenMM arena shadow-VMA */
#else
# define VM_CORTEN      VM_NONE              /* 32 位不支持 arena DECLARE */
#endif
#else
# define VM_CORTEN      VM_NONE
#endif
```

利: 无架构冲突 (M9 免重设计)、零语义纠缠 (其余子系统不识别该位, `vma_merge` 依 `vm_flags` 异同自然不与邻 VMA 合并, vma.c:97)。弊: 43 位是 "CONFIG_64BIT 自由区" 第一个被 corten 之外再占用的位 — 需在 commit message 里写明先例 (mseal 占 42) 与剩余空位 (44+)。

**同时设 `VM_NOHUGEPAGE`** (mm.h:328): 这是 THP/khugepaged 排除的**单开关** (§4.7)。

否决方案 (记录理由):
- *复用 `VM_MIXEDMAP`+私有标记*: `VM_MIXEDMAP ∈ VM_SPECIAL` (mm.h:493), 会连带改变 `vm_normal_page`/core-dump/mlock 等行为面, 且挡 mprotect 语义; 弃。
- *给 shadow-VMA 设私有 `vm_ops`*: `vma_is_anonymous()` 的定义是 `!vma->vm_ops`, 设 vm_ops 会让全部 anon 判定翻转, 波及面不可控; 弃。

shadow-VMA 最终 flags = 原 VMA 白名单位 `| VM_CORTEN | VM_NOHUGEPAGE`。

### 3.2 一个 shadow-VMA per arena (选定) vs 全进程单个大 shadow-VMA

**选 per-arena 1:1**: (a) DECLARE 要求恰好命中一个现存 VMA (§2.1-4), 天然 1:1; (b) RELEASE 只摘自己的 VMA, 不连坐; (c) 与 mremap/brk 的冲突面被"区间精确匹配"限制到最小。全进程单个大 VMA 需要把多个不连续 mmap 区拼接成一个 VMA — 违反 VMA 语义, 弃。

两个相邻 arena (同 flags) 会被 `vma_merge` 合并 (vma.c:87-107 `is_mergeable_vma`: vm_flags/file/anon_name 全同即合并) — **无害且有益** (VMA 数不膨胀): 查找走 xarray 不走 VMA, 合并只影响 legacy 观察者。记录为已知行为 (测试 §7.1 校验 /proc/maps 仍显示为单一区间)。

### 3.3 与 vma_merge / mprotect(split) / madvise 的冲突面

- shadow-VMA 一经声明, **M3 禁止一切会 split/merge 它的路径**: mprotect/mremap/madvise(除 DONTNEED 路由)/mmap MAP_FIXED 交叉/uffd 注册 全部 -EOPNOTSUPP 或路由 (§5 矩阵)。这是 M3 的一致性边界, M4 事务化空间操作后再逐步放开。
- `vma_start_write`/per-VMA 锁: shadow-VMA 的 flags 写只发生在 DECLARE/RELEASE (持 mmap_write_lock), 与 arena fault (不取 VMA 锁) 无交叉 — VMA 锁不进 arena 事务锁序图 (§6.1)。

### 3.4 /proc/maps 显示策略: **如实显示** (选定)

- shadow-VMA 本来就是合法的私有匿名 VMA: /proc/pid/maps (fs/proc/task_mmu.c:501 `show_map_vma`) 会如实显示为一个 `rw-p` 匿名区间, 尺寸=arena 大小。**不伪装** (不藏行、不伪造路径): 藏行需要动 seq_file 迭代器, 伪装需要假 vm_file, 两者都引入新的 VMA 层特判 — 与"最小侵入"相悖。
- 加名字便于观测: 若 `CONFIG_ANON_VMA_NAME` 开启, DECLARE 时 `anon_vma_name_set(vma, "corten_arena")` → maps 显示 `[anon:corten_arena]` (渲染点 fs/proc/task_mmu.c:424,439-441,473-475; `is_mergeable_vma` 对 anon_name 相等才合并, vma.c:105 — 相邻同名 arena 仍合并, 同 §3.2 无害)。未开 config 时退化为普通匿名显示, 不影响 DoD ("/proc/pid/maps 正常")。
- smaps: `smaps_pte_range` (fs/proc/task_mmu.c:1157) 持 mmap_lock 读 + ptl 走 PTE — 与 arena fault 的 ptl 使用天然互斥, 只读安全, RSS/Rss_anon 数字如实反映 (MM_ANONPAGES 计账见 §4.6)。`THPeligible` 因 `VM_NOHUGEPAGE` 恒 0 (huge_memory.c:132-143 → huge_mm.h:335), 如实。numa_maps 走同族 smaps 回调, 无需处理。

---

## 4. fault 钩子

### 4.1 插入点一 (热路径): arch/x86/mm/fault.c `do_user_addr_fault`

**位置: `#endif` (vsyscall 块, fault.c:1320) 之后、`if (!(flags & FAULT_FLAG_USER)) goto lock_mmap;` (fault.c:1322) 之前。**

理由: 此处 (a) `local_irq_enable()` 已过 (fault.c:1279), (b) flags (WRITE/INSTRUCTION/USER) 已齐 (fault.c:1289-1302), (c) vsyscall 仿真已短路 (fault.c:1316-1319 — vsyscall 地址高于 arena 可能区, 但保持"先特判后 hook"的次序与该注释 "do this emulation before we go searching for VMAs" 一致), (d) 尚未发生任何 VMA/锁动作。

```c
#ifdef CONFIG_CORTEN_MM
	if (static_branch_unlikely(&corten_enabled_key) &&   /* corten=on (nop when off) */
	    user_mode(regs)) {                               /* M3: 仅用户态 PF 走绕行 */
		if (corten_arena_user_fault(mm, address, error_code, regs, &flags))
			return;      /* 已处理 (含 SIGSEGV 投递); false = 回 legacy */
	}
#endif
	/* CONFIG_CORTEN_MM=n 时以上整段不存在 — 二进制零变化 */
```

- 双门 = static_branch (boot) + `mm->corten_state` (per-mm, §2.3 第一行)。`corten=off` 或无 arena: 一次跳转 + 一次 load。
- **kernel 态对 user 内存的访问** (get_user/WRUSS 等) **不在此绕行**: 走 fault.c:1322 `goto lock_mmap` → `lock_mm_and_find_vma` (mmap_lock.c:425) 找到 shadow-VMA → `handle_mm_fault` → 插入点二 (§4.2)。这不是热路径, 且保住了 SMAP/AC 等特判语义。
- SIGSEGV 语义复用 x86 出口: 权限错 → `bad_area_access_error(regs, error_code, address, NULL, NULL)` (SEGV_ACCERR, 与 fault.c:1330/1368 同一投递函数); 地址未声明 (metadata INVALID) → `bad_area_nosemaphore()` (SEGV_MAPERR, fault.c:1359 同款)。

### 4.2 插入点二 (慢路径统一入口): mm/memory.c `handle_mm_fault`

**位置: `handle_mm_fault()` 函数体最前 (memory.c:6525, 在 `sanitize_fault_flags` (6535) 之前)。**

```c
#ifdef CONFIG_CORTEN_MM
	if (corten_enabled_static() && (vma->vm_flags & VM_CORTEN)) {
		vm_fault_t cret;

		/* [P2-6] 防御: 带 VMA 锁进来的调用 (插入点一已拦用户态 PF, 此处
		 * 防未来调用方) 或原子上下文 → 放弃事务, 用 VM_FAULT_RETRY (唯一
		 * 允许的例外) 让 x86 层落到 lock_mmap 重入 — 此时 mmap_lock 已持
		 * (fault.c:1354-1357), 事务前提成立。 */
		if ((flags & FAULT_FLAG_VMA_LOCK) || in_atomic())
			return VM_FAULT_RETRY;

		cret = corten_arena_handle_mm_fault(vma, address, flags, regs);
		/* 约定: arena 慢路径永不返回 VM_FAULT_RETRY/COMPLETED
		 * (它不释放也不降级 mmap_lock; 调用方锁状态原样保留) */
		if (!(cret & VM_FAULT_FALLBACK))
			return cret;
		/* VM_FAULT_FALLBACK = 本事务路径暂不支持 (如 M5 前的 FOLL_FORCE COW 桩)
		 * → 转换为 VM_FAULT_SIGSEGV (M3 内凡 fallback 必是致命类, 见 §5 矩阵) */
		return VM_FAULT_SIGSEGV;
	}
#endif
	ret = sanitize_fault_flags(vma, &flags);   /* 原代码 memory.c:6535 */
```

覆盖面: GUP-slow (`faultin_page` → handle_mm_fault, mm/gup.c:1139; `fixup_user_fault` → handle_mm_fault, gup.c:1601)、ptrace、`get_user` 族 (插入点一的 lock_mmap 分支)、以及未来一切 handle_mm_fault 调用方 — **一处钩子, 全部收口**。已知其他调用方: mm/hmm.c:90 (hmm_vma_fault) 与 mm/ksm.c:648 (break_ksm — arena 永不设 `VM_MERGEABLE` ⇒ 不可达), 见 §8 R1。`mm_account_fault`/memcg OOM 包裹 (memory.c:6552-6553,6586) 在钩子之下不执行 — arena 慢路径自己调 `mem_cgroup_enter_user_fault/exit` 与 `mm_account_fault` 复刻 (签名照抄 6535-6586 段; mm_account_fault 定义 memory.c:6409, 覆盖 majf/minf 与 PERF_COUNT_SW_PAGE_FAULTS_[MAJ|MIN] — [P2-8] 采取**计数复刻**, 不做偏差文档化), 保持观测等价。

> 说明: 插入点一已把用户态 PF 拦截, 插入点二对用户态 PF 通常不可达; 它存在的意义是 GUP/ptrace/kernel 访问与"M3a real view ensure-alloc 需要 fault 上下文"的统一入口。两个插入点都受双门保护。

### 4.3 `corten_arena_user_fault()` — 论文 Fig.8 L15-41 的 C 版

```c
/* mm/corten_arena.c — 伪码级 (函数签名可执行) */
bool corten_arena_user_fault(struct mm_struct *mm, unsigned long addr,
			     unsigned long error_code, struct pt_regs *regs,
			     unsigned int *flags)
{
	struct corten_arena *ar;
	struct corten_txn txn;
	struct corten_pte_meta m;
	bool write = *flags & FAULT_FLAG_WRITE;

	ar = corten_arena_lookup_get(mm, addr);       /* §2.3: xa_load + percpu_ref_tryget_live */
	if (!ar)
		return false;                             /* 非 arena → legacy */

	/* [P1-1] 投机分配: folio_prealloc(order-0, need_zero=true) + memcg charge +
	 * throttle_swaprate 全部在取写锁**之前**完成 — desc 写锁持有段内 0 分配 0 睡眠
	 * (include/linux/corten.h:74-76 "write-lock holders do GFP_NOWAIT only" 红线,
	 * MASTER_PROMPT §7 地雷 1)。need_zero=true [P1-2]: 匿名首触必须清零
	 * (false 是从 wp_page_copy 调用点误抄, 那里由 __wp_page_copy_user 负责填充),
	 * 否则陈旧内核内存信息泄露。仅写故障 (将走 map-anon) 需要; 读故障 (零页) 与
	 * CORTEN_MAPPED 恢复不分配。-EAGAIN/pte 竞争败者: folio_put 后重分配重入。 */
	folio = write ? folio_prealloc(mm, shadow_vma(ar), addr & PAGE_MASK,
				      /*need_zero=*/true) : NULL;
	if (write && !folio)
		goto err_oom;

	for (;;) {   /* 重试循环: FAULT_FLAG_TRIED 上限 2 次 (与 fault.c:1408-1411 同构) */

	if (corten_arena_fill_upper(ar, addr))        /* §4.4: PMD 缺才走, 稀有 */
		goto err_retry;

	if (corten_lock_range(mm, addr & PAGE_MASK, PAGE_SIZE, &txn)) {
		switch (err) {
		case -EAGAIN:  folio_put(folio); continue; /* Fig.7: stale → 重试 (重入前重分配 folio) */
		case -ENOMEM:  goto err_oom;
		case -ENOENT/-EOPNOTSUPP: goto err_legacy_fallback;  /* 见下 */
		}
	}

	if (corten_query(&txn, addr & PAGE_MASK, &m))
		goto err_inval;

	switch (m.state) {                            /* ===== 分派表 ===== */
	case CORTEN_PRIVATE_ANON:
		if (!corten_perm_ok(&m, write, *flags))   /* WRITE/EXEC/READ vs CORTEN_PERM_* */
			goto err_accerr;                      /* → SEGV_ACCERR */
		corten_arena_map_anon(&txn, ar, addr, &m, write, folio);  /* §4.6; 成功则
				/* alloc ref 转移给 PTE (legacy 同语义), 循环外不再 put */
		break;
	case CORTEN_MAPPED:
		if (write && !(m.perm & CORTEN_PERM_WRITE)) {
			if (m.flags & CORTEN_PF_SHARED)
				goto err_cow_m5_stub;             /* M5; M3 → SIGSEGV + WARN_ONCE(1) */
			goto err_accerr;
		}
		corten_arena_restore_pte(&txn, ar, addr, &m);       /* PTE 被 NUMA-protnone/
				/* 越出型破坏时按 metadata 重建; M3 仅处理 protnone 修复 (§4.7 尾) */
		break;
	case CORTEN_INVALID:   goto err_maperr;           /* 未 mmap 声明 → SEGV_MAPERR */
	case CORTEN_SWAPPED:   goto err_swap_m6_stub;     /* M3 不可达: M3 不产生该状态;
				/* 编译期 WARN_ON_ONCE + SIGSEGV, M6 换 do_swap_page 事务版 */
	case CORTEN_FILE_MAPPED / CORTEN_SHARED_ANON:
			       goto err_notsup_stub;          /* 同上, M4+ 才有生产者 */
	}
	corten_unlock(&txn);
	percpu_ref_put(&ar->active);
	return true;                                  /* for(;;) 的成功出口 */

err_*:  /* 每个出口: [P1-1] folio_put(投机 folio) + corten_unlock + put + 对应投递:
	 * err_accerr  → bad_area_access_error(regs, error_code, addr, NULL, NULL)
	 * err_maperr  → bad_area_nosemaphore(regs, error_code, addr)
	 * err_oom     → pagefault_out_of_memory()  (x86 fault.c:1438 同款)
	 * err_retry   → for(;;) 内 continue 重试 (FAULT_FLAG_TRIED 上限 2 次, 同构
	 *               x86 fault.c:1408-1411), 超限转 legacy。 */
}
```

`-ENOENT/-EOPNOTSUPP` fallback 的含义: `corten_real_root` 发现 PT 页"存在但未被 corten 追踪" (boot 早期/分配失败的页, mm/corten.c real view 注释) 或 1G/THP leaf (`pud_leaf/pmd_leaf` → -EOPNOTSUPP) — 此时回 legacy 走 `handle_mm_fault` 完全正确 (shadow-VMA 存在, legacy 路径自洽)。arena 的不变量是"**arena 地址恒有 shadow-VMA 兜底**", 这正是 D1 opt-in 设计的安全网。

### 4.4 `corten_arena_fill_upper()`: 无 mmap_lock 的上层页表 ensure-alloc

M3a 遗留口子: `corten_real_root` (mm/corten.c) 要求 PMD 已存在, 否则返回 NULL (hole); root 级 hole 无父页可升级, `corten_txn_begin` 注释明言 "the real view wires its root-level ensure-alloc from inside the fault path (3b), where the upper page-table locks make the allocation exclusive"。接线:

```c
static noinline int corten_arena_fill_upper(struct corten_arena *ar, unsigned long addr)
{
	int ret = 0;

	mutex_lock(&ar->fill_lock);              /* 同窗口并发 fill 互斥 (每 arena 一把, 冷路径) */
	if (likely(pmd_present(...))) goto out;  /* 重查: fill 竞争者可能已完成 */

	/* 与 __handle_mm_fault 同一套原语 — 6.18 已证明这些可以在没有
	 * mmap_lock 的情况下调用: per-VMA lock 路径下 handle_mm_fault 就是
	 * 只持 VMA 锁进入的 (memory.c:6294-6295 注释, 6317-6352 的
	 * p4d_alloc/pud_alloc/pmd_alloc/pte_alloc 链)。
	 * legacy 侧永远不会并发 fill arena 窗口: 一切进入 arena 的 fault
	 * 都被 §4.1/§4.2 两个钩子改道, 而 shadow-VMA 的摘除须先过 §6.3 排空。 */
	if (p4d_alloc(mm, pgd_offset(mm, addr), addr) || ...) ret = -ENOMEM;
out:
	mutex_unlock(&ar->fill_lock);
	return ret;
}
```

同时把 `corten_real_alloc` 的 -EOPNOTSUPP 桩升级为真实现: `ops->alloc` 在"父页写锁已持有"语境下被调用 (mm/corten.h @alloc 契约), 但 real view 的父链止于 PMD — 因此 real view 的 alloc 仍保持"从 fault 路径带 `fill_lock` 语境进入"的调用方式: 即 M3b 的做法是**在事务前**由 `corten_arena_fill_upper()` 补齐上层 (fill_lock 互斥), 让 `corten_lock_range` 的 `ops->root` 命中; 而事务内 L5' 的 `ops->alloc` (覆盖页以下) 在 x86-64 real view 中永远不需要 (覆盖页就是 PTE 级页) — **`corten_real_alloc` 保持 -EOPNOTSUPP 桩不变**, 这正是 mm/corten.c 注释预设的接线方式 ("3b wires the real allocation here -- from inside the fault path") 的最小实现: fault 路径作为唯一调用方在事务入口前完成上层分配。

> fill 与 `__pte_alloc` 的区别: 不需要 `mmap_lock`; 依赖 (a) fill_lock 串行化 arena 侧 fill, (b) legacy 侧被钩子改道, (c) RELEASE 先排空 (§6.3)。

### 4.5 与 `handle_mm_fault` 的关系: **完全绕过** (论文语义), 不换路径调用

- 论文 Fig.8 L17-18: fault handler `lock(fault_range)` → `query` → 按 metadata 分派 `map`, 全程在事务内; §4.2 "the whole complex page fault handling logic executes in a transaction"。
- M3b 用户态热路径 (插入点一) 在 `lock_vma_under_rcu` (mmap_lock.c:224) / `lock_mm_and_find_vma` (mmap_lock.c:425) **之前**返回 — 对 VMA 树、maple tree、mmap_lock、per-VMA seqlock 完全零接触。"调 handle_mm_fault 但换路径"的方案 (给 vmf 塞假 vma 等) 会拖入 `sanitize_fault_flags`/`arch_vma_access_permitted` (memory.c:6535-6544) 的 VMA 语义依赖, 与论文不符 — 弃。
- 唯一例外是 §4.3 的 fallback (PT 页未追踪/大页 leaf): 该分支显式**降回** legacy — 安全网, 非主路径。

### 4.6 map 的 PTE 写入序列 (+ TLB 无需 flush 论证)

`corten_arena_map_anon()` — 逐行镜像 `do_anonymous_page` (mm/memory.c:5166-5279) 的顺序, 但锁环境不同:

```c
/* 前置 (P1-1): @folio 已由调用方在 corten_lock_range() 之前投机分配
 * (folio_prealloc, need_zero=true [P1-2], memcg charge + throttle 在内)。
 * 本函数 (即 desc 写锁持有段) 不分配不睡眠 — include/linux/corten.h:74-76
 * "write-lock holders do metadata work with GFP_NOWAIT only" 契约。 */
static int corten_arena_map_anon(struct corten_txn *txn, struct corten_arena *ar,
				 unsigned long addr, struct corten_pte_meta *m,
				 bool write, struct folio *folio)
{
	/* ① 预构 pte 值 (镜像 memory.c:5232-5235, 无锁) */
	__folio_mark_uptodate(folio);                       /* memory.c:5229: 页内容先于 set_pte 可见 */
	entry = folio_mk_pte(folio, shadow_page_prot(ar, m, write));  /* memory.c:5232 同款 */
	if (write) entry = pte_mkwrite(pte_mkdirty(entry), vma);      /* memory.c:5234-5235 */

	/* ② PTE 写入 (镜像 memory.c:5237-5246), 但互斥点是 desc->lock(写) + ptl */
	ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);    /* ptl 仍是 PTE 互斥点 (§6.1 规则 R2) */
	if (!ptep) goto out;
	if (unlikely(!pte_none(ptep_get(ptep)))) {          /* 竞争重查 (vmf_pte_changed 同款, memory.c:5240) */
		pte_unmap_unlock(ptep, &ptl);
		folio_put(folio);                    /* 投机引用退回 (上层 for(;;) 重分配重试) */
		return -EAGAIN;
	}
	/* ③ mm 稳定校验 + 记账 (镜像 memory.c:5248/5259-5263; 差异: 不进 LRU) */
	if (unlikely(check_stable_address_space(mm))) {     /* [P2-8] memory.c:5248 同款 (排除
			/* coredump/unstable 期安装映射); 失败同竞争处理 */
		pte_unmap_unlock(ptep, &ptl);
		folio_put(folio);
		return -EAGAIN;
	}
	/* nr_pages==1 ⇒ alloc 返回的唯一引用直接转为 PTE 引用, 不再 folio_ref_add
	 * (memory.c:5259 的 folio_ref_add(folio, nr_pages-1) 在 order-0 时为 0 —
	 * r1 此处误写 +1, 本版修正); 失败路径 folio_put, 成功路径引用归 PTE。 */
	add_mm_counter(mm, MM_ANONPAGES, 1);
	folio_add_new_anon_rmap(folio, shadow_vma, addr, RMAP_EXCLUSIVE);  /* memory.c:5262 同款;
		/* M3 关键差异: **不调** folio_add_lru_vma (memory.c:5263) —
		 * 页不进 LRU ⇒ 回收/迁移在 M3 永远碰不到 arena 页 (M6 接 LRU+rmap 互操作) */
	set_ptes(mm, addr, ptep, entry, 1);                 /* memory.c:5267 同款 */
	update_mmu_cache_range(NULL, vma, addr, ptep, 1);   /* memory.c:5270 */
	pte_unmap_unlock(ptep, &ptl);

	/* ④ metadata (M3a API): PTE 与 metadata 在同一 desc 写锁事务内, 原子对其他事务可见。
	 * corten_map 收 struct page*: 实参 folio_page(folio, 0) [P2-8]。 */
	corten_map(txn, addr, folio_page(folio, 0), perm, /*flags=*/0);   /* CORTEN_MAPPED */
	return 0;
}
```

**读故障零页**: 镜像 memory.c:5186-5190 (`pte_mkspecial(pfn_pte(my_zero_pfn(addr)))`, 只读) + ptl 内 `check_stable_address_space` (memory.c:5199 同款, [P2-8]) + 直接 goto-setpte (不 rmap 不计账 — 5191-5207 与 5207 `goto setpte` 的结构), metadata 仍记 `CORTEN_PRIVATE_ANON` (perm 无 WRITE)。后续写故障按 `CORTEN_PRIVATE_ANON` 走 ① 正常分配。

**TLB 无需 flush 的论证** (与 memory.c:5269 注释同构): 本 PTE 之前为 `pte_none` (② 已重查), 即该 VA 在本 CPU 的 TLB 中不可能存在有效翻译 — **一个从未 present 的 PTE 的安装不需要任何 invalidate**; x86 上 `set_ptes` 是普通 store, 全局可见性由 ② 的 ptl 释放屏障保证 (与 legacy 相同)。不满足"之前为 none"的情形 (NUMA-protnone 修复, §4.7) 走 `ptep_set_access_flags` (memory.c:6271 同款, 内部按需 flush)。M5 COW 的 `ptep_clear_flush` (memory.c:3751) 语义在 M5 设计中处理。

**rss/记账汇总**: `MM_ANONPAGES` 增 (add_mm_counter), memcg 在 `folio_prealloc` 内 charge (投机段完成, P1-1), `total_vm` 不变 (shadow-VMA 已计), `mm_account_fault` 在 §4.2 慢路径复刻。proc/smaps 看到的数字与 legacy 语义一致 (§3.4)。

### 4.7 khugepaged / THP 在 arena 的排除机制 (核对清单)

| 排除点 | 机制 | 证据 |
|---|---|---|
| khugepaged 扫描 (PTE 表 → PMD collapse, 会**释放 PT 页** → 摧毁 covering 锁协议!) | shadow-VMA 带 `VM_NOHUGEPAGE` ⇒ `thp_vma_allowable_order(TVA_KHUGEPAGED)` 在 `vma_thp_disabled` 首检查即 0 | khugepaged.c:2425 (扫描门); huge_memory.c:132-143; huge_mm.h:331-336 (`VM_NOHUGEPAGE → return true`, 先于 forced_collapse 分支) |
| MADV_COLLAPSE 强制 collapse | 同上, `TVA_FORCED_COLLAPSE` 亦被 `VM_NOHUGEPAGE` 短路; 且 madvise 钩子先行拒绝 (§5.8) | khugepaged.c:1530, 2756; huge_mm.h:335 |
| THP @ fault (PUD/PMD) | arena fault 不进 `__handle_mm_fault` 的 `thp_vma_allowable_order(TVA_PAGEFAULT)` 路径 (PUD: memory.c:6326-6327, PMD: 6360-6361); 万一 fallback 进 legacy, `VM_NOHUGEPAGE` 仍挡 | memory.c:6326-6327,6360-6361; huge_mm.h:335 |
| mTHP (alloc_anon_folio 的多 order) | arena map 只走 order-0 显式分配 (§4.6), 不调 `alloc_anon_folio`; fallback 时 `VM_NOHUGEPAGE` 挡 | memory.c:5150-5158 |
| MADV_HUGEPAGE 洗掉 NOHUGEPAGE | `hugepage_madvise` 会清 `VM_NOHUGEPAGE` (khugepaged.c:354-355) — 必须在 madvise 钩子拒绝 shadow-VMA 上的 MADV_HUGEPAGE/NOHUGEPAGE | §5.8 |
| NUMA balancing 的 protnone 化 (`change_prot_numa`, 持 mmap_lock 写, 不在 M3 拦截面内) | arena fault 对"present 但 protnone"的 PTE 按 metadata 重建权限 (走 `ptep_set_access_flags`), 语义=退化为无 NUMA 优化; 兜底自洽 | memory.c:6255-6256 (legacy 判定点, 供对照) |

---

## 5. 互操作矩阵 (M3 行为定案)

图例: ✅=支持; 🔁=路由到 corten 事务; ⛔=拒绝 (-EOPNOTSUPP); 🛡=fallback legacy; ⏳=M5/M6 桩。

| # | 操作 / 路径 | M3 行为 | 机制与证据 |
|---|---|---|---|
| 5.1 | **fork** (`dup_mmap`, mm/mmap.c:1739, 持 oldmm `mmap_write_lock_killable` :1766) | **M3: ⛔ fail-fast** — dup_mmap 循环内见 `VM_CORTEN` 即 `retval=-EOPNOTSUPP; goto out` (fork 返回错误, 父进程无损)。**M5 定案 (本设计推荐)**: **全量复制 metadata** (仅被 touch 过的 PT 页有 meta array, 每 2M 窗口一次 4KB memcpy, 持 oldmm mmap 写锁 + 逐 PT 页 desc 写锁+ptl, `folio_add_new_anon_rmap(new_folio, dst_vma,...)` 模式即 memory.c:1073), 同时双侧 `CORTEN_PF_SHARED` 置位、PTE 只读化 — **否决 copy-on-first-fault**: 子进程首写故障就会触发共享 meta 数组的 COW-split, 实现 (引用计数 meta + split) 远贵于顺路 memcpy; 论文 Fig.8 L26-34 的 COW 判定 (`map_count==1` 免拷贝) 属 M5, M3 的 fork COW 恒拷贝 | dup_mmap 在 mm/mmap.c:1739 (android 树移位; 非 kernel/fork.c — fork.c:1516 只是调用点) |
| 5.2 | **exit / 进程销毁** (`exit_mmap`, mm/mmap.c:1272) | ✅: 入口段插 `corten_arena_mm_exit(mm)`: 对每个 arena 执行 §6.3 排空 (kill_and_confirm+wait_for_completion), 然后 legacy `unmap_vmas/free_pgtables` 照常 — PT 页经 pte_free funnels → M2a `corten_on_pte_free` (arch/x86/mm/pgtable.c:45) → uninstall (desc 消亡)。exit 时 mm_users==0 无并发 fault, 排空为防御性 | mmap.c:1272-1333 |
| 5.3 | **GUP fast** (`gup_fast`, mm/gup.c:3157; `get_user_pages_fast` :3230) | ✅ 无 VMA 上下文问题: gup_fast 是无锁 PTE 走查 (R2 规则下 arena PTE 写必持 ptl ⇒ 与 lockless 读安全的竞速关系与 legacy 相同); 页在则取走 (FOLL_GET/FOLL_PIN 均为普通 folio 引用); 页不在 → 落到 slow | gup.c:3157-3230 |
| 5.4 | **GUP slow / fixup_user_fault** (gup.c:1367/1577) | 🔁: `find_vma` 命中 shadow-VMA → `handle_mm_fault` → §4.2 钩子 → 事务路径。FOLL_WRITE 缺页 = 正常 map; **FOLL_FORCE 写只读页 = ⏳ M5 桩** (§4.2 返回 SIGSEGV, ptrace 写 arena 在 M3 失败); **FOLL_PIN×回收互斥 = ⏳ M6 桩** (M3 页不进 LRU, 无回收, pin 天然安全, M6 接 LRU 时须先解此桩) | gup.c:1139,1601 |
| 5.5 | **munmap 区内 chunk** (sys_munmap, mm/mmap.c:1088 → `do_vma_munmap`, vma.c:1646) | 🔁: 钩子在 `sys_munmap`/`do_munmap` (mmap.c:1074) 入口: [start,end) 完全落在单一 arena 内 → `corten_arena_munmap()`: 按 2M 窗口 { `corten_lock_range(≤2M)` → ptl 内逐页 `pte_clear`+`folio_remove_rmap_pte`+`dec_mm_counter(MM_ANONPAGES)`+取 folio 引用 → `corten_unmap` (metadata→INVALID) → `corten_unlock` → **TLB batch flush** → folio_put }。**shadow-VMA 不变** (VA 保留, `total_vm` 不变 — 论文 unmap=清内容留 VA, Fig.8 L9-13); 跨界/精确整区 → 转换为 RELEASE (§2.1) 或 ⛔。**边界论证**: 摘 PTE 与 flush 之间他线程可重 map 同 VA, 该 flush 按范围清 TLB 只会引发多余 re-fault, 无正确性风险 (旧页引用在 flush 后才 put, 同 mmu_gather 语义, unmap_vmas 注释 mm/memory.c:2092-2094) | mmap.c:1074,1088; vma.c:1564,1577,1646 |
| 5.6 | **mmap MAP_FIXED 区内** (`do_mmap`, mm/mmap.c:340) | 🔁 **路由门 [P1-4]: `MAP_FIXED && MAP_PRIVATE && MAP_ANONYMOUS && !MAP_HUGETLB && !MAP_GROWSDOWN`** 且完全落在单一 arena 内 → `corten_arena_mmap()` = 论文 Fig.8 L1-7: `corten_lock_range(chunk≤2M 迭代)` → 空间已被 shadow-VMA 全局保留故 query 必为 INVALID → `corten_mark(chunk, PRIVATE_ANON(perm))` → unlock。无 VMA 变更、无 `vm_stat_account` (total_vm 已含)。**`MAP_FIXED_NOREPLACE` 落区内 → `-EEXIST`** (uapi 契约: 不得当作可覆盖区间路由)。其余 (无 MAP_FIXED 落点进 arena / 部分跨界 / 带 hugetlb·growsdown) → ⛔/自然落外 (get_unmapped_area 不会选中被 shadow-VMA 占用的区间) | mmap.c:284,340; 论文 Fig.8 |
| 5.7 | **mprotect / pkey_mprotect** (`do_mprotect_pkey`, mm/mprotect.c:863,1010) | ⛔ -EOPNOTSUPP (M3): mprotect 会 split shadow-VMA 且逐 PTE 改写不持 desc->lock → 违反 R2。权限变化在 arena 语义里= `corten_mark(perm 更新)` 事务, M4 放开 (钩子已有, 只换实现) | mprotect.c:863,1010 |
| 5.8 | **madvise** (`do_madvise`, mm/madvise.c:2000; 行为判定循环内 per-VMA) | 分档: `MADV_DONTNEED` 完全区内 → 🔁 等价 5.5 (清内容留 VA); `MADV_HUGEPAGE/NOHUGEPAGE/COLLAPSE` → ⛔ (防洗掉 VM_NOHUGEPAGE, khugepaged.c:354-365); 其余全部 ⛔ (M3 最小矩阵; NORMAL/SEQUENTIAL 等纯 hint 留 M4 评估放行) | madvise.c:2000; khugepaged.c:354-365 |
| 5.9 | **mremap** (mm/mremap.c:1962 `SYSCALL_DEFINE5`; `vma_to_resize` 校验链) | ⛔ -EOPNOTSUPP (M3)。**对齐 MASTER M4 授权**: M4 的 DoD "mremap arena 内→回退 legacy 路径并记录" 在 M3 的实现前置条件 (move_ptes 需 desc->lock 批量互斥 + shadow-VMA split 合法化) 不具备, 故 M3 钩子先行拦截报错; M4 把 ⛔ 换成"排空后回退 legacy" (§8 OQ-5) | mremap.c:1962 |
| 5.10 | **brk** (mmap.c:179 `do_vmi_align_munmap`) | 🛡: heap 与 arena 不重叠为常态; 万一 shrink 进入 arena, 5.5 的 do_vmi_align_munmap 层钩子同样生效 (跨界 ⛔)。无专用代码 | mmap.c:175-179 |
| 5.11 | **/proc/pid/maps · smaps · numa_maps** (task_mmu.c:501/1157) | ✅ 如实 (§3.4): maps 显示 `[anon:corten_arena]` 区间; smaps 数字真实 (MM_ANONPAGES 计账), THPeligible=0 | task_mmu.c:424-475,501,1157 |
| 5.12 | **ptrace / process_vm_*** | ⛔ (经 5.4): 读 ✅ (present 页 gup 直取); FOLL_FORCE 写 → M5 桩 SIGSEGV。文档化 M3 限制 | gup.c:1100,1139 |
| 5.13 | **userfaultfd 注册** (fs/userfaultfd.c:1240 `userfaultfd_register`) | ⛔: 注册循环 per-VMA 校验处加 `VM_CORTEN` 拒绝 (MISSING/WP 皆无意义 — fault 不进 handle_mm_fault) | userfaultfd.c:1240,1379 |
| 5.14 | **mlock/mlock2** | ⛔ (M3): mlock 走 gup populate (5.4 兼容) 但置 `VM_LOCKED` 参与 reclaim 语义面; M3 拒绝保持矩阵最小 | — |
| 5.15 | **mseal** (`VM_SEALED`, mm.h:447-450) | ⛔ (M3): 拒绝落点 = **`do_mseal()` 入口** (mm/mseal.c:139) 的 range 校验处加 arena 相交检查 (mseal 冻结 VMA 属性与 arena 事务化空间操作语义冲突, 未评估即拒) | mm/mseal.c:139 |
| 5.16 | **khugepaged / THP / KSM** | ✅ 排除 (§4.7); KSM: `VM_MERGEABLE` 永不设置 (ksm 只作用于 madvise MERGEABLE 的 VMA,DECLARE 白名单不含) | §4.7 表 |
| 5.17 | **swap/MGLRU 回收** | ⏳ M6 (M3 页不进 LRU ⇒ 回收不可达; 这是"arena 内存不受回收"的**声明性限制**, 写入文档与 debugfs 计数) | §4.6 差异注释 |
| 5.18 | **prctl(PR_SET_VMA / ANON_NAME)** (kernel/sys.c:2413 → `set_anon_vma_name`, 无 cap) | ⛔: 按名字边界会拆/建 VMA (shadow-VMA split 不可接受), 且改名影响 merge 判定 (vma.c:105); do_mmap 同类入口检查覆盖 (range 与 arena 相交即拒) | kernel/sys.c:2413 |
| 5.19 | **mbind / do_mbind** (mm/mempolicy.c:566) | ⛔ (M3): do_mbind 持 mmap_write_lock 做 vma_start_write + 迁移/PTE 改写, 不持 desc->lock → 违反 R2; 入口 range 相交 arena 即 -EOPNOTSUPP | mempolicy.c:566 |
| 5.20 | **hwpoison / memory_failure** (mm/memory-failure.c) | ⛔+噪声 (M3 不实现, 但**不得沉默**): collect/unmap 走 rmap 不持 desc->lock, 对 arena 页会破坏 R1 封闭性 ⇒ M3 在处理 arena PFN 时 `WARN_ONCE` + 拒绝 (页保持映射, 文档化"arena 页不参与 hwpoison"), 计入 debugfs 计数; M6 一并设计 | 稀有路径, 无 M3 实装点 |

**红线自检**: 所有 ⛔/🔁 钩子皆 `if (!corten_enabled_static() || !mm->corten_state) return legacy;` 开头 — `CONFIG_CORTEN_MM=n` 编译消除, `corten=off` 一跳, 满足铁律 3 与 D1。

---

## 6. 锁序与并发

### 6.1 全局锁序图 (从外到内 = 允许的获取顺序)

```
  xa_lock(corten_state->arenas)         (DECLARE/RELEASE 才取; 永不在其下取任何 desc/ptl)
      │
      ▼
  arena_ctl_lock (state->ctl_lock)      [P1-3] DECLARE/RELEASE 含 drain 全程持有;
      │                                  fault 路径不取 ⇒ drain 在锁下等待无环 (§6.3)
      ▼  (RELEASE: xa_erase+kill+drain 可与 mmap_write_lock 同锁段完成 — 无环)
  mmap_lock (W 或 R)                    (legacy 路径; arena fault 永不取它)
      │
      ▼
  vma write marks (vma_start_write)     (仅 DECLARE/RELEASE; 不与事务并发 — 已排空)
      │
      ▼
  percpu_ref(active)                    (不是锁, 是 drain 屏障; fault 取/放, RELEASE 等 0)
      │
      ▼
  arena->fill_lock (mutex)              (仅 fill_upper; 持有时不持任何 desc->lock ⇒ 在图中是自由叶)
      │
      ▼
  desc->lock (写)  [BH 对称, 唯一可嵌套场景 = 协议自顶向下多级, 严格降 level; M3 real view 仅 1 把]
      │
      ▼
  ptl (pte_offset_map_lock)             (PTE 互斥点)
```

**R1 (核心规则)**: 一切对 arena PT 页做 PTE/metadata 突变的路径 — arena fault (§4.6)、arena munmap (§5.5)、fork 复制 (§5.1, M5)、RELEASE 前排空后的 legacy zap (此时无并发事务) — 都持有该 2M 窗口的 `desc->lock` 写锁; 其下再取 ptl。**锁序 desc->lock(写) → ptl 全内核唯一**, 不可反序。

**R2 (收敛措辞 [P2-7])**: **PTE 语义内容 (present/prot/pfn) 的突变必持 desc 写锁**; advisory 位**显式枚举豁免** — soft-dirty 清位 (clear_soft_dirty)、young/accessed 位操作可仅在 ptl 下进行, 不需要 desc->lock。不做超出此清单的全称承诺: 未来任何新 advisory 位语义必须先归类进本豁免清单 (review 检查项)。metadata (CORTEN_INVALID/MAPPED/...) 只被事务读写者碰 (desc 写锁), M3 无 legacy metadata 读者。

**BH 红线继承** (M3a review 红线 1, include/linux/corten.h:503-514): arena 层新增的一切 desc->lock 触点 (5.5 munmap 迭代、5.1 fork 复制、§6.3 排空校验) **必须使用 `_bh` 变体** — 直接经由 `corten_lock_range/corten_unlock` 获得者天然合规 (M3a 已改 `_bh`), 不得绕过协议 API 直取 desc->lock。

### 6.2 序列图: fault × fault × munmap(chunk)

```
 CPU0 fault@addr1      CPU1 fault@addr1(同2M窗)   CPU2 munmap[chunk]   RELEASE 整区
 ───────────────      ────────────────────────   ──────────────────   ────────────
 xa_load→ar, get       xa_load→ar, get
 fill_upper?(无)        fill_upper?(无)
 lock_range(addr1)      lock_range(addr1)          
   desc->lock W ✔         desc->lock W ══阻塞══
   query/map/set_pte       ...                       对每个窗口:
   corten_unlock          ...等待...                   lock_range(win)
   put                   desc->lock W ✔                  desc->lock W (R1)
                          (② 重查 pte_none 裁决              zap+unmap
                           双 map 竞争, 败者 -EAGAIN          unlock+flush+put
                           由 fault 层 for(;;) 重试)
                                                    排空: kill_and_confirm+wait_for_completion
                                                      (CPU0/1 的 put 计数归零)
                                                    xa_erase → legacy munmap shadow-VMA
                                                      (mmap_write_lock, 无事务并发)
```

关键点: (a) 不同 2M 窗口的 fault 完全并行 (每窗一把 desc 写锁 — 论文 §3.3 concurrency semantics); (b) 同窗 fault 串行于 desc 写锁, 唯一竞争裁决点是 §4.6 ② 的 `pte_none` 重查 + metadata `-EEXIST` (CORTEN_MAP_FORCE=0 的防双 map); (c) munmap/fault 互斥于 desc 写锁 — 论文 Fig.6/7 的 unmap-vs-lock 竞争在本层由互锁吸收, 无需 stale-retry (stale-retry 只服务 PT 页退役, M4)。

### 6.3 排空方案 (RELEASE / exit): percpu_ref, **否决 arena 级 rw 信号量**

任务预案曾提 "arena 级 rw 信号量"。**否决理由**: fault 热路径必须取读侧 ⇒ 每 fault 一次全局 rwsem 原子操作, 跨 CPU 同一 cacheline 竞争 — 恰好复刻 M1 已量化的 mmap_lock 卡颈 (STATE.md M1 发现: 30s trace 101 万 PF vs 50 mmap_lock 事件, per-VMA 锁已解决读侧; 我们不能把读侧问题重新引入)。**选定: `percpu_ref(active)`**:

- fault: `percpu_ref_tryget_live()` (xa_load 后, rcu 内) … `percpu_ref_put()` (flush 后) — 每 CPU 本地计数, 零 cacheline 竞争;
- RELEASE/exit [P1-3]: **全程持 `state->ctl_lock` (mutex)**: `xa_erase` (此后 lookup 不中, 新 fault 回 legacy 报 MAPERR — 正确, 区在消失) → `percpu_ref_kill_and_confirm(&active, confirm_cb)` + `wait_for_completion(&drained)` (标准同步排空模式; 6.18 percpu-refcount.h:129 — 无 `percpu_ref_wait_for_zero`) → 取 mmap_write_lock 执行 legacy munmap (摘 VMA, free PT 页, upper 页表) → `percpu_ref_exit()` → `kfree_rcu(ar)`。**无环论证** (修正 r1 的误判句 "drain 须先于持锁完成"): (a) fault 路径对 ctl_lock 与 mmap_lock 都是零接触 — 它只做 percpu_ref tryget/put, 故 drain 在 ctl_lock (乃至 mmap_write_lock) 下等待时不存在等待对; (b) confirm 回调运行在 softirq/RCU 上下文且只做 `complete()`, 不碰 mmap_lock; (c) 排空完成前不触碰 VMA/页表。r1 的"先 drain 后持锁以免锁序环"约束撤销。

**"shadow-VMA 写操作 (munmap 整区) 怎么与 arena 事务互斥"的最终答案**: 不靠锁共域, 靠生命周期 — 先把所有在途事务排空 (percpu_ref), 再持 mmap_lock write 执行纯 legacy 拆除。`mmap_lock` 永远不会与 desc->lock 形成等待对 (fault 路径不取 mmap_lock; 拆除路径取 mmap_lock 时已无事务存在) ⇒ 图 6.1 无环。

### 6.4 2b 遗留契约 ("调用方层级稳定") 的消除

M3a 契约 (include/linux/corten.h:341-344): 调用方必须让被走查的上层页表层级在 `corten_lock_range` 期间保持存活 (当时靠 mmap_lock 读)。arena 设计逐条解除:

1. **上层 (PGD/PUD/PMD) 存活性**: 只在两处被释放 — RELEASE 的 legacy munmap、exit。两者都先过 `percpu_ref` 排空 (§6.3), 而事务自 xa_load 起到 unlock+put 止持有 active 引用 ⇒ 事务存在 ⇒ 上层不可能被释放。**契约被 percpu_ref 屏障替代。**
2. **PTE 级 (被追踪) 页存活性**: M3a 互锁 — uninstall 在 desc 写锁下发布 stale (include/linux/corten.h:56-76), 事务体拥有覆盖页; pin/stale 协议 (kfree_rcu + -EAGAIN, Fig.7) 兜底读侧。
3. **fill 竞争**: fill_lock + "legacy 永不 fill arena 窗" (§4.4) — 2b 时代不可想见的无锁 fill 在 arena 边界内是安全的, 因为**一切进入 arena 的写路径都在钩子后面**。
4. M4 的深层退役 (上层 PT 页经事务协议退役, 论文 Fig.6 rev_dfs) 仍按 M3a 红线 2 (include/linux/corten.h:516-522) 执行: 先标记后代 stale, 再 rcu 释放祖先 — 本设计不变量 1/2 是它的前提而非替代。

---

## 7. 测试计划

### 7.1 arena 压测器 `bench/arena-stress/` (项目侧非内核侧; 测试件已在此就位: `arena_stress.c` / `maps_check.sh` / `ksmoke.sh` / `README.md` — prctl 打包以本设计的 arg2=op 版为准, README 已对齐)

```
用法: arena_stress [-s ARENA_MB(默认1024)] [-t THREADS(默认8)]
                   [-i ITER(默认20000)] [-c CHUNK_KB(默认64)] [-S SEED] [--verify]
每线程 (per-core 独立 chunk 游标, 模拟论文 per-core VA 分配器的用户态版):
  loop ITER 次:
    off  = 本线程 PRNG (xorshift64*) 选 [0, ARENA_SIZE-CHUNK) 的 4K 对齐偏移
    mmap(base+off, CHUNK, RW, MAP_FIXED|PRIVATE|ANON)      /* 声明式分配 */
    for (每页) p[i] = checksum_seed ^ (addr|iter)           /* 触页: 写 fault */
    sched_yield() / 可选读扫描 (触发读 fault 与零页路径)
    munmap(base+off, CHUNK)                                 /* 内容丢弃 */
    若 --verify: 立刻重新 mmap+全页读校验 (须全 0 — unmap 后内容不得残留)
通过判据 (全部满足):
  ① 零 panic / 零 WARN / 零 lockdep 报警 (corten=on 构建含 PROVE_LOCKING 变体)
  ② --verify 校验和 100% 正确 (多轮 × 多线程 × {1,2,4,8} vCPU)
  ③ /proc/self/maps 恰好出现 1 条 (或合并后 1 条) [anon:corten_arena] 大区间,
     起止与 DECLARE 参数一致 (用脚本解析)
  ④ debugfs 计数闭合: faults_mapped == pages_touched(压测器自计)±已知零页偏差;
     fallbacks == 0 (M3 目标; 出现即 bug)
  ⑤ 对照组: 同程序在同内核 corten=off 下跑通 (路径一致性回归)
压力变体: --hammer-race 模式 = 多线程同 2M 窗口相邻 chunk 抢锁 (验 §6.2),
          外加独立 fork-bomb-ish 进程循环 prctl DECLARE/RELEASE (验 §6.3 排空)
          --numa-fake2 变体 [P2-7]: 内核 cmdline 加 numa=fake=2 重跑全参数 —
          单节点 QEMU 下 §4.7 的 prot_numa 修复路径 (PTE present+protnone) 不可达,
          此变体才真正激励 change_prot_numa/do_numa_page 与 arena 的交叉
```

**口径声明 [P2-8]**: M3-M8 的 fork 对照 (lmbench fork/fork+exec 等) 一律**不含 arena 进程**
(§5.1 fork fail-fast; arena fork 属 M5), 对照组与实验组隔离, 不影响 legacy fork 基线。

### 7.2 kselftests/mm 冒烟子集 (挑受影响面最大的 8 个)

我们改动了: handle_mm_fault 入口、x86 fault 入口、do_mmap/do_munmap/mprotect/madvise/mremap 入口、dup_mmap、exit_mmap、mm_struct。全部入口在 legacy 上应零变化 ⇒ 选对"这些入口压力最大"的测试:

1. `map_fixed_noreplace.c` — do_mmap/MAP_FIXED 路径 (我们挂了钩子)
2. `mremap_test.c` + `mremap_dontunmap.c` — mremap 校验链 (5.9 拒绝逻辑不得外溢)
3. `madv_populate.c` — madvise × gup populate 交叉 (5.4/5.8)
4. `cow.c` — COW 面回归 (我们碰了 fork/rmap 记账相邻代码, M5 前基线)
5. `mkdirty.c` — PTE dirty 位语义 (与 §4.6 写入序列同面)
6. `protection_keys.c` — pkey 位面 (我们审计过 pkey/高位 flags 相邻位)
7. `khugepaged.c` / `split_huge_page_test` — collapse 语义回归 (§4.7 排除机制不得误伤 legacy)
8. `gup_longterm.c` + `gup_test` — GUP fast/slow 互操作 (5.3/5.4)
运行口径: corten=off 全绿 (回归); corten=on 全绿 (双门关闭时行为一致); 另加本设计新增的 arena 专项 KUnit (S2/S4/S6)。

### 7.3 perf 验证法 (M3 DoD: "perf 确认 fault 路径无 find_vma/mmap_lock 符号")

```bash
# guest 内, arena 压测器 (corten=on):
perf record -e cycles:u -g --call-graph fp -o /tmp/arena.perf -- ./arena_stress -t8 -i20000
perf script -i /tmp/arena.perf > /tmp/arena.stacks
# 判定脚本 (host 侧, bash+awk, 落 bin/verify_no_vma_symbols.sh):
#   1) 抽出所有含 do_user_addr_fault 的调用栈块;
#   2) 命中以下任一符号即 FAIL:
#      find_vma | lock_vma_under_rcu | vma_start_read | lock_mm_and_find_vma |
#      mmap_read_lock | mas_walk | handle_mm_fault | __handle_mm_fault
#      (handle_mm_fault 出现在 user-fault 栈=绕行失败; 出现在 gup/ptrace 栈=预期, 脚本
#       按"栈内是否含 get_user_pages/fixup_user_fault"分流豁免并打印计数)
#   3) 反向阳性检查: 栈内必含 corten_arena_user_fault 与 corten_lock_range (各 >N 次)
#   4) 对照组: corten=off 同跑, 1)-2) 必然大量命中 (脚本自检有效性)
# 附加 tracepoint 交叉验证 (M1 口径延续): perfetto 采 mmap_lock fault_start/end,
#   arena 进程的 user-PF 期间 mmap_lock 计数应为 0 (SQL 按 upid+ts 窗口 join)
```

### 7.4 debugfs / proc 观测点

- `/sys/kernel/debug/corten/` (M2 已建) 增文件: `arenas` (逐 arena: start/end/shadow-VMA 区间/meta 页数), `arena_stats` (percpu 聚合: fault_total/fault_mapped/sigsegv_acc/sigsegv_map/fill_upper/fallback_legacy/unmap_pages/munmap_txns/mmap_mark_txns)。
- `/proc/<pid>/maps` 的 `[anon:corten_arena]` 名即观测点 (§3.4)。
- KUnit 扩展 (`mm/corten_test.c`): arena lookup (命中/否定/跨界), DECLARE 校验链 (对齐/白名单/重叠), 排空 (kill→wait→release 次序), pte_none 竞争重查 (注入并发), 计账闭合 (MM_ANONPAGES 增减)。全部走 `corten_enabled` 关闭下的内部 API (同 M2 测试模式, mm/corten.h 注释)。

---

## 8. 风险表 + 开放问题

| # | 风险 | 影响 | 缓解 | 证据 |
|---|---|---|---|---|
| R1 | 无 mmap_lock 的上层页表 fill 与未枚举的 legacy 写路径竞争 (漏网 syscalls 直接改写 arena 窗口页表) | 页表损坏 | 双钩子收口 (§4.1/4.2) + 5.13/5.14/5.15/5.18/5.19 拒绝 + syzkaller (M7) focus mmap/mprotect/madvise/uffd; R2 收敛措辞下剩余读者只读 | mm/gup.c:1139,1601; mm/hmm.c:90 (hmm_vma_fault, 稀有); mm/ksm.c:648 (break_ksm — arena 排除 VM_MERGEABLE ⇒ 不可达) [P2-6] |
| R2 | arena 页不进 LRU ⇒ 长跑压测内存膨胀 (1G 声明区全触页=1G 常驻) | guest 4G OOM | 压测器默认 1G/分块 munmap churn 即回收; debugfs 计数监控; M6 接 LRU 后消除 | memory.c:5263 (legacy 进 LRU 点=我们的差异点) |
| R3 | khugepaged 排除依赖 VM_NOHUGEPAGE 单开关, 上游语义变化 (如 forced_collapse 绕过 NOHUGEPAGE) 会击穿 | PT 页被 collapse 释放→事务 UAF | M3a 互锁兜底 (uninstall 先 stale, 事务 -EAGAIN); §4.7 表格化核对纳入每夜 review | huge_mm.h:331-336; include/linux/corten.h:56-76 |
| R4 | ~~folio_prealloc 在写锁内使用的语境假设~~ **已消解 [P1-1]**: 分配+charge+throttle 移至 desc 写锁之前, 锁内零分配零睡眠 (include/linux/corten.h:74-76 契约天然满足) | — | 实施期仅余常规核对: `fill_upper` 的 p4d/pud/pmd 分配用 GFP_KERNEL, 同样在写锁外 (§4.4 fill_lock 下) | memory.c:1190-1210 |
| R5 | VM_CORTEN bit 43 与后续上游 64 位 flags 冲突 (merge 冲突时) | 移植冲突 | 先例 mseal bit42 (mm.h:447-450); 冲突时换 44; 记入 rebase 清单 | mm.h:446-450 |
| R6 | 512G 级 arena 的 xarray 指针表内存 (1MB/512G) + xa_store 批量耗时 | 大 arena 声明慢 | M3 不承诺 >4G arena 性能; M4 per-core VA 分配器一并重设计 | §2.2 |
| R7 | RELEASE 的同步排空 (`percpu_ref_kill_and_confirm`+`wait_for_completion`) 在任务上下文阻塞, prctl 语义变成可长阻塞 | 调用方意外阻塞 | 文档化: RELEASE 语义=同步排空 (与 munmap 等价的阻塞量级); 可换 killable 变体 (wait_for_completion_killable, 失败则 arena 留在 dying 态下次 RELEASE 收割) | percpu-refcount.h:129; §6.3 |
| R8 | fork fail-fast (-EOPNOTSUPP) 打破 arena 进程调用 fork 的库 (如 daemon 化) | 应用崩溃感知 | opt-in 实验特性文档化; guest 内压测器不 fork; M5 兑现 | §5.1 |

**OQ-1** prctl 是否需要 CAP_SYS_ADMIN? M3 先收紧, 放松时机与 PR_SET_VMA (无 cap) 的先例对齐评估。**OQ-2** 多 VMA / 非 2M 对齐 arena 的 split 语义 (M4 与事务化 mmap 一起定)。**OQ-3 (已消解, r2)**: P1-1 投机分配后 `folio_throttle_swaprate` 全部在 desc 写锁之前, 不再是设计点。**OQ-4** kernel 态 (get_user) 频繁访问 arena 的 workload 是否需要热路径化 (当前走 shadow-VMA legacy, 正确但慢) — M8 数据再定。**OQ-5** M4 的 mremap 回退: "排空后 legacy mremap" 需要短暂摘 VM_CORTEN (VMA flag 写) — 与 mprotect 放开共用机制, M4 设计点。**OQ-6** 零页读路径与 `mm_forbids_zeropage` (VM_LOCKED 等场景, memory.c:5188) 的 arena 对应策略 — S5 实施核对。

---

## 9. dev agent 切片表 (目标 ~1000-1500 行 diff, 含测试)

建议实施顺序 = 依赖序; 每片独立编译 + 独立测试口径; 遵守 D8 授权惯例 (>300 行需 review 全绿)。

| 切片 | 内容 (文件: 动作) | 规模 | 测试要求 | 依赖 |
|---|---|---|---|---|
| S1 | **骨架+类型**: 新 `include/linux/corten_arena.h` (struct corten_arena/corten_mm_state, prctl 常量, 内联 lookup/门控); `include/linux/mm_types.h`: mm_struct 增 `corten_state` (#ifdef 包裹, 锚点 mm_types.h:967 附近); `include/linux/mm.h`: `VM_CORTEN` bit43 定义; `mm/Kconfig`: `CONFIG_CORTEN_MM_ARENA` (default y when CORTEN_MM, **`depends on X86_64`** [P2-8] — arm64 上 DECLARE 只付成本无收益, =n 即 VM_CORTEN=VM_NONE) | ~200 | make (=n 与 =m 双构建) 零警告; KUnit 骨架空跑 | — |
| S2 | **注册/查找**: 新 `mm/corten_arena.c`: xa 发布、DECLARE/RELEASE/QUERY 核心、`corten_prctl_arena()`; `kernel/sys.c`: `case PR_CORTEN_ARENA` (参照 :2882 模式); `mm/mmap.c`: `exit_mmap` (:1272) 入口 `corten_arena_mm_exit` | ~350 | KUnit: DECLARE 校验链/重叠/QUERY/RELEASE 排空 (用假 mm 或 VM 内 prctl 冒烟); debugfs arenas 文件 | S1 |
| S3 | **shadow-VMA 化**: DECLARE 内 VMA 改造 (mmap_write_lock+vma_start_write, flags 增 VM_CORTEN\|VM_NOHUGEPAGE, anon_vma_prepare, anon_vma_name); `fs/userfaultfd.c` (:1379 校验链) 拒绝 | ~120 | guest 冒烟: DECLARE 后 /proc/maps 校验 (§7.1-③); MADV_HUGEPAGE 被拒 | S2 |
| S4 | **fault 钩子×2 + 慢路径分派**: `arch/x86/mm/fault.c` (:1320/#endif 后) 插入点一; `mm/memory.c` `handle_mm_fault` (:6525 函数头) 插入点二 (含 [P2-6] VMA_LOCK/in_atomic→RETRY 防御); `corten_arena_user_fault` 分派表 (§4.3) 与 perm/SIGSEGV 出口; fallback→legacy | ~250 | **分派表状态机纯函数单测 (KUnit, 无 mm 依赖) + guest 冒烟为准** [P2-9]: 未声明地址 SIGSEGV(Maperr)/权限 SIGSEGV(Accerr) 与 legacy 对照 (si_code 断言) | S2,S3 |
| S5 | **map 实现**: `corten_arena_map_anon` (§4.6, 镜像 do_anonymous_page memory.c:5166-5279, 含 check_stable_address_space [P2-8]) + 零页读 + `fill_upper` (§4.4) + `mm_account_fault`/memcg 复刻 (§4.2, [P2-8] 计数复刻) | ~220 | 压测器单线程 --verify 通过; debugfs 计数闭合 (§7.1-④); perf 脚本首次阳性 (§7.3) | S4 |
| S6 | **空间操作路由**: `mm/mmap.c`: `do_mmap` (:340)/`do_munmap`(:1074)/sys_munmap(:1088) 钩子 (区内→事务, 跨界→⛔; **mmap 路由门 [P1-4]**: MAP_FIXED&&MAP_PRIVATE&&MAP_ANONYMOUS&&!MAP_HUGETLB&&!MAP_GROWSDOWN, MAP_FIXED_NOREPLACE 落区内→-EEXIST); `corten_arena_munmap` (§5.5: zap+flush+put); `corten_arena_mmap_mark`; `mm/mprotect.c`(:863)/`mm/madvise.c`(:2000)/`mm/mremap.c`(:1962) 拒绝钩子 | ~280 | 压测器多线程全参数 (§7.1 判据①-⑤); mmap/munmap 竞争变体 --hammer-race; DECLARE/RELEASE 循环 | S5 |
| S7 | **fork/exit 防线**: `mm/mmap.c` `dup_mmap` (:1739, for_each_vma 循环内) 见 VM_CORTEN → -EOPNOTSUPP fail-fast (M5 升级点); exit 防御性排空复核 | ~40 | guest: arena 进程 fork 返回 -1/-EOPNOTSUPP 且父进程后续 arena 操作正常; 普通 fork 全回归 (kselftests 7.2-5) | S2 |
| S8 | **观测/自检收口**: debugfs arena_stats (percpu 聚合), KUnit 扩展 (S2-S6 各测试), `bin/verify_no_vma_symbols.sh`, 压测器脚本化 + docs (publish/M3B_NOTES.md: 矩阵表复述) | ~180 | §7 全套走查; DoD 逐条验收 (见下) | S6 |

新增文件: `mm/corten_arena.c`, `include/linux/corten_arena.h`, `bench/arena-stress/` (项目侧, 已就位)。改动文件: `kernel/sys.c`, `include/linux/mm_types.h`, `include/linux/mm.h`, `include/uapi/linux/prctl.h`, `mm/memory.c`, `arch/x86/mm/fault.c`, `mm/mmap.c`, `mm/vma.c`(如钩子落在 do_vma_munmap 层), `mm/mprotect.c`, `mm/madvise.c`, `mm/mremap.c`, `fs/userfaultfd.c`, `mm/corten_test.c`, `mm/Kconfig`, `mm/Makefile`。**禁止触碰**: `mm/corten.c` 协议核心 (仅允许注释级更新 real_alloc 桩说明)、`desc->lock` 非 `_bh` 触点。

### M3 DoD 对照自检 (MASTER_PROMPT §4 M3)

| DoD 条款 | 覆盖 |
|---|---|
| boot `corten=on` + prctl(PR_CORTEN_ARENA, addr, len) 声明 | §2.1 (编号 79, 三操作契约) |
| shadow-VMA (粗粒度占位, fork/exit//proc/gup 入口照常) | §3 (flags/合并/显示), §5 矩阵 5.1-5.11 |
| arena 内 PF 走 corten 事务: 锁 covering → query → map, 全程无 VMA 树/mmap_lock 读 | §4.1/4.3/4.5/4.6 (插入点+序列+TLB 论证) |
| 压测器 (多线程 mmap+touch+munmap 循环) 零 panic | §7.1 (判据①-⑤) |
| /proc/pid/maps 正常 | §3.4, §7.1-③ |
| kselftests/mm 冒烟子集通过 | §7.2 (8 项, 双口径 off/on) |
| perf 确认 fault 路径无 find_vma/mmap_lock 符号 | §7.3 (脚本 + 反向阳性 + tracepoint 交叉) |
| 红线: D1/D2(C)/铁律 3 | 每钩子双门 (§2.3, §4.1, §5 红线自检行); =n 全部编译消除 |
| 口径补充 [P2-8] | fork 对照 (M3-M8) 不含 arena 进程 (§7.1 口径声明); min_flt/majf 计数复刻 (§4.2), 非文档化偏差 |

---

## 附: 证据行号总索引 (抽查用)

- x86 fault: fault.c:1207(函数):1279(irq_enable):1316-1320(vsyscall):1322(hook 点):1325(lock_vma_under_rcu):1329(access_error):1334/1385(handle_mm_fault):1354-1357(lock_mmap/lock_mm_and_find_vma):1408-1411(RETRY):1440-1446(SIGBUS/SIGSEGV)
- per-VMA 锁: mmap_lock.c:224-271(lock_vma_under_rcu):232(mas_walk):238(vma_start_read); mmap_lock.c:425-478(lock_mm_and_find_vma)
- memory.c:4364(do_pte_missing):5166(do_anonymous_page):5183(pte_alloc):5187-5190(零页):5191(ptl):5211-5215(vmf_anon_prepare/alloc_anon_folio):1190-1210(folio_prealloc: 分配+memcg charge+throttle):5158(fallback):5229(uptodate):5237(ptl):5259-5263(ref/counter/rmap/lru):5267(set_ptes):5269-5270(免 flush 注释):3614/3628(__vmf_anon_prepare):3663-3755(wp_page_copy, M5 模板):6294-6295(VMA 锁入口注释):6300-6391(__handle_mm_fault: 上层 alloc 6317-6352, PUD THP 门 6326-6327, PMD THP 门 6360-6361, 尾调 handle_pte_fault 6391):6525-6589(handle_mm_fault):6535(sanitize):6539(arch_vma_access_permitted):6552-6560(memcg/THP 分派):6586(mm_account_fault)
- mmap/vma: mmap.c:284-340(do_mmap):1074(do_munmap):1088(sys_munmap):1272-1333(exit_mmap):1739/1766(dup_mmap+写锁); vma.c:87-107(is_mergeable_vma):1564-1646(do_vmi_align_munmap/do_vma_munmap):2713/2789(__mmap_region/mmap_region)
- 其余: mprotect.c:863/1010; mremap.c:1962; madvise.c:2000; gup.c:1100/1139/1367/1577/1601/3157/3230; khugepaged.c:354-365(hugepage_madvise):2425(扫描门):1530/2756(forced collapse); huge_memory.c:132-143(THP 门); huge_mm.h:331-336(vma_thp_disabled); mm.h:279-329(VM 位):331-346(高位):348-363(pkey):375(shadow stack):404-405(VM_MTE):446-450(VM_SEALED 先例):493(VM_SPECIAL):496(VM_NO_KHUGEPAGED); mm_types.h:666(vm_flags_t):967(mm_mt 锚点); arch/x86/Kconfig:1815/1897(HIGH_VMA_FLAGS):1826-1827(PKEY_BITS=4); uapi/prctl.h:385(PR_FUTEX_HASH=78); kernel/sys.c:2882(case 模板):2413(PR_SET_VMA_ANON_NAME); mempolicy.c:566(do_mbind); mm/mseal.c:139(do_mseal); mm/hmm.c:90(hmm_vma_fault); mm/ksm.c:648(break_ksm); task_mmu.c:424-475/501/1157; userfaultfd.c:1240/1379; arch/x86/mm/pgtable.c:28/45(M2a 钩子); fork.c:1516(dup_mmap 调用点); memory.c:5199/5248(check_stable_address_space):6409(mm_account_fault)
