# M-V A.3b 开发报告：J1 计数器收口（A 系出口门的一半）

- 产出: A.3b 开发 agent（2026-09-23）
- worktree: `/home/ppw/linux-6.18-mva` @ 分支 `mv-a0`，基座 = 主树 HEAD **c4d3b55f5959**
  （A.3a 已入库为 788cb26e36c0）；本片为未提交增量，**未 commit**（红线遵守）
- 任务书: `next/va3-dev-brief.md` §2.7/§2.8/§2.10（P2 行）+ §4 A.3b 行 + §5 B 组；
  缺陷机理: `next/j2-audit-draft.md` #1/#2/#29 + "J1 卫生"小结 + 挂点清单五挂点
- 补丁: `/home/ppw/cortenmm/patches/r07-mva3b.diff`（11 文件 **+655/−21**，未 commit）
- 行号口径: 本文 file:line = A.3b 增量后的 worktree 实码

## 1. 改动总览

| 文件 | 增/删 | 内容 |
|---|---|---|
| mm/corten_arena.c | +153 | 四观测计数器 + 五个窗口域 helper（#1/#2/#29 判定 + #3/#7 计数）+ debugfs 四行 + 2 测试钩子 |
| mm/corten_arena.h | +45 | 四 helper 的 =y 声明 + =n static inline 折叠 |
| include/linux/corten_arena.h | +24 | `corten_fault_window_fallback`（arch 侧 caller）声明 + =n 折叠 + 2 测试钩子声明 |
| mm/mmap.c | +30/−8 | J1 挂点 4（find_vma_prev）+ generic 两 walker 查窗前置 + NOREPLACE 操作数序 |
| mm/mmap_lock.c | +15 | fault #2 慢路径窗口终答（lock_mm_and_find_vma） |
| mm/userfaultfd.c | +27 | J1 挂点 5（find_vma_and_prepare_anon）+ mfill/move 入口 #29 短路 |
| arch/x86/mm/fault.c | +13 | fault #1 快路径 FALLBACK→慢路径直达臂 |
| arch/x86/kernel/sys_x86_64.c | +12/−4 | x86 bottomup walker 查窗前置（对齐 topdown 的 A.2 形态） |
| mm/gup.c | +9/−1 | #3 观测：gup_vma_lookup 窗口 miss 计数 |
| mm/memory.c | +10 | #7 观测：__access_remote_vm 短读计数 |
| mm/corten_arena_test.c | +310 | KUnit B 组 4 用例 |
| **合计** | **+655/−21** | 内核 ~+345/−21，测试 +310 |

内核增量超 brief §4 预算（~+200）的主因与 A.3a 相同：注释密度对齐本屋惯例
（五 helper + 三 walker 改形全部带设计论证注释），以及任务书第 1 项之外的
豁免扫尾实改（§2.4 三 walker，发现项、非 brief 原文）。功能面未越 A.3b 界。

## 2. 逐项落点

### 2.1 J1 五挂点补齐（任务 1）

A.2 已挂三（find_vma_intersection / find_vma / lock_vma_under_rcu）。本片补：

4. **find_vma_prev**（mm/mmap.c:1113 定义体）——`vma_iter_load` 之后、`*pprev`
   写入之前插 `corten_j1_probe(mm, addr, addr + 1, vma)`，与 find_vma 同形
   （brief §2.7 建议的"签名用 load 结果"形态；probe 只读 vma 判 hits，
   不依赖 *pprev 语义）
5. **find_vma_and_prepare_anon**（mm/userfaultfd.c:49）——`vma_lookup` 之后插
   同形 probe。**该函数用 mtree_load 直读，不经任何已挂原语**，挂点不可省
   （brief §6.3 陷阱 3）。该文件新增 `#include "corten_arena.h"`

### 2.2 corten 自身豁免扫尾（任务 2）

- 全文件扫描 mm/corten_arena.c 的 find_vma 族直调：**零残留**——7 处树走查
  全部走 `corten_vma_find` 别名（:3510/:3643/:3703/:3836/:5365/:8590/:9507，
  A.2 转换 6 处 + A.3a P4 新落 1 处），本片新代码零新增直调（#2 helper 走
  `corten_arena_lookup` 帧表，不触树）
