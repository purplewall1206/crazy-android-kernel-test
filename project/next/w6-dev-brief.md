# W-6 执行任务书 · MV2 终判据（DoD 收官）

授权: specs/MV2_REMAINING_SPECS.md §W-6 + STATE W-5 收口条目（D33 判据收窄在案）。
基座: worktree /home/ppw/linux-6.18-mva @ 18614e29（HEAD; #297 =y 构建健康, sha
b939aa03…）。本片以**验证与判定**为主, 内核代码改动预期极小（wl 桶收缩的枚举面）。

## 判据清单（逐条给证据, 全部落 results/r07/w6/）
1. **J1 严格零**: W-5 后植入豁免前提已满足——audit_gate 的 J1 hooks 全 guest 电池
   gate_pass==1 / j1_hits==0 / j1_probes==0; KUnit j1 锚绿。若 wl/J1 代码里有
   "implant 豁免"分支已不可达, 移除之（最小 diff）或论证留作 backstop。
2. **J2 白名单收缩**: wl 分类 guest 电池读数只剩 SHARED/special/stack 三桶
   （anonymous/file-private leftover = FAIL; W-5 后应已自然满足, 读数固化）。
   wl_unclassified 应为 0 或给出构成。
3. **J3 maps 双源**: /proc/maps vs metadata 枚举全电池零 diff（含 exec 镜像行）;
   跨内核字节对拍: 用 bzimg/r07-mva1 的 A.1 基线 bzImage 起第二 VM（不同端口/
   pidfile 纪律）, 同 workload 跑 maps 快照, 与当前内核输出 diff——
   **逐字节一致判 PASS**（行序/地址布局差异按 REPORT 先例口径披露）。
4. **树归零 live 断言**: MODE 进程 maple 树条目 == 0。三载体: debugfs 常驻计数
   （加一行 tree_entries 若无现成读数——最小 diff）、KUnit 锚（既有 sweep 锚已断言
   map_count==0, 固化引用）、guest 电池（smoke/metis/probe 进程退出前后读数）。
   vdso/vvar 按 W-3 件4 排除口径入豁免表。
5. **零改动回归集全量**: smoke v2 双形态 / metis_eq ×2 / JTB / sweep-live /
   mva1_probe / S-3 双分支 / KUnit 三套件 on×2+off / =n 13 对象 / checkpatch。
6. **LoC 终账**: VMA 三件（mmap.c+vma.c+maple_tree.c）vs corten 各文件行数
   （REPORT §2.4 口径刷新, M-V2 增量后读数）。
7. **pgtables 残值 + j2_stale**: 复测读数（预期 1 笔残值=C2 登记容差;
   j2_stale=0）。

## 红线
- 内核改动仅限 wl/树归零计数枚举面（mm/corten_arena.c + test 文件）; 超出先报告。
- 不 commit（主会话判定+收口）; 零改动回归集是硬门（用户契约）, 不打折。
- 工件+判定表落 results/r07/w6/; 报告 next/w6-dev-report.md（判定表逐条
  PASS/FAIL+证据路径——REPORT M-V2 章由主会话执笔, 你供数字）。

## 坑清单
pidfile 杀 qemu; 双 VM 并存时端口/pidfile/镜像名全隔离（A.1 基线 VM 用
10025/trixie-a1.img 副本, 勿污染 trixie.img）; 9p 手动挂载; filter_glob=corten*;
interlock flake 复跑绿; make 串行 -j12; A.1 基线内核跑现代 workload 前先确认
其 smoke 可过（A.1 快照含当时全部能力, metis/S-3 等 workload 若依赖后加接口,
对拍范围收缩到 maps 行集并在报告披露）。
