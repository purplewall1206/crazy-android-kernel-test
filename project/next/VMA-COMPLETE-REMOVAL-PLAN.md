# VMA 完全移除总计划 v2（2026-10-08, 系统性 WBS 版, 取代 v1 框架）

> **最终目标（与用户对齐, 三句）**
> 1. CortenMM 成为内核唯一的用户内存管理器: 所有进程的全部映射
>    （库/主 exe/堆/栈/匿名/vdso/共享）由 arena region + 元数据 + 页表服务,
>    零 VMA。
> 2. CortenMM 功能彻底完备: VMA 层曾提供的每项语义要么由 arena 等价实现,
>    要么按裁决响亮拒绝（拒绝清单显式维护）。
> 3. 单世界内核: corten=off 废止; VMA 层（mmap.c 臂/vma.c/vma.h/dup_mmap
>    VMA 走查//proc maps VMA 渲染/memory.c VMA 路径/rmap VMA 位）整层从树
>    中删除, 无任何位置使用。

## 一、现状（已达成, 不重做——全部有电池/KUnit 证据）

- 窗口域 [16T,64T) 零 VMA; =on 生产 config 全系统电池绿（LTP 与基线逐字
  一致, exec_default_enters=351, smoke 26/26, metis 精确同值）。
- arena 已有机械: fault 快/慢双路径（MAP_ANON/FILE_READ/COW/EOF/SWAPIN/
  RESTORE/零页）、exec 默认进场+镜像采纳、mmap auto/explicit/punch/admission
  路由、munmap/mprotect/madvise(WILLNEED/PAGEOUT/DONTNEED/FREE/POPULATE)/
  mremap/brk 路由、fork begin/commit/abort、exit 纯 PT 走查、swap(M6)、
  shrinker(M6.T3)、GUP W-2 臂、远程访问面(MV3.b)、mseal/uffd 响亮拒绝、
  /proc/maps 双源渲染(V-C)、bss declare(PR-0)、brk region seed/adopt(W-3)。
- E2 审计退役已落地（corten-e2-audit tag）: J1/J2 探针、白名单分类器、
  PR-2 VMA 层计数器, 净 −1,015 行。

## 二、缺口总表（全部剩余工作, 关掉=完成）

| # | 缺口 | 树上残余 | 机械工作 | 量级 |
|---|---|---|---|---|
| G1 | 非零 hint 文件映射采纳（库/主 exe 段） | wl_file ~9-70/进程 | 去窗口化+原址 declare; fault/zap/fork/exit/渲染跟随 | 大 |
| G2 | 匿名映射原址收编 | wl_anon | 同 G1 anon 半 | 中 |
| G3 | 堆初始段 | wl_brk=1 | exec seed（PR-0 机械）+brk 路由去窗口化 | 小 |
| G4 | 栈 GROWSDOWN | wl_stack | 向下扩展 fault 臂+guard gap+[stack] 渲染 | 中 |
| G5 | special 家族（vdso RX/vvar vclock RO） | wl_special | 特殊页 region 臂+arch 挂钩 | 中 |
| G6 | MAP_SHARED 写穿透 | punch 登记形 | pagecache 背书共享 region 臂（多进程写穿） | 大 |
| G7 | mlock/mlockall | VM_LOCKED 降级 | region pin 臂（M6.T3 pin 基础） | 中 |
| G8 | THP 大叶 | -EOPNOTSUPP 降级 | PMD 叶 region | 中（后置） |
| G9 | numa 平衡/migrate | 未上 | region 迁移臂 | 中（后置） |
| G10 | MODE 零 VMA 总闸 | 无 | create_vma 总闸+/proc smaps region 化+OOM 收尾 | 小 |
| G11 | =off 废止 | corten=off 世界 | 参数强制/Kconfig 单世界 | 小 |
| G12 | VMA 层删除 | mmap.c/vma.c/vma.h/mmap_lock.c/memory.c VMA 路径/dup_mmap//proc maps VMA 渲染/rmap VMA 位/tools vma | 逐文件删除+grep 零依赖门 | 大 |

## 三、PR 级工作分解（14-18 枚, 顺序即依赖序, 每枚过全门即合并推送）

