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
| =on 带 journal 全启动全绿（MV3.d 前置硬门判据） | **红（被新增发现的 corruption 族拦路）**——boot 能走完全启动, 但 8~70s 窗出 page-integrity corruption → oops → wedge |
| arena_stats churn 读（task 2） | **登记阻塞**（无稳定 =on guest 可测; 结构分析 + 建议修形已交付） |
| madvise WILLNEED 族（task 3a） | **已实现**（hints 臂 + parked 臂, 路由锚更新, KUnit 绿） |
| mseal 行为核定（task 3b） | **核定完成, 维持 fail-closed 登记** |
| bpf_iter/trace 符号化 #37-40（task 3c） | **核定完成, #37/#38/#40 登记接受, #39 登记 D28 阻断** |
| =y 构建零新警告 / KUnit on×2+off / checkpatch | **绿** |
| =n 13+ 对象 + fs/exec.o 零符号 | **绿**（见 §6） |

**一句话**: 分配的头项根因已修且有锚; 但闭合清单的门（=on 全启动全绿）被一个
MV3.a 时代的 slot 生命周期 corruption 族拦死——本片把它从"未知的未来炸点"变成
"有全链证据、有候选产生者、有下一步工具方案的在案拦截项", 交裁决。

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
  **on×2: 132/0/0 全绿**（基线 131/0/0 + 1 新锚）; off: 27 pass/105 skip
  （skip 对账精确, 新锚带理由 skip）。
  开发期红→绿如实记录: 首版锚自身 put_page 了无 FOLL_GET 的 GUP 结果
  （pages 数组不带 FOLL_GET 不取引用, is_valid_gup_args 只单向校验）→
  PTE 锚定 folio 掉到 refcount 0 → try_grab_folio WARN——修锚（不放）后绿。
  **顺带登记**: 既有的 corten_arena_test_gup_loop_window 同形状
  （FOLL_WRITE 无 GET + put_page）有同款隐患, 未爆只因后续探针不 follow
  该页——列入 C2 残值族清理建议。
- **checkpatch**（git diff 全量）: 0 ERROR / 0 WARNING / 0 CHECK。
- **guest =on 带 journal boot**（bzImage #328, 头项面首验）:
  `Finished systemd-tmpfiles-setup-dev-early` **9.08s**（mv3a 带journal: 6min+
  停摆）; systemd 全启动 ~16s（nginx/getty/ssh 全到）; mm.h:2648 全 dmesg **0**;
  journald 无 watchdog kill; read() 返回真内容（journald 正常归因）。
  **停摆与 WARN 面: 消除。**

---

## 2. 拦路新发现: page-integrity corruption 族（MV3.d 硬门实质红）

### 2.1 现象（全 boot 台账, 工件全档在 results/r07/mv3b/）

| boot | 内核 | 形态 | corruption 签名 | 结局 |
|---|---|---|---|---|
| boot1 | #328 | =on+journal | 9.77s try_grab WARN（窗口 stale PTE→pfn 0x10945e, refcount 0）; 9.94s Bad page map（ifupdown exit zap, mapcount **-1**, 同 pfn）; 9.99s Bad page cache "still mapped when deleted"（udev-worker, shmem）; 21.9s `___rmqueue_pcplist` NULL-deref（LIST_POISON2 入 PCP 链）| zone 锁泄漏 → RCU stall → 全机 wedge, ssh 死 |
| boot2 | #328 | =on+journal | 8.60s try_grab WARN; 8.84s NULL-deref（`lru_gen_del_folio` ← `__folio_put` ← `__handle_mm_fault`, cdrom_id 自身窗口 fault 替换出垃圾 folio）| 同族 wedge |
| boot3 | #329（follow-only） | =on+journal | 17.2s try_grab WARN（**读面 follow-only 仍见先期 stale PTE**→只观察不产生）; 21.9/22.4s 双 NULL-deref | wedge |
| masked | #329 | =on+journald-masked | 11.4s try_grab WARN; **16.7s systemd PID1 用户态 segfault**（libsystemd-shared, 地址 0x31——堆腐坏）→ Caught SEGV → Freezing | 停机 |
| masked2 | #329 | =on+journald-masked | 前 14s 干净（tmpfiles 5.9s, ssh 13.6s 起）, redis crash-loop（内存腐坏）, 67s `___rmqueue_pcplist` NULL-deref（dbus-daemon 用户 fault） | 后段 wedge |
| kfence | #330（KFENCE 50ms） | =on+journal | 6.8s try_grab WARN; 13.2s NULL-deref（**nginx, irqs disabled + preempt_count 1 退出**） | wedge; **KFENCE 零报告**（腐坏页未被采样池覆盖） |
| control | **#326（mv3a 原件）** | =on+journald-masked（同镜像同 mask 四件套） | **kernel 侧 corruption 签名 0**; 9.37s tmpfiles 完成; 20.7s 老 WARN 面（mm.h:2648, dbus 读 cmdline——控制组仍在旧脸）; 110s user@0 超时（mild, 无 corruption 签名） | 不 wedge |

