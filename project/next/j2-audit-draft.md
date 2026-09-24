# J2 审计报告（find_vma/vma 查找调用面 · 窗口域可达性分类）

- 产出: 调研班 agent（严格只读, 2026-09-22 15:1x）; 主 agent 落盘
- 树: /home/ppw/linux-6.18 @ d40eae59ba76（V-A.0+A.1 已落, A.2 未入库）
- 口径（对齐 MV_VMA_FREE_SPEC §1.2/§1.3）: "窗口域地址" = [0x1000_0000_0000, 0x4000_0000_0000)。
  当前态: **活跃 region 仍有 shadow-VMA**（A.2a 未落）, **parked/空洞窗口零 VMA**（A.1 已落）。
  每个调用点按"今天 parked 地址可达吗 / A.2a 后可达吗"双层评估。
- file:line 均为当前树实测（规格附录行号基于 4ca6ef10, 已有漂移, 本文以现树为准）。

## 审计表

| # | 调用点 file:line | 子系统/入口 | 上下文（锁/RCU） | 分类 | 理由 / 钩子名 | 建议接管切片 |
|---|---|---|---|---|---|---|
| 1 | arch/x86/mm/fault.c:1386 `lock_vma_under_rcu` | 缺页快路径 fallback | 无锁 RCU (mas_walk) | **N** | 活跃窗被 corten_arena_user_fault（fault.c:1362-1382）拦截；**parked/空洞窗 FALLBACK → 以窗口地址做 mas_walk → NULL → MAPERR**。结局正确（S-1 已登记）但每次 park 区 fault 必产生窗口地址树走查，J1 永不归零 | V-A.3：快钩加"MODE∧窗口域∧无活跃 region → 直接 MAPERR"终答 |
| 2 | mm/mmap_lock.c:433/:459 `find_vma`（lock_mm_and_find_vma） | 缺页慢路径 fallback | mmap_read（可升 write） | **N** | 同 #1 的下游：lock_mmap 分支 find_vma(窗口)→NULL→SIGSEGV MAPERR。J1 计数器主要污染源 | V-A.3（随 #1 一并消解） |
| 3 | mm/gup.c:1305 `find_vma`（gup_vma_lookup，__get_user_pages:1426 调用） | GUP-slow（io_uring/9p/vhost/pin） | mmap_read（或 FOLL_UNLOCKABLE） | **N** | 活跃窗：corten_own（gup.c:1234-1272）信任 shadow-VMA 位，可用；**parked 窗：NULL→-EFAULT（errno 与 A.1 前一致，但产生窗口走查）；A.2a 后活跃窗也 NULL→全部 -EFAULT**。J4 主体 | V-C：corten_gup_probe（region 解析+check_vma_flags 仿真，规格 §3.3.2） |
| 4 | mm/gup.c:1415 `vma_lookup`（FOLL_MADV_POPULATE 臂） | MADV_POPULATE via GUP | mmap_read | U（今日） | madvise 路由（madvise.c:1961）对窗口 POPULATE 默认 -EOPNOTSUPP，GUP 不可达；A.2a 后若放行需随 #3 走 V-C 分支 | V-C（跟随 #3） |
| 5 | mm/gup.c:1973/:1975 `find_vma_intersection`（__mm_populate） | mmap(MAP_POPULATE)/mremap populate/mlock | mmap_read | U（今日） | 窗口形状全部上游拦截：auto 路由白名单拒 POPULATE（mmap.c:427→corten_arena_auto_mmap_route）、mremap populate_expand 只对 legacy（route mremap.c:2012 先返回）、mlock 被 mlock.c:645 range_overlaps 拒。A.2a 后需随 V-C 复核 | V-C 复核 |
| 6 | mm/gup.c:2041 `find_vma`（nommu __get_user_pages_locked） | GUP | — | U | CONFIG_MMU=y 折叠 | — |
| 7 | mm/memory.c:6956/:6970 `vma_lookup`+expand_stack（__access_remote_vm） | ptrace PEEK/POKE、/proc/pid/mem、process_vm_readv/writev | mmap_read_killable | **N** | 6956 早期检查：窗口→NULL→expand_stack 失败→**返回 0 字节（短读/短写）**；今天仅 parked 退化，A.2a 后全窗口断；外部调试器/checkpointer 静默失败 | V-C：经 corten_gup_probe 的 MODE 分支（规格明确 fixup_user_fault/process_vm_readv 无独立路径） |
| 8 | mm/memory.c:7081 `vma_lookup`（__copy_remote_vm_str） | BPF probe_read_user_str 远程读 | mmap_read_killable | **N** | 同 #7：窗口→-EFAULT | V-C（同 #7） |
| 9 | fs/proc/task_mmu.c:281-355 `proc_get_vma`/`get_next_vma`/`m_start`（maple 迭代；:307/:352 get_gate_vma） | /proc/pid/maps、smaps、smaps_rollup、numa_maps 枚举 | mmap_read 或 RCU+per-VMA lock | **N** | 单源 maple 游标：窗口条目整段缺失。parked 消失=S-4（已登记）；**活跃窗今天靠 shadow-VMA 渲染，A.2a 后全部消失 → J3 oracle FAIL**。C13 主体 | V-C：双源归并游标（m_start/m_next + corten_region_next） |
| 10 | fs/proc/task_mmu.c:600/:620/:642 `find_vma`（query_vma_find_by_addr） | PROCMAP_QUERY ioctl | mmap_read 或 RCU lock_next_vma | **N** | 窗口查询→-ENOENT；A.2a 后安卓 API（PROCMAP_QUERY）对 JVM 窗口全盲 | V-C（归并游标覆盖 ioctl 解析） |
| 11 | fs/proc/task_mmu.c:2029 `find_vma`（pagemap_pte_hole） | /proc/pid/pagemap | mmap_read（walk） | **N** | 窗口在 walk_page_range 视为洞→全零条目（活跃窗内存被渲染为不存在）；伴随 #12 | V-C（pagemap 双源：region 驱动 PT 走查） |
| 12 | mm/pagewalk.c:495/:511 `find_vma`（walk_page_range_mm） | 通用页走查漏斗（pagemap 等 mm 级调用） | mmap_read（walk_lock 可加 per-VMA） | **N** | mm 级入口以 find_vma(窗口)→NULL→pte_hole 收尾。per-VMA 调用者（smaps/clear_refs/queue_pages）结构性只见委托域；mm 级调用者（pagemap_read）受影响 | V-C（pagemap 走查前置 region 枚举） |
| 13 | mm/mincore.c:241 `vma_lookup`（do_mincore） | mincore(2) | mmap_read | **N** | 窗口→-ENOMEM。A.1 前 parked 有 PROT_NONE VMA → 返回全 0 向量 +0；现 -ENOMEM（**未登记的语义回归**）。活跃窗 A.2a 后同 | V-C（C11/OQ-MV-11：region+PT 直走路由；或登记 -ENOMEM 变更） |
| 14 | mm/mmap.c:466 `find_vma_intersection`（do_mmap MAP_FIXED_NOREPLACE 检查） | mmap NOREPLACE | mmap_write | **N（高危）** | **A.1 挖出的最大洞**：NOREPLACE 携 MAP_FIXED（mmap.c:373）→ classify（corten_arena.c:7647 族）对 NOREPLACE 返回 LEGACY → 路由不打拳；parked 窗无 VMA → :466 返回 NULL（A.1 前 PROT_NONE 预约在此 -EEXIST）→ __mmap_prepare（vma.c:2467）因 vms->vma==NULL 跳过 corten backstop（vma.c:2492）→ **legacy VMA 静默装入 parked 窗**：违反 NOREPLACE 契约 + 破坏 J2 域不变量 + 与 reactivate/后续 punch 冲突（帧表仍指向 parked 描述符） | V-A.3：:466 前加 corten 窗口帧表探测→-EEXIST；并给 __mmap_prepare backstop 补"vms->vma==NULL 但帧在册"分支 |
| 15 | arch/x86/kernel/sys_x86_64.c:145/:194 `find_vma`（hint 检查） | mmap hint 放置 | mmap_write(do_mmap 内) | **N（高危）** | mmap_address_hint_valid（arch/x86/mm/mmap.c:198-204）**接受窗口内 hint**；parked/空洞窗 find_vma→NULL→hint 被采纳→legacy VMA 落入窗口（同 #14 后果）。A.1 前预约 VMA 挡住 hint | V-A.3：placement guard（见钩点清单 P2） |
| 16 | mm/mmap.c:811/:862 `find_vma_prev`（generic_get_unmapped_area[_topdown] hint） | 非 x86 通用 hint 放置 | mmap_write | **N** | 同 #15 的通用形态 | V-A.3（同一 placement guard） |
| 17 | mm/vma.c:2467 `vma_find`（__mmap_prepare） | mmap_region 入口 | mmap_write | R（带洞） | corten backstop vma.c:2492 range_overlaps→-EOPNOTSUPP 已挡"有 VMA 重叠"形状；但其条件 `if (vms->vma)` 使 #14/#15 的零 VMA 形态绕过 | V-A.3（修 #14 时补） |
| 18 | mm/vma.c:1682 `vma_find`（do_vmi_align_munmap） | munmap 三层 | mmap_write | R | corten_arena_munmap_vma_guard（vma.c:1627）+ munmap 路由（mmap.c:1210）+ vma.c:3305 corten_arena_munmap_guard；零 VMA 窗口 for_each 空转=放行，正确（C4） | — |
| 19 | mm/mprotect.c:943 `vma_find`（do_mprotect_pkey walk） | mprotect/pkey | mmap_write | R | 路由 mprotect.c:917（corten_arena_mprotect_route，arena.c:8056）+ :928 range_overlaps 拒 | — |
| 20 | mm/madvise.c:1696 `find_vma_prev`、:1743 `find_vma`（madvise_walk_vmas）、:1644 `lock_vma_under_rcu`（try_vma_read_lock）、:962 `vma_lookup`（madvise_dontneed_free 重查，含 process_madvise） | madvise/process_madvise | mmap_read / RCU+per-VMA lock | **N**（parked/空洞） | 活跃窗：路由 madvise.c:1961（corten_arena_madvise_route，arena.c:8542）全覆盖 → walk 不可达；**parked/空洞窗：dontneed_route 的 lookup_get 不可见 idle（arena.c:8535→lookup 跳 idle）→ 返回 0 走 legacy → find_vma_prev(窗口)→NULL→-ENOMEM**。A.1 前 parked PROT_NONE VMA 使 DONTNEED 返回 0（**未登记的语义回归**）+ J1 命中 | V-A.3：madvise_route 对 `mm->corten_mode ∧ range⊂窗口 ∧ 无活跃 region` 直接终答（0 或 -ENOMEM，登记 S-5） |
| 21 | mm/mremap.c:1933/:1353/:1387 `vma_lookup`（do_mremap/shrink_vma/mremap_to） | mremap | mmap_write | R | corten_arena_mremap_route（mremap.c:2012，arena.c:8268）在 syscall 入口先答，窗口范围（含跨界）非 0 即返；do_mremap 内 lookup 只见 legacy | — |
| 22 | mm/mremap.c:660/:1423 `find_vma_intersection`（realign/vma_expandable） | mremap 内部 | mmap_write | U | 以既有 VMA 的延伸区间为参数；窗口无 VMA 可延伸 | — |
| 23 | mm/msync.c:64/:102/:108 `find_vma`（sys_msync） | msync | mmap_read | **N** | 窗口→-ENOMEM；A.1 前 parked VMA 使返回 0（匿名 no-op）。语义回归未登记；J1 命中 | V-A.3（corten 窗口判定→返回 0，对齐匿名语义） |
| 24 | mm/mempolicy.c:1109 `vma_lookup`（do_get_mempolicy MPOL_F_ADDR） | get_mempolicy | mmap_read | **N** | 窗口→-EFAULT；numactl --addr/库查询失败。未被 :1459/:1888 拒族覆盖 | V-A.3（窗口→返回 task/default policy，或登记 -EFAULT） |
| 25 | mm/mempolicy.c:859 `find_vma`（queue_pages_test_walk） | mbind/migrate_pages 走查 | mmap_write(PGWALK) | U | 参数=委托 VMA 的 vm_end；mbind/do_migrate_pages 已被 mempolicy.c:1459/:1888 range_overlaps 拒 | — |
| 26 | mm/mempolicy.c:1234 `find_vma(mm,0)`（migrate_to_node） | migrate_pages | mmap_read | U | addr=0；移动腿被 :1888 拒 | — |
| 27 | mm/migrate.c:2333 `vma_lookup`（add_folio_for_migration） | move_pages 移动腿 | mmap_read | R | kernel_move_pages:2650 `nodes && range_overlaps` → -EOPNOTSUPP 整体拒 | — |
| 28 | mm/migrate.c:2491 `vma_lookup`（do_pages_stat_array） | move_pages 查询腿（nodes==NULL） | mmap_read | **N** | **无守卫**：窗口页逐个 -EFAULT；numa 工具对 arena 全盲。A.1 前 parked 同样 -EFAULT（行为同），J1 命中 | V-A.3（廉价窗口短路：-ENOENT/EFAULT 不查树）或 V-C |
| 29 | mm/userfaultfd.c:48/:87/:127/:1581/:1590 `find_vma_and_prepare_anon`/`vma_lookup`、:74/:1634 `lock_vma_under_rcu`（uffd_lock_vma/mfill/move） | UFFDIO_COPY/MOVE/REGISTER/REMOVE | RCU+per-VMA / mmap_read | **N-low** | 窗口天然 -ENOENT/-EAGAIN（规格 C12 认可为终态）；但 J1 计数器会被用户态随意打爆（UFFDIO_COPY 目标=窗口） | V-A.3（J1 卫生：mfill/move 入口加 corten 窗口短路 -ENOENT） |
| 30 | mm/pgsize_migration.c:241 `lock_vma_under_rcu`（linker_ctx） | madvise DONTNEED 的 linker 判定 | RCU | **N-low** | MODE 进程 IP 落在窗口（JIT 代码在 arena）时 mas_walk(窗口)→NULL→保守 false；只影响 smaps 仿真输出，无错误路径 | V-A.3（接受+计数）或 V-C |
| 31 | mm/huge_memory.c:4349 `vma_lookup`（split_huge_pages_pid） | debugfs THP split | mmap_read | **N-low** | 窗口→break；debugfs 专用 | V-A.3（短路）或接受 |
| 32 | kernel/sys.c:2255 `find_vma`（prctl PR_SET_MM_*） | prctl 边界设置 | mmap_read+arg_lock | **N-low** | 窗口 addr→-EINVAL（结论合理），J1 命中；需 CAP_SYS_RESOURCE | V-A.3（短路同判） |
| 33 | arch/x86/kernel/cpu/sgx/ioctl.c:218 `find_vma`（__sgx_encl_add_page） | SGX enclave 添加页 | 无锁（后随 GUP） | **N-low** | MODE+SGX 源页在窗口→-EFAULT；冷门组合 | V-A.3（随 #3 GUP 分支自然覆盖） |
| 34 | arch/x86/kernel/shstk.c:357 `find_vma` | shadow stack token 校验 | mmap_read | U | 要求 VM_SHADOW_STACK VMA；窗口白名单恒拒 | — |
| 35 | arch/x86/kernel/uprobes.c:766/:1119、kernel/events/uprobes.c:1307/:2445/:2484 `vma_lookup`/`find_vma` | uprobe 注册/命中/优化 | mmap_write / RCU 投机 / mmap_read | U | 全部要求 file VMA/已注册 uprobe/special tramp；窗口 anon（carrier 不在树）结构性无法承载 uprobe；find_vma(窗口)→NULL→跳过 | — |
| 36 | kernel/futex/core.c:339 `vma_lookup`（__futex_key_to_node，CONFIG_FUTEX_MPOL） | futex NUMA hint | RCU 投机 | **N-low** | 私有 futex 主路径不查 VMA（:618 注记）；仅 MPOL hint：窗口→FUTEX_NO_NODE 优雅默认。JVM 锁在 arena 高频使用但无错误面 | V-A.3（接受+计数） |
| 37 | kernel/trace/trace_output.c:410 `find_vma`（seq_print_user_ip） | trace 用户 IP 符号化 | mmap_read | **N-low** | 窗口 IP→裸地址打印（丢 file+offset）；优雅降级 | V-C（region 查询补符号）或接受 |
| 38 | kernel/bpf/stackmap.c:196 `find_vma`（BPF stack build-id） | BPF 栈回溯 | mmap_read（bpf_mmap_lock） | **N-low** | 窗口 IP→回退 IP 模式；优雅降级 | V-C 或接受 |
| 39 | kernel/bpf/task_iter.c:529-546 `find_vma`（task_vma 迭代） | bpf_iter vma（bpftrace maps 等价） | mmap_read_killable | **N** | 枚举源单树：窗口条目整段缺失（参数多为 prev_vm_end-1，少直接窗口命中）；无错误，纯保真缺口 | V-C（双源迭代器） |
| 40 | kernel/bpf/task_iter.c:777 `find_vma`（bpf_find_vma kfunc） | BPF 程序按址查 VMA | mmap_read_trylock | **N-low** | 窗口→-ENOENT | V-C 或接受 |
| 41 | kernel/bpf/task_iter.c:932 `lock_vma_under_rcu`（bpf_iter_task_vma_find_next） | bpf_iter 新接口 | RCU | U | start 参数恒来自树上真实 VMA 的 vm_start（:923 vma_next 产物） | — |
| 42 | mm/khugepaged.c:910 `find_vma`（hugepage_vma_revalidate）、:1504 `vma_lookup`（collapse_pte_mapped_thp） | khugepaged/THP collapse | mmap_read | U | 地址源=khugepaged 自身树迭代/MADV_COLLAPSE（madvise 路由默认拒）；窗口零 VMA+VM_NOHUGEPAGE（C20 结构性排除） | — |
| 43 | mm/ksm.c:712 `vma_lookup`（find_mergeable_vma） | KSM | mmap_read | U | VM_MERGEABLE 需 MADV_MERGEABLE（madvise 路由默认拒 :8608） | — |
| 44 | mm/mmap.c:172/:196 `vma_find`（sys_brk） | brk | mmap_write | U | brk 在低段委托域（V-E.1），结构到不了窗口；shrink 走 do_vmi_align_munmap 有 guard | — |
| 45 | mm/mmap.c:1074/:1097/:1142 `find_vma_prev`/`find_vma`（find_extend_vma_locked/expand_stack） | 栈扩展 | mmap_read/write | U | 要求 VM_GROWSDOWN；窗口无此位。仅作为 #7 失败尾巴产生 J1 噪声 | 随 #7 消解 |
| 46 | mm/mmap.c:1261/:1298 `vma_lookup`（remap_file_pages） | 废弃 syscall | mmap_read/write | U | 要求 VM_SHARED file VMA | — |
| 47 | mm/vma.c:3152（expand_upwards）、:3335（insert_vm_struct） | 栈增长/内核 VMA 插入 | mmap_write | U | GROWSUP/内核固定地址特映，窗口非目标 | — |
| 48 | mm/vma.c:1942 `find_vma_prev`（mmap_region/vma_merge 准备） | mmap 正面 | mmap_write | U（今日） | 只处理路由后的 legacy 放置；窗口放置仅经 #14/#15 洞可达 | 随 P2 修复 |
| 49 | mm/memory.c:396/:416/:2121 `mas_find`（free_pgtables/unmap_vmas） | exit 拆表 | fullmm gather | R | exit_mmap:1408 先 corten_arena_mm_exit 排干窗口；树循环只见委托+植入（V-D 前提成立） | V-D 收口自拆 |
| 50 | mm/rmap.c:1895-1911/:2351-2357 守卫族、mm/oom_kill.c:577-581、mm/memory-failure.c:1595 | rmap/oom/hwpoison | 各自 | R | corten_rmap_unmap_one/corten_oom_reap_skip_vma/corten_arena_hwpoison_check；walk 均以树/folio 驱动，窗口 VMA 不存在→不触 | — |
| 51 | mm/swapfile.c unuse_vma/unuse_mm（:2430 起，for_each VMA 族） | swapoff 换回 | mmap_read_lock | **N** | S-3 已登记：A.1 后 parked 窗 swap 条目对 unuse 盲（A.1 前 unuse 经预约 VMA 走到但无 PTE，行为等价）；A.2a 后活跃窗也盲→换入只能走 fault 慢车道 | V-D 复测收口（S-3/OQ-MV-6） |
| 52 | mm/corten_arena.c:1082/:2288/:2421/:2481/:2614/:3667/:4007/:6730/:7354/:7371 | corten 自身（release/VA 杂志障碍扫描/fork 镜像/fault_owned tier2/punch） | mmap_write 或 RCU | R | corten 内部对自管范围的设计性走查（J1 计数器应豁免 corten 自身） | — |
| 53 | mm/mmap_lock.c:224 `lock_vma_under_rcu` 定义本体 | 共享原语 | RCU | （消费方单列） | 消费方：#1 RCU fault、#20 madvise、#29 uffd、#30 pgsize、#41 bpf iter | 见各自行 |
| 54 | fs/proc/task_mmu.c:1401/:3537、arch/x86/entry/vsyscall/vsyscall_64.c:305/318、mm/gup.c:1076 `get_gate_vma` | vsyscall 门页 | 各自 | U | 固定地址 vsyscall 页，与窗口无关 | — |
| 55 | mm/nommu.c 全部、mm/execmem.c mas_*、kernel/futex/core.c:616（注释） | nommu/模块文本 | — | U | 配置不可达/非进程窗口 | — |

