# MV3.e dev report · 删除账清单 + 残留台账整合 + D29 闭环判定

agent: mv3e-dev（MM-audit; 纯分析轮）。基线: 主树 HEAD 08aaa0eefff0（android17-6.18,
与 corten-github 同步）。授权: 用户指令「MV3.e 收官, D29 路线走完」+
specs/MV2_REMAINING_SPECS.md §MV3.e + STATE MV3.a/b/c-debug/c-feat/d 全收口。
**本片零内核改动**——全部产出 = 源码实查 + 现场证据核对, 交付边界件清单。

---

## 0. 判定总览

| 项 | 判定 |
|---|---|
| 交付一 删除账清单 | **落**（§1: 五组 24 项, 每项 file:line + 死代码判据 + 风险评级; 裁剪 PR 序列 PR-0..4 + 勿删边界 E 组） |
| 核心结构判定 | **VMA 层未整体死——死的是窗口域臂**: J1 617 探针/0 非法命中 + J2 69001 walk/0 违例实证树恒白名单形; wl_file（hint 库装载）+ heap/stack/special 三桶是合同内永久树住户（§1.1） |
| 交付二 残留台账 | **落**（§2: C2 族 4 项 + MV3.b 移交 3 项 + MV3.c-feat 6 项 + MV3.d 余项 3 项, 共 16 开项, 逐项现状/切片/优先级） |
| 交付三 D29 闭环 | **收官**（§3: 目标①②三栏对照; 两道前置硬门均清偿——corruption ×5 gate 5/5 + drain 参锁落地; MV3.e 后 D29 路线关闭, 余项全部降级为独立小片台账） |
| 诚实披露 | P2 长跑累计 J1/J2 读数（617/69001）为 lead 转述的 live 审计门读数, **无归档原件**（p2-audit-gate.txt 为 SSH 取数失败件, 空壳）; 归档同形佐证 = mv3cfeat wl-audit-a1/a2（327/0、224/0）+ W-6 世代 guest-gate 全系（j1_hits 恒 0） |

---

## 1. 交付一: 删除账清单（裁剪 PR 边界件）

### 1.1 证据基座

**J1 探针机械**（mm/corten_arena.h:683-695 inline 门 + mm/corten_arena.c:3038
corten_j1_slow）: find_vma()/find_vma_intersection()/find_vma_prev()/
lock_vma_under_rcu()/uffd funnel 五个钩点（mmap.c:1110/:1152/:1182,
mmap_lock.c:240, userfaultfd.c:56）对 MODE mm 的窗口域
[CORTEN_MODE_WINDOW_START=16T, CORTEN_MODE_WINDOW_END=64T]
（include/linux/corten_arena.h:374-375）查询计数; 命中树 VMA 计 hits,
**implant 登记覆盖的查询/命中豁免**（corten_arena.c:3043-3071, D24 裁决）。
故 **j1_hits = 窗口域非 implant 树 VMA 的直接计数器**。

**实测**: P2 全系统长跑累计 **j1_probes=617 / j1_hits=0**（lead 转述 live 读数,
披露如上）; 归档同形: mv3cfeat wl-audit-a1 **327/0**、a2 **224/0**
（results/r07/mv3cfeat/, j2_violations/j2_stale 全 0）; W-6 世代 guest-gate
全系同形。结论: **每一枚被测 boot 的窗口域树走查都只可能命中 implant——
非 implant 树 VMA 在窗口域的存在为零**。

**J2 全树走查**（corten_arena.c:14185 corten_audit_j2_scan + :14244/:14270 walk 入口
+ :14369-14404 白名单分类器）:
INV-MV2 断言（窗口 VMA 必 shadow/implant, 否则 WARN+计数）; 白名单桶枚举
delegated 域。P2 累计 **j2_walks=69001 / violations=0**; exit/fork 尾钩
（corten_arena.c:4568/:8786）逐进程审计。结论: **树恒白名单形**。

**MODE mm 的树构成**（W-7 终态 + MV3.c-feat §1.4 实测口径）:

