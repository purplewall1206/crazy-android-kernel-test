# G7 —— 内存开销定量测量（M8.T-g7，2026-09-21 当日闭环）

- 内核/VM: 与 G2/run5 同件 —— `6.18.32-gb9541335a554 (#93, bzimg/r07-t5run5)`，
  tmux `vm-t5run5`（port 10026），trixie-m4t12.img 8 vCPU/4G/KVM，zram lz4 2G。
  先 corten=on 臂测量 → **同 bzImage 同 qemu 参数重启仅换 `corten=off`** 跑对照 → 再还原
  corten=on（已还原，enabled=1，现场交还）。执行时段 = r07 连续工作段（D14 精神，
  与当日 G1 终验/G2 同模式）；每步起止时间见 `g7-raw/*.txt` 时间戳（guest 时钟 UTC）。
- 负载: 自制受控负载 `g7load`（/mnt/hostshare/g7load.c，guest /root/g7load，动态链接），
  stdin 命令循环 `map <MB> | touch | touch1 | unmap | rss | halt`（fifo 控制，EOF 续读）；
  MODE 进入 = `LD_PRELOAD=/root/corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1`
  （sha256 4846da71… 与 run5 meta 记录同件；strict rc=0 + "MODE on" 双验证）。
  对照臂（corten=off）同二进制不挂 hook（hook 在 off 内核无效果，等价 MODE-fallback）。
- 仪器: debugfs `{stats,arena_stats,arenas,dump}` + smaps/smaps_rollup/status/maps +
  meminfo + slabinfo + cgroup-v2 `memory.{max,current,swap.current,events}`。
  窗口数以 `dump`（逐 ptdesc 行 va_base 落窗计数）为准，不受系统级 legacy 页表 churn 干扰。
- 过程教训（如实登记）: ①g7load 首版误编 `-static` → LD_PRELOAD 不生效、进程纯 legacy
  （arenas=0 暴露），重编动态后 strict MODE 才成立; ②进程先在 root memcg 触页再挪 cgroup
  不迁移 charge（memory.current=0 假象）→ off/on 两臂均改为"先入组后触页"。
- 原始证据: `results/r07/g7-raw/`（14 件: on-idle-base / on2-run1[含 res64 全景] /
  on2-run2 / on2-res256b / on2-res1024[b] / on2-swapped / on2-park64 / on2-park1024 /
  on2-poolcap / dump-resident64 / dump-res1024 / off-battery + 两份早期基线）。

## 0. 结论一句话

**实测开销 = 8,288 B / 2M 窗 = 映射内存的 0.3952%（三档 64/256/1024 MB 全部同一比值，
与理论 8,272 B ≈ 0.3944% 到 0.4% 宣称差 ≤0.8%），resident_pages 与映射字节精确一致
（16384/65536/262144 页零误差）；换出态 metadata 全额驻留不变、三方账目
（ledger swapped_pages == memory.swap.current == smaps Swap）闭合到字节
（261223p = 1,069,969,408 B）；池 idle 态 park 保留 PT 页、释放 meta array
（开销减半至 0.1998%），池上限 16 实测命中（第 17 个 park 被拒、整块释放、pool_over=1）；
同负载 legacy 对照 = 4,096 B/2M（0.195%），CortenMM 使页表侧基础设施 ×2.02
（+0.20% of mapped），但同 memory.max 压力下 corten 臂 shrinker 换出 261,223 页零 OOM、
legacy 臂 OOM-kill。退出账目两轮全零（free_untracked=0 / legacy_drift=0，ptdescs 精确回落）。**

## 1. 三态 × 三档数字表（corten=on 臂）

理论比 = (PT 4,096 + meta array 4,096 + desc 80) / 2,097,152 = **0.3944%**（任务口径 ≈8.2KB/2M ≈ 0.4%）。
实测比按 desc 实际入桶 kmalloc-96 记 96 B: (4,096+4,096+96)/2,097,152 = **0.3952%**。

