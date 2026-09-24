# r06 rogue-fix — "present-RO PTE" 缺陷族闭环 (2026-09-18 日窗, D14 授权)

- 班次: 09-18 09:50–15:0x CST（自由实验窗, 无门禁）
- 对象: MODE 进程 "RW 提交区装 present-RO PTE → 写 → ACCERR SIGSEGV" 缺陷族
  （登记源: r06 夜 dg2-verify §3-②、t5-r06-report §2/§6、inflight r05 移交 #2）
- 内核: 主树 HEAD=ba77046c78fe 起班 → 修复 commit **e219920b0923**, tag **corten-r06-rogue**
  （bzimg/r06-rogue, sha256=c767ab30…c061b, 构建 #42, 主树与提交后重建等价）
- VM: trixie 8 vCPU/4G KVM, boot `corten=on mitigations=off kunit.enable=0`（收工留运行）
- 结论一句话: **该族 = 两个同根缺陷 —— "routed mprotect 的提交只活在 per-page metadata 里,
  任何把页/VMA 退出 arena 语义的路径都必须把 perm 带走"。①内容 drops（chunk munmap /
  MADV_DONTNEED 路由的 zap）把 metadata 整槽擦成 INVALID+perm=0 → 下次写 fault 的 FRESH 门
  跌回 DECLARE 时的 ar->prot（glibc reserve=PROT_NONE）→ ACCERR; ②fork_demote 把 shadow-VMA
  还原成只带 DECLARE flags 的 plain VMA → 全部 routed 提交对 parent+child 同时蒸发 →
  glibc 退出期 heap 清理写 → ACCERR（即 T5 应用 rc=139 族本尊, 与 OQ-D fork 边界同源）。**

---

## 1. 假设池判定表（任务书 5 条逐一裁决）

| # | 假设 | 判定 | 证据 |
|---|------|------|------|
| 1 | mprotect 路由窗口扫描漏页（pte_offset_map/表遍历漏某级） | **证伪** | 探针内核上跑 dedup_eq: PROT-UNRECORDED / PROT-SKIP-WINDOW / PROT-EAGAIN-LEAK 全零命中; protect_window 逐页 query+mark 完整; strace 中每条 `mprotect(…)=0` 的 range 与后续写入一一对应 |
| 2 | pending-perm 与已映射页次序（FRESH 装 RO 页后 mprotect 改 PTE, 元数据不一致） | **部分成立→真子因** | 路由对 present-real-RO 页确实"留 RO 等自愈"（设计如此）, 但本次取证从未触发该形状（PROT-LEAVE-RO/DROP-ZERO/MAPANON-CONFLICT 零命中）。真缺陷是 zap 把 perm 擦掉, 使"下次写"直接死在 FRESH 门（见 §2-Ⅰ） |
| 3 | flush 缺失/不全（窗口粒度 vs 4K, 单/多核） | **证伪** | 所有 ACCERR 崩溃点的 fault 都进入了 fault 路径并被 metadata/门判死（探针给出 metadata 原文）, 不是陈旧翻译; flush 累积区间 [first,last] 覆盖完整; 单核 taskset 复跑同形（meta 判定与 CPU 数无关） |
| 4 | fork_demote/multi-piece 端点 lookup 记错 piece | **替换为更准的同域缺陷** | demote 的 lookup/piece 处理本身正确; 真问题是 demote 语义缺口: plain VMA 只剩 DECLARE flags, routed perm 全丢（见 §2-Ⅱ）。假设 4 与 5 的"误吞"均未发生 |
| 5 | JVM CDS 被白名单误吞为 auto-arena → 权限分离错乱 | **证伪（对前 4 app）, 但 JVM 另有第三形状残留** | dedup/metis/psearchy 崩点全在匿名 arena 窗（无 file piece）; classes.jsa 在 MODE 窗的落点是 D-G'' punch 的正确产物（hole=file VMA, FB-LOOKUP 走 legacy）。JVM JThreadBench 仍有 ClassFormatError（零字节类页）, HEAD 内核 1:1 复现 → 既有登记族, 非本次回归（§6） |