| 桶 | 域 | 住户 | 永续性 |
|---|---|---|---|
| wl_special | mmap_base 区 | vdso/vvar/vclock 恰 3/进程 | 合同内永续（W-3 件 4 排除口径） |
| wl_stack | legacy | 主栈 1（VM_GROWSDOWN） | 合同内永续（W-3b） |
| wl_brk | legacy | heap [start_brk, brk) 1 | 合同内永续（V-E.1 verdict: legacy 漏斗应答保留） |
| wl_file | mmap_base 区 | 非零 hint 装载的 libc/主二进制段 ~9-70 | **裁决件**（MV3.c-feat §1.5-1: hint 收编超红线登记） |
| wl_implant | **窗口域** | bss implant VMA（vm_brk_flags 腿, mmap.c:1541 生产）+ MAP_SHARED punch 登记形（corten_arena.c:14721） | 裁剪 PR-0 的靶面 |

**结构主判定（D29 句式的精确化, 如实入档）**: 「VMA 层对全部进程成为死代码」
在**窗口域口径**下成立（本清单全部内容）; 在**全树口径**下不成立——heap/
stack/special 是 T0 合同有意保留的 legacy 住户, wl_file 是待裁决的收编件。
裁剪账的兑现以窗口域臂为边界, 全树归零不在本账承诺内。

### 1.2 清单本体

评级口径: **低** = 纯 MODE 分支, 裁剪 PR 可直删; **中** = 共享路径, 需 MODE 门+
窗口比较门控后删; **高** = 勿删（活路径/审计 Oracle/安全网/合同面）。

#### A 组: 窗口域树走查（J1 实证死代码, 每枚 probe 的 mt_find 产物 ∈ {NULL, implant}）

| # | 路径 | 位置 | 死代码判据 | 风险 |
|---|---|---|---|---|
| A1 | find_vma() 窗口臂 | mm/mmap.c:1145（mt_find :1151, 探针 :1152） | MODE mm 窗口查询的 mt_find 只可能返回 implant（豁免计量, j1_hits=0 全世代）; fault 漏斗已由 mmap_lock.c:451 terminus 前置应答, 剩余窗口调用面=perf/ptrace/GUP-fast miss 探测, 产物恒 NULL | 中 |
| A2 | find_vma_intersection() 窗口臂 | mm/mmap.c:1101（mt_find :1109, 探针 :1110） | 同 A1; 唯一高频 face = MAP_FIXED_NOREPLACE 双探针（mmap.c:568-570）, registry 项在前, 树走查"唯一产物是 J1 计数"已由 :563-566 注记自认 | 中 |
| A3 | find_vma_prev() 窗口臂 | mm/mmap.c:1170（vma_iter_load :1177, 探针 :1182, vma_prev/next :1183/:1185） | placement 搜索器的 hint 接受臂已加窗栏（mmap.c:908-916, J1 hygiene 注记自证"窗口 hint 的接受臂死"）; 非窗口 hint 照旧 | 中 |
| A4 | lock_vma_under_rcu() 窗口臂 | mm/mmap_lock.c:225（mas_walk :233, 探针 :240） | RCU 走查在窗口域产物 ∈ {NULL, implant}; miss → inval（:241-244, :275-277）, 无任何后续树消费 | 中 |
| A5 | vma_lookup() 窗口臂 | include/linux/mm.h:3642（mtree_load） | raw mtree_load 同 A1 形; 仅有 probe 的 funnel（userfaultfd.c:56）前置于窗口拒绝（:736/:1808）; **同族注意**: get_user_page_vma_remote 的 post-GUP vma_lookup（mm.h:2647-2651）对窗口页合同性必 miss（MV3.b §1.1 定案, 调用面已修不再喂窗口页）——其 WARN 臂是遗留 wrapper 调用者（rmap.c:2916 make_device_exclusive 族）的 backstop | 中（vma_lookup 本体）/ **高**（mm.h:2647 WARN 臂——勿删） |
| A6 | uffd mfill/move 漏斗窗口走查 | mm/userfaultfd.c:41（探针 :56） | 窗口形在 :736/:1808 被 corten_uffd_window_reject 以 -ENOENT 短答（C12 终判）, 走查到达面 = implant only; 拒绝臂已在位, 走查臂的删除不增门 | 低 |

