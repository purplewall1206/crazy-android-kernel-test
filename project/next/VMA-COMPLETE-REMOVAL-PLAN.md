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

## 7c. V1 收敛落点 + V2 执行状态（2026-10-09 晨）

**V1 终态**（a8392b7, 电池绿 + 双分支已推送）: 全窗布局 —— :482 门从
addr==0 加宽为一切非 MAP_FIXED 形状（hint 即弃, arena 定址入窗）;
fence 重设计（legacy 域全部置于窗口上方 [64T,128T), loader/栈碰撞带
消除）; C1 元数据感知（present+INVALID 元数据 = 卸载残孤儿, 非内容）。
原址收编的 hint 臂（计划 PR-V1.2 的"非零 hint 文件映射打通"）被全窗
布局取代: hinted 映射直接成窗内 region, 比原址收编更强。9 轮调试
（P1 损毁发现 + 布局转储）= pr-v1 分支的完整输入档案。

**V2 落地**（4f7b219 + 5aa8549）:
- 普查仪表: brk_funnel（生长时漏斗, wl_brk 验收分母）+ 既有
  sweep_skip_stack/special/shared/exec_time 桶（wl_stack/wl_special
  分母）。=n 门 PASS（vmlinux+十对象零符号）。
- 栈臂 V2.2: 转换+扩展机械完整（转换骑 sweep 匿名臂的
  pick/手术/finish 序列; 扩展镜像 expand_downwards 合法性全集;
  发布序 = 先插帧后降 start）。KUnit 207/0/9（taxonomy 锚换新语义）。
  **默认关**（corten_stack_extend=on 选择进入）—— r08/r09 boot 登记
  三个设计输入: (1) find_vma 在 rcu pre-check 的 mmap 锁断言（ud2,
  修 = lock_vma_under_rcu + USER 门）; (2) 早退丢失时每 fault 抢
  mmap_write → fork COW 风暴串行化 → sshd banner 超时（修 = 早退 +
  无候选即返）; (3) 转换与 lookup 竞态的 NULL 解引用
  （user_fault+0x4ff, 待 pieces/ownership 全程走查）。门控后 boot/ssh
  全绿（=on 行为与 a8392b7 逐字节一致）。
- exec-default 流不跑 sweep（enter ≠ enter_sweep）→ 栈 VMA 在默认
  世界终宿 = fault 路径惰性转换臂（已建, 待上述走查后默认开）。
  enter_sweep（prctl 世界）的 sweep 栈臂已落地（委托匿名臂）。
- **brk 实测**: brk_funnel=53/183 exec（boot 读数）→ 非零: 堆生长
  仍有漏斗形状, V2.1 的 wl_brk=0 需 bss declare 路由的去窗口化
  （现窗域门控, 全窗布局下堆在窗内应已命中 —— 53 次漏斗 = 命中后
  的 degrade 形状, 下轮读 degrade 原因桶）。

**V2.1 + V3 落点（2026-10-09 晨, f7408e7 + eb68326）**:
- **V2.1 wl_brk=0 达成**: brk_funnel 49-54/boot 的根因 = 全窗布局把
  exec 影像（连带堆）放在经典域, 而 brk declare 路由的调用门与内部
  门双重窗域门控 → 恒走漏斗。双门去窗口化（路由 MODE 门 + C1 守卫
  = 安全边界, 与去窗准入路由同形）。boot: brk_funnel 49→0,
  bss_legacy=0（零 degrade）。
- **V3 wl_special=0（census）达成**: vdso/vvar 家族在 arch 装载点
  影子收编（corten_arena_special_shadow, map_vdso 持 mmap_write）:
  记录拿 range（census+路由）, VMA 留树作 arch fault 载体, fault 经
  CORTEN_RF_SPECIAL_SHADOW 的 tier-1 门跳 tier-3 放行给特殊 .fault。
  vclock VMA 保持原形（VM_PFNMAP 无 struct page）。sweep 分类器:
  精确跨距的 VMA = ours（部分覆盖仍走诚实 -EEXIST 桶, W-7 契约）。
  boot: special_shadows=352=176 exec×2, sweep_skip_special=0。