### 1.1 常驻态（map N + touch 全页, MODE 进程单 arena）

| 映射 | resident_pages（ledger） | 映射字节 | 窗口数（dump 逐窗验证） | metadata 字节（实测=窗×8,288） | 实测开销比 | 理论比 | 对照基线（legacy 同负载） |
|---|---|---|---|---|---|---|---|
| 64 MB | **16,384 = 64MB/4K 精确** | 67,108,864 | 32（dump 数出 32） | 265,216 | **0.3952%** | 0.3944% | PT 32 页 = 131,072 B = 0.1953%（无 meta/desc） |
| 256 MB | **65,536 精确** | 268,435,456 | 128（ptdesc 算术 346+32parked+128=497✓） | 1,060,864 | **0.3952%** | 0.3944% | PT 128 页 = 524,288 B = 0.1953% |
| 1024 MB | **262,144 精确** | 1,073,741,824 | 512（dump: 672 = 512 驻留 + 160 park✓） | 4,243,456 | **0.3952%** | 0.3944% | PT 512 页 = 2,097,152 B = 0.1953% |

交叉验证（1024 MB 档）: smaps arena VMA `[anon:corten_arena]` 单条 [0x100080000000,0x1000c000000)
= 1 GB，Rss = **1,048,576 kB 精确** = Anonymous；VmPTE 2,752 kB（含本进程全部 PT 页）；
meminfo PageTables 2,780→5,376 kB（arena PT 页**计入** meminfo PageTables，512 页增量清晰可见）；
kmalloc-4k active 632→1,123（+491 ≈ 512 meta 阵列，slab 批处理噪声内）；
kmalloc-96 active +566 / kmalloc-128 +0（@+667 desc）→ **desc 落 kmalloc-96 桶**。

### 1.2 换出态（1024 MB 驻留 + memcg memory.max=512M, shrinker 压力通道 M6.T3）

| 量 | 值 | 一致性 |
|---|---|---|
| swapped_pages（arena ledger） | **261,223 页** | = memory.swap.current = smaps Swap = VmSwap **三方到字节**: 261,223×4096 = **1,069,969,408 B** = 1,048,892 kB |
| RSS 释放 | 1,050,376 → 5,300 kB（释放 1,045 MB ≈ 换出量；Δ≈184 kB = libc/堆等非 arena 页） | ✓ |
| memory.current | 5,959,680 B（≤ max ✓） | memory.events oom_kill = **0** |
| metadata 驻留 | **4,243,456 B 不变**（meta_arrays 853 = 332 legacy + 512 arena + 9 churn；换出不省 metadata） | 换出态开销比 = metadata/换出字节 = 0.3966%（≈常驻态；swap entry 存于 meta `__resv` 8B 载荷内，无边际内存） |
| 对照基线 | legacy 同压（同 bzImage corten=off, 同负载先入组后触页）: **OOM-kill**（oom_kill=1, kill 时 anon-rss 1,310,700 kB 全在驻留, 全部 young 无老化进度；swap.current=0） | 单腿观察, 如实登记 |

### 1.3 池 idle 态（munmap 后 park, 进程存活）

| 场景 | PT 页 | meta array | 驻留开销 | 占原映射比 | 呈现 |
|---|---|---|---|---|---|
| park 64MB（32 窗） | 32 页全留（VmPTE 184 kB 不变） | 32 个全释放 | 32×4,192 = 134,144 B | **0.1998%** | maps: VMA 变 `---p`（PROT_NONE）零 Rss 保留; smaps 无任何行; debugfs arenas 行 vma 指针置 0 |
| park 1GB（512 窗, 换出后 unmap） | 512 页全留（VmPTE 2,100 kB 不变; PageTables meminfo 不降） | 512 个全释放（meta_bytes 853→336 阵列） | 512×4,192 + arena 结构 ≈ 2.15 MB | **0.1998%** | swap 槽全还（swapped_pages 261,223→0, VmSwap→0） |
| 池上限 16 实测（17 个不同 size: 2,4,…,34 MB 各 map+touch1+unmap） | 保留 = 前 16 个 size 合计 **136 窗**的 PT 页（ptdescs 算术 346+136=482 精确✓） | 0（park 即释放） | 136×4,192 + 16×(desc 96+arena 结构) ≈ 575 KB | — | arenas=16; **pool_over=1** = 第 17 个 park 被拒、该 chunk 整块释放（非 LRU victim 驱逐——pool_ejects=0 且保留集=前 16 个） |

