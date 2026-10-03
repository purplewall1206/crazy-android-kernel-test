# W-6b 开发报告 · W-6 收官前综合修复轮（J3 验证 + 同族修 + C-fix B + sweep 收编）

2026-10-04。agent: w6b-dev (kernel-MM dev+verify, ponytail full)。基座
/home/ppw/linux-6.18-mva @ a9c89f5a8968（C-fix A 已入库；本片内核改动不 commit，
主会话收口）。任务书 = 主会话四任务指令；延续 results/r07/w6/。**不 commit。**

**四任务全落地：J3 验证 PASS（含 A.1 对拍落地与负言分诊）、or-next 同族臂修复并
guest 正言验证、special_mapping 分类修正并带 KUnit 锚、skip 逐因编码 + 全电池
因式分解（327 的 lump 归零）+ heap/brk 修复臂**。一处超轮次（frame-sharing
registry 契约）如实列清单交裁。最终件 = **#308** `6.18.32-ga9c89f5a8968-dirty`，
bzImage sha256 `a70b7915…fa801b9`（bzImage-w6b-final）。

## 0. 判定总表（对 W-6 报告 §0 的更新）

| # | 判据 | W-6 | w6b 判定 | 关键读数 |
|---|---|---|---|---|
| J1 | 严格零 | PASS | **PASS 维持** | 全电池 j1_hits=0 / j2_violations=0 / gate_pass=1；j1_probes 读数 4→11（登记型事件非硬门；or-next 修复后 query walk 更深到达 J1 slow path，增量如实登记未深挖归因） |
| J2 | 白名单收缩 | FAIL | **修复推进：unclassified 归零** | wl_unclassified 153→**0**（C-fix B：vdso/vvar/vclock 入 wl_special，3/进程 × 全 48+ walks = wl_special 171）；wl_file/anon 残留=frame-sharing declare 拒收（见 §4d，registry 契约面） |
| J3 | maps 双源 + 对拍 | FAIL | **PASS** | maps/smaps/numa_maps 对 MODE 进程渲染（17 行静态/4 窗口行，live+快照双证）；J3 oracle in-boot 断言 PASS（窗口行 ≥4）；**A.1 对拍落地**：procmap-first + pagemap flag 位**逐字节同**，maps/smaps 差异 = 既有披露面全集（V-B.3 file 行入窗 + 槽位级联 + A.1 magic 行缺），无新增差异；or-next 臂修复（§2） |
| J4 | 树归零 live | FAIL | **部分：构成修复 + brk 保卫 + 逐因计数；条目归零未达** | 静态 10（构成 file×4+anon×1+brk×1+stack×1+special×3——unclassified 三件转正）；条目残余 = skip_declare 288/电池（frame-sharing，§4d 超轮次清单）；**heap/brk 臂已修**（sys_brk 潜在破坏面拔除，§4b） |
| J5 | 零改动回归集 | PASS | **PASS 维持** | smoke 26/0 双形态 / JTB 2000×3 ×3 rc=0 / metis_eq ×2 checksum 同基准（65073 词 2d383eeed4ceb73b）/ sweep-live rc=0 / probe 17+1（CHUNK=陈旧预期，§1.3）/ S-3 双分支 rc=0 / KUnit on×2 绿 + off skip 对账精确（97+2 新锚）/ =n 85+30 对象零引用 / checkpatch 0E/0W |
| J6 | LoC | DONE | 供数 | 本片 +221/−38（task_mmu 7/3、arena.c 112/28、arena_test 90/7、mmap.c 11/0、mm.h 1/0） |
| J7 | pgtables/j2_stale | PASS | **PASS 维持** | PGTABLES_COUNT=2（8192B ×2，④ 族 C2 形状，容差 ±1 内）；j2_stale=0；dmesg corten 静默；arenas 退出归零 |

