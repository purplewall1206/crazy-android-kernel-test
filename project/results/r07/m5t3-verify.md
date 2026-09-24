# CortenMM M5.T3 —— GUP 互操作验证与收口（r07/m5t3）

- 班次: 2026-09-19 夜窗 21:30 起通窗（timegate 23:00 起计费外）
- worktree: /home/ppw/linux-6.18-m5t3（基座 1f8dfc78ae9f = M3b+M4.T0+M5.T1a/T1b 全量, branch m5-t3）
- **未 commit**（maintainer 判定; 本片非零 diff —— 两个真 bug 修复, 见 §2/§3）
- 出货件: worktree bzImage sha256=**8b7e658d…90648**（全部证据出自该件; gupmat guest 套件在其上 ALL PASS）

---

## 0. 结论一句话

**四态矩阵"证明而非新码"的命题成立了一半**: fast 路径零改动被证实（gupfix 门下放后 arena PTE 对 gup_fast 完全透明, guest 实测四种形态全对）, 但 slow 路径的"收口"挖出了 T2' 遗留的两个真 GUP 互操作 bug —— (1) fault_once 的 FOLL_FORCE 重定向把"perm 缺 WRITE 的外部写"回答成 RO 存活页, 普通 GUP 写 pin（pread/io_uring zc）re-follow 不了 → **GUP 自旋死循环**; (2) OQ-4 的 UNSHARE→write 映射让 **read-pin unshare 在 routed-RO 页上被误拒 ACCERR**。修复（force 语义整体移除 + UNSHARE-ACCERR 落回 legacy unshare）+ `zap_pinned` debugfs 计数 + 5 个 KUnit 矩阵锚 + guest 四态程序, 全套判据绿。

## 1. 审计: zap 路径 pin 判定（任务书假设证伪）

任务书假设"mmu_gather pages 数组会做 folio_check_pinned WARN, 若我们绕过 pages 数组则无风险"。读码判定:

- **arena zap 路径没有绕过 pages 数组**: `corten_arena_zap_window()`/`zap_untracked_window()` 对每个非 special present PTE 走 `folio_remove_rmap_pte` + `__tlb_remove_page_size` → mmu_gather 批量 → `tlb_flush_mmu` → `__tlb_batch_free_encoded_pages` → `free_pages_and_swap_cache`（仅放 PTE 引用）。
- **mm/mmu_gather.c 没有 pinned WARN**: 只有形状类 VM_WARN（page_size/folio 一致性）。FOLL_PIN 的保活是纯引用计数原生语义（pin = +GUP_PIN_COUNTING_BIAS 或 _pincount）, zap 放 PTE 引用, pin 引用携带 folio 到 unpin —— 与 legacy `zap_present_ptes`→`tlb_remove_page` 完全同构, **无缺口、无需 WARN**。
- **真正的 pin 危险面在"复用侧"而非 zap 侧**: 带写复用一个 pinned folio 才是数据损坏。该判定 T2' 已在 `cow_write()`/`force_write()` 两处实现（`folio_maybe_dma_pinned(old) → copy`）。
- 审计的正面产出 = SPEC §4.3 预告的 `zap_pinned` 观测计数落地（§2）+ guest 长期 pin 实测（§4, delta 5289）。

## 2. 实现

| 项 | 内容 |
|---|---|
| `zap_pinned` 计数 | `zap_window()` 清 PTE 处 `folio_maybe_dma_pinned(folio)` 命中 → 全局 atomic（T0b named-counter 惯例, per-mm percpu debugfs 不可达）→ `arena_stats` 新行 `zap_pinned`。不为拒绝（语义合法）, 为 M6.T2 迁移/回收的 skip-gate 频度证据 |
| KUnit 四态锚 ×5 | `corten_arena_test_gup_state{1,2,3,3b,4}_*`: ①PROT_NONE 预约未触页 + GUP 形状读/写 fault → VM_FAULT_SIGSEGV + 槽位保持 INVALID; ②committed RW + 写 fault → pte_write/exclusive/单引用 PTE（gup_fast 直通形状）; ③fork 共享 RO + FAULT_FLAG_WRITE → COW 拷贝支（新独占可写 folio, 子侧保持）; ③b routed mprotect 降权 → 写 ACCERR → 回升 → RESTORE 自愈（同 folio）; ④simulated FOLL_PIN（GUP_PIN_COUNTING_BIAS）+ `unmap_chunk` → folio 存活（refcount=BIAS+holder）→ unpin → 归零释放 + `zap_pinned` 计数 +1 |
| guest 程序 | `bench/share/gupfix/gupmat.c`（+`gupmat-run.sh` 驱动, `xprobe.c` 诊断器）: 四态端到端, pread(9p zc=写 pin)/process_vm_readv/writev/vmsplice/裸 syscall io_uring REGISTER_BUFFERS（长 term pin）, 校验和口径 |

