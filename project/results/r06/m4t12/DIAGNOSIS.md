# M4.T1+T2 诊断节（r06 m4t12 班, 2026-09-19）

> 方法: m5t1a-vm（宿主 /home/ppw/vm/trixie-m5t1a.img, 8 vCPU/4G/KVM, boot
> corten=on mitigations=off）。该 VM 的 bzImage 版本串 = 6.18.32-g0719bc6ae74e-dirty,
> 比本班基线 6869744 多一个不相关提交（GUP/fault-gate defer, 不触碰 MODE
> mmap/munmap 路径）——诊断结论不受影响。perf 采样 999Hz, 15s/腿,
> 符号来自 kallsyms（kptr_restrict=0）。

## D1（基础设施缺陷, 推翻既有微基准确读）: mmbench 是静态链接, LD_PRELOAD hook 从未进入 mmbench 的 mm

证据链:
1. `ldd mmbench` → "not a dynamic executable"; sha256 2a9c066c…（与 t5-run2/g1-consol
   env.txt 逐字一致 = D6 同一二进制）。
2. hook（bench/mode-hook/corten_mode_hook.c）靠 LD_PRELOAD constructor 在目标进程
   libc 初始化时 prctl(PR_CORTEN_MODE, ENTER)。静态二进制没有动态加载器 →
   constructor 永不在 mmbench 进程内运行。
3. g1-consol 原始 .err 里每条 mmbench t0 腿**只有一条** "corten_mode_hook: MODE on
   (pid X)" —— 该 pid 属于 env/timeout 包装进程链（execve 换 mm 后 MODE 位丢失,
   hook 注释自己写明了这个形态）; 而 dedup_eq t0 腿有**两条** marker（动态链接,
   真进入 MODE）。t5-run2 raw 同形。
4. 内核侧反证: g1-consol 全程 auto_mmaps 仅 +160（若 unmap-virt/mmap 的 T0 腿真在
   MODE, 单腿 15s 应产生 ~10^5 量级）; 本班复测: 同一二进制 + hook 直接跑
   `mmbench mmap low 2 2 3` → auto_mmaps **零增量**; 换动态链接重编的同一源码
   （gcc -O2 -pthread, 无 -static）跑同参 → **2s 内 auto_mmaps +19,365**。
5. **本班最初的 12 腿 perf 画像（静态二进制 + hook）全部是 legacy-vs-legacy**:
   两臂画像逐行雷同（osq_lock 62.5% vs 62.9%）, corten 符号合计 ≤0.09%,
   unmap-virt t4 两臂 ops 差 0.4%（=纯噪声）。

**结论**: t5-run2 / g1-consol 的全部 mmbench "T0" 臂从未进入 MODE。G1 的
"微基准平价/+7% 小效应"实际是 **legacy-vs-legacy 的 ABAB 慢漂移**（含 ABAB 固定
顺序 base 先行的顺序效应候选）; T5 报告 §1 的判定（G1 0/4）作为 "MODE 未达标"
的证据**作废**, 须以本班动态二进制口径重测（见 verify 报告）。apps 臂
（dedup/metis/psearchy/JVM, 动态链接, 双 marker 实证）不受影响——G3/G4 结论维持。

## D2（真实 MODE 画像）: 瓶颈分解（动态 mmbench + 自编探针, guest 独占度: 共享宿主）

动态重编 mmbench（同源码同参数, 双臂同二进制, in-boot 配对口径保持有效）后:

| 形态 | base | MODE | 差 | 主导符号（MODE 臂） |
|---|---|---|---|---|
| mmap low t8 | 0.00992 ops/µs | 0.00558 | **-44%** | osq_lock 69.3% + rwsem_spin 7.2%（写信号量排队）, corten 合计 **0.52%** |
| unmap-virt low t4 | 0.0463 | 0.00966 | **-79%** | IPI/flush 块 ~54%（__send_ipi_mask 27.0 + smp_call_function 20.2 + x2apic 7.0）+ 写队列 44% |
| unmap low t8 | 0.00117 | 0.00085 | **-27%** | osq 47.2% + IPI 块 ~37% |

⚠ 上表 MODE 臂在 perf record 之下测得; 无 perf 复测 unmap-virt t4 → 0.0389
（=对 base -16%, 而非 -79%）——**perf 采样 IPI 与 TLB IPI 相互放大**, 定时腿必须
无 perf。共享宿主有邻道 VM, 绝对值有污染, 但画像结构（符号占比）是稳定的。