- **栈臂 r10 NULL 解引用根因修复**（19f646a）: 反汇编对齐
  （[ar+0xb0]&0x40 = GROWSDOWN 位测试）→ 缺失的无记录 bail
  （!user miss + convert=false 直达 start 读）。同轮修复: 双转换点
  的 freed-VMA 读（adopt 成功即释放 VMA）、探测换 mas_walk+mas_next
  （lock_vma_under_rcu 只答覆盖范围, 零转换）、内核态 miss 提前返
  （不进写锁）。**首次硬件成功转换**（dhcpcd adopt+grow 存活;
  legacy 形 fork-exec 测试过）。
- **栈臂默认开前剩两线**: (a) 探测非普适触发（python 6000 帧增长
  未触发转换, adopts 保持 1）; (b) 转换后 dhcpcd 的 DHCP 腿死
  （eth0 无地址 = fork-mirror × 手术迁移锚交互, 已文档化）。

**栈臂两线进展（2026-10-09 午, 779d610 + d4a1ee8）**:
- **线 1（探测非普适触发）闭合**: 根因 = below-start 触发器在真实
  boot 从不发生（增长的构成 = exec 的内核态 copy_strings 的 !user
  扩展 + 已扩展 VMA 内的惰性页 fault）→ 触发器改为"最近 VMA 即
  栈则收编"（首内页 fault 即转换）; 探测修 mas_walk+mas_next →
  mas_find(addr+1)（NULL walk 后 mas 未定位, mas_next 答空）并补
  rcu 读侧。r19 实证: 全员普适触发。
- **线 2（转换后 fork 断链）受控复现 + 窄化**: debug 过滤参数
  corten_stack_convert_comm= 单进程转换（dhcpcd adopts=1 → 网断面
  死）; 对照（零转换）干净 → 破坏者 = 转换本身。KUnit 锚
  （fork_migrated, 147/0/3 过）: 真 sweep 栈臂 + 真 mirror 复制臂
  全对（wrprotect/子 RO/meta 重放/INV7 双侧净）→ 断点在 fault
  上下文转换的实机序列: **stage=2 异态**（记录 [cd9000,top) 已覆盖
  fault 地址 cf8000, arena lookup 却未命中 = 记录范围与帧注册
  不一致）。
- **默认开判据**: stage-2 异态修复 + alone-boot 对照
  （臂开+零转换 SSHOK）与全转换 boot 双绿。
- 可复现配方: corten_stack_extend=on corten_stack_convert_comm=dhcpcd
  → 串口手动 dhcpcd -d eth0 → segfault at sp 下方 + stage=2 打印。

**r23-r24 三度硬化 + 剩余断点精化（2026-10-09 午后）**:
自 dhcpcd 单进程复现连出三修: (1) 帧自愈（extend 落入已覆盖但未
注册帧时注册并服务 —— 同帧步零插值使漏注册在下降中存活）;
(2) frozen 可见性（栈扫描对 fork mirror 窗内 frozen 记录不可见,
与 parked 同待 —— extend 不得变异 mirror 正读的 start）;
(3) 探测 rcu 读侧 + mas_find 原语（r12）。剩余断点精化: 转换后
dhcpcd 仍死于一次栈写 —— fault 地址位于记录覆盖范围内、lookup 却
未命中且服务未达（stage=0 + 覆盖几何并存）= fault 上下文转换的
更深层状态交互。对照面保持绿: alone-boot（臂开+零转换 SSHOK）、
KUnit converted-fork 锚、全转换 mass boot 的断链 = 同族。

**r26-r28 决定性捕获（2026-10-09 下午）**: heal-skip 身份探针
示出 miss 帧的占用者 **= 栈记录本身**（[7fffe3179000,7fffe319a000)
rf=0x60 GROWSDOWN|ADOPTED, 已注册于自身帧），死亡 fault 落在
**end 边界附近**而服务未落地 —— 异态从"帧注册不一致"精化为
"**覆盖内地址服务未落地**"（记录跨度正确、帧注册正确、lookup
在边界地址 miss）。refuse 打印已补 end 字段（fbe4194），下轮首捕
即得 [start,end) × fault addr 配对。另见 V3 阴影的 rss 记账漂移
（+2 FILE/−2 ANON 每退出, WARN 级）待查。

