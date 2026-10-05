# MV3.b dev report（无损闭合清单；头项 = journald 外部访问面）

agent: mv3b-dev（CortenMM kernel-MM dev+verify; ponytail full; DoD/验证不打折）。
基线: worktree /home/ppw/linux-6.18-mva @ 2e811260e336（MV3.a 收口态, 主树同步）。
不 commit。工件 results/r07/mv3b/。VM 命名空间 mv3b（PORT 10029, qemu-mv3b.pid,
tmux mv3b-vm, monitor-mv3b.sock）—— 与 mv3a/w7v2/a1 全隔离。

---

## 0. 判定总览

| 项 | 判定 |
|---|---|
| 头项 journald 面（根因修复 + KUnit 锚 + 停摆消除） | **绿**（单元级 + boot 级: tmpfiles-setup-dev-early 9.1s 完成, mm.h:2648 零, 读内容成功） |
| corruption 族（本片开发期引入的 ref 下溢） | **根因定位 + 已修 + 实证消除**（DEBUG_PAGEALLOC 定罪, 修后同工具零事件; 见 §2——如实记录: 产生者 = 本片首版 face 臂, 非 MV3.a 旧账, 初判"MV3.a 竞态"已被证据推翻） |
| =on 带 journal 全启动全绿（MV3.d 前置硬门判据） | **绿**（tmpfiles-setup-dev-early Finished/ExecMainStatus=0, systemd 全启动, journald active + journal 完整 788 行 0 watchdog, dmesg 静默（唯一一行 = C2 容差 pgtables）, smoke 26/26+DRIVER PASS, metis ×2 精确基准; 例外 = arena_stats 读挂起→§3 登记） |
| arena_stats churn 读（task 2） | **定位 + 挂起复现 + 修登记**（§3: M6.T4 walk 本体, 首读即挂, R 态不可杀, sysrq-l 双采样; 与 brk 路由生命周期项同族待定罪） |
| madvise WILLNEED 族（task 3a） | **已实现**（hints 臂 + parked 臂, 路由锚更新, KUnit 绿） |
| mseal 行为核定（task 3b） | **核定完成, 维持 fail-closed 登记** |
| bpf_iter/trace 符号化 #37-40（task 3c） | **核定完成, #37/#38/#40 登记接受, #39 登记 D28 阻断** |
| =y 构建零新警告 / KUnit on×2+off / checkpatch / =n | **绿**（interlock flake 一笔, 复跑绿, mv3a 同款在案） |

**一句话**: 头项两层根因（wrapper 契约 WARN + IS_ERR 臂漏锁）修复并实证; 开发期
face 臂引入的 FOLL_GET 缺失 ref 下溢（corruption 族）被 DPA 定罪、一行修复、同
工具实证消除; =on 带 journal 全启动电池全绿。

---

## 1. 头项: journald 外部访问面（MV3.a 三红①）

### 1.1 根因（trace 结论, 两层）

**第一层（表面, mm.h:2648 WARN）**: `get_user_page_vma_remote()` 的 post-GUP
`vma_lookup(mm, addr)` 在 MODE mm 的窗口域必 miss——GUP remote 本身经 V-C 的
corten_gup_window 臂是**通的**（探针应答、页真实拿到）, 缺口在 wrapper 的
"page ⇒ VMA" 契约: 窗口页没有树 VMA 可找 → WARN_ON_ONCE(!vma) → ERR_PTR
(-EINVAL) → `__access_remote_vm` 的 IS_ERR 窗口臂判短答。brief 里
"GUP-slow 探针在 remote 语境的缺口"的准确答案: **探针没问题, wrapper 契约
被 VMA-free 页违反**。