自编探针（results/r06/m4t12/m4t12_probe.c, 单线程, 无 perf 干扰, 邻道影响小）:

| 形态 | BASE | MODE | 判读 |
|---|---|---|---|
| mmap+munmap 对 (mpl, t1) | 15.0 µs/op | 49.5 µs/op | **3.3× 慢**: 仪式税 ~34µs/op |
| unmap-virt (uv, t1) | 13.0 µs/op | 6.3 µs/op | **2.1× 快**: 事务 zap 胜出（PS-F4 方向） |
| unmap-virt (t4, 顺序块) | 14.0 µs/op | 3.5 µs/op | **4× 快** |

mpl 臂 perf 画像（仪式税分解, 占采样%）:
- **new_slab 19.1 + radix_tree_node_ctor 15.6 ≈ 35% = xarray 节点生灭**——
  每 auto-arena DECLARE xa_store / RELEASE xa_erase, 稳态 churn 下树无法保温,
  每	op 走节点分配+释放（memcg 记账钩子再 +2.5%）;
- kfree/kmem_cache_free/memcg free 钩子 ≈ 4%（arena 描述符 kfree_rcu 生命周期）;
- TLB flush 链 ≈ 3.6%（release 的 do_munmap 单次 flush——此形态无重复 flush）;
- 其余摊薄在 ctl_lock/refcount/obs 全局锁/anon_vma 等。

unmap-virt t4 的 MODE 崩塌（perf 下）逐样本链（perf script 实证）:
```
__do_sys_munmap → corten_arena_munmap_route → corten_arena_unmap_chunk
                → tlb_finish_mmu → flush_tlb_mm_range → __send_ipi_mask (17.5%)
do_mmap → corten_arena_mmap_route → tlb_finish_mmu → flush_tlb_mm_range
        → on_each_cpu_cond_mask (refill 侧同样整链 IPI)
```
即 **munmap(zap) 与 refill(mark) 两侧各付一次 mmu_gather 收尾 flush**; 且
mark 路径的 fill_upper+zap+mark 全程在 mmap_write 之内, 拉长两臂共享的写锁
临界区（BASE 臂同区间只做 VMA merge）。t8 画像两臂同以 osq_lock 为主
（BASE 62.5% / MODE 69.3%）——MODE 多做的工作在写锁持内, 排队放大。

## D3 → 实现重心的调整（brief 授权: "按诊断结果调整 T1/T2 的实现重心"）

1. **T1（per-cpu 2M-frame 杂志）**: 落地 per-cpu 段发放 + **真回收**
   （release 后 marker 复位 + recycle list, 下一发放下**同址复用**）。
   回收直接命中 D2 的 35% xarray 节点生灭: 稳态 churn 下 xa_store 落在已保温
   的槽位, 描述符/节点不再逐 op 生灭。障碍跳跃语义**原样保留**: 每次发放仍做
   marker 校验 + find_vma_intersection（T0 每次发放的同类检查, 成本不变）,
   claim 时整段扫描只是降低 skip 频率。
2. **T2（munmap 批处理）**: (a) munmap_route 单帧快速通道（start 与 end-1 同
   2M 帧 → 省 1 次 RCU lookup + born-atomic tryget/put）; (b) zap 窗口一次
   ptl 锁段扫过（原逐 PTE 加解锁）; (c) **单一 flush**: 删除 zap 的显式
   flush_tlb_range（与 mmu_gather 收尾 flush 重复）, 一次 zap 一次 shootdown;
   (d) 修复 tlb_remove_page 批溢出返回值被忽略的潜在越界（对齐 zap_pte_range
   force_flush 惯用法）。punch 路由核对: 复用 unmap_chunk, T2 改动自动覆盖,
   无需单独改。
3. **测量基建修复**（本班附带交付）: guest 侧动态链接 mmbench
   （同源码, 双臂同件, D6 的 in-boot 配对语义保持）; 后续 T5/M8 的 mmbench 臂
   必须换用动态口径并在 meta 里记 sha256。

诚实声明: 写锁排队（osq/rwsem）在 t8 是两臂共同瓶颈, T1/T2 消除的是 MODE
**额外**拉长的部分与仪式税, 不改变"shadow-VMA 架构下 mmap 仍取 mmap_write"的
DEV-7 口径——"arena mmap 不取 mmap_lock write" 的完整 T1 愿景仍属后续切片。
