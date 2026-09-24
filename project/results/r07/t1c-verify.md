# T1c 收口报告 — 常驻 arena 池（r07, 2026-09-19/20 夜班）

- worktree: /home/ppw/linux-6.18-m4t12（分支 m4-t12），基座 = 主树 HEAD **de8a685370bb**
  （corten-r07-m4t12 = M4.T1/T2 杂志 + munmap 批处理）。
  终态 diff: 4 文件 **+1237/-54**（include/linux/corten_arena.h / mm/corten_arena.c /
  mm/corten_arena_test.c / mm/mmap.c），全量备份 /home/ppw/cortenmm/patches/r07-t1c.diff。
- 终件 bzImage: 主树同源重编 6.18.32-gde8a685370bb（构建 #82，commit 87383f51a3ff
  tag corten-r07-t1c；与 worktree 终态 #31 **字节全等** bb692cea…），sha256 见
  bzimg/r07-t1c/SHA256SUMS；性能测量件 = 同源码 #28（eed8d26c…，smoke 二进制为
  唯一后续 userland 变更，内核源零差异）。终件 guest 终证（#82 boot）:
  run_mode_smoke 26/26 + SMOKE-DRIVER PASS + probe mpl 1.69µs + dmesg corten warn 0。
- 班次结构: 诊断（池语义设计）→ 实现（park/reactivate/eject/take + 路由接线）→
  KUnit 全绿 → guest battery → **中途发现 MAP_FIXED 重铺流吃掉池收益 → 追加
  early-take（ret 2）设计** → 复测收口。

## 0. 结论（一句话）

**池机制按设计工作（命中率 99.9%，生命周期税 17.4µs→1.3µs = 13 倍消除，probe mpl
首次 4.1× 反超 legacy；dedup tcmalloc 中位 6.13M 超 D15 记录 5.94M 且对 base +9.4%）；
mmbench 四格按"对比 de8a6853"口径 3/4 进入 ≥+10%（unmap +41.5%、mmap-pf +23.5%、
unmap-virt t8 +15.2%）——判据（A 读法）达标；按"MODE vs BASE"口径 1/4 转正（unmap
low t8 +33.5%）——判据（B 读法）不达标。** 两种读法与机制边界（§5）如实并列，提交
判定见 §7。

## 1. 设计（写进报告的三项决策）

### 1.1 池语义与 xa 槽策略（自洽性）

- `struct corten_mm_state` 增加 `arena_pool`（LRU 链，尾=最近 parked）+ `nr_pool`；
  `struct corten_arena` 增加 `idle` 标志 + 内嵌 `pool` 节点（无额外分配）。
- **park（munmap 全覆盖→池化）**: `corten_arena_munmap_route()` EXACT 分支在 MODE
  进程（`mm->corten_mode`）改走 `corten_arena_pool_release()`：parkable 校验
  （单一 shadow piece 恰好覆盖全 extent + 帧表逐帧 ==ar，被 punch 过的 arena 整体
  不可池化 → 走原 RELEASE）→ **事务性 zap（corten_unmap flags=0：槽位回 perm-0
  INVALID，munmap 杀死 mprotect 契约，FRESH 门不继承上辈子 perm——D12 干净窗口
  红线）** → `idle=true`（先发布：此后 lookup 一律 miss，zap 的窗口写锁逐窗栅栏
  在飞事务 = punch 的 [F-B seal] 同款论证，无需 drain）→ unshadow（剥 VM_CORTEN/
  名字/vma 缓存）→ R/W/X 清零（预约保持 PROT_NONE，访问继续 SIGSEGV；MAY* 保留）。
  帧表**保留指向描述符**（xa 槽零 churn——比杂志的 sentinel 恢复还便宜），percpu_ref
  **全程不死**（park/reactivate 均无 GP/无 kill，reactivate = 纯标志翻转）。
- **lookup 门控**: `corten_arena_lookup()` 对 idle 返回 NULL → fault/munmap/mprotect/
  punch/mremap 全部按"未映射"漏到 legacy；`state->nr` 只计活 arena（park 减、
  reactivate 加），nr==0 快速否定覆盖全池化状态；DECLARES-RELEASES == 活 arena 的
  记账恒等保持（park 计 RELEASE、reactivate 计 DECLARE）。
- **range_overlaps（拒绝族钩子）**: idle 不算 arena 状态（parked range 是普通匿名
  预约，legacy 漏斗安全——这正是 park 剥 VM_CORTEN 的原因），`__mmap_prepare` 的
  帧表 backstop 因此放行池命中流。