## 2. 取证原文（探针内核, 逐条）

### 2.0 复现基线（修复前 HEAD 构建 #31, corten=on）

```
$ LD_PRELOAD=/root/corten_mode_hook.so /root/dedup_eq 8 6 1   → rc=139 (3/3)
dmesg: corten-rogue: FRESH-GATE-ACCERR addr=0000100000250000 w=1 x=0
       arprot=8 m.state=0 m.perm=0          ← ar->prot=NONE, metadata 全空
strace 关联:  8539  mprotect(0x10002025b000, 4096, PROT_READ|PROT_WRITE)=0
              8741  madvise (0x10002025b000, 20480, MADV_DONTNEED)=0
              8956  SIGSEGV {si_code=SEGV_ACCERR, si_addr=0x10002025b118}
```

→ **根因 Ⅰ**: mprotect(RW) 路由把 metadata 记成 PRIVATE_ANON+RW → 写 fault 正常 →
`madvise(DONTNEED)` 走 dontneed_route → unmap_chunk → zap_window 里
`corten_unmap()` 整槽擦除（state=INVALID, **perm=0**）→ 下次写 FRESH 门查 ar->prot=NONE
→ ACCERR。strace 同页"commit → drop → 再写"三连即最小形状。

### 2.1 第二层: 揭露后的退出期崩溃（rc=139 残留, 无任何探针命中）

修掉 Ⅰ后 dedup 仍 3/3 rc=139, 且 **FRESH/DISP/MAPANON/RETRY/FB-\* 全部零命中**:

```
strace:  --- SIGSEGV {si_code=SEGV_ACCERR, si_addr=0x100038000030}   （每线程同址）
寄存器:  RIP libc.so.6+0xa2b40  Code: … <c7> 00 00 00 00 00 …
         RAX: 0000100038000030   ← movl $0,(%rax), 崩在窗口基页 +0x30
rogue_dump.so 取证（SIGSEGV handler + /proc/self/{maps,pagemap,smaps}）:
  SIGSEGV si_addr=0x10003c000030 si_code=2   ← 与 T5 登记 "0x10003c000030 error 7" 同址族
  覆盖 VMA: 10003b000000-100043000000 ---p … Rss: 132 kB, Shared_Dirty: 132 kB
            ↑ 128MB **---p**（PROT_NONE）、页 present、Shared_Dirty>0（fork 后共享）
触发条件矩阵:
  rogue_dump.so 单独（无 fork probe）      → rc=0
  rogue_dump.so + corten_mode_hook.so     → rc=98（dumper 抓到, 同 si_addr）
  corten_mode_hook.so 单独                → rc=139 (3/3)
```

→ **根因 Ⅱ**: hook 的 atexit fork 探针 → dup_mmap → `corten_arena_fork_demote()`:
shadow-VMA unshadow 成 plain anon VMA（只带 DECLARE flags=PROT_NONE reserve）,
而 glibc 已通过 routed mprotect 提交的 chunk（metadata-only 的 perm）在 plain VMA 上
**不存在**; 退出期 libc 清理写（heap_info/chunk 头, 窗口基页 +0x30/+0x10）→ x86
access_error 直接 ACCERR。present 页在 ---p VMA 里 = "present-RO PTE" 观感的真正来源。
fork_demote 在 VMA copy **之前**运行 → child 侧同样丢失 → 双侧同缺陷。
（=T5 三个 app 3/3 rc=139 的本尊; 与 OQ-D fork 边界同根, 本文修复即其落地。）

### 2.2 反证: 修复后探针零命中 + 计数器干净

修复构建上全矩阵复跑: FRESH-GATE-ACCERR / DISP-ACCERR / MAPANON-CONFLICT /
RETRY / FB-\*(除正常 legacy fallback LOOKUP) 均零命中; debugfs
rearm_failed=0 / eagain_leaked=0 / drain_timeout=0 / legacy_drift=0。

