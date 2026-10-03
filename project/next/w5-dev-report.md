# W-5 开发报告 · 植入消灭（显式 MAP_FIXED 全路由）

2026-10-03/04。agent: w5-dev。基座 /home/ppw/linux-6.18-mva @ 893c804（HEAD, 干净）。
任务书 next/w5-dev-brief.md；规格 MV2_REMAINING_SPECS.md §W-5；STATE W-4 遗留①②③。
**不 commit（主会话收口）**。最终 diff：5 文件 +533/−58（`git diff | sha256sum` =
`cd304794319c82ee59e21abc31501b5de81ba9e8521d6673e8680598b17b513a`）。

## 0. 结论一览

| 项 | 判据 | 读数 |
|---|---|---|
| =y 构建 | 零新增警告 | RC=0，bzImage #292，sha256 `9f83ab4c…b419e49`（bzimage-sha256.txt） |
| KUnit on×2 + 复跑 | 三套件全绿、旧锚不红 | on1/on2/on-final 全绿：corten 24/0/1 + **corten_arena 123/0/0** + corten_fault 34/0/5（W-4 基线 121→123=新增两锚） |
| KUnit off×1 | skip 对账精确 | 25/0/0 + 26/0/**97** + 7/0/32（arena skip 95→97=新锚 off-skip +2，精确对账） |
| =n 13 对象 | 零 corten 符号 | `make mm/`（=n）RC=0，`nm mm/*.o` 未定义 corten 符号 = **0**；corten*.o 不再产出 |
| checkpatch --strict | 0E/0W/0C | 810 行 diff：**0 errors, 0 warnings, 0 checks** |
| guest 门 | smoke 26/26 双形态等 | 全绿（§4）；registry 家族：implant_drops=0, **j2_violations=0, j2_stale=0**（W-4 为 3→0 ✓）；mmap_region_routes=1（smoke 固定地址映射走新路由的正证据）；refuses=0 |
| **pgtables_bytes 残值** | 判据 ==0 | **1 笔 8192B —— 未达标**（与 W-4 基线同值；诊断与定界见 §6，①的机制已按任务书落地但其前提在本电池被证伪） |

## 1. 改动点清单（file:line = 改后行号）

### mm/corten_arena.c（主体）
1. `:248-253` 计数器 `corten_nr_mmap_region_routes` / `corten_nr_mmap_region_refuses`
   （W-5 显式地址区域路由的披露账本）；`:2855-2861` debugfs arena_stats 渲染。
2. `:3685` 新 helper `corten_arena_frame_ptes_empty(mm, addr)`：ptl 下扫整帧 512
   PTE 判全空（纯读，INV6；无 PT 页/叶 PMD 记空）。
3. `:3838-3843` W-4 遗留①：exit walk owned-mixed 臂 zap 后，帧内 PTE 全空 →
   `corten_arena_free_ptes_span(win, win+PMD_SIZE)` 整帧退役（既有 helper 复用；
   上层页由 phase-B 随后收殓）。机制按任务书落地；本电池实测前提不触发（§6）。
4. `:13790-13802` punch_idle 的 D24 mark 加 W-5 注记：成为非可编码续程
   （MAP_SHARED 族）的专属生产者。
5. `:13883-13893` punch_route 加 `bool admitted` 形参；`:13904-13905` EXACT 臂、
   `:13922-13923` CHUNK 臂的 `corten_implant_mark` 加 `if (!admitted)` 守卫 +
   W-5 注记——admitted 续程（file 区域路由借用拆除臂）registry 写死。
6. `:13949-14080` **新函数 `corten_arena_explicit_region_route()`**（本片核心）：
   MODE mm 的显式地址（MAP_FIXED/MAP_FIXED_NOREPLACE）窗口域安装全量裁决——
   可编码形（classify 白名单旗标形，PRIVATE）：匿名→`corten_arena_auto_attach`
   在显式地址 declare ANON region（novma；parked 窗口由 declare 的 pool prepare
   驱逐/同尺寸再激活）；file→`corten_file_may` + punch_route(admitted=true) 拆
   live 重叠 + `corten_arena_file_attach` declare FILE region（V-B 全机制在树）。
   成功返 1（do_mmap 不再走 mmap_region——mark 腿既有契约）。不可编码形返 0 落
   既有续程；拒绝臂（file_may/corten_auto_validate/OVERCOMMIT_NEVER/declare
   失败）按 funnel 同 errnos 应答 + refuses 计数，**registry 写零**。
7. `:14091-14145` `corten_arena_mmap_route`：签名 `bool file`→`struct file *file`
   + 新增 `pgoff` 形参；先跑显式路由（在 state gate 之前——ENTER-only 无
   registry 的 MODE mm 也覆盖，V-A.3c 契约），再走既有 punch_idle/classify/mark
   腿；classify 调用改 `!!file`。
8. `:15239-15252` P4 backstop 空窗臂加 W-5 注记（可达租户 = 非可编码续程 +
   fail-open funnel）。

### mm/corten_arena.h
- `:282-285` / `:1137-1146` mmap_route 声明与 =n 桩同步新签名。

### include/linux/corten_arena.h
- 无改动。

### mm/mmap.c（白名单例外，理由见 §7）
- `:595-596` 传 `file`（指针本体）与 `pgoff`——显式 file 路由需要文件指针与
  映射偏移，do_mmap 的既有局部量，二行改动。

### mm/corten_fault_test.c
- `:1450/1497/1902/2052` 四处 mmap_route 调用点签名跟随（`NULL, 0`——punch-only
  契约不变，punch 路径不解引用文件指针）。

### mm/corten_arena_test.c
1. W-4 遗留②：`sweep_fork_mirror`（child）、`sweep_file_exit`（child+grand）的
   裸 mm_alloc 改 `kunit_add_action(mmput)` 形，显式 mmput 处
   `kunit_release_action`（mixed_frame_exit 既有范例；断言中止不再漏 mm）。
2. 重钉 `corten_arena_test_mapfixed_over_parked`（W-5 契约：region 进、funnel 出、
   registry 零写入；精确尺寸命中走 pool 再激活而非放置驱逐——placement_idle_ejects
   计数不再移动；implant_covers==FALSE、implant_nr==0、occupancy 翻正）。
3. 重钉 `corten_arena_test_inv_mv2_implant_fork`（route 不再为该形状产 entry——
   改 J2 锚的注入惯例：直接 `corten_implant_mark` + mkvm 孪生；所钉的是 V-A.3c
   registry fork 镜像机制本体）。
4. 新锚 `corten_arena_test_explicit_anon_region`（smoke step-5 形：region 进、
   pool 驱逐、region 故障臂服务内容、munmap 回池、routes+1）与
   `corten_arena_test_explicit_file_region`（shmem 私有 MAP_FIXED：FILE region、
   rfile/rpoff 折算、B.3 read 臂服内容、FILE_MAPPED 元数据、无 VMA 无 entry、
   munmap 全覆盖回池）。+ `corten_arena_test_op_file_mmap` op-runner。

## 2. 植入登记表生产调用点不可达论证表（规格判据承重件）

`corten_implant_mark` 生产调用点全枚举（corten_arena_test.c 的注入式调用为测试
代码，不计生产）。生产侧 4 点，W-5 后可达性：

| # | 调用点 | W-5 后可达形状 | 不可达论证 |
|---|---|---|---|
| 1 | punch_idle `:13802`（P1b 驱逐臂） | 仅非可编码续程（MAP_SHARED/special 族 MAP_FIXED over parked 窗口） | 可编码形（匿名/file 私有）在 mmap_route 先走 explicit_region_route；其 declare 的 pool prepare 自带 parked 驱逐/再激活，punch_idle 不再出现在该路径。guest 全电池 0 次触发（placement 侧驱逐未发生），KUnit 共享 punch 锚仍覆盖此回退 |
| 2 | punch_route EXACT `:13904-13905` | 仅 `admitted==false` 续程 | admitted==true 的唯一来源是显式 file 路由（该形成为 region，非 funnel 安装）；file 私有形不再以 admitted=false 抵达此臂（explicit_region_route 先行接管） |
| 3 | punch_route CHUNK `:13922-13923` | 同上 | 同 #2 |
| 4 | placement_backstop 空窗臂 `:15255`（P4） | 非可编码续程 + fail-open funnel（declare 因分配失败退化等） | 每一可编码显式地址窗口安装都被 explicit_region_route 收编为 region 或拒绝；P4 保留为"响铃不应"后备（V-A.3c 的 registry 创建契约由显式路由在 state gate 前承接） |

**guest 侧恒零读数**（gate §1/§6 前后对照）：implant_drops 0→0、j2_violations
0→0、**j2_stale 0→0（W-4 的 3 归零 ✓）**、mmap_punch_rejects 0→0、refuses 0→0；
正证据 mmap_region_routes 0→**1**（smoke step-5 匿名固定地址安装走新路由）。
mmap_punches 0→16 = mva1_probe punchfork 腿（8 迭代 ×2 的 **memfd MAP_SHARED**
固定地址 punch）：refuses=0 且 routes 未随之 +16 证明其为非私有租户形——即上述
论证表中 #1-#4 唯一的活租户（W-6 白名单 SHARED 桶的原形，KUnit exit_punchfork
同形锚定）。该形状今天仍写 registry（mark 在 admitted=false 续程执行）——这是
**有意保留的残余**（§7 披露 1），guest 计数面（drops/violations/stale）恒零。

## 3. 语义契约与兼容性

- 显式 file 映射成功语义不变：同字节域、同帧、同 errno 面；宿主从 legacy VMA
  换成 region（红线原文）。refuse 臂应答 funnel 同 errnos（file_may 链、
  auto_validate 的 RLIMIT/pkey 面）。
- 收窄两处（披露）：(a) OVERCOMMIT_NEVER 下窗口域显式安装 -EOPNOTSUPP（declare
  臂不建模 VM_ACCOUNT committed 账，W-4 B2 家族——拒绝而非半建模）；
  (b) PROT_EXEC-only 形 -EOPNOTSUPP（与 auto 路由同立场）。
- 不可编码形（MAP_SHARED/hugetlb/growsdown/populate/locked 等窗口域 MAP_FIXED）
  行为与 W-4 完全一致（punch+funnel+registry 租户），未收窄。

## 4. guest 门全读数（results/r07/w5/guest-gate.log）

kernel `6.18.32-g893c8043990d-dirty`（bzImage #292）：
- smoke v2：契约件 sha 37df16d7 ✓，SMOKE PASS 26/26 双形态，rc=0，ledger 归零。
- metis_eq ×2：rc=0 ×2，checksum 自一致且与 W-3fix 基准同值
  （65073 词 / `2d383eeed4ceb73b`）。
- sweep-live：RESULT PASS（anon 13 / file 21 / skip_other 240 / resident 2326）。
- mva1_probe：18/18 全绿（S-1 MAPERR / S-4 maps-clean / FRESH 复激活 / CHUNK
  半弃半活 / punchfork MemFree 对账）。
- S-3 电池：RESULT PASS（branch B 收敛形，swapins=32767 闭合）。
- registry 家族：见 §2 表尾。
- dmesg：corten warn/bug 计数 = 0（唯一非静默行 = §6 的 pgtables 记账打印）。
- 纪律：qemu 只按 pidfile 杀；9p 手动挂 hostshare；串行 make（锁
  project/run/lock）。

## 5. KUnit 读数（on×2 + 终件复跑 + off×1）

- on1（#285）：24/0/1 + 123/0/0 + 34/0/5，notok=0。
- on2（#285）：24/0/1 + 123/0/0 + 34/0/5，notok=0（interlock/负载敏感用例无红，
  无复跑需要——两跑全绿 + 终件第三跑全绿）。
- on-final（#292，洁净件）：同上全绿；kunit-on-final.log。
- off1（#286 =n 前件）：25/0/0 + 26/0/97 + 7/0/32，notok=0；skip 对账精确
  （W-4 基线 95→97 = 新锚 off-skip）。
- 签名对照（对 W-4 基线）：`INV-MV2 violated` WARN 1 次 = inv_mv2_inject 注入锚
  的既定 WARN_ONCE（同基线）；`non-zero pgtables_bytes` on-boot 26 次 = W-4 登记
  的 ④（KUnit 合成 mm PT 泄漏，task #6，W-6 前）家族 24→26（+2 = 本片两个新锚
  的合成 mm 同族泄漏，非新增缺陷类）。

## 6. pgtables_bytes 残值：判据未达标，定界与移交（红如实报红）

读数：电池全程 1 笔 8192B（与 W-4 基线完全同值）。三个诊断 boot（W5DIAG 临时
件，已全部撤除）定界：
- 来源进程 = **sweep-live**（smoke/metis/mva1_probe/S-3 单独跑均 0 次）。
- ①机制前提被证伪：sweep-live 退出路径的每一个 owned-mixed 帧在 walk 时刻都有
  驻留共居 PTE（实测 resident=2..322，无一本空）——任务书"共居 legacy 无驻留"
  的全空形状在本电池不存在，机制正确但零触发。
- 残值定界在 **legacy pass**：stranding mm walk-end 账面 36864（=4 PT+4 PMD
  +1 PUD，linked-pt 全表在案），exit 末剩 8192——free_pgtables 对 W-4 扫入
  造成的树内空洞（adopted region 的 vma-less 间隙）走 floor/ceiling 几何门时
  漏收 2 页。PT 页本体 free 不设门（free_pmd_range:219），漏的是上层页记账。
- 定性：这是 W-4 扫入形态与上游 free_pgtables 几何假设的交互残差，不是 ①命名
  的形状。修法要么在 walk 内提前代做 unmap_vmas（违背 walk 宪章、重写 legacy
  zap 记账，风险不成比例），要么动 mm/memory.c（超白名单）。**按 ④ 先例登记
  移交主会话裁定**；①的 prescribed 机制保留在树（对真全空形状正确且无害）。

## 7. 披露与移交清单

1. **registry 的活租户**：MAP_SHARED memfd 窗口 punch（mva1_probe punchfork 腿，
   guest 16 次）仍走 punch+funnel+registry——非可编码租户形，W-6 白名单 SHARED
   桶原形。若主会话要求"恒不可达"严格零（含此形），补一个 -EOPNOTSUPP 拒绝臂
   即可（显式路由的 shared 分支返拒绝 + 计数），但 exit_punchfork 等 KUnit 锚
   需随契约重钉——不在本片擅断。
2. **pgtables 残值**（§6）：判据红，移交。
3. **白名单例外**：mm/mmap.c `:595-596` 二行（file 指针 + pgoff 传参——显式
   file 路由的承重输入，无它路由不可实现）；mm/corten_fault_test.c 四处调用点
   签名跟随。理由如上，其余全在白名单内。
4. **=n 全量构建的既存红**（非本片引入，893c804 原样）：fs/proc/task_mmu.c
   `:891/896/1007/1040/1083` 在 CORTEN_MM=n 下隐式声明 `corten_row_file/
   corten_row_flags`（mvc 波的 procmap query 接线落在 `#ifdef
   CONFIG_CORTEN_MM_ARENA` 之外）。W-4 的 =n 检查只构建 mm/ 故未暴露。修复需动
   task_mmu.c（超白名单），登记移交。
5. **④ 家族 +2**：两个新锚各一枚合成 mm PT 泄漏（task #6 同族，§5）。
6. 环境：遗留一周的 tmux `vm` 会话（W-4 窗口残 VM，持 10022/镜像写锁）已精确
   清理；`trixie.img` 需 `systemd.mask=sys-kernel-config.mount` append（两次
   emergency-mode 复演后并入 gate 脚本）。

## 8. 工件（results/r07/w5/）

build-y1/y2/y3/final.log、build-n-objects.log、config-pre-n.snapshot、
kunit-on1/on2/on-final/off1.log、w5-full.diff、bzimage-sha256.txt、
w5-guest-gate.sh、guest-gate.log、本报告（next/w5-dev-report.md）。