**r29-r32 深挖 + V4.1 落地（2026-10-09 傍晚）**:
- **V3 竞态根因与修复**: 阴影 declare→marker 窗让首批特殊范围
  fault（glibc 时钟读）以零页 FRESH 服务并置 MAPPED（vDSO 内容
  损坏 + 退出 -2 ANONPAGES 的来源）。修 = SPECIAL_SHADOW 标记随
  declare 的 rf_birth 出生即带（declare_locked 加参数, 十调用点
  机械传 0）。
- **栈臂 frozen-wait**: r23 的 scan 跳 frozen 本身即回归 —— 冻结窗
  内 fault 被让给漏斗（无 VMA = SIGSEGV）。修 = scan 不跳 frozen
  （extend 的 mmap_write 对 dup_mmap 天然阻塞, 冻结后服务）+
  自愈对"槽内即本记录且覆盖"返回 covered。
- **V4.1 wl_shared census=0 达成**: 共享形状（vm_file 非空, 含
  shmem）经 FILE 臂收编 —— pagecache 锚即写穿, 多映射一致性走
  pagecache + W1.b inode 登记簿, 全程无 COW。taxonomy/mixed-frame
  重锚（共享标本收编, 混帧出口双侧 PTE 引用经 arena zap 归还）。
  boot: sweep_skip_shared=0。KUnit 208/0/9（interlock 偶发）。
- **栈臂剩余断点**: dhcpcd 单进程转换已过 SIGSEGV（frozen-wait 后
  干净早退, eth0 仍未配置）—— 剩余切片需客户机内进程调试
  （strace/gdb）, 串口速率诊断已到极限。alone-boot 对照持续绿。

**下一轮入点**: (1) 栈臂单进程切片（客户机内 strace/gdb 定位干净
早退点）→ 默认开; (1b) V3 阴影 rss 漂移余量核验; (2) V4.2 共享
匿名/shmem 臂余量 + mlock pin 臂; (3) V5 总闸; (4) VI VMA 层删除
+ J6 终账。
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

## 7f. 深夜三轮二（03:40）: 架构性 layout 修复 + C1 自相矛盾发现

- **架构修复落地**: fence 重设计——MODE mm 的 legacy 域分配移到窗口上方
  [64T, 128T)（原设计挤压在窗口起点正下方 [16T-1M, 16T) 窄带, ld.so 的
  向下 hint 与栈页碰撞 = C1 保护性 -EBUSY 的根因）。hint 移到经典区
  7f5f9d490000 ✓ layout 修复生效。
- **新发现: C1 自相矛盾**: 同一 check_empty_locked——原址臂预检查空通过
  → declare 内部 C1 报 PTE 内容 -EBUSY。微秒级窗口内状态变化或范围
  计算差。**下轮: per-PTE 转储**（C1 内容打印已含 first= 地址; 需加
  pre-check 与 declare-C1 的两次读数对比 + PT 页全 dump）。
- 稳定链 909bccf =on boot 绿不受影响; pr-v1 全量在案。

## 7g. 最终定位（04:15）: -EBUSY = ELF 重叠段映射 × [C1]

**根因闭环**: ELF 的 PT_LOAD 段映射天然互相重叠（RW 嵌在 RX memsz 内）。
ld.so 的段 MAP_FIXED 到达采纳门 → [C1] 检出**前一段的活页**在被收编范围
内 → 保护性 -EBUSY → ld.so "cannot map segment" → exit 127。
**修复点（单函数, 已精确）**: 采纳门的 overlap-teardown（punch borrow）
需覆盖 dlopen/ld.so 的重叠段形态——W-7 exec 镜像已有同款机械, 差异仅在
触发条件与 zflags 的页粒度对齐。这是 V1 的最后一块拼图。

## 7h. 收敛轮终态（05:20）: 失败机制完整定性

插桩轮定位: 失败 = ld.so 的**相邻段映射**（段 N+1 的 MAP_FIXED）到达原址
采纳 → 原址臂占用检查（VMA + arena-registry 两面）判空闲 → declare →
**[C1] 检出段 N 的活页在 N+1 范围内**（ELF 相邻段页粒度重叠的固有形状）
→ -EBUSY → ld.so 放弃。