## 3. 修复（commit e219920b0923, 6 文件 +514/-24）

### 3.1 CORTEN_UNMAP_KEEP_PERM（include/linux/corten.h, mm/corten.c）

`corten_unmap()` 增加第 4 参 flags; `CORTEN_UNMAP_KEEP_PERM` 位使槽回到
CORTEN_INVALID 时**保留 perm**（flags/__resv 仍清）。默认路径（fork demote scrub 等）
行为不变, 全部 12 个旧调用点显式补 0。

### 3.2 zap 带走 perm（mm/corten_arena.c zap_window）

chunk munmap / MADV_DONTNEED 路由的内容擦除改用 KEEP_PERM: 内容清零、VA 上已提交的
mprotect 契约存活 → FRESH 门重新推导出"已提交权限"（=legacy 的
zero-fill-on-demand 语义）。

### 3.3 FRESH 门 perm 优先（fault_once）

```c
.perm = m.perm ? m.perm : READ_ONCE(ctx->ar->prot),
```
INVALID+perm≠0 的槽 = KEEP_PERM 的内容 drop 页; 其余照旧回落 ar->prot。

### 3.4 demote 物化（mm/corten_arena.c `corten_arena_demote_materialize()` +40 行级）

fork_demote 在 scrub **前**（metadata 还活着时）把 recorded perm 重表达进 VMA 层:
逐 shadow piece 走 metadata → 压缩成 perm 同质 run（跨 2M 窗连续、punch hole 处断开、
untracked 窗按 ar->prot 封口）→ `__split_vma`（与 punch 路由同一手术、同一
dup_mmap mmap_write 嵌套）→ 对 piece 精确置 R/W/X flags + vma_set_page_prot()。
效果: parent 活下来带着已提交 chunk 的真权限; child 的 VMA copy 同样正确;
unrecorded 页=bound=plain VMA 原状, 只有真实提交产生 split。

### 3.5 KUnit 锚（+3 用例, 全部真链路）

- `corten_fault_test_zap_keep_perm`: commit→DONTNEED→再写 必须 zero-fill 成功
  （修复前=ACCERR, 本族主锚）; 槽断言 INVALID+perm=RW; 提交区外写仍 ACCERR。
- `corten_fault_test_zap_keep_perm_downgrade`: commit→mprotect(NONE)→DONTNEED→写仍拒
  （KEEP_PERM 不复活旧提交）→ 重新 commit→写恢复。
- `corten_arena_test_fork_demote_perm`: PROT_NONE reserve + 路由提交 2 页 →
  fork_demote 后 VMA 精确分裂为 [base,commit) NONE / [commit,+2p) RW / 尾段 NONE
  且全部 anonymous、无 VM_CORTEN。corten=off 时 skip（路由不存在）。

## 4. 复验矩阵

| 面 | 结果 |
|---|---|
| guest dedup_eq 8t glibc（fork probe 在内） | **5/5 rc=0**（修复前 3/3 rc=139） |
| guest metis_eq 8t/4t text1600（fork probe 在内） | **3/3 rc=0**（ checksum 8a8db99075665220 两轮一致; 修复前 3/3 rc=139） |
| guest psearchy_eq 8t text368 | **3/3 rc=0**（修复前 3/3 rc=139） |
| guest `java -version`（MODE on, 判据） | **rc=0**（横幅完整; 修复前横幅后 libc 崩） |
| run_mode_smoke | **26/26 SMOKE-DRIVER PASS** |
| debugfs 计数器 | rearm_failed=0 eagain_leaked=0 drain_timeout=0 legacy_drift=0 attach_fail=0 |
| KUnit on（corten=on kunit.enable=1） | **24/0/1 + 24/0/0 + 21/0/0 全绿**（含 3 新用例; 首跑 corten 套件 `txn_mutex_disjoint` 1 次 flake=M3a §5 既登记签名, 复跑全绿; boot-kunit-on 日志 r06-rogue-kunit-on.log） |
| KUnit off（corten=off kunit.enable=1） | **25/0/0 + 17/0/7 + 4/0/17, 零 fail**（skip=真链路用例按设计门控, 含新用例的 off-skip） |
| =n 回归 | CORTEN_MM=n: 16 个含钩子对象（sys/mmap/memory/madvise/mremap/migrate/mempolicy/mlock/mseal/vma/mprotect/rmap/page_alloc/fault.o/pgtable.o + kernel/{sys,fork}.o）**零 error 零 corten 符号**; memory-failure.o 的 redefinition 为上游 config 组合既有伪影（与本 diff 无关, =y config 下同样不编该对象路径） |
| =y+ARENA=n 组合 | CORTEN_MM=y + ARENA=n: corten.o/corten_test.o/mmap.o/memory.o/fork.o/madvise.o/mprotect.o **零 error**（新 4 参 unmap 在桩头与真体两态均成立） |
| checkpatch --strict | **0E/0W/0C**（714 diff 行） |
| 探针残留 | 全部撤除（grep ROGUE-PROBE/corten-rogue=0）; 提交后重建 bzImage 与验证构建逐字节同源 |

