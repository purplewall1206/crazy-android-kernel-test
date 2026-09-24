# M5 切片规格 — fork/COW/GUP + OQ-D（fork 边界）裁决建议（规划者提案, 2026-09-15）

- 评审基线: 主树 /home/ppw/linux-6.18 HEAD=9d74b22a1348（M3 全部 + M4.T0a/T0b,
  12 个项目提交）。**r06 依赖**: 本提案假定 D-G'' 修复（wt m4fix,
  patches/r06-m4dg2.diff）先于 M5.T1 合入——它与 fork_demote/release 改同一区域
  （m4fix 的 [F-B] 多片 shadow-VMA 适配直接改 corten_arena_fork_demote）。
- 行号约定: 全部为主树 HEAD 实测 `file:line`; 上游 6.18 事实同样带行号。
- 规范链: PAPER_SPEC(PS-x) > DESIGN(DEV 表) > 本文 > 实现细节。与 DESIGN 冲突处
  逐条标注"以实现为准"或给出理由（§8 DEV/OQ 登记）。
- 时间盒: 2.5h 规划产出; 供 r06/r07 夜窗执行。

---

## 0. 现象定案: OQ-D 的确切形状（先于裁决, 消除歧义）

DEV-11 fork_demote（mm/corten_arena.c:1488-1596, dup_mmap 挂点 mm/mmap.c:1877）
在 fork 时把每个 arena 排空→scrub 元数据（corten_arena_demote_scrub :1446, 纯
meta 全窗 corten_unmap）→shadow-VMA 还原（corten_arena_unshadow :404, 只清
VM_CORTEN|VM_NOHUGEPAGE, **不动 R/W/X 位**）→注册表擦除。之后 copy_page_range
对"已经 legacy 化"的 VMA 做标准 COW 复制。

由此 fork 边界按 mprotect 路由的三种提交形状分档:

| 提交形状 | demote 后父进程 | demote 后子进程 | 结果 |
|---|---|---|---|
| mprotect 全 arena（whole=true, mprotect.c 路由走 `CORTEN_UNMAP_EXACT`, corten_arena.c:3527 会抬 VMA R/W/X 位 :3419-3444） | VMA flags 已抬, PTE 经 copy_page_range COW 存活 | 同左 | **存活**（侥幸: 靠 VMA 位被路由抬过） |
| mprotect 部分区（whole=false, JVM G1 分块提交的常态） | **VMA 仍 PROT_NONE**（:3441 只在 whole 时改 flags）; 已 fault 的 PTE 内容在, 但访问走 legacy → ACCERR | 同父 + 元数据擦除 | **丢** |
| metadata-only 提交（mprotect 已路由为 meta perm/pending perm, 尚未 fault; 即 PAPER_SPEC Fig.8 的 PrivateAnon(Perm) 状态） | meta 被 scrub 擦回 INVALID; VMA PROT_NONE → 首触 ACCERR | 同左 | **丢** |

登记的现象（STATE r05: "fork 后 arena 提交不存活, 首触 ACCERR"）= 后两行。
根本原因: **DEV-11 把"语义状态"寄存在唯一真源（meta）里, 又在 fork 时把真源
擦掉, 只留下硬件残迹（PTE）和退化后的 VMA 位**——PS-B2 的直接后果, 不是实现
bug。三条候选路线都必须回答"真源如何过 fork"。

---

## 1. OQ-D 裁决建议（第一优先）

### 1.1 三方向对比

| | (i) demote→VMA split 翻译 | (ii) 文档化"重新提交" | (iii) M5.T1 忠实 fork（推荐） |
|---|---|---|---|
| 语义 | 子进程内容存活（COW 复制）; 把 pending/committed 按页翻译成 RW 子 VMA + PROT_NONE 子 VMA | 子进程必须重新 mprotect/fault 才能恢复提交 | **父→子全语义镜像**: meta 深拷贝（state/perm/COW 位）+ arena 注册复制; 子进程与 fork 前父进程逐位等价（PS-C3 真源语义） |
| 边界消失? | 否——每次 fork 都把 arena 拆成 legacy VMA; 父进程性能永久回退 | 否——边界明码标价为文档 | **是**——fork 不再触碰 arena 生命周期 |
| 实现行数 | ~250-350（shadow-VMA split: split/vma_merge 复用 + 逐页 perm 分类 + 与 release/munmap 路由的交互回补） | ~0 | ~850-1000 含测试, 按 §3 切成 2-3 片 |
| 风险 | 中: split 后 VMA 树形态改变, 与 munmap release 规则/mprotect 分类（按 ar->start..end 对 VMA 1:1 的假设）全链冲突; JVM 每次 fork 后 JIT heap 失去加速 | 低（无代码）, 但 **DoD 直接 FAIL** | 中: 冻结窗正确性 + COW 分支（见风险表） |
| D12（零改动兼容, 已编译程序照常跑） | 满足内容, 但 fork 后性能悬崖 + `/proc/maps` 形态突变（一个 arena 变 N 片）→ strace/smaps 等价性可过, 行为等价可过 | **违反**——"已编译程序必须照常运行"被加上了"fork 后自证"的括号 | **完全满足** |
| PS-F6（fork -17.7%/fork+exec +23%）可测性 | fork 数字被 split 开销污染, 无法对照论文口径 | 不可测 | 可测（fork 走页表+meta, 论文同款结构） |
| 与 M5 后续（T2 COW 位/M6 rmap）的兼容 | COW 位无生产者, T2 仍要做 | 同左 | T1 顺手点亮 COW 位（SHARED/WRITABLE 已在 meta: corten.h:126-127, M2 预留） |

### 1.2 裁决建议

**选 (iii) M5.T1 忠实 fork; (i)(ii) 不取。**

