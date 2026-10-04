# W-6 开发报告 · MV2 终判据电池（DoD 收官验证片）

2026-10-04。agent: w6-dev2 (verification/qemu-exec)。基座 /home/ppw/linux-6.18-mva
@ 197d654af9a0（与主树 18614e29 内核码全等, 只差 project/ 文档）。任务书
next/w6-dev-brief.md; 规格 MV2_REMAINING_SPECS.md §W-6。不 commit——枚举 diff 与
J3 C-fix 已由主会话在 worktree 入库（6bca0e7e078a + a9c89f5a8968）, 与本片存档
diff 逐字节核验一致（w6-full.diff / w6-j3-maps-fix.diff）。

**本片性质 = 验证与判定。内核改动两件**: ①wl/树归零计数枚举面（mm/corten_arena.c
+81/−5, 任务书明示授权面, 主会话开工报告后批准）; ②J3 一行 C-fix（fs/proc/task_mmu.c
+10/−4, 主会话中途授权, attribution W-2 680857, W-6 oracle 首见）。验证揪出三项
存量缺陷（一项 W-2 起的 /proc 面回归 + 两项 W-4/W-5 判据面未达）, J3 已修复并
全量复验; J2/J4 待主会话裁定修复切片。

## 0. 判定总表（终版; 修复后内核 #303 = 6.18.32-ga9c89f5a8968, bzImage sha 9caa678e…98edaa）

| # | 判据 | 判定 | 关键读数 |
|---|---|---|---|
| J1 | 严格零（guest J1 hooks + KUnit 锚） | **PASS** | 标准电池全程 j1_probes=0/j1_hits=0; oracle 族 +2/run = mvc 报告登记残差（非硬门）; 终读数 j1_probes=4, j1_hits=0, gate_pass=1; KUnit j1 锚绿（on×2 全绿）。implant 豁免分支经查仍可达（D33 保留的 MAP_SHARED punch 租户写 registry, find_vma 过 punch VA 即走豁免）→ 按任务书"论证留作 backstop"（§5） |
| J2 | 白名单收缩（三桶） | **FAIL** | wl 逐类计数器（本片新增）实测: 静态 MODE 进程树 = file×4+anon×1+brk×1+stack×1+unclassified×3（=10）; 动态 = file×23+anon×4+stack×1+unclassified×3（=31）。anonymous/file-private leftover 非零 = 判据原文 FAIL; wl_unclassified≠0（构成见 §4.2）。"W-5 后应已自然满足"的前提被本片读数证伪 |
| J3 | maps 双源 + 跨内核字节对拍 | **PASS after in-slice C-fix**（attribution W-2, 2 行 V-B.3 披露——主会话已裁定采信披露口径） | C-fix 后: oracle 驱动 rc=0（窗口行≥4/S-4/PROCMAP_QUERY 一致全绿）; procmap-first 逐字节一致 + pagemap 标志位一致; maps 15/16 行逐字节一致, 差异仅 2 行 = V-B.3 注册语义（file 行入窗 + magic 槽位级联; ledger other=0, §3.4）——主会话裁定: 该 2 行属"地址布局差异按 REPORT 先例口径披露"适用面, J3 终判 PASS |
| J4 | 树归零 live 断言 | **FAIL** | tree_entries（本片新增载体）实测: 静态 10 / 动态 31（vdso/vvar/vclock+stack 豁免口径 ≤4）; 豁免外 = file/anon/brk leftover。W-4 entry sweep 的 fail-open 面（skip_other=347/电池）只收编零头 |
| J5 | 零改动回归集全量 | **PASS**（fix 后全量复跑 + 正言断言分诊） | smoke v2 26/26 双形态 / JTB 3/3 / metis_eq ×2 checksum 同基准（65073 词, 2d383eeed4ceb73b）/ sweep-live PASS / S-3 双分支 PASS / KUnit 三套件 on×2 + off 全绿（off 首跑 interlock flake 复跑绿 = 坑清单家族, 双档留痕）/ =n 零符号（fix 后复验）/ checkpatch 两 diff 各 0E/0W/0C。mva1_probe 17/18: 唯一 FAIL = CHUNK maps 检查, 分诊 = mvd-leak-report §6 登记的既有 V-C 渲染行为（"mvc gate 同 FAIL"）, W-2 空 maps 时代被空真掩盖, C-fix 后按登记预期重新可见——非新回归, 断言属注册件不改, 披露入档（§3.5） |
| J6 | LoC 终账 | **DONE**（供数） | loc-final.txt: VMA 层四件 13,408（vanilla 12,955）; corten 生产 23,761 + 测试 20,573 = 44,334; 净增 +10,806 |
| J7 | pgtables 残值 + j2_stale | **PASS（带披露）** | #303 电池 PGTABLES_COUNT=2（8192B×2, 均 smoke 相 55.2s/55.3s; #298/#300 轮为 1 笔; 家族形状同 C2 登记容差, 笔数方差如实披露, 深查归 C2）; j2_stale=0; registry 家族恒零 + mmap_region_routes 正证据; dmesg corten 静默; arenas 退出后归零 |

