# V-A.3 开发包：路由扫尾 + J1/J2 审计钩（放置面守卫热修级最高优先）

创建: 2026-09-22（V-A.3 切片准备班, 只读审计产出）
基座: 主树 `/home/ppw/linux-6.18` HEAD = **d40eae59ba76**（V-A.1）。worktree
`/home/ppw/linux-6.18-mva` 有 A.2a/A.2b 未提交增量（~+1249/−500, 7 文件, 夜验中）——
**本文以"A.2 落地后的预期形态"为基座描述**；涉及 mm/corten_arena.c 与 mm/mmap.c 的
挂点全部给双行号（主树 HEAD / worktree A.2 后），worktree 只读参考。
规范链: MV_VMA_FREE_SPEC.md §3.1.3（V-A.3 全文）+ §1.3（J1–J4 终判据）+ §1.2（域
划分）+ §4.1（铁律）+ §5 切片表（V-A.3 ~+400/−150(+150 测试), A 系出口 = J1/J2 首次
全绿）。主要输入: `next/j2-audit-draft.md`（55 调用点分类 + 最危险 3 条 + 挂点清单）。

**现状一句话（决定 V-A.3 的真实剩余量）**: A.2 增量已经**提前吸收了原 V-A.3 的约
1/3**——hint 放置守卫（P2, `corten_addr_in_window` + `corten_fence_unmapped_area`,
sys_x86_64.c/generic 四 walker）、J1 五挂点中的三个（find_vma_intersection /
find_vma / lock_vma_under_rcu 的 `corten_j1_probe`）、corten 自身豁免的别名通道
（`corten_vma_find`, worktree mm/mmap.c:1011）都已写完。**但 A.2 的 NOREPLACE 守卫
调的是 `corten_arena_range_overlaps`——该函数跳过 idle/parked 帧与 reserve 哨兵
（worktree mm/corten_arena.c:8849-8850）**，所以审计 #14 的"parked 窗装入"洞**在
A.2 之后仍然敞开**；同一根因还打穿 plain-MAP_FIXED-over-parked（punch 路由的
`lookup_get` 同样跳 idle）与 `__mmap_prepare` backstop 的 `if (vms->vma)` 条件洞
（mm/vma.c:2472, worktree 未改）。V-A.3 = 放置面热修（含-idle 探测 + backstop 补臂）
+ J1/J2 收口 + S-5 三条终答 + C5/C6/C7 路由扫尾。

---

## 1. 目标与优先级

| 优先级 | 内容 | 来源 | 性质 |
|---|---|---|---|
| **P1（热修级）** | 放置面四守卫: NOREPLACE 含-idle 探测、plain MAP_FIXED over parked 的 idle-eject 臂、`__mmap_prepare` backstop 零 VMA 分支、reactivate/pool_take 防御断言 | 审计 #14/#15/#16/#17（N-high, 唯一 corruption 级族） | 用户态一次 syscall 可触发 dominion 不变量破坏 + NOREPLACE 契约违约 + 与 reactivate/punch 冲突（帧表仍指向 parked 描述符） |
| **P2** | J1 五挂点收口（find_vma_prev / find_vma_and_prepare_anon 两个未落 + corten 内部剩余调用面豁免扫尾）+ fault 慢/快路径窗口短路（#1/#2）+ uffd mfill/move 短路（#29） | spec §1.3 J1；审计 J1 卫生五条 | J1 计数>0 即 FAIL 的负向探针在带窗 workload 下永远红 |
| **P3** | J2 INV-MV2 walker（植入登记 + 树走查断言）+ 四观测计数器 | spec §1.3 J2；审计挂点清单 | A 系出口判据 |
| **P4** | S-5 三条语义终答（mincore:241 / madvise parked:1696 / msync:64-108）——A.1 直接造成且未登记的 -ENOMEM 型回归 | 审计 N 类小结 | guest DoD "strace 零新错误" 的直接威胁（JVM/mincore 路径） |
| **P5（扫尾）** | C5 perm_pgprot carrier/MAY 化、C6 mremap VMA 假设清理、C7 madvise RF 位 | spec §3.1.3 原定内容 | 纯函数矩阵/白名单注入可测 |

顺序理由: 放置面是唯一能造成内核侧**错误行为**（而非错误返回）的族, 且 A.2 落地后
触发面变宽（活跃窗也无 VMA 了）, 必须第一刀; J1 收口必须在 J2 walker 之前（walker
的"J1==0"前提）; S-5 在 J1 之后做也行, 但 mincore 的 vma_lookup 短路本身就是 J1
卫生的一部分, 建议同片。

---

## 2. 每个守卫/钩子的补丁形状（双行号: 主树 HEAD / worktree A.2 后）

### 2.1 帧表"含 idle"查询 API（P1 的地基, 先行）

**现状**: 能用的只有 `corten_arena_range_overlaps(mm, start, len)`
（主树 :8436-8469 / worktree :8816-8852）——"活跃 arena 状态"真源。它的语义被
16+ 个拒族钩子依赖（mprotect/madvise/migrate/mbind/soft-offline/move_pages…）,
**不能改语义**; 且它跳 idle（`READ_ONCE(ar->idle) → continue`, :8459/:8849）与
reserve 哨兵。`corten_arena_lookup`/`lookup_get`（主树 :2048/:3865, worktree
:2407/:4224 附近）同样跳 idle。**不存在任何"含 idle/parked"的帧在册判定**——需新增。

