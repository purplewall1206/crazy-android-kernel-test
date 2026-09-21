# G2 —— perfetto trace + SQL 定量（EVAL G2 / M8.T2，2026-09-21 当日闭环）

- 内核: **6.18.32-gb9541335a554（#93，tag corten-r07-a5，T1c 池+A5+M6 全量）**，与 run5 G1 终验
  同一 bzImage（bzimg/r07-t5run5/bzImage-6.18.32-gb9541335a554-93-t5run5，sha256 24a33880…55ab33）。
  注: 任务给的重启路径 bzimg/r07-t5final/bzImage 实为 #90（g2639d3294b9d），与"内核=b9541335a554"
  的身份断言及 run5 G1 判定件不符，故按身份断言选 #93 重启（tmux vm-t5run5, port 10026, corten=on
  mitigations=off kunit.enable=0 log_buf_len=64M, trixie-m4t12.img 8vCPU/4G/KVM, 9p+zram lz4 2G 复刻）。
- 负载: 动态 mmbench_dyn（sha 38304f06… = run3/4/t5final/run5 同件），LD_PRELOAD
  corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1（strict MODE 进入实证 + fork-probe OK）。
- 采集: guest 内 perfetto **tracebox v58.2**（/home/ppw/tools/perfetto/linux-amd64 同件拷入），
  cfg = sched + mmap_lock(3) + tlb_flush + ipi(3) + mm_page_alloc/free + **corten kprobe×7**
  （g2c_{txn_begin,txn_finish,lock_range,map,mark,unmap,zapw}，脚本注册、cfg 按普通 ftrace 事件使能；
  v58 原生 kprobe_events 配置为自动命名故弃用）。每腿 19s 窗、bench 15s（min_seconds=15）。
- 函数级: perf record -F 1999 **--call-graph fp**（子进程模式；本 6.18 构建在 paranoid=2 下 -a 系统级
  perf_event_open ENOENT，同 perf1 先例用子进程模式）+ perf report --stdio（flat self% + callers）。
- SQL: 宿主 trace_processor_shell（v58.2），查询件 sql/q00-q07（本目录）。
- 噪声声明: 测量窗宿主 load≈8（邻道 basecheck/m5t1a 系/m6t2/syz VM 在跑，含当日其它班的活跃
  VM，未 SIGSTOP）；判定一律同 boot 同参 T0 vs BASE 相对口径（对恒定背景负载稳健）；guest 内
  sched/计数器显示 bench 独占 8 vCPU。