**MV2 DoD 收口判定材料齐备**：J3 可判 PASS；J2/J4 的残余缺口已从"判据未测"推进到
"定位到 registry 契约 + 计数器全披露"，收口口径（frame-sharing 豁免 vs 立项
multi-record 切片）= 主会话决断（§4d）。

## 1. 任务 1：J3 修复（C-fix A）的 guest 验证

### 1.1 正言验证（#303 = 6.18.32-ga9c89f5a8968）
- J3 oracle（注册驱动）：in-boot 断言 **PASS**（窗口行 4 ≥ 4；本 boot 快照 maps
  17 行，窗口行 rw8/rw2/---2/magic 齐位，magic 槽 0x100000e00000 与 workload
  自报一致）。
- MODE 进程 /proc/{maps,smaps,numa_maps} 非空正证据：live 读 17 行（stage-8
  live audit，tree_entries=10 @ready）；`j3-vc-snap/j3-snap/` 快照在档。
- A.1 跨内核对拍（bzImage-mva1 / trixie-a1.img / 端口 10025 / 披露的
  J3_SKIP_VMREADV skip-leg 变体，同 inode 293286）：`cmp_j3.sh` 判据下
  **procmap-first 逐字节同 + pagemap flag 位逐字节同**；maps/smaps 各恰一处
  diff hunk = W-6 报告 §3.4 预披露面全集（A.1 file 行落 legacy 0x7ffff7de3000
  vs 现行 V-B.3 入窗 0x100000c00000 + 级联 rw 行 0x100000e00000；A.1 侧
  magic/park 行缺=release-recycle 形态）。**对拍落地，零计划外差异。**

### 1.2 负言断言分诊（全电池重跑）
既往空真断言现在真咬的盘点：
1. **mva1_probe CHUNK maps 断言（17+1，既往"18/18"）= 陈旧预期，非回归**：
   `[FAIL] CHUNK: maps shows 1 segment(s)` —— MVA1 影子 VMA 时代的期望形
   （inner munmap 拆两段）；MV2 region 模型下 chunk munmap = **内容投放**
   （CORTEN_UNMAP_KEEP_PERM：region 记录与 maps 行稳定，重触即重实化——功能腿
   `[ok] kept half / dropped half re-materialized` 双过；chunk_shape 复现 +
   arenas 表三态读数在档：A/B/C 三态 region `[100000000000,100000800000)`
   恒一）。该 FAIL **早于本片存在**：与 02:53 的 C-fix A 电池逐字节同现；
   #298 的 18/0 = 空 maps 下的空真通过（"maps carries no VMA-split residue"
   对 0 行真空真）——即 W-6 报告 §3.2 "掩蔽机制" 的又一实证。**probe 注册件无
   源码（binary-only），改断言需源码** → §6 清单。
2. S-4 族其余断言（maps clean/reservation 类）现读真 maps 全 PASS——C-fix A
   语义正确性的正面确认。
3. **证据链披露**：真 #300 电池日志已被 02:53 的 C-fix-A 复跑覆盖（主会话在
   commit 后自行跑过一轮，留存件 guest-gate-303-complete.log；我本轮首跑
   guest-gate-w6b-r1-cfixa.log 同内核复现同判）。W-6 报告 §2 J5 行引用的
   18/18 与现存两份日志（298: 18/0；303: 17/1）均不吻合——引用源日志已失，
   以本报告 §1.2 的分诊为准。

## 2. 任务 2：PROCMAP_QUERY or-next 同族臂修复

**Root cause**（文本链）：`query_merge_corten_row()` 行胜出臂 W-2 把
`*row_hit = true; return row_out->ar->carrier;` 改成裸 `return NULL`（连同
row_hit 一起丢）；调用方 `query_matching_vma()` 的 `if (!vma) goto no_vma`
先于 row_hit 判定 → 行胜出恒 ENOENT。C-fix A 只修了 seq_file 臂，本案即
W-6 报告 §3.3 的"同族第二处"。