**第二层（真根因, 停摆机制）**: 该 IS_ERR 窗口臂（memory.c:7033 旧址）
`return buf - old_buf` **没有释放 mmap_read**——corten 自己加的分支漏锁。
后果链: journald 读某客户端 cmdline → 漏掉目标 mm 的读锁 → 目标进程下一个
mmap 写操作冻结（systemd PID1 的 cmdline 被读 → PID1 冻结 → tmpfiles 停摆）
→ journald 对同 mm 的第二次读在 `mmap_read_lock_killable` 自锁 → watchdog
280s 击杀。与 mv3a 报告的"用户态停机流程卡死"观察吻合, 但机制是内核漏锁,
不是用户态流程。

### 1.2 修（mm/memory.c 两处 remote face, 4 文件 diff 主体）

- `__access_remote_vm()` / `__copy_remote_vm_str()` 的循环改 **arm-local 形**:
  每迭代先 `corten_gup_window(mm, addr, gup_flags, &page)`（与 GUP slow 路径
  同一探针/follow 引擎, 同 FOLL 旗标语义）:
  - `< 0`（parked/hole/perm 拒）→ `corten_remote_note_window_short()` 计数
    短答, **先 mmap_read_unlock 再返回**（漏锁修复点）;
  - `== 0`（活跃 region 页, 真实存在）→ 拿页 kmap 拷贝, **内容成功返回**;
    页无 VMA, 不再喂给 `get_user_page_vma_remote()`（WARN 面消除; 两处
    corten 面 不再驱动窗口地址进 wrapper, rmap.c:2916 的 make_device_exclusive
    等遗留 wrapper 调用者契约不动）;
  - `== 1`（非 MODE / 窗外 / implant / tree-anchored）→ 原 legacy 链原样。
  - 旧 IS_ERR 窗口分支（含漏锁点）随可连性消失删除; expand_stack 失败臂保持
    stock 语义（expand_stack 失败自身放锁）。
- **follow-only 硬化（追加）**: 最终构建的两处 arm 调用带 `FOLL_NOFAULT`——
  远端拷贝面只读已提交页, **绝不从读者语境对目标 faultin**。理由见 §2.3:
  arena 的 slot 生产者并非全走该 mm 的 mmap 锁（exit drain 是锁外）, 读者侧
  faultin 没有互斥保护。缺席 slot 保持短答（V-A.3b 契约, 计数）。
  语义边界（如实披露）: 远端 POKE/写 COW-break（ptrace /proc/pid/mem 写
  fork 共享窗口页）在 follow-only 下从"破 COW"退化为 -EFAULT; 读面
  （cmdline/environ/mem 读/pageread）不受影响。写 COW 远端破除需要 drain
  参锁后重评（§2.3 的根治件）。

### 1.3 验证

- **KUnit 锚** `corten_arena_test_remote_access_window`（corten_arena_test.c,
  corten_arena 套件 #83）: MODE mm + attach region, 以 proc 面同形
  （access_remote_vm + FOLL_ANON + 内核缓冲）直驱: 64B 读取回内容
  （DATACK_1 种子, MEMEQ）、short 账零动; arg_end-1 单字节探针读 =1;
  park 后短答 =0 且 short 计数恰 +1; exit 干净。
  **on: 132/0/0 全绿**（基线 131/0/0 + 1 新锚; 一轮 interlock flake 见 §5）;
  off: 27 pass/105 skip（skip 对账精确, 新锚带理由 skip）。
  开发期红→绿如实记录（终判版）: 首版 face 臂缺 FOLL_GET 时, 锚的
  put_page（GUP 强制 GET 的正确配对）+ face 的 folio_release_kmap put 叠加
  把种子页压穿——这正是 §2.0 全族根因的单元级复现; face 补 FOLL_GET 后
  （锚的 put_page 恢复保留）锚绿。
- **checkpatch**（git diff 全量）: 0 ERROR / 0 WARNING / 0 CHECK。
- **guest =on 带 journal boot**（bzImage #328, 头项面首验）:
  `Finished systemd-tmpfiles-setup-dev-early` **9.08s**（mv3a 带journal: 6min+
  停摆）; systemd 全启动 ~16s（nginx/getty/ssh 全到）; mm.h:2648 全 dmesg **0**;
  journald 无 watchdog kill; read() 返回真内容（journald 正常归因）。
  **停摆与 WARN 面: 消除。**

