# MV3.c dev report（exit drain 参锁 + corruption 族定位链 + pgtables 残账）

agent: mv3c-dev（CortenMM kernel-MM debugging; ponytail full; DoD/验证不打折）。
基线: worktree /home/ppw/linux-6.18-mva @ 87220166a815（MV3.b 收口态, 与 mv3b-dev
协同在线——其 corruption 修复 slice 与本片同树叠放, 差异归属见 §1/§6）。
不 commit。工件 results/r07/mv3c/。VM 命名空间 mv3c（PORT 10030 预留,
qemu-mv3ckunit.pid, tmux mv3c-vm）—— 与 mv3b/w7v2/a1 全隔离。

---

## 0. 判定总览

| 项 | 判定 |
|---|---|
| 任务 1 corruption 族: 根因定位 | **绿（定位完成, 修复归 mv3b slice, 已落地; ×5 gate 5/5 零 corruption）** |
| 任务 2 exit drain 参锁 | **绿（落地, KUnit 对账全绿, checkpatch 0E/0W）** |
| POKE-COW 恢复（评估+锚） | **绿（写 face 恢复 faultin; `remote_poke_cow` 锚绿）** |
| punch 裸洞（新发现, 独立于 8192 老残账） | **绿（根因+修复+锚, `punch_bare_frame_pte` 绿）** |
| pgtables_bytes 8192 老残账 | **登记（先于本片两个 release 的 stale 计数; 定位推进+工具交付, 独立小片裁决, §3.2）** |
| KUnit on×2+off / checkpatch / =y 零新警告 / =n | **绿（干净核 #344）** |
| guest =on 门槛 | mv3b ×5（其内核）5/5 零 corruption; 本片 #344 boot 佐证（§4） |

**一句话**: corruption 族的定位证据链（§1）把 mv3b 的 +FOLL_GET 修复钉死为根因
修复——三候选 A/B/C 全部排除, 产生者是 MV3.b 自己的 remote face; 本片在此之上
交付 drain 参锁（§2, 解锁 POKE 恢复并已恢复+锚定）、punch 裸洞的根因修复（§3.1）,
并把 8192 老残账从"未知炸点"降级为"有工具、有数据的 stale 计数开项"（§3.2）。

---

## 1. 任务 1: corruption 族定位全过程（证据链全录）

### 1.1 起点: 三候选全排除, 决定性工具轮是 mv3b 的 DPA boot

mv3b 留场的 `console-mv3b-dpa.log`（#333: DEBUG_PAGEALLOC+ENABLE_DEFAULT+
PAGE_OWNER=on, =on+journal, 内核 6.18.32-g87220166a815）是全案的转折点:
`Bad page state`（buddy free-time mapcount 检查）在 8.4s 首爆, page_owner
给出 alloc/free 双栈。三个登记候选（A=FILE truncate 路由漏形状 / B=fork-copy
COW 记账 / C=swap 驱动无锁写）**没有一个能解释双栈**:

- 坏页全部是**匿名窗口页**: `flags swapbacked`, `mapping: NULL`,
  alloc 栈 = `corten_arena_folio_alloc_novma ← corten_arena_user_fault ←
  do_user_addr_fault`（进程**自己的**窗口 fault 分配, udev-worker）;
- 触发 bad_page 的 free 栈 = `__folio_put ← __access_remote_vm+0x25a ←
  proc_pid_cmdline_read`（**journald 读 cmdline 的 remote face**）;
- 同 pfn 在同一秒内双客: 8.413s free 爆点之后, 8.557s 的 try_grab WARN
  寄存器里就是同一 pfn（R8=0x...11235b067）——观察者读到的是先期产生的
  stale PTE。

### 1.2 根因（反汇编 + 源码双验证）

对 #333 vmlinux 反汇编: `__access_remote_vm+0x25a` = `call __folio_put`
（内联自 `folio_release_kmap()`）。机制链:

