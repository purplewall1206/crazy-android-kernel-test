# DESIGN · CortenMM→Linux 移植技术设计（v3, 2026-09-13 重规划）
> **规范关系: docs/PAPER_SPEC.md(PS-x 条款) 是论文的唯一权威解读; 本文只做移植决策,
> 每处偏离论文之处登记为 DEV-x 并给出理由; 新增 DEV 须 STATE 决策编号。**
> v2 的"PTE 真源原则"已被 D11 废除(倒置了 PS-B2)。子设计: publish/M3B_DESIGN.md
> (M3b), publish/ARM64_PORTING.md(M9) — 与本文冲突处以本文+PAPER_SPEC 为准。

## 0. 移植姿态
论文 PS-G4: retrofit 成熟 OS 工程量巨大 → 本移植 = **opt-in retrofit**: 进程可把
自身(或一段)地址空间切到 corten 语义; 切换后**arena 内部必须完整保持 PS-B/C/D**,
即 metadata 唯一真源+事务接口强制; Linux 兼容 plumbing(shadow/chunk-VMA)只服务
arena 之外的内核世界, 不得反向侵蚀 arena 语义。

## 1. 概念落地总表（PS 条款 → 实现; ✓=M2 已在树）
| PS | 落地 | 偏差 |
|---|---|---|
| B1 desc(PFN 索引) | PFN→corten_ptdesc xarray ✓ | DEV-1: boot 连续数组→xarray(不占巨量连续内存, 查找 O(1) 均摊) |
| B1 per-PTE metadata(按需, 随 PT 页生灭) | desc 内 meta[ ], pte alloc/free 双钩子 ✓ | 无 |
| **B2 metadata 唯一真源** | **arena 内 Status=唯一真源; 页表=缓存**(§3) | 无(v2 曾倒置, 已纠) |
| B3 COW 双 bit | meta.flags shared/writable ✓(M2) | 无 |
| B5 rmap 改页表必经事务 | 回收/换出路径对 VM_CORTEN 页改 PTE 一律走 corten 事务(§6) | DEV-2: rmap 的"找到页"环节借 Linux chunk-VMA(论文用自家描述符反链) |
| C1 事务接口 | corten_lock_range/query/map/mark/unmap ✓(M2) | 无 |
| C2 mmap=mark 零 PTE | M4.T1 arena mmap = 纯 mark ✓设计 | 无 |
| D1-D4 锁协议 rw 版 | covering 页协议 ✓(M2b)+M3a BH 对称 | DEV-3: rwlock 实现用 qrwlock(论文 BRAVO-pfqlock), 功能等价 |
| E1 per-core VA 分配 | M4.T1 per-cpu 2M-frame 杂志 | 无 |
| E2 shootdown 优化 | M4 先 mmu_gather 批量; LATR 列 OQ5 | DEV-4: lazy shootdown 降级为可选项(8 vCPU 收益存疑) |
| F 评测 | docs/EVAL.md 全量复刻 F1-F9 | DEV-5: 8 vCPU 规模(PS-F5 边界声明) |

## 2. arena 两种进入模式（修正 v2 缺陷: 真实应用进不来）
- **MODE-targeted**(M3B_DESIGN 已设计): prctl(PR_CORTEN_ARENA=79, addr, len) 显式
  划段; mmbench/压测器用。
- **MODE-process(新增, 论文忠实姿态, 真实应用入口)**: prctl(PR_CORTEN_MODE=1)
  进程级开关(fork 继承)。生效后该进程所有 **addr=0 的匿名 MAP_PRIVATE mmap/munmap/
  mprotect/madvise(DONTNEED)** 路由进一个大 arena(内核在 0x1000_0000_0000-
  0x4000_0000_0000 预留窗, per-cpu VA 分配器发地址); **显式 addr 的映射(ELF/栈/
  vdso/固定地址)与 brk/文件映射走 legacy**(DEV-6: glibc 小分配走 brk 保持 legacy,
  论文全替换做不到, 如实披露)。metis_eq/dedup_eq/JThreadBench 等**零改动**即可
  进入 corten 语义 → PS-F7/F8 复刻成立。
