# PR-0 dev report（MV2 删除账 §1.3 · implant 树内退役）

agent: pr0-dev（CortenMM kernel-MM; MV2 删除账 PR-0 片）。
基线: worktree /home/ppw/linux-6.18-w4 @ 47cc6cfba90f（分支 w4-pr0, W-3fix5 收口态, 开工干净）。
不 commit。工件 results/r07/pr0/（/home/ppw/cortenmm/results/r07/pr0/）+
patches/r07-pr0.diff。授权: MV3.e 删除账清单 §1.3 PR-0 + 主会话任务书。

---

## 0. 判定总览

| 项 | 判定 |
|---|---|
| bss implant 收编（mmap.c vm_brk_flags 腿 → declare/region 形） | **绿（落地, 真生产者路径, 3 锚全红→绿面）** |
| MAP_SHARED punch 登记形裁决（D33 留守或裁） | **裁=留守（登记豁免口径成文, 结构论证 + 锚在案, §2）** |
| implant 登记表收缩 | **落（bss 生产者死; 活生产面收缩为 D33 SHARED 白名单 + fail-open backstop, 生产者普查入码）** |
| =y / =n 构建 | **绿: 零新增警告（唯一=基线 objtool cpuidle r04 判例）; =n RC=0 零警告, 消费对象零 corten 符号** |
| KUnit on ×3 + off ×1（filter_glob=corten*） | **绿: 26/0/1 + 144/0/0 + 34/0/5; on1 唯一 fail=台账 #7 已知 interlock 时序 flake（20.3s 运行时长）, on2/on3 复跑全绿; off skip 对账 +3 恰=新锚** |
| checkpatch --strict 全量 diff | **0E/0W/0C（703 行, "ready for submission"）** |
| mmbench 动态协议前后对照 | **噪声持平（如实报, §6: stock 对照臂跨 boot ±100% 盖过判据格; 同 boot MODE/stock 税比无回归信号; PR-0 触碰路径在该负载零调用）** |

**一句话**: PR-0 把删除账 §1.3 的第一靶——窗口域 bss implant——从
「funnel VMA + 登记表豁免」迁到「region 记录（树归零）」: vm_brk_flags 的
窗口腿现在先走 declare 路由（W-3 heap seed 的机械 + W-7 同帧桶吃邻接 FILE
记录的重叠）, 零 VMA 零登记; MAP_SHARED punch 登记形按 D33 裁定留守并成文
豁免口径（租户 VMA = pagecache 页的 rmap 锚, arena 无 shared write-through
服务臂）; 登记表的生产者普查随之收缩。A1-A6/B1-B4 的「窗口臂命中产物恒
NULL」按留守裁定收窄为「恒 NULL for every non-punch probe」——PR-1 短路
门设计保留 implant 命中豁免臂（D33 SHARED 租户专用）, 判据链如实入档（§4）。

## 1. 改动统计（5 文件, +556/−19; diff 703 行）

| 文件 | 行数 | 内容 |
|---|---|---|
| mm/mmap.c | +31/−19 | vm_brk_flags 腿重构: 窗口门（两比较, 原 gate 原样保留）+ `corten_bss_declare_route()` 调用, 0=region 答复（early-return 完成腿, 无 populate 可做——declare 守卫已把 populating mm 全部降级）; 1=计数降级, 原 funnel + implant mark 原样保留为守卫延拓; MV3.c 旧注记块重写为 PR-0 形 |
| mm/corten_arena.c | +176 | `corten_bss_declare1()/corten_bss_declare_route()`（brk 路由族同款 0/1 契约）; 计数器 `corten_nr_bss_declares/bss_legacy` + debugfs `bss_declares/bss_legacy` 行; 测试钩 `corten_arena_test_region_fault()`（vma-less fault 入口）+ `corten_arena_test_bss_route()`; implant_mark 头注 + punch 路由头注的 D33 裁决与生产者普查成文 |
| mm/corten_arena.h | +31 | 路由声明 + =n static inline 折叠（恒 1, 窗口门随 =n 消失）; 登记表头注的生产者普查 + D33 豁免口径 |
| include/linux/corten_arena.h | +10 | 测试访问器声明（`corten_arena_test_bss_route/region_fault`） |
| mm/corten_arena_test.c | +308 | op 包装 ×2（真 vm_brk_flags 腿 / vma-less region fault, 均走 use_mm op worker）+ 新锚 ×3（§3）+ 套件注册 |

## 2. 实现摘要

### 2.1 bss 收编（declare/region 形, 复用 W-7 同帧多段机械）