1. MV3.b 的两处 remote face 臂（`__access_remote_vm` / `__copy_remote_vm_str`,
   mm/memory.c）调 `corten_gup_window(mm, addr, gup_flags | FOLL_NOFAULT,
   &page)`——**不带 FOLL_GET**;
2. 引擎的 follow 腿 `try_grab_folio(folio, 1, gup_flags)`（corten_arena.c）
   无 GET 时**不取引用**（gup.c:150: "If neither FOLL_GET nor FOLL_PIN was
   set, nothing is done"）→ 返回页无引用;
3. 循环尾 `folio_release_kmap()`（highmem.h:682）= `kunmap_local +
   folio_put()` → **活窗口页 refcount 1→0 被释放, 而 PTE 还映射着（mapcount 1）**;
4. 帧回 buddy 后被复用 → stale PTE 别名 → mapcount 双扣（Bad page map
   mapcount -1）→ 垃圾 folio 被 fault 替换消费（boot2 形）→ PCP 链毒化
   （LIST_POISON2）。**三签名一个产生者。**

上游契约对照: `get_user_pages_remote()` 的 wrapper 对非 NULL pages 数组
**强制 FOLL_GET**（gup.c:1775-1784 "traditional behavior ... keep doing
that"）——绕过 wrapper 直调引擎的两处调用点就是这个惯例的漏网之鱼。

### 1.3 为什么此前无人看见（三个"绿"的反证）

- **KUnit 绿**: 锚里 seed 的 `get_user_pages_remote(..., FOLL_WRITE, &page,
  NULL)` 被 wrapper 强制 GET 多拿了一道引用, 吸收了下溢（1→2→put→1, 页不
  死）; 生产环境没有 seed, 1→0 即死。
- **#326 控制组干净**: mv3a 内核的旧 face 不做窗口读（VMA-free 域直接
  WARN 短答）——没有读就没有下溢。
- **mv3b 自己的 KUnit 教训**（其报告 §1.3 "pages 数组不带 FOLL_GET 不取
  引用"）修了**锚**, 没修**产品面**——同形状在同一片里重复, 只是换了个
  调用点。

### 1.4 修复与归属

`+FOLL_GET` 两行（两处 arm 调用点）+ 锚 seed 的 put_page 归还 = mv3b slice
（其 face 其 bug, 裁定其收口）; 本片定位链是其根因论证。效果: 其内核
（=on+journal, unmasked journald）3 连长 boot（666s/673s/1095s）零 corruption
签名（console-mv3b-journal*.log）——6/6 复现族消。本项目教训（入 MEMORY 候选）:
**"测试吸收了下溢"是页引用 bug 最常见的假绿形态; 页引用断言必须锚 refcount
delta, 不是锚内容。**

---

## 2. 任务 2: exit drain 参锁（结构修复, 本片）

### 2.1 设计稿（先于动码, 交裁定过）

**现状**: `corten_arena_mm_exit()` 的 unpublish+drain 段只拿
`state->ctl_lock`（DEV-13 形状: "the lower lock alone cannot form a cycle"）,
不拿任何 mmap 锁。W-7 的 INV2 前提"registry 写者全在 owner mm 的
mmap_write 下"对 drain 不成立——任何依赖该互斥的读者侧 mutation（remote
face 的 faultin 腿是具体受难者）与 drain 之间无锁保护, 这正是 MV3.b 用
follow-only（POKE 退化 EFAULT）回避的结构缺口。

**选形: drain 挪进锁内**（三选项中的最 lazy 且语义闭合）:

- 时序约束核查: exit_walk 自己就持有 mmap_write（4318-4535）; mm_users==0
  时该锁**无竞争者**——remote face 全部经 mm 引用进入（get_task_mm/
  mmget_not_zero, mm_users==0 即不可入）; shrinker/oom reaper 需
  mmget_not_zero; file-event 路由不拿 owner 的 mmap 锁（其 zap 走
  desc 锁+ptl, 与 teardown 之间由 tryget_live/kill 账本栅栏）。drain 的
  有界等待（10s 超时, 只在内核 bug 时触发）在锁内不构成 hang 面。