理由:
1. **边界消失是唯一满足 D12 的终态**。(i) 把每次 fork 变成一次"arena 拆除施工",
   fork 高频进程（shell、glibc system()/popen、daemonize）反复触发, 加速器退化成
   一次性; (ii) 与 T0 硬门（D12(b): "零改动回归集"）正面冲突, 评审过不了。
2. **成本是论文已知且接受的成本**。PS-F6: 论文 fork -17.7%——因为"须走页表而非
   VMA 枚举"。我们的忠实 fork 走 copy_page_range（PTE 层, 与 legacy 同构）+ meta
   遍历（增量）, 预期回退在 G5 的 ≤30% 内（§6 论证）; 换来 fork+exec 的 fault 快
   优势可测（EVAL G5 允许 fork+exec 净改善）。
3. **OQ-D 现象的三个形状一次性闭合**: 真源（meta）复制, VMA 位不动, ar->prot
   复制——上表三行在子进程全部与父进程 fork 前等价。
4. **T0 时代的两个结构包袱被顺势卸掉**: scrub/INV7-漂移窗（M4T0_SPEC §8 T0-R5）
   不复存在（没有 teardown）; "demote 失败也要继续 teardown"的Timeout 容错分支
   （corten_arena.c:1531-1542）不再需要（失败=中止 fork, 与上游一致）。

### 1.3 M5.T1 忠实 fork 接口签名级设计

**总体形态（对照 DESIGN §6 的修正——见 §8 DEV-14 登记）**: DESIGN §6 写"不走
copy_page_range"。本提案修订为**混合形态**: `copy_page_range()` 继续做它擅长的
PTE 层复制（两侧 wrprotect/rmap/folio 引用/记账, memory.c:1096-1099, :1120-1190）,
新增两段 corten 钩子只做它绕过的那件事——**元数据与注册表**。理由:
- arena 的 shadow-VMA 是完全普通的私有匿名 VMA（声明校验链 corten_arena.c:437-471
  保证）; arena 页是带标准 rmap 的匿名 folio（map_anon:2032
  `folio_add_new_anon_rmap`）。copy_page_range 对它的行为与 legacy 逐位一致
  （`vma_needs_copy` memory.c:1472-1497: shadow-VMA 有 anon_vma（shadowize:373
  anon_vma_prepare）→ 必复制）。
- PTE 层不重写 ⇒ PageAnonExclusive 清除（folio_try_dup_anon_rmap_pte,
  memory.c:1173 → rmap.h:633 ClearPageAnonExclusive）、GUP-pinned 父页给子页拷贝
  （memory.c:1173-1178 copy_present_page）、软脏/uffd 位处理全部白得。
- INV6（arena PTE 写必经事务）的豁免: copy_page_range 的 wrprotect 属 DESIGN §3.2
  预留的"唯一胶水入口白名单"（`corten_glue_pte_write`, 预算 ≤3 处, 目前用 0）——
  本提案用第 1 处（fork wrprotect）。事务覆盖不到 copy_page_range 的 ptl 临界区
  （它持 src PTE 锁, 与 desc->lock 无序）, 强行事务化=重写 copy_present_ptes,
  正是 DESIGN §6 想避免的双真源, 实际上反而制造。
- meta 侧的新增写（SHARED 置位、子侧深拷贝）全部走标准事务（corten_lock_range/
  query/mark）, 真源纪律完好。

**挂点（dup_mmap, mm/mmap.c:1845-2035）**: 单钩子改双钩子。

```
mmap.c:1862 现有 #ifdef CONFIG_CORTEN_MM_ARENA 块:
  - 删: corten_arena_fork_demote(mm, oldmm)
  + 增: retval = corten_arena_fork_begin(oldmm);   /* 冻结: 见 ③ */
        if (retval) goto loop_out;
mmap.c:1986 for_each_vma 循环结束后、arch_dup_mmap(:1988) 之前:
  + 增: retval = corten_arena_fork_commit(mm, oldmm);   /* 元数据+注册: 见 ④⑤ */
        if (retval) goto loop_out;   /* 与 demote 同款: dup_mm 把错误坍缩成 -ENOMEM
                                        （mmap.c:1873-1875 注释先例）, 子 mm 被丢弃 */
```

**① 数据结构增量（include/linux/corten_arena.h）**:
```c
struct corten_arena {
	...
	bool			frozen;	/* fork 冻结窗: lookup_get 拒新事务 */
};
```
frozen 写方只持 oldmm mmap_write + ctl_lock 的 fork 路径; 读方
corten_arena_lookup_get（corten_arena.c:1616-1628）加一拍:
```c
	ar = corten_arena_lookup(mm, addr);
	if (ar && READ_ONCE(ar->frozen))
		ar = NULL;                    /* 冻结窗: 走 legacy→被 mmap_write 挡住 */
	if (ar && !percpu_ref_tryget_live(&ar->active))
		ar = NULL;
```
选 frozen 标志而非 percpu_ref kill/reinit（percpu-refcount.h:132 有
percpu_ref_reinit）: 少一次 RCU 回调风暴, drain 语义完全复用现有
corten_arena_drain（:269-282）; kill 版作为 OPEN QUESTION-2 备选不取。

**② fork_begin(oldmm) — 冻结窗建立**（mm/corten_arena.c 新增, ~90 行）:
- `oldmm->corten_state == NULL`（smp_load_acquire, :1505 先例）→ 0 返回, 一次
  load, 无 arena fork 零开销（与 demote :1505-1507 同）。
- 持 oldmm mmap_write（dup_mmap 已持, mmap_assert_write_locked）→ ctl_lock
  （DEV-13 锁序 mmap_write > ctl_lock, D13）。
- 逐 arena（xa_for_each, :1518 先例; `frame < drained_until` 去重 :1527-1529 同款）:
  `WRITE_ONCE(ar->frozen, true)` → `corten_arena_drain(ar)`（在途事务清零;
  超时沿 demote :1531-1542 先例: 计数 + 照常继续, 但**不中止 fork**——见风险
  R-A 的论证: 冻结不完整只造成子侧 meta 陈旧, 由 §3.2 的 meta 快照+校验兜底）。