```c
/* include/linux/corten_arena.h（导出面）+ mm/corten_arena.c（实现, 建议紧挨
 * range_overlaps 放置, 主树 :8469 后 / worktree :8852 后） */
bool corten_arena_range_occupied_incl_idle(struct mm_struct *mm,
					   unsigned long start,
					   unsigned long len);
	/* 放置/所有权真源: [start,start+len) 与任何在册帧相交即 true——
	 * 活跃 arena、parked(idle) arena、reserve 哨兵全部算占用。
	 * 实现与 range_overlaps 同骨架（fast-negation 必须保留
	 * `!refcount_read(&state->nr) && !state->nr_pool` 双条件, 全池 parked
	 * 不得快否定）; 循环体去掉 idle/sentinel 的 continue。
	 * RCU xarray 走查, 任意上下文可调; 调用点全部在 mmap_write 下,
	 * 与 seg_claim/park 的 ctl_lock 写侧互斥, 无竞态窗口。
	 * =n 折叠: static inline return false（对齐 corten_arena.h:732 惯例）。 */
```

命名刻意与 `range_overlaps`（=活跃状态）区分: 一个回答"这里有活事务状态吗",
一个回答"这块 VA 是我的吗"。**不要**让 NOREPLACE 守卫复用 range_overlaps 再加参数
——那会诱惑后来者把 skip-idle 语义一并改掉。

### 2.2 P1a: NOREPLACE 守卫换弹药（热修第一刀）

- **插入点**: `do_mmap` 的 NOREPLACE 检查, 主树 mm/mmap.c:465-468 /
  **worktree :476-483**（A.2 已在此挂了 `corten_arena_range_overlaps`——换成
  incl-idle 变体即可, 一行改动 + 注释更新）:
```c
	if (flags & MAP_FIXED_NOREPLACE) {
		if (find_vma_intersection(mm, addr, addr + len) ||
		    corten_arena_range_occupied_incl_idle(mm, addr, len))
			return -EEXIST;
	}
```
- **锁上下文**: do_mmap 全程 `mmap_write_lock`（vm_mmap_pgoff 持有）; 无问题。
- **错误码**: **-EEXIST**——NOREPLACE 契约字面（legacy :466-467 同码）, 也是
  A.1 之前预约 VMA 时代该形状的返回值, 零语义偏移。
- **注意**: 哨兵也算占用后, 并发 seg_claim 期间（本就持 mmap_write, 互斥）不可能
  被观察到; 若实在担心, 可在注释中写明依赖该互斥。

### 2.3 P1b: plain MAP_FIXED over parked → idle-eject 臂（守卫族的隐藏成员）

审计 #14 只点名 NOREPLACE, 但**同一根因打穿 plain MAP_FIXED**: `corten_arena_mmap_punch_route`
（主树 :7480-7543 / worktree :7871-7934）用 `lookup_get(mm, addr)`（跳 idle）探测,
parked 窗 → 双端 NULL → "no arena involved" → 放行 mmap_region 在 parked 帧上装
legacy VMA。legacy 语义里 plain MAP_FIXED = **替换**, 所以这里不能学 NOREPLACE 拒绝,
要**先清场再放行**:

```c
/* mm/corten_arena.c, punch_route 早臂（lookup_get 之前） */
static int corten_arena_placement_punch_idle(struct mm_struct *mm,
					     unsigned long addr,
					     unsigned long len);
	/* MODE 无关（registry 即真源）; mmap_write 已持, 内取 state->ctl_lock
	 * （DEV-13 序: mmap_write → ctl_lock, 与 pool_prepare 同序）。
	 * 对 [addr,addr+len) 内每个 idle arena: 整覆盖 → pool_eject_locked
	 * （帧回杂志, 描述符拆除）; 部分覆盖（外来 MAP_FIXED 切 parked 边界）
	 * → 同样 eject（parkable 不变量要求整片, 不做部分拆除）。
	 * reserve 哨兵帧: 不动（seg_claim 互斥下此处见不到; 见到即 WARN+计
	 * 数, 保守返回 -EOPNOTSUPP 让上游拒）。
	 * 返回 0 = 清场完成/无事可做, 放行 legacy 装 VMA（=合法植入形态）。 */
```
- **插入点**: punch_route 的 `if (!(flags & MAP_FIXED) || ...NOREPLACE) return 0;`
  之后、`ar_start = lookup_get(...)` 之前（主树 :7489 / worktree :7890 前）。
- **错误码**: 0（放行）。eject 失败路径透传 -errno（内存性, 极冷）。
- **为什么 eject 而不是 -EOPNOTSUPP**: 拒绝会让 strace 出现 A.1 前不存在的
  错误返回, 破坏 guest DoD; eject 后帧表干净, legacy VMA 落在无主 VA 上,
  INV-MV2/后续 reactivate 都安全。

### 2.4 P3: `__mmap_prepare` backstop 补零 VMA 分支（纵深防御）

- **插入点**: mm/vma.c `__mmap_prepare`, `vms->vma = vma_find(vmi, map->end)`
  与 `if (vms->vma) {` 之间——主树 :2467-2472 / **worktree 同行号（vma.c 未被
  A.2 改动, 2467/2472 均实测一致）**:
```c
#ifdef CONFIG_CORTEN_MM_ARENA
	/* V-A.3 P3: the zero-VMA placement shapes (#14/#15/#16).  The routes
	 * above (NOREPLACE -EEXIST, punch idle-eject) make this unreachable;
	 * a hit here is an audit event, not a semantic return. */
	if (corten_arena_range_occupied_incl_idle(map->mm, map->addr,
						  map->end - map->addr)) {
		atomic_long_inc(&corten_nr_placement_backstop);
		return -EOPNOTSUPP;
	}
#endif
```
- **锁上下文**: mmap_region → mmap_write 全程。
- **错误码**: **-EOPNOTSUPP**（与同函数既有 corten backstop :2492-2494 同码;
  该分支正常不可达, 是"响"不是"答", 配计数披露）。