**池上限语义**: CORTEN_ARENA_POOL_MAX=16 计的是 **arena（chunk）数不是窗口数**——
单个 1GB chunk park 保留 2.15 MB（实测最大），16 个同规格 chunk 外推上限 ≈34 MB
（未测, 仅界）。halt/exit 两轮全程回收验证: ptdescs 1009→332、482→333 精确回落，
free_untracked=0、legacy_drift=0（两次均零泄漏零漂移）。

## 2. 异常开销排查（理论外实测项全部列出）

| 项 | 理论口径 | 实测/核算 | 备注 |
|---|---|---|---|
| corten_ptdesc 本体 | 任务模型"≈8.2KB/2M"内按 80 B 计 | **sizeof 计算 = 80 B**（rwlock 4+pad4, mm 8, va_base 8, meta 8, refs 4, stale 1, level 1, nr_children 2, nr_mapped 8, nr_swapped 8, magic 4+pad4, rcu_head 16; 无 lockdep 构建已核实 CONFIG_PROVE_LOCKING/DEBUG_SPINLOCK/DEBUG_LOCK_ALLOC 全 off）; pahole 不可用（vmlinux 无 BTF/DWARF, libbpf 报错实证）→ 以 kmalloc 桶实证: +667 desc 时 kmalloc-96 active +566 / kmalloc-128 +0 → **实收 96 B/desc**（16 B 内部碎片, +0.0008%/窗） | kfree_rcu 的 rcu_head 在结构内, 无额外 per-desc RCU 分配 |
| meta array 分配 | 4,096 B 整 | kmalloc-4k 精确一桶, 零碎片 | meta_bytes ledger = 阵列数×4096 与 slab 一致 |
| xarray 节点 | 模型未计 | 全局 PFN xa（1 entry/PT 页）+ per-mm arenas xa（1 entry/窗）+ shrink_aged xa（瞬态）: xa_node 584 B（radix_tree_node slab 实测 objsize）/64 slot → **≤9.13 B/entry, 双 xa ≤18.3 B/窗 ≈ +0.22% of 8,288** | 噪声级; slab 计数被系统级 xa 用户淹没, 无法单臂隔离（如实声明） |
| per-mm 状态 | 模型未计 | `corten_mm_state` 布局核算 ≈216-224 B（2×xarray, mutex, 4×list_head, percpu 指针×2, 游标/锁/RCU）+ 2 个 percpu 分配 + va seg（seg_claims 35→37 观测）≈ **<1.5 KB / MODE mm**，与窗口数无关摊薄 | |
| 池 ledger | park 保留 PT+desc | 实测: park 释放 meta array、保留 PT 页 + desc + arena 结构 + **parked VMA 对象本身**（vm_area_struct 256 B 桶 + maple 节点份额）——`---p` 保留 VMA 在 maps 可见 | parked VMA 使 MAP_FIXED refill 免重建（G2 +1133% 机制来源） |
| meminfo 口径 | — | arena PT 页**计入** PageTables（512 页增量可见），无会计旁路; VmPTE 亦含 parked PT 页 | 排除"开销藏进不可见桶"的疑点 |
| OOM 安全性 | — | 同 memory.max 下 corten 臂 0 OOM 换出 261,223 页 vs legacy 臂 OOM-kill | M6.T3 shrinker 行为红利, 单腿观察 |