`corten_bss_declare1()`（corten_arena.c, brk 路由族同款 0/1 契约）:
- 门: MODE + corten 使能 → 窗口包含（[16T,64T] 全含）→ def_flags
  VM_LOCKED 降级（legacy 臂 populate, region 不能）→ OVERCOMMIT_NEVER
  降级（funnel VMA 的 VM_ACCOUNT committed charge declare 臂不建模——
  W-5 admission 的拒绝族）→ `may_expand_vm()`（do_brk_flags 的原词:
  VM_DATA_DEFAULT_FLAGS|VM_ACCOUNT|def_flags|vm_flags; 拒绝则降级让
  funnel 自己答同一 -ENOMEM）。
- declare: 状态机按需创建（A5, mmap_write 下）→ perm =
  USER|READ|WRITE(+EXEC 当 VM_EXEC) → `corten_arena_declare_locked(novma)`
  ——零 VMA、零登记; W-7 帧桶吃与下方 FILE 记录的同帧页重叠（exec 镜像
  的 bss 正落在最后一段 FILE region 的边界帧上, 即「同帧多段 ELF」形）。
- 计数降级披露: 每答 1 = 一个没收编的 bss（`corten_nr_bss_legacy`）,
  D28 口径; 降级延拓 = 原 funnel VMA + implant mark（occupancy truth
  不变）。
- mmap.c 侧: 窗口门保持内联两比较（非 MODE/非窗口调用者成本不变）,
  采纳腿 early-return 完成尾（unlock + uffd complete; 无 populate——
  VM_LOCKED 已在 declare 守卫降级）。

### 2.2 MAP_SHARED punch 登记形裁决: **留守**（D33 复核维持）

读现码裁决链（三项独立, 任意一项即否决「可裁」）:
1. **rmap 锚**: punch 洞的租户 VMA 是真 legacy file VMA（i_mmap 节点）,
   页 cache 页的 writeback/truncate/reclaim 走 rmap_walk_file 到达它的
   翻译。arena 的 novma 机械（folio_add/dup/remove_file_rmap_novma +
   W1.b corten_arena_unmap_file_range + W1.d corten_rmap_ttu 前置降级）
   服务的是 FILE region 的 private/COW 消费——锚职责没有「已被替代」:
   SHARED 写穿租户今天完全由 funnel 经该 VMA 服务（implant 登记项只是
   J1/J2 豁免, 不参与服务）。
2. **服务臂缺失**: V-B 的写臂按合同 COW-only（「MAP_PRIVATE never
   writes through」）; region 形拿不下 MAP_SHARED 需要新建写穿 +
   脏跟踪 + 多进程一致性机械——feature, 不是删除 PR 的边界件。
3. **D33 本判**: 「MAP_SHARED punch 走 punch+registry 是规格正确的
   结构性白名单行为」（probe punchfork 的 memfd 形 16 次活体;
   drops/violations/stale 恒零）。
成文位: implant_mark 头注（生产者普查 + D33 + rmap 锚论证）、punch 路由
头注（裁决段）、mm/corten_arena.h 登记表头注。**登记豁免口径**: 留守形
的登记项即其 J1/J2 豁免（wl SHARED 桶记账）; guest 计数面
（drops/violations/stale）恒零维持。

### 2.3 登记表收缩（PR-0 后生产者普查）

| 生产者 | PR-0 后状态 |
|---|---|
| mmap.c vm_brk_flags 腿（bss） | **死**（采纳腿零登记; 降级延拓保留——计数可见） |
| punch 路由 EXACT/CHUNK 两臂（!admitted） | 活 = D33 SHARED 白名单 |
| P1b idle-eject（placement_punch_idle） | 活 = D33 SHARED 白名单 |
| placement backstop 空窗臂 | 活 = fail-open backstop（W-5「恒不可达 for 可迁形」口径维持） |
登记表 API 本体（mark/covers/covers_lockless + J1 豁免门 + J2 白名单
分类器）**不删**——留守世界它仍是 wl SHARED 桶的记账面。

## 3. KUnit 锚（新增 3, 套件 141 → 144; 全绿 on×3）

| 锚 | 套件位 | 锁死的契约 |
|---|---|---|
| `corten_arena_test_pr0_bss_adopt` | arena #17 | 真生产者 roundtrip: op worker 上真 `vm_brk_flags()`（current->mm 即受测 mm, 与 loader 同形）。腿① RW 3P @窗口基: region 答复（novma/auto_shape/RW perm）、vma_lookup NULL、implant_nr==0、implant_covers false、total_vm +3、map_count 不动; vma-less fault（`corten_arena_test_region_fault`, W-2 GUP 臂入口）装翻译 + 词 roundtrip（内容保真）。腿② exec 标 2P @同帧: W-7 桶双记录（独立 ar、EXEC perm 携带）、树仍空、登记仍零; INV-MV3 桶不变式 + J2-complete 走查 0 违例; mode_exit 干净收场。bss_declares +2/bss_legacy 不动 |
| `corten_arena_test_pr0_bss_degrade` | arena #18 | 降级契约: def_flags VM_LOCKED → bss_legacy +1、funnel VMA 在树（populate 腿原样）、implant_nr==1、implant_covers true、J2-complete 走查 0 违例（白名单自证）——pre-PR-0 形 = 守卫延拓, 逐断言在案 |
| `corten_arena_test_pr0_shared_punch_whitelist` | arena #19 | **D33 留守锚**: 活窗 + memfd MAP_SHARED MAP_FIXED punch（真 do_mmap funnel、EXACT 级、mmap_punches +1）→ 租户 VMA 在树（file-backed = rmap 锚的字面断言）、登记项恰一（implant_nr==1/covers true）、J2-complete 走查 0 违例（豁免履职）、租户 funnel 服务端到端（内容 fault+roundtrip） |

