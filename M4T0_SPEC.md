# M4.T0 切片规格 — MODE-process 透明接管（设计评审产出, 2026-09-13）

- 评审基线: 主树 /home/ppw/linux-6.18 HEAD=d040b61051af（M3b.S1-S3 已合入）+
  worktree /home/ppw/linux-6.18-m3b46（S4-S7 未提交增量, 4811 行）。
  **T0 的实施前提 = r03 先把 S4-S7 修复后 commit**（遗留 fill_upper pud Oops）。
- 行号约定: `主:`= 主树 HEAD 实测; `wt:`= m3b46 工作树实测（S4-S7 合入后即主树行号）。
- 规范链: PAPER_SPEC > DESIGN(DEV 表) > 本规格 > M3B_DESIGN（冲突处以实现为准, 见 §9 OQ/DEV）。
- 本文是给 dev agent 的执行文档; 机制语义见 docs/DESIGN.md §2/§5, 论文条款见 docs/PAPER_SPEC.md。

---

## 0. 与已实现 M3b 面的关系（一句话定调）

**T0 = 在 S6 "空间路由"之上加一层 "入口供给+模式门", 不是替代。**
S6 的路由/拒绝钩子按 *xarray 区间* 工作（"这个 range 是否在某个已声明 arena 内"）,
对谁声明的、怎么声明的无感知。T0 做三件事:
1. **供给**: MODE-process 下 do_mmap(addr=0, 匿名私有) 自动创建 arena（cursor 发 VA +
   建 VMA + 复用 `corten_arena_declare()` 做 shadow-VMA 化与 xarray 注册）;
2. **升级**: 把 S6 对 mprotect 的 ⛔ 拒绝升级为 🔁 最小事务路由, madvise 补 MADV_FREE/纯 hint 档;
3. **解雷**: fork 从 fail-fast 改为 "arena 全退场"（§5）, 使多线程程序在 MODE 下能 fork。
S6 的门（`corten_arena_mmap_classify`/`munmap_vma_guard`/`range_overlaps`）全部保留为内层路由与后备防线。

---

## 1. 接口签名

### 1.1 prctl（include/uapi/linux/prctl.h, 紧随 :396 `PR_CORTEN_ARENA 79` 之后）

```c
#define PR_CORTEN_MODE          80      /* 数字命名空间顺延: 78=futex_hash, 79=corten_arena */
# define CORTEN_MODE_ENTER      1       /* arg3..arg5 恒须 0, 违者 -EINVAL */
# define CORTEN_MODE_EXIT       2       /* 存活 arena>0 时 -EBUSY（先 RELEASE/exit） */
# define CORTEN_MODE_GET        3       /* 返回 0/1 */
```

- kernel/sys.c: `case PR_CORTEN_MODE:`（wt:2886 `case PR_CORTEN_ARENA` 旁）→
  `corten_prctl_mode(arg2, arg3, arg4)`; CONFIG_CORTEN_MM=n 时 = 空桩 -EOPNOTSUPP（铁律 3, 同 79 先例）。
- 权限/门控: ENTER 需要 `capable(CAP_SYS_ADMIN)` + `corten_enabled_static()`
  （对齐 M3b OQ-1 收紧姿态）; EXIT/GET 同需 corten=on（GET 在 =n 时 -EOPNOTSUPP）。
- fork 继承（DESIGN §2 "fork 继承" 的落地）: 见 §5.3。

### 1.2 per-mm 模式字段（include/linux/mm_types.h, wt:981 `corten_state` 同一 #ifdef 块内）

```c
#ifdef CONFIG_CORTEN_MM
	struct corten_mm_state *corten_state;   /* 已有 */
	bool                    corten_mode;    /* 新增: MODE-process 开关 */
#endif
```

- 放 mm_struct 而非 corten_state 内: MODE 门必须在 state==NULL（尚无任何 arena）时即可命中,
  且热路径检查 = 1 字节 load。写方仅 prctl ENTER/EXIT 与 fork（持 mmap_write_lock）, 读方
  READ_ONCE。CONFIG_CORTEN_MM=n 时布局零变化。
- `kernel/fork.c:1058`（mm_init 内 `mm->corten_state = NULL;` 旁）加 `mm->corten_mode = false;`
  （新 mm 必须 false, 不依赖 memcpy 零值）; 继承在 dup_mmap 显式拷贝（§5.3）。

