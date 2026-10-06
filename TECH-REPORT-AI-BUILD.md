# CortenMM → Linux 6.18：AI 多会话移植实录与技术评估

CortenMM（SOSP'25）主张把 Linux 内存管理热路径上的 VMA 树与 mmap_lock 整层拆掉，换成"页表页锁协议 + per-PTE 元数据数组 + 事务接口"。本项目把这套思想 retrofit 进 android17-6.18，2026-09-12 晚启动、10-05 收官，墙钟 23 天（约 552 小时）。执行者全部是 AI 会话：主控会话 1 个（r00-r08）、Claude Code 接力会话 1 个（r09），每个开发切片派 dev/review/verify subagent 并行。环境为 QEMU x86_64（8 vCPU / 4G / KVM），与论文 384 核只做方向性对照（D4）。文中数字全部标注出处：状态账 `project/STATE.md`（23 天全量决策与事故）、正式报告 `project/REPORT.md` v1.6（性能/正确性权威数字）、各片 dev report 在 `project/next/`、原始日志在 `results/`。素材冲突处取 STATE/REPORT 口径并注明。

## 0. 结论先行

第一个问题：VMA 移除效果是真是假。真，且可复核，但口径要说精确。MODE 进程的窗口域 [16T, 64T) 实现了零 VMA：W-2 之后的判据是 MODE mm 的 vm_area_struct 分配恒 0，当前树 `mm/corten_arena.c` 中 vm_area_alloc 调用实查为零；窗口域树走查在 MV3.d 全系统 7216 秒长跑里累计 69001 次、违例 0（J2），find_vma 族五钩点探针累计 617 次、非法命中 0（J1）——这两个 live 读数没有归档原件（SSH 取数失败，见 §4 事故），报告判据链以归档同形读数（327/0、224/0）加结构论证为主，live 值降为旁证，出处文件写明了这一保留。全树字面归零没有做，也不在账的承诺内：动态多段 ELF 负载下 tree_entries=5，全部落在白名单桶（主栈 1、vdso/vvar/vclock 3、brk 堆 1）；非零 hint 装载的库段每进程约 9~70 条留树待裁决。D28"字面树空"口径在 vdso（架构汇编直取，special mremap/timens 钩子无 region 对应物）与主栈（GROWSDOWN 增长臂 W-3b 未做）两处被明确排除（`project/next/mv3e-dev-report.md` §1.1 桶表）。

第二个问题：省了多少代码。没有省，净增 +10,806 行生产代码。W-6 终账（J6，`loc-final.txt`）：被替代的 VMA 层四件（mmap.c/vma.c/mmap_lock.c/maple_tree.c）13,408 行（vanilla 12,955，路由钩净 +453）；corten 生产 23,761 行 + 测试 20,573 行 = 44,334 行。删除账清单已建成：五组 24 项逐条 file:line、死代码判据、风险评级，配裁剪 PR-0..4 序列，每枚验收门是 j1_hits/j2_violations/白名单桶 delta 恒零加全套电池——但清单只登边界不登数，实际删除等裁决排期。"每一枚窗口域树走查的产物只可能是 NULL 或登记过的 implant"是实测判据；"已删"是尚未发生的事。

第三个问题：mmap_lock 竞争还有没有。MODE 窗口热路径上结构性没有了，锁本身没有从内核消失。三层实测：churn 4t 压测运行中 367K 个 perf 样本，find_vma/mmap_lock 六符号宽口径零命中（M3 头条，`results/r03/final-smoke/m3-verdict-final.md`）；G2 trace 实证池命中把每次操作的 mmap_lock 写获取从 2.02 次砍到 0.27 次（7.6 倍），基线侧 maple tree、kmem_cache、rcu_preempt 的 VMA 机器从每 op 路径上消失；mmbench unmap-virt 低竞争五轮固化 +1142% / +2583%（20/20 配对腿全正号）。当前树 `mm/corten_arena.c` 对 mmap_write_lock 的全部引用只有 14 处（实际调用 10 处，其余为注释），落在进程生命周期（mode_enter/exit、mm_exit、exit_walk）、prctl declare/release、池释放和 mremap-move 这类冷路径——mmap/munmap/mprotect/madvise 的窗口操作零触碰。两点不粉饰：非 MODE 进程照旧走完整 VMA 层；MODE 侧 unmap-virt 的 T0 臂仍余 52% 的 osq/rwsem 旋等，竞争消除靠关键区缩短与树从 per-op 路径退场，不靠发明无锁结构（`project/REPORT.md` §4.4-G2）。

## 1. 这个 idea 为什么"疯狂"

### 1.1 前置知识：VMA 树和 mmap_lock 在管什么

Linux 给用户态地址空间记账的单位是 vm_area_struct：一段连续虚拟区间的权限、映射对象与文件偏移，全部区间挂在 per-mm 一棵 maple tree 上，查找走 find_vma 族。结构性修改（mmap/munmap/mprotect/madvise/brk）串行在 mmap_lock 写端，读端（GUP 慢路径、rmap 走查、/proc 枚举）拿读锁。6.18 的 PER_VMA_LOCK 已经把纯缺页从读锁上摘走大半——M1 基线的 30 秒缺页 trace 里 page_fault_user 超过一百万次而 mmap_lock 事件只有 50 次——所以"缺页慢"从来不是这条锁的原罪。真正的瓶颈是写端：任何一次树结构修改与任何一次树结构读取全局互斥，fork 的 dup_mmap、munmap 的 split/merge、mprotect 的区间改写、/proc/maps 的遍历全排在同一条 rwsem 上。多线程进程 churn mmap/munmap 的形状下，mmbench BASE 臂量到的天花板就在这里。

