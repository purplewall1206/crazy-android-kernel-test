# M-V A.3a 开发报告：放置面热修（J2 审计 #14-#17 收口）

- 产出: A.3a 开发 agent（2026-09-22）
- worktree: `/home/ppw/linux-6.18-mva` @ 分支 `mv-a0`，基座 = 主树 HEAD **217a9922a7d3**
  （A.2a/A.2b 已提交）；本片为未提交增量，**未 commit**（红线遵守）
- 任务书: `next/va3-dev-brief.md` §2.1-2.4/2.6/2.9（前半）+ §4 A.3a 行 + §5 A 组；
  缺陷机理: `next/j2-audit-draft.md` #14/#15/#16/#17 + P1-P4 守卫段
- 补丁: `/home/ppw/cortenmm/patches/r07-mva3a.diff`
- 行号口径: 本文 file:line = **A.3a 增量后的 worktree 实码**（brief 的行号基于
  d40eae5+A.2 未提交快照，落地时按函数名+锚注释重定位，符合 brief §6.10 约定）

## 1. 改动总览

| 文件 | 增/删 | 内容 |
|---|---|---|
| mm/corten_arena.c | +459/−28 | occupied_incl_idle + placement_backstop + 植入登记 + P1b idle-eject 臂 + P4 断言 + 计数器/测试钩子 + eject obs 修复 |
| mm/corten_arena_test.c | +501/−2 | 6 个 KUnit 锚 + 2 处既有测试的 V-A.2a 形状对齐（见 §5） |
| mm/corten_arena.h | +67 | 三组 API 声明 + =n static inline 折叠 |
| include/linux/corten_arena.h | +40 | `struct corten_implant_range` + `corten_mm_state` 登记字段 + KUnit 钩子声明 |
| mm/mmap.c | +9/−2 | NOREPLACE 守卫换弹药（一行调用 + 注释） |
| mm/vma.c | +20 | `__mmap_prepare` 零 VMA backstop 分支 |
| **合计** | **+1068/−30** | 内核 ~+597/−28，测试 +501/−2 |

超出 brief §4 预算（~+260/−40 内核 / +140 测试）的主因：注释密度对齐本屋惯例
（新增函数全部带设计论证注释，本文件族注释:代码 ≈ 6:4），以及两项必要的
伴随修复（§4 的 obs_remove 与 §5 的既有测试对齐）。功能面严格未越 A.3a 界。

## 2. 逐项落点（file:line）

### 2.1 帧表"含 idle"查询 API（P1 地基）

- `corten_arena_range_occupied_incl_idle(mm, start, len)` —
  **mm/corten_arena.c:9258**（紧挨 range_overlaps 之后，实现骨架照抄）
  - fast-negation 保留 `!refcount_read(&state->nr) && !READ_ONCE(state->nr_pool)`
    双条件（全池 parked 不快否定）
  - 循环体**去掉 idle/sentinel 的 continue**；哨兵槽键控（自身 start/end 无意义），
    迭代索引命中即占用
  - RCU 走查、无 ref（只读 start/end/idle，kfree_rcu 免疫）；调用面全在 mmap_write
  - `range_overlaps`（:9165 起）**零改动**——16+ 拒族钩子的 skip-idle 语义保持
- 导出: mm/corten_arena.h（=y 声明 + =n `static inline return false`，对齐 :732 惯例）

### 2.2 P1a: NOREPLACE 守卫换弹药

- **mm/mmap.c:484** — `corten_arena_range_overlaps` → `corten_arena_range_occupied_incl_idle`
  （一行调用替换 + 注释更新：parked 窗与 reserve 哨兵均计占用；-EEXIST 契约字面，
  与预约 VMA 时代零语义偏移）
- 锁上下文: do_mmap 全程 mmap_write ✓

### 2.3 P1b: plain MAP_FIXED over parked → idle-eject 放行（D24）

- `corten_arena_placement_punch_idle(mm, addr, len)` — **mm/corten_arena.c:8112**
  （punch_route 之前）
- 挂点: `corten_arena_mmap_route` 头部 **:8292-8299**（enable 门之后、classify 分派
  之前）——同时覆盖 MARK 腿的 `!ar → return 0` 早退与 punch 腿的双 NULL 退出口
  （两腿的 lookup_get 都跳 idle）；门 = `(flags & MAP_FIXED) && !NOREPLACE`