**定性**: wl 域页粒度原址收编 × ELF 重叠段 = 需要 W-7 级 co-frame 共存
机械的 dlopen 形态扩展——declare 的 [C1] 对"范围与前一段重叠"的形状需要
W-7 桶式的页粒度共存判定（exec 镜像同形已由窗口机械覆盖, wl 域为新增）。
**这是有明确设计路径的机制片**: (a) 原址臂预检改用 W-7 桶式重叠判定;
(b) 或 declare 的 C1 对 wl 域增加"页粒度共存"半（前段记录覆盖的页不算
内容）。估 1-2 枚 PR。

**今夜成果封存**: pr-v1 = d7bc2d6（全量插桩+四轮修复+机制定性）; 稳定链
909bccf =on 绿; E2 A/B/C 已落地; THE PLAN v2 与 J6 在案。

## 7i. 机制完全闭环（05:45）: ld.so DSO 装载协议 × 采纳门

**完整机制（全部插桩实证）**: ld.so 的 DSO 装载 = (1) mmap(NULL, total,
PROT_NONE) 预订 → (2) 逐段 MAP_FIXED 进预订范围。V1 原址臂把预订收编为
PROT_NONE region ✓; 段 1 的 MAP_FIXED = 采纳门 declare ✓; **段 2（与段 1
页粒度重叠——ELF 段布局固有）的 MAP_FIXED → 采纳门重 declare → [C1] 检出
段 1 的 FILE_MAPPED 标记 → -EBUSY → ld.so "cannot map segment" → 127**。

**窗口机械已有同款处理**（=on exec 351 次进场、窗口内重叠段全绿为证）——
**修复 = 采纳门移植窗口级的重叠段 declare 处理**（W-7 co-frame 的
admission 形态）: 段 N+1 的 declare 对段 N 已标记页 = 元数据重写（新段
赢）, 非 -EBUSY。估 1 枚 PR（admission 的 overlap 扩展 + KUnit 锚）。
**V1 至此 = 机械全通, 唯此一片**。

## 7j. 最终调试发现（06:00）: 元数据/PT 页腐蚀类（P1 级）

C1 的 present PTE 值解码 = 物理地址超出 VM 内存的垃圾四元组 → **PT 页/
元数据数组被先前的原址收编操作腐蚀**（非合法 PTE）。定级 P1: 内存腐蚀类。
这是 wl 原址收编需要"设计片而非手术片"的最终实证: 交互面 =
(原址 declare) × (punch borrow) × (窗口 placement 共存) × (fork/exit
走查) 的状态一致性, 需要在设计文档层面先闭合（每条路径的状态转移表）,
再写代码。今晚的 9 轮调试 + 全部插桩资产 = 该设计片的完整输入。

**用户可见状态保护**: android17-6.18 @ 909bccf = 稳定 =on 链（boot 绿,
电池绿, E2 退役含）——未受 V1 实验影响。V1 全部工作在 pr-v1 分支封存。

## 7k. strace 阶段（2026-10-09 晚, r33-r38）: 栈臂单进程切片的最终数据

strace 已在镜像。frozen-wait 后 dhcpcd 的死亡形态从 SIGSEGV 变为
干净早退。strace -f 捕获（dhcpcd-only boot）: **uid=100 的特权子
进程 345/346 以 exit(0) 即刻退出为首因**（fork 后不 exec、继续以
特权角色运行的同一二进制 —— 其 mm = mirror 副本），随后父的
write(5) EPIPE ×3 → exit_group(1)。健康对照（zzz boot, 已存主机
/tmp/dh-good.trace, 155 行）: edge 长活为常态。子进程 loop 早退的
驱动 = 下一轮的第一问题。

**下轮工具与判据**: boot 形状直接带 9p（trace 全量落主机, 串口
反复 boot 后退化不可靠）; MISSGATE 探针已在树（lookup miss 的门
身份: frozen/dead-ref, 变量修正后的构建未上机）; 默认开双绿判据
不变（alone-boot SSHOK + dhcpcd-only eth0 获地址 + ssh 存活）。