### Phase V1 去窗口化 + 原址收编（G1+G2, 消灭 ~80% VMA）
- PR-V1.1: 采纳/declare 路由去窗口化（窗口双比较门→MODE 门; 原址 declare,
  wl 域地址可收编; registry xa 帧键本就全域）+ fault 快/慢路径去窗口化 +
  E3 门卫改写（域界从 [16T,64T) 变为 MODE mm 全空间）。验收: wl 域文件映射
  收编 KUnit 锚 + 电池。
- PR-V1.2: 非零 hint 文件映射打通（load_elf mmap 链 → auto/explicit 路由;
  库/主 exe 段原址收编）+ /proc/maps 原址 region 渲染。验收: **wl_file=0**。
- PR-V1.3: 匿名原址收编（线程栈/dlopen anon）+ zap/fork/exit 去窗口化收尾。
  验收: **wl_anon=0**。

### Phase V2 堆+栈（G3+G4）
- PR-V2.1: exec 堆 seed + brk 路由去窗口化。验收: **wl_brk=0**。
- PR-V2.2: 栈 GROWSDOWN region + 向下扩展 fault 臂 + guard gap。验收:
  **wl_stack=0**。

### Phase V3 special 家族（G5）
- PR-V3.1: 特殊页 region 臂（vdso RX/vvar vclock RO）+ arch 挂钩。验收:
  **wl_special=0**, vdso 回归绿。至此 MODE 进程 map_count==0（V5 总闸就位）。

### Phase V4 语义补全（G6/G7/G9）
- PR-V4.1: MAP_SHARED 文件写穿臂（pagecache 背书）。
- PR-V4.2: MAP_SHARED 匿名/shmem 臂。
- PR-V4.3: mlock/mlockall region pin 臂。
- PR-V4.4/4.5（可选后置）: THP 大叶 / numa-migrate。

### Phase V5 零 VMA 总闸（G10）
- PR-V5.1: MODE mm create_vma 总闸（WARN+拒绝）+ /proc smaps region 化 +
  OOM 走查 region 化。验收: 全系统电池长跑总闸零触发。

### Phase VI 单世界 + VMA 层删除（G11+G12）
- PR-VI.1: =off 废止（参数强制 on/Kconfig 单世界）。
- PR-VI.2..n: VMA 层逐文件删除（vma.c→mmap.c VMA 臂→vma.h→dup_mmap→
  /proc maps VMA 渲染→memory.c VMA 路径→rmap VMA 位→tools/testing/vma）。
- PR-VI.n+1: grep 零依赖门入 CI + J6 终账（VMA 层 13,408 + E1/E3 残面 +
  白名单机械全部出账）。

## 四、决策点（推荐默认已选, 异议随时推翻）
1. **单世界**: =off 废止（必须, 否则 G12 不可能）。推荐: 是。
2. **G6 共享臂**: 建成（tmpfs/POSIX IPC/共享匿名依赖）。推荐: V4 建。
3. **THP/numa**: 后置性能项, 不挡 DoD。推荐: V4 可选。
4. **pkey/hugetlb/uffd/mseal**: 维持响亮拒绝（语义闭合口径）。推荐: 永久拒绝。
5. **过渡披露**: V4 落地前 MAP_SHARED 写负载（tmpfs/IPC）在 MODE 进程不可用
   ——红字披露, 不静默降级。

## 五、风险与不可回滚点
1. **原址收编 × 内核混居假设**: wl 域 region 与零星遗留 VMA 混居期,
   free_pgtables 邻居/total_vm 记账假设需逐处核对——V1 每枚 PR 全门+
   电池, V5 总闸前必须零混居。
2. **G6 共享臂正确性**: 多进程写穿透+pagecache 一致性, 最高正确性风险
   ——独立分支+LTP shm/ipc 全家+压测后才合并。
3. **单世界不可回滚**: VI 删除后 =off 无法恢复——tag/branch 保留回滚点,
   删除前整周 soak 级电池。
4. **性能**: 全 arena 化后 wl 域高地址 region 的 xarray/shrinker 全树
   成本——M6.T3 预算机制在; V1 每枚带 mmbench 配对。

## 六、里程碑
- M1（V1 落地）: wl_file=wl_anon=0 —— MODE 进程 ~80% VMA 消失。
- M2（V2/V3 落地）: map_count==0 达成（零 VMA 进程诞生）。
- M3（V5 落地）: 总闸长开零触发。
- M4（VI 落地）: VMA 层源文件删除, 单世界, J6 终账 = 项目常驻目标达成。