- 既有 `if (vms->vma)` 内的 range_overlaps backstop 保持不动（活跃+有 VMA 形态
  继续由它管）。

### 2.5 P2 验证（hint 守卫, A.2 已落, V-A.3 只剩复核+测试）

A.2 已挂: `corten_addr_in_window(addr,len)`（双门: static-branch +
`current->mm->corten_mode`, 窗口 = `[CORTEN_MODE_WINDOW_START, CORTEN_MODE_WINDOW_END)`
= `[16T,64T)`, include/linux/corten_arena.h:289-290）于 arch_get_unmapped_area
hint 臂（sys_x86_64.c:148）、topdown hint 臂（:197-202, goto fenced walker）、
generic 两处（mm/mmap.c:816/:869）; `corten_fence_unmapped_area(&info)` 于全部
六个 walker 出口（bottomup/topdown × x86/generic + topdown 失败重走臂）。主树对应
原始行号: sys_x86_64.c:143-149/:189-197、mm/mmap.c:809-816/:860-867。
V-A.3 剩余: (a) 复核 topdown 的 `bottomup:` 标签重入（MAP_32BIT goto——重入
arch_get_unmapped_area, 已有守卫, 确认即可）; (b) KUnit 锚（§5）;
(c) 溢出证明: `addr + len > WINDOW_START` 判式中 len 上溢不会把窗内地址误放行
（addr 已 PAGE_ALIGN, len 上限 TASK_SIZE, 相加 < 2^63, 安全; 写进注释）。

### 2.6 P4: reactivate/pool_take 防御断言

- **插入点**: `corten_arena_pool_reactivate`（主树 :6578 / worktree :6938）与
  `corten_arena_pool_take`（主树 :7008 / worktree :7397）头部, idle 翻转
  （`WRITE_ONCE(ar->idle, false)`）之前:
```c
	/* P4 (V-A.3): a parked window must be VMA-free at handout.  V-A.1
	 * retired the vma_lookup validation arm; this is its defensive
	 * replacement -- a foreign tree VMA inside the range means a
	 * placement guard failed upstream.  Fail safe: eject, never hand
	 * frames out under someone else's VMA. */
	if (corten_vma_find(mm, ar->start, ar->end - ar->start)) {
		WARN_ONCE(1, "corten: foreign vma in parked window %lx-%lx\n",
			  ar->start, ar->end);
		atomic_long_inc(&corten_nr_p4_ejects);
		corten_arena_pool_eject_locked(state, ar);
		return -EAGAIN;	/* pool_take: 走 fresh 路径重选窗 */
	}
```
- **锁上下文**: 两函数均要求 mmap_write + ctl_lock（既有注释明示）; `corten_vma_find`
  = mm 内部别名（worktree mm/mmap.c:1011-1029, 不触发 J1 probe）。
- **错误码**: -EAGAIN → pool_take 退 fresh 路径（对上层是普通 pool miss,
  无新错误面）。

### 2.7 J1 五挂点: 3/5 已落, 补 2 + 豁免扫尾

**已落（A.2, 复核即可）**:
1. `find_vma_intersection`（主树 :988 / worktree :1000-1009, probe 在 mt_find 后）;
2. `find_vma`（主树 :1005 / worktree :1044-1052）;
3. `lock_vma_under_rcu`（mm/mmap_lock.c:224 定义体, 主树 mas_walk 在 :230 /
   worktree :234-237 probe 已插在 mas_walk 后、`if (!vma)` 前）。

**待补**:
4. `find_vma_prev`（主树 mm/mmap.c:1027-1043 定义体 / worktree ~1057-1073）:
   与 find_vma 同形——`vma = vma_iter_load(&vmi);` 前后插
   `corten_j1_probe(mm, addr, addr + 1, NULL)`（此函数无单值返回可判 hit, hit
   判定移到 probe 内部或只计 probes; 建议签名扩成 probe(mm,start,end,vma) 用
   load 结果, 与另两处一致）。
5. `find_vma_and_prepare_anon`（mm/userfaultfd.c:41-56, 主树=worktree, 未改）:
   在 `vma = vma_lookup(mm, addr);` 后插 probe。**注意**: `vma_lookup` 是
   `mtree_load` 直读（include/linux/mm.h:3641）, **不经过 find_vma**——所以这个
   挂点不可省, 且见 §6 陷阱 3。

**计数器写法（用户核心问题: RCU 无锁下怎么写）**: A.2 选择了 `static atomic_long_t
corten_nr_j1_probes / corten_nr_j1_hits`（worktree mm/corten_arena.c:1802
`corten_j1_slow`）, `atomic_long_inc()`。**结论: 保持 atomic_long, 不要 per-cpu/
this_cpu_inc**。论证: (a) `lock_vma_under_rcu` 在 preemptible RCU 下可被抢占迁移,
`this_cpu_inc` 的取址-递增两步会被迁移打断（需要 preempt_disable 包裹才安全）,
atomic_long 无条件安全（含 NMI）; (b) 这些计数只在"违例路径"递增（终态目标 0）,
竞争压力不存在, per-cpu 的收益为零; (c) 与全屋 60+ 个 `corten_nr_*` 惯例一致
（主树 mm/corten_arena.c:176-278）。常开开销只有 inline 门
（`corten_enabled_static() && READ_ONCE(mm->corten_mode) &&` 两次窗口比较,
worktree corten_arena.h:393-401）——锁无关读, RCU 内合法。guest 侧 bpftrace
（kprobe `find_vma` 第一参数 ≥ 窗口起点）独立双口径不变（spec §1.3 J1(b)）。

