# M-V V-E 电池件产出报告（bench 五件，guest 终判据电池）

产出目录：`bench/share/mve-battery/`（9p share，guest 路径
`/mnt/hostshare/mve-battery/`）。设计真源：`next/mve-dev-report.md`
§2.1 / §2.3 / §4（判据表逐条落码）；S-3 两分支 = `next/mvd-dev-report.md`
§6。内核 worktree（`linux-6.18-mvb`，+739 增量）零触碰——只读取证
（git status 前后同 6 文件，见 §3 红线）。

## 1. 五件清单

| 件 | 行数/形态 | 内容 |
|---|---|---|
| `mve_battery.sh` | 主驱动，可执行 | `[j1][j2][j3][j4][brk][s3][all]`（默认 j1 j2 j3 j4 brk）+ `--dry-run`（判据清单自检）+ 位置参数/env `BASE_SNAP`。每判据一行 `[mve <leg>] PASS/FAIL/SKIP`，末行 `MVE-BATTERY SUMMARY legs=.. PASS=n FAIL=n SKIP=n`，rc=FAIL>0。含 MODE 探针 boot 门（防 corten=off 假绿）、9p share 发现（mva2 同形）、metis 形态 MODE 调用（LD_PRELOAD hook + STRICT） |
| `mve_workload.c` + `mve_workload`（静态产物） | C ~530 行 | 四模式：`j4`（6 断言：活窗读/PROT_NONE 拒/parked 拒/堆读/窗写/写往返，process_vm 双进程矩阵）；`brk [N=200]`（fork 子进程 raw syscall 四臂锤 GROW/SHRINK/NOOP/REJECT + 守护字存活 + break 连续性 + 父进程 glibc malloc/trim 流量）；`hold [s]`（J2 活体多类别持有者：窗 RW+PROT_NONE+MAP_SHARED/32BIT/STACK+长堆）；`s3 [MiB=64]`（逐页写+位置编码校验和，SIGUSR1 读回/SIGUSR2 重脏/SIGTERM 退）。MODE：prctl ENTER(幂等)+GET==1 严格检查（与 hook 构造器同序列） |
| `mve_heap_trace.bt` | bpftrace | kprobe `find_vma`+`lock_vma_under_rcu`，`mm->corten_mode==1` 过滤，四桶：mode_window（J1(b) 判据）/mode_heap（份额分子）/mode_other（分母余量）/legacy_total（非 MODE 基线）；整数 map 单元（无 count() 聚合 printf）；10s 周期 + 120s 自退 + END FINAL 行（电池 sed 解析） |
| `mve_s3_swapoff.sh` | 两分支，可执行 | 真盘 swap（块设备或 swapfile，prio 200 盖 zram）+ `mve_workload s3` 持有者 + debugfs `evict` 通道驱动换出（m6t2 同形，无内存压力 hog）。分支 A：读回（swapins>0 硬）→ swapoff 干净成功；分支 B：有界自旋（state≠D）→ SIGINT 可中断 → 持有者 exit → inuse ≤30s 归零 → 重试成功；`unuse_blind_mms/zap_swap_frees/swapins` 披露（zap 静止=FAIL） |
| `README.md` | 判据书 | §4 判据表原文全文 + 逐腿判罚/容差/跳过语义 + BASE_SNAP 重拍指引 + MODE 形态说明（静态二进制与 LD_PRELOAD 的关系） |

## 2. 判据落码对照（§4 逐条）

- **J1(a)**：audit_gate `j1_probes/j1_hits` 跨 metis×2（mva2 同形，含 MODE
  标记硬检；metis 缺件回退 workload j4+brk50 并 SKIP）delta==0 严格。
  **J1(b)**：.bt window 桶==0；无 bpftrace/BTF → SKIP 登记。
- **J2**：活体 `whitelist <pid>` rc==0；`wl_violations/wl_brk_anomalies`
  delta==0（跨手动+退出）；`wl_walks` ≥ +2（手动 1 + exit 常驻走查——内核侧
  `corten_audit_whitelist_walk_locked()` 在 mm_exit 与 j2 walk 并排，已核实）；
  交叉 `wl_violations==j2_violations`（不等=FAIL 先分类）；组成三行披露。