## 5. 假设外的新证据与口径修正

- "fork 前/后提交丢失"二分口径**合并**: 修复前的 dedup "fork 前"形状（DONTNEED 族, 根因Ⅰ）
  与登记为 OQ-D 的"fork 后 ACCERR"（根因Ⅱ）是同一不变式的两个暴露点, 本次一并闭环。
  OQ-D 的残余（如 M5 真共享语义、demote 后窗口游标行为）不发生变化。
- tcmalloc -91.8% 与 JVM spawn >480s 两个 T5 性能信号此前被本族 rc=139 遮蔽;
  本修复后 dedup tcmalloc 臂可正常完成, **M8 应按 t5-overhead-diagnosis §4 的口径重测**。
- 首跑 KUnit flake（txn_mutex_disjoint, max_crit==1 断言）为本班新观测到的第 3 例
  M3a §5 签名, 复跑全绿; 建议规划者把该用例列入"首跑容忍/复跑判定"清单（与
  txn_uninstall_interlock 同列）。

## 6. 残留（登记, 不阻塞本次判据）

1. **JVM JThreadBench MODE on ClassFormatError**（`Unknown constant tag 0`）: 类页读到
   全零。-Xshare:on/off 同样失败; HEAD 修复前内核 1:1 复现（hs_err 同族）→ **既有缺陷,
   非本班回归**; 属"archive/jimage 内容路径"第三形状, 与 CDS punch hole 的内容页相关
   （FB-LOOKUP 证明 fault 已正确走 legacy, 零字节另有来源）。java -version（今日判据）
   rc=0 不受影响。建议下一切片: rogue_dump 扩展为 SIGBUS/ClassFormatError 联动取证 +
   jimage 私有映射地址核算。
2. find_vma_intersection（fault_owned tier-2）在 RCU 语境下的 mmap_assert WARN:
   java 重载类时高频出现（本班 VM 控制台 42 条）。功能无害（RCU walk 合法）, 属噪音级
   设计瑕疵, 建议改用 lockless 变体或压掉 assert。
3. memory-failure.o 在 =n 变体下的上游 redefinition 伪影（见上表注）。

## 7. 产物与状态

- commit: **e219920b0923** "mm: CortenMM arena: carry routed permissions across content
  drops and fork demotion (D16)", tag **corten-r06-rogue**（主树 android17-6.18, 未 push）
- bzImage: bzimg/r06-rogue/bzImage sha256=c767ab30…c061b（=已提交树构建 #42）
- 本目录: rogue_dump.c/.so（SIGSEGV 取证 preload）、r06-rogue-kunit-on.log
- VM: tmux `vm` = 最终构建 corten=on kunit.enable=0, dedup/java/smoke 验证后留运行
- 时间: 09:50–15:0x CST; 密码未落盘; 未 push; 未触碰其他 worktree