### 1.2 CortenMM 的三个主张

论文（`project/docs/PAPER_SPEC.md` 为唯一权威解读）给出三件事：其一，事务化热路径——每张页表页配一个描述符（rwlock 加 per-PTE metadata array），fault/map/mark/unmap/COW 全部在 covering 写锁下走事务接口，元数据是唯一真源，页表只是硬件缓存；其二，VMA-free 运行——树与锁从热路径上消失；其三，性能收益——论文原型（独立 OS）拿到量级提升，部分负载 26×。这个项目只承诺方向性对照，不承诺复现量级（D4，REPORT 内嵌声明）。

### 1.3 为什么 retrofit 进 6.18 是"疯狂"的

retrofit 的约束是树还在、锁还在、每个消费者一个都不能死。D12 实测 89 个 mm/*.c 文件依赖 vma；换成消费者视角更直白：rmap 系（try_to_unmap 沿 anon_vma 逐 VMA 裸写 PTE、oom_reaper、hwpoison、migration）、fork（dup_mmap 逐 VMA 复制加 copy_page_range）、GUP（check_vma_flags 沿 VMA 走，fast 路径按 PTE 直查）、proc 族（maps/smaps/pagemap/numa_maps/PROCMAP_QUERY）、swap（unuse_pte、swapoff 全量读回）、mprotect/madvise/mremap 漏斗、bpf_iter/task_vma、process_vm_*/ptrace 的远程访问面——每个调用点的第一假设都是"给我一个 vma"。把一类地址从树里拿走，等于把每个消费者对"树里有它"的假设逐个证伪一遍：漏掉一个就是静默数据损坏，或者一个可触发的 UAF。23 天里光是在 GUP 一条线上，就先后补过探针应答（V-C）、remote wrapper 契约（MV3.b）、FOLL_GET 引用配平（MV3.b）、get_futex_key 取键臂（W-3.2）、check_vma_flags 的 pending-perm 语义（W-3fix）五处——这只是"树外页被既有代码摸到"的众多入口之一。

## 2. 前后架构对比

### 2.1 改造前

```
      用户态系统调用                        内核隐式消费者
  mmap / munmap / mprotect /          page fault(慢门) / GUP /
  madvise / brk / mremap              rmap 走查 / fork / /proc / swap
        |                                    |
        v                                    v
  +----------------------------+----------------------------+
  | mmap_lock  (per-mm rwsem)                               |
  | 写端: 全部结构性修改, 全局串行                          |
  +----------------------------+----------------------------+
                               | 读写都要过锁 (缺页快门经 PER_VMA_LOCK 已绕开读端)
                               v
  +----------------------------+----------------------------+
  | VMA 树 = maple tree (per-mm 一棵)                       |
  | find_vma / split / merge -- 区间唯一真源                |
  +----------------------------+----------------------------+
                               | 一切消费从树开始
         +-------------+-------+--------+--------------+
         v             v                v              v
         page fault    GUP/rmap         fork           /proc 与 swap
         慢门沿树走查  ttu/oom_reaper/  dup_mmap       maps/smaps 逐 VMA
                       hwpoison 逐 VMA  逐 VMA 复制    渲染; unuse_pte 逐
                       裸写+守卫        + copy PTE     VMA 扫描换回
```

### 2.2 改造后

```
                    MODE 进程 (corten=on)
  +-------------------------------+--------------------------------+
  | 窗口域 [16T, 64T)              | 委托域 (白名单住户, 合同内留树) |
  | 匿名/私有文件映射全生命周期    | heap(brk) / 主栈(GROWSDOWN)     |
  | maple 树住户 = 0               | vdso/vvar/vclock (special)      |
  | [J1] 五钩点探针: find_vma /    | 非零 hint 装载的库段 (wl_file,  |
  | intersection / prev /          | ~9-70 条/进程, 裁决件)          |
  | lock_vma_under_rcu / uffd      | 照走 legacy 路径与漏斗          |
  | -- 对窗口域查询恒 NULL/豁免    |                                 |
  +-------------------------------+--------------------------------+
                  | 事务接口 (mm/corten.c + corten_arena.c)
                  v
  +----------------------------------------------------------------+
  | per-PTE metadata array = 唯一真源; PT 页描述符 rwlock           |
  | 事务接口: lock_range/query/map/mark/unmap (covering 写锁原子)   |
  | per-cpu VA 杂志 + 常驻池 (park/reactivate, 命中 99.99%+)        |
  +----------------------------------------------------------------+
  | 原生 rmap / 换出驱动 (W-1 起, 不经 VMA):                        |
  |   folio_add/remove_anon_rmap_novma + per-inode file registry    |
  |   匿名换出: ttu 守卫翻转 -> 驱动 scan -> swap-out 直驱          |
  +----------------------------------------------------------------+
        | 消费者改道 (窗口域)
        +-- fault:    do_user_addr_fault 快门 + handle_mm_fault 慢门
        |             -> 事务查询 -> 装页 (FRESH 门/KEEP_PERM)
        +-- GUP:      gup.c:1450 corten 臂 (探针+follow); gup_fast 零改动透明
        +-- fork:     fork_begin 冻结窗 -> copy_page_range 照常
        |             -> fork_commit 元数据镜像 (树零操作)
        +-- swap:     换出=驱动事务; 换入=swap_in 三臂(直读/缓存pull/异步等待)
        +-- /proc:    双源归并游标 (registry 行 + 树行;
        |             maps/smaps/pagemap/PROCMAP_QUERY/numa_maps)
        +-- mmap_lock: 窗口操作零触碰; 剩余 10 处写锁调用全在
                    进程生命周期/prctl/池释放/mremap-move 冷路径
```