---

## 2. corruption 族: 定罪-修复-实证消除（产生者 = 本片首版 face 臂, 已纠正）

### 2.0 终判（推翻本报告开发期的初判, 如实留痕）

开发期（§2 旧版）曾判"MV3.a 时代 slot 生命周期竞态, 我的 diff 构造性只读"——
**两个结论都错了**, 错因相同: 我论证只读时漏看了 `folio_release_kmap()` 的
无条件 `folio_put()`（include/linux/highmem.h:682, base 树既有）。首版 face 臂
把 `&page` 交给 `corten_gup_window` 时未携带 FOLL_GET: 真 MMU
`__get_user_pages_locked()`（mm/gup.c:1745）对非空 pages 数组**强制 FOLL_GET**
（源码注释自证: "Traditional behavior is to set FOLL_GET if the caller wants
pages[] filled in (but has carelessly failed to specify FOLL_GET)"）, 探针的
try_grab 因此不取引用, 而循环收尾的 folio_release_kmap 照 put——**每次窗口域
cmdline 读都净减一引用**。=on+journal 下 journald 满频读 → 数秒内把活跃 arena
folio 压到 0（PTE 仍映射）→ 提前释放/帧重用/双重释放 → 全部症状。
旧代码（wrapper 走真 GUP）被强制 GET 平衡——控制组干净是真的, 不是运气。

### 2.1 定罪链（DEBUG_PAGEALLOC + PAGE_OWNER 轮, console-mv3b-dpa-unfixed.log）

修 ref 前的 DPA boot（#334, unfixed face）: 9 秒内 12+ 条
`BUG: Bad page state ... nonzero mapcount`, 且 **同 pfn 在 systemd-journal 与
(spawn) 两个上下文成对出现**。关键页 pfn 0x11235b 的完整证据:

```
page: refcount:0 mapcount:1 ...  page dumped because: nonzero mapcount
page last allocated: corten_arena_folio_alloc_novma ← corten_arena_user_fault
page last free pid 190 tgid 190 (spawn)
```

而再次发现它坏状态的正是 journald 自己的释放路径:

```
bad_page ← __free_frozen_pages ← __access_remote_vm+0x25a (内联 folio_put)
           ← proc_pid_cmdline_read ← vfs_read
```

即: arena fault 页被 cmdline 读的面多放一次 → refcount 归零提前释放 → 后续
put 方（(spawn) 的 mm 遣散等）再放 → bad page state; 帧 reuse 后被别的映射者
持有 → mapcount 错位（boot1 的 mapcount -1）→ PCP 毒化/NULL-deref 全族。

### 2.2 修

两处 face 臂的 arm 调用补 `| FOLL_GET`（memory.c, 一行×2 + 契约注释）,
try_grab 取引用与 folio_release_kmap 的 put 平衡。KUnit 锚的种子
put_page 相应恢复（GUP 强制 GET, 该 put 本来就是对的——开发期"锚自_bug"的
判断一并纠正）。

### 2.3 实证消除

同工具（DPA+PAGE_OWNER, #334→#334 重编）修后 boot: **前 60s 零 Bad page/
零 family 事件**（修前同窗 12+ 条）, ssh 正常, H1-H5 门全部可跑（journald
active、journal 完整、mode_probe=1、**systemd/journald/sshd 的 /proc cmdline
读全部返回真实内容**、dmesg 0 WARN/0 BUG/0 mm.h:2648）。

**×5 门槛（corruption-signature gate, 主会话裁定口径）**: 最终内核连续五次
=on+journal 全启动（x5-ledger.txt, console-mv3b-x5-{1..5}.log）:

| boot | corruption 签名 | pgtables 残留 | journald | journal 行 |
|---|---|---|---|---|
| x5-1 | **0** | 1 | active | 774 |
| x5-2 | **0** | 1 | active | 768 |
| x5-3 | **0** | 1 | active | 772 |
| x5-4 | **0** | 0 | active | 747 |
| x5-5 | **0** | 0 | active | 744 |