**MV2 DoD 收口建议**: J3 已闭环（修复+复验）; J2/J4 待主会话裁定修复切片或改判据
口径后收官。

## 1. 现场与工件

- 验证内核: 终版 **#303**（6.18.32-ga9c89f5a8968, sha 9caa678e…, bzimg/r07-w6/
  bzImage-w6-fix + results/r07/w6/bzImage-w6-fix + bzimage-sha256-fix.txt）。
  过程件: #298（7598bb70, 枚举 diff 初版电池）/ #300（a76defaa, 枚举 diff 终版,
  全电池两轮全绿, 日志 guest-gate-298.log / guest-gate-303-complete.log 前身）/
  #302（fix 首建）——#302 与 #303 同源同内容（主会话入库前后各一建, version 串
  构建号之差）。
- 主会话入库核验: 6bca0e7e078a（枚举面, +81/−5）与本片 w6-full.diff 逐字节一致;
  a9c89f5a8968（C-fix）与 w6-j3-maps-fix.diff 内容一致——git diff 双向核验在档。
- 内核 diff: w6-full.diff（枚举面, checkpatch 0E/0W/0C）+ w6-j3-maps-fix.diff
  （C-fix, checkpatch 0E/0W/0C）。
- 电池: w6-guest-gate.sh（主 VM: 端口 10026/trixie-w6v2.img 副本/qemu-w6v2.pid/
  tmux w6v2-vm, 与 W-5 的 10022 全隔离）; 日志: guest-gate-298.log（#298 轮）/
  guest-gate-303-complete.log（#303 终轮全档——见 §8 运维披露: 该档曾被并发运行
  的同名 gate 拷贝为 guest-gate-300.log, 已以 guest-gate-303-complete.log 固定）。
- J2/J4 live 审计: live-audit-j3.log + live-audit-bash.log（audit_gate 前后夹
  echo pid > whitelist; wl_walks +1 对账）; rows-j3-19140.txt（PROCMAP_QUERY 行枚举）。
- 跨内核: w6-a1-vm.sh（A.1 VM: 端口 10025/trixie-a1.img 副本/qemu-a1.pid/tmux
  a1-vm; 基线 bzimg/r07-mva1/bzImage-mva1 = d40eae59ba76 系）; a1-snap.log;
  j3-a1-snap/（A.1 maps 快照 17 行）; j3-vc-snap/（fix 后内核, maps 18 行）;
  cmp-j3-result.txt + j3-divergence-ledger.txt; tools/（oracle A.1-compat 变体 +
  rowwalk + chunk_shape 诊断件 + j3-divergence.py, 源码/构建式在档; 注册件未改动,
  registered binary 重建逐字节一致已验）。