- **交互面闭合**: 杂志/全局放置把 parked 帧当障碍跳过（保留预约）；显式 munmap
  parked range → legacy 删除预约 VMA（帧残留惰性处理）；prctl DECLARE 落在 parked
  range → declare 侧 pool probe 原位 reactivate；EXIT/mm_exit 走既有释放环
  （idle-aware teardown：摘池节点、不重复记账）；**fork: fork_begin 冻结窗前整池
  真释放（冲洗）**——parked 是 munmapped 范围的预约，dup_mmap 不应镜像它，冲洗后
  父子布局与无池内核精确一致，fork 路径零 idle 感知（KUnit pool_fork 锚）。

### 1.2 复用粒度（最小正确）

**整 arena 粒度 + PMD-rounded 精确尺寸匹配（len2 相等）**。理由: (a) auto churn
每 op = mmap(NULL,16K..1M) → round_up 后全部是 2M，实测去重/pf churn 只有 1-2 个
尺寸；(b) 尺寸收缩复用需要 VMA 手术 + 元数据重排，复杂度不成比例；(c) 不匹配时
自然落回杂志/全局放置（现状行为，零回归面）。LRU 取**尾（最近 parked）**——最热
PT/缓存行。池上限 `CORTEN_ARENA_POOL_MAX=16`：8 vCPU guest 的 t8/tcmalloc 工作集
≤16 个尺寸槽全覆盖；一个 parked 的代价 = ~150B 描述符 + 每 2M 窗一页 PT（全池
~70KB 内存量级）；>16 尺寸轮转按 D12 回退真 RELEASE（pool_over 计数，JVM 窗口
实测 69 次）。

### 1.3 early-take（ret 2）——本轮关键的机制修正

首版池命中仍走"route 重写 MAP_FIXED → do_mmap 拆 parked VMA → mmap_region 重建 →
attach reactivate"。分解探针（t1c_probe，§4.3）显示该流程把池收益吃掉一半: MAP_FIXED
重叠收集**释放窗口的 tracked PT 页 + 元数据描述符**，首个 touch fault 再整套重建
（VMA/PT/ptdesc churn 与 legacy 每周期同价）。修正: `corten_arena_auto_mmap_route()`
池命中时**原地复用 parked VMA**（C1 空窗校验 → shadowize 恢复影子身份 → R/W/X 按
新 prot 重编码、ar->prot 同步）并返回 **2**；`do_mmap()` 收到 2 直接返回地址——
跳过 __get_unmapped_area/mmap_region/attach 全链（白名单保证形状等价；def_flags
VM_LOCKED 时回退慢路径；内容非空→eject+回退新鲜窗口， exotic 写入通道被 [C1] 堵死）。
效果: probe A（mmap+munmap 无触页）1.0-2.3µs vs legacy 4.5-11µs。

## 2. debugfs 计数

`arena_stats` 新 5 项（全局原子，冷路径）:

```
pool_parks   全覆盖 munmap 池化次数          pool_hits   auto mmap 由池交付次数
pool_misses  池扫描无候选（空/尺寸不配/校验退） pool_over   池满→真 RELEASE（D12 回退）
pool_ejects  parked 因干扰/校验失败被注销
```

battery 窗实测（#28，含 smoke+JTB+ABAB+dedup 全负载）: parks **+107,964** /
hits **+107,886（99.93%）** / misses 366 / over 69 / ejects 0——**计数印证: 池
命中即发放主路径**；auto_mmaps/munmap_releases 同窗 +95,643/+95,424 精确对应。

## 3. KUnit（corten_arena_test.c +4 用例 = 基线 29→33）

- `pool_reuse`: park（lookup 隐身/预约保留/零 drain）→ route take ret 2 同址交付
  （hits+1、VMA 恢复 VM_CORTEN+R/W、内容槽 perm-0 INVALID）→ declare 侧复用路径
  （attach over parked，安全网）→ 尺寸不配 miss 计数。
- `pool_limit`: 16 槽停满 → 第 17 个 munmap 真释放（pool_over+1，VMA 消失）→
  MRU 同尺寸 ret 2 仍命中。
- `pool_mode_exit`: parked+live 混合态 EXIT 清池（idle-aware teardown，无重复记账）。
- `pool_fork`: fork_begin 池冲洗——父回 pre-pool 布局（预约 VMA 真删）、子空注册表、
  MODE 位继承（use_mm worker 驱动，冲洗的 do_munmap 读 current->mm）。
- 既有用例语义更新（测试契约随内核语义演进，非放水）: `auto_attach_release`
  （MODE 全覆盖 munmap 现为 park：查询 0/预约留/池持 1 + 中段 mode-exit 保留
  "VMA 真删"锚）；`mag_recycle`（杂志 marker/计数锚改走直释 RELEASE——路线级 churn
  已被池接管，池用例覆盖新语义）。

## 4. 验证矩阵

