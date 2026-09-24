# ROADMAP · CortenMM→Linux 移植总路线图（v2, 2026-09-13 重规划）
> 状态唯一真源 = 本文件 + STATE.md「当前阶段」。任务 ID 全局唯一，夜报/commit/结果目录都用它引用。
> 旧 M0-M9 编号保留（与已有 tag/日志兼容）。证据链接指向 results/ 与 patches/。

## 1. 现状一览（**2026-09-22 v1.5 校准**——上一版停留在 09-13, 期间 M3-M8 全部走完, 属文档漂移, 本表对齐实际）

| 阶段 | 状态 | 证据 |
|---|---|---|
| M0 环境完备 | **✓ 完成** | results/r01/{boot-m0-console.log,m0-dod-*}; tag corten-r01-m0 |
| M1 基线测量 | **✓ 完成** | results/r01/baseline/; BASELINE_DONE |
| M2 内核骨架(事务 API+KUnit) | **✓ 完成** | 提交 b8386e4e/2fd4070e/1284a235; KUnit 16/16; bzimg/r01-m2c-fix1 |
| M3a+M3b | **✓ 完成（M3 DoD 4/4 PASS）** | e911b31a/d040b610/7bba3b9f/96466df2/ae236ee0; perf 367K 样本 find_vma/mmap_lock 零命中（results/r03/final-smoke/m3-verdict-final.md） |
| M4 事务化空间操作 | **✓ 完成（判定 PASS, run5 四门）** | T0/T1/T2/T1c/perf1/A5 全在树; G1 MET 2/4 + G4/G5 MET（REPORT §4.2-4.4, results/r07/t5-run5/） |
| M5 fork/COW/GUP | **✓ 完成** | 6869744(m5t1a)/1f8dfc78(m5t1b)/5c545359(m5t3); G5 NOT MET→A5 修复→MET |
| M6 rmap/回收/swap | **✓ 完成（T1-T4）** | 0e469cd9/b5175400/2639d329; swap roundtrip 69164=69164, RSS 281→3.8MB |
| M7 稳定性 | **✓ 完成（两轮终判 T2 DoD PASS）** | results/r07/syz-report.md + **results/r08/m7-final.md**（corten=on 面 3.11M exec 零内存安全; KCSAN 余项 A8） |
| M8 终测报告 | **✓ 完成（REPORT.md v1.5 = 冻结口径）** | G1-G8 对账 + G2/G7 定量闭环（报告表 7） |
| M9 ARM64 | **P1 ✓ P2 ✓（P3 = contpte 决议未做）** | results/r06/m9-p1/ + 93f834060cd1(m9p2, arm64 KUnit 4×25/0/0) |
| **M-V 移除 VMA 层（D20 轨道, 规格后增）** | **A.0 ✓ A.1 ✓; A.2a/A.2b 代码完成待夜验（09-22 23:00）; A.3-V.E 未开始** | specs/MV_VMA_FREE_SPEC.md（9 切片 ≈4700 行）; 34f1ae661be3(mva0)/d40eae59ba76(mva1); results/r07/mva1-verify.md; patches/r07-mva2.diff |

> 依赖图、§3 切片表与 §5 夜窗预算为 v2 规划历史存档（任务 ID 语义不变, 状态以上表为准;
> §5 的 23:00-09:00 夜窗口已由 STATE D21 撤销——持续工作制, MASTER_PROMPT v3）。

## 2. 全局依赖图（箭头 = 必须先完成）

```
M0 ✓ → M1 ✓ → M2 ✓ → M3a ◐ → M3b → M4 ─┬→ M5(fork/COW/GUP) ─→ M6(rmap/swap/回收)
                                          └───────────────────→ M6(依赖 M5 的 COW 位) → M8
M7(稳定性) 从 M3b 起每夜伴随, 不阻塞主线但 M8 前 DoD 必须满足
M9(ARM64) 设计已并入 M2-M6 的抽象约束; 交叉编译 gate 在 M4 末
```

## 3. 剩余任务分解（切片 ≤300 行 diff / 单夜可验收；S=小 ≤1h, M=中, L=大需拆夜）

### M3a 收尾（今夜优先）
| ID | 内容 | DoD | 规模 |
|---|---|---|---|
| M3a.F1 | 修 blocking: corten desc->lock 全链 BH 对称(write_lock_irqsave 一致化, 消除同 CPU read↔BH-write 死锁) | KUnit×3 + lockdep 构建压测 + corten=on 冒烟全绿; commit+tag | S |

