# M-V A.3c 开发报告：J2 审计 walker（A 系出口门的另一半）

- 产出: A.3c 开发 agent（2026-09-23）
- worktree: `/home/ppw/linux-6.18-mva` @ 分支 `mv-a0`，基座 = 主树 HEAD **51b4b09f7751**
  （A.3b 已入库）；本片为未提交增量，**未 commit**（红线遵守）
- 任务书: 任务派发（A.3c 段）+ `next/va3-dev-brief.md` §2.9/§4 A.3c 行 + §5 C 组；
  缺陷机理: `next/j2-audit-draft.md` "J2 审计钩挂点清单" corten_audit_j2_walk 设计
  + A.3b 收口两个移交（stale fallback 谓词 / J1==0 出口门断言接口）
- 补丁: `/home/ppw/cortenmm/patches/r07-mva3c.diff`（6 文件 **+999/−24**，1246 diff 行）
- 行号口径: 本文 file:line = **A.3c 增量后的 worktree 实码**

## 1. 改动总览

| 文件 | 增/删 | 内容 |
|---|---|---|
| mm/corten_arena.c | +454/−24 | J2 walker（scan + 双入口）+ 四计数器 + 采样静态键 + 九触发点接线 + fork 植入镜像 + 植入登记三处闭环修复 + debugfs 行 + audit_gate 渲染 + 手动触发/开关后端 + 4 测试钩子 |
| mm/corten_arena.h | +52 | walker/后端 =y 声明 + =n static inline 折叠 |
| include/linux/corten_arena.h | +14 | audit_gate 渲染声明 + =n 空体 + J2 测试钩子声明 |
| mm/corten.c | +100 | debugfs 三新文件（audit_gate 0444 / j2_walk 0200 / j2_walk_every 0200）+ render 枚举臂 |
| mm/corten.h | +1 | CORTEN_DBG_AUDIT_GATE 枚举 |
| mm/corten_arena_test.c | +378 | KUnit C 组 5 用例 + 采样键清理 action |
| **合计** | **+999/−24** | 内核 ~+621/−24，测试 +378 |

内核增量（~+621）超 brief §4 预算（~+230）的主因与前两片相同：注释密度对齐本屋
惯例，以及两项**必要的伴随修复**（§3 的植入登记闭环——没有它们 walker 的
fork_commit/空洞窗触发点会对合法形状误报）。功能面未越 A.3c 界（§3 的三处修复
全部是"登记口径"域，A.3b 报告 §6 残余清单第 11 条移交时点名本片复议）。

## 2. 逐项落点

### 2.1 `corten_audit_j2_walk`（任务 1，INV-MV2 断言）

- 核心扫描 `corten_audit_j2_scan` — **mm/corten_arena.c:9863**（implant 登记表
  旁）：
  - **违例判定**：MA_STATE 从窗口下界起步 `mas_for_each` 全窗走查（RCU 内，
    mm_mt 为 USE_RCU 树，任意锁上下文可调）；每个与 [16T,64T) 相交的 VMA 满足
    `VM_CORTEN`（arena 自有树件：targeted DECLARE 影子与 punch 拆分幸存片——
    brief 草图未列、实码必须分类，否则 fork_redeclare 类合法形状误报）∨
    `implant_covers(∩窗部分)`（排序数组单遍前向扫描谓词 `corten_audit_j2_covered`
    :9825，与 corten_implant_covers 同构）；违例 = **WARN_ONCE + 计数 + 首违例
    地址留档**（atomic_long_cmpxchg(0, addr)，首写者胜，0=未见）
  - **stale 分类**（A.3b 移交 1 的 fallback 谓词）：登记表逐条 `mt_find` 查树上
    有无任何相交 VMA；**登记有但树上无 → j2_stale++，不 WARN**。两个生产源：
    mark-then-install 失败残留（backstop 先登记后 install，install 可失败）与
    植入片被 munmap（登记不追清，A.3a 披露的超集白名单）；walker 收集分类而非
    误报
  - 只读自证（INV6）：全程 maple 树 + 登记表读，零 PTE 走查零写入
- 双入口（锁上下文二分， walkers 的两形态）：
  - `corten_audit_j2_walk(mm)` — **:9922**，自足形：自取 `state->ctl_lock` 保护
    登记表扫描（DEV-13 序下 mmap_read/write 持有者同样合法）。消费者：三 route
    尾 / debugfs 手动触发 / KUnit
  - `corten_audit_j2_walk_locked(mm)` — **:9948**，稳定登记形：调用方上下文已
    隔离全部 mark 写者（本 mm mmap_write 持有 / ctl_lock 持有 / mm_users==0），
    零取锁。消费者：mm_exit 头 / park / take / reactivate / fork_commit