### 1.3 MODE 窗口与 per-mm cursor（mm/corten_arena.c, 新增至 corten_mm_state/新 per-mm 字段）

- 窗口: `[0x1000_0000_0000, 0x4000_0000_0000)`（DESIGN §2 原文; x86_64 TASK_SIZE=128T 之内,
  与 legacy mmap_base/brk/vdso 无重叠）。cursor `corten_next_va` 存 corten_mm_state（惰性分配,
  ENTER 时建）; T0 不做 VA 复用（T1 per-cpu 杂志做）, 耗尽 → 回退 legacy + 计数（§8 R3）。
- `struct corten_mm_state` 增: `unsigned long next_va;`（仅 ctl_lock/mmap_write 下访问）。

---

## 2. 前置重构 P0: 锁序统一为 `mmap_write → ctl_lock`（必做, 否则 T0 死锁）

**现状（wt 实测, mm/corten_arena.c:336/344）**: `corten_arena_declare()` 先
`mutex_lock(&state->ctl_lock)` 再 `mmap_write_lock(mm)`（头文件注释锁序也是 ctl_lock→mmap_lock）。
T0 的 auto-attach 在 do_mmap 内执行时**已持 mmap_write**（vm_mmap_pgoff, mm/util.c:570-584 持锁
调 do_mmap; do_mmap:352 有 assert）, 此时取 ctl_lock 即形成反向边, 与并发 DECLARE 成环。

**改法**（改动 declare/release/mm_exit/fork-退场 四处获取顺序, 不改对外 API）:
1. `corten_arena_declare()`: 改为 `mmap_write_lock(mm)` → `ctl_lock` → 校验/shadowize/xa_store → 逆序放。
   （VMA 校验本就要写锁, 语义不变。）
2. `corten_arena_release()`: `mmap_write_lock` → `ctl_lock` → xa_erase → `percpu_ref_kill_and_confirm`
   + `wait_for_completion(&drained)`（**drain 等待改在双锁下**: fault 路径不取 mmap_lock/ctl_lock,
   排空仍无环; drain 期间 mmap_write 持有只造成 legacy 操作短暂阻塞, 量级=在途事务 µs 级）
   → 原地做 legacy teardown（省一次重复取锁）→ 逆序放。
3. `corten_arena_mm_exit()`（wt:1330 调用点）: 维持"无锁排空"（mm_users==0 无并发 fault）,
   仅取 ctl_lock——"单独取下级锁"不构成环（无人持 ctl_lock 再等 mmap_write）。
4. 文档: DESIGN §7 INV2 锁序注记增补 `mmap_write > ctl_lock > drain-wait > desc->lock > ptl`;
   include/linux/corten_arena.h 头注释同步。**此项须在 STATE 决策编号登记（INV2 修订）。**

---

## 3. 四入口精确插入点与路由规则

> 插入点行号: T0 基于 "主 HEAD + S4-S7" 实施, 故给 wt 行号（S4-S7 commit 后即主树行号）;
> 括号内给主树对应锚点。所有钩子保持既有双门惯用法:
> `if (!corten_enabled_static() || !mm->corten_mode) return legacy;`（MODE 门）
> 与既有 `!READ_ONCE(mm->corten_state)` 门并存。

### 3.1 mmap — `do_mmap()` mm/mmap.c（wt:421-447 现有 MAP_FIXED 路由门在 :424-447; 主锚 :394 `__get_unmapped_area` 之前）

**新增 MODE 变换门（放在 wt:424 现有路由门之前, 即主 :394 前——必须在 `round_hint_to_min`/len
对齐之后, 才能看到最终 len）**:

```c
/* MODE-process: addr==0 匿名私有白名单映射 → 自动 arena 化 */
bool auto_arena = false;
if (static_branch_unlikely(&corten_enabled_key) && !file &&
    mm->corten_mode && addr == 0) {
    int cret = corten_arena_auto_mmap_route(mm, len, prot, flags,
                                            &addr, &len, &flags);
    if (cret < 0) return cret;        /* 仅窗口耗尽/内部错; 其余不产生错误 */
    auto_arena = (cret == 1);         /* 命中: addr/len/flags 已被改写 */
}
```