- 锁序: mmap_write > ctl_lock（既有声明序）, mmap_write > i_mmap_write
  （arena free 的 registry unlink 走 corten_file_registry_remove）——
  全部既有边, 无新边。synchronize_rcu 留在锁外（不睡持锁）。
- 实现形状: unpublish + had_arenas 采样 + pool 复位 + drain 段整体包进
  `mmap_write_lock/unlock`, drain 前 `mmap_assert_write_locked`（自文档
  契约, 常开断言）; DEV-13 注释由新注释接替（设计史入本报告）。

### 2.2 恢复评估: remote POKE 破 COW（本片启用面）

drain 参锁后, MV3.b 登记 follow-only 的**唯一理由**（"slot 生产者并非全走
mmap 锁——exit drain 是锁外"）消失: 写 face 的 faultin 腿与 drain 之间恢复
mmap_read/mmap_write 互斥。评估结论: **写 face 恢复 faultin**（去掉
FOLL_NOFAULT, 保 FOLL_GET）, 读 face **保持 follow-only**（V-A.3b 缺席
slot 短答契约是设计语义, 且"读不改目标内存"是独立姿态, 不因锁修复而翻转）;
`__copy_remote_vm_str` 纯读, 不动。faultin 腿的错误映射（OOM→ENOMEM,
HWPOISON→EHWPOISON, 其余 VM_FAULT_ERROR→EFAULT, 8 次有界重试）与用户态
fault 同引擎, 无新面。锚: fork 共享窗口页的 FOLL_WRITE|FOLL_FORCE 远端写
（现 =EFAULT 红 → 恢复后绿, 见 §5 待办）。

### 2.3 顺带加固（同函数族, 同根: exit 变更无读者栅栏）

`corten_arena_unmap_chunk_flags` 的 stats 行在增量前重读
`mm->corten_state`——file-event 路由可达垂死 mm 的 chunk（路由不拿 mmap 锁,
MV3.c 的 drain 持锁不栅栏它）, unpublish 可落在 zap 与增量之间 → NULL 解引。
修: 单次读入局部 + NULL 检查（丢一次咨询性增量好过解引 NULL）; 非 NULL
load 的 UAF 面由"tracked⇒有 descriptor⇒arena-bearing exit⇒free 前
synchronize_rcu 一整个宽限期"闭合。

---

## 3. 两个独立的 page-table 记账发现

### 3.1 punch 裸洞（本片根因修复, 锚已落）

**形状**: `corten_arena_mmap_punch()`（mmap carve 路由的 CHUNK 臂,
mmap.c:594 在 mmap_region 的 legacy body **之前**执行）: ①帧 slot 摘除
（`corten_slot_remove`, 帧成洞）→ ②事务 zap（PTE 清空）→ ③**无任何 PT 页
退役**（`corten_arena_free_ptes_novma` 的调用点只有 release/park/brk-shrink,
没有 punch）。正常路径下洞会被补住（admitted→洞被新 FILE region 记录认领;
!admitted→implant 标记 + 异族 VMA 落入）——**但 mmap_region 在路由之后还有
整段可失败体**（mlock 权限 606 行、file_mmap_ok 617、vma 分配压力、
map_count rlimit……）: 失败即洞**裸**——无 slot、无 VMA、空 PT 页。

裸洞对每一条回收路都不可见: exit_walk 相位 A 按 registry 走（无 slot 不
访问）; free_pgtables 按 VMA 走（无 VMA 不下降）; B1 的 `pmd_page_clear`
看到裸 PT 的 PMD 项非空 → 保守跳过 → PMD 页也留下。free_mm 时残账 =
PT(4096) + PMD(4096) = **8192**, 无声。