### 2.3 数据通路对照表

| 操作 | 改造前（vanilla） | 改造后（MODE 窗口域） |
|---|---|---|
| mmap | mmap_lock 写端 → maple 树插 VMA → vma_merge | addr==0 匿名/私有文件走路由（auto/explicit_region_route）→ declare 事务写元数据+帧表，树零操作；显式地址/SHARED/非零 hint 留 legacy 漏斗 |
| munmap | 写端 → split/merge VMA → unmap_vmas → free_pgtables | munmap 路由 → unmap_chunk 事务（zap+元数据复位）→ PT 域退役；guard 网兜住内部调用者 |
| mprotect | 写端 → vma_modify/split | mprotect 路由 → 事务提交 perm 进槽位；帧命中但字节不重叠的邻接操作经 route_hit 判定放行 legacy |
| madvise | 写端 → 逐 VMA behavior 遍历 | madvise 路由：DONTNEED 走 unmap 事务，hints/WILLNEED 计数应答，其余按语义拒绝或放行 |
| fork | dup_mmap 逐 VMA 复制 + copy_page_range | fork_begin 冻结窗 → copy_page_range 照常（wrprotect=白名单 glue）→ fork_commit 元数据镜像；swept 进程 EXIT 拒绝 -EBUSY |
| swap out | vmscan LRU → ttu 沿 rmap 逐 VMA（读锁） | 原生驱动 scan→swap-out 全事务（ttu 匿名守卫翻转直驱；512MiB 全量 131072 页实证） |
| swap in | do_swap_page → unuse_pte（swapoff） | corten_arena_swap_in 三臂：SWP_SYNCHRONOUS 直读 / 缓存滞留 pull / 真盘异步等待+锁回取 |
| GUP | check_vma_flags 沿 VMA；fast 按 PTE 直查 | gup_fast 零改动（窗口 PTE 对其透明）；slow 走 __get_user_pages 三路显式流，窗口域进 corten 臂探针应答 |
| /proc/maps | seq_file 逐 VMA .show 渲染 | 双源归并游标：registry 行（corten_row token）与树行归并，maps/smaps/pagemap/PROCMAP_QUERY/numa_maps 同一机械 |

## 3. benchmark 与 harness

### 3.1 harness 全景

| harness | 测什么 | 在哪/怎么跑 | 判据 | 最近实测 |
|---|---|---|---|---|
| mmbench 微基准 | mmap/unmap/unmap-virt/mmap-pf/pf 各格吞吐 | bench/mmbench；guest 端必须用动态口径 mmbench_dyn（sha256 38304f06…，meta 记档——静态链接吃不进 LD_PRELOAD 的教训见 RQ1） | G1：低竞争 t4&t8 两格同时 ≥+10% × ≥2 项；同 boot ABAB 双臂三遍中位 | unmap-virt +1142/+2583%（五轮固化）；mmap-pf 税 -84.3%→-63.2%（D35 后口径）→ **w3fix6 树刷新 t4 -33.3% / t8 -61.5%**（10-06, ×3 median） |
| lmbench lat_proc | fork/fork+exec/shell 进程生命周期微秒 | guest 系统自带 lat_proc，同 boot 双臂 ×3 中位 | G5：MODE ≤+30% | fork -10.65% / +3.77% / +0.52%（修复前 +516/+172/+354） |
| JThreadBench | JVM 2000 线程生命周期与 GUP 面 | bench/apps，guest ×3 | rc=0 且零 ClassFormatError | 全代 rc=0（run5 及以后各班独立复证） |
| metis/dedup/psearchy 等价件 | map-reduce 词频 / malloc churn 流水线 / 倒排索引 | bench/apps（checksum 跨机一致）；metis 带 fork-probe | rc=0 + checksum 双臂同值 | metis 65073 词 checksum 2d383eeed4ceb73b 与无 hook 跑同值；dedup tcmalloc 六 boot +8.7~+19.2% |
| smoke 契约件 | 26 步内核契约断言（FRESH/CHUNK/park/S-1/S-4/植入/EXIT 语义） | share/t0dod/mode-smoke（v2 sha 37df16d7），每片入库必跑 | 26/26 + SMOKE-DRIVER PASS | W-7 至 MV3 各代 26/26 双形态 |
| J1-J7 live 断言 | 窗口域零 VMA 的七判据终表 | 内核五钩点探针（mmap.c:1110/1152/1182、mmap_lock.c:240、userfaultfd.c:56）+ INV-MV2 走查器 + 九类白名单分类器 | J1 严格零 / J2 白名单零违例 / J3 双源 / J4 树归零 / J5 零改动回归 / J6 LoC / J7 残值 | MV3.d 长跑 j1 617/0、j2 69001/0（live 转述）；归档同形 327/0、224/0 |
| audit_gate | 上行判据的一站式读出 | debugfs：gate_pass、j1/j2/wl 家族计数器 | gate_pass==1 ∧ j1_hits==0 ∧ j2_violations==0 ∧ j2_stale==0 | W-7 终代 gate_pass=1，skip_declare 288→0，tree_entries=5 全白名单 |
| J3 oracle | /proc 渲染双源正确性 | bench/share oracle 驱动件 + 跨内核对拍 cmp_j3.sh | rc=0 + procmap-first 逐字节一致（布局差异入 ledger） | W-6 修复后全绿；与 A.1 基线差异恰 2 行、全部 ledger 化 |
| syzkaller | syscall fuzz 内存安全 | host syz-manager，KCOV+KASAN 内核，corten=on + 手写 PR_CORTEN prctl 描述 | T2 DoD：连续两夜无可复现内存安全 crash | 两轮 453 万 exec（on 面 311 万）零 KASAN/BUG/WARNING，6 崩溃目录全环境类 |
| LTP | 全系统 syscall 矩阵 | MV3.d 电池：guest 源码编译+安装+运行 | =on 世界与 =off 基线行为面对齐，零 panic 零 corruption | P1-off：PASS=98/FAIL=11（基线既有面）；P2-on 7216 秒全程零 panic |
| KUnit | 内核不变量锚 | mm/corten*_test.c 三套件；无盘 qemu `-append "kunit.filter_glob=corten*"`，on×2+off，lockdep/DAS 变体 | 0 fail + skip 对账精确 | 锚 157（MV2 终代）；MV3.c-feat 单轮 pass 总数 194（24+136+34） |