**理论外的净额外开销 ≈ desc 16 B 碎片 + 双 xa ≤18.3 B/窗 + per-mm <1.5 KB 一次**
→ 每窗合计 < 0.45% 映射（8,288+~20 B ≈ 0.3978%），"0.4% of mapped memory" 宣称在实测口径下成立。

## 3. 对照基线细节（corten=off, 同 bzImage 同负载）

| 量 | corten=on | corten=off (legacy) | 差 |
|---|---|---|---|
| 1024MB 常驻: meminfo PageTables 增量 | +2,596 kB（含上界页, 与 legacy 同构） | +2,504 kB | +92 kB（≈+3.7%, 两 boot 间 churn ±100 kB 内） |
| 1024MB 常驻: slab 侧（meta+desc） | +2,048 kB（kmalloc-4k）+ ~49 kB（kmalloc-96 desc） | 0 | **+2,097 kB（结构性, 无 churn 解释空间）** |
| 页表侧基础设施合计 / 1GB | ≈4,693 kB = **0.4475%**（meminfo 口径, 理论精确值 0.3952%+上界页） | ≈2,504 kB = **0.2388%**（理论 0.1953%+上界页） | **×1.87, +0.209% of mapped** |
| 每窗精确 | 8,288 B = 0.3952% | 4,096 B = 0.1953% | **+4,192 B/窗 = ×2.02** |
| ptdescs / meta_arrays | 512/512（@1GB 档） | 0/0（enabled=0, stats 可读全零） | |
| unmap 后 | park 保留 PT（0.1998%）| 全释放（PageTables 回落 idle） | CortenMM 用驻留换 refill 速度（T1c 1.3μs） |
| 同压 swap 腿 | shrinker 261,223 页, 0 OOM | OOM-kill | §1.2 |

（THP 维度: 本内核 CONFIG_TRANSPARENT_HUGEPAGE is not set → legacy 即 4K 粒度 1 PT/2M，
对照天然同粒度，无 THP 2M 混淆变量。）

## 4. 与论文理论模型的对照结论

1. **常驻态**: 实测 0.3952% vs 论文/任务理论 0.3944%（8,272 B）/宣称 0.4% —— 三档窗口数
   全部精确、比值恒定，模型成立且实测略优（模型把 desc 记 96 B 后完全重合）。
2. **换出态**: metadata 不随换出缩减（0.3966% of 换出字节），swap 标识复用 meta 载荷零边际
   ——模型未单独陈述，实测补齐。三方账目到字节 = M6.T4 口径在受控场景的加强复现
   （M6 先例 Δ32 页容差内，本次 Δ=0 B）。
3. **池 idle 态**: park 开销 0.1998%（半额），上限 16 是 arena 数上限而非字节上限——
   语义修正登记（T1c 注释"~70KB worst case"按 16 窗估的，实测 1GB chunk park = 2.15 MB）。
4. **异常项全排**: 无隐藏会计旁路、无 slab 超额、无泄漏；理论外净开销 <0.003%/窗。

## 5. 边界与诚实声明

- 单 boot 单镜像（#93）；on/off 对照为两次重启（同 bzImage 同 qemu 参数仅 boot 参数差），
  PageTables/slab 对比含两 boot 间系统 churn（已在表内给出 churn 量级）。
- cgroup 压力腿负载形态: on 臂 memory.max 施加时 touch 尚在飞（风暴后稳定态取样），
  off 臂同构；OOM 对照为单腿单次，未做重试分布（登记，不作为 M6 判定使用）。
- g7load 的 touch 为单线程线性布局，不代表 mmbench 工作集形状（G2 已覆盖）；
  窗口/页数断言全部来自 ledger+dump 精确计数，不依赖采样时机。
- 16 池上限的外推界（16×1GB chunk ≈34 MB）未实测，仅语义推演。
- 时间戳: on 臂 05:55–06:15 UTC，off 臂 06:17–06:22 UTC，还原 06:3x UTC（2026-09-21）。