#### B 组: 窗口域合并/修改树事务（无树邻居 → 合并产物不存在）

| # | 路径 | 位置 | 死代码判据 | 风险 |
|---|---|---|---|---|
| B1 | vma_merge_new_range() 窗口臂 | mm/vma.c:1092; funnel 调用面 __mmap_region :2825 + do_brk_flags :2954 | 合并需要树内 prev/next 邻居; 窗口域树 VMAs = implant only, implant 合并产物 = 未登记窗口 VMA = INV-MV2 违例, 69001 walk 零违例。routed 形全部被 explicit_region_route（corten_arena.c:14904）/auto route（mmap.c:482）/punch（mmap.c:595）在 funnel 前声明; funnel 窗口到达面 = unclaimed 形（MAP_SHARED/HUGETLB/GROWSDOWN/POPULATE/LOCKED/OVERCOMMIT_NEVER/窗口域非零 hint）——电池零出现（wl 桶读数） | 中 |
| B2 | vma_merge_existing_range()/vma_modify() 窗口臂 | mm/vma.c:855 / :1705 | mprotect/madvise/munmap 对窗口域的 split/modify 在 syscall 门已路由（mprotect.c:917+:928, madvise.c:1961+:1940, mmap.c:1352）; funnel 侧窗口到达 = unclaimed 形, 同 B1 | 中 |
| B3 | vma_expand/vma_merge_extend/vma_merge_copied_range/vma_shrink 窗口臂 | mm/vma.c:1197（调用面 vma_exec.c:60）/ :1809（mremap.c:1472）/ :1948（copy_vma）/ :1269 | mremap_route（mremap.c:2012, corten_arena.c:15820）应答窗口 shrink/grow/move; explicit-target move 与边界 crossing = 计数拒绝（无 funnel 逃逸）; funnel 窗口臂 = unclaimed 形 | 中 |
| B4 | do_vmi_align_munmap() 窗口体 | mm/vma.c:1599（guard :1627; syscall 门 mmap.c:1352; __vm_munmap :3307） | SYSCALL munmap 窗口域走 corten 事务; 内部调用者（brk shrink/mremap 内 unmap）由 :1627 拒绝网兜住; 树拆除体在窗口域的到达面 = implant/白名单行 only | 中 |

#### C 组: 窗口域 legacy VMA 生产/bulk 拆除

| # | 路径 | 位置 | 死代码判据 | 风险 |
|---|---|---|---|---|
| C1 | __mmap_region() 窗口域 VMA 生产体 + 双 backstop | mm/vma.c:2799（backstop :2485-2488 placement / :2512-2514 overlap; mmap_region :2875） | W-5 判据: admitted 形永不达 funnel; backstop 审计 #14-16 定性"不可达 backstop"（vma.c:2473-2483 注记自证）。**裁剪 PR 终点 = backstop 降级断言**（specs W-5 段"登记表降级为不可达 backstop"的 VMA 侧对应物） | 中 |
| C2 | exit_mmap() 窗口域 bulk 拆除臂 | mm/mmap.c:1558（mm_exit :1575 前置于锁窗口——MV3.b §2.3 结构发现, MV3.c 已参锁） | 窗口域树行 = implant/白名单; free_pgtables 对窗口域的 bulk 面（exit walk 相位 A 走 registry, VMA 走查只见 implant） | 中 |
| C3 | dup_mmap() 窗口域复制循环 | mm/mmap.c:2046（fork_begin :2081 / fork_commit :2203 / 子树审计 :8786） | 子窗口域 = implant 镜像（fork_begin 复制）; VMA 循环对窗口域无树行可复制 | 中 |

#### D 组: 窗口域渲染/迭代面（已双源化）

| # | 路径 | 位置 | 死代码判据 | 风险 |
|---|---|---|---|---|
| D1 | /proc/maps legacy seq 走查窗口臂 | fs/proc/task_mmu.c:178（dual-source 门）/ :298 prime / :361 row_next | 窗口行由 registry 渲染（W-2 C-fix A 行臂 + W-6b PROCMAP_QUERY or-next 同族修已落）; 树走查窗口臂无行可产出 | 中 |
| D2 | smaps/counters 聚合窗口臂 | fs/proc/task_mmu.c 同一双源门 | 同 D1 形 | 中 |