- 语义: 冻结窗内父进程 arena 段的所有新 fault: fast 钩子（arch/x86/mm/fault.c:1342）
  lookup 失败 → FALLBACK → legacy 路径 → 需 mmap_lock/per-VMA lock → 被 dup_mmap
  写锁挡住**阻塞**（不是丢）; 慢门（mm/memory.c:6552-6571）同理。GUP-fast 无锁读
  PTE: 与 copy_page_range 并发是上游原生容忍形态（gup.c:2896-2900 重校验）。
  ⇒ 冻结窗内**无人能写 arena PTE/meta**, copy_page_range 看到的是静止快照。
- 子进程 `mm->corten_mode = oldmm->corten_mode`（:1500 语义保留, 移到这里）;
  子 state 惰性创建（corten_arena_get_state, :1180-1190; 子 mmap_write 也被
  dup_mmap 持有——mmap.c:1860 nested, 合法）; `child_state->next_va =
  old_state->next_va`（窗口游标复制, 防子进程新 mmap 撞上继承的 arena 段）。

**③ copy_page_range 照常跑**（mmap.c:1980）: shadow-VMA 原样（VM_CORTEN|VM_NOHUGEPAGE
经 vm_area_dup 复制）, PTE 两侧 wrprotect（memory.c:1096-1099）, 子页/引用/rmap/
PageAnonExclusive 全按 legacy。VM_DONTCOPY 的 VMA 被循环跳过（mmap.c:1900-1908）——
对应 arena 见 §2 边缘情形 E3。

**④ fork_commit(mm, oldmm) — 元数据镜像**（~200 行, 仍是 dup_mmap 写锁窗内, 子进程
不可调度, 零并发; mm_flags 未设 MMF_UNSTABLE）:

对每个 arena（xa_for_each old_state）:
1. **子侧跳过条件**: 子 VMA 树里找不到 `[ar->start, ar->end)` 的 VM_CORTEN VMA
   （`vma_lookup(mm, ar->start)` 为 NULL 或无 VM_CORTEN——VM_DONTCOPY/WIPEONFORK
   形状）→ 该 arena 只留在父侧, 置 SHARED 位都不需要（唯一潜在 mapper 没了;
   遗留 SHARED 位由 §3.2 免拷贝分支自愈）, 计数 `fork_skips`。
2. **父侧 SHARED 置位 + 快照**: 逐 2M 窗: `corten_arena_pmd(mm, addr)`（:1752）
   presence 门——**pmd_none 的窗直接跳过**（无 PT 页 ⇒ 无 meta ⇒ 无内容, INV4 同生灭;
   这是把 fork 成本从"预留 GB 数"压到"已触窗数"的关键, 见 §6 成本模型）;
   非空窗: `corten_lock_range(oldmm, win)`（covering 写锁）→ 逐页 `corten_query`
   → 512 项快照入栈/堆缓冲（4K, kmalloc; 禁 512×8B 栈数组进 16K 内核栈的谨慎取
   kmalloc）→ 对 `state == CORTEN_MAPPED` 的页: `corten_mark` 置
   `flags |= CORTEN_PF_SHARED`（perm 不变; WRITABLE 规则由 corten_mark 内建:
   corten.h:409-410——可写页 shared 必带 WRITABLE, 编译期拒非法组合）→
   `corten_unlock`。CORTEN_PRIVATE_ANON 页不动（无内容可共享, 子侧纯复制 perm）。
3. **子 arena 注册**: 按 declare_locked（:551-656）的构造顺序造子 arena 对象
   （kzalloc GFP_KERNEL_ACCOUNT / mutex_init(fill_lock) / init_completion /
   percpu_ref_init :560-570）, `start/end/mm(=child)/prot(=父 ar->prot)` 拷贝,
   `vma = vma_lookup(mm, ar->start)`（子 maple 树已建完 :1956; fork 冷路径一次
   maple walk 可接受——热路径零 maple 的 M3 DoD 不受影响）; ctl_lock(child) 下
   xa_store 全 frame（:618-625 先例）+ `refcount_set(&state->nr, +1)`（:633 set
   语义先例）+ obs_add（:640, debugfs 台账）。**不 shadowize**（VMA 已带
   VM_CORTEN, 由 vm_area_dup 继承; anon_vma 已由 anon_vma_fork mmap.c:1943 接上）。
   **OPEN QUESTION-1（m4fix 联动）**: D-G'' punch 后一个 arena 可对应多片
   shadow-VMA, `ar->vma` 缓存的语义以 m4fix 落地的形状为准（其 [F-B] 已给
   fork_demote 做过多片适配, patches/r06-m4dg2.diff @@-1566 段）; T1 实施前以
   m4fix 的辅助函数（分片查找）复用到子侧 vma 缓存。
4. **子侧 meta 深拷贝**: 逐窗（同样 pmd presence 门——子 PT 页由 copy_page_range
   按 `pte_alloc` 镜像父侧分配, 形状与父一致）: `corten_lock_range(child_mm, win)`
   → 按快照逐页 `corten_mark`（state/perm 原样 + SHARED 位对 MAPPED 页置上;
   INVALID 页跳过）→ `corten_unlock`。**父/子两棵 desc 树的锁从不嵌套**
   （④-2 先完成父窗全部事务再进子窗, 无跨树锁序问题; 子进程无并发事务, 排队
   深度为零）。
5. **解冻**: 逐 arena `WRITE_ONCE(ar->frozen, false)`。顺序: 先子后父——解冻父
   后父事务立刻可能在快照外的页上 map（新 fault）, 不影响已发布状态; 解冻必须在
   ctl_lock 释放前完成对每 arena 的一次写。