**corten 自身豁免**: A.2 用**别名通道**而非审计原提的 per-cpu 标记——
`corten_vma_find(mm, start, end)`（worktree mm/mmap.c:1011-1029, 不导出）镜像
`find_vma_intersection` 的 mt_find。worktree 已转换 6 处（:2647/:2780/:2840/:2973/
:4364/:7840 = 放置障碍扫描×4 / fault tier-2 / punch split 扫描）。**V-A.3 扫尾**:
对照审计 #52 清单核对主树其余调用点（:1082 DECLARE validate 的 vma_lookup——A.2
auto 路径后该形态是否仍可达, 复核; :6730 parkable 的 vma_lookup; :7354/:7371
punch_split 的 vma_lookup——这三处查的是**真树 VMA**（植入/遗留片）, 若留在
find_vma 族上会自污染 J1, 需逐个判断: 查窗口地址的换 `corten_vma_find`; 查
委托域地址的（不会进窗口判定）可不动）。**语义陷阱**: `corten_vma_find` 是
intersection 语义（区间内第一个 VMA）, 不是 find_vma 的"addr 或其后第一个"——
替换 `find_vma(mm, a)` 时必须写成 `corten_vma_find(mm, a, a+1)` 并确认原调用
不依赖"下一个 VMA"语义（worktree :4364 的 tier-2 原是 find_vma_intersection,
安全; 换 vma_lookup 场景时同样只在"判定覆盖"语义下等价）。

### 2.8 J1 卫生: fault 两跳（#1/#2）与 uffd（#29）

- **#1 快路径**: arch/x86/mm/fault.c corten 钩子 FALLBACK 臂（主树 :1381
  `default: break;` 处, worktree 未改此文件）——不能直接 MAPERR 终答（会引入
  与并发 DECLARE 的假 SIGSEGV 竞态: legacy 的串行化点在慢路径的 mmap_read）。
  正确形状:
```c
		default:
			/* V-A.3 #1: window-domain faults take the slow path
			 * directly -- lock_vma_under_rcu's mas_walk is pure
			 * overhead there (always NULL post-A.1) and it
			 * pollutes J1.  The mmap_read in the slow path keeps
			 * the legacy race serialization vs concurrent
			 * DECLARE/park. */
			if (static_branch_unlikely(&corten_enabled_key) &&
			    mm->corten_mode &&
			    address >= CORTEN_MODE_WINDOW_START &&
			    address < CORTEN_MODE_WINDOW_END) {
				atomic_long_inc(&corten_fault_fallback_window);
				goto lock_mmap;
			}
			break;	/* CORTEN_FAULT_FALLBACK: run legacy */
```
- **#2 慢路径**: mm/mmap_lock.c `lock_mm_and_find_vma` 的两个 find_vma 分支
  （主树 :433/:459, worktree 行号+8 左右）——在各自 `mmap_read_lock` 之内、
  find_vma 之前: `MODE ∧ 窗口 ∧ !corten_arena_lookup(mm, addr)`（active 命中则
  继续走 find_vma 不该发生——快钩已拦; 此判等于纯防御）→ 计数 + `return NULL`
  （MAPERR 结局与今天相同, 少一次树走查）。此处在 mmap_read 下, 与 DECLARE/park
  的 mmap_write 串行, **无新竞态**。
- **#29 uffd**: mm/userfaultfd.c mfill 入口（:1581/:1590 附近的
  `find_vma_and_prepare_anon` 调用前）与 uffd_move 入口加同一双门窗口短路:
  `-ENOENT`（C12 已认可的终态错误码）+ `corten_uffd_window_reject` 计数。
  process_uffd 与 ioctl 层无需另挂（同漏斗）。

### 2.9 J2: INV-MV2 walker + 植入登记

**植入登记（先有登记, walker 才有白名单可查）**:
```c
/* include/linux/corten_arena.h + mm/corten_arena.c */
	/* per-mm 植入区间树（interval_tree; spec §1.2 "登记植入"的落点）。
	 * 生产者封闭两处: (1) punch_route 成功臂（D-G'' file MAP_FIXED,
	 *   主树 :7530 / worktree ~7920）; (2) P1b idle-eject 放行臂。
	 * 消费者: J2 walker 与 exit 清理。mmap_write + ctl_lock 下写。 */
void corten_implant_mark(struct mm_struct *mm, unsigned long start,
			 unsigned long len);
bool corten_implant_covers(struct mm_struct *mm, unsigned long start,
			   unsigned long len);	/* VMA ⊆ ∪登记区间 */
```
页粒度区间（punch 可 sub-PMD）, 用内核现成 `struct interval_tree_node`。split/
merge 天然容忍: 拆出的子 VMA 仍 ⊆ 原区间; 相邻植入合并后 VMA ⊆ 区间之并（区间
判定逐点成立）。munmap 路由/exit 时清除（防泄漏: 也可不清、exit 免检——区间树
随 corten_state 释放即可, walker 只在 mm 存活期跑）。