`corten_arena_auto_mmap_route(mm, len, prot, flags, &addr, &len2, &flags2)`（mm/corten_arena.c 新增）:
- **白名单分类** `corten_arena_auto_mmap_classify(flags, file)`:
  - 允许位: `MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE`，其余位中
    `MAP_FIXED/MAP_FIXED_NOREPLACE/MAP_SHARED(类型位)/MAP_HUGETLB/MAP_GROWSDOWN/MAP_POPULATE/
    MAP_LOCKED/MAP_SYNC/MAP_DROPPABLE/MAP_UNINITIALIZED` 任一出现 → legacy。
    **MAP_STACK 与 MAP_DONTUNMAP 类提示位: T0 排除**（线程栈留 legacy, 砍掉最热的
    mprotect-guard 交叉面; T1+ 评估放行）。
  - `sysctl_overcommit_memory == OVERCOMMIT_NEVER` 时 MAP_NORESERVE 不被 honors（mmap.c:563-567）,
    VMA 会带 VM_ACCOUNT → DECLARE 校验链必拒 → 此配置下直接 legacy（加注释+计数）。
  - len 对齐后为 0 → legacy（让正常路径报 -ENOMEM/-EINVAL）。
- 命中时（在 `mmap_write_lock` 已持有的前提下, do_mmap 全程持锁）:
  1. `len2 = round_up(len, PMD_SIZE)`; 在 `[WIN_START, WIN_END)` 内取
     `addr2 = round_up(state->next_va, PMD_SIZE)`, `next_va = addr2 + len2`;
     `next_va > WIN_END` → 计数 fallback + 返回 0（legacy, 优雅降级, 不报错）。
  2. 改写 `*flags2 |= MAP_FIXED | MAP_NORESERVE`, `*addr = addr2`, `*len = len2`,
     返回 1。后续正常流程: `__get_unmapped_area(MAP_FIXED)` 校验窗口段空闲 → `mmap_region()`
     建 VMA（NORESERVE ⇒ 无 commit 记账, 与 DECLARE 白名单一致; 记账偏差见 §9 DEV-12）。
- **attach 钩子**: do_mmap 尾部 `addr = mmap_region(...)`（wt:580, 主 :580）成功且 `auto_arena` 时,
  调 `corten_arena_attach(mm, addr, len2, prot)` = `corten_arena_declare()` 的 locked 内联版
  （P0 重构后直接复用: 已持 mmap_write, 取 ctl_lock, 走 validate/shadowize/xa_store）。
  - 残窗竞争说明: mmap_region 返回到 attach 之间 VMA 尚无 VM_CORTEN, 若 khugepaged 恰在此微窗
    collapse（需扫描周期命中, 实际不可达窗口）, 后续 fault 走既有 -EOPNOTSUPP fallback → legacy,
    正确性无损（优雅降级, 计数可见）。T1 纯 mark 路径消除此窗。
- PROT_NONE / 任意 prot 组合均允许（`corten_arena_prot_from_vma` 已处理）; pkey 不进窗口
  （pkey_mprotect 的显式地址路径天然 legacy）。

### 3.2 munmap — `sys_munmap()` mm/mmap.c（wt:1120-1146 路由已在; 主 :1089）

**T0 唯一改动 = "release-on-full-coverage" 规则**, 在 `corten_arena_munmap_route()`
（mm/corten_arena.c:1745）的 `corten_arena_unmap_classify()` 之后:

- `class == CORTEN_UNMAP_CHUNK && start == ar->start && (ar->end - end) < PMD_SIZE` →
  **按 EXACT 处理**: 用户 munmap 的 len 是它 mmap 时的请求长（页对齐）, 与内核 2M 圆整的
  arena 尾差 ≤2M-4K——glibc `free()` 对 mmap 块正是这种形态。走 RELEASE 撤销整个 arena
  （尾差区间本就是内核圆整私有区, 对应用不可见）。**没有这条规则, dedup_eq 型 churn
  每轮泄一个 VMA + 2M VA, 必须有。**
- 其余分类维持 S6 现状: CHUNK→事务 zap（内容清零 VA 保留, 论文 Fig.8 L9-13 语义）;
  PARTIAL→-EOPNOTSUPP; EXACT→RELEASE。
- `__vm_munmap()`（wt:3262-3284）与 `do_vmi_align_munmap` guard（wt:1618）不变（后备防线）。