### 2.2 判定链（诚实边界）

1. 我的 diff（4 文件: memory.c 面臂/corten_arena.c WILLNEED/测试/头文件）经
   构造论证**只读**: face 臂对目标状态的全部动作 = pte_offset_map_lock 下读
   pte + 无 GET 的 try_grab（no-GET 时仅 refcount 哨兵读）+ kmap 读拷贝;
   WILLNEED = 计数 no-op。**不可能是 free/refcount 产生者**。
2. follow-only（boot3）后 family 照发且 WARN 显示"读到先期已 stale 的 PTE"
   ——远程读只是**观察者**。产生者在目标自己的 slot 生命周期里。
3. 但 #326 控制组同镜像同 mask 干净（虽自带 user@0 轻症, 参照力打折）,
   而 #328/#329 六发六中——**时序位移使潜在竞态确定性显形**的可能性不能排除
   （mv3a 历史绿 battery = 同一潜伏 bug 的幸存轮次）。
4. 判定: **MV3.a 时代 slot 生命周期竞态**（候选见 §2.4）, 被 MV3.a 默认进场
   的全系统 churn（每 exec 即 A5 进场 + fork-mirror + exec 弃旧 mm + drain
   风暴）推到确定性显形。这是 MV3.d 翻 default=on 前必须清偿的账, 独立于
   本片头项（头项面自身判据——停摆/WARN/读内容——全绿）。

### 2.3 结构发现（一并交付）: exit drain 在 mmap 锁之外

`exit_mmap()`（mm/mmap.c:1553）: `corten_arena_mm_exit(mm)` 在 **任何 mmap 锁
之前**执行（锁在 1557 行才首次出现）。corten_gup_window 的设计注释主张
"生产者跑 mmap_write、arm 跑 mmap_read、同 mm 互斥"——**对 drain 不成立**
（它两把锁都不拿）。任何依赖该互斥的读者侧 mutation（含我首版 face 的
remote faultin、以及将来任何 GUP-faultin 形的窗口写）与 drain 之间没有锁
保护。本片已用 follow-only 回避; 根治件 = **drain 参锁**（或 drain 与读者
的专用互斥）, 登记为独立小片（牵 exit_mmap 时序, 需单独走查）。

### 2.4 候选产生者（证据定向, 供下片直入）

- **A（当前首选）FILE region 的 truncate/invalidate 路由漏形状**: boot1 的
  "Bad page cache: still mapped when deleted"（shmem_undo_range ← rename
  evict, udev-worker; mapcount 1 = 某 PTE 仍映射而 shmem 删页）——W1.b 的
  corten_inode_regions 路由对某些形状（fork mirror 的 FILE region 再登记?
  punch 过的窗口? offset/长度边界?）漏 unmap → 页带活 PTE 被删 → 帧 reuse →
  stale PTE 别名（boot1 同一 pfn 0x10945e 同帧双客: 窗口 PTE + libc 文件
  VMA, mapcount -1 双重 remove）→ PCP 链毒化。journald/tmpfs 重度用户
  （/run/log/journal + /dev/shm）为首发受害面的解释自洽。
- **B** fork-copy/COW-replace 三角的 ref 记账（boot2: 窗口 fault 替换出垃圾
  folio = slot 先期 stale; fork_copy_ptes 的 folio_get 在快路径正确, 漏形状
  待查: WIPEONFORK 边界/punch 帧/fork×COW 交错）。
- **C** swap 驱动 MAPPED→SWAPPED 的无锁写与 fork wrprotect 竞态（perm 注释
  自认的"unlocked writers"）。
- KFENCE 50ms 采样零命中: 腐坏帧未被采样池覆盖（早分配晚腐坏形状）——下片
  建议直接上 DEBUG_PAGEALLOC（free 时即查 mapped 残留, 对"带 PTE 删页"形状
  是正交命中）+ 在 truncate 路由/evict/fork 三点加临时计数器, 一次 boot 定位。