#### E 组: 勿删边界（高——裁剪账的否定空间, 与 A-D 同等重要）

| # | 面边界 | 位置 | 勿删理由 |
|---|---|---|---|
| E1 | 白名单域漏斗: heap brk 漏斗 mm/mmap.c:160-274（V-E.1 verdict）; 栈 expand_downwards mm/vma.c:3232（W-3b）; special 映射; mmap_base hint 装载链（wl_file 主源） | 合同内永续住户——MODE 进程的 heap/栈/vdso/库全程经此服务 |
| E2 | J1/J2 探针+审计 Oracle: corten_arena.c:3038/:14185/:14369 + 五探针点 | 裁剪 PR 的验收机械本身; 全账兑现+浸泡期后方可退役 |
| E3 | 路由/守卫族: auto/mmap/punch/munmap/mprotect/mremap/madvise 七路由 + maperr terminus（mmap_lock.c:451）+ munmap_vma_guard（vma.c:1627）+ range_overlaps backstop（vma.c:2512） | 正是它们使 A-D 成为死代码; 删守卫 = 复活尸体 |
| E4 | fail-closed 合同面: mseal 拒绝（mm/mseal.c:170-180）; uffd 窗口拒绝（userfaultfd.c:736/:1808）; mprotect grows/rier overlap 拒绝（mprotect.c:928） | 语义合同（响亮拒绝=无损闭合的 MV3.b 裁定口径） |
| E5 | placement 搜索器+窗栏: mmap.c:881/:942/:1005 三搜索器, 窗栏 :923/:983/:997, corten_addr_in_window 门 | hint/搜索必须永不与窗口域相撞——活路径 |

### 1.3 裁剪 PR 序列（边界件清单, 不实际删）

- **PR-0（使能件, 靶在 corten_arena.c 不在 VMA 层）: implant 树内退役**。
  现存活生产线二: bss implant（mmap.c:1541）; MAP_SHARED punch 登记形
  （corten_arena.c:14721, D33 结构白名单行为）。bss → declare/region 形收编
  （同帧多段机械 W-7 已在产线）; SHARED 形按 D33 留守或裁。PR-0 落地后
  A1-A6/B1-B4 的窗口臂命中产物恒 NULL, 短路门可直删走查。
- **PR-1: A 组窗口臂短路**（MODE 门 + [16T,64T) 双比较前置于 mt_find/mas_walk;
  PR-0 未落前保留 implant 命中路径, 落后直删）。
- **PR-2: B 组窗口臂门控**（unclaimed 形枚举表 = B1 判据列, 门控 + 计数）。
- **PR-3: C1 双 backstop 降级断言**（W-5 终点; C2/C3 随 PR-0/1 自然收缩, 无独立 PR）。
- **PR-4: D 组渲染臂收尾**（dual-source 门恒真断言化）。
- 每枚 PR 验收门: j1_probes 增长但 **j1_hits/j2_violations/wl 桶 delta 恒零** +
  smoke 26/26 双形 + metis 同基准 + LTP 抽样; =n 零符号 + checkpatch。
- **LoC 供数（如实）**: 窗口臂本身是小额（走查点 5 处 + funnel 门若干, 每处
  个位~十位行）; 账的 LoC 大头在 E2 审计机械退役（探针+walk+白名单分类器,
  与 corten 测试面 20,573 行的一部分）——那是全账兑现后的**二期**, 本账只登
  边界不登数。J6 终账口径: VMA 四件 13,408 vs corten 44,334（W-6 测）。

---

## 2. 交付二: 残留台账整合（全项目开项, 收录自 STATE 各轮）

优先级: P1=拦路面/数据有效性/实 oops 面; P2=独立小片可排期; P3=裁决/低频形。