**walker**:
```c
/* mm/corten_arena.c */
int corten_audit_j2_walk(struct mm_struct *mm);
	/* INV-MV2: 树上每个 VMA 满足 (vm_end <= WINDOW_START ∨
	 * vm_start >= WINDOW_END ∨ corten_implant_covers(∩窗部分))。
	 * 违例: WARN_ONCE + corten_nr_j2_violations++，返回非 0。
	 * 触发点（全部冷点）: corten_arena_mm_exit 头部（主树 :1894 /
	 * worktree :2253, mm_users==0 无锁）; pool_park_locked /
	 * pool_take / pool_reactivate 尾部（主树 :6802/:7008/:6578,
	 * worktree :7191/:7397/:6938, mmap_write+ctl_lock 下）;
	 * munmap/madvise/mremap route 返回前（主树 :7106/:8542/:8268,
	 * worktree :7497/:8922/:8648, 各自锁下）; fork_commit 尾部
	 * （主树 :3078 附近 / worktree :4096, 双 mmap_write 下）;
	 * debugfs 手动触发（mmap_read + RCU 走查形态, 见下）。
	 * 实现: VMA_ITERATOR 全树走查（冷点, 成本无关紧要）; debugfs 变体
	 * 用 mmap_read_lock + vma_next（RCU 安全, 对齐 proc 迭代惯例）。 */
```
KUnit 注入违例样本走 debugfs 同一入口（§5）。**walker 谓词设计说明**: 严格的
"⊄ 窗 ∨ 登记植入" 需要 implant 登记（本节）; 审计的降级形态（"∩窗部分无在册帧"）
只能抓帧冲突、抓不住"外来 VMA 落在窗口空洞"（P1-P3 失守的另一半）, 不建议作为
主断言, 可作为登记缺失时的兜底 WARN。

### 2.10 四观测计数器（+ 一个 P3/P4 披露计数器）

全部 `static atomic_long_t corten_nr_*`, 双门 + 窗口判定, 无返回值影响:

| 计数器 | 挂点（主树 / worktree） | 上下文 |
|---|---|---|
| `corten_gup_window_miss` | mm/gup.c gup_vma_lookup 的 find_vma 返回 NULL 且窗口（主树 :1305 / A.2 后漂移小） | mmap_read 或 FOLL_UNLOCKABLE |
| `corten_remote_access_window_short` | mm/memory.c `__access_remote_vm` 早退臂（主树 :6950-6960）/:7081 同族 | mmap_read_killable |
| `corten_uffd_window_reject` | §2.8 #29 两入口 | mmap_read |
| `corten_fault_fallback_window` | §2.8 #1 FALLBACK 臂（fault.c:1381） | 无锁（RCU 退出的钩子内） |
| `corten_nr_placement_backstop` / `corten_nr_p4_ejects` | §2.4 / §2.6（披露用, 正常 0） | mmap_write |

（#3/#7/#8 的**修复**在 V-C `corten_gup_probe`; V-A.3 只量化。）

### 2.11 S-5 三条终答

A.1 前基线（parked = PROT_NONE 匿名预约 VMA）: mincore → 0 + **全 0 向量**
（can_do_mincore 对匿名 vma 恒 true, 无 PTE → `__mincore_unmapped_range` 写 0,
mm/mincore.c:124-140）; msync → 0（匿名无 file, msync.c:88-108 空转）;
madvise(DONTNEED) → 0（PROT_NONE 不挡 DONTNEED）。空洞窗（从未有 arena）A.1
前后都是 -ENOMEM/-EFAULT——**终答只对"占用"（含 idle）窗恢复基线, 空洞保持
legacy 错误码**。

- **mincore**（mm/mincore.c:235-253 `do_mincore`, vma_lookup 在 :241, 主树=
  worktree 未改）: 入口前置路由:
```c
#ifdef CONFIG_CORTEN_MM_ARENA
	{
		long crt = corten_arena_mincore_route(current->mm, addr,
						      pages, vec);
		if (crt != -EAGAIN)	/* -EAGAIN = 非我族, 走 legacy */
			return crt;
	}
#endif
```
  `corten_arena_mincore_route`（新, corten_arena.c）: 门 = MODE ∧ addr⊂窗 ∧
  [addr, min(end, WINDOW_END)) **整段** occupied_incl_idle → parked 段直接
  `memset(vec, 0, pages) + return pages`; **活跃段**走 `walk_page_range_novma`
  直读 PTE（present→1, swap→mincore_swap, none→0; 窗内无 THP, C20 结构排除,
  pmd 锁可省）——真值而非 0 向量（A.2 前活跃窗 mincore 是真驻留向量, 退化为 0
  向量是未登记回归）。非整段占用/跨界混合形态 → -EAGAIN 走 legacy（保持既有
  -ENOMEM, 登记为边界残余）。这是 OQ-MV-11 的提前交付（~+120）。
- **msync**（mm/msync.c:63-110, find_vma 在 :64/:102/:108）: 循环内 `if (!vma)`
  前加跳过臂: `MODE ∧ start⊂窗 ∧ occupied_incl_idle` → `start = min(end,
  占用段末端)`、不置 `unmapped_error`、`vma = find_vma(mm, start)` 重入循环;
  纯占用段最终 `error = 0` 返回。空洞仍 -ENOMEM（`unmapped_error` 机制原样）。
  只挂首入口 :64 一处不够——分段推进后 :102/:108 的重取也要过同一跳过臂
  （抽一个 `corten_msync_skip(mm, &start, end)` 内联helper, 三处调用）。
- **madvise parked**（mm/madvise.c:1696 walk 起点; 但修在路由层:
  corten_arena_madvise_route 主树 :8542 / worktree :8922）: 函数头部加
  MODE∧⊂窗∧occupied_incl_idle∧非活跃臂:
  - DONTNEED/DONTNEED_LOCKED/FREE(+LOCKED 不存在, uapi 无) → 返回 1（内容在
    park 时已清, 0 即语义等价）;
  - NORMAL/SEQUENTIAL/RANDOM/COLD 四 hint → 返回 1（既有活跃臂的 parked 延伸）;
  - 其余行为（WILLNEED/POPULATE 系…）维持现状（走 legacy → -ENOMEM）, **登记
    为 S-5 残余行**（A.1 前对 PROT_NONE vma 多为 0, 逐行为补齐放 V-C 前复议）。
  该臂在 `madvise_do_behavior` 的路由块（madvise.c:1944-1961, worktree 漂移小）
  内生效, 对 `try_vma_read_lock` 的 RCU 快臂（:1644）天然免疫——路由在前。
  madvise.c:962 的 dontneed 重查与 :1696 的 walk 随路由终答不再触窗。