6. 全程计数: `corten_nr_fork_faithful`（新, debugfs arena_stats）, 每成功的
   fork_commit 至少 +1（有 arena 时）; `fork_demotes`（corten_arena.c:140/:929）
   保留为历史计数不再增长。

**⑤ 失败回滚**:
- fork_begin 失败（仅 -ENOMEM）: 返回错误 → dup_mmap `goto loop_out` → fork 中止
  （dup_mm 坍缩 -ENOMEM, mmap.c:1873-1875 先例注释）; **父侧无任何变更**
  （frozen 置位失败的 arena 不足一个就返回, 已置位的逐个解冻——unwind 循环 ~15 行）。
- fork_commit 中途失败（子 arena percpu_ref_init/xa_store/meta mark -ENOMEM）:
  已注册的子 arena **不清扫**——直接返回错误; 子 mm 走 MMF_UNSTABLE（mmap.c:2015）
  → dup_mm 丢弃 → exit_mmap（mmap.c:1371）→ `corten_arena_mm_exit`（mmap.c:1385
  已有）无锁排空全部子 arena, 释放干净。父侧: 部分页已带 SHARED 位、且 frozen
  未解冻 → **必须先解冻所有已冻结 arena 再返回错误**（否则父进程永久 legacy 化,
  R-A 的最坏形态）; SHARED 位残留**无需回滚**: 子进程不存在,
  `folio_mapcount()==1` → 首次写 fault 走 §3.2 免拷贝分支清位自愈。
  （前提: T1 必须与 §3.2 的 COW 分派同窗落地——见 §3.4 时序裁决。）

**⑥ 与 T0 fork_demote 代码的关系: 替换, 不保留 fallback。**
- 删: `corten_arena_demote_scrub`（:1446-1486）、`corten_arena_fork_demote`
  （:1488-1596）、mmap.c:1862-1880 块内的调用与注释、KUnit
  `corten_arena_test_fork_demote`（corten_arena_test.c:1583, 改写为忠实 fork 用例）。
- 留: `corten_arena_drain`（:269, fork_begin 复用）、`corten_arena_unshadow`
  （:404, RELEASE 路径仍用）、`fork_demotes` 计数（历史可观测）。
- 不做"demote 兜底": 失败即中止 fork 与上游语义一致（fork 失败是应用可见但合法）,
  双路径并存 = 两倍测试面 + INV7 双份漂移风险, 收益为零。
- bench/arena-stress/fork_arena_test.c（现断言 fork 必失败, :85-100）**已过时**
  （T0 起就该失败）, T1 重写为 §2 的 fork_arena_test v2。

---

## 2. M5.T1 完整切片规格

### 2.1 语义判据（DoD）
1. **父子隔离校验和一致**: 父 fork 前对 arena 区间（含: 已 fault 页、mprotect
   whole/部分提交、pure PROT_NONE 预留三种形状混合, ≥2 个 arena）算 FNV 校验和;
   fork 后父、子各算一遍 → 三和相等（子首触覆盖每页后, PROT_NONE 形状除外——
   其"提交"是 perm 而非内容, 判据 = 子写 fault 成功且父不可见——写后父侧重读
   校验和不变 = 隔离成立）。
2. **1k 页往返**: 64M arena 触 1k 页（magic 填充）→ fork → 父子各改写各自一半,
   交叉校验 1000 轮（fork→touch→exit→重触→再 fork）, 无 ACCERR/无内容互串。
3. **多线程程序 fork 不死锁**: 8 线程 arena_stress 形态（持续 fault）+ 主线程
   fork()/waitpid 循环 1k 次: lockdep 构建零 splat（drain 等待有界, 冻结窗
   mmap_write 阻塞的 fault 在 fork 返回后放行）; `fork_demotes` 不再增长,
   `fork_faithful` == fork 次数。
4. **JVM 形状回归**（依赖 m4fix 合入）: LD_PRELOAD MODE 下 JVM spawn 线程 + 中途
   `system("true")`（fork+exec）→ 子进程退出码 0; 父 JVM 继续分配无 ACCERR
   （D-G'' 修复依赖: 若 m4fix 未合入, 本项顺延并登记）。
5. **回归**: CONFIG_CORTEN_MM=n 构建零变化; corten=off 冒烟; arena_stress 全参数
   （MODE-targeted 回归不破——declare/release/fault 面零改动）; kselftests/mm
   8 件套; **S7 fail-fast 语义正式废止**（M3B_DESIGN §5.1 的 -EOPNOTSUPP 形状
   不再存在于任何路径, 审计表记录）。

### 2.2 切片与文件清单（ROADMAP M5.T1 = L, 拆 T1a/T1b 两夜）

| 片 | 内容 | 预计 diff | 依赖 |
|---|---|---|---|
| **M5.T1a** | frozen 位 + lookup_get 一拍 + fork_begin/fork_commit（快照/marks/注册/meta 拷贝/解冻/回滚）+ mmap.c 双钩子 + dispatch 的 SHARED 写 fault 临时收敛（见 §3.4）+ KUnit（fork 状态机注入用例: 快照→父改→断言子 meta 不变; 回滚解冻覆盖） | ~+520/-180, 5 文件 | m4fix 合入 |
| **M5.T1b** | 测试件: fork_arena_test v2（判据 1/2）、fork+多线程压测、debugfs 计数收口、kselftests 回归、INV7 checker 扩展（SHARED⇒PTE 可 RO, 见 §8） | ~+330/-60, 4 文件 | T1a |

- 改: `include/linux/corten_arena.h`（+~35: frozen/声明/锁序注释）、
  `mm/corten_arena.c`（~+400/-190: 删 demote 系, 增 begin/commit）、
  `mm/mmap.c`（+~18: 双钩子）、`mm/corten_arena_test.c`（+~230/-45）、
  `mm/corten_fault_test.c`（+~60: dispatch SHARED 用例）、
  `bench/arena-stress/fork_arena_test.c`（重写 ~+130/-50, 项目侧不计入内核 diff）。