- KUnit: kunit-on1/on2/off.log（#300）+ kunit-on1/on2/off-303*.log（#303; off 首
  跑 flake + rerun 绿双档）。
- =n: build-n.log + nm-n-symbols.txt（85 mm + 30 fs/proc 对象零 corten 引用）+
  build-n-fix.log（fix 后 =n 复验: fs/proc+mm RC=0 零符号, token 字段无 ifdef 归属
  问题——corten_row 在 proc_maps_private 无条件存在, 行臂在 ifdef 内）; 两份
  config snapshot。
- LoC: loc-final.txt。修复提案存档: w6-j3-maps-fix-proposal.diff（已被正式
  w6-j3-maps-fix.diff 取代）。
- qemu 纪律: 全程 pidfile 管理（w6v2/a1/w5chk 全部归零或移交）, 镜像全隔离
  （trixie.img 零污染）, make 串行（project/run/lock）, 日志全落盘。

## 2. J5 零改动回归集 · 终轮读数（#303, guest-gate-303-complete.log）

| 项 | 读数 |
|---|---|
| smoke v2 | rc=0, PASS 26 / FAIL 0 双形态, 契约件 sha 37df16d7 ✓, arenas after=0 |
| JTB | 2000×3 ×3 全 rc=0 |
| metis_eq ×2 | rc=0 ×2, checksum 与 W-3fix 基准同值（65073 词 / 2d383eeed4ceb73b） |
| sweep-live | rc=0, RESULT PASS |
| mva1_probe | **17/18**（见 §3.5 分诊; 功能腿 FRESH/CHUNK kept/revived/S-1/S-4b 全 ok） |
| S-3 | 分支 A PASS（swapoff clean rc=0）; 分支 B PASS（提前收敛形, swapins 闭合）——双分支判定 PASS |
| KUnit on×2 | 24/0/1 + 123/0/0 + 34/0/5 ×2; WARNING/BUG 签名集与 W-5 逐族一致（24× pgtables 8192 + 2× 12288 = ④ 族; wl scan WARN = 注入锚既定 WARN_ONCE; drain/txn/pool/foll_force/两 guard 全同 W-5） |
| KUnit off | 首跑 24/1/0（txn_uninstall_interlock, 30s 锁超时 = 坑清单 interlock flake 家族）→ 复跑 **25/0/0 + 26/0/97 + 7/0/32**（W-5 同值, skip 对账精确）; 双档留痕（kunit-off-303.log / kunit-off-303-rerun.log） |
| =n | RC=0; 零 corten 符号; corten*.o 不产出; task_mmu.o =n 干净（fix 前后各验一次） |
| checkpatch | w6-full.diff 0E/0W/0C（164 行）; w6-j3-maps-fix.diff 0E/0W/0C（21 行） |

计数面终读数: j1_probes=4（全 oracle 族登记残差, 标准电池 0）/ j1_hits=0 /
j2_walks=76 / j2_violations=0 / j2_stale=0 / wl_violations=0 / wl_brk_anomalies=0 /
gate_pass=1 / mmap_punches=16（D33 活体）/ mmap_region_routes=1（正证据）/
implant_drops=0 / swapins 闭合 / PGTABLES_COUNT=2（披露见 J7 行）/ dmesg 静默 /
arenas 全退出后仅剩表头。

## 3. J3 · W-2 回归 → C-fix 闭环

### 3.1 现象与根因（fix 前实测）
corten=on 的 MODE 进程读 /proc/{maps,smaps,numa_maps}（自读/跨进程同）: rc=0 且
零字节。根因 = MV2 W-2（680857104271）把 proc_get_vma 行臂 `return carrier` 改为
`return NULL`——seq_file 契约 NULL=EOF, .show 未被调, 三个 corten_row_active 渲染臂
（show_map:717/show_smap:1806/numa:3921）死码; W-4 扫入后 MODE mm 的树行全在窗口
上方, 首次归并必走行臂 → 恒空。掩盖机制 = 电池负言断言（S-4 族"无 X"空真通过）;
正言断言只在 mvc 波的 J3 oracle 驱动里, W-2 后从未跑过（REPORT-FINAL "J3 待补拍"
登记项即此债）。归因: #297（W-5 收口件, 零 W-6 改动）隔离 VM 复现同一失败。

