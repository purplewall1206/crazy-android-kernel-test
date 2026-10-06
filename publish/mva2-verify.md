# M-V A.2a/A.2b 验证报告 · auto-attach 零 VMA + carrier detached VMA（VMA 移除系列第三、四片）

- 日期: 2026-09-22（白班实现 + 通宵验证, D14 精神延续, timegate 后跑矩阵）
- worktree: /home/ppw/linux-6.18-mva（分支 mv-a0, 基座 d40eae59ba76 = A.1, **未 commit**）
- 合同: MV_VMA_FREE_SPEC.md §3.1.2（A.2a 验证与路由 / A.2b 载体与 fork）+ §2.3 载体 + §1.2/1.3（Dominion/J1）
- 状态: 【草稿——夜间矩阵跑完后回填 §2-§5】

## 0. 手术清单（实现面）

### A.2a（auto-attach 不经 mmap_region + 验证清单）

| # | 位置 | 手术 |
|---|---|---|
| A1 | `corten_auto_validate()`（新, mm/corten_arena.c） | mmap_region 验证清单移植为纯函数: def_flags VM_LOCKED→-EAGAIN（mlock_future 口径）、prot==PROT_EXEC→-EOPNOTSUPP（execute-only pkey 位无 region 编码, sec 2.6 REJECT 行）、may_expand_vm（RLIMIT_AS/DATA）。由 route 在 classify/OVERCOMMIT 之后、**get_state/placement 之前**调用; 拒绝→`auto_vgate` 计数+legacy 降级（同一 errno 由 legacy 全链交付, 无窗口段被发放） |
| A2 | `do_mmap()` cret==1 分支（mm/mmap.c） | **不再走 mmap_region/MAP_FIXED 流**: placement 后直接 `corten_arena_auto_attach(mm, addr, len, prot)`（novma declare+charge）成功即 return addr; declare 失败（内存压力）才降级 legacy 流（attach_fails 计数, 既定降级合同）。`corten_auto_arena/corten_auto_len` 局部变量与尾部 attach 钩删除 |
| A3 | LSM 补钩 → **SPEC 修正** | SPEC 称"anon 路径今天也没调 security_mmap_file"在 6.18 不成立: 一切 mmap syscall 均经 `vm_mmap_pgoff()`（mm/util.c:580）先调 `security_mmap_file(file=NULL,...)` + fsnotify_mmap_perm, cret==1/cret==2 两臂都继承覆盖。**不再补调**（二次调用=LSM 钩子双触发, 行为变更）; MV-4"既成缺口"据此修正, 摘要见 §6 |
| A4 | do_mmap MAP_FIXED_NOREPLACE 门 | 加 `corten_arena_range_overlaps()`: 零 VMA 后 NOREPLACE 的 -EEXIST 保证不能依赖树上 VMA（活窗口已不可见）; parked 窗不拦（S-4 口径: parked==已 munmap, 语义允许放置; 后续 [C1] eject 兜底） |
| A5 | 分配器 Dominion 栅栏（新） | `corten_fence_unmapped_area()` + `corten_addr_in_window()`（mm/corten_arena.h 内联, 双门形态）接入 mm/mmap.c generic_get_unmapped_area{,_topdown} + arch/x86/kernel/sys_x86_64.c 同族: MODE 进程的 hint-less 分配 topdown 钉在 WINDOW_START 之下 / bottom-up 钉在 WINDOW_END 之上; 窗内 hint 拒走 accept 快路径（落回有栅栏的 walker, 对窗内 hint 最终 -ENOMEM）。**闭合 A.1 遗留洞**: parked 窗树自由后 legacy 分配可落窗（A.1 未及, A.2a 活窗树自由后必修） |
| A6 | J1 前置计数器（新） | `corten_j1_probe()`（内联双门）挂 find_vma/find_vma_intersection（mm/mmap.c）+ lock_vma_under_rcu（mm/mmap_lock.c）: 窗口域查询计 `j1_probes`, 命中树上 VMA 计 `j1_hits`（J2 负探针: 唯一合法 hits=植入）; corten 内部行走（placement obstacle/punch split/fault tier-2）改走未计量别名 `corten_vma_find()`（mm/mmap.c 定义, #ifdef CORTEN_MM_ARENA 包裹保证 =n nm 零符号）; debugfs arena_stats 新行 j1_probes/j1_hits |

### A.2b（carrier detached VMA）