### 2.12 C5/C6/C7 路由扫尾（spec §3.1.3 原定）

- **C5 perm_pgprot**: 主树已双轨——`corten_arena_perm_pgprot(vma, perm)`（:594,
  吃 shadow-VMA flags）与 `corten_arena_perm_pgprot_pure(perm)`（:590, 零 VMA）。
  fault 族已全部 `vma ? pgprot(vma) : pure`（A.2b 后 ctx.vma=carrier, 这些调用
  读到的已是 carrier MAY 位——本身就是 C5 终态形状）。剩余: `protect_window/
  protect_range` 的 PTE 重编码（主树 :8308 附近）从 `corten_arena_shadow_vma(ar)`
  改读 `ar->carrier`（A.2b 字段）; 全文件 `grep corten_arena_perm_pgprot(`
  逐个标注数据源; KUnit 纯函数矩阵（pure(carrier MAY 位) ≡ pgprot(carrier, perm)）。
- **C6 mremap VMA 假设清理**: `corten_arena_mremap_route`（主树 :8268 /
  worktree :8648）与 `mremap_move` 内 residual shadow deref 清点（A.2b 后
  `corten_arena_shadow_vma()` 恒 NULL 的死臂删除/改 carrier）; shrink 尾 zap
  的 gather 参数复核; 与审计 #21（syscall 入口先答, 内部 lookup 只见 legacy）
  对表确认无新洞。
- **C7 madvise RF 位**: `MADV_DONTFORK/DOFORK/WIPEONFORK/KEEPFORK` 对活跃 region
  从默认拒（`range_overlaps → -EOPNOTSUPP`）改为 `ar->rflags` 的
  CORTEN_RF_DONTCOPY/CORTEN_RF_WIPEONFORK 置/清（spec C7 行: "由拒绝改 RF 记录,
  行为等价: fork 镜像读 RF"）。**锁纪律**: rflags 声明为 mmap_write 下写
  （include/linux/corten_arena.h @rflags 注记）, 而 madvise 可在 mmap_read/
  per-VMA read 下跑——RF 四行为要么 (a) 在路由层强制走 write（madvise_lock 升级
  不可行, process_madvise 远程也走这里）, 要么 (b) rflags 改 desc 写锁保护的
  RMW（bitmap set/clear 原子性用 desc lock, fork 镜像在 freeze+drain 后读, 无竞）。
  **建议 (b)**: 一次 `corten_arena_rflags_update(mm, addr, mask, set)` helper,
  lookup_get 拿 active ref → desc 锁内 RMW → put。fork_mirror 读侧对拍 A.0
  KUnit（rflags 镜像已有断言惯例）。

---

## 3. 帧表"含 idle"查询 API —— 现状与新增（汇总）

| 需求 | 现有函数 | 能否直接用 | 结论 |
|---|---|---|---|
| 活跃 arena 状态（拒族） | `corten_arena_range_overlaps`（主 :8436 / wt :8816） | 是 | 不动（16+ 调用者语义依赖 skip-idle） |
| 单点活跃覆盖（事务） | `corten_arena_lookup` / `_get`（主 :2048/:3865 / wt :2407） | 是 | 不动（fault/space 路由依赖 skip-idle+freeze+ref） |
| parked 窗在册判定（放置守卫/INV-MV2/S-5） | **无** | — | 新增 `corten_arena_range_occupied_incl_idle(mm, start, len)`（§2.1 签名） |

实现约束三条: fast-negation 保留 `nr==0 && nr_pool==0` 双条件（全池 parked 不得
快否定, range_overlaps :8451 已有同款）; 哨兵计占用（放置语义上哨兵=已认领 VA;
mmap_write 互斥使运行期实际见不到）; RCU 走查照抄邻函数（不需要 ref: 只读
start/end/idle, RCU 免疫释放）。

---

## 4. 子切片建议（300 行红线）

spec 原 V-A.3 预算 ~+400/−150(+150 测试)。A.2 已吸收 ~300 行（hint 守卫+3 挂点+
别名通道）; 审计新增量（S-5/观测计数/放置热修/J2 登记）进来后, 剩余总量约
**+800 内核 / +450 测试**——必须切 4 片（每片一夜+review）, 依赖严格线性:

| 片 | 内容 | 预估（内核+测试） | 依赖 |
|---|---|---|---|
| **V-A.3a 放置面热修** | `occupied_incl_idle` API + NOREPLACE 换弹药（2.2）+ punch idle-eject 臂（2.3）+ `__mmap_prepare` 零 VMA backstop（2.4）+ P4 防御断言（2.6）+ 植入登记 `corten_implant_mark`（2.9 前半, eject/punch 两生产者）+ KUnit（§5 A 组） | ~+260/−40 内核, ~+140 测试 | A.2b 入库（carrier 字段; 若 A.2b 拖期, 本片可仅依赖 A.2a——`shadow_vma` 改读可后置） |
| **V-A.3b J1 收口** | find_vma_prev + find_vma_and_prepare_anon 两挂点（2.7）+ corten 内部豁免扫尾（#52 逐点）+ fault #1/#2 短路（2.8）+ uffd #29 + 四观测计数器（2.10）+ KUnit（B 组） | ~+200 内核, ~+110 测试 | A.3a（occupied_incl_idle 被 #2/#29 复用） |
| **V-A.3c J2 walker** | INV-MV2 walker + 全触发点接线 + debugfs 手动入口 + KUnit 违例注入（C 组） | ~+230 内核, ~+90 测试 | A.3a（implant 登记） |
| **V-A.3d S-5 + 扫尾** | mincore/msync/madvise-parked 三终答（2.11）+ C5/C6 sweep + C7 RF 位（2.12）+ KUnit（D 组） | ~+290 内核, ~+150 测试 | A.3b（S-5 短路本身是 J1 卫生; C7 需要 A.3a 的 rflags 决议） |