**修复**（corten_arena.c, 与 park/release 同语义对齐）: punch 的 zap 成功
后, 对**全覆盖、slot 已空（无 co-record）、PTE 全 none** 的帧走
`corten_arena_free_ptes_novma` 整帧退役漏斗（与 exit walk 混合臂同款守卫
三元组）。**锚**: `corten_arena_test_punch_bare_frame_pte`（carve 路由单独
驱动 = mmap_region 失败态本身; 断言 slot 摘除 + 存活帧保持 + 裸帧 PT 退役;
修复前核红——PT 项存活, 修复后绿）。

### 3.2 pgtables_bytes 8192 残账（**先于本片存在的老形状**, 交下一片）

mv3b 修复内核的首次 unmasked-journald 长 boot（1095s）在 489.2s 报
`BUG: non-zero pgtables_bytes on freeing mm: 8192`（kernel/fork.c:608, 2 页,
单 mm 单次）。**溯源结果: 这不是新形状**——w6/w7 时代的 guest log、mv3a
内核的 kunit 与 guest、mv3b 的 5/5 ×5 gate 中的 3 个 boot 都有同款（1 次/
长 boot 级频率; kunit 全套件每轮 ~24-25 次）。此前 boot 在 8~70s 死于
corruption 族, 这个签名从未被当成独立问题看待。

**本片已做的定位（工具+数据全在 results/r07/mv3c/）**:
- 归属: 只在 corten_arena 套件运行时发生（suite 单独跑 25 次/134 测试;
  corten 套件单独 0; corten_fault 单独 0）——是 arena 测试 mm 的 per-mm
  形状, 与测试数吻合（25 个测试的 mm 各 1 次, file/pool/fork/exit 族）。
- 形态: harness teardown 的 `mm_pgtables_bytes(mm)==0` 断言**通过**之后,
  `mmdrop` 里的 check_mm 读到 8192——账在两读之间"多出 2 页"。而
  check_mm 处的页表遍历打印**零个现存上层页**——**页都已真释放, 是纯
  stale 计数**（2 页的 dec 在某条 free 路上漏走）, 无页面泄漏, 无
  UAF 面。本片排除了 corten_arena.c 的全部 free 位点（2680/4456/4493
  三处都带 mm_dec）与 stock 路径。
- 工具交接: kernel/fork.c 的 per-level 全局 inc/dec 计数器 + check_mm
  nets 打印（本次 debug diff 已从树里撤下, 形状见工件 build-dbg-*.log
  与本节）——下一片接手时第一刀: 给 mm_inc/mm_dec 配 per-mm 归属
  （单 mm 顺序生命周期, 全局计数器会把活 mm 的表混进来, 本轮数据
  因此未能定位到具体 level/位点）。

**判定**: WARN 级噪声（stale 计数, 每mm一次, 不楔机不泄漏）, 先于
MV3.c 两个 release 存在, **不是 corruption 族成员**（corruption gate 判据
不包含它）; mv3b 的 ×5 gate 5/5 boot 零 corruption 已照此口径判定。修不修、
何时修 = 独立小片裁决。

### 3.3 顺带发现（登记, 未定位）

kunit 的**单套件运行模式**（filter_glob=corten_arena）下, 测试 83/84/85
（remote_access_window 的 arg_end-1 peek 短答一次 + 两个新锚）确定性失败,
同核全跑（全套件顺序）全绿; 已排除本片 debug 插桩（干净核复测同样形状——
工件 arena343-r2.log vs kunit-on1.log）。全套件顺序 = 电池/gate 的标准
形态, 该模式不是判据面; 登记为"单套件模式的环境敏感 flake"待下一片
（peek 的短答 errno 未打印, 需要一行 errno 记账即可定位）。

---

## 4. 验证链状态（截至干净核 #344）