- 审计 #52 点名的 5 处 `vma_lookup`（DECLARE validate :1800 / fork 镜像 :5003 /
  parkable :8777 / punch_split :9412/:9429）**逐一复核后保留**：vma_lookup 是
  mtree_load 直读，不在五原语覆盖内，**不会自污染 J1**（brief §2.7"若留在
  find_vma 族上才需换"——它们不在）。punch_split 两处查的是即将拆分的真树
  VMA 片段（split 前后各一次），语义上必须读树
- **发现项（本片实改）**：三个 hint 放置 walker 的 find_vma/find_vma_prev
  在 `corten_addr_in_window` **之前**执行——x86 bottomup（sys_x86_64.c 原
  :146）、generic bottomup/topdown（mmap.c 原 :838/:901 附近）。MODE 进程带窗口
  hint 的合法 mmap 会在接受臂已死（窗口 hint 必被 fence 重放）的情况下白走
  一次窗口树查询 → **J1 probes+1/次，用户态可任意触发**。x86 topdown 的
  A.2 守卫本就是"先查窗后走树"。修正：三处全部改为窗口门前置、lookup 内
  嵌（接受条件去掉了恒真项，语义零变化）；NOREPLACE 检查（mmap.c:511）同
  理把 `occupied_incl_idle` 操作数前置（纯谓词 OR 交换，occupied 窗口短路
  后不再白走 intersection）。A.3a 的 hint_fence 用例回归覆盖此面
- `corten_vma_find` 的 intersection 语义陷阱（brief §2.7 末段）：本片零新
  转换，不适用

### 2.3 fault #1/#2 短路（任务 3）

**#1 快路径**（arch/x86/mm/fault.c:1399 FALLBACK `default:` 臂）：
`corten_fault_window_fallback(mm, address)` 为真 → `goto lock_mmap`。
双门 + 窗口域即计 `corten_fault_fallback_window` 并改道慢路径——**不做无锁
MAPERR 终答**（brief §6.7 红线：并发 DECLARE 未发布期的 fault 必须等锁序点，
不能假 SIGSEGV；任务书"直接 MAPERR 终答"按 brief 权威形状落实为"慢路径
mmap_read 下的 MAPERR 终答"，结局与 S-1 完全一致且竞态语义不动）。

**#2 慢路径**（mm/mmap_lock.c:451 lock_mm_and_find_vma）：
get_mmap_lock_carefully 之后、首个 find_vma 之前：`corten_fault_window_maperr`
（双门 + 窗口 + `corten_arena_lookup` 无活跃 region，RCU 走查帧表）为真 →
`mmap_read_unlock + return NULL` → 上游 bad_area MAPERR（S-1 结局同今天，
少一次树走查）。**活跃 region 命中则继续 find_vma**——ownership-fallback
形状（活跃窗下压了 punch 植入 VMA）必须让 legacy 找到真 VMA 服务文件 fault。
mmap_read 与 DECLARE/park 的 mmap_write 串行，无新竞态。第二个 find_vma
（upgrade 分支）仅当首个查到 GROWSDOWN VMA 才可达，窗口地址永无此形状，
一处检查覆盖两分支（注释写明）。

**勿动活跃窗路径**：快钩对活跃窗 fault 的事务服务（HANDLED/ACCERR/...）
零改动；#1 只作用于"快钩已答 FALLBACK 且地址在窗口域"的尾巴。

### 2.4 uffd #29 短路（任务 4）

- `corten_uffd_window_reject(mm, dst_start, dst_len, src_start, src_len)`
  （mm/corten_arena.c:2416）：双门 + "[dst ∪ src] ∩ [16T,64T) ≠ ∅" → 计数
  + true；调用方返回 **-ENOENT**（C12 认可的终态错误码）
- 挂点两处（process_uffd/ioctl 同漏斗无需另挂）：
  - `mfill_atomic` 入口（mm/userfaultfd.c:736，四条 mfill 命令的公共漏斗，
    src 传 0/0——mfill 的源是 copy_from_user 缓冲，永不查树）
  - `move_pages` 入口（:1808，src/dst 两腿都查树，两腿任一入窗即拒）