- 路由规则(do_mmap/do_vmi_munmap/mprotect 入口判): MODE-process?addr==0?ANON?
  →arena 路径; 否则 legacy。MAP_FIXED 显式地址永远 legacy(allocators 预留不受扰)。
- 风险 R11: 路由漏判(某入口未收口)→ M4 首夜做入口审计清单(见 §5 审计)。

## 3. 真源纪律（PS-B2 的移植执行）
1. arena 内: Status=真源; **一切 PTE 写必须持对应事务**(fault/map/unmap/COW/
   换入/回收换出)。代码评审红线: 任何直接 ptep_* 写 arena 范围 = FAIL。
2. 无法避免的内核胶水路径(GUP 迁移等)收敛到唯一入口 `corten_glue_pte_write()`,
   内部取事务+同临界区更新 metadata; 该入口白名单化(M: ≤3 处), 超出即设计错误。
   白名单账(r07 登记, M5.T1a/M6.T2): ① copy_page_range 的 wrprotect 置位(fork
   忠实路径的 PTE 层, mmap.c; STATE D17/DEV-14); ② ttu 换出事务的 swap-PTE 安装
   (try_to_unmap_one 的守卫完成臂, rmap.c; PTE 与 meta 同临界区, M6.T2);
   ③ swapoff unuse_pte 的换回 PTE 安装(swapfile.c; meta 先行同步+故障侧自愈,
   M6.T2) —— **≤3 处已用 3, 额度已满**; 再有第 4 处即设计错误, 必须走事务路由。
3. chunk-VMA(§6)只是 Linux 侧的**可见性 plumbing**(让 rmap/MGLRU/proc 找得到),
   它的页表修改也必须经事务(PS-B5 原文规则)。
4. INV9(调试构建): metadata≡PTE 一致性 checker(resync 对比工具), 任何漂移=
   BUG 而非"预期 stale"(v2 说法作废)。

## 4. M3 arena+fault（机制在 publish/M3B_DESIGN.md, 本文只列 v3 修订点）
- fault 双门/2M-frame xarray/shadow-VMA(VM_CORTEN bit43) 不变。
- 修订: fault 分派按 PS-C3 对齐 query 语义(query 返回 Status, 以 metadata 为准;
  PTE 只在持事务时读取以装载硬件缓存)。
- R1(fill_upper 封闭性)不变, 审计清单加 MODE-process 的四个路由入口。

## 5. M4 事务化空间操作
- 锁策略: arena 的 mmap/munmap/mprotect 取 **mmap_read_lock + 事务**(DEV-7:
  论文无 mmap_lock 可取, 移植以读锁与 fork/exit 写锁互斥换正确性; 写锁串行化点
  消失=移植版性能论点, 需 trace 验证消退)。
- mmap: MODE 判定→per-cpu 杂志发 VA→mark(PrivateAnon) 一步完成(PS-C2)。
- munmap: unmap 事务: 映射页 mmu_gather 批量 zap; 虚拟页纯 meta 清除(PS-F4 的
  unmap-virt 大胜来源); 空 2M frame 回杂志。
- mprotect/DONTNEED: 事务改 meta+已映射 PTE 批量权限位+批量 flush。
- 入口审计清单(收口证明): do_mmap / vm_munmap(do_vmi_munmap) / do_mprotect_pkey
  / do_madvise(DONTNEED 分支) / mremap(arena 内→-EINVAL, OQ2) / brk(legacy)。