### 3.2 按研究问题组织的结果

#### RQ1：热路径提升真假

判定史本身先讲：首跑 0/4（present-RO 缺陷族遮蔽）→ run2 的 mmbench 读数全部作废（静态链接，LD_PRELOAD 从未进入其 mm，"T0 臂"实为 legacy-vs-legacy 慢漂移，D1 口径更正）→ run3 换动态口径 1/4 → perf1 修掉 TLB 风暴后 run4 首次 2/4 → 五轮 ABAB 固化 MET → run5 终验维持。固化方法：同一 boot 内 ABAB 交错消除构建间漂移，seed 公式（20260913+b×1009+c×97+t×7+k）配对双臂，每格 5 轮，有效性断言全过（109/109 腿 rc=0、池命中 99.999%、seed 配对 50/50）。终读数：unmap-virt +1142.3/+2583.0%，20/20 配对腿无一正号例外；unmap +45.4/+142.0；mmap 全族 +141.3/+251.4（`results/r07/g1-final.md`）。机制归因被 trace 钉死：池命中把写获取从 2.02 次/op 砍到 0.27 次/op，perf1 修掉的自持 TLB 风暴（ftrace 实测 MODE 侧 3 秒窗 82,942 次 tlb_flush 事件对基线 95 次）是更早的翻盘前提。

#### RQ2：mmap_lock 竞争消除了吗

热路径证据在 §0 第三问已给全（367K 样本零命中 + 写获取 2.02→0.27 + 14 处冷路径清单）。跨进程干扰面的证据是同一组读数的反面：窗口操作不再进入任何共享写队列，churn 压测中基线侧的 VMA 机器（maple tree/kmem_cache/rcu_preempt）从 per-op 路径上消失。MV3.b 的 journald 事故从另一个方向证明了锁的生命周期正确性仍然要命——一个 IS_ERR 短答臂漏放一次 mmap_read，就冻结了 PID1 并连锁停摆整个 systemd（§4 案例二同班）。

#### RQ3：无损吗

零改动回归集贯穿全项目：JThreadBench 2000 线程 rc=0、metis_eq checksum 跨臂三方同值、dedup 双档、psearchy、kselftests fail-set 与基线一致、strace 55 对零新错误类（仅 EAGAIN/ETIMEDOUT/ESRCH 竞争采样噪声）。fuzz 两轮 453 万次零内存安全崩溃（首轮 corten=off 面的覆盖缺口在 v1.2 如实缩限并撤回过一条误归因旁证）。锁面：PROVE_LOCKING 全家族零 splat、DEBUG_ATOMIC_SLEEP 首开抓出 zap 路径 47 处原子内睡眠修到 0、KCSAN 短跑零 data race。全系统：MV3.d 把 corten_mode_default=on 翻到整个世界跑 7216 秒，LTP 全编译安装运行、systemd 全栈，零 panic 零 corruption，dmesg 唯一签名是 75 笔 pgtables 计数噪声（登记容差）。

#### RQ4：性能地板在哪里