**下一轮入点**: (1) 栈臂单进程切片续（9p + strace 全量 → master
首错/子 loop 驱动定位）→ 默认开; (1b) MISSGATE 上机首捕;
(1c) V3 阴影 rss 漂移余量核验; (2) V4.2 mlock pin 臂; (3) V5 总闸
（create_vma WARN+拒绝, /proc smaps region 化, special 影子豁免）;
(4) VI VMA 层删除 + J6 终账。

## 7l. 9p 捕获轮（19:30）: ptrace 活锁征兆 + 通道结论

9p 形状 boot + mount 成功（dh-full.trace 落主机 = 9p 通路验证）,
但 strace 全量运行后 qemu 升至 707% CPU 且客户机串口全静默 ——
**ptrace(strace) × 转换栈 fault 路径的活锁征兆**（tracer 的 GUP/
ptrace 停走与被踪者的 fault 处理互旋）, 新交互面入档。反复 boot
后串口会话退化 = 本环境（serial-only + 无 gdb-stub）不支持
dhcpcd 切片的交互级调试。

**下一会话的既定工具序**: (a) boot 带 lodging内核打印即可（放弃
交互）: 栈臂 + MISSGATE 探针的 dmesg 已含 stage/end/门身份 全套,
dhcpcd-only boot 的 console 落盘即可全分析; (b) 或 gdb-stub
(kgdb) 形状。判据不变: alone-boot SSHOK + dhcpcd-only eth0 地址
+ ssh 存活 → 默认开翻转。V4.2 mlock pin / V5 总闸 / VI + J6 =
其后序列。

## 7m. 默认开翻转（r39-r42）: 栈臂收口达成

**根因 = 扫描起点**: 栈扫描从 frame+1 起 —— 刚转换栈的首个增长
fault 落在其 start 下一页 = **转换注册的那一帧本身**; 扫描跳过它
→ 答空 → 漏斗无 VMA → 每次转换后 60ms SIGSEGV。修复 = i 从 0 起
（含自身帧）。

**实证**: dhcpcd-only boot 双绿（adopts=1, grows=5, DHCP 租约经
region 形态服务, eth0 配置, ssh 活）; **默认开 mass-conversion
boot 全绿（180/180 进程栈 region 化, grows=10, 零 trap, ssh 活,
DHCP 完成）**。M2 里程碑的栈腿达成。判据双绿 ✓。调试过滤器
（corten_stack_convert_comm=）与 off 开关保留。

## 7n. V4.2 落地（40c1ea3）: mlock pin 臂

sweep 的 flags skip 掩码移除 VM_LOCKED/VM_LOCKONFAULT: 形状经 FILE/
ANON 臂收编, mlock 契约以 CORTEN_RF_PIN（bit 8）随 rflags 反射出生。
shrinker 两处 pin-loop 保持 PIN 记录免于 aging/eviction（mlock 的
region 形态核心承诺）; fork mirror 逐字重放 rflags。后续余量:
mlock()/munlock() 系统调用在 region 范围上的路由（现 legacy 路径对
无树 VMA 的范围响亮 -ENOMEM）。KUnit 208/0/9, checkpatch 0E/0W。

**V4 段至此三项落地**（V4.1 shared census=0 / V4.2 pin / THP+numa
后置）。**下一入点**: V5 总闸 —— create_vma WARN+拒绝（MODE mm 内
非豁免形状: 豁免 = special 影子 VMA + VM_CORTEN 阴影片）+
/proc smaps region 化 + 总闸长跑零触发验收; 其后 VI VMA 层删除
+ J6 终账。

## 7o. V5.1 落地（6f12aa1）: create_vma 普查闸 = 零

vma_link 的非豁免臂（VM_CORTEN 阴影片 / special 影子族 / dup_mmap
fork 拷贝之外）按 MODE mm 计数: **=on boot 读数 vma_gate=0** ——
MODE mm 内每个 create_vma 都是自有/阴影/fork 拷贝: V1-V4 机械覆盖
全部形状空间, **V5 拒绝翻转已证明安全**（随 smaps region 化同片
执行）。WARN 首事件即响（boot-loud）。