5/5 零 corruption 签名（修前 6/6 boot 在 8-70s 必炸）。pgtables 残留递归数据
（供 mv3c task-2 猎踪）: 3/5 boot 出现、恒 8192（2 PT 页）、每 boot 一次、
时点晚（数百秒）——间歇性记账残留, 非 page-integrity; 口径裁定 = corruption
gate 为 ×5 判据, pgtables 按 house 惯例排除但逐 boot 记账。

**锚红面补齐（§1.3 锚升级）**: remote_access_window 的两个 FOLL_ANON 读各加
folio_ref_count delta 断言（refs0 播种后取, 读后断言不变）——修前 face 下该
断言红（folio 被压到 0）而内容断言绿, 补齐"锚缺红面"缺口。联合树 KUnit:
134/0/0（含 mv3c 的 2 个新测试全绿）。

### 2.4 修后遗留: brk 路由 PT 生命周期 UAF（DPA 暴露的独立预存项, 登记）

修后 DPA boot 在 86s 一次新 oops（与本族无关, 完整栈在
console-mv3b-dpa-fixed.log）: `corten_brk_grow_route → corten_arena_declare_locked
→ corten_arena_check_empty_locked+0xb6` 在 journalctl 的 brk 增长时对**已被
释放（DPA unmap）的内核页**取指——check_empty 的窗口占用巡查走到一页已退休
的页表页。判: brk/declare 路径的 PT 页生命周期预存竞态（本片未触碰该路径;
无 DPA 时为静默走查, 是否致腐未知）。**=on 无 DPA 世界是否触发见 §5 的最终
boot 读数**（登记为独立核查项: check_empty 的 PT 页引用应在 desc 锁/RCU 下
取; DPA 复测一轮即可定罪）。该 oops 杀了 journalctl, 其 do_exit 在一个 mmap
锁上滞留（170s acct_collect 栈）→ 该 boot 后段 wedge——属 DPA 放大的次生。

### 2.5 boot 台账（开发期, 保留作证据; 判定列以 §2.0 为准）

| boot | 内核 | 形态 | corruption 签名 | 结局 |
|---|---|---|---|---|
| boot1 | #328 | =on+journal | 9.77s try_grab WARN（窗口 stale PTE→pfn 0x10945e, refcount 0）; 9.94s Bad page map（ifupdown exit zap, mapcount **-1**, 同 pfn）; 9.99s Bad page cache "still mapped when deleted"（udev-worker, shmem）; 21.9s `___rmqueue_pcplist` NULL-deref（LIST_POISON2 入 PCP 链）| zone 锁泄漏 → RCU stall → 全机 wedge, ssh 死 |
| boot2 | #328 | =on+journal | 8.60s try_grab WARN; 8.84s NULL-deref（`lru_gen_del_folio` ← `__folio_put` ← `__handle_mm_fault`, cdrom_id 自身窗口 fault 替换出垃圾 folio）| 同族 wedge |
| boot3 | #329（follow-only） | =on+journal | 17.2s try_grab WARN（**读面 follow-only 仍见先期 stale PTE**→只观察不产生）; 21.9/22.4s 双 NULL-deref | wedge |
| masked | #329 | =on+journald-masked | 11.4s try_grab WARN; **16.7s systemd PID1 用户态 segfault**（libsystemd-shared, 地址 0x31——堆腐坏）→ Caught SEGV → Freezing | 停机 |
| masked2 | #329 | =on+journald-masked | 前 14s 干净（tmpfiles 5.9s, ssh 13.6s 起）, redis crash-loop（内存腐坏）, 67s `___rmqueue_pcplist` NULL-deref（dbus-daemon 用户 fault） | 后段 wedge |
| kfence | #330（KFENCE 50ms） | =on+journal | 6.8s try_grab WARN; 13.2s NULL-deref（**nginx, irqs disabled + preempt_count 1 退出**） | wedge; **KFENCE 零报告**（腐坏页未被采样池覆盖） |
| control | **#326（mv3a 原件）** | =on+journald-masked（同镜像同 mask 四件套） | **kernel 侧 corruption 签名 0**; 9.37s tmpfiles 完成; 20.7s 老 WARN 面（mm.h:2648, dbus 读 cmdline——控制组仍在旧脸）; 110s user@0 超时（mild, 无 corruption 签名） | 不 wedge |