mmap-pf（映射区缺页）是全项目唯一持续负向的格子，它的读数史值得完整交代。t5final 时代固化在 -17.3/-22.3%（五轮全负，非方差），机制归因为论文 per-fault 事务语义的裸成本（每 fault 一事务 ≤11% + take/park 簿记 3-5%，profile 两臂同形）。D35 判定这组基线数字已过时：W-2/W-7/M-V 各轮累计把这条通道成本推高，本树单进程起点实测 -84.3%（逐轮 bisect 未做，登记独立小片）。批 mark 第一刀（warm park：re-park 不再退役 PT 域，消掉每 op 第二次 flush 与下轮重建）把税收到 -63.2%，配对中位改善 +134.6%（t4）/ +135.4%（t8）（`project/next/mv3cfeat-dev-report.md` §2.3）。**10-06 刷新**：脏域有界化已落地（W-3fix5，`corten_unmap()` 两循环 O(1) 未覆盖拒绝；park PTE 级全幅走查经裁决有意保留）；W-3fix6 树实测税 **t4 -33.3% / t8 -61.5%**（mmpf stock vs mode ×3 median，`project/results/r07/w3fix7/bench-base/`）——t4 较 -63.2% 显著收窄，逐刀归因不预支（bisect 未跑）。余两刀：per-fault 簿记批化（+~1.4µs/页归因；w3fix7 arena fill 温快路径片在制）、地板演化 bisect（台账 #10）。

## 4. AI 是怎么一步步实现的

### 4.1 执行模式

人类只下达架构级指令，共三条主线：D20（09-21）"移除整个 VMA 层"；D28（09-24）"说要完全移除就是要完全移除"；D29（09-24）终局目标两定型。另有两条协议指令（D14/D21：解除时间窗、撤销日夜双窗改持续工作制）。主控会话把每条指令拆成切片，每片走固定协议链：主会话写 dev-brief 任务书（附默认裁决条款）→ dev agent 在隔离 worktree 开发 → review agent 只读复审（PASS/FAIL 加逐条 findings，有真实否决权）→ maintainer 收口入库（checkpatch --strict 零警告、主树应用后与 worktree cmp 字节全等、打 tag、green.txt 台账）。gate 不绿不入库：=y 零新增警告、KUnit 三套件 on×2+off、=n 编译折叠后 nm 零符号、lockdep/DAS 变体、guest 电池（smoke 26/26、metis checksum 同基准、J1/J2 严格门）、bzImage sha256 归档。单写者锁（run/lock）保证同一时刻只有一个会话动树；每片入库即随手 push（D31），决策编号 D1-D35 逐条落 STATE.md。

### 4.2 三条指令与它们引发的执行链

D20 原文（STATE 节录）："**移除整个 VMA 层**——从 D1 的 opt-in shadow-VMA 升级为论文完整形态：MODE 进程零 VMA 运行（maple tree 空，全部内存经 corten metadata/事务）……无时间限制持续工作直到完成，沿途 commit+push。" 执行链：主会话先批 D20-a 诚实化口径（保留 detached carrier 作 rmap 载体、划分窗口/委托双域），落 MV_VMA_FREE_SPEC，随后 36 小时连落 12 切片（A.0 region record 到 V-E brk 裁决），tag corten-mv-complete——期间 dev agent 死于限额一次、gate 五轮迭代一次、oracle 首战揪出渲染死码一次。

D28 原文："**说要完全移除 VMA 就是要完全移除**——字面完全移除取代 D20-a 诚实化口径：委托域白名单与 detached carrier 两处保留均不接受，终态 = MODE 进程 maple 树条目数 0。" 一句话推翻了两天前的口径，直接产生 W 系列：W-1 原生 rmap 七提交（ttu 匿名守卫翻转直驱 131072 页实证）→ W-2 carrier 消灭（vm_area_alloc 恒 0）→ W-3 委托域迁移+GUP 重构 → W-4 入场扫入 → W-5 植入消灭 → W-6 终判据（首测 4 PASS/3 FAIL）→ W-6b/W-7 修正与 multi-record registry，修复轮 65+ 提交，tag corten-mv2-complete。

D29 原文（目标②）："**对所有应用程序无损完整接管**——不再是 LD_PRELOAD 按进程自愿进场，而是 corten=on 时全部进程自动接管（exec/mm 创建即 MODE，含 static/suid/系统服务），全 syscall 语义无损。" 执行链即 MV3 五片：a 默认进场（execve 换 mm 即 MODE，exec_default_enters=153 实证）、b journald 远程访问面双层根因修复 + corruption 族定罪、c exec 镜像收编 + warm park、d 全系统电池 7216 秒、e 删除账清单与闭环判定。

### 4.3 修复迭代实录（四个案例）

#### 案例一：退出走查终段帧退休泄漏（W-3fix，mm_exit 路径）

症状：pgtleak 探针族实测每次 MODE 进程退出一条 pgtables_bytes BUG（4096/8192B）——V-D 曾把 B-2 归零的账被 W 系列打回。定位：heap region 跨帧形状一个 printf-only 程序即可复现，全部非帧对齐 end 的窗口同形。根因：exit walk 相位 A 的终段 run 以 arena->end（字节粒度）收尾，free_ptes_span 的"整帧才退役"守卫跳过末帧，PTE 页搁浅。修复：zap 保持记录跨度，退休伸展到帧界（末帧已验证 walkable：无树 VMA 且注册表槽位指向本 arena）。验证：全部探针形态（heap-only/auto 窗口/混合）dmesg 泄漏=0；KUnit 锚 brk_region_exit 扩展为"fill 末帧 + mm_exit 后 pmd 条目必须非 present"（`results/r07/w3fix/futex-swapout-boundary.md` §3）。

#### 案例二：futex 路径双重 folio_put（MV3.b，DPA 定罪）