### 3.2 C-fix（主会话授权, a9c89f5a8968 入库）
行臂改返 `(struct vm_area_struct *)&priv->corten_row`（非 NULL token; .show 的
corten_row_active 先行分流, 从不解引用为 vma）+ 注记。=n 归属核验: corten_row 字段
在 proc_maps_private（fs/proc/internal.h:404）无条件存在, 行臂在 #ifdef
CONFIG_CORTEN_MM_ARENA 内 → =n 下该臂不编译; fix 后 =n 复验 fs/proc+mm 零符号
RC=0（要求 #3 ✓）。同族第二处（query_merge_corten_row 的 or-next 行胜出臂
return NULL + 调用方不查 row_hit → or-next 在行胜出/首行场景 ENOENT, rowwalk
实测矩阵在档）**未修**——不在授权一行内, 登记为后续小片; PROCMAP_QUERY 覆盖式
（oracle 所用形态）健康。

### 3.3 修复后电池重跑 + 正言断言分诊（要求 #2）
全量重跑 rc 面: smoke/JTB/metis/sweep/S-3 = 0; oracle 驱动 = 0（fix 前恒 1）。
新咬出的正言断言: mva1_probe CHUNK maps 检查 FAIL（"maps shows 1 segment"）——
分诊: **mvd-leak-report §6 已登记的既有 V-C 双源渲染行为**（"CHUNK maps 残段
FAIL……mvc gate 同 FAIL, 与本片无关"）, 即 V-C 时代 maps 渲染时该检查同样 FAIL;
W-2 起被空 maps 空真掩盖; C-fix 后按登记预期重现。佐证实验（tools/chunk_shape +
rowwalk, 双内核三态矩阵）: 内层 munmap 的 region 在 A.1 与 #303 上同为"记录不裁、
渲染全跨度、再触复 materialize"的软丢弃形态（head/tail 两侧一致）; probe 的具体
形状分差（A.1 上 dropped 半区不渲染）登记为 probe 形状与 V-C 渲染的既有行为,
非 W-6 引入。判定: 非新回归 = 陈旧/登记行为; 断言属注册件（probe 二进制）不改,
披露入档。probe 判 17/18。

### 3.4 跨内核字节对拍（J3 本体; A.1 基线 vs fix 内核, 同 workload 同 inode 293286）
- A.1 侧: smoke v2 快验 24/1（唯一 FAIL = mmap-stack-routes-to-window, W-3 语义
  晚于 A.1, 预期披露）; 注册 oracle 件 rc=3（process_vm_readv 过窗 = V-C 能力）→
  披露的 skip-leg 变体（tools/mvc_j3_workload_a1.c, J3_SKIP_VMREADV=1, maps 面
  不变, 同名同路径同 inode）取快照 j3-a1-snap/（17 行, 窗口 3 行,
  [anon:corten_arena] shadow 标签与现行合成行逐字节对齐）。
- fix 侧快照 j3-vc-snap/（18 行, 全渲染）。
- cmp_j3.sh: **procmap-first 逐字节一致; pagemap 标志位一致**（PFN 掩码后）;
  maps/smaps 差异 = 恰两行 ledger（j3-divergence-ledger.txt, other=0）:
  ① A.1 的 file 行落 legacy 区（7ffff7de3000）vs fix 落窗口（100000c00000）——
  **V-B.3 注册语义**（"dlopen-shaped addr==0 private file maps are served from
  the window", mvc/mvb 报告在案）, 同 dev:ino 同 perms 同 pgoff 同路径;
  ② fix 多一行 magic region（100000e00000）——①的槽位级联（A.1 的 magic 复用
  park 释放槽 0xc00000; fix 该槽被 file region 占用, magic 移 0xe00000; A.1 自身
  的 magic 行亦不渲染, 其 release-recycle 形态披露）。