- 双门：`corten_enabled_static() ∧ mm->corten_mode ∧ mm->corten_state`——非
  MODE mm 的窗内普通 VMA（hint 可达的合法 legacy 使用）不属窗口域管辖，零误报

### 2.2 触发点接线（任务 2，九处/审计清单全覆盖）

| 触发点 | 位置 | 形态 | 默认 |
|---|---|---|---|
| mm_exit 头部 | :3075（state 加载后、V-A.1 zap 前，树与登记表全冻结） | walk_locked | **常开** |
| fork_commit 尾部 | :5299（双 mmap_write 下，审计 child） | walk_locked | **常开** |
| pool_park_locked 尾部 | :9082（ctl+mmap_write 持有） | sample_locked | 采样 |
| pool_reactivate 尾部 | :8760（ctl+mmap_write 持有） | sample_locked | 采样 |
| pool_take 成功尾 + eject 尾 | :9246/:9261（ctl 已放、mmap_write 仍持） | sample_locked | 采样 |
| munmap_route EXACT 臂 + CHUNK 尾 | :9385/:9410（无锁，自足形） | sample | 采样 |
| madvise_route DONTNEED/FREE/hints 三终答臂 | :11109/:11165/:11492（madvise_lock 任意形态，ctl 自取合法） | sample | 采样 |
| mremap_route shrink 成功 + move 成功 | :11439/:11454（无锁） | sample | 采样 |
| debugfs 手动 | `j2_walk <pid>` 写口 → `corten_arena_j2_walk_pid` :9999（find_get_task_by_vpid + get_task_mm，evict 同骨架） | walk | 手动 |

- **采样开关**：`corten_j2_sample_key`（DEFINE_STATIC_KEY_FALSE :9969），
  `corten_audit_j2_sample{,_locked}`（:9973/:9979，static_branch_unlikely——
  关态热路径成本 = 一个 patched-out 分支）。默认只在生命周期点（mm_exit /
  fork_commit）跑；debugfs `j2_walk_every`（0200，kstrtobool）经
  `corten_arena_j2_sample_set` :9991 开关——任务书"static key 或 mm 级 flag"
  二选一，取 static key（对齐 corten_enabled_key 惯例）

### 2.3 debugfs 面（任务 3）

- `arena_stats` 四行新增（:2653-2661）：`j2_walks` / `j2_violations` /
  `j2_stale` / `j2_first_violation`（%lx，0=未见）
- 三新文件（mm/corten.c corten_debugfs_init）：`audit_gate`（0444）/ `j2_walk`
  （0200）/ `j2_walk_every`（0200）

### 2.4 J1==0 出口门一站式读取（任务 5，A.3b 移交 2）

`corten_arena_audit_gate_report`（**mm/corten_arena.c:10035**，debugfs
`corten`/`audit_gate` 0444，KUnit 经 `corten_test_render_dbg` 同渲染器驱动，
枚举臂 CORTEN_DBG_AUDIT_GATE）：

```
j1_probes          N      ← 严格口径原始值（A.3d S-5 落地前可非零，见 §6）
j1_hits            N      ← 硬不变量 1：窗口内 find 命中（合法形态 = punch 植入）
j2_walks           N
j2_violations      N      ← 硬不变量 2：INV-MV2
j2_stale           N      ← 分类披露，不进判决（登记超集是设计形状）
j2_first_violation 0x…    ← 首违例地址留档
gate_pass          0|1    = (j1_hits==0 ∧ j2_violations==0)
```

gate_pass 只编入两条硬不变量；probes/stale 印原始值——guest 判据的严格分级见 §7。

### 2.5 KUnit C 组（任务 4，mm/corten_arena_test.c:7394-7745，5 用例）

| 锚 | 位置 | 断言要点 | 结果 |
|---|---|---|---|
| inv_mv2_clean | :7404 | 纯 MODE（活跃窗+委托域 VMA）walk==0、walks +1、violations 不变；madvise hint+park 循环后仍 0；audit_gate 渲染含 j1/j2/gate_pass 全部行 | ok |
| inv_mv2_inject | :7476 | mkvm 窗内外来 VMA（绕过全部放置守卫=上游失守唯一形状）→ walk 恰 1、violations +1、首地址留档；`implant_mark` 补登记 → walk==0（白名单自证）；drop VMA 后 stale +1 非 violation | ok（WARN 恰 1 次，p4_eject 同惯例） |
| inv_mv2_stale | :7538 | mark-then-no-install（A.3b 移交谓词）：walk==0、stale +1、零 WARN；VMA 后到 → stale 停增；ENTER-only mm（无 arena）登记可用（§3.1 锚） | ok |
| inv_mv2_implant_fork | :7590 | P1b 合法植入端到端 walk==0；fork 镜像 child 登记 =1；child 树上窗内 VMA（dup_mmap 拷贝形状，mkvm 模拟）对拍 child 自身 walk==0——**镜像修复的回归锚** | ok |
| j2_triggers | :7653 | fork_commit +1 / mm_exit +1（常开）；采样默认关：park 0 / take 0；开键后 park +2（park_locked 尾+route EXACT 出口）/ take +2（reactivate 尾+take 尾）/ CHUNK munmap +1 / madvise hint +1，步进 == 断言；kunit_add_action 收键 | ok |

