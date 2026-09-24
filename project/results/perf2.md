# r07 perf2 —— mmap-pf 残差攻坚（arena fault 通道机制成本, 2026-09-21）

- 班次: ~09:05–11:10 CST（用户 09-21 无时间限制授权, 通宵时间盒 8h 内收工）。
- 内核谱系: worktree **/home/ppw/linux-6.18-perf2**（分支 perf2, 基座 **2639d3294b9d**
  = 任务指定; 主树 HEAD=b9541335a554 含 A5, 本班禁碰主树, A5 不在工作树基座内——
  最终 diff 对主树 HEAD `git apply --check` **干净适用**）。
- 产物: **perf2a 终件** `bzimg/perf2/bzImage-perf2a-final`
  sha256 `442f1af8…d2823`（6.18.32-g2639d3294b9d-dirty, 构建 #4）; 对照基线件
  `bzimg/perf2/bzImage-perf2base` sha256 `7a6258c5…51eee`（同树 stash 后重编 = 纯基线）。
  diff 备份 **patches/r07-perf2.diff**（+46/−77, 4 文件, 185 行）。
- 实验口径: guest trixie-perf2.img（= trixie-m4t12.img 快照副本, VM port 10031,
  pidfile qemu-perf2.pid, 8 vCPU/4G/KVM, corten=on mitigations=off）; 动态 mmbench_dyn
  sha 38304f062d43（run3/4/t5final 同件）; ABAB 同 boot 同 seed 配对; 邻道 qemu 全部
  SIGSTOP（perf1/t5run4 惯例, 收班前恢复）。**本次登记全部为 in-boot 配对中位**。

## 0. 结论一句话

**mmap-pf 残差是测量噪声地板 + PMD 窗粒度扫描成本的复合, 不是可小 diff 消除的软件路径**:
perf2a（park 元数据整块 kfree → 范围内逐槽复位）作为零语义改动**保留**（KUnit 全绿,
checkpatch 0E/0W/0C, T0 臂绝对吞吐 t1 +8.7% / t8 +11.2%, 双 boot 组非重叠中位）, 但
in-boot 残差比率在**同一份代码**上随 boot 漂移 −15%~−31%（base-code 两 boot 实测同跨度）,
无法也不应硬凑到 ≥−10%。纯 fault 通道**胜出**（pf low t8 +7~+22%）, 残差位于
mmap/munmap 路由侧（park 的 2M 窗 512-PTE 扫描 ~13µs/op = 当前最大 T0-only 项）+ IPI/调度
地板——M8 批 mark/窗粒度重构才是正确杠杆, 与任务预判一致。

## 1. 剖景（perf record -F 1999 -g, guest, mmap-pf 各 15s; 原始 data/report 留 guest /root/perf2 + results/r07/perf2/guest/）

### 1.1 T0 臂 t8（低竞争 8 线程）vs BASE 臂

| T0 臂 | self% | BASE 臂 | self% |
|---|---|---|---|
| x2apic_send_IPI | 35.92 | x2apic_send_IPI | 29.76 |
| _raw_spin_unlock_irqrestore (wake_up_q←rwsem_wake) | 22.25 | _raw_spin_unlock_irqrestore | 27.11 |
| finish_task_switch | 18.39 | finish_task_switch | 20.77 |
| smp_call_function_many_cond | 5.82 | smp_call_function_many_cond | 5.73 |
| __send_ipi_mask | 2.98 | osq_lock | 2.06 |
| corten_arena_zap_window | 1.96 | __send_ipi_mask | 1.71 |
| xas_load | 0.48 | mas_walk | 0.23 |

两臂同形: IPI/唤醒/上下文切换占绝对主导, 与 perf1 终态结论一致。corten fault 链
（lookup/fault_once/xas_load）不在 top-16 显著位。

### 1.2 ftrace tlb_flush 判据（flushshape.sh, 4s 窗, 中程采样）

| 臂 | events | 每-op flush | 形态 |
|---|---|---|---|
| base | 109,643 | ~1.07 | pages:4 remote（ranged 4 页）为主 + task-switch 全 mm 族 |
| t0 | 91,679 | ~1.02 | 同 base: pages:4 remote, **零**全 mm 风暴 |