若 A.3d 超 300 红线（mincore novma 走查 ~120 是最大变数）, 优先把 C7 RF 位拆出
成 A.3e（~+90）, mincore 活跃段真值走查可降级为 0 向量 + 登记披露（决策点写死在
切片 DoD 里）。**A 系出口检查（J1/J2 首次全绿）放在 A.3c 末**: guest run13+churn,
`find_vma` 窗口命中计数==0（计数器 + bpftrace 双口径）+ INV-MV2 walker 零违例。

---

## 5. KUnit 测试锚草案（对照 mm/corten_arena_test.c 惯例）

惯例: `corten_arena_test_mm_setup(test)` 造 mm; 需要进程上下文的走
`corten_arena_test_run_op`（kthread_use_mm, :217-229）; `corten_enabled_static()`
为假时 `kunit_skip`（:554 惯例）; 计数器读取需要 test accessor（主树 :1863
`corten_arena_test_perm_pgprot_pure_eq` 同型, 新增
`corten_arena_test_j1_counters(u64 *probes, u64 *hits)`）。

**A 组（A.3a, 放置守卫）**:
1. `corten_arena_test_noreplace_parked`: MODE mm + declare→fault→munmap 整窗
   （park）; `vm_mmap_pgoff(parked_addr, len, PROT_READ|PROT_WRITE,
   MAP_FIXED_NOREPLACE|MAP_ANONYMOUS|MAP_PRIVATE, ...)` → **-EEXIST**; 交叉断言
   树无 VMA（vma_lookup 为 NULL）+ 帧仍在册（occupied_incl_idle==true）。
2. `corten_arena_test_noreplace_active`: 同形对活跃窗（A.2 后活跃窗无 VMA）→
   -EEXIST（防止 A.2 已有守卫回退的回归锚）。
3. `corten_arena_test_mapfixed_over_parked`: plain MAP_FIXED 同址 → 返回该址、
   VMA 在树、**该窗帧已出册**（occupied==false）+ implant 登记 covers; 再
   pool_take 同尺寸 → 不再命中该窗（fresh 重选）。
4. `corten_arena_test_occupied_incl_idle`: range_overlaps 的镜像矩阵——
   parked/hole/活跃 × 内/边界/跨界/零长（对齐 :546 既有 range_overlaps 用例的
   九形态, 断言值取反于 idle 轴）。
5. `corten_arena_test_p4_eject`: 手工往 parked 窗注入外来 VMA
   （`__split_vma`/vm_brk 形态或直接 punch 残留）→ pool_take → 观测
   -EAGAIN/fresh 重选 + `corten_nr_p4_ejects` +1 + WARN_ONCE 抑制
   （kunit 下用 WARN 俘获或计数断言）。
6. `corten_arena_test_hint_fence`（x86 KUnit 或走 generic 变体）: MODE mm,
   `generic_get_unmapped_area(NULL, 窗内 hint, len, 0, 0, 0)` → 返回址 ∉ 窗;
   hint 跨窗下边界（16T-1 页起）→ 同拒; 非 MODE mm 同 hint → 原样接受
   （零扰动锚）。

**B 组（A.3b, J1 开合）**:
7. `corten_arena_test_j1_open_close`: 计数器快照 → 直接调 `find_vma(mm, 窗addr)`
   → probes +1、（无 VMA）hits +0; `find_vma_prev(mm, 窗addr, &p)` 同; 清零
   语义：counter 只增, 断言用 delta。
8. `corten_arena_test_j1_self_exempt`: MODE mm 走一轮
   `corten_arena_auto_mmap_route`（内部 obstacle 扫描经 corten_vma_find）→
   probes delta == **0**; 正控制: route 后手工 find_vma(窗addr) → delta == 1。
   （豁免的正确性锚 = "corten 自身零自计数", 对审计 #52。）
9. `corten_arena_test_j1_legacy_zero`: 非 MODE mm（corten=on 但 mode=0）上
   find_vma 系列 delta == 0（双门的第二门锚）。
10. `corten_arena_test_uffd_window_reject`: mfill 形状（合成 userfaultfd_ctx 可
    造性差时退化为: 直接断言短路函数返回 -ENOENT + 计数）。

**C 组（A.3c, INV-MV2）**:
11. `corten_arena_test_inv_mv2_clean`: declare→fault→mprotect→madvise→mremap→
    fork→park 全生命周期后 `corten_audit_j2_walk` == 0（spec 终局锚的子集）。
12. `corten_arena_test_inv_mv2_inject`: 注入外来 VMA 入窗（测试特权: 直接
    `vm_brk` 窗址或 punch 后未登记形状）→ walker 返回非 0 + 计数 +1; 注入后
    `corten_implant_mark` 补登记 → 复跑 == 0（白名单机制自证）。