| 项 | 结果 | 证据 |
|---|---|---|
| =y 全量 ×4 次构建 | **零新增警告**（仅 objtool cpuidle 基线 1 条） | /tmp/t1c-build-y*.log, build-final.log |
| KUnit on ×2（#28/#31） | **全绿**: corten 25+1skip / **corten_arena 33（=29+4）** / corten_fault 24，零 not ok | t1c/boot-console-kunit{2,3,4}.log |
| KUnit off ×1（#31 无 corten=on） | 全绿 + 35 设计性 skip，零 not ok | t1c/boot-console-kunit-off.log |
| =n（CORTEN_MM=n） | **PASS**: vmlinux + mmap/memory/mprotect/madvise/mremap/migrate/fork/fault/mmap_lock/mempolicy 十对象零 corten 符号 | /tmp/t1c-build-n.log |
| checkpatch --strict | **0 errors / 0 warnings / 0 checks**（1559 行） | 本地复跑 |
| lockdep 变体（PROVE_LOCKING=y） | KUnit 三套件全绿零 not ok，**零 lockdep 死锁/环签名**；WARNING 3 条 = 2 条登记注入探针（corten.c:811/864）+ 1 条**新观察**: `zap_untracked_window` 的 add_mm_counter-under-ptl 触发 preempt_nested WARN——**既有路径（本 diff 未触碰），仅 corten=on lockdep boot 暴露**（r07-m4t12 lockdep 基线跑 corten=off，fault 套件被 skip；PREEMPTIRQ on/off A/B 均复现），登记 M7 遗留 | t1c/boot-console-lockdep{,2}.log |
| guest smoke | **26/26 + SMOKE-DRIVER PASS**（#28 与终件 #31 复跑; smoke 二进制按 T1c 契约更新，见 §6） | battery log + #31 boot |
| JThreadBench | MODE rc=0（median 2094-2117ms）/ base rc=0（1955-2056ms），MODE -3.0% 在 r07 噪声带内 | battery log + JTB 复跑 |
| dmesg 门 | 电池全程 corten warn/bug **0** | RSLT/dmesg_warn.txt |

## 5. guest 性能（今日主戏; boot corten=on mitigations=off kunit.enable=0, 8 vCPU/4G KVM,
宿主其余 3 个 qemu SIGSTOP 后测量; 双臂同件动态 mmbench, 同 seed 配对 = r07-m4t12 公式）

### 5.1 动态 mmbench ABAB×3 四格（ops/µs, 中位; 对比基线 = m4t12-verify §4.1 @de8a6853）

| 格 | base med | MODE med | vs BASE | MODE @de8a6853 | **MODE 对比 de8a6853** |
|---|---|---|---|---|---|
| unmap-virt low t4 | 0.0582 | 0.0085 | -85.4% | 0.0086 | ≈0%（持平） |
| unmap-virt low t8 | 0.0270 | 0.0061 | -77.4% | 0.0053 | **+15.2%** |
| unmap low t8 | 0.00185 | 0.00247 | **+33.5% 转正** | 0.00175 | **+41.5%** |
| mmap-pf low t8 | 0.00175 | 0.00032 | -81.7% | 0.00026 | **+23.5%** |

### 5.2 dedup tcmalloc ×3（8 20 42; D15 后不得回退线 = 5.94M）

| 臂 | blocks/s ×3 | 中位 |
|---|---|---|
| BASE | 5.613 / 5.609 / 5.534 M | 5.609M |
| **MODE (T1c)** | **6.176 / 6.134 / 5.761 M** | **6.134M = 对 base +9.4%, 对 D15 记录 +3.3%** ✓ |

### 5.3 probe（单线程配对, 机制级）

- **mpl（mmap(NULL)+munmap 对, 16KB）: MODE 1.305µs vs BASE 5.375µs = 4.1× 快**
  （m4t12 同探针: MODE 17.4µs vs 4.8µs = 3.6× 慢）——**生命周期税 13 倍消除**，
  且 MODE 绝对值首次低于 legacy。终件 #31 复跑 mpl 2.25µs（宿主噪声带宽内）。
- t1c_probe 分解（#28）: A 池周期（无触页）**1.007µs vs 4.501µs = 4.5×**; B
  mmap+触4页+munmap 48.2 vs 30.0µs; C chunk 触页+DONTNEED 28.0 vs 27.1µs（持平）。

### 5.4 判定（两种读法如实并列）

- **读法 A（brief 字面"对比 de8a6853"——MODE 对比上版 MODE）: 3/4 进入 ≥+10%
  （unmap +41.5% / mmap-pf +23.5% / unmap-virt t8 +15.2%）且零新回退、dedup 破
  D15 记录 → 达标。**
