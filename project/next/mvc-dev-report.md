# M-V V-C 开发报告（proc 双源渲染 + GUP-slow MODE 分支 + J3 oracle）

- 树: /home/ppw/linux-6.18-mvb @ f4ffec5e0005 (A.3d) + 本片未提交增量
- 切片: MV_VMA_FREE_SPEC.md §3.3（C13/C15/C25/C30 + j2-audit #3/#7/#8/#9/#10/#11/#12）
- diff: patches/r07-mvc.diff（**+1507/−48, 8 文件**；其中内核 ~+1178/−48，KUnit +329）
- 验证产物: results/r07/mvc/（build-n.log + 9 份 KUnit 日志）；oracle 件 bench/mvc-oracle/

## 1. 改动清单（按面）

| 文件 | 增量 | 内容 |
|---|---|---|
| include/linux/corten_arena.h | +246 | V-C 公共 API：行流（corten_row_iter/row_next/row_query/dual_source）、smaps 聚合、pagemap fill、CORTEN_PM_* 位常量 + CORTEN_REGION_ROW_LABEL + =n 折叠 stub（纯数据游标结构体移出 ifdef——fs/proc/internal.h 按值内嵌） |
| mm/corten_arena.h | +50 | corten_gup_probe / corten_remote_vm_window 的 =y 声明 + =n inline 折叠 |
| mm/corten_arena.c | +490 | 行流实现（植入裁剪/去重/parked 跳过/targeted 树锚跳过）、corten_gup_probe（region 解析→carrier；植入→NULL；parked/hole→ERR_PTR(-EFAULT)；FOLL_ANON×FILE→-EFAULT）、corten_remote_vm_window、smaps PT 聚合（corten_smap_pte/fill_pt/pagemap_pte 三助手，全部 pte_offset_map_lock 下读 PTE，INV6）、3 计数器（gup_probes/gup_probe_rejects/maps_window_rows）+ debugfs stats 三行 |
| mm/gup.c | +27/−10 | gup_vma_lookup 增加 gup_flags 参，find_vma 前置 corten_gup_probe（两调用点：__get_user_pages + fixup_user_fault）；窗口拒绝落 NULL 臂→调用方 -EFAULT（与 find_vma miss 同 errno） |
| mm/memory.c | +30/−7 | __access_remote_vm 预检查窗口旁路（#7）+ 循环内 IS_ERR 回退臂窗口短路；__copy_remote_vm_str 预检查旁路（#8）；corten_remote_note_window_short 保留给 parked/hole 短答 |
| fs/proc/internal.h | +9 | proc_maps_private 增 corten_rows/corten_row/corten_row_valid/corten_row_active |
| fs/proc/task_mmu.c | +357 | 归并游标（m_start 预进/corten_maps_prime + proc_get_vma 按地址归并、lookahead 前推、*ppos=row.end 重启语义）；双源 mm 强制 mmap_read 锁臂（carrier 寿命 + J1 卫生）；show_map/show_smap/show_numa_map 窗口行臂（show_corten_map_row 复用 show_vma_header_prefix，[anon:corten_arena] 标签逐字节对齐 shadow 时代）；PROCMAP_QUERY covering-row 优先（免树走）+ or-next 归并 + piece pgoff 推进；smaps_rollup 窗口聚合；pagemap_pte_hole 窗口段真值 fill；PM_*/CORTEN_PM_* 六条 static_assert 位配对；=n 四助手折叠 |
| mm/corten_arena_test.c | +329 | 三 KUnit 锚（§4） |

## 2. 设计要点与裁决