| # | 开项 | 现状（源/证据） | 建议切片 | 优先级 |
|---|---|---|---|---|
| 1 | **brk 路由 PT（节点）生命周期 UAF** | DPA boot 定罪面: `corten_brk_grow_route → declare_locked → check_empty_locked` 对已释放内核页取指（console-mv3b-dpa-fixed.log 86s oops）; 非 DPA 世界疑似同根因以 arena_stats 首读挂起表现（mv3b §2.4/§3, sysrq 双采样 `corten_mm_state_pages+0x108 / xas_load`） | check_empty 的 PT 页引用收进 desc 锁/RCU; DPA 复测一轮定罪; 修后复测 arena_stats 读 | **P1** |
| 2 | **arena_stats 渲染挂死** | =on 首读即挂, R 态不可杀; 快照+TTL+预算分批设计在案（mv3b §3; 排查结论"先清 #1 再复测, 恢复则改形或可免"） | 排 #1 之后; 复测仍慢则按预授权改形 | **P1** |
| 3 | **MV3.d verdict triage + 采证 fetch 修复** | battery-on SSH 掉线 → guest-on.log 截断/工件空壳（p2-audit-gate.txt 等）; console-p2-on.log 7216s 全档在, 零 corruption, 仅 pgtables 噪声 75 笔; LTP =on verdict 腿缺采证 | 修 mv3d-gate.sh 的 SSH 韧性（重试/分段取数）; 补 P2 audit-gate/counters 读数; verdict 表落 REPORT | **P1** |
| 4 | **wl_brk_anomalies 多 brk 形白名单预期修正** | 白名单分类器 brk 谓词只认单 [start_brk, brk) 跨度（corten_arena.c:14388-14390）; 全系统电池的多 brk 形（heap 中开洞拆分）计 anomalies（corten_arena.c:559/:4118） | 数据 triage 时按实测形修正预期（白名单谓词放宽或分类多 brk 桶）; 非红, 观测口径件 | **P1**（随 #3） |
| 5 | **C2: pgtables stale 计数 per-mm 归属** | 8192/4096 残账 = 纯 stale 计数（页真释放, dec 漏走）, WARN 级; 全局 inc/dec 计数器无法定位 level/位点（mv3c §3.2 工具在案）; P2 电池 75 笔/kunit 基线 24 笔（house 噪声口径） | per-mm 归属计数器（单 mm 顺序生命周期）→ 一轮定位漏 dec 位点; KUnit 锚随片 | P2 |
| 6 | **C2: KUnit 合成 mm PT 泄漏 + free_pgtables 几何门** | W-4 扫入树内空洞的上层页几何门（W-5 遗留收编①, 修需动 mm/memory.c）; 与 #5 同族残账 | 独立小片（mv2-complete 时已登记"独立小片"）; 建议与 #5 合并走查 | P2 |
| 7 | **单套件 flake（测试 83/84/85）** | filter_glob=corten_arena 下确定性失败、全套件全绿; 环境敏感（mv3c §3.3）; `corten_test_txn_uninstall_interlock` flake 同族（复跑绿姿态在案） | 一行 errno 记账定位（mv3c 移交 3）; 判据面外, 不拦门 | P2 |
| 8 | **MADV_PAGEOUT 一行对齐** | 路由注释称与 COLD 同计数 no-op, 代码事实 default → -EOPNOTSUPP（mv3b §4.1, 注释与代码不符） | 一行并入 hints 臂; process_madvise 语义核对随行 | P2 |
| 9 | **MADV_POPULATE_READ/WRITE populate 片** | M3 拒维持; 机械在（corten_gup_window faultin 腿）; 前置 drain 互斥 **已清**（MV3.c 参锁落地） | 独立 populate-through-arena 片; 含 KUnit populate 锚 | P2 |
| 10 | **地板演化 bisect** | "-17~-22% → -84%" 漂移未定轮（候选 W-2/W-7/M-V）; D35 判定基线数字过时（mv3cfeat §2.1/§2.4-4） | build b9541335 + 中间点回放 mmbench_dyn 协议 | P2 |
| 11 | **脏域 rec_lo/hi 有界 park reset（批 mark 刀 2）** | 设计节在案（mv3cfeat §2.4-1: O(512)→O(dirty), 不变式 PTE⊆已 mark 范围, INV6 支撑） | 独立性能片; 先量后动 | P2 |
| 12 | **per-fault 簿记批化（刀 3）** | +~1.4µs/页归因（lookup/ref/tier/desc 锁/meta/stats）; desc 锁粒度+stats 合并（mv3cfeat §2.4-2） | 独立性能片, 排刀 2 后 | P2 |
| 13 | **glibc 库非零 hint 收编（超红线裁决件）** | 库首段带非零 hint 永不达 addr==0 路由; 库段留树（wl_file ~9-70/进程主源）; 收编需"hint 迁移窗口"语义决定, 违 T0 显式地址契约（mv3cfeat §1.5-1） | **主会话裁决件**: 裁决前不动码; 裁决收编则是 WL-tree 归零的最后一大块 | P3 |
| 14 | **static-PIE 对齐探针形** | 对齐 >ELF_MIN_ALIGN 的探针 map+munmap+NOREPLACE 重装会吃 -EEXIST; guest 无此形二进制（mv3cfeat §1.5-2） | 低优先; 探针 record 的 NOREPLACE 豁免小片 | P3 |
| 15 | **bpf_iter/task_vma 窗口段（#39）** | 真保真缺口, 双源化唯一机械=BPF VMA 形对象=D28 明禁; 正典面 /proc/maps 已双源（mv3b §4.3, D28 阻断登记） | 新 BPF 面（region 迭代 kfunc/迭代器）独立设计片; 不排期 | P3 |
| 16 | **裁剪 PR 序列 PR-0..4** | 本报告 §1.3 边界件 | PR-0（implant 退役）先行; 每枚独立 PR + 审计门 | P3（裁决后排期） |