| # | 位置 | 手术 |
|---|---|---|
| B1 | `corten_arena_carrier_alloc/free()`（新, §2.3 落地） | vm_area_alloc + vma_set_range + vma_set_anonymous + vm_flags_init（RWX=perm 位、MAY 全界、NORESERVE、VM_CORTEN、VM_NOHUGEPAGE）+ vma_start_write + anon_vma_prepare; **绝不 vma_link/attach**（vm_refcnt 恒 0=upstream detached 形态, `vm_area_free()` 的 vma_assert_detached 直接满足）; free=unlink_anon_vmas+vm_area_free; 计数 `carriers` |
| B2 | `corten_arena_anchor_vma()`（新） | 载体 ?: 树 shadow——rmap/PTE-API 消费面的统一锚读取。切换点: fault ctx get_vma、swap_in、user_fault 预分配、unmap_chunk_flags（zap 的 rmap 移除）、mmap mark 路由、mprotect 路由、shrink walk（含 pick 门）、fault_owned tier-1。树-only 读取保持直读 ar->vma: park 的 do_munmap 臂、pool_parkable、punch split、fork piece 扫描、mm_exit skip 判 |
| B3 | `corten_arena_declare_locked(perm, novma)` | 双臂化: novma=auto 形态（无 VMA 校验/shadowize; perm 来自 mmap prot; may=may_full; rflags=0; [C1] 两臂共享）; **anchor 发布先于 xa_store（[FAIL-2]）, register() 的 INV tripwire 读锚配对故再先于 register**; 成功路径补 mmap_region 的记账（vm_stat_account take 形态）; unwind 按 novma 卸锚（carrier_free / unshadow）; **novma 成功尾部补发 perf_event_mmap(carrier)**（保 C25 观察保真, targeted-DECLARE 臂跳过——其 VMA 当年已报过） |
| B4 | `corten_region_register()` | 不再清 carrier（生命周期=描述符; park/reactivate 翻转不churn载体）; INV-MV3 扩充锚配对: 活 ANON 至多一锚（carrier∧vma 同非空=撕裂）、idle⇒无树 shadow（"至多一"非"恰一": A.1 混合态 MV-11 一等公民） |
| B5 | `corten_arena_free()` | 描述符析构收口点释放 carrier（release/mm_exit/pool eject/fork 失败全部覆盖） |
| B6 | `pool_reactivate(perm, novma)` | 纯翻转保留（槽已有载体则零操作）; **无载体槽重武装**（targeted-DECLARE 旧槽被 auto take 复活的混合态, MV-11）: carrier_alloc 失败→-ENOMEM 槽保持 parked（同记账降级合同） |
| B7 | fork PTE 复制**搬家**（ponytail 裁决: 搬） | `corten_arena_fork_mirror()`: 载体 arena 在 register_child（子侧新造 carrier+`anon_vma_fork(ccarrier,pcarrier)`——dup_mmap 同形, copy_page_range 的子侧 rmap 复制走标准机制）后, **对 carrier 对调一次 `copy_page_range(child_carrier, parent_carrier)`**（白名单 #1 形状逐字保持; vma_start_write(parent) 满足其 vma_assert_write_locked(src)）; A.1 显式臂 `fork_copy_window_novma` **整体删除**（150 行）——锚恢复后其手滚 COW/swap/计数已是 copy_present_ptes 的残缺重写（缺 folio_try_dup_anon_rmap_pte/GUP-pinned 拷贝/THP-batch）; skip 判与 F2 子 pmd 门不变（只读树 shadow）; `corten_arena_span_has_vma` 随显式臂删除（唯一用户） |
| B8 | `release_arena_locked()` **混合形状修复**（A.1 遗留缺陷） | 旧二值分支（any_vma→do_munmap / !any_vma→novma 拆）对"carrier 窗+植入洞"漏拆 arena 自身内容（A.1 起 punch+全量 munmap 即漏到 mm 死亡）。新形状: any_vma→do_munmap 成功后**补 novma zap+PT 退休**（植入窗已被 funnel 清空→no-op）; 手工回冲以 carrier 为钥（carrier 窗的账在 reactivate/declare, 从不在树 VMA 上）; do_munmap 失败路径维持 A.1 语义（剥影存活, 载体窗页留待 exit walk） |
| B9 | `corten_arena_mremap_move` | move 目标=直接 novma declare（旧 do_mmap 装 VMA 再 shadowize 的绕路连同 VMA 一起消失）; 失败无 VMA 可回卷 |

### 测试面

| # | 内容 |
|---|---|
| T1 | 新 KUnit `corten_arena_test_auto_validate`: 真值表（白名单形状 0 / PROT_NONE 0 / def_flags VM_LOCKED -EAGAIN / PROT_EXEC -EOPNOTSUPP / RLIMIT_AS 压限 -ENOMEM / 纯函数零副作用） |
| T2 | 新 KUnit `corten_arena_test_carrier_vma`: detached 全断言（树中无/refcnt 0/边界/flag 形状/anon_vma prepared/total_vm 记账/record 锚互斥）+ RELEASE 释放载体+计数闭合 |
| T3 | 新 KUnit `corten_arena_test_carrier_shrink_pick`: 载体窗真 fault 内容→两轮 scan→pick 门放行（skipped 不动）+无 swap 下页 keep 完整（PTE/meta/folio ref）——B1 锚的载体版 |
| T4 | 重写 `vma_free_shrink_pick` 为真实混合态形状（declare→park→declare 侧 reactivate=无锚活窗）——A.1 的 B1 锚在 A.2b 后唯一合法构造 |
| T5 | A.1 系用例去 mkvm 化（pool_attach=零 VMA attach 后预置 VMA 成孤儿）: vma_free_reuse/inv_mv3/fork_vma_free/vma_free_shrink_pick/pool_limit/pool_mode_exit/pool_fork/pool_reuse/auto_attach_release/mremap_route/conc 窗口臂; `fork_seed_mapped` 锚回退到载体（vma_lookup NULL→carrier_of）; 断言 S-4 口径翻转处（mremap move 目标/chunk munmap 后 vma_lookup）改 EXPECT_NULL+注释 |
| T6 | 头文件/接口: auto_attach 增 prot 参、corten_auto_validate/corten_vma_find/corten_j1_* 声明（=n 桩全） |

