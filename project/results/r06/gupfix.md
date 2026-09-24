# CortenMM arena 多页 GUP 短读修复 —— r06/gupfix（D14 自由窗）

- 班次: 2026-09-18 下午~晚（诊断+修复+验证+入库, 时盒 5h）
- 对象缺陷: MODE 进程 arena window 内多页 `pread` 短读 → JDK21 libjimage 零字节类页 → `ClassFormatError`（jtbcfe-final.md §1.4 的内核根因）
- 环境: guest 6.18.32-g025756094542-dirty build #51（验证主构建）/ #53（提交绑定构建, 与 #51 代码全等, 仅 KUnit 空白行+config 往返差异）, corten=on, tmux `vm`
- 入库: **commit 0719bc6ae74e** "mm: CortenMM arena: defer GUP and kernel-mode fault gates to arena metadata", tag **corten-r06-gupfix**; bzimg/r06-gupfix sha256=bf814413…fc84

---

## 0. 结论一句话

**短读不是 arena fault 链拒绝的（慢钩子从未收到这些 fault）, 而是上游两个 legacy 权限门 —— GUP 的 `check_vma_flags()` 与 x86 内核态 fault 腿的 `access_error()` —— 用 shadow-VMA 的 DECLARE 时 R/W/X 位（PROT_NONE 预留的编码）提前拒绝: 路由提交（D12/D-G）只写 per-page metadata, 不改 shadow-VMA 位, 于是内核态多页访问在第一个未装页处被干净打回（extable fixup → EFAULT 无信号）, 拷贝停在页边界。** 修法: 两个门对 VM_CORTEN shadow-VMA 把裁决权下放给 arena 钩子（metadata 逐页裁决, 未提交页 VM_FAULT_SIGSEGV → GUP/EFAULT=legacy PROT_NONE 同款语义）; 故意不放宽 shadow-VMA 位 —— `corten_arena_demote_apply_run()` 的"already encoded"快路径拿它当未记录页基线（Fix-F 试错实证）。

## 1. 诊断（最小复现 + kprobe 取证, 无 JVM 依赖）

### 1.1 复现器（/mnt/gupfix/repro.c, 宿主 bench/share/gupfix/）
`mmap(NULL, 8MB, PROT_NONE, MAP_PRIVATE|ANON|NORESERVE)`（MODE 下 auto-arena, buf 落 0x100…window）→ 形状化 mprotect → `pread(fd, buf, 32768, 0)`（9p zc = GUP 写 pin）→ 返回值/内容/pagemap 三口径。基线（修复前 #47）:

| 形状 | 修复前 | 含义 |
|---|---|---|
| base（无 MODE, PROT_NONE 直读） | rc=139 SIGSEGV | legacy 对照（未动） |
| prot0（mprotect 4K RW） | **ret=-1**（0 拷贝）, 页 0 未装 | 门在 fault 派发前拒绝 |
| touch（user 写页 0 后读） | **ret=4096 短读**, 页 1+ 未装 | 已装页可拷, 未装页被拒 |
| full（整窗 RW=whole 路由） | ret=32768 全对 | whole 路由改了 VMA 位 → 门放行 |

`touch` 的 4096 = GUP 逐页 pin: 页 0 已在（GUP-fast 命中）, 页 1 走 GUP-slow → `check_vma_flags` FOLL_WRITE+!VM_WRITE → **-EFAULT, 无 fault 派发**; `i ? i : ret` 语义交付 4096 短读。jtbcfe 的 ext4 缓冲读同构: 页 0 PTE 在 → 拷 3120/3472, 页 1 fault → access_error 拒 → fixup → 干净短读。

### 1.2 kprobe 铁证（#47 内核, /mnt/gupfix/kprobe4.sh）
prot0+touch 两形状 pread 期间: `handle_mm_fault` 293 次（无关 fault 混入）, **`corten_arena_handle_mm_fault` = 0, `fixup_exception` = 0（9p/GUP 形状）, `force_sig_fault` = 0, `bad_area_access_error` = 0**。→ fault 根本没到 arena 钩子; 短读与信号无关（GUP 形状在 check_vma_flags 直接 EFAULT）。**任务假说"慢钩子对无 RETRY 许可 fault 返回 RETRY"证伪**——慢钩子内部 for(;;) 重试封顶 2 次, 出口只有 HANDLED/OOM/ACCERR/`CORTEN_FAULT_FALLBACK_BIT|VM_FAULT_FALLBACK`（继续 legacy 漏斗）, 无裸 RETRY 出口; VMA_LOCK/in_atomic→RETRY 只会送达带重试机器的调用方（GUP-slow 无 VMA_LOCK、不原子）。