## 3. 两个真 bug 与修复（本片 diff 主体）

### 3.1 FOLL_FORCE 重定向 × 普通 GUP 写 = GUP 自旋（guest s3 形状发现）

- **复现**: MODE arena committed RW 页 → routed `mprotect(R)`（whole, 阴影 VMA 失 VM_WRITE）→ `pread` 写 pin 进该页 → `corten_arena_fault_once` 100% 内核态自旋（guest sysrq-l 抓获: RIP `corten_arena_fault_once`, `__get_user_pages` 循环）。
- **根因链**: gupfix 门（`corten_own = VM_CORTEN && !FOLL_FORCE`）让非 FOLL_FORCE 写穿过 !VM_WRITE 阴影 VMA（jtbcfe pread 的必要形状）→ T2' 重定向 `ctx.force && disp==ACCERR → FORCE_COPY` → force_write 拷贝支**按记录 perm 装 RO 存活页**（契约保形, mkwrite 红线）→ 该存活页只有 `can_follow_write_common()`（要求 **FOLL_FORCE**）能 re-follow → 普通 GUP 写 pin（无 FORCE）follow 失败 → 再次 faultin → force_write 再拷 → **无进展死循环**。`FAULT_FLAG` 里没有 force 位, 事务层无法区分 ptrace 与 pread。
- **修复与裁决**: 移除 force 语义整体（重定向 + `corten_arena_force_write()` + `CORTEN_DISP_FORCE_COPY` + `ctx.force` + `CORTEN_ARENA_STAT_FORCE_WRITES`）。perm 缺 WRITE 的外部写（**含 ptrace poke**）统一 ACCERR → 响亮 EFAULT —— 把 T2' 残余登记 ② 的裁决（"外部写不得无声废止进程契约", 原 routed-partial 形状）贯彻到 routed-whole。代价: 调试器对 routed-RO arena 页的 poke 现在 EFAULT（upstream 可写, 经 FOLL_FORCE RO 存活页）; 与自旋二选一, 按 T2' 既有裁决方向取 ACCERR。cow_write/RESTORE 的可写契约形状不受影响（SHARED+WRITABLE → COW_MAYBE → cow_write 拷贝支天然可写, unshare_pin 锚不变）。
- **测试**: `corten_fault_test_foll_force` 重写为"外部写契约"锚（四种形状断言 SIGSEGV + PTE/perm/exclusive 不动; 保留 routed-partial 边界 beat）。

### 3.2 OQ-4 UNSHARE→write 映射 × read-pin = routed-RO 页读被误拒（guest s3 第二拍发现）

- **复现**: routed-RO 页 + fork 残留（子退出后 exclusive 已清）→ 跨进程 `process_vm_readv`（**读** pin）→ `gup_must_unshare()`（!exclusive+PIN, 无视读写）→ -EMLINK → `faultin(FAULT_FLAG_UNSHARE)` → OQ-4 映射 `ctx.write=true` → dispatch ACCERR → 读操作 EFAULT。对照: 同页未降权时 child 读 rc=256 ✓（`xprobe.c` 两相判定）。
- **修复**: `ctx.unshare` 记录 UNSHARE 来源（write 映射保留, 写 pin unshare 语义不变）; gate 返回 ACCERR 且 `ctx.unshare` 时 → `CORTEN_FAULT_FALLBACK_BIT|VM_FAULT_FALLBACK` → legacy `do_wp_page(unshare)` 接管 —— 它 native 拥有"仅破 folio 共享、按记录 perm 编码装 PTE（无 mkwrite）", 读 pin 随后 follow 成功。
- **自愈性核对**: 写 pin 永不产生 UNSHARE（`gup_must_unshare` 对 FOLL_WRITE 返回 false）, 故该 fallback 不会吞掉真正的写拒绝。

## 4. 四态矩阵终表（KUnit 锚 × guest 实证, 出货件 8b7e658d）

| 形态 | KUnit 锚（ok 号） | guest 证据 |
|---|---|---|
| ① PROT_NONE 预约+未触 + FOLL_WRITE/READ | `gup_state1_uncommitted`（35）| `gupmat s1`: pread 写 pin EFAULT/EIO, process_vm_writev/read EFAULT; legacy 对照一致 |
| ② committed RW + FOLL_WRITE | `gup_state2_committed_rw`（36）| `gupmat s2`: pread 32K 逐字节 ✓, 跨进程 read pin ✓, vmsplice→pipe 往返 ✓ |
| ③ fork 共享 RO + FOLL_WRITE → COW | `gup_state3_fork_cow`（37）| `gupmat s3cow`: 父写 pin COW 成私有可写拷贝, 子视图 magic 不变, 双向核对 |
| ③b routed mprotect 降权 → ACCERR/RESTORE | `gup_state3b_downgrade_restore`（38）| `gupmat s3`: RO 写 pin 响亮拒绝（读 pin 验证 ✓, 修复后跨进程读 ✓）, 回升 RESTORE 再 pin ✓ |
| ④ FOLL_PIN × arena zap 保活 | `gup_state4_pin_zap`（39）| `gupmat s4pin` ×2（与 arena_stress 4T 并发）: io_uring 注册 buffer 长期 pin 存活全程 churn, 1430/864 次远程 pin 读零损坏, **zap_pinned delta=5289**, unpin 后正常回收 |