## N 类统计与风险小结

**N 类总数：27**（其中 N-high 4 条：#14、#15、#16、#3；N 主要 12 条；N-low 9 条；V-D 1 条 #51；另 #1/#2 为 J1 残留）。R 类 8 组，U 类 21 组。

按子系统：
- **放置面（mmap）**：#14/#15/#16/#17——同一根因（A.1 撤掉预约 VMA 后，"窗口=已占用"这一事实只剩帧表知道，而放置路径不问帧表）。后果是 **dominion 不变量被静默破坏 + NOREPLACE 契约违约**，后续 punch/reactivate 会与乱入 VMA 冲突（corruption 级），是全部 N 中唯一能造成内核侧错误行为的族。
- **外部数据面（GUP/remote）**：#3/#7/#8——今天只伤 parked，A.2a 后 process_vm_readv/ptrace//proc/pid/mem/io_uring pin 对全部窗口 -EFAULT 或 0 字节。规格已规划（V-C corten_gup_probe），本审计确认无独立旁路。
- **观察面（proc/BPF/trace）**：#9/#10/#11/#12/#37/#38/#39/#40——渲染缺失型，无错误返回，J3 oracle 的主体。
- **syscall 杂项**：#13/#20/#23/#24/#28/#31/#32/#33/#36——多为"-ENOMEM/-EFAULT 型"语义回归，其中 #13(mincore)/#20(madvise parked)/#23(msync) 是 A.1 直接造成且**未进 S-1..S-4 登记集**的三条（建议补 S-5）。
- **J1 卫生**：#1/#2/#29/#30/#45——结局正确但永久污染 J1 计数器，会让"计数>0 即 FAIL"的负向探针失去判别力，必须在 V-A.3 一并短路。