- **披露角落**：被 uffd 注册过的 punch 植入 VMA 现在同样 -ENOENT——给
  MAP_FIXED 打入窗口的植入片注册 uffd 超出一切已登记形态（C12 口径：
  窗口=无 uffd），注释与 §6 残留清单均已写明

### 2.5 四观测计数器（任务 5）

全部 `static atomic_long_t corten_nr_*`（对齐全屋惯例，不 per-cpu——
brief §2.7 论证：违例路径无竞争压力，RCU/可抢占上下文无条件安全）：

| 计数器 | 挂点 | 性质 |
|---|---|---|
| `corten_nr_fault_fallback_window` | #1 改道臂 + #2 终答臂 | **计臂次不计 fault 数**：一个 parked/空洞窗 fault 依次过两臂 = +2（注释写明） |
| `corten_nr_uffd_window_reject` | §2.4 两入口 | 终答计数 |
| `corten_nr_gup_window_miss` | mm/gup.c:1313 gup_vma_lookup miss 且窗口 | 纯观测（#3 修复在 V-C corten_gup_probe） |
| `corten_nr_remote_access_window_short` | mm/memory.c:6992 __access_remote_vm 短读臂 | 纯观测（#7 修复在 V-C） |

debugfs stats 四行新增（mm/corten_arena.c:2610-2622）；KUnit 钩子
`corten_arena_test_fault_fallback_window/_uffd_rejects`（mm/corten_arena.c:2867 附近）。
A.2 未落任何一者，本片四计全新（核对结论）。

### 2.6 S-5 三终答：裁定留 A.3d（任务 6）

任务书允许"红线内装得下，否则留 A.3d"。测算：本片内核侧 ~+345/−21 已达
brief §4 红线（300）量级；S-5 三条（msync 跳过臂 ~+45 / madvise parked 臂
~+45 / **mincore 活跃窗 walk_page_range_novma 真值走查 ~+120**，brief §2.11
自估 +290 内核）整体装入将把本片推到 ~+640 内核——**超红线一倍**，且
mincore 真值走查（present/swap/none 三态 + 无 THP 假设）是独立设计件。
整组移交 A.3d（brief §4 本就归其所有），其 J1 关联面见 §6 残留清单前两条
（msync/madvise 的窗口 find_vma 走查仍会计 probes——A.3d 的 S-5 落地即消）。

## 3. KUnit B 组锚（mm/corten_arena_test.c:7078-7376，4 用例）

| 锚 | 断言要点 | 结果 |
|---|---|---|
| j1_hooks | 五挂点全开：MODE mm 活跃窗（VMA-free）上 find_vma_intersection/find_vma/find_vma_prev/lock_vma_under_rcu 逐一调用 → probes 恰 +N、**hits +0**；委托域地址 delta 0（第一门）；mode_exit 后同窗调用 delta 0（第二门）。挂点 5 经 `find_vmas_mm_locked`（extern，`#ifdef CONFIG_USERFAULTFD`——本验证配置 =n，N 自适应 4） | ok |
| j1_self_exempt | MODE mm 走两轮真实 do_mmap 漏斗（addr=0 自动接管：障碍扫描走别名；窗口 hint：walker fence 前置）→ **probes delta == 0**；正控制：随后手工 find_vma(同窗址) → delta == 1（审计 #52 别名通道正确性锚） | ok |
| uffd_window_reject | helper 级（brief B10 退化形态）：mode_enter 即生效（MODE=接管）；mfill/move/跨界三形 TRUE 计数 +3；下边界外切/上边界/纯委托域 FALSE 计数不变；mode_exit 后静默 | ok |
| fault_window_shorts | #1 臂域宽（parked/active 均 TRUE、委托域 FALSE）；#2 端到端：parked 窗 `lock_mm_and_find_vma` 返回 NULL 且 **probes delta == 0**（S-1 不变 + 零树走查，本片核心断言）、计数 +3（2×#1+1×#2）；active 窗 carve-out：继续走树 probes +1、计数不增（ownership-fallback 的合法成本锚） | ok |