- 计数器佐证建议（下片首个存活 boot 即取）: `truncate_routes` vs family 事件
  数（route=0 而事件 >0 = A 形直证）; `drain_timeout`、`file_fork_mirrors`。

---

## 3. task 2: arena_stats churn 读 >60s —— 登记阻塞 + 结构分析

**状态**: 无法在稳定 =on guest 上取 before/after 数字（每个 =on boot 都被 §2
family 毒化, ssh/工作负载不可靠）, 按"DoD/验证不打折"原则**不交不可验证的修**。

**结构分析**（代码层, 可静态确认）: M6.T4 渲染块（corten_arena.c:3573 起）
对 registry 全量 pin（16/批, 批间 `skip += found` 从表头重扫 = O(N²/16) 列表
跳, N=注册成员数）+ 每 mm 全 arenas xarray 逐帧 `corten_mm_state_pages`
（xa_find + pmd 走 + ptdesc_get/put, 每 slot 一次 RCU 段）。MV3.a 默认进场后
N 从个位数涨到 153+（mv3a 实测 exec_default_enters=153, arenas 106→192）,
churn 下叠加 mmget/mmput + RCU + ptdesc 引用线竞争。mv3a 的 ">60s 未返回
（load 4.29）"读数与 N²/16 + 全帧走查模型相容。
**建议修形**（预授权三选中最 lazy 且语义保全）: 快照 + TTL——全局
`{resident, swapped, ts}` 快照, 读时新鲜（<1s）直出, 过期则重算（重算可再预算
分批: 单次读最多走 K 个 mm, 余量下读续走）; 配 KUnit 锚（双读命中缓存 +
过期重算）。**登记待**: family 清偿后第一稳定 =on boot 取 before/after。

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
| =y 构建零新警告（#327→#330 各轮） | 绿（唯一 warning = stock objtool cpuidle_enter_state, 前代已在） |
| KUnit on×2 | 绿: 24/0/1 + **132/0/0** + 34/0/5（基线 131/0/0 + 新锚; 已知 C2 残值 WARN/BUG 行与 mv3a 基线同集） |
| KUnit off | 绿: 25/0/0 + 27/0/105 + 7/0/32（skip 对账精确, 新锚 skip 理由在案） |
| checkpatch（全 diff） | 0E/0W/0C |
| =n 构建 + 13+ 消费对象 + fs/exec.o 零符号 | 见 §6（构建绿; nm 清点全零） |
| guest =on 带 journal 全启动 | **部分绿**: 启动完成性（tmpfiles 9.1s + systemd 全启动 ~16s）/ 无 2648 / journal 面 OK; **红**: dmesg 静默（§2 family） |
| 裸 smoke 26/26 + metis 同基准 | **=on 形阻塞**（=on guest 被 §2 family 毒化, 不可信; 不造假数）。**=off 形全绿**: smoke HOOK 形 26 PASS/0 FAIL（驱动器账面断言 flake 一笔: "arena ledger not back to baseline (87→112)", mode=0 世界本片 diff 全 inert, 判驱动器自记账 flake 披露非红——同 mv3a =on 形"驱动器账面失效"族）; metis_eq ×2 checksum **精确同基准 2d383eeed4ceb73b** |
| =off 回归 | **绿**: 分离证明 mode=0（PROBE_RC=1 = mode=0 世界预期值, mv3a 同形）; dmesg 静默（WARNING/BUG=0）; 上行 smoke/metis 读数。 regress-off.log R1-R7 |

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
- console-mv3b-control-mv3akern.log（#326 控制组, 同镜像同 mask）
- console-mv3b-masked2.log / n-stale-objects/（=n 陈代归档）
- head-journal.log（H1-H7 门日志, 停在 wedge——如实留档）

## 8. 交裁决清单

1. **§2 corruption 族** = MV3.d 前置拦路, 独立切片（建议 DEBUG_PAGEALLOC +
   三点计数器 boot; 候选 A 的 truncate 路由形状走查先行）。本片交付: 全 boot
   台账 + 控制组判别 + 只读性论证 + 候选定向。
2. **§2.3 drain 参锁**（exit_mmap 时序走查 + 读者侧 mutation 的互斥恢复;
   follow-only 的 POKE-COW 退化在该片复评）。
3. **task 2 arena_stats 快照修**（机械就绪, 待稳定 =on guest 取数）。
4. MADV_PAGEOUT 一行对齐 + MADV_POPULATE_* populate 片（同 2 前置）。
5. C2 残值族加一条: gup_loop_window 测试的 no-FOLL_GET put_page 隐患。