- 判定口径: 任务书"逐字节一致判 PASS（行序/地址布局差异按 REPORT 先例口径
  披露）"——差异全部为已注册的语义演进（V-B.3/V-B）, 无一行无主; 判定表记
  "PASS after in-slice C-fix, attribution W-2"。若主会话按 cmp_j3.sh 字面
  （budget 未列 V-B.3 行）判 FAIL, 余留面即此两行的口径裁决。

### 3.5 mva1_probe CHUNK maps FAIL 分诊（要求 #2 的逐条分诊义务）
见 §3.3。结论: 登记既有行为（mvd §6）, 非回归, 不改注册断言, 披露披露。
（PGTABLES_COUNT=2 的第二笔亦经分诊: 两笔均 smoke 相 mm 退出, 家族形状 8192B,
笔数方差登记入 J7 行, 深查归 C2。）

## 4. J2/J4 FAIL · 白名单收缩与树归零（判据首次被真正测量）

### 4.1 读数（live 审计, 单次审计 audit_gate 前后 delta, wl_walks +1 对账）
| 形状 | tree_entries | delta 构成 |
|---|---|---|
| 静态（j3 workload @ready） | **10** | stack+1, file+4（exec 镜像残段）, anon+1, brk+1, unclassified+3（vdso/vvar/vclock 误桶）, special+0 |
| 动态（hook+bash holder） | **31** | stack+1, **file+23**（ld.so/libc/bash 镜像）, anon+4, unclassified+3 |
| 全电池累计（~48 walks） | — | wl_file 556+, wl_anon 117+, wl_unclassified 141→153, wl_implant=88（D33 punch 租户活体）, wl_special 恒 0 |

逐类 delta 之和与 tree_entries 逐笔吻合（10/10, 31/31）——计数器自洽。
fix（a9c89f5a8968）不触碰此面（渲染层）, 判定对 #303 同样成立。

### 4.2 判据面两个缺口（移交主会话裁定修复切片）
1. **special 桶失明（分类器缺陷）**: wl classify 用 arch_vma_name() 判 special,
   但 vdso/vvar/vclock 是 special_mapping（名字走 vm_ops->name, arch_vma_name
   不认）→ 全部落 UNCLASSIFIED。48 walks wl_special 恒 0; wl_unclassified 恒定
   构成 = 该三件/进程。豁免表口径（W-3 件4 "2 条/进程"）也需更新为 3 件（vclock
   页）。修法 = classify 加 vma_is_special_mapping()（生产审计语义变更, 红线外
   未动）。
2. **sweep fail-open 面宽（W-4 遗留的真实形态）**: entry sweep verdict（结构旗标/
   file_may 拒绝臂 → skip_other）实测只收编零头——静态形状收 1 件, 动态近全留
   （skip_other=347/电池）; 初始 heap VMA 也不在收编面。**"anonymous/file-private
   leftover = FAIL" 与 "树条目==0" 按原文 = FAIL**。改判据口径（收窄豁免表 +
   逐类构成披露替代零判）或立项扫入加强片 = 主会话决断; 修法候选已定位到函数级
   （verdict 的 file_may/结构旗标枚举复核）。

## 5. J1 · implant 豁免分支 backstop 论证（不移除）

corten_j1_slow 的两段 corten_implant_covers_lockless 豁免（V-A.3d/D24）W-5 后
仍可达: D33 保留的 MAP_SHARED memfd 窗口 punch（probe punchfork 腿, 电池 16 次）
以 admitted=false 续程写 registry（wl_implant=88 = 其累计活体）, 任何 find_vma 过
该 VA 即走豁免臂。移除将使合法 punch 租户污染 J1 ledger（gate 恒红）。留作
backstop; 若主会话取"恒不可达"严格零（含 SHARED 形 -EOPNOTSUPP 臂）, W-5 报告
§7.1 已备方案（exit_punchfork 等锚需随契约重钉）。