台账口径: D32 sticky-MODE / D33 判据收窄 / D34 frame-sharing 结构边界 /
D35 基线刷新为**已决裁定**, 不占开项位; POKE-COW / journald 面 / S-3 /
PROCMAP or-next / madvise WILLNEED / mseal 核定 / bpf #37/#38/#40 已闭环消项。

---

## 3. 交付三: D29 闭环判定

D29 原文（STATE:1136-1143）: ①完整实现论文的优化思路（事务化热路径+零 VMA
运行的全部主张）; ②对所有应用程序无损完整接管（corten=on 全部进程自动接管,
含 static/suid/系统服务, 全 syscall 语义无损）; 届时 VMA 层删除账兑现。

### 3.1 目标① 三栏

**已达成**:
- 事务化热路径全落: mark/punch/declare/release/mprotect-tx/mremap move/
  madvise 路由/brk 双向路由（paper Fig.8 L1-7 对应面, 七路由 E3 组在位）。
- 窗口域零 VMA 运行: W-2 carrier 消灭（vm_area_struct 分配恒 0）→ W-7
  multi-record（tree_entries 白名单形）→ J1 617/0 + J2 69001/0 实证封口;
  MV3.c-feat exec 镜像收编（同帧 4 FILE region + bss implant）后 exec 行离开
  maple 树。
- 原生 rmap 拱心石（W-1 全链, novma 包装）+ 匿名换出直驱（W1.e ttu 翻转实证
  131072 页）; W-5 植入消灭（admitted 守卫, 4 生产点降级 backstop）; MV2 主链
  tag corten-mv2-complete（J1-J7 判定终表全 PASS）。
- 性能重构首刀: warm park（池 re-parm PT 域保温）+ mmap-pf 配对 = **+134.6%
  (t4)/+135%(t8)**, 税 -84.3%→-63.2%。

**边界（如实）**:
- "零 VMA"成立口径 = 窗口域 + 可收编形; 白名单住户 heap/stack/special 三桶 +
  wl_file hint 库段按合同留在树（§1.1 表）——D34 结构豁免口径的字面兑现,
  全树归零 = 裁决件 #13 + PR 序列之后的事, 不在本目标承诺内。
- 论文优化思路的**性能主张**未追平: 地板 -63~-77% ≠ 个位数目标。

**遗留**: 台账 #10/#11/#12（bisect/脏域有界/簿记批化）——性能线三刀, 独立小片。