### 2.6 =n 折叠

新导出面 6 个（walk/walk_locked/walk_pid/sample_set/audit_gate_report + 渲染
枚举）全部 =n static inline 假值/空体（mm/corten_arena.h :866-887、
include/linux/corten_arena.h =n 段）；14 对象（mva2-verify n-objects 全集）RC=0
零警告 + nm 零 corten 符号（build-n.log）；.config 已还原 =y 并全量重建。

## 3. 伴随修复：植入登记闭环三处（本片实改，均 A.3b 残余清单第 11 条移交域）

KUnit stale 首跑红灯暴露的**真实生产缺口**，非测试形状问题：

1. **`corten_implant_mark` 无登记表即静默丢弃**（:9556 实码根因）：mode_enter 是
   无分配的（A5：登记表迟至首次 arena 工作才建），ENTER-only mm 的植入登记
   （backstop 空窗臂产品）进 void → 该合法 VMA 之后被 fault 终答
   （corten_fault_window_maperr）MAPERR + walker 记违例。修复：mark 经
   `corten_arena_get_state` 自建登记表（生产者全在 mmap_write 下，契约保持）
2. **`placement_backstop` 空窗登记臂在快否定之后**（:10702 实码根因）：登记表
   存在但零活跃/零池（全 punch 光）时 `!nr && !nr_pool → return false` 抢先返回，
   登记臂不跑——A.3b gate 修的"backstop 补登记臂"只覆盖了有 arena 的形状。
   修复：占位帧走查仅在 `nr ∨ nr_pool` 时进行，空窗登记臂无条件到达
3. **mark 裁剪对窗下界外区间会伪造条目**（防御性加固）：`start<WSTART →
   start=WSTART; end=start+len` 的旧序会把纯委托域请求改写成窗基条目（A.3a
   生产者全部窗内可达故未触发；本片 mark 调用面变宽后成为可达形状）。修复：
   窗相交前置守卫 + 真实 end 先算后裁剪（跨界区间只登记 ∩窗 部分）
4. **fork 不镜像植入登记表**（:4532）：dup_mmap 把父方植入 VMA 当普通 legacy
   VMA 拷贝进 child，child 登记表为空 → child 的 fault 终答 MAPERR（与 A.3b
   gate bug ① 同族、child 侧变体）+ fork_commit 触发点对合法 fork 永误报。
   修复：fork_begin 的 child 登记表创建条件扩到 `nr_implants>0` 并 kmemdup
   镜像（oldmm mmap_write 下父数组稳定）；implant_fork 锚回归之

## 4. 验证结果