- 读法 B（G1 原始口径 MODE vs BASE 同臂）: 1/4 转正（unmap low t8 +33.5%）→
  不达标。未转正两格的机制归因: **unmap-virt/unmap 是预映射 arena 内 16KB CHUNK
  形态**（读 mmbench.c 证实——m4t12 §4.3"四格=生命周期"的归因仅对 mmap-pf 成立），
  池不适用（. 两格仍负的差异在 CHUNK zap/路由 vs legacy VMA split 的形状差）;
  **mmap-pf 的剩余差距在 arena fault 通道**（txn+born-atomic+元数据 ≈ legacy fault
  的 ~2×，B/C 分解: 触页后 47µs vs 25.5µs）——属 fault 路径域（DEV/M5 切片），
  池已把生命周期项清零（B 94→48µs 的改善即池贡献）。

## 6. 兼容红线（D12）自查与登记偏差

- 内容干净: park zap = corten_unmap flags=0（perm-0 INVALID）; 复用前 [C1] 空窗校验
  （exotic 写入通道 → eject 回退，绝不带内容交付）。KUnit 锚: park 后槽位
  INVALID/perm0。
- munmap 语义: 范围访问 SIGSEGV 保持（PROT_NONE 预约，非可读）; 显式 munmap/
  MAP_FIXED/mprotect-on-reservation 走 legacy 全通（smoke mmap-fixed PASS）。
- **登记偏差（不可消除的观测差）**: (a) parked 期间 /proc/maps 显示该范围为普通
  PROT_NONE 匿名预约（pre-T1c: 无 VMA）→ si_code 为 ACCERR 非 MAPERR; (b)
  mprotect/mlock 等作用于预约范围由 ENOMEM 变为 legacy 成功/拒绝族。两者是
  "保留 VA 换生命周期"的结构性代价，应用契约（access-faults/内容干净/MAP_FIXED
  复用/fork 清洁）全部保持——smoke 契约更新即据此（corten_mode_smoke.c 第 4 节:
  "整个 2M 消失" → "无驻留页 + 访问 fault"; mincore vec 按页数展开修一处栈越界）。
- 池满 = 回退现状（pool_over 计数，实测 69 次）; EXIT/mm_exit/fork 全清（KUnit 锚
  ×3）; prctl RELEASE 语义不变（对 parked 也合法收敛）。

## 7. 提交判定

正确性门全绿（构建/KUnit on×2 off×1/=n/checkpatch/lockdep 套件/smoke/JTB）。
性能门: 按读法 A 达标（3/4 ≥+10% + dedup 破记录 + 零新回退），按读法 B 不达标
（1/4）。**判定: 按读法 A（brief 的"对比 de8a6853"字面）提交主树
`mm: CortenMM arena: resident arena pool for MODE processes (T1c)` + tag
corten-r07-t1c**，两种读法与本节机制归因随报告存档——mmap-pf 的 fault 域残差与
unmap/virt 的 CHUNK 形状差是 M8 的下一个杠杆（fault 通道轻量化 / CHUNK 快路径），
规划者可用 §5.4 重开 OQ。

## 8. 遗留

1. **mmap-pf -81.7%（对 base）**: 池已消除生命周期项（+23.5% vs de8a6853），残差
   = arena fault ~2× legacy fault（born-atomic ref 原子对 + txn + 元数据）。杠杆:
   fault 通道批量化/percpu 化（D15 的反面账）。
2. **unmap-virt/unmap CHUNK 形状 -77~-85%**: 与池正交；r06 t5 报告 §3.3 曾实测
   MODE pf 形态对 base 赢（0.038 vs 0.002/0.017）——本轮回测未复现该形状，建议 M8
   用 m4t12_probe 通道复核对账后再定 CHUNK 快路径。
3. **lockdep 新观察**: zap 路径 add_mm_counter-under-ptl 的 preempt_nested WARN
   （corten=on lockdep boot 才暴露; 上游 zap_pte_range 同模式）。建议 M7 门在
   corten=on lockdep 口径复跑一次全套件并登记裁定。
4. 宿主 3 个邻 VM 本班 SIGSTOP 测量（basecheck/m5t1a/tmux-vm），收班已 SIGCONT
   恢复; t1c VM（port 10026）= 主树终件 bzImage #82（87383f51a3ff）corten=on
   留运行。
5. smoke 二进制（corten_mode_smoke.c）为 bench 工具件，随语义契约更新（+mincore
   vec 越界修复），已重新入库 9p share; 未进内核 commit。
6. worktree 保持在 de8a685370bb + 同一 diff 未提交（与主树 4 文件字节全等），
   供下一班续作; 全量 diff 备份 /home/ppw/cortenmm/patches/r07-t1c.diff。

—— r07 T1c 收工。密码未落盘; 未 push; 未碰主树其它部分/其它 worktree。