### 1.3 为什么页 0 行而页 1+ 不行
差异不在 FAULT_FLAG/fill_lock/pending-perm, 而在 **PTE 是否已在**: 已在（历史 user fault 装的）→ 拷贝不触发门 → 成功; 不在 → 需要派 fault → 门用 DECLARE 位拒绝。≤8KB 读全对 = 其缓冲恰为已触摸页。SPEC §4.6 投机 folio 单页假设无涉。

## 2. 修复（方案演进, 两轮实测）

- **Fix-F（首版, 已撤销）**: 路由提交时把 perm 位 OR 进 shadow-VMA flags（upgrade-only）。复现矩阵全绿 + java CFE 消失, **但 fork-probe 挂死**: `corten_arena_demote_apply_run()` 以 piece 现有 RWX 位为基线做"已编码则免拆分"快路径（D16 的未记录页=DECLARE 位不变量）, OR 进的位让物化走样 → demoted 子进程/父进程 fault 风暴（perf: 350K faults/s, `do_user_addr_fault` 47%, 风暴线程 Common-Cleaner/Monitor-Deflation）。撤销并沉淀为不变量: **shadow-VMA RWX == ar->prot（DECLARE 界）**, 见新 KUnit 用例。
- **Fix-A（本提交）**: 门让位, metadata 裁决 ——
  1. `mm/gup.c check_vma_flags()`: `corten_own = VM_CORTEN && !FOLL_FORCE`; 写腿/读腿的 !VM_WRITE/!VM_READ 即死路径对 corten_own 放行 → faultin_page → handle_mm_fault → arena 慢钩子按 metadata 裁决（提交页 map_anon/restore 装; 未提交页 ACCERR→VM_FAULT_SIGSEGV→GUP -EFAULT, 与 legacy PROT_NONE 观感一致）。FOLL_FORCE 保持 legacy COW 语义不动。
  2. `arch/x86/mm/fault.c access_error()`: PK/SGX 检查后, VM_CORTEN shadow-VMA 返回 0 → handle_mm_fault 的 memory.c 慢钩子接管（同款裁决）。用户态腿不受影响（热钩子先行, 仅 fallback 到达此处, 语义仍收敛到 metadata）。
  3. `mm/corten_arena.c`: protect_range 尾注释固化"chunk 路由不改 shadow 位"决策。
  4. `mm/corten_arena_test.c`: 新用例 `corten_arena_test_protect_flags_kernel_gate`——PROT_NONE 形状 declare + chunk mprotect(RW) 路由后, 断言 shadow RWX 仍为 0 且 ar->prot 仍 USER（钉死 demote 基线不变量 + FRESH 界不随 chunk 动）。

## 3. 复验矩阵