### 3.3 mprotect/pkey_mprotect — `do_mprotect_pkey()` mm/mprotect.c（wt:895-911 拒绝钩子在, 主 :863 函数）

**把 wt:904 的 `-EOPNOTSUPP` 拒绝升级为路由**（JThreadBench 硬依赖: openjdk CodeCache/G1 heap
= mmap(NULL, PROT_NONE) + mprotect 提交, 不路由则 JVM 直接起不来）:

```c
if (corten_enabled_static() && (mm->corten_mode || mm->corten_state)) {
    int cret = corten_arena_mprotect_route(current->mm, start, len, newprot, pkey);
    if (cret < 0) { error = cret; goto out; }
    if (cret == 1) { error = 0; goto out; }
    /* 0 = 与窗口无交集 → legacy（含 PROT_GROWSDOWN 等, 维持原校验链） */
}
```

`corten_arena_mprotect_route()`（新增, T0 最小正确版; T3 再优化）:
- `[start,end)` 与 arena 关系复用 `corten_arena_range_overlaps`/classify:
  **完全落单 arena 内** → 逐 2M 窗: `corten_lock_range` → 逐页 `corten_query`:
  - meta 为 `CORTEN_PRIVATE_ANON`: 改 meta.perm（= `corten_mark` 语义, 复用 `corten_mark`）;
  - meta 为 `CORTEN_MAPPED` 且 PTE present: 更新 meta.perm + ptl 内改写 PTE 权限位
    （`ptep_modify_prot_start/commit` 模式）→ 窗口结束统一 `flush_tlb_mm_range`;
  - `CORTEN_INVALID`: 不动（未 mark 的空间, 权限挂到后续 mark; 记 meta 层"pending perm"于
    arena->prot 上界不覆盖时报 -EACCES）。
  → `corten_unlock` → 返回 1。pkey≠-1 或 newprot 含语义外组合 → -EOPNOTSUPP。
- **部分跨界**: 维持 -EOPNOTSUPP（与 S6 一致; 窗口隔离使正常 allocator 打不到边界）。
- mprotect 钩子从"重叠即拒"变为"路由优先、跨界才拒"——`madvise_vma_behavior` 式 per-VMA
  拒绝（wt:1391）不受影响。

### 3.4 madvise — `do_madvise()` mm/madvise.c（wt:1957 路由已在; 主 :2000 函数）

`corten_arena_madvise_route()`（mm/corten_arena.c:2103）扩展决策表:

| behavior | T0 行为 |
|---|---|
| MADV_DONTNEED / DONTNEED_LOCKED | 维持既有 `corten_arena_dontneed_route`（区内 zap 留 VA） |
| MADV_FREE / MADV_FREE_LOCKED | **新增 → 同 DONTNEED 事务**（MADV_FREE 本就允许"内容变 0", 清零是合规超集; 注释+计数披露） |
| MADV_NORMAL / SEQUENTIAL / RANDOM / COLD | **新增 → 区内返 0 no-op**（纯 hint; 防止 metis_eq 对文件映射外的 hint 打到 arena 报错） |
| MADV_WILLNEED / PAGEOUT / FREE / HUGEPAGE / NOHUGEPAGE / COLLAPSE / WIPEONFORK 等 | 维持 -EOPNOTSUPP |
| MADV_SOFT_OFFLINE / HWPOISON | 维持 wt:1936 拒绝 |

brk（`sys_brk` mm/mmap.c:122）: **零改动**（DEV-6; shrink 经 wt:1618 guard 后备, 窗口隔离
使其不可达 arena）。

### 3.5 门控矩阵（MODE × ARENA × 入口 — 审计清单的判定表）