**最小修**（fs/proc/task_mmu.c +7/−3）：与 seq_file 臂同形——返还
`row_out` 存储为 token（`*row_hit = true` + `(struct vm_area_struct *)row_out`，
永不按 vma 解引用：调用链所有消费点（FILE_BACKED/VMA_FLAGS/`vm_start<=addr`/
skip 推进/do_procmap_query 渲染）全部 row_hit 三元守卫，调用方零改动。

**验证（#308 final）**：
- 修复前基线（#303 实测，or-next-prefix-wound.log）：MODE pid or-next
  @0x4f3000（wound 位点，past-heap gap）→ **ENOENT**；rowwalk 全枚举 rows=0
  （首查即亡）。covering 面健康（对照）。
- 修复后：or-next @0x4f3000 → **0x100000000000-100000800000
  [anon:corten_arena]**（恰为应答的 rw8 行）；or-next @0x10000 → exe r--p 行；
  **rowwalk 全枚举 16 行 = maps 17 行 − [vsyscall] 门行**（门行非树 VMA，query
  面不服务——预期口径），5 条窗口行全数按址序枚举（rows-w6b-enum.txt）。
- oracle procmap-first 面**逐字节不变**（cmp_j3 与 A.1 仍 byte-identical——
  该腿走 covering，修复不触）——零改动回归自证。
- 运维坑实录：rowwalk "30 行正常" 读数 = `pgrep -f` 自匹配（枚举的是非 MODE
  bash，dual_source off）；wound 复现必须 MODE mm。

## 3. 任务 3：special_mapping 分类修正（C-fix B）

**Root cause**：wl/sweep 两处分类谓词用 `arch_vma_name()` 判 special——
x86_64 **根本无 override**（仅 UML 有），vdso/vvar/vclock（special_mapping 族，
名字走 `vm_ops->name`）全落 UNCLASSIFIED → wl_special 48 walks 恒 0、
sweep 侧落 vm_ops-no-file 桶。

**最小修**：
- mm/mmap.c +11：`vma_is_special_mapping_family()`（`vm_ops ==
  &special_mapping_vmops`，族级判据，与既有 `vma_is_special_mapping(vma,sm)`
  并排；include/linux/mm.h +1 声明）。族级而非逐 arch 枚举——W-3 §1(3) 的
  多架构爆炸半径不进本判据（只做桶归类，不 region 化，W-3 的 timens/mremap
  语义面不触碰）。
- corten_arena.c wl classify + sweep classify(-4 臂) 各一行并入该谓词；
  豁免表口径 2→3 件（vdso+vvar+vclock，W-3 件4）随 wl_special 3/进程读数
  兑现（in-code 注释同步更新）。
- **KUnit 锚**（arena 套件 +2）：`corten_arena_test_sweep_skip_special_mapping`
  ——`_install_special_mapping()` 直驱真 vm_ops 形状：sweep skip_special +1、
  VMA 留树、wl 直方图 SPECIAL=1/UNCLASSIFIED=0（红面 = 预修复形态 0/1）；
  `corten_arena_test_sweep_skip_brk`（§4b 臂锚）。既有 fork_flags 锚随桶拆分
  移至 skip_flags(8) 并加 skip_other 不动锚；taxomomy 注释更新。

**验证**：wl audit delta（MODE pid，+2 walks）：wl_special +6（3/进程×2）、
wl_unclassified **+0**；全电池 wl_unclassified 153→0。sweep 侧
skip_special=63/电池（=3×21 enters）。KUnit 两跑全绿。

## 4. 任务 4：W-4b sweep 收编完备性

### 4a. skip 逐因编码
`corten_sweep_classify` 的每个 skip 路径独立返回码 + 独立计数器（flags/uffd/
filemay/ops/brk/declare 六新桶 + 既有 stack/special/shared/window/other），
arena_stats 渲染 +6 行，`corten_arena_test_sweep()` 访问器 case 8-13。
adopt 臂改为透传 declare errno（`-EEXIST` 单列 declare 桶，其余失败留 other）。