症状：corten_mode_default=on 的全系统 boot 下 6/6 复现 Bad page map（mapcount -1）、Bad page cache、PCP LIST_POISON2，进而 oops 加 RCU stall wedge，8~70 秒窗口内三个内核构建全中。开发期初判"MV3.a 时代 slot 竞态、本片 diff 构造性只读"——两个结论都被 DEBUG_PAGEALLOC 加 PAGE_OWNER 的定罪链推翻：同 pfn 在 journald 与 spawn 两个上下文成对出现，pfn 0x11235b 的 page last free 落在 spawn，而发现它坏状态的栈是 journald 自己的 `__access_remote_vm` 内联 folio_put ← proc_pid_cmdline_read。根因：首版 remote face 臂把 page 交给 corten_gup_window 时没带 FOLL_GET；真 MMU 的 __get_user_pages_locked 对非空 pages 数组强制取引用（源码注释自证），探针因此不取，循环收尾的 folio_release_kmap 照 put——每次窗口域 cmdline 读净减一引用，压穿后提前释放、帧重用、双重释放。修复是两处 face 臂各补一个标志位加契约注释，KUnit 锚补 folio_ref_count delta 红面。验证：DPA 同工具修后 60 秒零事件（修前 12+ 条），×5 全启动 gate 5/5 零签名，MV3.d 长跑零复发。初判错误的推翻过程全部留痕。

#### 案例三：/proc/maps 游标首行丢失（V-C，oracle 首战）

症状：J3 oracle 首测 maps 渲染错乱——首行永不输出、末行重复、树头被吞、seq 回卷不变量破。定位的关键是"为什么旧测试全盲"：单 region 探针自洽，多 region oracle 一上就现形。根因：游标行推进先于渲染，每行渲染成了下一 region 的跨度。修复：peek 头、promote 后再推进、树头归还、last_pos 不变量恢复，配新归并序 KUnit 锚；同片顺带抓到 PROCMAP_QUERY 的 ioctl 号错配（ENOTTY）。这个 oracle 的价值在两代之后再次兑现：W-2 把行臂改成 return NULL（seq_file 契约 NULL=EOF）导致 MODE 进程 maps 恒空，被"无 X"式空真断言掩盖了两个世代，W-6 oracle 首跑当场照出（`project/next/w6-dev-report.md` §3.1）。

#### 案例四：get_futex_key 族臂与 ro 契约语义（W-3.2 → W-3fix）

症状：metis_eq 在 W-3 终件上 100% Abort（rc=134），glibc 报 "The futex facility returned an unexpected error code"；当时的换出边界归因经复核被推翻——同 boot swapped_out==0，根本没有换出发生。定位：strace 抓到 `futex(0x100001000990, FUTEX_WAIT_BITSET|FUTEX_CLOCK_REALTIME, tid, NULL, ...) = -1 EFAULT`，地址落在 glibc 线程栈映射内（PROT_NONE MAP_STACK declare + 顶片 mprotect RW）；kprobe get_futex_key 返回 0xfffffff2；gup_probe_rejects 每次复现 +4。根因两层：W-3.2 的 GUP 重构给取键路径落了 arena-anon key 臂（窗口页没有 VMA，不补这条臂 JVM 这类高频用户直接失联）；但 corten_gup_window 的 check_vma_flags 仿真只读 ar->prot，而 CHUNK mprotect 只把 perm 提交进槽位元数据（pending-perm 契约），栈 region 的 FOLL_WRITE 全被拒。修复：探针对齐 FRESH 门同一规则（m.perm 优先，退 ar->prot，经 corten_ptdesc_get RCU 钉住读）。验证：KUnit 锚 w3_gup_chunk_promoted_perm 红→绿，metis 双跑 rc=0 且 checksum 与无 hook 跑同值。

### 4.4 工程事故：AI 长跑项目的真实面

限额中断。V-B.2 的 dev agent 21:47 死于 1302 速率限制，留下 +420 行半成品，主会话审计确认完成度后补齐验证链入库；W-4 期间 agent 两次 429 殉职（配额窗 5 小时），同样的主会话接管前后应用三处。同族还有运维面：09-22 夜验脚本随会话退出被带走，setsid 重启才脱离会话；08:10 的自动收数 cron 在触发时刻会话不在运行未触发，改手动收数。

pkill 自匹配。坑清单第一条就是"pkill qemu 用 [q]emu 防自匹配"，之后仍重演两个变体：一次模式串自匹配杀掉自会话，一次模式串里含端口号同样自匹配。同一条坑以不同变体咬人，说明运维教训的价值在执行不在记录。

SSH 瞬断取数失败。MV3.d 电池的 battery-on 腿在 6780 秒处 PROC_GONE_OR_SSH_DEAD，guest-on.log 截断在 SEC2，p2-audit-gate.txt 是空壳——P2 长跑累计的 617/69001 读数从此只剩 lead 转述，判据链被迫降级为"归档同形读数为主、live 值旁证"。GitHub 侧同样：TLS 瞬断让推送多次累积后补推，代理下 git 挂死以 http.version=HTTP/1.1 解决。