## 6. M5 fork/COW + M6 回收/swap（v2 两处倒置的修正重设计）
**M5 fork — 默认论文忠实(DEV-8 修正)**: fork 对 arena 走论文路径: mmap_write 持有
下遍历 arena PT 页+meta: 每映射页 wrprotect+shared 置位(父子两侧), meta 数组深拷贝,
arena 注册/per-cpu 杂志复制(corten_arena_dup)。不走 copy_page_range(它绕过事务,
制造双真源)。备选"复用 copy_page_range+钩子"降级为 perf fallback(仅当忠实版
fork 回退 >2×论文的 17.7% 时再评估, 记 OQ7)。PS-F6 预期: fork 单线程回退,
fork+exec 因 fault 快而净赚。
**COW**: 事务内按 PS-C3: shared&&!writable 写 fault → map_count==1 原页回收写
权限(免拷贝), 否则 alloc_copied+map(COW_COPY)。页引用计数操作仍用标准 folio API
(DEV-9: 论文自管计数; 我们借标准计数=引用安全, 事务只锁语义)。
**GUP**: fast 无锁 PT 走天然兼容; slow 经 shadow/chunk VMA 查找; 写 GUP 触发
fault→事务 COW; pin 页被事务 unmap 时 zap PTE 保留 folio(标准语义)。
**M6 回收/swap**:
- Stage1(wired, 不入 LRU, memcg 走 shadow)不变 — 正确性优先的显式妥协(DEV-10,
  REPORT 披露; 论文无此阶段差)。
- Stage2 chunk-VMA(2M 首触建档, 均摊一次 mmap_write)给 Linux 侧 rmap/MGLRU
  **可见性**; 但 try_to_unmap/换出/迁移对 arena 页的 **PTE 写经事务**路由
  (PS-B5); Swapped 状态入 meta(BlockDev,BlockNum,Perm 按论文编码; zram 即块设备)。
- 换入: fault 查 meta=Swapped → 事务内换入+map(标准 swap cache 复用, DEV-9 同理)。
- MGLRU 对接: **lru_gen walker 对 arena folio 结构性 skip**(arena 页永不入
  LRU ⇒ `folio_lru_gen() < 0` 闸门, vmscan.c:3623, eviction P3 同此不可达);
  arena 自有压力通道的 aging **清 young 位且走两遍事务**(pass-1 age / pass-2
  eval; desc 写锁 > ptl 内 `ptep_clear_young_notify`, 镜像 vmscan.c:3717)。
  **勘误(r07 登记, M6.T3 / M6_RMAP_SPEC §1.2 P2/P3)**: 本节旧文"MGLRU aging
  走 mm_walk 只读统计、不改 PTE、无事务需求"**有误**——上游 walk_mm aging 对
  LRU 页本就清 young 位(vmscan.c:3717), 任何 arena 范围 young 位写必须经
  事务; 以 SPEC/corten 实现为准。

## 7. 不变量（映射 KUnit/lockdep/KCSAN; v3 修订加粗）
INV1 事务原子(covering 写锁持满全程) / INV2 锁序 mmap_write>read>desc->lock>ptl
/ INV3 desc 锁 BH 对称 / INV4 meta 与 PT 页同生灭 / **INV5 arena 热路径零 maple
零 mmap_lock(read 也不取, 慢门除外)** / **INV6 真源纪律: arena PTE 写必经事务或
白名单胶水; 违=FAIL** / **INV7 metadata≡PTE(调试构建 checker, INV9 同义强化)** /
INV8 引脚页经 unmap 后 folio 存活至 pin 释放 / INV9 CONFIG 未开=原内核。

## 8. ARM64（publish/ARM64_PORTING.md; 红线不变）
core 只经 pgtable 抽象; PTRS_PER_PTE 参数化; contpte/BBM 决议 M9.T3;
M4.T4 交叉编译 gate。

## 9. OQ（v3 更新）
OQ1 contpte/事务共存(M9) / OQ2 mremap 跨界 EINVAL(M3b.S7) / OQ3 userfaultfd 拒绝
(M3b.S7) / OQ4 KSM/mbind 白名单(M6 后) / OQ5 LATR 值不值(M8 后) / OQ6 wired 盲区
护栏 RLIMIT_AS 参数(M3b.S1) / **OQ7(新) 忠实 fork 若回退>35% 是否启用 fallback**。