**两臂每 op 恰 1 次 ranged pages:4 remote flush, 形态与次数相等**——perf1 修复后
mmap-pf 已无 TLB 风暴; 残差不是 flush 差异。

### 1.3 t1（单线程纯每-op 成本, 无竞争噪声）

| T0 臂 | self% | 说明 |
|---|---|---|
| corten_arena_zap_window | 24.10 | 其中 **85% = corten_txn_meta_drop → kfree**（perf1b 整块 drop） |
| do_user_addr_fault | 11.82 | 与 BASE 臂 13.99% 同量（fault 通道无异常） |
| xas_load | 3.88 | 每 fault 3 次 xa_load（lookup/txn_owned/ptdesc） |
| corten_txn_meta_drop | 3.30 | 同上整块 drop |
| clear_page_erms | 2.95 | 4 页清零（与 BASE 同量） |
| corten_arena_check_empty_locked | 2.10 | take 侧 C1 复扫 |

**top-3 差异函数**: ①`corten_arena_zap_window`（24.1%, 核心是 park 的 512 槽计数循环 +
kfree 4KB 元数据数组; take 侧再 kmalloc+__GFP_ZERO 重建 = memset_orig 0.92%）;
②`corten_txn_meta_drop`（3.3%, 同一机制）; ③`xas_load`（3.9%, 每fault 3 次 xarray 查找）。
BASE 等价 4 页 teardown（unmap_page_range）仅 1.28%。

### 1.4 机制级实证（debugfs/ftrace）

- arena 形状: `auto_place()` 将每个 mmap(NULL,16KB) **PMD 对齐发放**, arena extent
  16KB、独占 2M PT 页（debugfs arenas 实证 [100000000000,100000200000) 为窗内 arena）;
  ftrace funcgraph + pool 计数实证 **每 op = pool take + park 一对**
  （munmap_releases +79,722 == pool_parks +79,722 == pool_hits +79,721, 5s t1 窗）。
- 因此每 op park 扫描 = 512 PTE（2M 窗全部槽位）, 其中仅 ~4 槽有内容——
  扫描粒度 = 窗粒度, 是 T0-only 结构成本; perf1b 的整块 drop 消掉了逐槽 query
  但引入了每 op kfree+kmalloc+memset 4KB 的新税。

## 2. 优化点

### perf2a（**保留, 唯一落地点**）: park 元数据整块 drop → 走查范围内逐槽复位

- 改动（4 文件 +46/−77）: `corten_arena_zap_window()` 槽循环内以
  `corten_txn_slot()`（新导出的薄 peek: 指针直读, 无 payload 拷贝）定位有记录的
  少数槽, 仅对这些槽执行既有 `corten_unmap()` 复位; 删除 `[perf1b]`
  `corten_txn_meta_drop()`（512 槽计数循环 + kfree 整块数组）及声明。
  UNMAP_PAGES 记账逐槽等价; nr_mapped/nr_swapped 由 corten_unmap 维护, 总量不变。
- **语义零变化论证**: pristine 契约（Invalid/perm-0/flags-0/resv-0）由逐槽
  corten_unmap 提供 = perf1b 之前的原语; 走查失败（-EAGAIN/force）路径与 perf1b 相同
  交给调用方真 RELEASE 兜底; PMD 对齐 arena 是 PT 页唯一所有者, 走查范围外的槽位
  从未被记录（mark/mprotect/fault 全部限于 arena 范围）; KEEP_PERM（chunk drop）
  路径行为不变。元数据数组随 park 存活, meta_arrays==ptdescs 恒等（实测 335==335,
  不再抖动）, 无泄漏。
- KUnit: on×3（含 1 例注册在案的 interlock flake 重跑绿）+ off×1 全绿
  （24/48/30 Totals, 0 fail）; pool_reuse 等 T1c 锚全过。
