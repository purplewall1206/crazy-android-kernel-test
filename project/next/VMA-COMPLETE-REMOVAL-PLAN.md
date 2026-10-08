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

## 7. V1 执行状态（2026-10-08 深夜, 调试中）

- **已落地机械**: 原址臂（hint 页粒度收编, exec 判别器 + 窗口外限定）、
  fault fallback/maperr MODE 门、madvise/mincore/msync 路由门
  （lookup/occupancy 判定, parked 窗口语义保持）、move_pages 回退（V2/V3
  前 wl VMA 不被接管）、GUP/remote 门、bss/implant 去窗口。
- **登记的回归**: =on 全系统 boot 非确定性失败（boot4 = init -14 ×2;
  boot5 = init 更早 SIGSEGV）——**布局依赖型**: 每次 boot 哪些 hint 映射
  原址收编/哪些重定位不同 → 不同 region 布局 → 不同崩法。已排除: exec
  判别器方向（修正后仍崩）、窗口门（修正后仍崩）。
- **下一步调试序列**: (1) exec 链全程插桩（elf_map 每段打印收编/迁移/
  原址决策 + region 布局转储）→ 定位布局敏感交互; (2) 检查 wl region 与
  magazine 池帧的地址干扰（wl 地址与窗口帧的 xarray 帧键冲突?）; (3)
  load_elf 的 padzero/ELF_READ 对 wl region 页的访问面。
- **工作树**: pr-v1 = 1d2953a + 插桩（load_elf 失败打印）; 稳定 =on 链 =
  android17-6.18 @ 909bccf（boot 绿验证于今晨电池）。

## 7b. 调试进展补充（2026-10-08 23:15）

插桩轮结果: place ✓ 成功（段已收编）、file_attach/auto_attach 零失败、
padzero 零失败 → **load_elf_binary 的 -14 发生在晚段**（interp 装载 /
create_elf_tables auxv / start_thread 之间）。插桩已就位（load_elf 失败
打印 + padzero 失败打印 + attach 失败打印 + PLACE/入口决策打印）。
下轮: 晚段三点的逐点插桩（interp map / create_elf_tables /
ELF_PLAT_INIT）+ wl region 布局转储。非确定性与 wl 收编布局相关
（boot4 vs boot5 崩点不同）。

## 7c. 深夜推进（00:15）: exec 已深入至 wl 采纳策略洞

修复 maperr 窗口域回退（栈 GROWSDOWN 扩展 fault 的 -14 根因）后, exec 前进
到晚段 OK（entry/interp 装载完成, 第二次 placement 正确推进到 window+2M）,
然后暴露**新洞**: ld.so 的一个 hint 映射（wl 域, 紧贴窗口下边界）走原址臂
→ 占用检查（find_vma + incl-idle）判空闲 → 原址 declare → **[C1] 内容探针
-EBUSY**（该范围有 PTE/元数据内容而无 VMA 覆盖——先前映射的残余或栈页）→
整个 mmap 以 -EBUSY 失败（**未重定位**——get_unmapped_area 已按无 VMA 尊重
了 hint）。init 三连 -16 → panic。

**V1 完成的最后一块**: 原址臂的占用检查升级为 [C1] 等价（PTE+元数据+注册
表三面), 或采纳前预跑 [C1]、拒时回退重定位语义。已登记, 明日首项。

## 7d. 深夜二轮（01:20）: 失败点推进至 ld.so 的段映射

布局转储轮: init 的 exec 本体成功（ld.so 已启动运行!）, 失败点推进到
**ld.so 自己的段映射**: ld.so 的 mmap（MAP_FIXED 文件段, wl 域地址）收到
-EBUSY → ld.so 报 "cannot map segment" → exit 127 → init 死。C1 内容探针
打印未触发 → -EBUSY 来自采纳链更早的占用/状态检查点（范围已收窄至
explicit admission → pool_prepare/validate 链）。下轮: -EBUSY 源头 Hunt
（admission 链逐点插桩）+ 与 wl 原址采纳的语义对齐（[C1] 等价预检或
EBUSY→重定位降级）。

## 7e. 深夜三轮总结（03:00）: 失败面完全定性, 机制待收敛

连续 8 轮 boot 调试的收敛结果:
- **确定性失败**: 每次 =on exec 都在 ld.so 的 libc/段映射链上失败
  （-EBUSY → ld.so "cannot map segment" → exit 127）。非随机。
- **失败链**: ld.so 的 hint 映射 → 原址臂占用检查通过（无 VMA/无
  arena-registry 冲突）→ 原址 declare → **[C1] PTE 内容探针 -EBUSY**
  （发现 [16T-0xFD000, 16T-0x90000) 有 present PTE——先前原址收编段的
  页）→ mmap 整体失败（未重定位）→ ld.so 放弃 → init 死。
- **已修的中间层**: maperr 去窗口化的栈扩展 SIGSEGV（真根因, 已修）、
  exec 判别器、窗口外限定。
- **结构性发现**: wl 域原址收编的段**页粒度共存**需要占用判定的完整
  语义（VMA + registry + **PTE 内容**三面合一）; 现行 find_vma +
  incl-idle 检查漏第三面。C1 等价预检已加但与 ld.so 的 hint 序列
  仍有交互未收敛（round5-8 的失败面在多个锚间漂移）。
- **明日首项**: (1) 原址臂的 C1 预检结果改为"跳过该 hint 回退重定位"
  已实现但仍红——需 dump 失败时 [16T-1M,16T) 的完整 region+PTE 布局
  定位残余交互; (2) 或评估原址臂改为"hint 范围 C1 拒 → 整段迁移到
  窗口 placement"的混合语义。
- 仓库: pr-v1 = d16e13d5→(本轮) 全部插桩与修复在案; 稳定链 909bccf
  =on boot 绿（今晨电池）。