fast 命中确认: ②/s2 的多页 pread 在已装页上不进慢门（r06 kprobe 口径）, ①③/b 均为 bail-to-slow 后由元数据干净裁决。`perf` 符号确认项由 guest dmesg 零 WARN 替代（无盘无 perf 符号表）。

## 5. 判据

| 判据 | 结果 |
|---|---|
| =y 全量零新增警告 | PASS（唯一 warning= `objtool: cpuidle_enter_state … return with instrumentation`, T1b final/r06 build 既有）|
| KUnit corten\* on×2 | PASS ×2: `m5t3-kunit-final-on1/on2.log` = 24/0/1 + **43/0/0**（+5 新用例 ok35-39）+ **26/0/0**（foll_force 重写后全绿）; 出货件重建后 on3 复锚同数 |
| KUnit corten\* off×1 | PASS: `m5t3-kunit-final-off1.log` = 25/0/0 + 18/0/25 + 5/0/21（新用例 off 臂设计性 skip）|
| lockdep 变体（M7 联动）| PASS: PROVE_LOCKING 件 KUnit 全绿（24/0/1+43/0/0+26/0/0）, 零 lockdep 签名; 首跑 `txn_uninstall_interlock` flake = 已登记宿主噪声签名, 静置复跑全绿 |
| =n 七对象 | PASS: kernel/sys.o + mm/{mmap,memory,migrate,mempolicy,mremap,madvise,mprotect}.o RC=0, nm 零 corten 符号 |
| checkpatch --strict | PASS: **0E/0W/0C**（958 行, patches/r07-m5t3.diff）|
| guest 压测 | PASS: **GUPMAT-RUN ALL PASS**（出货件, boot4）: 四态 + s4pin×2 与 arena_stress（4T/16MB, ops=83635, checksum 一致, errors=0）并发零损坏零 panic; zap_pinned delta=5289; **DMESG AUDIT CLEAN**（零 WARNING/BUG/Oops）|
| 已知噪声（非本片）| `txn_uninstall_interlock` flake（2/9 跑次, gupfix 已登记签名）; foll_force 用例 2 条 `rwsem.h:88` WARN（T1b on5-final 同款, vm_flags_set/clear 无 mmap 写锁的测试侧噪声, 非失败）|

## 6. 遗留与移交

1. **ptrace 语义分叉（设计裁决, 非缺陷）**: routed-RO arena 页的 FOLL_FORCE poke 现在响亮 EFAULT（upstream: COW 写成功）。恢复 upstream 行为需要 handle_mm_fault 可见 FOLL_FORCE（上游级接口变更）—— 若 maintainer 要, 走独立切片。
2. **foll_force rwsem 测试噪声**: 2 条 `rwsem.h:88` WARN（测试 vm_flags_set/clear 不持 mmap 写锁）T1b 起既有; 可加 mmap_write_lock 消音, 测试侧小片。
3. **madvise/ROUTE 计数面**: gupmat churn 走 routed DONTNEED（zap_pinned 5289 为证）, 无遗留。
4. **GUP 门下放的指针**: check_vma_flags 的 `corten_own` 放行面 + 事务层 verdict 必须可被该调用者 re-follow —— 本片把该不变量写成显式注释（fault_once [T3] note）; 后续任何新 verdict 都要过"谁 re-follow 得了"这一问。
5. VM 终态: tmux `m5t3-vm`（hostfwd 10028, ssh key /home/ppw/vm/trixie.id_rsa）= 出货件 corten=on, 复现物 /mnt/share/gupfix/ 原样留运行。磁盘 /home/ppw/vm/trixie-m5t3.img（trixie.img 克隆）。**未 push; 未碰主树/其它 worktree**（tmux `vm` 会话按任务书授权停用, 其盘/镜像未动）。

## 7. 产物

- patches/r07-m5t3.diff（= worktree diff, 5 文件 +528/−285: corten_arena.c / corten_arena.h / include/linux/corten_arena.h / corten_arena_test.c / corten_fault_test.c）; 快照 results/r07/m5t3.diff-snapshot
- results/r07/m5t3-*.log（build y1..y9 / build-n-final / lockdep-build / kunit final-on1,on2,off1,on3,fix1..3,lockdep,r2 / guest-boot*.log）; m5t3-gupmat-results.txt; checkpatch-m5t3.txt
- guest: bench/share/gupfix/{gupmat.c, gupmat-run.sh, xprobe.c}（宿主同目录）