- 不动: `mm/corten.c`（协议核心零改动——fork 全部用既有事务 API）、
  `kernel/fork.c`、`mm/memory.c`、`arch/x86/mm/fault.c`、`mm/mprotect.c`。
- **T1a 超单夜 300 行预算**（D8 惯例: 需 review 全绿 + 不得第二片同夜）。

### 2.3 边缘情形（实施走查清单）
- **E1** 空注册（无 arena）fork: 一次 load 返回, 零开销（=现状 :1505-1507）。
- **E2** `clone(CLONE_VM)`: 不经 dup_mmap, 零影响（M4T0_SPEC §5.4 先例）。
- **E3** MADV_DONTFORK 打到 shadow-VMA: madvise 路由表无此 behavior
  （M4T0_SPEC §3.4 决策表未列）→ 落 legacy 置 VM_DONTCOPY → fork 循环跳 VMA
  → §1.3 ④-1 的子侧跳过分支接管。**OPEN QUESTION-3**: madvise_route 是否应把
  DONTFORK 显式 -EOPNOTSUPP（拒绝比半支持诚实）; T1 先按"legacy 放行 + 子跳过"
  实现（内容安全: SHARED 位不置, 免拷贝自愈）。
- **E4** fork 瞬间正有 RELEASE: RELEASE 需 mmap_write → 被 dup_mmap 挡, 冻结窗
  内不可达; fork_begin 看到的注册表即静止。
- **E5** drain 超时（内核 bug 形态）: demote 时代"带漏继续 teardown"改为
  "带漏继续 fork"——冻结不完整时并发事务可能写父 PTE, copy_page_range 的
  ptl 临界区保证页级原子, 子侧可能拿到撕裂语义（新页未在快照/已在快照但 PTE
  旧）——快照是在 drain 后取的, 真正暴露=事务引用泄漏 bug, 与 demote 同级,
  计数 + WARN, 不中止 fork（可运行进程优于卡死 fork 的既有裁决先例 :1532-1539）。
- **E6** `fork()` 嵌套在 GUP-slow（io_uring 线程持 mmap_read 做 faultin）:
  dup_mmap 取写锁被挡 → fork 串行化在后, 无锁环（GUP-slow 不持任何 desc 锁等
  mmap 锁之外的东西; INV2 无反向边）。

### 2.4 与 PAPER_SPEC 的偏离登记（进 DESIGN DEV 表）
- **DEV-14**: DESIGN §6 "不走 copy_page_range"修订为混合形态（§1.3 理由:
  PTE 层与 legacy 同构白得 GUP/rmap/exclusive 语义; meta 层事务化保真源纪律;
  copy_page_range 的 wrprotect 记为 `corten_glue_pte_write` 白名单第 1 处,
  INV6 口径不变）。论文语义（PS-C3/PS-F6 的 fork 结构）不受影响——论文页表
  枚举≈我们的 copy_page_range+meta 遍历。
- **DEV-15**: fork 冻结窗（frozen 位）为移植自造语义, 论文无对应物（论文事务
  与 fork 同锁协议自然互斥; 我们 fault 不取 mmap_lock, 必须显式静止）。
  REPORT 披露。
- **DEV-11 废止**: M4T0_SPEC §9 DEV-11（fork=全退场）由本切片取代,
  STATE D13 登记的 "M5 兑现后废止" 条款兑现。

---

## 3. M5.T2 — COW 写 fault 事务（状态机点亮）

### 3.1 状态机扩展
现状: dispatch 对 `CORTEN_MAPPED && !perm_ok(write) && SHARED` 返回
CORTEN_DISP_STUB（corten_arena.c:1713-1715, 注释"fork COW: M5"）; 对
`perm_ok(write)` 直接 CORTEN_DISP_RESTORE（:1718）——**SHARED 位一旦有生产者
（T1）, RESTORE 直通就是错误**（父写绕过 COW → 子见父写, §1.3 已述）。T2 改:

```c
case CORTEN_MAPPED:
	if (!corten_arena_perm_ok(m, write, instruction))
		return (m->flags & CORTEN_PF_SHARED) ? CORTEN_DISP_COW_COPY
						     : CORTEN_DISP_ACCERR;
	if (write && (m->flags & CORTEN_PF_SHARED))
		return CORTEN_DISP_COW_MAYBE;      /* 新: 免拷贝 or 拷贝 */
	return CORTEN_DISP_RESTORE;
```
两个新 disp 共用一个 handler `corten_arena_cow_write()`（~120 行）:

### 3.2 写错处理事务（两分支, PS-C3 忠实）
进入条件: 持 covering desc 写锁（fault_once 既有流程 :2207）+ meta:
`MAPPED, perm 含 WRITE, flags 含 SHARED`。

- **免拷贝分支（map_count==1, 论文原语）**: `folio = pte_page(PTE)`;
  `folio_mapcount(folio) == 1`（我们的 map_count 对应物; 在 desc 写锁+ptl 下读,
  另一侧最后 PTE 的移除方也持其 ptl, 竞争方向只可能"多拷贝"不可能"漏拷贝",
  §5 R-B 论证）→ 同事务 `corten_mark`: `flags &= ~SHARED`（state 自身到自身,
  corten.h:406-407 合法迁移）→ PTE: `ptep_set_access_flags(mkwrite+mkdirty)`
  （复用 restore_pte :2127-2180 的骨架, 加 meta 位清除）。
- **拷贝分支（map_count>1）**: 用 fault 路径预分配 folio（user_fault :2384-2398
  写 fault 必预分配）: `copy_user_highpage(new, old)` → 断链重建（map_anon
  :1987-2044 骨架 + wp_page_copy 语义）: `ptep_clear_flush`（旧 RO 翻译可能缓存,
  map_anon:2017 先例）→ `set_ptes(new, writable)` →
  `folio_add_new_anon_rmap(RMAP_EXCLUSIVE)` + MM_ANPAGES 对冲 → 旧 folio put →
  同事务 `corten_map(txn, addr, newpage, perm, CORTEN_MAP_FORCE)`
  （MAPPED→MAPPED 需 FORCE, corten.h:380-383——正是为 COW copy-in 预留的口）。