**最危险 3 条**：
1. **#14 MAP_FIXED_NOREPLACE 装入 parked 窗**（mm/mmap.c:466 + vma.c:2492 backstop 的 `if (vms->vma)` 条件洞）：唯一能造成内核错误行为（NOREPLACE 契约违约、J2 不变量破、与 reactivate/punch 冲突）的路径，且用户态一次 syscall 即可触发。
2. **#15/#16 hint 放置入窗**（sys_x86_64.c:145/194 + mmap.c:811/862）：与 #14 同族同后果，触发面更宽（任意非 FIXED hint mmap）。
3. **#3/#7/#8 GUP-slow 与 __access_remote_vm**：A.2a 落地瞬间全部窗口外部访问（process_vm_*/ptrace//proc/pid/mem/9p/io_uring 注册 pin）静默归零——若无 V-C 分支先行，A.2a 不可独立合入（建议把"corten_gup_probe 就绪"列为 A.2a 的硬依赖）。

## J2 审计钩（V-A.3 应加）挂点清单

计数器/断言函数建议：
- `corten_audit_vma_lookup_hit(mm, addr)`（双门：corten_enabled_static && mm->corten_mode && 窗口判定）挂：
  1. `find_vma`（mm/mmap.c:1011 定义体首）
  2. `find_vma_intersection`（mm/mmap.c:992）
  3. `find_vma_prev`（mm/mmap.c:1034）
  4. `lock_vma_under_rcu`（mm/mmap_lock.c:224，mas_walk 前）
  5. `find_vma_and_prepare_anon`（mm/userfaultfd.c:42）
  （corten_arena.c 自身调用点用 per-cpu 标记豁免，避免自触发）