| 入口 | MODE=0（默认, 含 MODE-targeted 现状） | MODE=1, 范围不在任何 arena | MODE=1, 范围在 arena 内 |
|---|---|---|---|
| mmap addr=0 匿名私有白名单 | legacy VMA | **自动 arena**（cursor+declare） | 同左（各 arena 独立, 不合并） |
| mmap addr=0 带外位（SHARED/FIXED/HUGETLB/POPULATE/LOCKED/STACK…） | legacy | legacy | legacy（不可能命中窗口: 显式 addr 永远 legacy） |
| mmap 显式 addr（ELF/栈/vdso/allocator 预留） | legacy | legacy | 落点在窗口内=不可能; 窗口外 legacy |
| mmap MAP_FIXED 落入某 arena（MODE-targeted 老用法） | S6 路由门: mark 事务 | 同左 | 同左（两模式正交叠加） |
| munmap 区内 chunk | S6: zap 留 VA | （无 arena 不可达） | 同左 |
| munmap "start 对齐+尾差<2M" | S6 现状=CHUNK | — | **T0: 升级为 RELEASE** |
| munmap 跨界/部分 | -EOPNOTSUPP | — | 同左 |
| mprotect 区内 | M3b=-EOPNOTSUPP | — | **T0: perm 事务路由** |
| madvise DONTNEED/FREE 区内 | DONTNEED 路由 / FREE 拒 | — | **T0: FREE 并入路由**; hint no-op |
| brk（grow/shrink） | legacy | legacy | legacy（guard 后备） |
| mremap 触 arena | -EOPNOTSUPP（DESIGN OQ2） | legacy | 同左 + 计数（§9 OQ-A） |
| mlock/mlockall/mseal/mbind/uffd 触 arena | -EOPNOTSUPP/拒绝 | legacy | 同 S6（不变） |
| fork（有存活 arena） | S7 fail-fast | legacy | **T0: arena 全退场（§5）** |
| move_pages / migrate_pages 触 arena | **无钩子（审计缺口）** | legacy | **T0 新增: 入口 -EOPNOTSUPP** |
| fault @ 窗口 | xarray 未命中→legacy | 自动 arena 命中→事务 | 同左（S4 钩子零改动） |

---

## 4. 入口审计清单（DESIGN §5 的完备化, T0 交付物之一）

DESIGN §5 清单（do_mmap / vm_munmap / do_mprotect_pkey / do_madvise / mremap / brk）在 S4-S7
实现中已超额覆盖, T0 补齐缺口后**全量收口表**（每行须有实现点或"不可达"论证）:

| # | 入口/漏斗 | 收口点 | 状态 |
|---|---|---|---|
| 1 | 所有用户 mmap（含 ksys_old_mmap） | `vm_mmap_pgoff`→`do_mmap` 唯一漏斗, mm/util.c:570 | 已收口+T0 MODE 门 |
| 2 | munmap | sys_munmap(wt:1140)/`__vm_munmap`(wt:3273)/`do_vmi_align_munmap`(wt:1618) 三层 | 已收口+T0 release 规则 |
| 3 | mprotect/pkey_mprotect | do_mprotect_pkey 唯一漏斗 | **T0 路由** |
| 4 | madvise/process_madvise | do_madvise(wt:1957); **审计动作: 验证 process_madvise(madvise.c:2111) 流经同一 decision 点**（remote-mm 传参路径走查+KUnit/测试留痕） | T0 核对项 |
| 5 | mremap | mremap.c wt:2002/2008 | 维持拒绝+计数 |
| 6 | brk | guard 后备 wt:1618 | 已收口（不可达论证） |
| 7 | mlock/mlock2/mlockall | mlock.c wt:644/785 | 已收口 |
| 8 | mseal / userfaultfd / mbind / PR_SET_VMA | mseal.c wt:177 / uffd wt:1334 / mempolicy wt:1457 / madvise 名字钩子 wt:1391 | 已收口 |
| 9 | **move_pages / migrate_pages（mm/migrate.c:2377 do_pages_move 及 migrate 系统调用）** | **S4-S7 未覆盖——rmap 迁移会无事务改写 arena PTE, 违反 R2** | **T0 新增入口重叠拒绝** |
| 10 | hwpoison / SOFT_OFFLINE | memory-failure wt:1592 / madvise wt:1936 | 已收口（WARN+拒） |
| 11 | khugepaged/THP/KSM | VM_NOHUGEPAGE 单开关（M3B_DESIGN §4.7） | 已收口（自动 arena 同享） |
| 12 | 内核态访问 user 内存（get_user/GUP/ptrace） | fault 慢门 wt:memory.c:6537 + fallback | 已收口（FOLL_FORCE=M5 桩, 文档化） |

---

## 5. fork / exit 在 MODE-process 下的行为（T0 必须解, M5 前的过渡策略）