- 环事实: ipi_raise 等 ipi/* tracepoint 在 x86 上静默（IPI 成本经 perf 的 x2apic_send_IPI 自样本与
  tlb_flush 事件承担），如实登记。mmap_lock tracepoint 在本 6.18 无 usecs 等待参数 → 锁等待时长
  不入 trace，竞争面由 acquire success 率 + perf 的 osq/rwsem 自样本承担。

## 0. 结论一句话

**两问都有了事件级答案**: ①mmap-pf 残差是**加性地板**——两次独立配对读数的 T0−BASE 差都是
**≈ +207µs/op（per-thread 墙钟）**（不随 BASE boot 漂移缩放），构成 = 每 op 4.7 次事务 + 3.8 次 mark + 0.95 次
zap-window + 写锁段 1.9 次/op（BASE 1.55）+ ranged(4) flush 2.0 次/op（BASE 1.6），profile 上
corten 独有 self 仅 3.5pp（zap_window 2.56 + xas_load 0.61 + txn_meta_drop 0.33），其余两臂同形
（IPI/唤醒/切换）= §4.6"机制成本地板"的事件级实证; ②unmap-virt +1133%（±）的大胜**与 TLB 无关**
（T0 窗内 tlb_flush 全部 34 次 ≈ 0.0001/op，BASE 同样 ≈0——perf1 风暴已灭），来源是**池命中把
每 op 的 mmap_lock 写锁获取从 2.02 次/op 砍到 0.27 次/op（7.6×）**：MAP_FIXED refill 复用 parked
VMA（全窗 g2c_map 仅 20 次 vs 5.1M ops，legacy 每 op 都要 maple-tree 建删 VMA）、chunk 释放走
park 不拆 VMA——BASE 的 per-op 真实 VMA 机器（mas_wr_store/__split_vma/kmem_cache/
vms_complete_munmap_vmas ≈8% self + rcu_preempt 4862 切片）整层从关键区消失。

## 1. 臂读数（同 boot #93，未加 tracer 的干净腿）

| 场景 | 臂 | ops/15s | ops/µs | 判定 |
|---|---|---|---|---|
| A mmap-pf low t8（seed 20261979）配对1 | BASE | 208,735 | 0.00174009 | — |
| | T0 | 153,639 | 0.00128055 | **-26.4%** |
| A 同上 配对2 | BASE | 121,116 | 0.00100948 | — |
| | T0 | 100,107 | 0.000834406 | **-17.3%** |
| B unmap-virt low t4（seed 20263969）配对1 | BASE | 1,668,557 | 0.0542194 | — |
| | T0 | 7,612,390 | 0.664122 | **+1125.0%** |
| B 同上 配对2 | BASE | 1,531,660 | 0.0504666 | — |
| | T0 | 7,214,402 | 0.626944 | **+1142.3%** |

- A 两配对中位 **-21.9%**，落在 REPORT §4.3 登记的 -14~-22% 家族带宽（perf2a 补充至 -31%）内。
- B 两配对中位 **+1133%**，与 run5 终验 +1156.14% 同核同级（G1 判定复现）。
- **加性地板发现**: 配对1 BASE=574.7µs/op、配对2 BASE=990.6µs/op（BASE 两腿相差 1.72×，boot 内
  漂移），但 T0−BASE 差分别为 **+206.2 / +207.8 µs/op（per-thread）**——残差是每 op 一个 ≈0.21ms/op（per-thread）的常数税
  （不是乘性回退; per-thread 墙钟口径）。这解释了家族带宽为何"比率漂移、判定稳定"（perf2.md 的 -15~-31% boot 跨度）。
- 加 tracer 的腿全面降速（kprobe 打在 fault 路上，BASE 也吃 2× 降速: A-base-r1 0.00085 vs 干净
  0.00174），故 trace 只用于事件形态/计数，臂比值一律取自干净腿。

## 2. 场景 A（mmap-pf t8）——残差的函数级证据链

### 2.1 每函数 self%（perf3 flat，干净腿）

| self% | T0 函数 | BASE 函数 |
|---|---|---|
| 33.34% | x2apic_send_IPI | 28.80% x2apic_send_IPI |
| 21.03% | _raw_spin_unlock_irqrestore | 25.95% 同左 |
| 15.88% | finish_task_switch | 17.83% 同左 |
| 8.18% | smp_call_function_many_cond | 7.28% 同左 |
| **2.56%** | **corten_arena_zap_window** | 2.10% osq_lock |
| 2.54% | __send_ipi_mask | 1.67% handle_softirqs |
| 1.72/1.64/1.61% | osq_lock / rwsem_down_write / spin_on_owner | 1.65% do_user_addr_fault |
| **0.61%** | **xas_load**（每 fault txn_owned xa 检查） | 1.26/1.10% rwsem 系 |
| **0.33%** | **corten_txn_meta_drop**（perf1b 整块释放） | — |

两臂 top-4 块完全同函数（IPI 33.3/28.8、解锁唤醒 21.0/25.9、切换 15.9/17.8、CSD 8.2/7.3）→
**"终态 profile 两臂同形，差异进入 IPI/调度噪声地板"（perf1 §4）在 #93 上复验成立**。
corten 侧在榜 self 合计 **3.5pp**（zap_window 2.56 + xas_load 0.61 + txn_meta_drop 0.33）；
query/mark/lock_range/txn_begin 经内联折叠进 do_user_addr_fault（1.68 vs 1.65，与 BASE 持平）
与 rwsem 系——**fault 送达通道本身两臂等价**（do_user_addr_fault self 持平 + page allocs 3.4/op
vs 3.1/op 持平），加税发生在**路由层**（下节计数）。

### 2.2 每事件计数（trace，窗内速率归一）

| 指标（每 op） | T0（A-t0-r2，g2c 窗 13.02s / 79,782 窗op） | BASE（A-base-r1，15.58s / 102,085 op） |
|---|---|---|
| g2c_txn_begin/finish/lock_range | **4.74 /op** | 0（无事务） |
| g2c_mark（每 fault 填 mark） | **3.79 /op**（≈4 页/op） | 0 |
| g2c_zapw（窗 zap，0 内容走懒路径） | **0.95 /op** | 0 |
| mmap_lock 写获取 | **1.90 /op** | 1.55 /op |
| mmap_lock 读获取（success%） | 0.95 /op（100%） | 1.25 /op（**75.6%**，2.4 万次失败） |
| tlb_flush 总数 | 2.44 /op（14,961/s） | 1.81 /op（12,319/s） |
| ├ ranged(4) | 2.01 /op | 1.61 /op |
| └ fullmm(-1) | 0.22 /op（swapper 背景 1,334/s） | 0.19 /op（背景 1,232/s） |
| mm_page_alloc | 3.4 /op | 3.1 /op |

判读:
1. **每 fault 事务语义直接可见**: 4 页/op ≈ 3.8 mark ≈ 4 个 fault 事务，外加 1 次窗 zap 事务 =
   4.7 txn/op；txn 内联的 lock_range（desc rwlock write, BH）+ xas_load（0.61% self）+ query/mark
   就是"per-fault transaction"逐字段的 trace 实体。
2. **T0 每 op 多 0.3-0.4 次 ranged(4) flush**（2.01 vs 1.61）+ 0.35 次写锁段（1.90 vs 1.55）——
   take/park 生命周期附加的 flush/锁段，量级与 ≈0.21ms/op（per-thread）的加性税一致。
3. **无风暴**: mmbench 线程 fullmm 每线程 27-60 次/13s（背景级）; swapper 名下的 fullmm 两臂同率
   （1,334 vs 1,232/s，boot 背景非 MODE 产物）。perf1 修复在 #93 上有效。
4. **无竞争劣化**: T0 mmap_lock success 100%（读 75,766/75,766），BASE 读 success 75.6%——MODE
   没有把锁挤爆，残差不是竞争回退（§4.6"非锁竞争退化"的事件级判据）。

### 2.3 残差 -14~-22% 的归因分摊（结论）

| 成分 | 证据 | 量级 |
|---|---|---|
| 每fault事务机器（fill_upper+lock_range+xa+query+mark） | 4.7 txn/op、xas_load 0.61% self、mark 3.79/op | **主要项**（对应 §4.6-1"纯通道 -7~11%"） |
| take/park 簿记 + 窗 zap | zapw 0.95/op self 2.56%、txn_meta_drop 0.33%、写锁段 +0.35/op | **次要项**（对应 §4.6-2 3-5%，perf2a 薄 peek 前形态） |
| 附加 ranged(4) flush +0.4/op | IPI 族 self T0 略高（33.3+8.2+2.5 vs 28.8+7.3+1.6） | **小项** |
| 两臂共同地板（IPI/唤醒/切换 ≈70%） | top-4 同形 | 不计入残差 |

## 3. 场景 B（unmap-virt t4）——+1000% 的机制来源实证

### 3.1 TLB 侧: 无风暴、无 flush（大胜与 TLB 无关）

| | T0（B-t0-r1，g2c 窗 3.14s） | BASE（B-base-r1） |
|---|---|---|
| tlb_flush 总数 | **34 次/3.14s = 11/s（≈0.0001/op）** | 窗内 43 次（ranged(0) 32 + fullmm 11） |
| 对照 perf1 风暴（#83 修复前） | 82,942 次/3s（pages:-1 主导） | 95 次/3s |

perf1 的风暴在 #93 上已死; 本格两臂 flush 都≈0 → +1133% **不是** flush/TLB 侧收益，只能来自
VMA/锁路径。

### 3.2 锁与 VMA 层: 池命中把关键区砍掉一层

| 每 op | T0 | BASE | 倍数 |
|---|---|---|---|
| mmap_lock acquire 合计 | **0.27 /op**（91.6k/s） | **2.02 /op**（171k/s） | BASE 7.6× |
| 新建映射（g2c_map） | 全窗 **20 次** / 5.1M ops ≈ 0 | 每 op 都走 __mmap_region 建新 VMA | ∞ |
| 上下文切换 | 154 /s（osq 忙旋） | 2,151 /s（慢路径睡眠-唤醒） | BASE 14× |
| rcu_preempt 切片 | （B-T0 窗内无） | **4,862 切片/10.1s**（legacy munmap RCU 机器） | — |

T0 路由形态（trace 计数）: 每 op ≈ 1 txn + 1 zapw（0 内容懒路径，不开 gather）+ 2 次
corten_unmap（chunk 释放）+ 0.78 mark，而 **g2c_map≈0、pool_misses 仅 +8** = 5.1M 次 op 全部
命中既有 parked/arena 窗（MAP_FIXED refill 复用 parked VMA，early-take ret 2 原地复用）。
arena_stats 侧本格 munmap_releases/pool_parks 不走数（该 bench 形态的 release 不落 RELEASE 契约
路径; mmap-pf 格 parks==releases==ops 精确对账见 §2/§4），池命中判据以 g2c_map≈0 + misses≈0 为准。

### 3.3 每函数 self%（perf3 flat）

| self% | T0 | BASE |
|---|---|---|
| mmap_lock 写竞争 | 34.04% osq_lock + 15.27% rwsem_spin + 2.65% down_write_killable = **52.0%** | 48.00% + 14.25% + 0.68% = **62.9%** |
| 唤醒/切换 | （低，忙旋） | **8.27% spin_unlock_irqrestore + 3.03% finish_task_switch** |
| 每 op 真实工作 | xas_load 2.76 + txn_begin 2.26 + zap_window 1.84 + txn_meta 1.81 + mmap_route 1.69 + txn_finish 1.67 + unmap_chunk 1.05 + ptdesc_get 0.99 ≈ **14.1%（corten 系）** | mas_wr_node_store 2.22 + __vma_start_write 1.24 + mas_walk 1.01 + mas_store 0.64 + __mmap_region 0.57 + __split_vma 0.56 + mas_prealloc 0.54 + kmem_cache 0.97 + vms_complete_munmap 0.48 ≈ **8.2%（legacy VMA 机器，且只喂 1/5 的 ops）** |

**机制链**: 两臂都被 mmap_lock 写锁串行化（T0 也还有 52% 旋等——胜利不是"消灭了锁"）; 但 BASE
每个临界区装着整套 VMA 编辑器（maple tree 增删/分裂 + VMA 分配释放 + vms_complete_munmap_vmas
+ RCU），T0 临界区只剩描述符簿记（xas/txn/meta ≈ 8-9% self）。关键区长度差 ~5-7× 直接兑换成
吞吐差 +1125~+1142%（同锁、同 4 线程、同 15s）。**"无 TLB 风暴 + 池命中"两个来源中，本格真正
变现的是池命中（VMA 层消失 + 关键区缩短）; 无风暴是 perf1 修复的维持条件（否则 MODE 侧自己
就会把收益吐回去，run3 的 -83.8% 即反例）。**

## 4. A6"SQL 三件套"交付状态

| 约定件 | 状态 |
|---|---|
| mmap_lock_contention | sql/q02（计数+成功率; 本 6.18 tracepoint 无 usecs 等待参数，等待时长以 perf osq/rwsem self 承担——差异如实登记） |
| fault_latency | sql/q06（页分配/释放每 op 持平）+ g2c_lock_range 4.7/op（每 fault desc 锁）+ do_user_addr_fault self 持平 |
| sched_breakdown | sql/q04（每线程 on_cpu/rq_wait; A-BASE 8 线程 rq_wait≈1.0s/线程、rcu_preempt 9899 切片; B-BASE 62% on_cpu + 14× 切换率） |

## 5. 产物清单（本目录）

- traces/: A-t0-r1 (36MB, 环回卷存 5.04s), A-t0-r2 (86MB/13.02s), A-base-r1 (41MB/15.58s),
  B-t0-r1 (134MB/3.14s), B-base-r1 (134MB/10.12s), smoke-probe{,2,3}（kprobe 链路冒烟）。
  环回卷说明: 131072KB 环满后保新弃旧，所有计数以 q07 span 归一; 臂比值不用 trace 腿。
- evidence/perf/: {A,B}-{t0,base}-perf3.{data,flat.txt,callers.txt}（函数级证据，§2.1/§3.3 表源）。
- evidence/json/（每腿 mmbench JSON）、evidence/stats/（每腿 arena_stats 前后快照）。
- sql/: q00_discover（arg key 发现）、q01_events、q02_mmap_lock、q03_tlb_flush、q04_sched、
  q05_kprobe、q06_pagefault、q07_span、run_summary.sh（一键五 trace 汇总）。
- logs/: battery.log（trace 腿全输出）、battery-perf3.log、summaries.log（全部 SQL 结果）。
- g26（ssh 助手）、../g2 采集脚本在 9p: /home/ppw/bench/share/g2/{cfg.pbtxt,run_leg.sh}。

## 6. 边界与诚实声明

- 未跑满 5 臂×2 contention×多线程网格; 本班只采任务指定的两个机制格（A: mmap-pf low t8,
  B: unmap-virt low t4），各 2 配对（trace+perf），非 G1 判定口径的替换件。
- 单 boot 单镜像; 邻道 VM 负载（host load≈8）未隔离，判定依赖同 boot 相对口径。
- B 格 pool_parks/munmap_releases 计数器在本 bench 形态不走数（如上），池命中判据由 trace 的
  g2c_map≈0 + pool_misses≈0 承担——与 mmap-pf 格的 parks==releases==ops 对账通道不同，如实分述。
- A-t0-perf/A-base-perf 首轮 perf 腿因 `-g fp` 解析错误作废（bench JSON 仍有效并已用作配对1），
  重跑腿为 *-perf3; 过程记录在 logs/battery.log。