- **TLB/flush 一致性（与 mprotect 路由先例对齐）**: 单页 fault 用
  `ptep_clear_flush`/`ptep_set_access_flags` 的自带 flush; 批量窗场景不存在
  （fault 单页粒度）, 无需 protect_range 的范围 flush 累积器。
- **SHARED⇒硬件形状不变量**: fork 后两侧 PTE 均 RO（copy_page_range :1096-1099）
  = mprotect 降级后"perm W 但 PTE RO"的既有形状（protect_window :3288-3298 +
  RESTORE 自愈 :2159-2172）, INV7 checker 的既有豁免面覆盖, 仅需加
  "SHARED 允许 PTE RO"一条（§8）。

### 3.3 免拷贝分支与 mprotect 路由先例的一致性走查
| 关注点 | mprotect 路由先例（protect_window :3221-3313） | T2 COW | 一致? |
|---|---|---|---|
| meta 先于/同于 PTE 改 | meta 先（:3246-3267） | 同事务内 meta↔PTE 相邻, desc 写锁隔离观察者 | ✓（更严: 单事务） |
| PTE 写模式 | ptep_modify_prot_start/commit（:3288-3298） | COW 换页用 clear_flush+set（map_anon 先例 :2017/:2038）; 保页用 set_access_flags（restore 先例 :2172） | ✓（两先例各取其一） |
| mmu_notifier | protect_range :3339-3341 发 INVALIDATE_START | 单页 fault 无 notifier（legacy do_wp_page 同） | ✓ |
| zero page 不升写 | :3274-3286 special 拒写 | 拷贝分支天然换真页 | ✓ |
| -EAGAIN 重试 | :3529-3537 C1 | fault_once 既有 retry 帽（:2401-2417, 2 次） | ✓ |

### 3.4 时序裁决（重要）: T2 的分派核心必须随 T1a 落地
T1 让 SHARED 位有生产者的那一刻, `RESTORE 直通` 即成为父子互串的正确性 bug;
而 T1 的回滚论证（§1.3 ⑤）又依赖免拷贝分支存在。因此:
- **T1a 携带 §3.1 分派改动 + §3.2 免拷贝分支 + 拷贝分支的最小实现**（~+150 行,
  已计入 T1a diff）; `CORTEN_DISP_COW_*` 的 KUnit 纯函数用例同窗。
- **M5.T2（ROADMAP 原片）收窄为**: FOLL_FORCE/FOLL_UNSHARE 语义
  （ptrace POKE 走拷贝分支的专项测试; FAULT_FLAG_UNSHARE 映射, 见 §4-OPEN4）、
  PageAnonExclusive 重置决策（OPEN QUESTION-5）、与 GUP/mprotect 交叉的组合
  测试、Fig.8 L26-38 逐行语义对拍表、免拷贝分支的 mapcount 竞争压力用例。
  规模 ~+260, 单夜。

---

## 4. M5.T3 — GUP 互操作

### 4.1 fast 路径验证设计（命题: arena PTE 是普通 PTE, gup_fast 天然工作）
逐条核对 `gup_fast_pte_range`（mm/gup.c:2857-2932, HAS_PTE_SPECIAL 版）对 arena
PTE 的行为假设:
| gup_fast 检查 | 行 | arena 形状下的行为 |
|---|---|---|
| `pte_protnone` | gup.c:2879 | mprotect PROT_NONE 降级产生的 PTE → bail 落 slow（slow 会给 ACCERR/EFAULT——正确语义） |
| `pte_access_permitted(pte, FOLL_WRITE)` | gup.c:2882 | fork 后 RO PTE + FOLL_WRITE → bail 落 slow → faultin → 事务 COW → 重试通过。**arena 无需感知** |
| `pte_special` | gup.c:2885 | 共享零页 bail（不可 fast-pin）→ slow follow_page_pte 对 zero pfn 放行读（gup.c:852-853） |
| 重校验 `pmd/pte` | gup.c:2896-2900 | 与事务并发: zap/protect 的 TLB 批量语义下与 legacy zap 同形态; 事务窗内 PTE 写持 ptl, gup_fast 无锁读+重校验=上游原生竞争面 |
| `gup_must_unshare` | gup.c:2907 | PageAnonExclusive+RO+PIN|WRITE: fork 已清 exclusive（§1.3）, T2 免拷贝分支重写可写 PTE → 命中率≈0; 保留上游行为 |
| THP/PMD leaf | gup.c:2952+ | arena 恒 VM_NOHUGEPAGE（shadowize :381）→ 无 PMD leaf, 恒 PTE 级 |
结论: **fast 路径零改动**。T3 交付 = 验证而非实现: (a) KUnit 无法覆盖 lockless
路径 → guest 实测: io_uring/`process_vm_readv`/vmsplice 对 arena 页（fork 前后、
COW 前后、mprotect 降级后四态）的 GUP-fast 命中率与正确性（读到的字节 vs 校验和）;
(b) perf 符号确认 `internal_get_user_pages_fast` 路径无 WARN。

### 4.2 slow 路径 shadow-VMA 交互（现状收口确认）
- 唯一漏斗: `faultin_page`（gup.c:1100）→ `handle_mm_fault` → 慢门
  （memory.c:6552-6571: VM_CORTEN 且非 VMA-lock/atomic →
  `corten_arena_handle_mm_fault` :2452）→ 事务分派。GUP 无 FORCE 写
  RO PTE: `can_follow_write_pte`（gup.c:798-813）失败 → faultin(FAULT_FLAG_WRITE)
  → arena COW（T2 拷贝分支）→ 重试 pin 到新页。**与 legacy 行为逐位对齐**
  （legacy 同样 faultin→wp_page_copy）。