13. `corten_arena_test_implant_lifecycle`: punch→mark→split→merge→munmap 全链
    covers() 断言（区间树谓词的拆合容忍）。

**D 组（A.3d, S-5 与扫尾）**:
14. `corten_arena_test_mincore_parked`: park 后 sys_mincore 等价调用
    （do_mincore 直调 + kthread op）→ 返回 pages、vec 全 0。
15. `corten_arena_test_mincore_active_truth`: 活跃窗 fault N 页（其中 M 页
    swap_out）→ vec 前导 N' 个 1、swap 位与 mincore_swap 口径一致（真值锚,
    A.2 前基线对拍）。
16. `corten_arena_test_msync_madvise_parked`: park 后 msync/DONTNEED/FREE/
    四 hint → 全 0/成功; 空洞窗址 → -ENOMEM（保留锚）。
17. `corten_arena_test_rflags_rf`: MADV_DONTFORK→WIPEONFORK→DOFORK 序列后
    rflags 位与 fork 镜像读取对拍（A.0 镜像断言扩展）。
18. `corten_arena_test_perm_pgprot_carrier`: pure(perm) ≡ pgprot(carrier, perm)
    矩阵（perm 8 态 × pkey 2 态, 对齐 :1863 既有 eq accessor 形态）。

---

## 6. 风险与红线核对

1. **=n 折叠（铁律 1）**: 所有新钩子 = `#ifdef CONFIG_CORTEN_MM_ARENA` + 双门
   （static-branch + mm->corten_mode）; `occupied_incl_idle`/`corten_j1_probe`/
   `corten_addr_in_window` 在 =n 下给 static inline 常量假值（对齐
   include/linux/corten_arena.h:732 惯例）; 每片过 =n 八对象 + nm 零符号。
2. **hint 守卫不得打坏 legacy mmap**: 判定必须精确 `[16T,64T)` ∧ `corten_mode`。
   已核: 32bit 兼容 syscall 的 find_start_end 终点 <4T, 判式恒假; MAP_32BIT goto
   bottomup 重入带守卫的 arch_get_unmapped_area; 非 MODE mm 只付 static-branch
   读。J1 probe 的窗口判定用调用参数（mm/start/end）而非 current->mm——远程
   process_madvise/process_vm_readv 查目标 mm 时也必须计数（审计 #20/#7 的本意）。
3. **J1 五挂点网眼的诚实披露**: `vma_lookup`（= `mtree_load`, mm.h:3641）与
   nommu/gate 等不走 find_vma——mincore:241 / memory.c:6956 / gup.c:1305 /
   userfaultfd.c:42 的裸 vma_lookup **不在这五挂点覆盖内**。对策: S-5/#29 的
   消费者短路（§2.8/2.11）把这几个高频裸点在到达 vma_lookup 之前终答;
   REPORT 的 J1 口径一节写明"五原语 + 消费者短路"的覆盖形状, 剩余裸
   mtree_load 消费者以审计 #3-#13 清单兜底（V-C 归零）。
4. **不改 `range_overlaps` 语义**（§2.1/§3）: 16+ 拒族调用者依赖 skip-idle;
   新语义只存在于新函数。
5. **punch idle-eject 的锁序**: mmap_write（punch 路由既有）→ ctl_lock
   （pool_eject_locked 要求）——DEV-13 既有序, 不新增边; eject 后必须同步
   implant 登记, 防 walker 窗口期假阳性（两者同锁界内完成）。
6. **msync 跳过臂不得吞 unmapped_error**: 只有"占用"窗段跳过且不置错误;
   混合形态（窗+空洞+委托 VMA）保持 legacy 的"处理全部 + 尾部 -ENOMEM"行为。
7. **fault #1 短路不改竞态语义**: `goto lock_mmap` 保留慢路径 mmap_read 的
   DECLARE 串行化; 不做无锁 MAPERR 终答（那会引入并发 DECLARE 下的假 SIGSEGV）。
8. **C7 rflags 写锁**: 见 §2.12——mmap_read 下的 madvise 不能裸写 rflags,
   desc 锁 RMW; fork 镜像读侧已在 freeze+drain 静止期, 无新边。
9. **walker 触发点全冷点**: 全树走查只挂 exit/park/route 尾/fork_commit/debugfs;
   严禁挂热路径。debugfs 变体 mmap_read+RCU 走查（对齐 proc 迭代）。
10. **行号基座声明**: 本包主树行号 = d40eae59ba76 实测; worktree 行号 =
    A.2 未提交增量实测（corten_arena.c 主 2048→wt 2407 / 主 8436→wt 8816 /
    主 8542→wt 8922 等, §2 已逐点双标）。A.2 入库若再动, 以函数名 + 锚注释定位。
11. **guest 判据（A 系出口, A.3c 末验）**: run13 + churn + J1 计数器==0 ∧
    bpftrace `find_vma` 窗口命中==0（双口径）∧ INV-MV2 walker 零违例;
    strace diff 除登记的 S-5/S-1..S-4 例外清单外零新错误。

---

## 7. 一页决策表（提请主 agent 批准的三处）

| 决策点 | 建议 | 备选 |
|---|---|---|
| plain MAP_FIXED over parked | idle-eject 后放行（legacy 替换语义保真） | -EOPNOTSUPP 拒绝（strace 新错误, 不取） |
| mincore 活跃窗终答 | `walk_page_range_novma` 真值（~+120, OQ-MV-11 提前交付） | 0 向量 + 登记披露（省行数, 保真度损失） |
| 植入白名单形态 | per-mm interval tree 登记（`corten_implant_mark`） | 降级为"∩窗无在册帧"谓词（抓不住空洞外来 VMA, 只作兜底） |