### 2.2' （开发期判定过程存档, 已被 §2.0 终判取代——保留证据链价值）

开发期控制组判别（#326 同镜像同 mask 干净 vs 我的三发三中）方向正确, 但
归因（"MV3.a 旧账 + 时序幸运论"）错误; KFENCE 零命中当时已提示"不是页分配
采样可覆盖的形状"（正确信号被误读为采样率问题）。record-UAF 三道守卫核对
（kfree_rcu/tryget_live/i_mmap 锁对）仍为有效排除。

### 2.3 结构发现（一并交付, 仍有效）: exit drain 在 mmap 锁之外

`exit_mmap()`（mm/mmap.c:1553）: `corten_arena_mm_exit(mm)` 在 **任何 mmap 锁
之前**执行（锁在 1557 行才首次出现）。corten_gup_window 的设计注释主张
"生产者跑 mmap_write、arm 跑 mmap_read、同 mm 互斥"——**对 drain 不成立**
（它两把锁都不拿）。任何依赖该互斥的读者侧 mutation（含我首版 face 的
remote faultin、以及将来任何 GUP-faultin 形的窗口写）与 drain 之间没有锁
保护。本片已用 follow-only 回避; 根治件 = **drain 参锁**（或 drain 与读者
的专用互斥）, 登记为独立小片（牵 exit_mmap 时序, 需单独走查）。

### 2.4' （旧"候选产生者"节撤销——A/B/C 三候选均为 ref 下溢的次生症状,
重用帧的无辜持有者; DPA 定罪后不再需要。原候选 A 的 truncate 路由走查撤销;
"still mapped when deleted" 的 shmem 页与 boot1 的 bdev 页同属被下溢页的重用
受害面。）

---

## 3. task 2: arena_stats 读挂起 —— 定位到 walk 本体, 修登记（未实现）

**终态升级**: 不止">60s 变慢"——最终 =on+journal boot（#335）上 **arena_stats
的第一次读（boot 后 ~25s, 无 churn）即挂起**。现场证据（全档
console-mv3b-journal-final335.log）:

- 读者 `cat` 进程 **state=R 不可杀**（kill -9 不达; 持 rcu_read_lock 的
  内核自旋, xa_find 内无调度点）。
- 两次 sysrq-l 采样（相隔 189s）同点位: `corten_mm_state_pages+0x108`
  / `xas_load+0x49`（内联 xa_find）← `corten_arena_stats_report+0x9b6`。
- 反汇编核对: +0x108 = 循环尾 rcu_read_lock/xa_find 调用点; 循环为
  "idx++ + xa_find 逐 present 键"形, 252 arenas/毫米级键数下本应毫秒级——
  不可杀 + 无限自旋指向 **xarray 节点链腐坏**（xa_find 在坏节点链内成环）,
  与 §2.4' 的 brk 路由 PT/节点生命周期 UAF 同族（DPA boot 里同一 brk 路径
  对已释放内核页取指）。
- 复现口径: 该 boot 上 H5 的 arena_stats grep（无 churn）已挂 → **非 churn
  活锁, 首读即挂** → mv3a 的"churn 下 >60s"读数（#326）与本挂起是同一路径
  的两种表现, 产生条件 = =on 世界的 registry/arena 构成 + （疑似）节点腐坏。

**修（登记, 未实现）**: 先清 §2.4' 的 brk/declare 生命周期项（DPA 一轮定罪）,
再复测本读; 若仍慢, 按预授权改形（快照+TTL / 预算分批 walk）。本片不交不可
验证的修——挂起根因在 corten 预存路径的假设尚未定罪, 盲改渲染层是盖被子。