### 5.1 为什么 S7 的 fail-fast 必须换掉
wt:1830-1848 对含 VM_CORTEN VMA 的 dup_mmap 返回 -EOPNOTSUPP。MODE-process 的真实应用
（glibc `system()/popen()`、daemonize、benchmark 驱动脚本）一 fork 即废——DoD 的
"多线程程序 fork 行为" 判定直接 FAIL。

### 5.2 T0 策略: **fork = arena 全退场（双方转 legacy, MODE 位保留）**（记 DEV-11, M5 兑现论文忠实版）
不能只处理子进程: dup_mmap 持 oldmm 写锁 copy_page_range, 而 arena 事务不取 mmap_lock——
不排空则复制与事务竞写 PTE。也不能简单"排空后照抄": 排空窗内落到 legacy body 的新 fault
会造出"有 PTE 无 metadata"（INV7 漂移, 重 DECLARE 后是错数据）。因此:

1. dup_mmap 开头（wt:1762 `mmap_write_lock_nested` 之后、VMA 循环前）插入
   `corten_arena_fork_demote(oldmm, mm)`:
   - `mm->corten_mode = oldmm->corten_mode;`（MODE 位继承——旧映射已 legacy 化, 后续新
     mmap(NULL) 仍自动 arena 化, "fork 继承"语义保住）;
   - `oldmm->corten_state` 为 NULL → 直接返回（一次 load, 无 arena fork 零开销）;
   - 取 `ctl_lock`（此时已持 oldmm mmap_write, P0 锁序合法）→ 逐 arena:
     `percpu_ref_kill_and_confirm` + `wait_for_completion` 排空（在途事务清零, PTE 冻结）;
     逐 2M 窗 `corten_lock_range` → **metadata 全窗 `corten_unmap`（纯 meta 失效, 不动 PTE）
     → `corten_unlock`**（清掉排空期 legacy fault 造成的任何漂移; 退场后该区间永不再被事务读,
     重 DECLARE 由本步保证干净）→ `xa_erase` 全 frame → `percpu_ref_exit` + `kfree_rcu`;
   - 仍持写锁+`vma_start_write` 下把各 shadow-VMA 的 `VM_CORTEN|VM_NOHUGEPAGE` 清除
     （还原为普通 MAP_NORESERVE 匿名 VMA, 防 madvise/mprotect 钩子与 khugepaged 语义残留）;
   - 释放 ctl_lock; 删除 wt:1830-1848 的 fail-fast（循环里已无 VM_CORTEN）。
2. 子进程: `corten_state=NULL`（mm_init 已置）, 继承 `corten_mode`——**子进程旧映射=纯 legacy
   匿名内存（内容经 COW 复制, 值一致）, 新 mmap 重新进 arena**。
3. 父进程: 同上退场; fork 后性能回退到 legacy（旧映射）——**文档化 + debugfs 计数
   `fork_demotions`**, M5 用论文忠实遍历替换（wrprotect+shared+meta 深拷贝, DESIGN §6）。
4. clone(CLONE_VM)（pthread_create/vfork/posix_spawn）: 不经 dup_mmap, 零改动零影响。
5. exit: `corten_arena_mm_exit`（wt:1330）已收口, 无改动。

**规模**: ~150-180 行（含 meta-scrub 辅助与 KUnit）。

---

## 6. KUnit / 测试件

- `mm/corten_arena_test.c` 追加（纯函数层, 无 mm 依赖, 沿用 M2/M3b 测试模式）:
  `corten_arena_auto_mmap_classify()`（白名单逐位真值表 ≥16 例）; release-on-full-coverage
  分类（含 len>2M 多窗、尾差=0、尾差=2M-4K 边界）; mprotect 决策表; madvise 决策表新行;
  fork 退场状态机（注入假 arena/meta 验证 scrub 后 query=INVALID）。
- guest 冒烟（qemu-exec 代理）: `bench/arena-stress`（MODE-targeted 回归不破）+
  新 `bench/apps/` 复用 metis_eq/JThreadBench（零改动, 只设 `prctl(PR_CORTEN_MODE,ENTER)` 的
  **包装器**（`setarch` 式 LD_PRELOAD 或 runner 进程 fork 后 ENTER 再 exec——ENTER 后 exec 无效,
  因 exec 换 mm; 故 runner 形态: wrapper ENTER→fork→child exec target? **不行**: fork 走 §5 退场
  且 exec 重建 mm。**正确形态 = 目标程序首行自调 prctl**, 或 LD_PRELOAD constructor 在
  libc 初始化时 ENTER——constructor 在 target mm 上运行, 合法。DoD 判定用 LD_PRELOAD
  `corten_mode_hook.so`（6 行, bench 侧, 不算改应用源码）。）