**V5 段余量**: 拒绝翻转 + /proc smaps region 化 + 总闸长跑零触发
验收。**其后 = VI**: VMA 层逐文件删除（vma.c→mmap.c VMA 臂→
vma.h→dup_mmap→/proc maps VMA 渲染→memory.c VMA 路径→rmap VMA
位→tools/testing/vma）+ grep 零依赖门 + J6 终账（VMA 层 13,408
+ E1/E3 残面 + 白名单机械出账）。

## 7p. 拒绝翻转首试回退（r43-r44）: funnel 形状普查是前置

拒绝翻转首 boot 即杀 init: **init 的 hinted 文件 mmap（2.4MB, 经
ksys_mmap_pgoff）被 auto 路由拒绝（decline 原因未录）→ 漏斗 →
ENOSYS → exec 死**。in_execve 豁免不足（exec_mmap 后已清）。
回退 = WARN-only 普查态（vma_link 门保持, r42 已证绿）;
corten_refuse_vma_funnel 函数保留待接。

**下轮首题**: auto 路由的 **decline-reason 计数器**（哪个门拒绝了
init 的 hinted 文件 mmap）—— 路由应答 vs 漏斗服务的形状普查对齐
后, 拒绝翻转才有正确的豁免面。smaps region 化同片。

## 7q. V4.3 尝试回退（0a112614）: 库装载高速路的 file 臂调试轮

MAP_DENYWRITE（ld.so 装库携带的无操作兼容位）准入 → 库装载高速路
进 auto file 臂 → **init 首次 libc load 即段错误**: 该臂的
read/COW/EOF 面从未服务过 dlopen 形状。回退恢复 r42 已证绿的
漏斗=库高速路状态（披露 resident）。V4.3 切片 = auto file 臂的
调试轮（strace 工具在联网 boot 上, 死亡可在 init 首次 libc load
复现）。复现配方: classify 文件白名单 + MAP_DENYWRITE → boot。

**链状态**: V2.1 ✓ V3 ✓+竞态修 ✓ V4.1 ✓ V4.2 ✓ V5.1 普查闸 ✓
(vma_gate=0) 栈臂默认开 ✓+电池 ✓。**剩余**: V4.3 file 臂调试
（库高速路）→ V5 拒绝翻转+smaps region 化 → VI 删除 + J6。

## 7r. V4.3 第二层（2026-10-09 深夜）: EOF 再派发 × slot 机械

DENYWRITE 准入 + EOF 再派发（越 EOF 的 FILE 槽按 anon 再派发: 读 =
共享零页无态, 写 = map_anon 永久转 PRIVATE_ANON——语义与 mmap 契约
逐字一致）把死亡推进到 **corten_slot_remove WARNING**: meta 改写
(FILE_MAPPED→PRIVATE_ANON) × 槽机械的未操练交互 = 第二层洋葱。
回退恢复 r42 已证绿（boot ssh+eth0 实证）。**两层均已入档**:
(1) classify 白名单 + DENYWRITE; (2) fault_once 的 EOF 再派发。
V4.3 = 两层修复 + slot 机械走查的设计轮。复现: (1)+(2) 两补丁
（本提交的父提交可寻）+ boot。

**链状态不变**: V2.1/V3/V4.1/V4.2/V5.1 ✓ 栈臂默认开 ✓+电池 ✓。
**剩余**: V4.3 设计轮 → 拒绝翻转 → smaps → VI + J6。

## 7s. V4.3 设计轮走查完成（b447995）: 占用根因闭合

SR MISS 身份探针捕获: 解退的"非成员"帧 0x800002 持有**邻居窗口
记录**（b0≠ar）, init 的段错误地址（0x100000400238）落在邻居
注册范围 [100000400000,100000800000) 内 —— **第二次库装载的
漏斗 VMA 落入第一次装载的 region 跨度**。根因 = **auto FILE 臂的
窗口摆放无占用检查**（adopt 路径跳过 C1' 重叠探针）→ 相继库装载
重叠共注册 → 跨重叠的 W-7 桶合并 + 插入失败解退覆盖错帧 = 混乱。

**V4.3 切片精确入口**: auto FILE 臂摆放改占用感知的帧分配（或
adopt 路径加重叠检查）→ 两层修复（DENYWRITE 白名单 + EOF 再
派发）其上落地。探针留树。