- 语义: 对 [addr,addr+len) 内每个 idle arena 整体 eject（部分覆盖同 eject——
  parkable 不变量要求整片）；返回 0 = 清场完成，放行 legacy 装 VMA
  （= 合法植入形态，零新错误类，strace 无新错误面）
- **对 brief 草图的两处实码修正**（brief §2.3 落地以 worktree 实码为准）:
  1. eject 用 **erase 变体**（`pool_eject_locked(state, ar, false)`），不是缺省的
     marker 回写：被替换帧即将被外来 VMA 覆盖，回写哨兵会让 (a) 零 VMA backstop
     把刚放行的 mmap 又拒掉、(b) 杂志 recycle 名单在 VMA 之下再发牌。erase 对齐
     punch 先例（marker 丢失 → 杂志跳过、计数、帧泄漏）。`pool_eject_locked`
     增加 `bool recycle` 参数（:6958），3 个既有调用点 `recycle=true` 语义不变
  2. reserve 哨兵帧: **不动也不拒**（brief 草图 "WARN+-EOPNOTSUPP" 不取）。
     实码依据: `corten_va_mag_alloc_cpu()` 的 T0 obstacle 契约（每次发牌重验
     marker+树, bump 臂 :2873 / recycle 臂 :2813）已把"显式地址映射压哨兵帧"
     列为合法形状；在此拒绝反而制造 D24 要避免的新错误类。注释引用该契约
- 植入登记生产者之一: eject 后 `corten_implant_mark(mm, addr, len)`（:8142）

### 2.4 P3: `__mmap_prepare` backstop 零 VMA 分支

- **mm/vma.c:2486-2489** — `vms->vma = vma_find(...)` / `if (vms->vma)` 之间
  （brief 指定位置），`#ifdef CONFIG_CORTEN_MM_ARENA` 包裹
- 谓词走 `corten_arena_placement_backstop()`（**mm/corten_arena.c:9312**）:
  live+idle 帧在册即真 + `corten_nr_placement_backstop++`，**不计哨兵**——
  哨兵压 VMA 是 T0 obstacle 契约的合法形状，计入会把 plain-MAP_FIXED-压-
  已认领杂志段的合法 mmap 误拒（P1b erase 变体与之配套，自洽）
- 返回 -EOPNOTSUPP（与同函数既有 backstop 同码）；正常不可达，"响不应答",
  计数披露。既有 `if (vms->vma)` 内的 range_overlaps backstop 原样保留

### 2.5 P4: reactivate/pool_take 防御断言

- `corten_arena_pool_reactivate` 头部（**mm/corten_arena.c:7020-7046**），
  idle 翻转之前，`novma` 门控:
  - **auto 形态**（pool_take 与 DECLARE 侧 novma 探测）: `corten_vma_find` 命中
    即 WARN_ONCE + `corten_nr_p4_ejects++` + `pool_eject_locked(recycle=true)` +
    返回 -EAGAIN——绝不把帧发到别人的 VMA 之下
  - **targeted DECLARE（novma==false）豁免**: 声明 VMA 合法覆盖窗口
    （declare_locked 随后收养并校验它）——该豁免由既有测试
    fork_redeclare_content 的合法流程反证得出（初版无门控时误伤）
  - 调用方翻译: pool_take 的 -EAGAIN 对 auto route 即普通 miss（计数落
    pool_misses, fresh 重选窗）; pool_prepare 的 -EAGAIN → return 1（fresh
    declare 路径）——对上层零新错误面
- `corten_vma_find` = mm 内部别名（mm/mmap.c:1026，不触发 J1 probe）✓

### 2.6 植入登记（D24, per-mm）

- 数据结构: `struct corten_implant_range` + `corten_mm_state.implants/
  nr_implants/nr_implants_alloc`（**include/linux/corten_arena.h:329-348/422-431**）
  ——**按需排序数组**（brief 允许的简化形态；TODO(V-A.3c) 换 interval tree,
  注释写死在字段处）