---

## 7. DoD（验收判定, 全部满足才收口）

1. **metis_eq 零改动**: LD_PRELOAD 下 MODE=on 跑通, 输出 checksum 与 corten=off 同机同输入一致;
   debugfs: `auto_mmaps>0`, `munmap_releases>0`, `faults_mapped` 闭合, `fallbacks==0`
   （或逐条解释）。多线程 8 vCPU 无 panic/WARN/lockdep 报警。
2. **JThreadBench 零改动**: openjdk-21 完整跑通 1..8(×超订 16) 线程; debugfs
   `mprotect_routes>0`（JVM PROT_NONE reserve→mprotect 提交被路由的证据）; spawn→init 窗口
   出数（性能非 gate, 行为正确是 gate）。
3. **fork 行为**: MODE 进程（8 持续 fault 线程）中 `fork()+waitpid` 正常返回; 父子进程旧映射
   读写值一致（COW 语义）; 子进程 `mmap(NULL)` 后仍 arena 化（debugfs `auto_mmaps` 增）;
   `system("true")`/`popen` 可用; fork 前后无 panic/死锁（lockdep 构建）。
4. **strace 等价性（R11 缓解落地）**: metis_eq 在 MODE on/off 的 syscall 结果 diff——除
   mmap 返回地址（窗口 vs legacy 区）外, 无新增 -EOPNOTSUPP/-EINVAL/-ENOMEM。
5. **回归**: CONFIG_CORTEN_MM=n 构建 + corten=off 冒烟零变化（铁律 3）; kselftests/mm 8 件套
   （M3B_DESIGN §7.2）双口径全绿; arena_stress 全参数回归。
6. **审计清单收口评审**: §4 表逐行 PASS/豁免留痕（review agent 核对项）。

---

## 8. 风险表（T0 特有; 通用风险见 ROADMAP §4）

| # | 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|---|
| T0-R1 | mremap-on-arena=-EOPNOTSUPP 打断 glibc `mmrealloc`（大块 realloc 走 mremap, 失败即向应用返 ENOMEM） | 中 | 应用可见行为差异, 违反"零改动行为正确" | T0 计数+strace 等价性测试显式暴露; OQ-A 提请规划者: 增补 "mremap区内=新auto-arena+内核逐页copy+释放旧arena" 路由（~120 行, T0.5/T1 窗口）; 短期 DoD 应用侧确认 metis_eq/JThreadBench 不触发 |
| T0-R2 | MODE churn（dedup_eq 型）VA/VMA 增长: release 规则未命中的 munmap 永留 VA; cursor 不回收 | 中 | 窗口耗尽→优雅降级为 legacy（正确但"接管"失效, 论文叙事受损） | release-on-full-coverage 规则（§3.2）+ 耗尽回退 legacy+计数; T1 per-cpu 杂志做真回收; debugfs `auto_mmap_fallbacks` 监控 |
| T0-R3 | 透明接管放大 glibc 热路径回退: mmap/munmap/mprotect 每次多 1-2 load + churn 全量 teardown/rebuild（2M 粒度 vs legacy 可合并复用） | 中 | M4.T5 中期数据 apps 不升反降 | 门成本=static branch+1 byte（corten=off 零付）; churn 成本如实记录为 T0 口径（T1 纯 mark/T2 事务 unmap 收益正对此）; tcmalloc 档对照（PS-F8）定位 |
| T0-R4 | 锁序重构（P0）引入回归: release 改为持 mmap_write 等 drain, drain 期间 legacy 操作被阻塞 | 低 | mmap_write 持有时间变长 | drain µs 级（在途事务上限）; arena_stress --hammer-race + fork 竞争用例; lockdep 构建 |
| T0-R5 | fork 退场窗内 legacy fault 造成 meta 漂移未被 scrub（理论: scrub 只在退场时做一次） | 低 | 重 DECLARE 该区间时 query 陈旧 MAPPED | scrub 是退场路径必经且在排空后持 desc 写锁执行; KUnit 注入用例; INV7 checker（调试构建）兜底 |
| T0-R6 | JVM/allocator 未预期 syscall（madvise WILLNEED、mlock、userfaultfd fallback）打中 arena 被拒 | 低 | 应用启动失败 | 白名单从紧+hints no-op 档; strace 预跑一遍 openjdk 启动核对（一次性审计动作, 记入 DoD-4） |