---

## 4. task 3: 闭合清单余项

### 4.1 madvise WILLNEED 族 —— 已实现（从登记升级）

- 路由（corten_arena.c `corten_arena_madvise_route`）: `MADV_WILLNEED` 并入
  pure-hints 臂（in-arena 计数 no-op success——stock 对 anon 映射的 WILLNEED
  恰是 madvise_willneed() 的无 vm_file no-op, 语义精确同形; FILE region 的
  readahead 是 advisory, 丢弃合规）。parked-span 臂同步并入（park 形回 0,
  消 S-5③ 残差行）。
- 原"`WILLNEED stays rejected`"测试断言与 parked-terminal 断言按新契约更新。
- **登记（未实现, 带理由）**: MADV_POPULATE_READ/WRITE 保持 M3 拒（硬 populate
  契约值得独立"populate-through-arena"片——机械在（corten_gup_window 的
  faultin 腿）, 但 §2.3 的 drain 互斥未清前不该加读者侧 faultin 面——与
  family 修复同一前置）。MADV_PAGEOUT（process_madvise）: 路由注释宣称与
  COLD 同为计数 no-op, **代码事实是 default → -EOPNOTSUPP**（注释与代码不符,
  如实记录）; 一行并入 hints 臂即可对齐注释, 本片未动（scope 纪律: brief 点名
  的是 WILLNEED 族）, 留一行 diff 的尾巴件。

### 4.2 mseal 行为核定 —— 核定完成, 维持现状（登记）

mm/mseal.c:170-181: 窗口重叠 range → **-EOPNOTSUPP**（fail-closed, M3B_DESIGN
§5.15 既定: mseal 冻结 VMA 级属性与 arena 事务性空间操作冲突, M3 起有意不
评估）。核定结论: MODE 进程的 mseal 可用域收缩到 whitelist 树件（stack/
special/brk 影子）, 窗口域拒绝是**响亮且语义诚实**的（无静默错行为）——
"无损闭合"由显式拒绝满足。登记理由: 若未来要开, 是"arena 域 mseal 语义设计"
特性片（per-region sealed 位 + mprotect/madvise/munmap 路由的 sealed 检查）,
不是本清单的残差修。

### 4.3 bpf_iter/trace 符号化（#37-40）—— 核定完成, 三个接受一个阻断

- **#37** trace_output.c seq_print_user_ip: 窗口 IP → 裸地址打印（优雅降级,
  无错误路径）。**登记接受**: 补符号需 trace 渲染层嵌 region 查询（rfile/poff
  → file+offset）, 诊断保真收益低, 非无损性缺口。
- **#38** bpf stackmap build-id: 窗口 IP → 回退 IP 模式（优雅降级）。**登记接受**,
  同上理由。
- **#39** bpf_iter/task_vma 迭代: 窗口段**整段缺失**（真保真缺口）。**登记
  D28 阻断**: 双源化的唯一机械是给 BPF 程序铸 VMA 形对象——正是 W-2 载体
  退役 + D28 判据明文禁止的形状。正典双源面是 /proc/maps（corten_row_next
  机械已在产线）。要补齐 = 新 BPF 面（region 迭代 kfunc/迭代器）, 独立设计片。
- **#40** bpf_find_vma kfunc: 窗口 → -ENOENT（kfunc 契约内优雅错误）。**登记接受**。

---

## 5. 验证链状态