- FOLL_FORCE（ptrace）: `can_follow_write_common`（gup.c:598-620）要求
  `PageAnonExclusive` —— fork 后父页非 exclusive → **必走 faultin** → 事务 COW
  拷贝 → ptrace 写的是子拷贝语义与 legacy 相同（FOLL_FORCE 打穿的是旧 COW,
  写进本进程独占页）。现状的"FOLL_FORCE COW stub 返回 VM_FAULT_SIGSEGV"
  （memory.c:6568 注释 + dispatch STUB）被 T2 撤销——审计清单 #12
  （M4T0_SPEC §4）正式关账。
- **OPEN QUESTION-4**: `FAULT_FLAG_UNSHARE`（faultin_page gup.c:1130-1134）进
  慢门时 ctx.write=false, 会按读 fault 分派（RESTORE）——对 SHARED 页语义应映射
  为 T2 拷贝分支。GUP unshare 触发频率极低（pin+fork 组合）, T3 决策: 慢门把
  FAULT_FLAG_UNSHARE 翻译成 ctx.write=true 走 COW 分派（+KUnit 锚）。

### 4.3 FOLL_PIN 引脚保活 × arena unmap 路由（M3 桩的兑现）
- **保活 = 引用计数原生正确**: `try_grab_folio_fast`（fast, gup.c:2892）/slow
  取 pin 引用; arena zap（corten_arena_zap_window :2619-2716）`tlb_remove_page`
  → folio_put 只放 PTE 引用; pin 引用使 folio 存活至 `unpin`（INV8 达成, 无新
  机制）。标准语义: unmap 后 VA 读 0 页（meta INVALID, FRESH 门按 ar->prot 走
  分配）, pinned folio 的 DMA/写继续打到旧物理页——与 legacy munmap 完全同构,
  论文语义一致（unmap 清翻译, 页生命周期归引用计数）。
- **T3 新增的观测/护栏**（~40 行）: zap_window 里
  `folio_maybe_dma_pinned(folio)` 命中时 debugfs 计数 `zap_pinned`——不为拒绝
  （语义合法）, 为 M6 铺路: 迁移/回收（M6.T2）必须跳过 pinned 页, 该计数即
  频度证据。
- **互斥的唯一真实风险点**: mremap grow 路由的 kernel copy
  （corten_arena.c:3579-3600 无锁 copy_to_user）对 pinned 源页同样成立（读旧
  内容, 与 legacy mremap 对 pinned 页的契约一致, :3569-3571 注释已声明同契约）。
- 测试: process_vm_readv 父↔子 arena 页; /proc/pid/mem poke（FOLL_FORCE 链）;
  io_uring 注册 buffer + munmap 竞争压测（guest 有 liburing 则跑, 无则
  OPEN 登记, 不阻塞）。

---

## 5. M5.T4 — lat_proc fork/fork+exec/shell + JVM（MODE 下）

- 矩阵: `lat_proc fork / fork+exec / shell` × {corten=off, on+MODE} × ≥3,
  ABAB 交错（EVAL §2）; JVM = JThreadBench spawn→init 窗口 + 一次
  `system()` fork+exec 行为判据（T1 判据 4 复用）。
- 对照: PS-F6 论文口径 fork -17.7%/fork+exec +23.0%/shell 持平;
  **EVAL G5: fork 回退 ≤30%, fork+exec 允许净改善**; 回退 >35% 触发 ROADMAP R5/
  OQ7（fallback 评估, 见 §5 风险 R-C）。
- 成本模型（用于预判与 M8 报告口径）: T1 fork 增量 = per-arena [frozen+drain
  （µs 级）+ 2M 窗遍历（pmd 门跳空窗）+ 快照/mark（∝已触窗数×512 query）+ 子
  arena 对象数] 。lat_proc 子进程 arena 少而小（glibc/mm arena 1-3 个）→ 增量
  主导项是 drain 常数; JVM/dedup 型大堆 fork: 成本 ∝ 已触窗数, 与
  copy_page_range 的同区成本同阶（同一下潜深度）, 预期在 G5 带内。**若密集
  触窗（>50% 窗非空）场景超标, 优化顺序**: ①快照窗与 copy_page_range 的窗循环
  合并遍历（一次下潜双用）②SHARED mark 改批量 meta 写（mark 本就支持 range,
  但页粒度 perm 不同需逐页——仅对"全窗同状态"窗批量化）。
- shell: fork+exec 高频, 论文预期持平; 我们的 fork 略贵 + exec 后 fault 快
  （fault 路径 M3 已实证零 VMA 树）→ 方向待测。

---

## 6. 风险表（M5 特有; 通用见 ROADMAP §4）