### 4b. 根因修复
全电池因式分解（#308，等价旧 skip_other 口径的 327/347）：

| skip 桶 | 读数/电池 | 判定 |
|---|---|---|
| **skip_declare** | **288 (88%)** | **真大头 = frame-sharing 邻居**：一 2M frame 一 region 记录（`corten_arena_overlaps` 按帧占位拒），静态形状 5/enter（4 个 exe 同帧段 + bss-anon）、bash/libc/JTB 同族。**任务书预期假设（file_may/结构旗标拒收）被计数器证伪：skip_filemay=0、skip_flags=1 全电池** |
| skip_special | 63 | = 3/进程 × 21 enters（C-fix B 转正，原落 ops/other） |
| skip_brk | 10 | **新修复臂**：heap VMA 留 VMA 形（wl BRK 谓词同形守卫插在旗标掩码前）。修的不只是归桶——**拔除潜在 sys_brk 破坏**：W-3 brk GROW 臂以 `vma_find()` 为契约，sweep 若 adopt 堆 VMA（原代码 anon 臂会收，唯 frame-sharing 侥幸挡住大半），GROW 臂落空后 legacy do_brk_flags 会往外来 region 记录的帧里写裸 PTE。锚：`sweep_skip_brk` 测试（VMA 留树 + 计数 +1） |
| skip_stack | 21 | 既有（grows 族，规格豁免桶） |
| skip_flags/uffd/filemay/ops/other | 1/0/0/0/**0** | other 归零（原 327 lump 不复存在）；flags 1 笔 = S-3 mlock 形状（规格豁免） |
| adopts | anon 13 + file 34 | 与 W-6 同量级（可收编面本就只有零头） |

### 4c. 重测
静态 tree_entries=10、构成 wl 六桶全正读（file×4+anon×1+brk×1+stack×1+
**special×3**）——**白名单桶外条目未归零**（file×4+anon×1 = declare 拒收残余），
目标未达，构成与根因全披露（4d）。KUnit 每修复臂一锚齐（special/brk/flags）。

### 4d. 超轮次清单（交主会话裁决，不硬凑）
**frame-sharing = registry 契约面**：`state->arenas` xa 按帧存指针、
`corten_arena_overlaps` 按帧占位拒、fault 面 `xa_load(addr>>PMD_SHIFT)` 单记
录解析（INV2/magazine/marker 全挂其上）。同帧多 VMA（ELF 五段是常态形状）
要全收编只有两条路：(i) 同 frame 多记录（registry 键结构 + fault 解析 +
INV2/marker 语义重做，量级 ≥ W-4 本体）；(ii) 跨段合并 declare（一记录一
prot，r--/r-x/rw- 合段必破 W^X，否决）。判据口径二选一：收窄豁免表
（"+frame-sharing declare 拒收"逐类披露替代零判，W-4 fail-open posture 的
显式化）或立项 multi-record registry 切片。

## 5. 验证链（最终件 #308 全跑）

| 项 | 读数 |
|---|---|
| =y 新增警告 | **0**（2 条既有基线现象：objtool cpuidle_enter_state=r04 判例、modpost memblock=r06 判例） |
| KUnit on×2 | 24/0/1 + **125/0/0** + 34/0/5，两跑全绿（125 = 123+2 新锚）；off：25/0/0 + 26/0/**99** + 7/0/32（skip 97+2=新锚 =n 对账精确）。披露：轮内一度 interlock 3 连败（co-tenant A.1 VM + 构建重载窗口；该测为钉 CPU kthread 时序型，r02 时代即有同型败绩，corten_test 代码零改动、宿载回落后两跑全绿） |
| =n | RC=0；85 mm + 30 fs/proc 对象 nm 零 corten 引用；corten*.o 零产出；task_mmu.o =n 干净（build-n-w6b.log / nm-n-w6b.txt / config-pre-n-w6b.snapshot） |
| checkpatch | --strict **0E/0W/0C**（w6b-full.diff，467 diff 行 / 423 检查行） |
| smoke v2 | 26 PASS / 0 FAIL 双形态，契约件同，arenas 退出归零 |
| JTB / metis_eq / sweep-live | 2000×3 ×3 全 rc=0 / ×2 rc=0 checksum 同基准（65073 词 2d383eeed4ceb73b）/ rc=0 RESULT PASS |
| mva1_probe | 17 PASS + 1 FAIL（CHUNK=§1.2 陈旧预期，披露） |
| S-3 | 双分支 rc=0 |
| J3 oracle 正言 | PASS（窗口行 ≥4；快照在档） |
| PROCMAP_QUERY 正言 | or-next wound 位点应答 rw8 行（§2）+ procmap-first 与 A.1 逐字节同 |
| tree_entries 白名单外 | **未归零 = 5/静态**（file×4+anon×1，frame-sharing declare 拒收，§4d） |
| wl 六桶 | shadow/implant/stack/special/file/anon 全正读；unclassified=0；implant=88（D33 活体，W-6 §5 维持） |
| PGTABLES / j2_stale / dmesg | 2 笔（8192B，±1 容差内）/ 0 / 静默 |
| mmap_write 纪律 | sweep 全程 mmap_write 持有（既有代码未触）；本片零树操作新增面 |

红线自查：INV6（wl/sweep 只读面未触，新代码全部计数器/谓词）✓；fail-open
保持（declare 拒收仍 fail-open 归桶，brk 守卫=skip 非 adopt）✓；=n 折叠 ✓；
已编译程序零破坏（smoke/metis/JTB/probe/S-3 全绿 + checksum 同基准）✓；
checkpatch 0E/0W ✓；mmap_write 下树操作：无新增 ✓。

## 6. 披露与移交清单

1. **frame-sharing registry 契约**（§4d）——J2/J4 收口口径二选一待裁。
2. **mva1_probe CHUNK 断言陈旧**——改断言需注册件源码（binary-only）；主会话
   若认 MV2 content-drop 口径，重发源码或出 oracle 替代件。
3. **j1_probes 读数 4→11**（非硬门）：or-next 修复后 query walk 更深到达 J1
   slow path 的登记型事件；j1_hits=0/gate_pass=1 恒。归因未深挖，登记备查。
4. 证据链：真 #300 电池日志失存（被主会话 02:53 C-fix-A 复跑覆盖，双份留存
   guest-gate-303-complete.log / guest-gate-w6b-r1-cfixa.log）；W-6 报告 §2
   的 18/18 引用源已失，分诊以本报告 §1.2 为准。
5. qemu 纪律：主 VM pidfile 归零待收口；A.1 VM（qemu-a1.pid，主会话 02:58 起）
   本片未触碰；kunit VM 每 跑即清。
6. w6-guest-gate.sh 两处运维修（stage-8 setsid-over-ssh 挂连规避 → guest 侧
   驱动 base64 注入；`pgrep -f`→`pgrep -x` 自匹配规避）——电池脚本变更，
   非内核面。

## 7. 工件索引（results/r07/w6/ 新增）

bzImage-w6b-cfixa / bzImage-w6b-final + bzimage-w6b-{,final-}sha256.txt；
w6b-full.diff；build-n-w6b.log + build-y-restore-w6b.log + nm-n-w6b.txt +
config-pre-n-w6b.snapshot；kunit-on1/on2/off.log（w6b 轮，覆盖前值已另存
kunit-on{1,2}-w6.log/kunit-off-w6.log）；guest-gate.log（#308 终轮）+
guest-gate-w6b-r1-cfixa.log（#303 复现轮）+ guest-gate-303-complete.log
（主会话 02:53 轮）；or-next-prefix-wound.log + or-next-postfix-verify.log +
rows-w6b-enum.txt；live-audit-j3-w6b.log；cmp_j3-w6b.log；本报告
（next/w6b-dev-report.md）。