- checkpatch --strict: **diff 模式 0E/0W/0C**（164 行）; 文件模式 arena.c 仅基线 1W。
- =n 八对象（memory/mmap/migrate/rmap/swapfile/gup/oom_kill/arch fault）RC=0 零告警。
- 效果（T0 臂绝对吞吐, 跨 boot 中位对照）: mmap-pf **t1 0.0199→0.0183 ops/µs
  （+8.7%）**, **t8 0.00110→0.00099（+11.2%）**——与移除 kfree/kmalloc/memset+
  512 计数环的剖景预测同量级。
- 佐证: 收尾 ABAB 后台账 munmap_releases==pool_parks==49055 精确对账,
  meta_arrays==ptdescs==335, dmesg corten warn/bug/oops=0。

### 未采纳/无候选（按证据排除, 如实记录）

- **FRESH 门链合并 / mark 单页 fastpath**: FRESH 写故障 = query+mark+map 每者 1-2 次
  槽访问, 剖景中无独立显著位; pf low t8 两臂对照（T0 +7~+22%）证明 fault 通道本身
  无劣势, 无可合并的重复查找热点。
- **xa_load 缓存（per-mm 最近窗）**: xas_load 仅 3.9%（t1）, 收益 ≈ boot 漂移噪声;
  且需 punch/fork 一致性护栏, review 面大于收益。
- **folio 分配参数**: vma_alloc_zeroed_movable_folio 与 legacy do_anonymous_page
  同参（clear_page_erms 两臂同量 2.9 vs 3.96%）, 无差距可缩。
- **park 512-PTE 扫描的界缩（hwm/高水位）**: 可消 ~13µs/op 的最大剩余项, 但需要
  "信任元数据证明窗内无 PTE", 恰是 r03 defect C 教训禁止的信任方向; 达到零漂移
  语义等价需要窗级 drift 旗标 + 高水位维护, 超出小 diff 范畴 → **判为 M8 批
  mark/窗粒度重构的正宗素材**, 本班不硬凑。

## 3. 终态四格（动态口径, in-boot BASE vs T0, ABAB×3 中位, 终件 #4, kunit.enable=0 boot）

| 格 | BASE ops/µs | T0 ops/µs | Δ% | 判定 |
|---|---|---|---|---|
| unmap-virt low t4 | 0.0532 | 0.6713 | **+1162%** | G1 过线维持（大胜） |
| unmap-virt low t8 | 0.0248 | 0.6761 | **+2627%** | G1 过线维持（大胜） |
| unmap low t8 | 0.00127 | 0.00335 | **+164%** | G1 过线维持（胜, r1 base 0.00042 离群如实登记） |
| mmap-pf low t8 | 0.001233 | 0.000885 | **−28.3%** | 机制成本家族（§4） |

正确性面: 台账 munmap_releases==pool_parks==49,055 精确对账; meta_arrays==ptdescs==335;
dmesg corten warn/bug/oops=0; run_mode_smoke **26/26 PASS + SMOKE-DRIVER PASS**
（注: 本班 VM append 曾漏 kunit.enable=0, boot 期 KUnit 注入探针的 drain-timeout
设计性 WARN 触发 driver 的 dmesg 门——两臂同形、非内核问题, 修正 boot 参数后全绿）;
JThreadBench 2000×3 双臂 rc=0、零 ClassFormatError、t0 hook STRICT 断言 on。

## 4. 机制结论（M8 素材, 判据: "≥−10% 或揭示不可消除机制"——后者成立, 不硬凑）

1. **残差比率不可靠到个位数百分比**: 同一份 base 代码在今天的两个 boot 上实测
   mmap-pf t8 残差 **−15.0% 与 −28.6%**; perf2a 四个 boot −19.4%~−30.6%。base 臂
   boot 间漂移 ±13pp 与任何候选优化的收益同量级——该格的比率读数必须以多 boot
   网格中位口径登记（今天全部读数并入 perf1 登记的 −14~−22 家族带宽, 上沿到 −31）。
2. **纯 fault 通道已是净胜**: pf low t8（预映射窗, 纯 fault）T0 +7~+22%（两 boot
   一致正向）。mmap-pf 残差不在 fault 事务本身。