- `corten_implant_mark(mm, start, len)` — **mm/corten_arena.c:7981**:
  窗口域裁剪、ctl_lock 内（DEV-13 序）有序插入+相邻/重叠合并（krealloc 倍增,
  失败计数 `corten_nr_implant_drops` 降级不登记）
- `corten_implant_covers(mm, start, len)` — **:8062**: 单遍前向扫描，
  "VMA ⊆ ∪登记区间" 谓词（A.3a 消费者 = KUnit 锚；A.3c J2 walker 接手）
- 生产者封闭两处（brief §2.9 前半）: P1b 放行臂（:8142）+ punch_route 两成功臂
  （EXACT :8220, CHUNK :8235）；EXACT 臂范围先捕获再 release（防 use-after-free）
- 生命周期: state_free 随 state 释放（:435 kfree(state->implants)）；munmap 不
  追清（陈旧条目只造成超集白名单，注释披露，A.3c 复议）

### 2.7 计数器与测试钩子

- 新增 `static atomic_long_t`: `corten_nr_placement_backstop` /
  `corten_nr_p4_ejects` / `corten_nr_placement_idle_ejects` /
  `corten_nr_implant_drops`（mm/corten_arena.c:296-317）
- KUnit 钩子: `corten_arena_test_placement_backstop/_p4_ejects/
  _placement_idle_ejects/_implant_nr`（arena.c:2312-2340, 声明在
  include/linux/corten_arena.h:697-703）

## 3. 伴随修复（本片暴露的既有缺陷，非范围蔓延）

### 3.1 eject 漏摘观察账本（use-after-free, 潜在）

`corten_arena_pool_eject_locked`（T1c 起）free 描述符前**从不调用
`corten_arena_obs_remove`**（obs_remove 注释自己的契约: "Called at
deregistration"），被 free 的描述符滞留 `corten_arena_list`，debugfs arenas
walk 走查已回收内存。既有 eject 流（pool_limit LRU 逐出等）已制造该形状，
只是 slab 未复用前"侥幸"不炸；本片的 P1b/P4 eject 使其变为热点——KUnit 实测
`corten_arena_stats_report` GPF（非规范地址）/ seq_file 挂死。修复:
**mm/corten_arena.c:6991** eject 路径补 obs_remove（+11 行, 与 release 路径
:1597 同序: ctl_lock 内取 list spinlock）。

### 3.2 A.2 夜验门未覆盖 corten_arena 套件（流程发现）