branch 拓扑错位。D27 重放时 rebase 误传主分支名导致主分支哈希链被重写，即时 reset --hard 恢复（树零差异、tag 完好），"重放操作永远在临时 checkout 上做"入坑清单；W-4 吸收时发现 mva worktree 的在制草稿基座陈旧（缺 W-3fix2 全部三修复），此前一次"只读 diff 前 150 行即断言 swap_in 零差异"的审计判断被全量核验推翻，改用「W-3fix 基座提取纯 W-4 patch → git apply 到主树正身」重建；51 个 tag 悬在与远端不相交的 140 万提交历史链上，数 GB 补传从本机网络不可行，如实放弃并登记。零散的还有：并发 make 撞车三次后立树内串行化约定；config 翻转致增量构建状态腐坏，验证件在被 =n 链接覆写前抢救归档；w6 与 w6b 两个会话的同名 gate 脚本碰撞，靠 md5 权威件仲裁。

## 5. 效果与评估

### 5.1 三张表

性能账（同 boot ABAB 双臂口径）：

| 项 | 读数 | 出处 |
|---|---|---|
| unmap-virt 低竞争 t4/t8 | 五轮固化 +1142%/+2583%；run5 终验 +1156/+2327 | results/r07/g1-final.md |
| unmap（预映射区）t4/t8 | 固化 +45.4%/+142.0%；run5 +61.2/+138.7 | 同上 |
| mmap 全族 | 固化 +141.3%/+251.4% | 同上 |
| fork/fork+exec/shell | 修复前 +516/+172/+354 → A5 后 -10.65/+3.77/+0.52 | results/r07/g5-gate/、a5-fix.md |
| dedup（tcmalloc 档） | 六个独立 boot 全正，+8.7~+19.2%，幅度不定 | t5 各轮谱系 |
| mmap-pf | D35 刷新起点 -84.3% → warm park 后 -63.2%（改善中位 +134.6%/+135.4%）→ w3fix6 树 t4 **-33.3%** / t8 **-61.5%** | next/mv3cfeat-dev-report.md §2.3；results/r07/w3fix7/bench-base/ |
| arena 生命周期税 | 17.4µs→1.3µs（13 倍），池命中 99.999% | results/r07/t1c-verify.md |
| journald/tmpfiles 面 | 停摆 6min+ → 9.08s | next/mv3b-dev-report.md §1.3 |
| 内存开销（G7） | 8288B/2M 窗 = 映射内存 0.3952%（legacy 0.195%，页表侧 ×2.02） | results/r07/g7-mem.md |

正确性账：

| 面 | 读数 |
|---|---|
| syzkaller | 两轮 453 万 exec（corten=on 面 311 万）零内存安全崩溃；6 崩溃目录全环境类 |
| KUnit | 锚 157（MV2 终代）；MV3.c-feat 单轮 194 pass / 0 fail；lockdep 变体零 splat |
| DEBUG_ATOMIC_SLEEP / KCSAN | 首开 47 处原子内睡眠修到 0；KCSAN 短跑零 data race |
| J1 / J2 | 617/0、69001/0（live 转述，无归档原件；判据链以归档同形 327/0、224/0 为主） |
| MV3.d 全系统电池 | =on 世界 7216 秒，LTP 全编译安装运行，零 panic 零 corruption |
| swap 对账 | swapped_out 69164 = swapins 69164；shrinker 换出 RSS 281MB→3.8MB；三方口径 Δ<0.05% |
| corruption 族 | ×5 全启动 gate 5/5 零签名（修前 6/6 boot 必炸） |
| 零改动回归 | smoke 26/26 双形态、metis checksum 同基准、JTB rc=0、strace 零新错误类 |

代码量账（W-6 J6 口径）：

| 侧 | 行数 |
|---|---|
| VMA 层四件（mmap.c/vma.c/mmap_lock.c/maple_tree.c） | 13,408（vanilla 12,955，路由钩 +453） |
| corten 生产 | 23,761（此后 MV3 各片仍净增；当前树 corten_arena.c 单文件 19,160 行） |
| corten 测试 | 20,573 |
| 净增（生产 vs vanilla 四件） | +10,806 |
| 删除账 | 五组 24 项（A 走查/B 合并臂/C 生产与 bulk/D 渲染/E 勿删边界）+ PR-0..4 序列，未实际删 |

### 5.2 诚实边界

J2/J4 的判定史必须保留：W-6 中期首测 4 PASS / 3 FAIL——J3 是 W-2 起的 /proc/maps 恒空真回归（空真断言掩盖两代），J2 的 special 桶分类器失明（arch_vma_name 不认 special_mapping，vdso 全落 UNCLASSIFIED），J4 的 sweep fail-open 面宽（skip_declare=288/电池，收编率不足）。三者分别在 W-6b（vma_is_special_mapping_family + 逐因计数器）与 W-7（multi-record registry，skip_declare 288→0，tree_entries=5）修复后转 PASS，FAIL 判定原文在案不改。