| 判据 | 结果 |
|---|---|
| 复现器 commit 形状（glibc/JTBCFE 等价: 提交前缀≥读长） | **ret=32768 内容逐字节一致 ×100**（修复前 4096/-EFAULT 短读族） |
| 复现器 full 形状 ×100 / prot0 / touch / base | full 32768×100; prot0=4096、touch=4096（页 1+ 未提交, 正确拒）; base rc=139 未变（legacy 对照） |
| `LD_PRELOAD=hook java -Xmx512m HelloFmt` | **3/3 rc=0, `[hello] done`, 零 CFE, fork-probe OK**（#51 与 #53 各再验一发） |
| **JThreadBench 2000 线程 ×3 reps（M4.T0 判据）** | **counter=2000×3, median 2999ms, rc=0, 零 CFE, fork-probe OK —— 零改动 DoD 最后缺口闭合** |
| dedup_eq 8 6 1 / metis_eq 8 text1600 / psearchy_eq 8 text368 | 各 1 轮 **rc=0**（JSON 有效; dedup 4.99M blocks/s 量级正常） |
| run_mode_smoke | on 臂 **26/26 + SMOKE-DRIVER PASS**（#51、#53 各一轮） |
| green.txt 快验 off 臂 | corten 缺省 boot: 18/26 PASS, 8 失败全部=prctl mode 门 EOPNOTSUPP（mode-enter/get/exit、window mmap、fork-child）=off 态设计行为, legacy 一致性面全过 |
| KUnit on | ×2 全绿: corten 24/0/1（verify 脚本口径）; 全套 `filter_glob=corten*`: **24/0/1 + 25/0/0（含新用例 ok 17） + 21/0/0** |
| KUnit off | ×1 全绿: **25/0/0 + 17/0/8 + 4/0/17**（skip=off 臂设计性） |
| KUnit flake 记录 | `txn_uninstall_interlock` 首跑连挂 2 次 = 已登记 host 噪声签名（VM 同宿 8vCPU 抢占）, 静置复跑全绿, 与改动零耦合 |
| =n 对象 | kernel/sys.o + mm/{mmap,memory,migrate,mempolicy,mremap,madvise,mprotect}.o RC=0, **nm 零 corten 符号**（首次误列 mm/sys.o 无此对象, 以 kernel/sys.o 代） |
| checkpatch --strict | **0E / 0W / 0C**（169 行; results/r06/gupfix/checkpatch-gupfix.txt） |
| 计数器面 | drain_timeout=0, rearm_failed=0, legacy_drift=0, eagain_leaked=0, auto_attach_fail=0; dmesg 仅既有 fault_owned tier-2 RCU 噪音（java 高频, 已登记残留）, 零 Oops/BUG/panic |

## 4. 根因链（对应 jtbcfe-final.md §1.5 的内核环节替换）

glibc new_heap() PROT_NONE 预留 → DECLARE（ar->prot=NONE, shadow RWX=0）→ grow_heap/commit mprotect(RW) → D12 路由: **只写 metadata+PTE**（pending perm）→ JVM user 触摸页经热钩子装真页（页 0）→ pread 32963: 页 0 已在拷 3120, 页 1 fault → 内核态 → access_error（shadow RWX=0）拒 → kernelmode fixup → **干净短读无信号** → libjimage 不查返回值 → 零字节 CP → CFE。修复后: 同 fault → access_error 让位 → 慢钩子 metadata RW → map_anon 装 → 全量读。

## 5. 边界与移交

1. **mremap_move 的 PROT_NONE 窗**: `corten_arena_mremap_move()` 以 `perm_to_prot(ar->prot)` 声明新窗, old 窗 ar->prot=NONE 而内容全靠路由提交时, grow 的 copy_to_user 目的页 FRESH 界=NONE 仍会拒（潜在同族, 无 workload 触发, tcmalloc/dedup 均为 RW 声明窗）。建议后续切片: mremap_move 的 perm 取值改为"路由提交的并集"或显式路由。
2. **FOLL_FORCE 对 shadow-VMA**: 维持 legacy（is_cow 检查）即 ptrace-poke 仍 EFAULT——未动, 与修复前一致; 后续如需 ptrace 支持, 单独切片（sanitize_fault_flags 的 WARN 需一并审视）。
3. **arm64**: 门在 `access_error()` 等价物（arm64 fault.c 的 permission 检查）+ 通用 gup.c 已覆盖一半; arm64 热钩子未接线（M9-P2/P3）, 移植切片时按同型处理。
4. ** Fault Flag 组合审计结论**（报告第 0 节任务项）: 慢钩子无"无许可 RETRY"出口, 无需 plan A 改动; memory.c 钩子的 VMA_LOCK/in_atomic→RETRY 维持（D-G'' 契约）。
5. JVM 侧放大器（libjimage 忽略 pread 返回值）维持上游可提建议, 非本仓库。

## 6. 产物

- commit 0719bc6ae74e（主树 android17-6.18）, tag corten-r06-gupfix; patches/0001-mm-CortenMM-arena-defer-GUP-and-kernel-mode-fault-ga.patch（=patches/r06-gupfix-full.patch, 增量 patches/r06-gupfix-increment.diff）
- bzimg/r06-gupfix + .sha256（bf814413…fc84, build #53）
- 复现器与驱动: bench/share/gupfix/{repro.c,run.sh,loop.sh,kprobe4.sh}; 宿主侧同目录
- KUnit/构建/checkpatch 日志: results/r06/gupfix/
- VM 终态: tmux `vm` = #53 corten=on, 复现物 /mnt/gupfix/ 原样, 留运行; 未 push