## 4. 判据对账（任务书判据 → 实测, 含裁定偏差如实）

- **「窗口域 implant 登记表项归零」**: bss 族**归零**（采纳腿零登记,
  锚断言）; 登记表整体非恒零——SHARED punch 留守世界它恰是 D33 白名单
  的记账面。此为按任务书「裁定留 SHARED 形则登记豁免口径」支路的
  **裁定后读法**, mv3e §1.3 原文「A1-A6/B1-B4 窗口臂命中产物恒 NULL」
  相应收窄为「恒 NULL for every non-punch probe」; **PR-1 短路门设计
  保留 implant 命中豁免臂（D33 SHARED 租户专用）**—— §1.3 的
  「落后直删」按此口径执行。
- **「tree_entries 白名单外==0 维持」**: 维持（新锚三形 whitelist walk
  全 0 违例; 采纳形窗口零树项、降级/留守形全在 IMPLANT 桶）。
- **「J1/J2 维持零」**: 维持（J2: 三锚 walk==0 + 套件全绿; J1: 豁免门
  未触新形——采纳形无树项可命中）。

## 5. 验证链（终码全链; 终件 = bzImage-pr0-final d55a62eb…, #5）

| 门 | 结果 |
|---|---|
| =y 构建 | RC=0 零新增警告（唯一 = 基线 objtool cpuidle_enter_state, r04 判例; build-y-pr0.log）; =n 往返还原后收敛重建 #5（fixdep 陈旧 .d 一过, 重跑收敛——W-6 教训复用） |
| KUnit on ×3 | on1: 25/0/1 + 144/0/0 + 34/0/5（唯一 fail = `corten_test_txn_uninstall_interlock`, 台账 #7 钉 CPU kthread 时序 flake, 本跑 20.3s）; on2: **26/0/1 + 144/0/0 + 34/0/5 全绿**（flake 复跑绿判定）; on3（终码 #5）: 同全绿 |
| KUnit off | 27/0/0 + 28/0/**116** + 7/0/32; skip 对账: W-3fix5 基线 113 + 新锚 3 = 116 精确; 三新锚 off 全 SKIP（理由串在案） |
| =n 折叠 | 全树重建 RC=0 零警告; **17 对象清单零 corten 符号**（mva2-verify-par.sh 口径: mm/{mmap,mmap_lock,memory,mprotect,madvise,mremap,rmap,gup,oom_kill,swapfile,migrate,mempolicy,vma,mincore,msync}.o + arch/x86/mm/fault.o + arch/x86/kernel/sys_x86_64.o）; fs/proc/task_mmu.o =n 干净; config 已恢复 =y 并重建（on3 全绿闭环） |
| checkpatch --strict | **0E/0W/0C**（703 行, "ready for submission"） |
| mmbench 动态协议 | §6（噪声持平, 如实报） |

## 6. mmbench 动态协议前后对照（如实报: 环境噪声主导, 无回归信号）

协议（w3fix5 §6 同款）: mmbench_dyn（9p share 原件）+ corten_mode_hook
LD_PRELOAD（guest 内重编, "MODE on" 实证）+ seed 公式（mode 20260951-53 /
stock 20260979-81）+ min_seconds=2 + 三遍取中位 + 每 boot 判据
pool_parks>0（MODE 路径实走: base=1334/base2=17519/pr0=1394）。append 按
w3fix5 bench boot 原形（sys-kernel-config.mask 等三件）。

| 腿 | 内核 | 判读 |
|---|---|---|
| before（base） | mva 树 38a164b6fa05（W-3fix5 内容 = 47cc6cfba90f diff 同内容; bzImage b31898fc… = w3fix5 归档件, vermagic 6.18.32-gb4b425245193-dirty） | 全 12 run 完成 |
| before 复跑（base2） | 同上 | 撞宿主负载尖峰（mode 先跑臂被打穿: mode/stock 比劣化 5 倍）——弃为对照, 留证 |
| after（pr0） | bzImage-pr0-final d55a62eb…（6.18.32-g47cc6cfba90f-dirty #5） | 全 12 run 完成 |

- **stock 对照臂跨 boot 摆动 +94~116%**（base 4.01e-4 → pr0 8.65e-4 @t4
  中位）——stock = 非 MODE legacy 路径, PR-0 对它零触碰（唯一共享行 =
  vm_brk_flags 的两比较门）, 该摆动即环境噪声的直测: 宿主 mv3d 长电池
  （另一会话, 未触碰）同机运行, 噪声带 ±100% 盖过判据格。
- **同 boot MODE/stock 税比**（同 boot 双臂共享负载相位, 唯一抗噪读数）:
  t4 7.0% → 5.2%, t8 3.4% → 2.2%——无回归信号, 方向偏好但不作主张。
- **结构论证（判据格不动的主因, 与 w3fix5 §6 同判词形）**: PR-0 触碰
  路径 = 窗口域 vm_brk_flags 腿 + bss declare; mmbench 工作负载
  （mmap-pf: mmap+fault 循环）零 brk 调用, 且 loader 先于 MODE 进场
  （boot 无 corten_mode_default, MODE 来自 hook 的 prctl）——bss 路由
  在该负载**零调用**, 绝对值位移不具归因力。
- **判定: 噪声持平, 无回归信号。** 稳态读数建议安静宿主窗重跑配对
  A/B（w3fix5 移交第 3 条同款登记）。

## 7. 验证门之外的披露

1. **测试钩新增一个**: `corten_arena_test_region_fault()`（corten_arena.c,
   =y 侧测试面）——vma-less fault 入口（`__corten_arena_handle_mm_fault`
   的 W-2 GUP 臂形）。首版锚走了「scratch shadow-VMA + 公共包装」形,
   FRESH 臂的 `folio_add_new_anon_rmap` VM_BUG（地址不在 vma 内）红面
   实测照出后改走 vma-less 入口——红绿链在 on1 首跑 log（kunit-on1 前
   一版）, 如实披露。
2. **w4 树首次构建**: 本片在 w4 worktree 从零起建（config 自主树拷贝 +
   olddefconfig, No change）; 构建计数 #1（=y 首件）→ #3（=n）→ #5
   （=y 恢复终件）。=y 首件 bzImage-pr0-y（044e7c13…）与终件分档归档。
3. **宿主共存**: mv3d 长 battery VM（port 10031, trixie-mv3d.img, 另一会话）
   全程未触碰; 本片 VM（trixie.img, port 10022）跑完即清（tmux vm +
   qemu.pid 收尾, 三腿均清）; KUnit VM 每跑即清。
4. base 内核 = mva 树 38a164b6fa05 而非字面 47cc6cfba90f: 两树同 diff
   内容（同标题 W-3fix5、同基 b4b425245193/88ec127bbad3 链）, 交验披露
   （mva 树是 w3fix5 的原生构建地, bzImage b31898fc 与归档件同 sha）。

## 8. 工件清单（/home/ppw/cortenmm/results/r07/pr0/）

bzImage-pr0-y（#1 首件, 044e7c13…）+ bzimage-pr0-y-sha256.txt +
bzImage-pr0-final（终件 #5, d55a62eb…）+ bzimage-pr0-final-sha256.txt;
config-y-pr0.snapshot / config-y-pr0-final.snapshot; build-y-pr0.log +
build-n-pr0.log + build-y-restore-pr0.log; kunit-on1/on2/on3/off.log +
pr0-kunit.sh; mmbench: mmpf-{mode,stock}-t{4,8}-k{1,2,3}.{base,base2,pr0}.json
（原始 18+ 件, 含 ssh/hook 噪声行）+ pool-parks-{base,base2,pr0}.txt +
pr0-mmbench.sh + launch-{base,base2,pr0}.log + hook-build-*.log;
pr0-full.diff（= /home/ppw/cortenmm/patches/r07-pr0.diff）。
本报告: /home/ppw/cortenmm/next/pr0-dev-report.md（w4 树镜像
project/next/pr0-dev-report.md）。

## 9. 移交

1. **PR-1（A 组窗口臂短路）**: 短路门前置 MODE 门 + [16T,64T) 双比较;
   按 §4 裁定读法, implant 命中豁免臂保留（D33 SHARED 租户专用）, 其余
   窗口走查（产物恒 NULL 的面）直删。
2. PR-2（B 组门控）/ PR-3（C1 backstop 降级断言）/ PR-4（D 组渲染臂）:
   mv3e §1.3 序列不变。
3. mmbench 稳态配对 A/B: 安静宿主窗（mv3d 电池收官后）重跑, 本片协议与
   seed 已工件化可直跑（pr0-mmbench.sh <tag> <bzImage>）。