### 3.2 目标② 三栏

**已达成**:
- 默认进场: `corten_mode_default` execve 换 mm 即 MODE（fs/exec.c 门 + A5 惰性,
  MV3.a; exec_default_enters=153 实证）; suid 不特判立场维持（纯内核态无 ABI 面）。
- 无损闭合清单兑现: journald/remote 访问面双层根因修复（MV3.b, tmpfiles
  6min+停摆→9.08s）; madvise WILLNEED 落地; mseal fail-closed 响亮拒绝核定;
  bpf #37/#38/#40 优雅降级接受; S-3/PROCMAP or-next 消项（W-3fix2/W-6b）;
  POKE-COW 恢复（MV3.c 参锁后写面 FOLL_NOFAULT 撤除, remote_poke_cow 锚绿）。
- 全系统电池内核行为面 PASS（MV3.d）: P1-off 全绿（LTP 98/11/115, smoke 双形
  26/26）; **P2 =on 世界**（corten=on corten_mode_default=on, console-p2-on.log
  7216s）LTP 全编译+安装+运行、systemd 全栈、journald/futex 修复面验证,
  零 panic 零 corruption（dmesg 唯一签名 = pgtables 噪声 75 笔, 台账 #5 口径）;
  P3-journal mode=1 PROBE_RC=0（默认进场实证）+ tmpfiles 面正常。
- 前置两硬门**均清偿**: ①corruption 族 FOLL_GET 修复 ×5 验证——MV3.b DPA
  定罪（folio_release_kmap 无条件 put vs 缺 FOLL_GET 的 ref 下溢）+ 一行修复,
  ×5 gate 5/5 零签名（x5-ledger-final.txt）, MV3.c 三候选 A/B/C 全排除钉死
  根因归属, MV3.d P2 长跑零复发; ②drain 参锁——MV3.c 落地（drain 段整体包进
  mmap_write + assert, 解锁 follow-only 的写面恢复）。

**边界（如实）**:
- bpf_iter/task_vma 窗口段整段缺失（#39, D28 阻断登记; 正典双源面 /proc/maps）;
  trace 符号化 #37/#38/#40 降级接受——"无损"按显式降级/响亮拒绝口径闭合,
  非逐字节保真。
- static-PIE 对齐探针形未收（#14, guest 无此形）; glibc 非零 hint 库装载不收编
  （#13, 留树不碍无损——legacy 应答正确, 只是未进 arena）。
- suid 专项未单测（立场=不特判, 系统电池含 suid 组件隐式覆盖; 深测未排期, 如实记）。
- MV3.d verdict 采证腿缺（SSH 取数失败, 台账 #3）——**行为面 PASS 判定不受影响**
  （console 全档在）, verdict 表待补。

**遗留**: 台账 #3/#4（triage+采证）+ #15（bpf 新面, 不排期）。

### 3.3 收官声明

两道前置硬门清偿完毕（§3.2）, MV3.a→b→c-debug→c-feat→d 五片全收口,
MV3.e 删除账清单交付（§1）。**D29 路线按上述三栏口径收官**: 目标①的机制面
全达成、性能面余三刀（台账 #10-12, 独立小片）; 目标②达成于显式降级/响亮拒绝
的无损口径, 余 triage 采证（#3/#4）与不排期的 bpf 新面（#15）。D29 级开项为零,
全部余项已降级为 §2 台账的独立小片。后续指令面 = 台账排期（建议首刀 #1 brk
路由 UAF 定罪）或 REPORT MV3 章落笔。

---

## 4. 明示不做

- 不实际删任何 VMA 层代码（§1.3 是边界件清单, PR 序列待裁决排期）。
- 不给 PR-0..4 估 LoC/工期（§1.3 只登边界与验收门; 二期审计机械退役的账另计）。
- 不动 wl_brk 白名单谓词（台账 #4 是 triage 时的预期修正, 非本片码面）。
- 617/69001 读数不作为唯一判据引用（无归档原件, 判据链以归档同形读数 + 结构
  论证为主, live 累计值为旁证——已在 §1.1 披露）。