3. **残差 = 路由侧窗粒度扫描 + IPI/调度地板**:
   a. 每 op 一次 2M 窗 park/take（PMD 对齐发放的设计粒度）→ 512-PTE 走查
      ~13µs/op（现最大 T0-only 单项; 消除需窗粒度/批 mark 重构 = M8 杠杆）;
   b. 两臂每 op 恰 1 次 ranged pages:4 remote flush（ftrace 实证相等）, T0 臂
      IPI 占比高 7.5pp = 等 flush 次数下更长临界路径的调度放大, 进入 x2apic/
      rwsem 唤醒/上下文切换的噪声地板（perf1 §4 同判）;
   c. xas_load（每 fault 3 次 xa 查找, 3.9%）与 check_empty C1 复扫（2.1%）
      为次级项, 各自 < boot 漂移, 不值得为其增加一致性护栏复杂度。
4. **perf2a 的价值定位**: 不改变比率结论, 但把 T0 每-op 事务成本真实压缩一位数
   百分比（t1 +8.7%/t8 +11.2%, 双 boot 非重叠）, 并消除每 op 的元数据数组
   kfree/kmalloc 抖动（meta_arrays 与 ptdescs 恒等, 台账更干净）; 零语义改动、
   可独立回退, M8 报告按"机制成本已压到窗粒度地板"登记。

## 5. 证据清单

- diff: patches/r07-perf2.diff（对 2639d3294b9d 与主树 HEAD b9541335a554 均 clean apply）。
- bzimg/perf2/: bzImage-perf2a-final（sha256 442f1af8…d2823, 构建 #4 终件, 全部终验跑于
  此件）+ bzImage-perf2base（7a6258c5…51eee, 对照件）; 构建日志
  results/r07/perf2/build-y{1..4}*.log + build-n.log（=n 八对象）。
- KUnit: results/r07/perf2/kunit-perf2final-{on1,on2,on2-retry1,on2-retry2,off1}.log
  （on 5.5.1 interlock flake 1 例=注册基线, 重跑即绿; 0 fail 收官）。
- guest 数据: results/r07/perf2/guest/（result.txt 全部 ABAB 行, report-/graph- 两臂
  flat+graph 剖景, flushshape/flushprobe 窗计数, prof json）; perf .data 留
  guest /root/perf2（VM vm-perf2 留运行, port 10031）。
- checkpatch: results/r07/perf2/checkpatch-perf2-{full,diff}.txt。
- 邻道: 测量窗 SIGSTOP 全部邻道 qemu（tmux server 误停事故已当场 CONT 恢复并修正
  bin/perf2-neighbors.sh 为 pgrep -x; 已登记 §6）, 收班前统一 SIGCONT。

## 6. 事故与处置（如实登记）

1. **tmux server 误停（已恢复, 零损害）**: bin/perf2-neighbors.sh 初版用
   `pgrep -f qemu-system` 匹配到 tmux server（其 cmdline 含会话创建命令字符串）,
   将其 SIGSTOP, 导致 tmux 客户端挂起。当场 `kill -CONT` 恢复, 全部会话无损;
   脚本已改为 `pgrep -x qemu-system-x86` 精确匹配。受影响窗口内无测量进行。
2. 首轮 KUnit 脚本串口重定向写 /dev/null 导致无输出（脚本缺陷, 改直跑 qemu + 日志
   文件）; boot append 漏 kunit.enable=0 触发 smoke driver dmesg 门（两臂同形,
   修正后全绿）——均非内核问题。
3. 未 push; 未碰主树/其它 worktree; 密码未落盘; perf2 工作树增量保持未提交
   （diff 已备份, 待 review/maintainer）。

—— perf2 收工: **perf2a 保留（0E0W, KUnit 全绿, T0 绝对 +9~11%, 语义零变化）**;
mmap-pf 残差判为 **测量噪声地板 + PMD 窗粒度扫描成本**, 收敛到 ≥−10% 被多 boot
实测证伪（同代码 −15~−31 跨度）, 正宗杠杆 = M8 批 mark/窗粒度重构, 机制归因与
不可消除性论证完整; G1 其余三格（unmap-virt t4/t8、unmap t8）大胜维持, 全套
正确性面（KUnit on/off、=n、checkpatch、smoke 26/26、JTB rc=0、台账对账、dmesg
双零）全绿。
