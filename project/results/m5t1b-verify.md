# M5.T1b + T2' — 验证报告（r07/m5t1b，夜窗班 2026-09-19）

- worktree: /home/ppw/linux-6.18-m5t1b，分支 m5-t1b
- 基线: **802ff7551bd0**（主树 HEAD = M5.T1a 68697442097e + m4t12 + T1c + perf1）
- 工作树: 5 文件 **+1127/-13**（含 KUnit 与 FOLL_FORCE/UNSHARE 事务化）;
  **未 commit**（review/maintainer 后续）
- 备份: /home/ppw/cortenmm/patches/r07-m5t1b.diff（1316 行, checkpatch 全绿版）
- guest 判据盘: /home/ppw/vm/trixie-m5t1b.img（独立时点盘, 源=trixie.img 拷贝）,
  VM = tmux `m5t1b-vm`, hostfwd 10027, pidfile /home/ppw/vm/qemu-m5t1b.pid,
  未触碰主树 VM(tmux vm/perf 班)与 m5t1a-vm
- 产物: bzimg/r07-m5t1b/{bzImage(普通 #5, sha256=313111b4…3a836e),
  bzImage-lockdep(lockdep 变体 #6)}

---

## 1. 实现清单（对 M5_FORK_SPEC §2.2 T1b 行 + §3.4 T2' 收窄 + STATE D18 F2/F3/F4）

| 项 | 内容 | 位置 |
|---|---|---|
| **F2 修法**（选修法=子侧 pmd 门） | fork_mirror 窗循环在 mark 前加子侧 pmd presence 门: 窗口无子 PT 页（DONTCOPY 片被 dup_mmap 跳过 / WIPEONFORK 片被擦）⇒ 无第二 mapper, 跳过 SHARED mark+子回放——消除"过度 SHARED + 父 PTE 仍可写"的 INV7 漂移形状 | corten_arena.c fork_mirror（门 ~8 行+注释） |
| **INV7 checker 本体** | KUnit 侧 walker: 走注册表全部 arena, 每 2M 窗 pmd 门 + covering 事务 + 逐页 query, 断言 SHARED⇒PTE 存在且不可写; 豁免条目注释明示"perm 带 WRITE 的 SHARED 页允许落 RO（fork wrprotect）, 不变式禁的是 SHARED+可写 PTE"; 用例 inv7_shared_ro 覆盖 fork 双侧/父 COW 后双侧 | corten_arena_test.c inv7_walk + inv7_shared_ro |
| **F3 drain 注入** | KUnit: lookup_get 持在飞事务引用跨冻结窗 → fork_begin drain 10\*HZ 超时 → 断言带漏继续（begin 返 0）+ drain_timeout 计数 +1 + frozen 保持 → straggler put 安全落地 → fork_commit 正常收口（faithful+1、解冻、子侧 SHARED 镜像） | corten_arena_test.c fork_drain_leak |
| **T2' FOLL_FORCE 分派** | 慢门语义: 凡到 handle_mm_fault 慢门且非 FAULT_FLAG_USER 的写 fault = 内核路径 COW 写（用户 fault 走 arch 钩子, 与上游 sanitize_fault_flags 的 is_cow 宽免同理）→ ctx.force; fault_once 把 MAPPED+perm 缺 WRITE 的 ACCERR/COW_COPY 改派 CORTEN_DISP_FORCE_COPY→corten_arena_force_write(): 破页共享（map_count==1 复用清 SHARED / >1 拷贝）但 **永不 mkwrite、永不改 perm**（PS-B2: 进程自己的下一次写必须继续 fault）| corten_arena.c force_write + fault_once 转派 |
| **T2' UNSHARE（OQ-4 裁决落地）** | 慢门把 FAULT_FLAG_UNSHARE 翻译为 ctx.write=true 走 COW 分派（WRITE\|UNSHARE 互斥, sanitize VM_WARN 保证映射全函数）; KUnit unshare_pin 用真 fork 双 mapper 驱动慢门: UNHARE→拷贝分支, 父侧私有可写拷贝/子侧保持共享 RO, mapcount 2→1 | corten_arena.c 慢门 + unshare_pin |
| **OQ-5 裁决（按上游语义）** | **SET PageAnonExclusive**: 实读上游 do_wp_page()——`if (!PageAnonExclusive) SetPageAnonExclusive(page)` 在 wp_page_reuse() 之前, 即上游复用分支必置 exclusive（OQ-5 原文"倾向不设置=与 wp_page_reuse 一致"的前提有误, 以树内代码为准）。arena cow_write/force_write 复用分支同形置位; DMA-pinned（folio_maybe_dma_pinned）→ 让路拷贝分支（对齐 wp_can_reuse_anon_folio 拒绝方向, R-B"多拷贝安全"）。KUnit fork_reuse_exclusive 锚: fork+子退出后 mapcount1+非 exclusive → 写 → 复用+exclusive 置位 | corten_arena.c cow_write 复用分支 + force_write |
| **FORCE 边界（新登记, 诚实披露）** | perm RO + **VMA VM_WRITE**（routed-partial 降级片）的 forced 写: can_follow_write_common 拒 FOLL_FORCE 于 VMA 可写 ⇒ GUP 复跟要求 mkwrite, 而 mkwrite 会无声废止进程自己的 RO 契约; 裁决=响亮失败（ACCERR→SIGSEGV, ptrace 见 EIO）, **绝不 fallback**（legacy wp_page_copy 会背事务 mkwrite）。VMA !VM_WRITE 形状（routed-whole RO/PROT_NONE reserve）正常强制拷贝。KUnit foll_force 双形状覆盖 | fault_once 转派条件 + foll_force 用例 |
| **Fig.8 L26-38 对拍锚** | KUnit fig8_cow: 逐行映射表（L26 COW=meta 位对; L27 write&&COW 双路由; L28-31 map_count==1→复用=清 SHARED+同 folio mkwrite; L33-34 alloc_copied; L36 else=非特权 ACCERR+特权读 RESTORE 移植扩展; L39 超范围注记 FRESH 偏差）, 真链腿引 cow_reuse/cow_copy | corten_fault_test.c fig8_cow |
| **fork_arena_test v2**（bench, 项目侧） | 四阶段: isolation（三形状: 触页+partial-RO 提交 / MAP_FIXED punch 多片 / PROT_NONE reserve 子侧重提交; 父子校验和+子改写不可见）/ roundtrip（1k 页×R）/ mtlock（8 故障线程 + fork 循环, 子侧逐页 magic 归属校验 + debugfs 对账）/ counts（T1c park 对账: munmap→pool_parks+1, fork_begin flush, faithful+1, demotes/timeout 恒 0）| bench/arena-stress/fork_arena_test.c 重写 + fork_battery.sh |
| **mapcount 竞争压测** | cow_collision: 每轮 fork 后双侧各 2 kthread 同时改写全部页（对撞）, 各侧校验和/逐页 tag 零互串, corten+legacy 双臂 | bench/arena-stress/cow_collision.c |
| **新增观测** | CORTEN_ARENA_STAT_FORCE_WRITES（强制写计数, 与 COW_REUSE/COW_COPY 同族）| include/linux/corten_arena.h |

## 2. 验证矩阵

### 2a. =y 全量（普通变体）
- 构建 #1–#5（含 =n 往返后还原重建）**零新增警告**; 全部日志仅上游既有
  objtool cpuidle_enter_state 一条（与 r07 既有 lockdep-build-r06.log 同源）。
- 最终普通 bzImage #5: bzimg/r07-m5t1b/bzImage, sha256=313111b4aed76d8ae43942e50204c8321ae384ac9e1305527b1e3e7acd3a836e。
- lockdep 验证后 config 已还原, 重建 #7（与 #5 同源码同 config, 仅链序不同）,
  工作树静止在普通 =y 状态; guest 全部证据产自 #5。

### 2b. KUnit corten\*（无盘 qemu, filter_glob=corten\*）
| 轮 | 内核 | corten | corten_arena | corten_fault | not ok |
|---|---|---|---|---|---|
| on1/on2 | 中途轮（测试自身两处断言缺陷, 已修——见 §5 诚实登记） | 24/0/1 | 33/5, 35/3 | 26/0/0 | 5, 3 |
| **on3 / on4** | 修复后 | **24/0/1** | **38/0/0** | **26/0/0** | 0 |
| **on5-final** | 最终普通 #5 工件 | **24/0/1** | **38/0/0** | **26/0/0** | 0 |
| **off1** | corten=off | **25/0/0** | **18/0/20s** | **5/0/21s** | 0 |
| **on-lockdep** | lockdep 变体 #6 | **24/0/1** | **38/0/0** | **26/0/0** | 0 |

（off 臂 skip=设计性门控, 较 T1a 基线 +11+1=新增用例的 off-skip; on-lockdep
全绿且 **lockdep splat=0**。新增 9 用例: inv7_shared_ro / fork_f2_gate /
fork_drain_leak（含 10s 真实 drain 超时等待, 用例 ~10s 为 E5 契约设计）/ unshare_pin
/ fork_reuse_exclusive / fig8_cow / foll_force + force 路径融入既有用例断言。）

### 2c. =n 七对象回归
- scripts/config -d CORTEN_MM/-d ARENA/-d KUNIT_TEST → olddefconfig → 八对象
  （mmap/memory/migrate/mempolicy/mremap/madvise/mprotect + kernel/sys）零警告零
  error, nm **corten 符数全 0**; config 快照精确还原。

### 2d. checkpatch --strict
- **0 errors / 0 warnings / 0 checks**（1275 行 checked; 唯一一处
  "spinlock_t definition without comment" CHECK 已补注释后复跑全绿）。
  patches/r07-m5t1b.diff 为全绿版。

### 2e. guest 判据（corten=on, 8 vCPU/4G KVM）
**普通内核（bzImage #5）**:
| 判据 | 结果 |
|---|---|
| fork_arena_test v2 corten 臂 | **全 PASS**: isolation 三形状（checksum 076534f9ba241483=前班 fork_roundtrip 同值, 跨工具内容等价）+ roundtrip 200 轮 + mtlock 200 fork×8 线程+对账 + counts（faithful+1, park flush, demotes/timeouts 平）|
| fork_arena_test v2 legacy 臂 | PASS（isolation+roundtrip 50 轮, 同校验和——跨臂等价）|
| **cow_collision 压测** | **corten+legacy 双臂 PASS 200 轮**（64 页×2+2 kthread 对撞, 零互串）|
| **ksmoke/mm 8 件套**（M3B_DESIGN §7.2: map_fixed_noreplace mremap_test mremap_dontunmap madv_populate cow mkdirty protection_keys gup_longterm gup_test） | **9/9 pass, 0 skip 0 fail**（预编译口径, CORTEN_ON=1; r03 预编译先例）|
| run_mode_smoke | **26/26 PASS** + SMOKE PASS + SMOKE-DRIVER PASS |
| metis_eq MODE 全量（8 线程 text1600 1.6G, STRICT hook） | **rc=0**, checksum **8a8db99075665220**（=T1a 三方同值）, **fork-probe OK**（threads+arenas alive, child exit 42）|
| JThreadBench 2000×3×3 JVM | **3/3 rc=0**, counter=2000×3, **零 ClassFormatError/Exception**, fork-probe OK ×3, median 2084/2261ms |
| dmesg | **零 BUG/WARNING/Oops/lockdep splat** |
| debugfs 对账（boot 累计） | fork_faithful=608→增长与 fork 次数一致, **fork_demotes=0**, fork_skips=0, drain_timeout=0, rearm_failed=0, legacy_drift=0（auto_attach_fail=3=T0b 既有遥测: auto DECLARE 失败计数+legacy 回退, 与 auto_fallbacks=3 对应, 非 drift）|

**lockdep 变体（bzImage-lockdep #6, PROVE_LOCKING 全家族）**:
- 无盘 KUnit 全绿（§2b）, splat=0。
- VM boot "Lock dependency validator" 确认激活 → **spec 字母全形状**:
  - roundtrip **1000 轮** PASS（checksum 076534f9ba241483 稳定）
  - mtlock **1000 fork × 8 持续故障线程** PASS + counts 对账（判据 3 的并发全形状, T1a 移交项兑现）
  - counts/legacy 臂/cow_collision 双臂 PASS
  - ksmoke 8 件套 **9/9 pass**（lockdep 内核上复跑）
  - 全程 dmesg **零 BUG/WARNING/deadlock splat**（含 1000 次 fork + 全回归）
  - 终态对账: **fork_faithful=2202**, fork_demotes=0, fork_skips=0, drain_timeout=0, auto_attach_fail=0

## 3. 语义对拍与裁决登记（供 STATE 回填）

- **OQ-4 关闭**: FAULT_FLAG_UNSHARE → 慢门翻译 ctx.write=true 走 COW 分派。
  调用点核对: 唯一生产者=gup.c faultin_page(unshare=true), 触发面=gup_must_unshare()
  （FOLL_PIN+anon+非 exclusive, follow_pud/pmd/pte 三处 -EMLINK）; WRITE|UNSHARE
  互斥由 sanitize VM_WARN 保证 → 映射全函数。任务书 "check_pin_pages" 核对:
  树内无此符号（论文/上游均无）, 实际生产者以上述 gup 路径为准。
- **OQ-5 关闭（按上游代码裁决）**: 复用分支 **SetPageAnonExclusive**（上游
  do_wp_page 复用路径先置位再 wp_page_reuse; 非置位则后续 GUP PIN 命中
  VM_WARN_ON_ONCE_PAGE(PIN && !exclusive)）; pinned→拷贝分支（wp_can_reuse
  拒绝方向一致）。拷贝分支新 folio 经 folio_add_new_anon_rmap(RMAP_EXCLUSIVE)
  天然 exclusive。
- **INV7 扩展落地**: checker 豁免条目（SHARED 允许 PTE RO）+ F2 子侧 pmd 门
  （review 建议的 pmd 门修法, 未取 checker 豁免条款路线）。
- **DEV-14 白名单不变**: 本片零新增 glue 写点（copy_page_range wrprotect 仍是
  第 1 处）; force_write/cow_write 的 PTE 写全在 desc 写锁事务窗内（INV6 口径不变）。
- **残余登记（非阻塞, 下片候选）**:
  1. F2 门为窗粒度: 同一 2M 窗内页粒度 DONTCOPY 片边界（madvise DONTFORK 页级
     形状）仍可能过度 SHARED 残留（良性自愈, 但该窗 INV7 checker 可见）——
     代码注释已声明, 如需页粒度可在 mark_window 加子 PTE 门（+~10 行）。
  2. FORCE 边界: perm RO + VMA VM_WRITE（routed-partial 降级片）的 FOLL_FORCE
     外部写 = EIO（响亮拒绝）, 与 legacy（wp_page_copy 成功）不同——语义裁决
     为"外部写不得无声废止进程契约", 若要闭环需 routed-partial 同步 VMA 位
     （T0 遗留形状, 规模另评）。
  3. fork_battery.sh 的 ksmoke 步骤需显式 `--kdir /root/ktree`（预编译口径,
     guest 无内核树; 报告内 ksmoke 证据来自手动带参复跑, rc=0）。

## 4. 产物与状态

- diff: patches/r07-m5t1b.diff（5 文件 +1127/-13, 1316 行, checkpatch 0E0W0C）
- bzImage: bzimg/r07-m5t1b/{bzImage, bzImage-lockdep} + .sha256
- 日志: results/r07/m5t1b-*（kunit-on3/on4/on5-final/off1/on1-lockdep,
  build-y/y2/y3/final/n/lockdep-build, checkpatch-m5t1b.txt）; guest 证据
  results/r07/m5t1b-guest/
  （fork-battery.out fat-corten.log fat-legacy.log cow-collision-*.log
  ksmoke-8.{json,log} smoke/metis/jtbc 日志 arena-stats-lockdep-final.txt）
- bench（项目侧, 不计内核 diff）: bench/arena-stress/{fork_arena_test.c 重写,
  cow_collision.c 新增, fork_battery.sh 新增}
- **未 commit**; 未 push; 密码未落盘; 未触碰主树/其它 worktree/主树 VM。
- VM 留运行: tmux `m5t1b-vm`（lockdep 变体内核, hostfwd 10027,
  trixie-m5t1b.img）供 maintainer 复核; 普通 #5 内核证据可由
  bzimg/r07-m5t1b/bzImage 复现（KUnit: corten=on kunit.filter_glob=corten\*;
  guest: /root/fork_battery.sh --prebuilt /root/ks-ship --outdir …）。

## 5. 诚实性登记

1. KUnit on1/on2 轮为本班测试代码自身缺陷（seed_mapped 需先 fill_upper 跟踪窗;
   DECLARE 要求 range==VMA; 复用后 meta flags 期望错写为 WRITABLE——corten_map
   重置 flags; F3 首版持 desc 锁而非 active ref, 不触发 drain 超时）, 修复后
   on3/on4/on5-final/on-lockdep 全绿。on1/on2 日志保留作过程记录。
2. guest 第一轮 boot（无 systemd.mask=sys-kernel-config.mount）落入维护 shell,
   按主树先例补 append 重启后正常; 全部 guest 证据产自正常启动内核。
3. phase_counts 打印 "park+0" 为步 2 重新读数后的差值显示（步 1 的 parks+1 断言
   独立通过）, 打印文案易误读, 功能无影响——下轮顺手修。
4. fork_battery.sh 首版两处 CLI 缺陷（outdir 参数序、PREBUILT 未传递）当场修复;
   修复版已回写 bench 侧并重推 guest。
5. 时延口径: JThreadBench median ~2.1-2.3s 与 metis_eq 34.3s 为无同宿负载单值,
   不作 G5 判定（T4 正式矩阵按 EVAL §2 出数）。

## 6. 判定

**T1b 三项（fork_arena_test v2 / INV7 checker / F3 注入）+ T2' 四项
（FOLL_FORCE/UNSHARE 事务化 / OQ-5 裁决 / mapcount 压测 / KUnit 锚与 Fig.8 对拍）
全部落地且验证矩阵全绿** —— 具备提交合入条件（review/maintainer 班次处理,
本班不 commit）。STATE 回填建议: OQ-4/OQ-5 关闭、F2/F3 关闭（F4=OQ-5 随之关闭）、
残余 §3-1/§3-2 登记为 T2''/下片跟踪项。