---

## 9. OQ / DEV 登记（提交 STATE 决策编号用）

- **DEV-11**（fork 过渡）: T0 fork=arena 全退场, 偏离 PS-C3/PS-F6 的 arena-fork 语义; M5 兑现忠实版后废止。
- **DEV-12**（自动 arena 形态）: DESIGN §2 "路由进一个大 arena" 落地为 **per-mmap N 个 auto-arena**
  （窗口内 cursor 发放）; 强制 MAP_NORESERVE ⇒ OVERCOMMIT_HEURISTIC 下不参与 commit 记账
  （与 M3b DECLARE 白名单一致）; OVERCOMMIT_NEVER 配置下 MODE 自动接管整体降级 legacy。
- **DEV-13**（INV2 修订）: 锁序增补 `mmap_write > ctl_lock > drain-wait > desc->lock > ptl`;
  declare/release 获取顺序反转（M3B_DESIGN §6.1 与实现注释以本规格为准）。
- **OQ-A** mremap 区内路由（kernel-copy 版）是否进 T0.5（影响 glibc realloc 语义, §8 T0-R1）。
- **OQ-B** MAP_STACK 是否放行白名单（放行则线程栈进 arena, 必须同步保证 guard-mprotect 路由已就绪——T0b 之后才可考虑）。
- **OQ-C** process_madvise 对 remote arena-mm 的行为口径（核对项 #4; 如走独立路径需补钩子）。
- **OQ-D** PR_CORTEN_MODE 放松 CAP_SYS_ADMIN 的时机（与 M3b OQ-1 合并决策）。
- **以实现为准**记录: ①DESIGN §5 "arena 空间操作取 mmap_read_lock+事务"（DEV-7）在 S6 实现
  为 "munmap_route 无锁 / mmap 路由在 write lock 下"——T0 沿用实现, DEV-7 的读锁降级留给
  T1/T2 重评; ②S6 的 `profile_munmap` 不见 routed munmap（[C3d] 注释, wt:1128）——M4 一并处理。

## 10. 切片分解 / diff 规模 / 文件清单

| 片 | 内容 | 预计行数（含测试） | 依赖 |
|---|---|---|---|
| **T0a** | P0 锁序重构 + prctl 80/corten_mode + auto-mmap 路由（cursor/白名单/attach）+ munmap release 规则 + fork 退场 | ~550 | S4-S7 commit |
| **T0b** | mprotect 路由 + madvise FREE/hints + move_pages/migrate_pages 拒绝钩子 + debugfs 计数（auto_mmaps/releases/mprotect_routes/fallbacks/fork_demotions）+ 审计清单文档 + LD_PRELOAD runner + DoD 测试 | ~420 | T0a |

> 两片均超 300 行——按 D8 惯例需 review 全绿（M2 先例 1254/1862 行）; 一夜最多一片（ROADMAP §5）。

文件清单:
- 改: `include/uapi/linux/prctl.h`（+8）、`include/linux/mm_types.h`（+4）、`include/linux/corten_arena.h`
  （声明+注释 ~15）、`kernel/sys.c`（+6）、`kernel/fork.c`（+3）、`mm/mmap.c`（+45）、
  `mm/mprotect.c`（±20, 拒绝→路由）、`mm/madvise.c`（±10）、`mm/migrate.c`（+15）、
  `mm/corten_arena.c`（~+450/-60）、`mm/corten_arena_test.c`（+250）、`mm/corten_fault_test.c`（+40）
- 新: `bench/mode-hook/corten_mode_hook.c`（LD_PRELOAD, 项目侧）
- 不动: `mm/corten.c` 协议核心、`arch/x86/mm/fault.c`、`mm/memory.c`（fault 双钩子零改动——
  fault 路径由 xarray 命中自然覆盖 auto-arena）、`mm/vma.c`（guard 保留）