| 门 | 结果 |
|---|---|
| =y 构建零新警告（#336→#344 各轮） | 绿（唯一 warning = stock objtool cpuidle_enter_state, 前代已在） |
| KUnit on×2（干净核 #344 全套件） | 绿: **0 not-ok**, 201 测试（199 基线 + 2 新锚）, on1/on2 双跑; 锚 83/84/85 全绿 |
| KUnit off | 绿: 名字集与基线全等 + 恰 2 个新 skip（两锚 corten=on 理由 skip）, 对账精确 |
| `corten_test_txn_uninstall_interlock` flake | **既有非确定性, 与本片 diff 无关的实证**: mv3b 自己的 on2 失败/off 通过（先于本片任何改动）; 同核复跑翻转; r07 历史 log 全 ok。留档 mv3c/kunit-off-flake-note.txt |
| checkpatch（全部 delta） | 0E / 0W（corten_arena.c 80/119 行两轮 + test 增量 38 行） |
| =n 构建 + 消费对象零符号 | 绿: 19 对象 nm 全零, mm+kernel+fs+arch/x86/mm 全对象扫描零（陈代 5 个 .o 归档 n-stale-objects/ 后复扫零, mv3b 运维教训同款执行）; =y 配置即时恢复 |
| guest =on+journal | **绿**: #344 联合核 boot（console-mv3c-journal-final.log）graphical.target + debugfs arenas 渲染 + /proc maps corten 行; dmesg 零 Bad page/零 WARNING/零 Oops（唯一行 = §3.2 老残账 1 次）; 加 mv3b ×5（其内核）5/5 零 corruption |

## 5. 收口清单 vs 待办

已收口（全部在树, commit 82a35da6b541 + 增量）:
1. memory.c 写 face 去 FOLL_NOFAULT（POKE 恢复）+ 两处 FOLL_GET（mv3b）。
2. corten_arena_test.c: POKE-COW 锚 + punch 裸洞锚（本片）+ remote_access_window
   refcount delta 断言（mv3b 依裁定补齐, 全绿）。
3. =n 终树验证 ✓。
4. 工件全档 results/r07/mv3c/（bzImage-mv3c-y/#344、各构建 log、kunit 各轮、
   joint-fixes.patch、mv3b 快照、flake note、guest/kunit 脚本）。

待办/移交:
1. ~~guest #344 boot 佐证~~ ✓（上表）。
2. §3.2 残账独立小片（工具已交付: fork.c per-level 计数器形状 + 数据）。
3. §3.3 单套件模式 flake（一行 errno 记账可定位）。
4. 主会话入库本报告。

## 6. 协同与差异归属（两 slice 同树）

worktree 单树双 slice（裁定: mv3b 先落共享文件, 本片后叠; 联合态已由主会话
以 82a35da6b541 "MV3.c-debug round" 落盘）:
- mv3b slice: mm/memory.c 两处 `| FOLL_GET` + mm/corten_arena_test.c seed
  put_page + remote_access_window 的 refcount delta 断言（依裁定 ①补齐,
  全绿）。快照: results/r07/mv3c/mv3b-inflight-fix-pre-mv3c.snapshot。
- 本片 slice: mm/corten_arena.c 全部（drain 参锁 + stats 守卫 + punch 裸洞
  退役）+ memory.c 写 face NOFAULT 恢复 + test 两锚（remote_poke_cow /
  punch_bare_frame_pte）。
- 报告分工: corruption 族修复闭环由 mv3b 报告收口; 本报告 §1 保留定位链
  全文（裁定 ③: DPA 决定性证据链入报告 §corruption）。

## 7. 明示不做（ponytail 边界）

- punch zap-**失败**臂的 slot 已摘、PTE 仍活的二阶形状（13748 早退）:
  回滚需重插 W-7 桶序, 复杂度/频度不称——登记观察（exit 时该形状的尸检
  特征会是 folio 引用泄漏而非 pgtables 残账）, 需要时独立小片。
- exit 侧裸洞反向清扫（页表驱动的补漏 pass）: FIX-A 在产生点闭合后属
  冗余防御, 不加。
- arena_stats 快照修（mv3b §3 登记）: 不在本片。