## 6. LoC 终账（J6, loc-final.txt 全表）

- 被替代的 VMA 层四件（REPORT-FINAL"三件"口径, maple_tree 实在 lib/）: **13,408 行**
  （vanilla 6f884f1658b1 = 12,955; mmap.c +323 / vma.c +107 / mmap_lock.c +23 /
  maple_tree.c 0——路由钩净 +453）
- corten 侧: 生产 **23,761**（REPORT-FINAL 快照 19,676 → +4,085, M-V/MV2 增量;
  arena.c 18,178 为主）+ 测试 **20,573** = 44,334
- 净增（生产 vs vanilla 四件）: **+10,806**。"代码更少"主张在该移植口径继续不成立;
  可删性主张延续——但 §4 树归零判据未达, "可删"的 live 断言面随 J4 一并移交。

## 7. 披露与移交清单

1. J2/J4 两缺口（special 分类器 + sweep fail-open 面）——等裁定（§4.2）。
2. PROCMAP_QUERY or-next 臂（query_merge_corten_row 第二处 return NULL + 调用方
   不查 row_hit）未修——不在授权一行内; rowwalk 实测矩阵在档; 建议随 J2/J4 裁定
   片同修（同为 task_mmu.c 小行）。
3. PGTABLES_COUNT 笔数方差（1→2, 家族形状不变）——深查归 C2。
4. mva1_probe CHUNK maps 检查 = 登记既有 FAIL（mvd §6）, 注册件断言未改。
5. 诊断件（tools/）: oracle A.1-compat 变体 + rowwalk + chunk_shape + 
   j3-divergence.py——新引入, 源码/构建式在档, 注册件未改动（registered binary
   重建逐字节一致已验）。
6. 隔离副本 trixie-w6v2.img / trixie-a1.img / w5chk.qcow2 交付态在盘; trixie.img
   零污染。
7. **运维披露（并发碰撞）**: 02:56-02:58 检测到另一会话（tmux w6v2-vm 重建 +
   同名 w6-guest-gate.sh gate 运行 + guest-gate.log 截断）与本片收尾并发——本片
   完整日志已先固定为 guest-gate-303-complete.log（md5 c31ed144…, uname
   ga9c89f5a8968 含 stage 10 全档）; 同名产物以 §8 索引为准。建议主会话按 STATE
   先例重申"同一 worktree/同一 VM 基建必须串行化"。
8. A.1 基线 dmesg 有 3 笔 non-zero pgtables_bytes（4096B×3, 含 boot 期 2 笔）——
   ④ 族祖先形态, 供 C2 对照。

## 8. 工件索引（results/r07/w6/）

bzImage-w6-final（#300 枚举面终版）+ bzImage-w6-fix（#303 交付版）+
bzimage-sha256.txt + bzimage-sha256-fix.txt + bzimg/r07-w6/SHA256SUMS;
w6-full.diff + w6-j3-maps-fix.diff + checkpatch-w6.txt;
w6-guest-gate.sh + guest-gate-303-complete.log（#303 终轮全档）+
guest-gate-298.log（#298 轮）; w6-a1-vm.sh + a1-snap.log; w6-kunit.sh +
kunit-on1/on2/off.log（#300）+ kunit-on1/on2/off-303*.log（#303, off 含 flake
双档）; live-audit-j3.log + live-audit-bash.log + rows-j3-19140.txt;
j3-a1-snap/ + j3-vc-snap/ + cmp-j3-result.txt + j3-divergence-ledger.txt;
tools/; build-n.log + build-n-fix.log + nm-n-symbols.txt + config-pre-n*.snapshot;
loc-final.txt; w6-j3-maps-fix-proposal.diff（提案存档, 已被正式 fix 取代）;
本报告（next/w6-dev-report.md）。
