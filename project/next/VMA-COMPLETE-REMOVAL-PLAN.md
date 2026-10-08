# VMA 完全移除总计划（THE PLAN, 2026-10-08）

> 终态定义（用户指令）: CortenMM 彻底实现全部功能；VMA 功能彻底移除、任何
> 位置不再使用；VMA 代码整层从树中删除。CortenMM 成为内核唯一的用户内存
> 管理器（单世界内核, corten=off 世界终止）。
> 本文取代 vma-full-removal-roadmap.md 的分期框架, 吸收其 V1-V4 内容。

## 0. 完成判据（Definition of Done, 三条全满足才算完成）

1. **零使用**: 每个 MODE 进程（init/systemd/全部守护进程）mm->map_count==0
   且零 VMA 系统调用到达 VMA 层——总闸 BUG_ON 长开无触发（V5 总闸）。
2. **零依赖**: corten 生产代码对 find_vma/vma_lookup/vma_merge/
   lock_vma_under_rcu 的调用 = 0（grep 门入 CI 口径）。
3. **零代码**: CONFIG_CORTEN_ONLY 下 mm/vma.c、mmap.c VMA 臂、vma.h、
   dup_mmap VMA 走查、/proc maps VMA 渲染整层编译输出为零, 源文件从树删除;
   =n/=off 世界随之废止（单世界内核）。

## 1. 现状基线（已达成, 不重做）

- 窗口域 [16T,64T) 零 VMA（MV3 全链）; =on 生产 config 全系统电池绿
  （exec_default_enters 351, systemd 153+ 进程）。
- arena 机械完备面: fault 快/慢双路径、file 读/COW/EOF、swap、fork
  begin/commit/abort、exit 纯 PT 走查、mmap/munmap/mprotect/madvise/mremap/
  brk/punch 路由、GUP W-2 臂、/proc/maps 双源渲染、M6.T3 shrinker。
- E2 退役分支 pr-e2a/b/c（净 −1,015）待浸泡收口落地。

## 2. 功能缺口总表（"彻底实现所有功能"= 关掉这张表）

| # | 缺口 | 住户 | 机械工作 | 量级 |
|---|---|---|---|---|
| G1 | 非零 hint 文件映射采纳（库/主 exe 段） | wl_file ~9-70/进程 | 采纳路由地址门去窗口化; 原址 declare（无迁移）; fault/zap/fork/exit 去窗口化; /proc/maps 原址 region 渲染 | 大 |
| G2 | 匿名映射原址收编（线程栈/dlopen anon/ harness VMA） | wl_anon | 同 G1 的 anon 半 | 中 |
| G3 | 堆初始段 | wl_brk_vmas=1 | exec 时 brk seed（复用 PR-0 机械）; brk 路由去窗口化 | 小 |
| G4 | 栈 GROWSDOWN | wl_stack | 向下扩展 fault 臂 + guard gap 语义 + [stack] 渲染 | 中 |
| G5 | special 家族（vdso 可执行 + vvar/vclock 只读） | wl_special | 只读/可执行特殊页 region 臂; arch_setup_additional_pages 挂钩 | 中 |
| G6 | MAP_SHARED 写穿透 | punch 登记形 | 共享缓存臂（pagecache 背书 region, 多进程写穿） | 大 |
| G7 | THP | huge leaf 现降级 | PMD 叶 region（性能项, 可后置） | 中 |
| G8 | pkey/uffd/mseal/hugetlb | 响亮拒绝（现状） | 维持拒绝=语义闭合（不需要臂） | 零 |
| G9 | numa 平衡/迁移 | 未上 | region 迁移臂（M6.T3 延伸, 可后置） | 中 |

## 3. 分期执行（每组可独立验证, 顺序即依赖序）

### Phase V1 白名单域原址收编（G1+G2, 消灭 ~80% VMA）
机械: (a) declare/route 的窗口双比较门 → "MODE 即收编", 原址 declare（无
placement）; (b) fault 快/慢路径去窗口化（xarray 帧键本就地址无关）;
(c) zap/fork 镜像/exit 走查去窗口化; (d) /proc/maps 原址 region 渲染;
(e) E3 门卫改写为白名单域门卫（域界从 [16T,64T) 变为 "MODE mm 全空间"）。
DoD: wl_file=0 且 wl_anon=0（audit gate 快照）, 电池三腿绿, KUnit 全绿,
/proc/maps 与 region 双源零差异。估 3-5 枚 PR。

### Phase V2 堆+栈（G3+G4）
exec 时堆 seed + brk 路由去窗口化; 栈 region + 向下扩展 fault 臂。
DoD: wl_brk=0, wl_stack=0。估 2 枚。

### Phase V3 special 家族（G5）
vdso 可执行 region + vvar/vclock 只读 region（页预设, 无分配）。
DoD: wl_special=0, vdso 功能回归（gettimeofday/auxv）绿。估 1-2 枚。

### Phase V4 收敛（G7 决策后置 + G9 后置）
THP/numa 按需; E3 门卫逐个改不可达断言。

### Phase V5 MODE 零 VMA 总闸（DoD#1）
create_vma 总闸: MODE mm 任何 VMA 创建即 WARN+拒绝; 电池长跑零触发。
估 1 枚。

### Phase VI VMA 层移除（DoD#2/#3, 单世界裁决生效）
(a) G6 MAP_SHARED 共享写臂或其响亮拒绝的最终裁决; (b) CONFIG_CORTEN_ONLY:
VMA 层编译输出为零 → 源文件删除; (c) corten=off 废止; (d) J6 终账:
VMA 层 13,408 + E1/E3 残面 + 白名单机械全部出账; (e) grep 零依赖门。
估 3-6 枚（含清理）。

## 4. 决策点（推荐默认已选, 异议随时推翻）
1. **单世界**: corten=off 废止（推荐, 否则 VMA 代码不可整层删除）。
2. **MAP_SHARED 写**: 建共享臂（V4 前完成, tmpfs/POSIX IPC 依赖它）。
3. **THP/numa**: 后置性能项, 不挡 DoD。
4. **窗口概念**: V1 起溶解（全地址空间 arena 域）。

## 5. 顺序与在制关系
浸泡（=on 正确性前置）→ E2 A/B/C 落地（审计死重出清）→ V1 → V2 → V3 →
V4/V5 → VI。V1 起草可与浸泡并行（分支 pr-v1）。