| 项 | 命令/口径 | 结果 |
|---|---|---|
| 构建（=y） | `make -j8`；全量 + 触改文件强制重编 | RC=0；触改文件零警告（全树仅存既有基座警告 objtool cpuidle_enter_state，与 A.3a/A.3b 基线一致） |
| KUnit（三套件） | `timeout 480 qemu … kunit.filter_glob=corten*`，corten=on | **corten 24/0/1、corten_arena 85/0/0（80 既有 + 5 新）、corten_fault 31/0/2**；on3 首轮 corten 套件 interlock 单例失败（A.3a 报告登记的已知首跑 flake，本片零接触 txn 层）→ final/final2/**final3（风格修正后的最终 diff 内核）** 三轮逐位全绿，flake 判定成立。日志: results/r07/mva3c/kunit-on{1,2,3}+final{,2,3}.log |
| 内核日志签名 | mva2-verify 同款 lockdep/oops grep | 零命中；WARN 清单与 A.3b 基线逐条对拍 = 7 条既有全同 + **恰 1 条新增**（inject 用例自身的 INV-MV2 WARN_ONCE，设计内）；pgtables_bytes BUG 22 = 基线 22（mkvm 测试工件） |
| =n 折叠 | mva2-verify n-objects 14 对象 | RC=0 零警告 + nm 14 对象零 corten 符号；.config 已还原 =y 重建（build-n.log） |
| checkpatch | `--strict --no-signoff --ignore FILE_PATH_CHANGES` | **0 errors / 0 warnings / 0 checks**（1246 行 checkpatch 口径 / 1305 diff 文件行；checkpatch-mva3c.txt；四轮修正：3 尾注释换行 + 声明后空行 + 双空行 + 2 续行对齐） |
| diff 导出 | `git diff HEAD > patches/r07-mva3c.diff` | 6 文件 +999/−24；**未 commit**（HEAD 仍 51b4b09f7751，6 M） |

## 5. 自证清单（红线核对）

1. **INV6**：walker 及全部新钩子零 PTE 走查零写入——scan 是 maple 树 + 登记表
   纯读；触发点接线只加调用 ✓
2. **=n 折叠**：6 个新导出面全有 =n 假值/空体；14 对象 nm 零符号实证 ✓
3. **热路径默认零开销**：采样 static key 默认关（一个 patched-out 分支）；
   生命周期点（exit/fork_commit）全冷点，对齐 brief §6.9 ✓
4. **锁纪律**：自足形自取 ctl_lock 仅在无锁/mmap_read/mmap_write 上下文
   （DEV-13 序）；ctl 持有点全部用稳定登记形（park/reactivate 尾不重入
   ctl_lock——首版设计稿的池锁递归坑在接线前排除）；scan 的 RCU 走查不睡 ✓
5. **不动 A.3b 已验证面**：J1 五挂点/fault 终答/uffd 短路/四观测计数器零改动；
   B 组 4 用例原样全绿；本片对 corten_fault_window_maperr 零触碰（§3 修的是
   登记生产者，不是终答本身） ✓
6. **计数器 atomic_long**（对齐全屋惯例，违例路径无竞争压力）✓
7. **不 commit / 与 B 系列无交集**：6 文件全在 mm/ 与 include/linux/ ✓
8. **行号口径**：本文 file:line = 最终实码 ✓

## 6. 残留与移交

1. **A.3d（S-5）之后 j1_probes 才可进判决**：msync/madvise-on-parked 的窗口
   find_vma 走查仍计 probes（A.3b 报告 §6 #1/#2，修复在 A.3d）；故 gate_pass
   只编入 j1_hits ∧ j2_violations 两硬不变量，j1_probes 以原始值披露
2. **interval tree 升级未做**（A.3a §8 TODO）：排序数组 + 插入合并形态在本片
   全部消费形状下工作（KUnit 锚证）；条目数上界 = punch 次数，暂无压力信号，
   留后续（非本片任务书项）
3. **stale 计数是累计值**：进程生命周期内只增（munmap 不追清登记）；guest 判据
   用 violations/stale 的**差分**而非绝对值（§7 判据已按此写）
4. mm_exit 的 walk 在 mode_exit 过的 mm 上跳过（mode 门）——mode_exit 后的植入
   片是普通 legacy VA，无域可审，非缺口

## 7. J2 walker 在 guest 验收（A 系出口门）的建议判据

入口：`cat /sys/kernel/debug/corten/audit_gate`（单文件一站式；j2_* 三计数在
arena_stats 同步可见；`echo <pid> > j2_walk` 可随时手动加测）。

1. **硬判据（gate_pass == 1，双跑差分）**：
   - workload 前后 `j2_violations` 差分 == 0 ∧ `j1_hits` 差分 == 0
   - `j2_first_violation == 0x0`（全程零违例的地址级证据）
   - 复核：`echo $PID > j2_walk` 返回值 0（写入字节数=成功；非零返回 =
     -ESRCH/-EINVAL）且 violations 不增
2. **run13+churn 特有**：
   - park/take/reactivate 高频路径默认**不**产生 walk 计数（j2_walks 差分应只
     来自 fork/mm_exit——churn 里 fork 每次即 +1/child，可对拍子进程数）
   - 想压测热路径采样：`echo 1 > j2_walk_every` 跑一轮 churn 再关掉，期间
     j2_walks 快速累积但 violations 差分仍必须 == 0（热触发点上的不变量证明，
     这是采样键存在的意义）
3. **stale 语义（预期非零，勿当失败）**：churn 里有 punch+munmap 的形状会累计
   j2_stale——它是登记超集的分类披露；若要干净读数，在 workload 起点记基线取
   差分，stale>0 与 gate_pass 无关
4. **j1_probes 的分级**：gate_pass 不含 probes（A.3d 前合法非零）；A 系出口的
   严格终判（probes==0）留给 A.3d 落地后的同一文件读数——接口已备好，无需
   改 guest 脚本
5. **与 bpftrace 双口径**：audit_gate 的 j1_hits 与 kprobe `find_vma` 窗口命中
   计数应同零（brief §6.11 双口径判据不变，本片给的是计数器侧的一站式落点）