| 门 | 结果 |
|---|---|
| =y 构建零新警告（#327→#335 各轮） | 绿（唯一 warning = stock objtool cpuidle_enter_state, 前代已在） |
| KUnit on×2（最终 #335 内核） | 绿: on1 24/0/1 + **132/0/0** + 34/0/5; on2 同绿但 corten 套件一笔 `corten_test_txn_uninstall_interlock` flake（runtime 20.29s 超窗, **mv3a 在案同款"interlock flake 复跑绿"**）→ 复跑 on1 全绿 |
| KUnit off | 绿: 25/0/0 + 27/0/105 + 7/0/32（skip 对账精确, 新锚 skip 理由在案） |
| checkpatch（全 diff） | 0E/0W/0C |
| =on+journal 头门 H1-H5 | 绿（§5 上行）; H6 = task 2 挂起（§3）; H7 由 console 补审: dmesg 唯一一行 = C2 容差 pgtables, 其余静默, mm.h:2648=0, watchdog=0 |
| =n 构建 + 13+ 消费对象 + fs/exec.o 零符号 | 见 §6（构建绿; nm 清点全零） |
| guest =on 带 journal 全启动 | **部分绿**: 启动完成性（tmpfiles 9.1s + systemd 全启动 ~16s）/ 无 2648 / journal 面 OK; **红**: dmesg 静默（§2 family） |
| 裸 smoke 26/26 + metis 同基准 | **=on+journal 形全绿（新 Lands）**: smoke 裸形 26 PASS/0 FAIL + **SMOKE-DRIVER PASS**（mv3a 仅 masked 形达成; arenas after: 246）; metis_eq ×2 checksum **精确同基准 2d383eeed4ceb73b**（RC 0/0）。=off 形: smoke HOOK 形 26/0（驱动器账面 flake 一笔"87→112"披露非红）; metis ×2 同基准 |
| =off 回归 | **绿**: 分离证明 mode=0（PROBE_RC=1 = mode=0 世界预期值, mv3a 同形）; dmesg 静默（WARNING/BUG=0）。 regress-off.log R1-R7 |
| arena_stats 读 | **挂起（task 2, §3）**: =on+journal 首读即挂（R 态不可杀, sysrq-l 双采样定位于 walk; 登记） |

---

## 6. =n 门明细

（构建 log: results/r07/mv3b/build-n-mv3b.log; 验证: n-verify-mv3b.log;
快照: config-pre-n-mv3b.snapshot; 陈代 corten*.o 五件抢救归档: n-stale-objects/）

- 构建零 error; corten*.o 产出（清走代后）= **0**（=n 构建不删旧代, 五件陈代
  对象 mv 归档后复点为零——mv3a 运维教训同款执行）。
- 消费对象清点（nm -u | grep -c corten, **19+1 全零**）: mm/{memory,mmap,
  migrate,rmap,swapfile,gup,oom_kill,mempolicy,mremap,madvise,mprotect}.o,
  arch/x86/mm/fault.o, kernel/sys.o, fs/coredump.o, kernel/futex/{pi,requeue,
  syscalls,waitwake}.o, fs/exec.o。
- mm+fs+fs/proc+kernel+arch/x86/mm 全对象 corten 符号 = **0**。
- =y 配置已即时恢复（/tmp/config-mv3b-pre-kfence = 无 KFENCE 的最终 =y 形）
  并重建归位。

---

## 7. 工件索引（results/r07/mv3b/）

- bzImage-mv3b-y（#329, =y + follow-only face; KUnit 用的同件）
- build-y-mv3b-1.log / build-n-mv3b.log / n-verify-mv3b.log /
  config-pre-n-mv3b.snapshot
- kunit-on1.log / kunit-on2.log / kunit-off.log
- mv3b-full.diff（4 文件: mm/memory.c, mm/corten_arena.c, mm/corten_arena_test.c,
  include/linux/corten_arena.h）
