# PAPER_SPEC · CortenMM 论文规范规格书（v3, 2026-09-13）
> 本文是论文(SOSP'25, doi 10.1145/3731569.3764836, 文本 $PAPER)的**唯一权威解读**。
> 所有设计/实现/评测以本文 PS-条款为准; 偏离必须在 DESIGN.md DEV 表登记+STATE 决策编号。
> 目的: 终结"各 agent 各自理解论文"的漂移(此前已发生: v2 的 PTE 真源倒置)。

## A. 问题定义(论文 §1-§2)
- PS-A1 根因论断: MM 的性能瓶颈与并发 bug 都源于**两层抽象**(软件层 VMA 树 +
  硬件层页表), 同步两个异构复杂数据结构是难度源头。证据: per-VMA lock 合入后
  两年 10 个 CVE; mmap/munmap 全程 mmap_lock 写锁; fault 需在 VMA 层找最可扩展
  锁序(L2-L15 的 expand 复杂度)。
- PS-A2 论文主张: 主流 ISA(x86/ARM/RISC-V)统一用多级基数树页表 → 软件层抽象
  失去存在理由(可移植性用宏/trait 解决; 高级语义的状态可挂在页表侧)。

## B. 数据结构(论文 §3.3, §4.3)
- PS-B1 页表页描述符(page descriptor): 按**物理页号 PFN 索引**, boot 期连续分配。
  内容: ①自旋/rw 锁(保护自身+对应 PT 页) ②per-PTE metadata array(按 PTE offset
  索引, **按需分配, 随 PT 页一起释放**)。
- PS-B2 **metadata array 是地址空间的权威描述(唯一真源)**, 每 entry 存 Status:
  - `Invalid`
  - `Mapped(PhysPage, Perm)` — 已映射物理页
  - `PrivateAnon(Perm)` — 虚拟分配(按需分页的"已 mmap 未触页"), **无任何 PTE**
  - `PrivateFileMapped(File, Offset, Perm)`
  - `Swapped(BlockDev, BlockNum, Perm)` — 换出页存磁盘 ID+块号+权限
  - 共享匿名等其余变体
- PS-B3 COW 双 bit(per metadata entry): `shared`(多进程共享) + `writable`(该虚拟页
  语义上可写)。硬件 PTE 只读化仅是机制, 语义判定在 metadata。
- PS-B4 页表 = 硬件翻译缓存; PT 页本身按需建立; 上层 PT entry 可表达"大区域同
  状态"(huge page 4K/2M/1G 走常规大页机制)。
- PS-B5 rmap 记录在**页的描述符**(page descriptor)上: 命名页→文件对象(内含
  AddrSpace 树), 私有匿名→AddrSpace。**"Access to the page table via reverse
  mapping always goes through the transactional interface"(§4.5 原文)** — rmap 是
  提示, 改页表必经事务。

## C. 事务接口(论文 §3.3, Fig.4/Fig.8)
- PS-C1 `AddrSpace::lock(range) → RCursor`; 事务内 `query(addr)→Status`,
  `map(addr, page, perm)`, `mark(range, Status)`, `unmap(range)`; RCursor 析构释放。
  事务**原子**; 不重叠 range 的事务完全并行(重叠才串行)。
- PS-C2 语义映射: mmap = lock+query(检查)+mark(PrivateAnon) — **零 PTE 操作**;
  munmap = lock+unmap; mprotect/msync = lock+改状态; page fault = lock(fault
  range)+query+按 Status 分派(Fig.8 L15-41)。
- PS-C3 fault 分派(论文 Fig.8): PrivateAnon → map(alloc_zeroed); Mapped 且
  write 且 COW → map_count==1 则原页去 COW 加 WRITE, 否则 alloc_copied 后
  map; Invalid → SIGSEGV; Swapped/FileMapped → 各自恢复路径。

## D. 锁协议(论文 §4.1, Fig.5/6/7)
- PS-D1 covering PT page: 自根下潜, 若当前 PT 页存在**完全覆盖 range** 的子 PT 页
  则继续下潜; 停在最低可能完全覆盖处。
- PS-D2 CortenMM_rw(简化版): 下潜时对覆盖子页持**读锁**; 到 covering 页释放读锁
  取**写锁**(自顶向下单向, 无锁倒置); unlock 逆序释放。声称已比 Linux 快 15×。
- PS-D3 CortenMM_adv: ①遍历阶段在 RCU 读临界区内**无锁**下潜; ②锁 covering 页,
  检查 stale(被并发 unmap 则重试); ③前序 DFS 锁全部后代; ④unlock 逆序全释放。
- PS-D4 unmap 释放 PT 页(论文 Fig.7): 先原子清父页 PTE → 子树标 stale → 解锁 →
  **RCU monitor** 延迟释放(等所有读临界区退出); 与遍历者的竞争靠 stale+重试收敛。
- PS-D5 原子性来源: 两阶段锁(先取全锁后操作); 单一 covering 页锁即隔离整个
  子树 → 大 range 事务天然串行化整个子树(这是语义不是缺陷)。

## E. 优化(论文 §4.5)
- PS-E1 per-core 虚拟地址分配器(引 MOSBENCH): 每核私有 VA 份额, 消除分配竞争。
- PS-E2 TLB shootdown: 并行 flush+提前确认(EuroSys'20); **LATR** lazy shootdown
  (munmap 后各 CPU 惰性刷, 定时器/调度点检查他人 per-CPU buffer)。
- PS-E3 物理内存: buddy/slab 沿用 Linux; 页描述符沿用 struct page 思想。

## F. 评测协议(论文 §6 —— 我们的复刻基准)
- PS-F1 环境: 2×AMD EPYC 9965 = 384 核, 512G×2, **关 HT/关 turbo**, 且**在 VM 里
  测**(OS 缺驱动; 论文认为不影响结论) ← 我们 QEMU 方法学上站得住。
- PS-F2 **公平性: Linux 关闭全部 mitigations(如 KPTI)**; Linux=6.13.8, 两 OS 同
  Ubuntu 22.04 用户态。→ 复刻要求: 对照两臂同加 `mitigations=off`。
- PS-F3 微基准(Table 3, 16KB=4 页/操作, ops/µs, 1..384 线程扫描):
  | 名 | 定义 |
  |---|---|
  | mmap | 每线程 mmap() 16KB 区域(仅 mmap 调用本身) |
  | mmap-PF | mmap() 16KB 后访问它 |
  | unmap-virt | munmap() 16KB **无物理页**区域 |
  | unmap | munmap() 16KB **有物理页**区域 |
  | PF | 访问 16KB 未触区域 |
  低竞争=各线程私有区域; 高竞争=大共享区域内随机偏移。16KB 恰好单 PT 页内
  (512 PTE) → 单 covering 页事务, 竞争时碰撞在末级 PT 页。
- PS-F4 论文单线程结果(Fig.13, vs Linux): adv: mmap **-3.1%**(PT 页分配初始化贵于
  VMA), mmap-PF **+46.8%**, PF **+53.6%**, unmap-virt **+76.9%**, unmap **+7.8%**;
  rw: -19.2/+22.4/+28.4/+56.0/+18.5。→ 预期我们的方向: mmap 允许小幅回退,
  其余正向; mmap-PF/PF 是 fault 路径胜利, unmap-virt 是"无 VMA 拆分"胜利。
- PS-F5 多线程(Fig.14): 低竞争 adv 近线性, 384 核 vs Linux 33×(PF)~2270×
  (unmap-virt); 高竞争 adv 在 >64 线程因末级 PT 页碰撞不再扩展但仍 3×~1489×;
  rw 低竞争 1.8×(PF)~275×(unmap)。**8 vCPU 复刻边界: 只测 1-8(+16 超订)的
  低/高竞争斜率, 64 线程平台期不可测, 2270× 量级差距不可复现**(诚实声明)。
- PS-F6 枚举型最坏情况(LMbench, Fig.20): fork **-17.7%**(须走页表而非 VMA 枚举),
  fork+exec **+23.0%**(fault 快), shell 持平。
- PS-F7 真实应用(Fig.15-17): JVM 线程创建(仿 Android app 启动: N 线程各占一核,
  测 spawn→初始化完成窗口, 384 核 +32%); metis(MOSBENCH, 1.6GB 文本 map-reduce,
  8MB chunk 不归还, 384 核 **26×**); dedup(PARSEC, ptmalloc 下 Linux >16 线程不
  扩展——**因频繁 munmap 归还内存撞 mmap_lock 写锁**, adv 64 线程 2.69×);
  psearchy(索引 368MB/33k 文件, ptmalloc ~2×@64)。其余 PARSEC ≈ 持平(非 MM 敏感)。
- PS-F8 **分配器维度(Fig.17/18, 复刻必含)**: dedup/psearchy 各跑 ptmalloc 与
  **tcmalloc** 两档: tcmalloc 极少 munmap 归还 → Linux 也能扩展但代价 RSS ~2×
  (Fig.18)。这组对照揭示"用户态绕过 vs 内核修复"的结构, 是论文叙事核心之一。
- PS-F9 内存开销(Fig.22): metis 下 CortenMM≈Linux(PT+metadata); 满配 metadata
  理论上界≈2× 仍在 2% 内; RadixVM 复制页表最差。复刻: PT 页字节+metadata 字节
  vs 理论上界 vs 基线。
- PS-F10 总纲: "formally verified CortenMM outperforms Linux by 1.2×–26× on
  real-world benchmarks"。
- PS-F11 验证(§5, 我们不做 D2): Verus 证明互斥+基本操作功能正确+页表良构,
  proof:code=5.2:1, 8 人月。→ 我们的替代: INV 不变量×KUnit/lockdep/KCSAN/syz。

## G. 论文自认边界(复刻时同样适用)
- PS-G1 NUMA 策略不支持(与 Linux 对比时关平); PS-G2 ARM 未移植(contiguous bit/
  BBM 只是工程量); PS-G3 非基数树 MMU 不适用; PS-G4 retrofit 成熟 OS"工程量巨大"
  → 我们 opt-in arena 即 retrofit 姿态, 但 arena 语义必须保 PS-B2/C/D 的完整度。