初版 uffd 用例一处断言写反（mode_enter 后 MODE 位即置位——MODE 本身就是
接管，短路理应立即生效），修测试不改产品码；复跑全绿。

## 4. 验证结果

| 项 | 命令/口径 | 结果 |
|---|---|---|
| 构建（=y） | `make -j8`；触改 11 文件强制重编 + 全链 | RC=0；全树仅存 2 条 A.3a 已登记基座警告（objtool cpuidle_enter_state / modpost memblock_end_of_DRAM），**零新增** |
| userfaultfd 编译 | 本验证配置 `CONFIG_USERFAULTFD=n`（userfaultfd.o 不在构建内）。临时 `-e USERFAULTFD` 编译 mm/userfaultfd.o 后恢复配置 | RC=0 零警告（挂点 5 与两入口短路的唯一 =y 编译证据；.config 已还原并复核） |
| KUnit（三套件） | `timeout 480 qemu … kunit.filter_glob=corten*`，corten=on | **corten 24/0/1、corten_arena 80/0/0（76 既有 + 4 新）、corten_fault 31/0/2**；三次复跑（on1 首轮 1 用例断言修正除外 / on2 / on3 / final）后两轮逐位一致，无 flake；最终轮在括号风格修正后的最终 diff 内核上复跑。日志: results/r07/mva3b/kunit-on{1,2,3}.log + kunit-final.log |
| 内核日志签名 | mva2-verify.sh 同款 grep | 零命中；7 条 WARNING 全为既有测试工件（P4 用例自身 WARN_ONCE、drain-timeout 用例、corten_txn_begin、foll_force rwsem），与本片无关 |
| =n 折叠 | `mva2-verify.sh n-objects`（14 对象：含本片触改的 mmap/mmap_lock/memory/gup/fault/sys_x86_64 六对象） | RC=0 零警告 + **nm 14 对象零 corten 符号**；.config 已恢复 =y（build-n.log 于 r07/mva3b/） |
| checkpatch | `--strict --no-signoff --ignore FILE_PATH_CHANGES` | **0 errors / 0 warnings / 0 checks**（829 行；results/r07/mva3b/checkpatch-mva3b.txt） |
| diff 导出 | `git diff HEAD > patches/r07-mva3b.diff` | 11 文件 +655/−21；**未 commit**（HEAD 仍 c4d3b55f5959，11 M） |

## 5. 自证清单（红线核对）

1. **INV6**：零新增 PTE 写路径——#1/#2/#29 是判定+返回，#3/#7 纯计数；
   walker/NOREPLACE 改形是谓词求值顺序调整，无语义变化 ✓
2. **=n 折叠**：新导出面 5 个（fault_window_fallback 在 public header；
   其余四者在 mm/corten_arena.h）全部 =n static inline 假值/空体；
   userfaultfd.c 挂点随 CONFIG_USERFAULTFD 整文件折叠；14 对象 nm 零符号 ✓
3. **legacy 零扰动**：全部新钩子双门（static-branch + mm->corten_mode）+
   窗口域判定；三 walker 改形对非 MODE 进程只把一个恒假检查从复合条件的
   第三操作数提到最前（短路求值次数不变）✓
4. **不动活跃窗 fault 事务路径**：corten_arena_user_fault 函数体零改动 ✓
5. **锁纪律**：#2 在 mmap_read 内（DECLARE/park 写串行）；RCU 仅包帧表
   走查；uffd 入口在锁外但只读 mm->corten_mode（锁无关读）✓
6. **计数器 atomic_long**（RCU/抢占安全，违例路径无竞争压力）✓
7. **不 commit / 不动 B 系列代码**：11 文件全部在 mm/、include/linux/、
   arch/x86/{mm,kernel}/，与 B worktree 无交集 ✓
8. **行号口径**：本文 file:line = 最终实码；brief 双行号已按 §6.10 约定
   以函数名+锚注释重定位 ✓

## 6. J1 收口后的残留清单（J1 卫生视角，按优先级）