### M3b arena fault（按 publish/M3B_DESIGN.md 的 S1-S8 执行, 此处只列门禁）
| ID | 内容 | DoD | 规模 |
|---|---|---|---|
| M3b.S1-S3 | prctl(PR_CORTEN_ARENA=79) + per-mm 2M-frame xarray + shadow-VMA(VM_CORTEN bit43) | 设计文档 §S1-S3 验收条; KUnit; arena 声明/撤销正确 | M |
| M3b.S4-S6 | fault 双钩子(do_user_addr_fault 快门 + handle_mm_fault 慢门) + corten_arena_user_fault + fill_upper 封闭性(R1 风险: 双钩子收口证明) | bench/arena-stress 零 panic; /proc/pid/maps 如实; perf 符号验证 fault 路径无 find_vma/mmap_lock | L(两夜) |
| M3b.S7-S8 | 互操作矩阵落地(userfaultfd/mremap/madvise 拒绝路径) + kselftests/mm 冒烟子集 | M3B_DESIGN §5 矩阵逐条可执行判定 | M |

### M4 事务化空间操作（v3: 入口前移 T0 透明接管, 见 DESIGN §2/§5）
| ID | 内容 | DoD | 规模 |
|---|---|---|---|
| M4.T0 | **MODE-process 透明接管**: prctl(PR_CORTEN_MODE) + do_mmap/vm_munmap/do_mprotect_pkey/do_madvise 四入口路由(addr=0 匿名私有→arena, 显式地址/brk/文件→legacy) + 入口审计清单(DESIGN §5) | metis_eq/JThreadBench 零改动在 MODE-process 下跑通且行为正确; 审计清单收口评审过 | M |
| M4.T1 | arena mmap: per-cpu 2M-frame VA 分配器 + mark(PrivateAnon) 纯 meta 路径 | arena mmap 不取 mmap_lock write(trace); 功能测试 | M |
| M4.T2 | arena munmap: 事务 unmap + mmu_gather 批量 zap + frame 回收 | unmap/unmap-virt 微基准 vs 基线出数 | M |
| M4.T3 | mprotect/madvise(DONTNEED) 事务化 | kselftests mprotect 子集 + arena 功能测试 | M |
| M4.T4 | **gate: arm64 交叉编译通过**(DESIGN §8 约束) | aarch64 defconfig+corten 构建零错 | S |
| M4.T5 | M4 中期对比(mmbench + apps 经 MODE-process, EVAL 协议含 mitigations=off 重校基线组) | 中期数据落盘; 基线若需补测 mitigations=off 组须先补 | M |

### M5 fork/COW/GUP（v3: 默认论文忠实 fork, 见 DESIGN §6/PS-C3）
| ID | 内容 | DoD | 规模 |
|---|---|---|---|
| M5.T1 | **忠实 fork**(默认): mmap_write 下遍历 arena PT+meta, wrprotect+shared 置位+meta 深拷贝+arena 注册复制(不经 copy_page_range) | fork 压测器: 父子隔离校验和一致, 1k 页往返; INV6 真源纪律评审 | L(两夜) |
| M5.T2 | 写时 COW 事务(map_count==1 免拷贝 / alloc_copied 两分支) | 论文 Fig.8 L26-38 语义测试 | M |
| M5.T3 | GUP 互操作: fast 验证 + slow 走 shadow/chunk + 引脚页保活 | process_vm_readv/ptrace/io_uring 注册 arena 页测试 | M |
| M5.T4 | lat_proc fork/fork+exec/shell + JVM 线程(MODE-process) | 数字落盘, 对照 PS-F6(-17.7%/+23%); 回退>35% 触发 OQ7 | S |

### M6 rmap/回收/swap（v3: 回收/换出的 PTE 写经事务, 见 DESIGN §6/PS-B5）
| ID | 内容 | DoD | 规模 |
|---|---|---|---|
| M6.T1 | Stage1 确认: arena 页 wired 不入 LRU, memcg 计费走 shadow | 压力下 arena 页不进 LRU; 计费正确 | S |
| M6.T2 | Stage2 chunk-VMA(2M 首触建档)提供 rmap/MGLRU 可见性; **try_to_unmap/迁移对 arena 页的 PTE 写路由进事务** | arena 页可被 MGLRU 回收; INV6 评审(回收路径无裸 ptep 写) | L(两夜) |
| M6.T3 | swap: Swapped 状态入 meta(zram 块号编码); 换入走事务+标准 swap cache | arena 压 4G→zram 换入换出正确; INV7 checker 零漂移 | M |
| M6.T4 | 内存开销测量(PS-F9) | PT+meta vs 理论上界 vs 基线, 对比表落盘 | S |

### M7 稳定性（伴随式, M8 前收口）
| ID | 内容 | DoD | 规模 |
|---|---|---|---|
| M7.T1 | syzkaller 环境: Go+syz clone 已备; syzconfig 构建(KCOV+KASAN_GENERIC); syzlang 增 PR_CORTEN_ARENA 常量描述 | manager 起动, crash 落 workdir | M |
| M7.T2 | 每夜 ≥4h 挂机(focus: mmap/munmap/mprotect/madvise/fork/clone/prctl/ptrace/userfaultfd) | 连续 2 夜无可复现 crash; crash→repro→修复闭环记录 | **✓ PASS（2026-09-22, results/r08/m7-final.md: 两轮 4.53M exec 零内存安全）** |
| M7.T3 | kselftests/mm 全量(guest 编译) + lockdep 构建 arena-stress + KCSAN 构建短跑 | 三件套报告; 新 fail 清零或豁免记录 | M |