其余边界照登：性能三刀落一（W-3fix5 脏域夹取）余二（簿记批化在制、bisect 未动，§3.2 RQ4）；brk 接管有 [C1] 红线——入场前已驻留的堆不收（resident PTE 无元数据 = MAPERR 自家数据），glibc 电池 brk_legacy=2882 披露，初始 heap VMA 至今留树（wl_brk 桶，V-E.1 verdict 维持 legacy 漏斗应答）；~~P2 采证缺口~~ 已清偿（w3fix4 补读归档原件落库：p2-audit-gate.txt 21 行 j1 310/0 + j2 3456/0 + tree_entries=0 + gate_pass=1，dpa-arena-stats.txt 120 行；LTP =on verdict 腿级仍为登记缺口）；55 tag 未补传（旧 tag 指向重写前历史，补传需传整棵老树，登记不阻塞）；G3 的字面门（真实应用单项稳定 ≥10%）没过，按"六 boot 方向恒正 + 机制归因"定案；mmap-pf 地板 w3fix6 树 t4 -33.3% / t8 -61.5% 未达个位数；bpf_iter/task_vma 窗口段整段缺失（#39，双源化的唯一机械是给 BPF 铸 VMA 形对象，D28 明禁），trace 符号化三处降级接受，mseal 维持 fail-closed；suid 未单测（立场：不特判，系统电池隐式覆盖）；库 hint 收编是超红线裁决件；残留台账 9 开项（P2×5、P3×4；P1×4 全清偿——含 brk 路由 PT 生命周期 UAF，w3fix4 pin 协议修复 + DPA 复测零 oops）。

## 6. 结论与观察

### 6.1 D29 两目标的判定

目标①（完整实现论文的优化思路）机制面全达成：事务化热路径七路由在产线、窗口域零 VMA 有 J1/J2 实证封口（归档原件在库）、原生 rmap 与匿名换出直驱落地、warm park 拿到 +134.6% 的重构首刀；性能面三刀已落一（脏域夹取），w3fix6 树地板 t4 -33.3% / t8 -61.5%，距论文个位数目标未达，如实登记。目标②（对所有应用无损完整接管）达成于显式降级与响亮拒绝的口径：execve 即 MODE 覆盖全部进程（exec_default_enters=153），bpf_iter 窗口段整段缺失登记为降级，mseal/madvise 若干臂响亮拒绝，7216 秒全系统电池零 panic 零 corruption。D29 级开项为零，全部余项降级为残留台账的独立小片（现 9 项：P2×5、P3×4，`project/next/mv3e-dev-report.md` §2 起算、W-3fix4/5/6 清偿七项，REPORT.md §11.5）。

### 6.2 对"AI 多会话长跑开发系统软件"的观察

以下是我在 23 天记录里反复得到验证的三条判断，供同行参考。

第一，质量控制的主体是 gate 与判据，模型负责在其中填空——而判据自身的覆盖面才是最容易漏的地方。修复的真缺陷绝大多数拦在协议链上而非靠模型的一次性正确率：lockdep 变体抓出 47 处原子内睡眠，DEBUG_PAGEALLOC 定罪了漏 FOLL_GET 的引用压穿，J3 oracle 揪出被空真断言掩盖两代的渲染死码，review agent 在 W-3fix2 的"自验全绿"里仍判 FAIL 并抠出两处真缺口。反面同样清楚：A.2 全部夜验实际只跑了 corten 主套件——verify 脚本的 filter_glob 写错，而 09-21 就登记过的同一个坑被新脚本原样重新引入（D25）。gate 驱动的代价也真实：一个中型切片烧 5-7 个连续 boot 的验证矩阵，MV2 修复轮 65+ 提交里相当一部分是判据面与锚的加固。这笔账换来的东西写在 453 万次 fuzz 零内存安全、6/6 必炸的 corruption 族 5/5 归零里。

第二，subagent 分工的边界在"验证者的独立性"上，而不在功能分工上。dev/review/verify 三角加"brief 是假设、guest 是裁判"的次序在 23 天里没有例外——MV3.a 任务书的六条默认裁决被 dev agent 用红证推翻或修正了三条，其中一条直接来自 =on boot 首跑 init 的 SIGSEGV 现场。但两处失守都发生在验证者与开发者共享了同一个盲点时：一次是"只读 diff 前 150 行"的审计断言（W-4 基座陈旧），一次是开发与验证共用的测试件自身形状缺陷（fork_redeclare 在纯 A.2 基座上即红）。另外两条工程铁律是拿事故换的：dev agent 死在半路时主会话接管能成立，前提是每片半成品都留在 worktree 里可审计；多 agent 并发写同一 worktree 必须串行化（三次 make 撞车、mv3b 的并发写者事件各立一条）。

第三，人类指令的密度极低、杠杆极高，而最有价值的两次输入都是对 AI 口径的干预。23 天里方向性的输入只有三五条：D20 定方向，D28 推翻 AI 自己两天前批下的诚实化口径（"说要完全移除就是要完全移除"），D29 定终局。D28 的推翻把工程往前推了一大截——没有它就没有 W 系列的 carrier 消灭与 tree_entries=5；反过来 MV3.e 又把"字面全删"精确化为"死的是窗口域臂"，逐桶对账。激进目标与诚实边界在反复对撞中把真实工程边界磨了出来，这个对撞需要人参与。至于人类不在场的时段，事故全部集中在运维面（限额、pkill、SSH、网络、branch 拓扑），没有一次是方向性的——多会话接力可行，交接面必须落盘，上下文会死，账本不能死：r08 主控会话僵死后，r09 从 STATE.md 这份一千六百多行的现场账无缝接续，连 W-4 的在制增量都是从 worktree 里找回吸收的。对想复现或接着做的人，入口是两份文件：`project/STATE.md` 记录每个判断是怎么做出来的，`project/next/mv3e-dev-report.md` §2 的残留台账列着还欠什么——brk 路由 PT 生命周期 UAF 的定罪与修复、性能三刀、裁剪 PR-0..4 的排期，都在那里等下一条指令。