## 1. 语义变更清单（本片增量）

| 项 | 内容 | 状态 |
|---|---|---|
| S-5（新登记） | MODE 进程 auto/move/mremap 目标映射**从 /proc/maps 消失**（零 VMA 的直接推论; J3 oracle 的 A.2 例外档, V-C 双源渲染收口）; perf 符号面经 perf_event_mmap(carrier) 保持 | 落地 |
| S-5b | map_count 不再随 arena mmap 增长（max_map_count 消耗降为零——改善, 披露） | 落地 |
| S-6（新登记） | MODE 进程 legacy 分配被 Dominion 栅栏限制在窗外（topdown<16T / bottomup>64T）; 窗内 hint -ENOMEM（app 端惯例重试 addr=NULL）| 落地 |
| S-7（新登记） | NOREPLACE 对**活窗**仍 -EEXIST（注册表即占用真相）; 对 parked 窗放行=已 munmap 语义（S-4 同源）; 命中后 [C1] eject 兜底 | 落地 |
| MV-4 修正 | LSM anon mmap 钩 6.18 已由 vm_mmap_pgoff 覆盖——不补调（双触发才是回归）; RLIMIT/mlock/pkey 清单照 SPEC 落地 | 落地 |
| J1 前置 | probes/hits 计数器（V-A.3 全量审计 walker 的地基）; 口径: probes=外部消费面窗域查询数（含 S-1 形状的 miss-leg, 预期非零）; hits=窗域树上命中数（唯一合法形状=植入, 断言纯 workload 下零） | 落地 |

## 2. 验证矩阵（夜间, timegate 后）

【回填】

## 3. 过程缺陷

1. declare novma 臂初版漏 [C1] 判空（两臂应共享）→ review 自捕, 归位共享。
2. reactivate 注释重复/接缝注释断裂（两轮脚本编辑残留）→ 清理, 零行为差。
3. `corten_vma_find` 初版裸全局（=n nm 会现形）→ #ifdef 包裹。
4. A.1 系 KUnit 预置 VMA 在零 VMA attach 下成孤儿（vma_lookup 断言/total_vm 双记/树覆盖假象）→ 全量去 mkvm 化（§0-T5）。
5. release_arena_locked 混合形状（carrier 窗+植入洞）漏拆自身内容——**A.1 遗留缺陷, A.2a 活窗化后升级为常见路径**→ B8 修复（any_vma 成功后补 novma 拆+carrier 钥回冲）。

## 4. 留给 A.3 / V-B 的边界

- J1 前置计数器仅三入口（find_vma/find_vma_intersection/lock_vma_under_rcu）; vma_lookup/find_vma_prev/for_each_vma 迭代族未计量——A.3 全量审计 walker（J1/J2+bpftrace 双口径）收口。
- perf_event_munmap 对 park/novma release 不再发（A.1 起既成; 事件面归 V-C 工具验证统办）。
- file carrier（i_mmap 参与/truncate 门路由化）= V-B 本体, 本片明确不含; CORTEN_FILE_MAPPED 状态仍无生产者。
- GUP-slow 窗域 EFAULT 维持 A.1 边界（V-C corten_gup_probe 返回 carrier——本片已为其备好锚形状）。
- mincore/clear_refs 窗域口径不变（V-C）。

## 5. 证据文件

【回填: 构建日志/KUnit 三件/guest 门/checkpatch/diff sha】

## 6. SPEC 偏差登记（供主 agent/规划者复核）

1. **LSM 补钩改判**: 前提失实（vm_mmap_pgoff 已覆盖 anon）, "补"改为"核实+防双调"; 原清单其余五项照落（corten_auto_validate）。
2. **fork PTE 复制搬家裁决**: 按 SPEC 搬（copy_page_range carrier 对调）, 未按 ponytail 保守保留显式臂——因为锚恢复后显式臂的正确性成本（rmap 复制/AnonExclusive/pin 拷贝/swap 族）恰是重写 copy_present_ptes; 搬家后 M5 语义对拍逐位白得（同一机器码路径, 驱动 vma 换成 carrier）。
3. **分配器 Dominion 栅栏**: SPEC §1.2 的窗界承诺的执行面（A.2a 前由树 VMA 占位隐式成立, A.1/A.2 后需显式）; 涉及 x86 文件（mm/ 外第二改动面, V-C 前例为 fs/proc）。