### M8 终测报告
| ID | 内容 | DoD |
|---|---|---|
| M8.T1 | 全矩阵重跑(EVAL §2-§4 协议, 与 M1 同参同件) | results 完整, CV>5% 配置重测 |
| M8.T2 | perfetto 深度分析(EVAL §5 SQL 固化) | mmap_lock 消退证据 + fault 延迟分布 + kernel/user 分解 |
| M8.T3 | publish/REPORT.md(论文分组对照) + ARM64_PORTING.md 定稿 | 验收门 EVAL §6 G1-G8 全过或机制级解释+偏离声明 |
| M8.T0(前置) | 基线补测 mitigations=off 组(PS-F2 对齐, mmbench+lat_proc+apps) | 补测组与既有基线并存, REPORT 注明口径切换 | M |

### M9 ARM64
| ID | 内容 | DoD |
|---|---|---|
| M9.T1 | 交叉编译全功能(依赖 M4.T4 gate) | defconfig+CONFIG_CORTEN_MM=y 零错 |
| M9.T2 | 可选: qemu-system-aarch64(已装) + TCG + edk2 启动内核到 shell | 串口日志留档; 不阻塞验收 |
| M9.T3 | contpte/BBM 冲突决议(ARM64_PORTING OQ1/P3) 实施或文档豁免 | 决议记录进 STATE 决策 |

## 4. 风险登记册（触发器 → 处置）

| 风险 | 概率 | 触发信号 | 处置 |
|---|---|---|---|
| R1 fill_upper 封闭性破坏(arena 上层页表经非 corten 路径建立) | 中 | M3b.S4 互操作测试泄漏 | 双钩子收口审计清单化(M3B_DESIGN §4.7); 泄漏路径白名单化逐个堵 |
| R2 khugepaged/软中断上下文与 desc->lock BH 不对称 | 高(已命中) | lockdep splat | M3a.F1; 规则固化进 DESIGN §7 INV3 |
| R3 8 vCPU 下多线程提升不显著 | 中 | M4.T5 曲线平 | 报告斜率/拐点; 线程超订至 16; 机制级 trace 解释优先于数字 |
| R4 chunk-VMA 建档放大 mmap_lock write 回报(高触点 workload) | 中 | M6.T2 微基准回退 | chunk 建档预算+warm 后快路径无 VMA 论证; chunk 粒度可配 2M×N |
| R5 忠实 fork 遍历 arena 延迟线性化 | 高(设计使然) | M5.T4 fork >-35% | 论文允许(-17.7%); OQ7 触发 fallback(copy_page_range 复用)评估 |
| R6 真源纪律被胶水路径侵蚀(裸 ptep 写 arena) | 中 | INV6 评审/INV7 checker | 白名单唯一入口 corten_glue_pte_write ≤3 处; 超出=设计错误回炉 |
| R7 KASAN+syzkaller 资源超夜窗 | 中 | 夜报未完成 | syz 减半; KASAN_LIGHT; 保 lockdep 底线 |
| R8 会话并发写冲突(已发生过一次) | 已发生 | STATE/结果文件互踩 | MASTER_PROMPT v2 §2 会话锁协议, 强制 |
| R9 push 凭据缺失 | 确定 | git push 401/404 | 本地累积+夜报提醒, 不阻塞 |
| R10 android17 树与上游 6.18 行为漂移(GKI 补丁) | 低 | 基线数字异常 | 基线即 android17 实测(M1 已定), 对照论文只作定性 |
| R11 MODE-process 路由漏判/误判(某 mmap 入口未收口; 显式地址误入 arena) | 中 | M4.T0 审计/压测 | 四入口审计清单+strace 全量 diff(MODE on/off 的 syscall 结果等价性); 显式地址永远 legacy |
| R12 glibc brk 主导使 apps 提升被稀释(DEV-6) | 中 | G3 不达 | 双分配器维度定位; tcmalloc 档放大 mmap 路径; 如实报告 |

## 5. 夜窗预算（每夜 23:00-09:00, 10h）
- 构建 3 档(普通/lockdep/syz)并行后台 ≈ 1h 机时(不占 token 窗口策略: 派 qemu-exec 后轮询)
- dev 切片 ≤2 个/夜; review 1:1 跟随; 中期测量 ≤1h; syzkaller 挂机不占交互
- 08:30 硬停新实验, 收尾(wrap-up)至 09:00; 进行中超 20min 的构建可延至完成但结果只入 results 不再派生