1. **行流（row stream）而非整区渲染**: punch 擦帧后 region 有洞——行 = region ∩ 植入补集，多片 region 渲染为多行（shadow 时代经 split_vma 渲染同样形状，J3 字节对齐的前提）。`npieces/rpieces` 表无生产者（V-A.0 现状），行划分由帧表+植入注册表实时推导。
2. **carrier 即渲染上下文**: 行返回 `ar->carrier`，show 臂经 priv->corten_row_active 区分；PROCMAP_QUERY 的 flags/vm_file/page_size 直接吃 carrier（与 shadow-VMA 时代同形）。ANON 行名 `[anon:corten_arena]` = shadow 的 anon_vma_name 拼写（CORTEN_REGION_ROW_LABEL 常量钉死）。
3. **prot 渲染口径**: 行 prot 取 `ar->prot`（DECLARE/mmap 时上界）——与 shadow-VMA flags 不随 routed mprotect 演进的既有契约逐字节一致（M4T0 §3.3）。
4. **双源 mm 锁臂**: maps/smaps/numa/PROCMAP_QUERY 对 MODE mm 从 per-VMA RCU 臂改为 mmap_read 臂（region/carrier 写方全在 mmap_write 下；也把 lock_next_vma 的窗口 mas_walk 排除出 J1）。
5. **GUP probe 的 J1/J4 收口**: probe 在 find_vma **之前**；窗口内非植入地址永不触树。carrier 寿命契约 = mmap_read + arena fault 永不 VM_FAULT_RETRY（mm/memory.c 慢钩契约），故 carrier 指针不会跨越锁掉落存活。FOLL_FORCE/M5.T3 裁决不变：check_vma_flags 的 corten_own 臂在 carrier 上逐字工作（VM_CORTEN 位在）。
6. **PROCMAP_QUERY covering-row 优先**: 覆盖型窗口查询零树走（J1）；or-next 竞争仍取树候选比较（披露：or-next 型窗口查询 residual 一次 find_vma）。
7. **pagemap/smaps 聚合口径**: PTE 直读（mincore 骨架，ptl 下）而非 meta 读——INV6 同纪律，且 PM_FILE/MMAP_EXCLUSIVE/PSS 除数需 folio 真值；desc->nr_mapped 计数与 PTE present 由 INV7 等价。smaps 窗口行只填 Rss/Pss/Anonymous/Swap（spec 的 debugfs 同源口径），其余桶登记为零值披露。
8. **numa_maps 简化披露**: 窗口行 = 头行 + task policy + kernelpagesize，无逐页 NUMA 计数（OQ-MV-14，spec §3.3 允许）。

## 3. 边界与登记

- **语义保持**: S-1..S-5 全部不变；本片新增披露：smaps 窗口行的 Referenced/Shared/Private×Clean/Dirty/THP 桶为零值（非 legacy 对齐，登记在案）；numa_maps 窗口行无 N*= 计数；PROCMAP_QUERY or-next 型窗口查询有 residual find_vma（J1 计数器口径披露，覆盖型零）。
- **INV6**: 一切 PTE 读（smaps/pagemap/mincore 族）走 pte_offset_map_lock ✓；无新 PTE 写点（白名单 3/3 不扩）。
- **=n 折叠**: 18 对象（前 16 + mincore.o + msync.o 本片扩至含 **fs/proc/task_mmu.o**）RC=0 零警告，nm 零 corten 符号（task_mmu 四助手 #ifdef+inline 折叠后复扫为零）。.config 已还原 =y 并全量重建。
- **非 MODE 零扰动**: 全部新臂双门（corten_enabled_static + mm->corten_mode）前置；gup probe 对非 MODE/窗外返回 NULL 走原路径零行为差；m_start 锁臂仅对 dual_source mm 改变。
- **不 commit**（未入库，diff 已导出）。

## 4. KUnit 锚（mm/corten_arena_test.c, filter_glob=corten*）

1. **corten_arena_test_mvc_row_stream**: 三 region（2帧 RW/1帧 R/1帧 PROT_NONE）行序与字段精确断言（start/end/rclass/prot/carrier）；row_query 覆盖与 or-next；**punch 形状**（擦帧+implant_mark）行分裂为 [a, a+PMD) ；munmap-route park 后 S-4 不渲染；maps_window_rows 计数器 +5 对账。
2. **corten_arena_test_mvc_gup_probe**: 矩阵——pre-MODE NULL / 窗外 NULL / 活跃 region→carrier（0 与 FOLL_WRITE 两形）/ hole→ERR_PTR(-EFAULT) / park 后→ERR_PTR(-EFAULT) / implant→NULL / FILE region FOLL_ANON→-EFAULT；probes/rejects 计数器差分对账。
3. **corten_arena_test_mvc_smaps_pagemap**: state-2 形状（mark RW + carrier 写 fault）→ smaps resident=anon=PAGE_SIZE、pss=PAGE_SIZE<<12；pagemap fill 首页 PRESENT|EXCLUSIVE|非零 pfn、次页零条目。

## 5. 验证结论