**A.3d（S-5，workload 可见的最后两条 J1 源）**
1. **madvise on parked/空洞窗（#20）**：dontneed_route 对 idle 不可见 →
   返回 0 走 legacy → `find_vma_prev`(窗) + `try_vma_read_lock` 的
   `lock_vma_under_rcu`(窗) 各 probes+1，且 -ENOMEM 语义回归未登记。
   A.3d 的 madvise parked 终答臂落地即消
2. **msync on parked 窗（#23）**：`sys_msync` 三处 `find_vma`(窗) probes+1
   + -ENOMEM 回归。A.3d 的 skip 臂落地即消
3. mincore（#13）**无 J1 面**（vma_lookup = mtree_load，不在五原语内），
   纯 -ENOMEM 语义回归——A.3d 的 novma 走查（D24 裁决）修语义不修 J1

**V-C（gup_probe 归零族）**
4. GUP-slow（#3）：`gup_vma_lookup` 的 find_vma(窗) 仍 probes+1——本片已
   计数（gup_window_miss），修复在 V-C corten_gup_probe
5. 远程访问（#7/#8/#45）：`__access_remote_vm` 早退臂的 `expand_stack` →
   `find_vma_prev`(窗) probes+1——已计数（remote_win_short），修复在 V-C
6. SGX 源页 GUP（#33）、fixup_user_fault 家族：随 #3 的 V-C 分支自然覆盖

**低频/接受候选（提请主 agent 复议）**
7. pgsize linker_ctx（#30）：窗口内 IP（JIT 在 arena）+ madvise 时
   `lock_vma_under_rcu`(窗) probes+1，无错误面——brief 允许"接受+计数"，
   本片未加计数（发生面极窄）
8. prctl PR_SET_MM_*（#32，CAP_SYS_RESOURCE 门控）/ trace 符号化（#37）/
   BPF stack build-id（#38）/ bpf_find_vma（#40）：冷路径 probes，优雅降级，
   建议接受并写入 J1 口径披露
9. 空洞窗 NOREPLACE（非占用）：`find_vma_intersection` 仍执行（两操作数
   皆假 → 放行装入）——probes+1 一次性、无语义问题；若 A.3d 想清零需
   在门内再前置一个纯窗口短路（收益极小）

**非 J1 的关联残留（移交登记）**
10. uffd 注册过的窗口植入片 → 现答 -ENOENT（§2.4 披露角落）
11. 空洞窗 MAP_FIXED 植入的 implant_mark 生产者缺口（A.3a 报告 §8 已
    登记，A.3c walker 的登记口径复议项，本片复核仍然成立）

**J1 收口后的净形状**：纯 MODE workload（declare/fault/park/mmap/hint/
NOREPLACE-over-occupied/uffd 打窗）下，五原语 + 消费者短路的窗口 probes
**全零**；剩余可见 J1 源 = 本清单 #1/#2（A.3d S-5 首刀）与 #4/#5（V-C，
已量化）。hits 面除 punch 植入（合法形态）外无已知来源——J1==0 出口径以
"run13+churn 计数器 + bpftrace 双口径"在 A.3c 末 gate 验收（brief §6.11）。

## 7. 提请主 agent 追认/决策

| 决策点 | 本片处置 | 备选 |
|---|---|---|
| 任务书"直接 MAPERR 终答"vs brief §6.7 红线 | 按 brief：#1 只改道慢路径，MAPERR 终答发生在 mmap_read 下（S-1 结局一致，无假 SIGSEGV 竞态） | 无锁 MAPERR（拒绝，brief 明示会引入与并发 DECLARE 的假 SIGSEGV） |
| 三 walker + NOREPLACE 查窗前置（brief 未列、审计 J1 卫生族内） | 实改（纯谓词顺序，消用户态可任意触发的 J1 自污染） | 留 A.3d（会让 hint_fence 形状在 J1 门下永红，不取） |
| `fault_fallback_window` 语义 | 计臂次（一 fault 过两臂=+2），注释+报告写明 | 拆两计数器（超出审计四计数器清单，不取） |
| S-5 整组 | 留 A.3d（§2.6 测算） | 拆 msync+madvise 先行（碎片化 D 组 DoD，不取） |