- `corten_audit_j2_walk(mm)`（INV-MV2：树上每个 VMA ⊄ 窗口 ∨ 登记植入）触发点：`corten_arena_mm_exit`（arena.c:1894）头部、park_locked/pool_take/reactivate 尾部、munmap_route/madvise_route/mremap_route 返回前、fork_commit 尾部、debugfs 手动触发。
- 修复型守卫（非计数）：
  - P1 `corten_arena_placement_noreplace_guard`：mm/mmap.c:465-468（窗口帧表探测→-EEXIST）；
  - P2 `corten_arena_placement_guard(mm, addr, len)`：mm/mmap.c:461 `__get_unmapped_area` 返回后 + arch_get_unmapped_area hint 臂（sys_x86_64.c:145/194、mmap.c:811/862）——落点在窗且非 auto_arena → 拒绝重放；
  - P3 `__mmap_prepare` backstop 补 `vms->vma==NULL && 帧在册` 分支（vma.c:2467-2495）；
  - P4 reactivate/pool_take 防御断言"窗口无外来 VMA"（替代退役的 vma_lookup 校验）。
- 观测计数器：`corten_gup_window_miss`（gup_vma_lookup 返回 NULL 且窗口）、`corten_remote_access_window_short`（__access_remote_vm 窗口早退）、`corten_uffd_window_reject`（mfill/move 窗口短路）、`corten_fault_fallback_window`（user_fault 对窗口地址 FALLBACK 的次数——#1/#2 的量化）。

**一句话结论**：A.1 的"预约 VMA 兜底退役"在 fault/GUP/proc 三面已被规格切片覆盖（V-C/V-D），但**放置面（NOREPLACE+hint）出现了一个规格未预见的静默破洞**，且 **mincore/madvise-parked/msync 三条 -ENOMEM 型语义回归未登记**——这三件事应作为 V-A.3 的最优先增量。