| 项 | 结果 |
|---|---|
| make -j8（=y）×4 | RC=0，仅 2 条既有基座警告（objtool cpuidle_enter_state、modpost memblock），**零新增** |
| KUnit corten=on ×5 | **corten 24/25(skip1 既有)、corten_arena 93/93、corten_fault 31/33(skip2 既有)** 全绿；零 lockdep/oops 签名 |
| KUnit corten=off ×3 | 全绿（新三例按 corten=on 门 skip，23/93/7 pass + 70/26 skip 与基线形态一致） |
| flake 复跑判定 | `corten_test_txn_uninstall_interlock`（mm/corten_test.c，**本片未触碰**）于 9 跑中 2 次 timing 失败（on1 前修复轮与 off 轮），复跑即绿；与本片改动面无交集（本片 8 文件不含 corten_test.c/corten.c/事务层） |
| =n 折叠 | 18 对象 RC=0 零警告 + nm 零 corten 符号 |
| checkpatch | **0 errors / 0 warnings**（1842 行） |
| 中途修复 | ① pagemap fill 对 none-PTE 曾漏 emit 零条目（KUnit 抓出，改分类助手后无 emit 分支全集）；② dual_source 断言时机；③ task_mmu =n 符号泄漏（show_corten_map_row 等）→ #ifdef 折叠 |

## 6. J3 oracle 与 guest 门判据（留给主会话）

**件**: `bench/mvc-oracle/`（mvc_j3_workload.c 确定性负载 + run_mvc_oracle.sh in-boot 驱动 + cmp_j3.sh 跨内核对拍）。负载形态: MODE 内 8M RW(触碰)/2M RW/2M PROT_NONE 储备/2M 私有文件映射(读)/2M 建后 park/2M magic 页 + 子进程 process_vm_readv 跨窗读。host 编译: `gcc -static -O2 -Wall -Wextra -o mvc_j3_workload mvc_j3_workload.c`（已验证零警告）。

**判据（guest, corten=on, root, setarch -R）**:
1. **in-boot**（run_mvc_oracle.sh, 任意本片内核）: maps 窗口行 ≥4 且地址序；parked 2M rw 窗口行不出现（S-4）；pagemap 首 RW 窗口页 bit63 PRESENT；PROCMAP_QUERY 覆盖行 start == maps 同页行 start；process_vm_readv 跨窗取回 MAGIC（rc=3 即 FAIL，A.3b #7 回归门）；`audit_gate` 严格零维持（j1_probes/j1_hits/j2_violations == 0）；stats 披露 maps_window_rows>0 ∧ gup_probes>0；dmesg 无 corten WARN/BUG。
2. **跨内核字节 oracle**（J3 本体）: 同负载在 **A.1 基线 bzImage（d40eae59ba76，auto 窗口尚有 shadow-VMA 渲染）** 与 **本片 bzImage** 各跑一次 run_mvc_oracle.sh 存快照，`cmp_j3.sh <A1快照> <VC快照>`: **maps 与 procmap-first 必须逐字节相同**；pagemap 掩 PFN 后标志位相同；smaps 各块头行（maps 行）相同。分歧行政预算: smaps 数值桶（登记披露）、numa_maps 窗口行计数——二者不作为 FAIL 门但须记录。
3. **标准门**: 26/26 smoke、run13 fails=0、metis_eq/dedup_eq/psearchy_eq checksum 三方同值、JThreadBench 3×rc=0（外部数据面恢复后 process_vm_* 是重点）、io_uring/9p 四态矩阵（M5.T3 件复用——本片 gup 分支的行为应与 shadow 时代逐位一致）、bpftrace find_vma 窗口命中==0 双口径复核。
4. **建议加测**: MODE 进程 cat /proc/self/maps 行数 == 委托 VMA 数 + 窗口 region 行数（workload 的固定形状可精确预期）；perf report 对 MODE JVM 符号率对齐（C25 下游）。

## 7. 已知残留（下片/登记）

- PROCMAP_QUERY or-next 型窗口查询的 residual find_vma（覆盖型已免）——若 J1 要绝对零，需 row-stream 的 or-next 全序归并（YAGNI，登记）。
- bpf_iter_task_vma（j2-audit #39）与 trace 符号化（#37/#38）仍是单树源——V-C 未覆盖，登记为观察面残项（无错误路径，纯保真）。
- swapoff unuse 盲区（S-3）与 exit 走查属 V-D。
- oracle 的跨内核对拍需 A.1 基线 bzImage 在主会话侧就绪（bench/bzimg 或重建 d40eae59ba76）。