| # | 风险 | 概率 | 影响 | 触发信号 | 缓解 |
|---|---|---|---|---|---|
| R-A | frozen 位错误路径未解冻 → arena 永久冻结, 进程静默退化为 legacy | 低-中 | 性能悬崖无报错 | debugfs `fork_freeze_leaks` 计数（commit/begin 每出口对账） | 解冻收口在单一 unwind 函数; KUnit 注入 fork_commit 中途失败; 判据: 每次fork后 frozen==false 断言（调试构建） |
| R-B | 免拷贝分支 mapcount 竞争（读==1 时另一侧正在解除映射中） | 低 | 漏拷贝=互串（不可接受）或多拷贝（安全） | 压测: fork×并行写×exit 循环 + 校验和 | ptl 下读 mapcount; 论证"解除方先清 PTE 后减 count"⇒观测值≥真值⇒只会多拷贝; INV7 checker + 父子校验和测试兜底 |
| R-C | fork 回退 >G5 30%（密集 arena 场景 meta 遍历放大） | 中 | G5 FAIL | M5.T4 数字 | §5 优化①②; 仍超 → OQ7 fallback 评审（copy_page_range-only + fork 后首触时按需重建 meta 的"懒镜像"—— DESIGN OQ7 原意, 需再设计, 本提案不预授权） |
| R-D | m4fix 联动: 多片 shadow-VMA 下 ar->vma 缓存/子侧注册形状漂移 | 中 | T1a 返工 | m4fix review | T1a 排在 m4fix 合入后; 子侧 vma 缓存复用 m4fix 的分片查找辅助（OPEN QUESTION-1） |
| R-E | T1 冻结窗内 legacy fallback fault 阻塞放大 fork 尾延迟（8 线程持续 fault 时, 被 mmap_write 挡住的 fault 在 fork 全程排队） | 中 | lat_proc/JVM fork 尾延迟可测劣化 | fork 压测 p99 | 量级=dup_mmap 本身时长（legacy fork 同样挡 fault, 非新增量级）; 数字说话, 进 T4 报告 |
| R-F | JVM CDS（D-G''）未闭合拖累 T4 JVM 判据 | 确定（若 m4fix 未验） | T4 JVM 项顺延 | dg_probe2 | T4 JVM 行为判据依赖 D-G'' 验证（java F→P）先行; 性能数字可先出 |
| R-G | fork 期间父进程被 OOM-kill（fatal_signal_pending mmap.c:1914）: 冻结已置、commit 未跑 | 低 | 同 R-A 的泄漏形态 + MMF_UNSTABLE 路径 | 压测 fork×memory pressure | begin/commit 每轮检查 fatal_signal_pending 提前收口解冻（对齐 mmap.c:1914-1917 的现有退出点） |

**风险前三**: R-A（正确性+可观测性, 设计已闭环, 落 KUnit 锚）、R-B（语义红线:
父子互串, 论证+压测双保险）、R-C（G5 性能门, 有 OQ7 后手）。

---

## 7. 每 T 验证判据汇总 × EVAL 对照

| T | 判据（全过才收口） | EVAL 门 |
|---|---|---|
| T1a | §2.1 判据 1-5 + KUnit（fork 注入/回滚/dispatch SHARED 纯函数） | G6 稳定性回归面 |
| T1b | fork_arena_test v2 全绿（含 1k 往返/多线程 lockdep 构建）; INV7 checker SHARED 条目零漂移; debugfs fork_faithful 对账 | G6 |
| T2 | Fig.8 L26-38 对拍表逐行（map_count==1 免拷贝计数>0、拷贝分支计数>0、ptrace POKE 语义、UFFD 不涉及）; process_vm_readv/proc mem 全绿; UNSHARE 决策落地 | G6 + G1 间接（fault 路径无回归） |
| T3 | §4.1 四态 GUP 实测矩阵全绿 + zap_pinned 计数上线 + FOLL_FORCE stub 撤销后的 kselftests 差集为零 | G6 |
| T4 | lat_proc 三项 + JVM 出数（3 中位）; fork 回退 ≤30% 或触发 OQ7 评审记录; fork+exec 方向性记录 | **G5** |

**G5 预期声明（诚实性, G8）**: 8 vCPU 无法复现论文 384 核口径; lat_proc 单值
对 ±2% boot 漂移敏感（EVAL §2.3）, 判定用 ABAB 中位; 回退落在 17.7%~30% 带内
即算同向复刻。

---

## 8. DEV/OQ 登记（提交 STATE 决策编号用）

- **DEV-14**（DESIGN §6 修订）: fork = copy_page_range（PTE 层, 胶水白名单第 1 处）
  + 事务 meta 镜像; "不走 copy_page_range" 作废, 理由 §1.3。
- **DEV-15**: fork 冻结窗（frozen 位）为移植自造语义, REPORT 披露。
- **DEV-11 废止**: 由 M5.T1 取代; `fork_demotes` 计数留作历史遥测。
- **INV2 补注**: 冻结窗内无新锁序（frozen 在 mmap_write>ctl_lock 之下读写）;
  父/子 desc 树锁不嵌套（§1.3 ④-4 单侧事务纪律）。
- **INV7 扩展**: SHARED 位 ⇒ PTE 允许 RO（perm W 时）; checker 加此豁免。
- **OQ-1**（阻塞 T1a 排期）: m4fix 多片 shadow-VMA 形状 → 子侧 arena->vma 缓存
  语义对齐（§1.3 ④-3）。
- **OQ-2**（关闭备选）: percpu_ref kill/reinit 冻结方案不取, frozen 位替代。
- **OQ-3**: MADV_DONTFORK/MADV_WIPEONFORK 对 arena 的路由口径
  （现按"legacy 放行+子跳过"实现; 若收紧为 -EOPNOTSUPP 需补 strace 等价性证据）。
- **OQ-4**: FAULT_FLAG_UNSHARE → COW 分派映射（T3 决策点, §4.2）。
- **OQ-5**: 免拷贝分支是否 `SetPageAnonExclusive`（影响后续 GUP
  gup_must_unshare 命中与 fork pin-copy 形状; 倾向不设置=与 wp_page_reuse 的
  保守形态一致, T2 review 定）。
- **OQ-6**: arena 段是否要在 `/proc/pid/smaps` 上暴露 SHARED 计数（RSS/共享
  口径与 legacy fork 后一致——copy_page_range 已计入共享, 无需动作; 记录即可）。

---

## 9. 时间盒内的产出边界声明

本文为规划者提案（2.5h）: OQ-D 裁决建议（§1）+ 切片表（§2.2/§3.4）+ 风险前三
（§6）。所有"6.18 事实"断言带 file:line 并经主树 HEAD=9d74b22a1348 实读核对;
不确定处均已标 OPEN QUESTION（1-6）。实施前需: ①m4fix 合入（OQ-1/R-D）;
②STATE 决策编号回填 DEV-14/15; ③ROADMAP M5 行按 §2.2/§3.4 的 T1a/T1b/T2'
重切。