- mv3b-guest-gate.sh / mv3b-kunit.sh（mv3b 命名空间, pidfile 纪律）
- console-mv3b-journal-boot{1,2,3}.log（#328 ×2 + #329, =on+journal, family 全档）
- console-mv3b-masked.log（#329 masked, PID1 segfault 档）
- console-mv3b-masked2.log（#329 masked 重试, redis crash-loop + 67s oops 档）
- console-mv3b-kfence.log（#330 KFENCE 50ms, 零命中档）
- console-mv3b-dpa-unfixed.log（#334 DPA 定罪档: Bad page state 成对事件 + journald 释放栈 `__access_remote_vm+0x25a → __folio_put`）
- console-mv3b-dpa-fixed.log（#334 DPA 修后档: 前 60s 零事件 + 86s brk 路由 oops）
- console-mv3b-journal-final335.log（**最终 #335 =on+journal 全启动档**: tmpfiles Finished / dmesg 静默（pgtables 容差一笔）/ smoke 26 + DRIVER PASS / metis 基准 ×2 / arena_stats 挂起 sysrq 双采样在案）
- console-mv3b-control-mv3akern.log（#326 控制组, 同镜像同 mask）
- n-stale-objects/（=n 陈代五件归档）
- head-journal.log（最终 head 门 H1-H5 日志; H6 挂起如实留档）

## 8. 交裁决清单

1. **§2.4' brk/declare 路由 PT（节点）生命周期 UAF**（预存, 独立切片）: DPA
   boot 定位到 `corten_brk_grow_route → declare_locked → check_empty_locked`
   对已释放内核页取指; 非 DPA 世界疑似同一根因以 arena_stats 首读挂起表现
   （§3, sysrq 双采样）。建议: check_empty 的 PT 页引用收进 desc 锁/RCU;
   DPA 复测一轮定罪; 修后复测 §3 读。
2. **§2.3 drain 参锁**（exit_mmap 时序走查 + 读者侧 mutation 的互斥恢复;
   follow-only 的 POKE-COW 退化在该片复评）。
3. **task 2 渲染层改形**（快照+TTL / 预算分批）——排在 1 之后（若 1 定罪并
   修复后读数恢复, 改形或可免）。
4. MADV_PAGEOUT 一行对齐 + MADV_POPULATE_* populate 片（同 2 前置）。
5. （撤销）开发期 §8-5"C2 加一条 gup_loop_window put_page 隐患"——该 put_page
   是 GUP 强制 GET 的正确配对, 非隐患, 撤回。

## 9. 后记: 并发写与提交溯源（20:0x 时段, 如实留痕）

- **用户提交 87220166a815**（18:46:56, "the MV3.b closure", 4 文件 224/66）=
  本片 **FOLL_GET 修复之前**的 face（含 ref 下溢）——该 commit 不是本片验证态。
  验证态 = commit + 两处 `| FOLL_GET`（memory.c face 臂, 一行×2 + 契约注释）+
  测试文件 put_page 恢复, 即 bzImage #334fix/#335 的源态; 修复 delta 在本报告
  §2.2 与 §1.2 完整可重建, 亦已 staged 于 worktree（听裁决处置）。
- **20:01-20:02** worktree 的 memory.c/corten_arena.c 被外部操作回退至 commit 态
  （本 agent 未执行任何 git 写操作; reflog 无 reset/checkout 记录）→ 20:05 本
  agent 重应用修复。**20:2x 起 corten_arena_test.c 出现非本 agent 的新测试**
  （remote_poke_cow / punch_bare_frame_pte, #339 上双 fail 未分析）——
  worktree 存在并发写者, 本 agent 自此停写 worktree, 冲突交裁决（§8 + 已报 lead）。
- 构建溯源: #335（19:27, 修复态）= §5 全部电池的验证内核; #339 = 并发态构建,
  本 agent 不背书。bzImage 工件为 #339（被后续构建覆盖）——#335 的重建口径 =
  commit 87220166a815 + 本报告 §2.2 修复 delta。
- **合流终态勘误（82a35da6b541 起）**: §1.2 的"远端 POKE 写 COW-break 退化为
  -EFAULT"为 follow-only 中间态描述——mv3c 的 drain 参锁落地后, 写面 arm 的
  FOLL_NOFAULT 已对 FOLL_WRITE 撤除（POKE-COW 恢复, remote_poke_cow 锚绿,
  合流树 82a35da 起即此态）; 读面保持 FOLL_NOFAULT（V-A.3b 契约不变）。本报告
  §1.2 该句按此勘误读。