`mva2-verify.sh run_kunit` 的 `kunit.filter_glob=corten` 只匹配字面 `corten`
套件（mm/corten_test.c）；**corten_arena（61 用例）与 corten_fault 从未进过
A.2 夜验门**（r07/mva2/*.log 复核证实）。本片验证改用 `filter_glob=corten*`
三套件全跑；建议主 agent 修正 mva2-verify.sh 的 grep/glob（其
`grep -q "# Subtest: corten"` 因子串匹配掩盖了 glob 缺陷）。

## 4. 既有测试的两处处置（必要且最小）

1. **corten_arena_test_fork_redeclare_content**（test.c:5971 附近）: 在**纯净
   A.2 基座内核**（bzImage-mva2-y-fix）上即失败（-EEXIST，本片前置诊断
   kunit-base-arena.log 实证: pass:60 fail:1）。根因是**测试自身缺陷**——
   "Park: the EXACT munmap route" 注释意图停泊整窗，实际传 PAGE_SIZE
   （CHUNK zap, 窗口仍 live）→ 重声明撞在册帧。该测试 A.2 落地后从未被门
   覆盖（§3.2）故未被发现。处置: munmap 长度 PAGE_SIZE → CORTEN_ARENA_TEST_LEN
   （落实测试自己写明的意图, 1 行）
2. **corten_arena_test_region_park**（test.c:6641 附近）: 手工 mkvm 的树 VMA +
   auto_attach + park 组合使 VMA 残留在 parked 窗内（auto_attach 的
   ar->vma==NULL → park 的无 VMA 分支不拆它）——正是 P4 要逐出的"parked 窗内
   外来 VMA"不可能形状（生产路径: mmap(FIXED) 压 parked 先被 P1b eject）。
   处置: 删去 vestigial mkvm（对齐 fork_vma_free 在 A.2a 的同类清理
   "the mkvm the pre-A.2 shape needed is gone"）, 测试断言集不变

两处均为测试形状修复、零产品代码迁就；未动 classify/file 区域，与 V-B
worktree（B.1）无文件交集（本片只触上表 6 文件, 均在 mm/ 与 include/linux/）。

## 5. KUnit 锚（mm/corten_arena_test.c:4847-5339, +6 用例）

| 锚 | 位置 | 断言要点 | 结果 |
|---|---|---|---|
| noreplace_parked | :4925 | 窗内 auto arena park 后 NOREPLACE 整窗 mmap → **-EEXIST**; 树无 VMA; occupied_incl_idle==true ∧ range_overlaps==false（语义分叉锚）; 窗仍 parked | ok |
| noreplace_active | :4975 | 活跃窗（A.2 后无 VMA）NOREPLACE → -EEXIST（A.2 守卫回归锚, 仅凭注册表作答） | ok |
| mapfixed_over_parked | :5011 | 窗内 parked 窗 plain MAP_FIXED → **返回原址**（零新错误类）; VMA 在树; **occupied==false**（erase-eject 不回写哨兵）; implant covers==true; 池空 + idle_ejects+1; 同尺寸 pool_take → 计数 miss + fresh 重选他窗 | ok |
| occupied_incl_idle | :5102 | 镜像矩阵: 洞/活/parked × 内/边界/跨界/整扫/零长; parked 轴与 overlaps 取反; 哨兵轴（真杂志 seg claim 的未分配尾段）: occupied==true ∧ overlaps==false ∧ **backstop==false**（哨兵豁免锚, 计数不增）; backstop 对活帧计数 +1 | ok |
| p4_eject | :5232 | 手工注入外来 VMA（vma_link 直挂, 绕过全部放置守卫=只有上游守卫失守才能产生的形状）→ 同尺寸 auto 请求: **P4 WARN_ONCE+计数 +1**、槽被逐、pool_take 降级 miss、fresh 落他窗 | ok |
| hint_fence | :5289 | 真 do_mmap 漏斗端到端: MODE mm 窗内 hint → 非 FAST 接受, fenced walker 重放到窗外（r ∉ [16T,64T) 且 VMA 落在返回址）; 非 MODE mm 同形 hint 原样接受（双门零扰动锚） | ok |

辅助件: `corten_arena_test_op_do_mmap`（op worker 内持写锁直调 do_mmap,
绕开 LSM 依赖, 记录原始返回值）、`occupied_incl_idle` 行长短名别名
（checkpatch 对齐）、`corten_arena_test_park_targeted`（低域 targeted
declare→park 助手）。

backstop 零 VMA 分支的 vma.c 侧: 分支体即 `!vms->vma && placement_backstop()`,
其谓词+计数在 occupied_incl_idle 锚中直测；分支自身设计为正常不可达
（P1a/P1b 在上游终答）, 报告如实披露口径。

## 6. 验证结果

| 项 | 命令/口径 | 结果 |
|---|---|---|
| 构建 | `make -j8`（=y） | RC=0; 改动文件零警告（全树仅存 2 条与 mm/ 无关的既有警告: objtool cpuidle_enter_state / modpost memblock_end_of_DRAM, 与基座一致） |
| KUnit（全量） | `timeout 480 qemu-system-x86_64 -enable-kvm -m 2048 -smp 4 -kernel arch/x86/boot/bzImage -append "console=ttyS0 panic=-1 corten=on kunit.filter_glob=corten*" -nographic -no-reboot` | **corten 24 pass/0 fail/1 skip**（skip=corten=on 布局的既有 layout 用例）; **corten_arena 67 pass/0 fail/0 skip**（61 既有 + 6 新）; **corten_fault 30 pass/0 fail/2 skip**（与基座逐位一致）。日志: results/r07/mva3a/kunit-on4/-on5/-final1.log（两次复跑全绿, 无 flake; 无 interlock 单例问题）; P4 WARN 恰 1 次（其自身用例） |
| KUnit（任务原样命令） | 同上但 `kunit.filter_glob=corten` | `# Subtest: corten` + `# corten: pass:24 fail:0 skip:1`, 0 not-ok（kunit-final2.log）。注意: 该 glob 不含 arena/fault 套件（§3.2）, 全量结论以上一行为准 |
| 内核日志签名 | lockdep/oops grep（mva2-verify.sh 同款） | 零命中（recursive locking/deadlock/unsafe locking/bad unlock/DEBUG_LOCKS_WARN/BUG/GPF）; "BUG: non-zero pgtables_bytes on freeing mm" 为基座同款 mkvm 测试具工件（base-arena.log 同现）, 非本片引入 |
| =n 折叠 | 15 对象（mva2-verify.sh n-objects 的 14 + 本片新触的 mm/vma.o）: `./scripts/config -d CORTEN_MM ... && make $OBJ` | RC=0, 0 警告; **nm 15 对象零 corten 符号**（build-n.log）; .config 已恢复 =y 并重建 |
| checkpatch | `./scripts/checkpatch.pl --strict --no-signoff --ignore FILE_PATH_CHANGES r07-mva3a.diff` | **0 errors / 0 warnings / 0 checks**（1304 行; checkpatch-mva3a.txt） |
| diff 导出 | `git diff > /home/ppw/cortenmm/patches/r07-mva3a.diff` | 6 文件 +1068/−30; **未 commit**（git status 全 M） |

## 7. 自证清单（红线核对）

1. **INV6（arena PTE 写必经事务）**: 本片零新增 PTE 写路径——idle-eject 走
   `pool_eject_locked`（park 后窗内无 PT 页, 无 PTE 可写）; backstop/P4/NOREPLACE
   均为纯注册表读+拒绝。唯一改写既有写路径的是 punch_route 的 implant_mark
   （登记表写, 非 PTE）✓
2. **=n 折叠完整**: 新导出面 5 个（occupied_incl_idle / placement_backstop /
   implant_mark / implant_covers + eject 参数）全部有 =n static inline 假值
   或 #ifdef 包裹; vma.c 分支 #ifdef; mmap.c 调用经 inline 假值折叠;
   15 对象 nm 零符号实证 ✓
3. **legacy 零扰动**: 全部新钩子双门（static-branch + corten_state/mode）+
   窗口/形状域判定; NOREPLACE 守卫只换探测函数不改门结构; hint 守卫为 A.2
   既有（本片只加测试锚）; 非 MODE 进程只付既有静态分支读 ✓
4. **不动 range_overlaps 语义**: 函数体零改动, 16+ 拒族调用面零改动;
   新语义只存在于新函数 ✓
5. **锁序**: 全部新写点 mmap_write → ctl_lock（DEV-13 既有边, 无新边）;
   P4 的 WARN 在 ctl_lock 内（与既有 eject 内 WARN_ON_ONCE 同形）✓
6. **计数器 atomic_long**（不 per-cpu）: 4 个新计数器全部 static atomic_long_t,
   违例路径专用、无竞争压力, 对齐全屋 60+ corten_nr_* 惯例 ✓
7. **不 commit**: worktree 仅工作区修改 ✓
8. **并行轨无交集**: 只触 mm/{corten_arena.c,corten_arena.h,corten_arena_test.c,
   mmap.c,vma.c} + include/linux/corten_arena.h; 未动 classify/file/V-B 区域 ✓

## 8. 遗留与移交

- **D24 两处实码裁决偏移 brief 草图**（erase-eject、哨兵不拒, §2.3）: 依据为
  worktree 实码（杂志 T0 obstacle 契约 + backstop 自洽性）, 提请主 agent 追认
- **auto_attach 失败降级 VMA 未登记植入**（mmap.c:463 既有路径, 窗内无主 VMA）:
  brief 明确将生产者集合封闭于两 punch 臂; 该形状留给 A.3c walker 的
  attach-failure 登记口径复议
- **implant 登记为排序数组**: TODO(V-A.3c) interval tree（字段处注释写死）;
  covers() 谓词已按 A.3c 消费形状实现（锁契约 mmap_read+）
- **建议修 mva2-verify.sh**: filter_glob=corten → corten*（§3.2）, 否则
  A.3b/A.3c 的夜验继续漏掉 arena/fault 套件
- **P4 的 targeted 豁免**: 生产不可达性论证（mmap(FIXED) 压 parked 先被 P1b
  eject）写在 :7028 注释; A.3c walker 上线后可加 debugfs 触发的全树对拍