- **J3**：复用 `$SHARE/mvc-oracle/run_mvc_oracle.sh`（rc 硬判）；
  `BASE_SNAP` 给定（位置参数 `/path` 或 env）→ `cmp_j3.sh` rc 硬判；缺省
  → SKIP 登记（A.1 快照重拍指引在 README）。
- **J4**：workload 6/6 + rc==0 + `MODE on` 标记 + `gup_probes` delta>0。
- **OQ-MV-7/brk**：四臂 delta 全>0（接线证明）+ `heap_lookups` delta>0 +
  份额三档（<5 维持 / 5–25 登记 / >25 升级；裁决输出不置 FAIL）+ 双口径
  同量级（×10 窗，bpftrace 缺件 SKIP）。
- **S-3**：两分支判罚如上表；D 态挂死/条目滞留=FAIL 升级。

内核侧语义核对（只读）：GUP 判据面按 `corten_gup_probe()` 实际形状落码——
parked/储备窗地址直接 `ERR_PTR(-EFAULT)`、活区返回 carrier；`process_vm_*`
不带 FOLL_FORCE（mm/process_vm_access.c 只置 FOLL_WRITE），PROT_NONE 拒与
parked 拒的 EFAULT 断言与内核行为一致。四臂计数挂点复核
（mm/mmap.c L170/191/212/226：NOOP/SHRINK/GROW/REJECT）。

## 3. host 侧自测结果（全部通过）

| 项 | 结果 |
|---|---|
| `bash -n` 两脚本 | OK（另用 stub debugfs 全腿跑通控制流，修出一处 `set -u` 解析 bug） |
| `gcc -static -O2 -Wall -Wextra` | **零警告**，ELF 静态产物同目录 |
| `--dry-run` | 打印 7 行判据清单，rc=0，不触碰 guest 依赖 |
| brk 循环数学（宿主内核） | **200 cycles：grow/shrink/noop/reject 各 200 次、守护字 200/200 存活、break 连续性精确、glibc churn ok、rc=0**（REJECT 臂 raw `brk(0x1000)`——glibc sbrk 大负增量会吞拒绝，已按此实现） |
| 其余模式 host 行为 | j4/hold/s3 无 corten 即刻 rc=2 带原因（防误测）；`s3 0` 参数校验 rc=1 |
| `.bt` 语法 | 静态核对（花括号平衡、探针形态、map 引用一致）；host bpftrace 0.14 需 root 且宿主 BTF 缺 corten 字段，`-d` 不可行——按任务 fallback 条款登记，guest 实跑留主会话 |
| MODE 探针 boot 门 | host 上正确 FATAL exit 2（corten=off/非 root 拒跑，防 0→0 假绿） |
| 红线 | `linux-6.18-mvb` git status 前后同为 6 个 V-E 增量文件，**零触碰** |

## 4. 需主会话注意的落点

1. **guest bpftrace 件**：guest 内核 `CONFIG_DEBUG_INFO_NONE`（无 BTF），
   `.bt` 的 `mm->corten_mode` 字段解析需 BTF 或带 corten 头文件的内核源
   树（`bpftrace -I <kernel-src>/include` 形态）。缺件时 J1(b)/份额/双口径
   自动 SKIP 登记（内核口径不受影响）——若主会话要 bpftrace 口径，需在
   guest 备内核源 include 或重开 BTF 构建。
2. **静态二进制与 hook**：`-static` 产物不执行动态加载器，LD_PRELOAD 不
   生效；workload 内嵌同一 prctl ENTER+GET 序列，电池调用时仍带 metis 形态
   环境（对动态件生效、对静态件空转）。README 已登记。
3. **A.1 基线快照**：`bzimg/r07-mva1/` boot 后 `run_mvc_oracle.sh <dir>`
   拍一次得 BASE_SNAP，再 `mve_battery.sh j3 <dir>`（或 env）。
4. **S-3 环境**：需可写 `/root/bench`（swapfile 落点）或 `SWAPDEV=<块设备>`
   env；zram 只作背景（prio 200 盖过）。evict 通道失败时 swapout 判据硬
   FAIL（不静默降级为压力 hog）。
5. 旧草稿目录 `cortenmm/bench/share/mve-battery/`（前次会话误置）不在
   交付路径；本报告以 `bench/share/mve-battery/`（真 9p share）为准。
