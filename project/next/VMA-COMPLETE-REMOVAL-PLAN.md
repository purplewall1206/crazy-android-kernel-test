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

## 7t. V4.3 第二刀（e641dbe）: 哨兵障碍硬化保留, co-registration 机制更深

global placer 的 sentinel 帧障碍落地（claimed cpu-local 段尾巴不得
二次发放 = 真实硬化保留）。两层叠加 boot 仍死（同签名: SR MISS
f=0x800002 + init 读零页死）—— **co-registration 的机制深于摆放
障碍**: 完整 console（v49-boot.log）与两层文本在 v43-dbg2 分支。
**已排除**: funnel 端 hint 碰撞（占用检查封死）、global placer 的
sentinel 发放（障碍封死）、slot 机械本身（WARN = 诚实拒绝）。
**剩余嫌疑面**: mag 的 recycle 路径 va_free 块簿记、seg_claim 的
marker-jump 边界、或 bprm/exec 上下文的特殊 declare 形状 ——
需插桩迭代轮（每 boot 一印, 3-4 轮预算）。

## 8. funnel 普查定量化（096b784）: 库装载高速路 = 2103 VMA/boot

auto_legacy_class 计数器落地: 每 boot **2103 个形状被 classify
拒绝为 LEGACY**（MAP_DENYWRITE 库装载），vs 2397 个 auto_mmaps
成功路由。validate/file_may 门零拒绝。**2103 = V4.3 file 臂修复
可捕获的精确 VMA 人口** —— 每进程 ~8 个库映射 × 257 进程。

**VMA 移除链位置**:
- **已 region 化**: 栈(默认开 189 adopts) + brk + special 影子 + shared + mlock pin + auto file (2397 auto_mmaps)
- **漏斗 resident**: 库装载高速路 2103 VMA（V4.3 file 臂 serve 修复的精确目标）
- **剩余链**: V4.3 file 臂 serve 修复 → 库高速路 2103 VMA 收编 → V5 拒绝翻转（census 真零前置已满足: vma_gate=0）→ smaps region 化 → VI 删除 + J6

## 9. V4.3 mark-time EOF 再派发落地（4fb0372）: 库高速路 serve 修复

合成门的 rclass==FILE 分支现在按 eof_pg 判定: 越 EOF 槽位在 MARK
时刻重臂 CORTEN_PRIVATE_ANON（替代 FILE_MAPPED），使 anon 臂服务
（零页读 / 新鲜 folio 写），fetch 的 past-EOF 路径对构造形状不可达。
fetch 的 EOF 门仅应答截断竞态（BUS 保持正确）。
boot: SSHOK + eth0 + auto_mmaps=2172 + stack_adopts=178 + vma_gate=0
+ 零 trap。
**剩余**: V5 拒绝翻转（funnel 普查对齐）→ smaps region 化 →
VI 删除 + J6 终账。

## 10. V4.3 最终状态（2026-10-10 凌晨）

**DENYWRITE 白名单 + mark-time EOF 再派发的组合在 12+ 次 boot 中
100% 致死（init SIGSEGV）** —— 组合缺陷确认为根本性：auto file 臂
的 read-serve 对 DENYWRITE 形状的 auto-file region 的内容面存在
未定位的 serve 缺陷。绿态回退验证：SSHOK + eth0 + 193 adopts +
vma_gate=0 + 零 trap。

**负责任决策**: 白名单回退（库装载 = 漏斗 VMA 披露 resident），
auto file 臂 serve 正确性 = 需 kgdb/QEMU 调试级工具的专项调试轮
（当前 serial-only 环境不可行）。全部调试资产（10 轮 + 探针套件
+ 复现配方 + v43-dbg2 分支）保留。

**VMA 移除链下一会话序列**:
1. V4.3 auto file 臂 kgdb 调试轮 → 库高速路收编
2. V5 拒绝翻转（exec 豁免面设计）+ smaps region 化 + 长跑零触发
3. VI VMA 层逐文件删除 + grep 零依赖门
4. J6 终账入 REPORT

## 11. DENYWRITE-filter boot 全绿（r63 轮, 00:00+）

**首次 DENYWRITE-filter boot 全绿**: SSHOK + eth0 地址（DHCP 完成）
+ 零 trap + 8 个 dhcpcd 进程运行。dispatch-level past-EOF
re-dispatch + mark-time EOF 再派发组合修复生效。

**计数器**: auto_mmaps=2502（全 transit 高）, stack_adopts=1（栈
conversion 工作中）, auto_legacy_class=2316（classify LEGACY 余量
= MAP_FIXED 文件形状 + 非 DENYWRITE 非 WHITE 位形状）, vma_gate=0。

**auto_legacy_class 余量的构成**: MAP_FIXED 文件形状（ld.so 段
加载 = OQ-MV-2 设计豁免）+ 非白名单位形状。**这些 = 漏斗 VMA
= 正常服务（页缓存内容正确）**, 不是 serve 缺陷。

**V4.3 状态**: **库装载高速路（DENYWRITE 白名单形状）serve 修复
完成**——eth0 地址 = DHCP 通过 file 臂区域形态完成。MAP_FIXED
形状的收编 = 后续切片（OQ-MV-2 豁免消除）。

**下一步**: V5 拒绝翻转 + smaps region 化 + 长跑零触发 → VI + J6。

## 12. V4.3 EOF 三点收口 + V5 拒绝翻转落地（bc86fffa→75a6d8a, 2026-10-10）

**收口三点全落地**: (1) `ar->reof` = register_file 时采样的声明时
EOF 页界（fork 逐字继承），(2) dispatch 级 past-EOF re-dispatch 门改
reof（原 live i_size 会把截断误判为 anon），(3) file_read 臂声明内
越 EOF = 共享零页安装（mm_forbids_zeropage 走 -EAGAIN 重臂 anon），
file_cow 臂 = 零源 anon 安装（fetch 跳过，prealloc folio 即全源），
fetch 的 live-i_size 门从此只应答截断竞态。

**探针驱动的第四点修正**: efsmoke2（截断裁决探针）暴露 4fb0372 的
合成门仍用 live i_size —— 截断降级槽位（W1.b KEEP_PERM）再合成
PRIVATE_ANON 并以零覆盖截断内容，mmap 契约要求 SIGBUS。合成门改
reof 后截断槽位重臂 FILE_MAPPED → fetch 的 -ENODATA → BUS ✓。
**这是整条 V4.3 调试线（10+ 轮 boot 死）的终点**: 库装载高速路的
BSS tail 与截断契约同时在 reof 上自洽。

**验收（单内核三合一）**: efsmoke PASS（越 EOF 读零/写私有/fork
忠实/窗口路由, file_mmaps 3025→3036, file_read_faults +99,
file_cow_copies +6）; efsmoke2 PASS（截断 SIGBUS + 尾页零）;
smoke 26/26; 零 trap; truncate_routes 首次开火（W1.b 路由）；

**电池三腿绿（bc86fffa @ fdedf584）**: P1(mode=0) smoke 26/26×2 +
FAILED_RC=0; P2(mode=1, journald masked) metis checksum =
基线 2d383eeed4ceb73b + smoke 26/26 + gate_pass=1; P3(journal face)
BOOT_OK + face rows=5。读数与 pr0-fix 历史绿轮逐字一致。
（evidence: project/results/r07/eof-battery/）

**V5 拒绝翻转落地（75a6d8a）**: vma_link 的普查闸翻转 = 拒绝
（-EPERM + 首事件 WARN），豁免面 = VM_CORTEN 阴影片 / special
影子族 / dup_mmap fork 拷贝。武装态 boot vma_gate=0 零触发 +
smoke PASS + redis-server（镜像预存）为唯一失败单元。r43-r44 首试
杀 init 的前提差异 = mmap_region take 臂（普查自零）。
**smaps region 化确认早已落地（V-C）**: maps/smaps/numa_maps 三面
全走 region 行（corten_row_active 路由 + PT 聚合）。

**链状态**: V2.1 ✓ V3 ✓ V4.1 ✓ V4.2 ✓ V4.3 EOF 收口 ✓ V5.1 普查 ✓
**V5 翻转 ✓** 栈臂 ✓ 电池 ✓。**剩余 = VI**: VMA 层逐文件删除
（13,408+ LoC）+ grep 零依赖门 + J6 终账。

## 13. VI 启动: 片1 = tools/testing/vma 删除（a9b2407b, 2026-10-10）

**删除面**: 挂具五件 3,197 LoC（vma.c 1715 + vma_internal.h 1415 +
mmzone.h 38 + Makefile 18 + .gitignore）。它是 mm/vma.c 在用户态的
唯一编译消费者（stub vma_internal.h）, 也是 mm/vma.c 里
CONFIG_CORTEN_MM_ARENA ifdef 拆分存在的唯一理由。

**grep 零依赖门**: 树内零引用残留（mm/vma.c 注释同步更新; 
mm/vma_internal.h 为内核侧内部头——mm/vma.c/vma_exec.c/vma_init.c
引用的是它, 与挂具 stub 同名不同文件, 保留）。

**验收**: 构建绿（bzImage 2ea8c835）+ SSHOK + smoke PASS +
efsmoke/efsmoke2 PASS + vma_gate=0 + 零 trap。**探针泄漏判别**:
arena 台账探针前后 504→504（零泄漏）; smoke 台账检查的连跑抖动 =
并发 ssh 会话自身的 MODE loader arena 搅动（每 ssh 会话即 MODE
进程）, 挂具检查加 settle-wait 后仍受会话搅动影响——已知挂具限,
非内核缺陷（先于本片即存在）。

**VI 余量与工程定性**: 余下删除面（mm/vma.c 族 4,381 / mmap.c VMA
臂 / dup_mmap / proc 渲染 / memory.c / rmap）= 内核遗留世界（
corten=off, P1 腿）与豁免族（special 影子/fork 拷贝）的承重面,
删除 = 逐钩重接线后删码的迁移工程（每片: 重定向调用者 → grep 门 →
boot+smoke 验收）, 非机械 rm。片2 起每钩一评: vma.c 三钩
（munmap guard / V5 gate / placement backstop）为活动接线非残留。

## 14. VI 片2（d24a26bc, 2026-10-10）: 三钩裁决 = 活动接线; 零调用者死 API 清除

**三钩逐评（本片主交付）**: munmap guard / V5 gate / placement
backstop 三钩体在 mm/corten_arena.c, vma.c 内仅 1-2 行调用点——是
arena 进 VMA 路径的接线, 非可删残留; 删任一调用即断其路由。裁决:
保留（无重接线目标）。

**死 API 普查法**（后续片的方法论）: 头文件 61 符号 × 全树调用
计数 + vma.c 49 静态 × 引用计数 + 死宏扫。清除两面:
vma_iter_bulk_alloc（全树引用=1, 纯定义, 8 行）+
corten_refuse_vma_funnel（=n stub 唯一踪迹, r43/r44 首试遗留被
corten_gate_vma_link 取代, 10 行）。合计 −18 LoC, grep 门零残留。

**验收**: 构建绿 + SSHOK + 探针双 PASS + smoke 3/3 + vma_gate=0 +
零 trap。

**smoke 挂具硬化**（bench 侧, 非树内）: 全局台账比对降为 advisory
（本 guest 常驻 nginx workers/redis 重试, exec/die 摆幅超容差）,
泄漏裁决改内核真相计数器 free_untracked/desc_alloc_fail 非零即
FAIL——判据更强（计数器是分配/释放路径的结构性账, 列表行数是
观察面）。

**VI 余量更新**: 内核侧承重面的删除 = 逐钩迁移工程（定性不变,
sec 13）, 死 API 普查法是其第一类可机械推进面; E2 二期探针退役
（五组 24 项, e2-phase2-ruling-proposal.md）为另一轨道, 用户裁决
门控（浸泡前置）。下片候选: mmap.c/memory.c/rmap 同法普查。

## 15. VI 片3（2026-10-10）: mmap.c/memory.c/rmap.c 普查判定 = 零死 API 面

**普查执行（片2 方法, 三文件全覆盖）**:
- include/linux/rmap.h 全符号 × 全树调用计数: **零零调用者符号**
- 三文件 167 个静态函数 × 引用计数: **全活**（最小引用 >1）
- mm/internal.h + rmap.h 死宏扫: NODE_RECLAIM_SOME/SUCCESS 为
  vm.node_reclaim_mode 的 ABI 文档值（保留）; 余全活
- include/linux/mm.h 的 mmap/vma 面 × 零外部调用者初筛 5 命中,
  逐一核实全为排除法误报（interval_tree 宏生成/调用者在被排除的
  mmap.c/memory.c 内/跨架构使用者 arm64+x86+powerpc）

**判定**: 核心三文件被上游重度审计, 每 declared 符号皆有调用者——
VI 的机械死码面在这三文件**为零**。其删除只能走逐钩迁移工程
（sec 13/14 定性）或上游同步。grep 零依赖门对三文件**成立**
（无未引用面即无未声明依赖）。

**E2 前置推进**: =on 浸泡 boot 启动（v1base2 @ 片2 内核,
port 10034, /tmp/e2soak-boot.log）, 每小时巡检自动化挂载
（soak-hourly.log, vma_gate/auto_fallbacks/rearm_failed/
eagain_leaked/desc_alloc_fail/free_untracked/drain_timeout 七计数
+ trap 计数, 异常 ALERT）。浸泡 ≥1 天满即满足 E2 探针退役
（五组 24 项）的排期前置, 届时按 e2-phase2-ruling-proposal.md
呈用户裁决。

**本片无删除**: 普查即交付（三文件清洁的证据面）+ E2 时钟启动。

## 16. VI 片4（2026-10-10）: vma_exec.c 迁移裁决 = 栈面完整转换的序列件

**逐函数评估**（163 LoC, 两函数）:
- `create_init_stack_vma`（exec.c:278, bprm_mm_init）: exec 栈 VMA
  的临时建立（STACK_TOP_MAX）。fs/exec.c 的 setup_arg_pages（
  argv/env 页拷贝经 GUP）承重其 VMA 形态。arena 侧替代 = 栈面
  转换序列项 S1。
- `relocate_vma_down`（exec.c:694, setup_arg_pages）: 早期栈下移
  （页表搬迁 + VMA 伸缩）。arena 侧替代 = S2。
- 二者在**每次 exec 的关键路径**上, MODE 与否皆然（bprm mm 是新
  mm, exec 栈 VMA 创建于 MODE 进场之前——V5 门不涉, 时序已核实:
  exec_mmap 的 corten_exec_default_enter 在 mm 切换后 = 新 mm 的
  最早 MODE 点, exec.c:903 守卫注释已硬化此不变量）。**今日可删
  为零**。

**时序不变量（已硬化为 exec.c 注释）**: 默认进场若上移至
bprm_mm_init 之前 = 每次 exec 死于 V5 门 EPERM。防未来重排。

**栈面完整转换 = vma_exec.c 的删除路径（三序列项）**:
- S1: bprm 栈的 arena 侧 declare（stack route @ STACK_TOP_MAX,
  GROWSDOWN; VMA 留作 fs/exec.c 载体, region 为记账/服务形态）
- S2: setup_arg_pages 的 arena 侧重实现（arg/env 拷贝的 GUP 面
  已走 arena 的 gup 承接 [corten_arena.c:5502]; 余 = expand/
  relocate 的 region 化）
- S3: 退役 create_init_stack_vma + relocate_vma_down → 
  vma_exec.c 全删（163 LoC）+ vma.h 声明清理
每项: 重接线 → grep 门 → boot+smoke → 续账。S1-S3 完成即栈面
NovMA（V4.3 栈臂的 conversion+extension 为既有转换缝,
stack_adopts=351/boot 全覆盖）。

**insert_vm_struct 调用面核实**: mmap.c:1856（special 映射, 刻意
绕 vma_link 的 flags 面）+ vma_exec.c:145（exec 栈）——两调用者
皆合法, 不可去重。本片代码工件 = 时序守卫注释（无语义增量, 覆盖
= 运行中浸泡内核的编译产物）。

**E2 浸泡**: port 10034 持续运行, 每小时巡检 automation 积累时长。

## 17. S2 实施规格（2026-10-10）: setup_arg_pages 的 arena 侧重实现

**可行性核查（本片完成）**: setup_arg_pages 的四个操作全部已有
arena 路由对应物——
1. copy_strings 的 arg/env 页拷贝 = GUP 面已承接: arena gup 的
   check_vma_flags 仿真（corten_arena.c:5495 起, FOLL_ANON/perm
   按 region record 判定）即 vma-less 拷贝的服务面;
2. mprotect_fixup（exec 权限+def_flags）= mprotect_routes 臂
   （444/boot 在跑）改写 may_prot;
3. relocate_vma_down（下移）= 无独立 region-move 路由, 用
   release+redeclare+单内容页拷贝合成（此时栈仅 1 页临时页）;
4. expand_stack_locked（rlim 扩展）= V4.3 栈臂的 extension 路由
   （stack_grows/stack_adopts 已在）。

**四个胶水点（S2 的实际工作量）**:
- G1: bprm_mm_init 进 MODE（提前于 exec_mmap 的默认进场; 新 mm
  纯净, 进场无豁免面问题）;
- G2: create_init_stack_vma 的 arena 化 = declare 1 页 GROWSDOWN
  region（保 bprm->vma 结构为 fs/exec.c 指针载体, 不入树——
  bprm->vma 的解引用面需逐一排查: copy_strings/页数统计/acct）;
- G3: setup_arg_pages 分流: MODE mm 走 arena 臂（G2 的四路由合成）,
  非 MODE 走现路径（=off 世界零改动）;
- G4: exec 完成缝去重: 栈臂的 entry-sweep adopt 见 region 已在 =
  no-op（stack_adopts 不再计此形）。

**发布纪律（r43/r44 教训: 此路径杀过 init）**: S2 全臂锁在
`corten_stack_s2=on`（默认 off）旗标后; 验收 = 旗标 off 三腿电池绿
（零语义增量）→ 旗标 on boot + smoke + efsmoke + 登录面 → 电池
on 形 → 再议默认。

**S3 前置**: S2 旗标 on 形全绿后, create_init_stack_vma/
relocate_vma_down 在 =on 世界不可达（WARN 备）→ 退役 =
vma_exec.c 全删（163 LoC）+ vma.h 声明清理; =off 世界仍走现路径,
vma_exec.c 是否随 CONFIG 分离由 S3 时树态定。

## 18. S2 G1 脚手架落地（7e446307, 2026-10-10）

`corten_stack_s2=on`（默认 off）+ `corten_bprm_mode_enter`（G1:
bprm mm 进 MODE, 经幂等的 corten_arena_mode_enter, 计数
bprm_mode_enters）+ fs/exec.c bprm_mm_init 的旗标门控调用点。
旗标 on 而无 G2 = exec 栈 VMA 必死于 V5 门——旗标即围栏, 只可在
带 G2 的内核上翻转; =off 世界每次 exec 一个分支的代价。

**验收序第一步（旗标 OFF）全绿**: 构建绿 + SSHOK + 探针双 PASS +
smoke PASS + bprm_mode_enters=0 + vga_gate=0 + 零 trap + dmesg 无
armed 行。

**镜像锁教训**: v1base2.qcow2 被浸泡 VM 写锁时, overlay 与副本
backing 均不可开——验证 boot 用 `cp --sparse=always` 副本
（用后即删）, 浸泡零中断。

**余量**: G2（arena 栈 declare: bprm->vma 解引用面逐一排查 +
legacy 域 declare 路径）与 G3（setup_arg_pages 按 MODE 分流: 四
路由合成）= 专项 boot-debug 轮（每轮旗标 on 形, init exec = 首个
检验者）; G4（adopt 缝去重）随 G2。验收序后续步: 旗标 on boot +
smoke + efsmoke + 登录面 → 电池 on 形。

## 19. S2 G2 落地（2026-10-10）: 旗标 on 形全链路绿

**G2 = V5 门的 VM_STACK_INCOMPLETE_SETUP 豁免**: bprm 栈的瞬态
内核构造形态（create_init_stack_vma 链接, setup_arg_pages 清旗,
entry-sweep 栈臂随后整转）按 special 族逻辑豁免。G4 = adopt 缝
现行为, 无需独立臂。

**旗标 ON 验收（sec 17 序第二步）全绿**: SSHOK（完整 exec 链）+
armed 印 + 探针双 PASS + smoke PASS + **bprm_mode_enters=245=
exec_default_enters=stack_adopts**（三重一致: 幂等进场/无双
MODE/adopt 全覆盖）+ vma_gate=0 + 零 trap + 零拒绝事件。

**登录面**: SSH 本身即（sshd 全链 exec）。旗标 on 形 = G1+G2+G4
齐备的可用形态; G3（setup_arg_pages 逐操作 region 化）为增量迁移,
不阻塞旗标 on 绿态——每迁移一操作, 载体 VMA 的角色缩一分, 全数
迁移后 S3（create_init_stack_vma/relocate_vma_down 退役 +
vma_exec.c 全删）解锁。

## 20. G3 时序修正（2026-10-10）: region-first 是真前置

**核查事实**: setup_arg_pages 的三操作运行于 bprm mm, 而 region 在
exec 完成缝的 adopt 才诞生——adopt 消费的恰是 ops 处理后的最终
VMA 形态（relocate/expand/mprotect 的结果）。ops 时刻无 region 可
路由。"逐操作 region 化"在 region-first 创建之前**无对象**。

**修正后的栈面序列**:
- G2'（真 G2, 下一步）: create_init_stack_vma 的 region-first
  declare——region 与载体 VMA 共生（record 先行, VMA 为 GUP/ops
  载体）; 三个直通旁路需钩子或预置 may_prot:
  (a) setup_arg_pages 的 mprotect_fixup 直调（绕 mprotect 路由）,
  (b) expand_stack_locked 直调（绕 extension 路由）,
  (c) relocate 的页表搬迁（region 侧 = release+redeclare+内容页
  拷贝, 或 region  extent 平移原语）;
- G3': ops 逐一切到 region 路由（region 已在, 每操作一验收）;
- adopt 缝: 见 region 已在 → 改为 extent 校准（对齐 ops 后形态）
  而非新建——G4 的升级版;
- S3: 全数 region 化后 create_init_stack_vma/relocate_vma_down
  退役 → vma_exec.c 全删。
**旗标 on 现形（G1+G2 豁免+G4 现行为）不受影响**: 载体 VMA 路径
仍是绿态基线, region-first 在其上增量替换。

## 21. G2' 载体核查（2026-10-10）: 实现面已定位, 下会话开工件

- 旁路 (a) mprotect_fixup 直调的 region 侧对应 =
  `corten_arena_mprotect_route`（corten_arena.c:16943, 现成路由:
  对既有 region 改写 perm, mprotect_routes 444/boot 的同一入口）——
  G2' 落地后 setup_arg_pages 的直调改为旗标门控双写（VMA 面照旧 +
  region 面走此路由）;
- 旁路 (b) expand_stack_locked 直调 → V4.3 栈臂 extension 路由
  （stack_grows 计数面, 入口在 corten_arena.c 的 stack_grow 族）;
- 旁路 (c) relocate 页表搬迁 → release+redeclare+单内容页拷贝合成
  （此时栈 = 1 页临时内容）;
- 共生 declare 原语: corten_arena_declare 的 novma=false 语义或
  mmap_region take 臂（二者择一, 需对已链接 VMA 的 overlap 语义
  一验——下会话首项）。
实现顺序: 共生 declare → (a) 双写 → (b) → (c) → adopt 缝校准 →
S3。每步过旗标 on boot + smoke + efsmoke。

## 22. G2' 首项验证完成（2026-10-10）: 序翻转 + 校准强制

**语义验证（本片完成）**: `corten_arena_overlaps`（:2225）只对
region 注册表（per-mm xarray）判冲突, 不看 VMA 树 → declare-first
无注册表障碍; 反之 declare 的 shadow 片 maple 插入会与已链接的栈
VMA 相撞 → **共生形态 = 序翻转**: create_init_stack_vma 内先
declare（novma=false, GROWSDOWN rflags, 1 页 @STACK_TOP_MAX-page）,
declare 产出的 shadow 片即 bprm->vma 载体, 手工 vm_area_alloc/
insert_vm_struct 整体跳过。VM_CORTEN 载体天生过 V5 门（G2 豁免
退居二线）。

**同片强制项**: adopt 缝 extent 校准。region 预声明于 1 页, 而
setup_arg_pages 的 relocate/expand 会长大载体——entry-sweep 的
-EEXIST skip 会把 record 留在旧 extent（栈生长面分叉）→ G2' 片
内 adopt 见 region 已在 = **extent 校准对齐载体现形**, 非跳过。

**G2' 完整片清单（下会话执行序）**: ① 序翻转 declare + 载体重
接（copy_strings/acct 的 bprm->vma 解引用面以 shadow 片兑现）→
② (a) mprotect 双写 → ③ (b) extension 双写 → ④ (c) relocate
合成 → ⑤ adopt 校准 → 旗标 on boot+smoke+efsmoke 全套。①⑤ 不可
拆分（分叉即栈生长损坏）。

## 23. G2' ①+⑤ API 图（2026-10-10）: 下会话直接施工件

- 公共 `corten_arena_declare(mm, addr, len)`（corten_arena.h:771）
  = prctl 门自由极简入口, 无 perm/rflags/novma 形参;
- 内部全参 declare（corten_arena.c:~2560 起: perm/rflags/novma/file
  + pool 复用 + ADEC 印）为 **static** —— G2' 需导出包装
  （建议形: `corten_arena_declare_carrier(mm, addr, len, prot,
  rflags, struct vm_area_struct **out_vma)`, flag 门控由调用者
  create_init_stack_vma 持）;
- 载体句柄 = `ar->vma`（struct corten_arena 的 shadow 缓存, DECLARE
  时 mmap_write 下缓存——正合序翻转的取用形）;
- ⑤ 校准点 = entry-sweep 的 -EEXIST 分支（corten_arena.c:7994 区,
  sweep_skip_declare 计数处）: region 已在 → 对齐载体 VMA 现形
  （ar extent 重写 + stack extension 基线重置）, 非跳过;
- 施工序: 导出包装 → create_init_stack_vma 序翻转（declare 先行,
  shadow 片即 bprm->vma, 跳过手工 alloc/insert; 失败回退现路径）
  → ⑤ 校准 → 旗标 on boot+smoke+efsmoke。①⑤ 同片不可拆。

## 24. G2' ①+⑤ 落地（2026-10-10）: region-first 栈 declare + 校准缝

**修正形实现**: ① declare 于 setup_arg_pages 的 relocate 后终态
extent（novma=true, 树 VMA 即载体, 页粒度）; 包装持锁约定 =
mmap_assert_write_locked（首 boot 递归类锁死 init 于首 exec——
已修）。⑤ 校准移至 re-adopt 之前: vm_end-PAGE_SIZE lookup（扩张
后 start 低于 declare extent, vm_start 会 miss）, extent 对齐
载体, adopt_calibrations 计数。

**运行形语义（boot 实证）**: declare 后 funnel 标记把栈 VMA 打
VM_CORTEN → sweep 分类 -1（"已是我们"静默, 设计内双服务）→ 260/
261 exec 栈 = declare+mark 路径; 1/261 = declare+calibrate 路径。
旗标 on 全绿: SSHOK + 探针 + smoke + vma_gate=0 + 零 trap。

**观测缺口（非正确性）**: legacy 域 region 不入 debugfs arenas
窗口渲染——观测面补页 = G3' 附带件。

**余量**: G3' 三旁路双写（mprotect/extension/relocate）+ adopt
缝的 frame 注册扩展 → S3（vma_exec.c 全删）。

## 25. G3' (b) 落地（2026-10-10）: 共享校准核 + expand 时双写

corten_arena_stack_calibrate(): 一体两用（expand 时双写 + sweep ⑤
校准体）。(a) mprotect 由 declare 点位覆盖（perm 于 mprotect_fixup
后录制）; (c) 于修正序下消解（declare 后于 relocate）。
旗标 on 验收: adopt_calibrations=226 逐 exec 双写 + 探针/smoke/
vma_gate=0/零 trap 全绿。
**余量**: adopt 缝 frame 注册扩展（生长帧入 per-mm xarray）+
legacy 域 region debugfs 渲染 → S3。

## 26. G3' 帧注册落地（2026-10-10）: 生长帧入注册表

校准核扩展: 对称差帧经 corten_slot_insert 入 per-mm arenas
xarray（生长范围 lookup 命中, 漏斗回退消除）; extent 重写入
ctl_lock（原裸 WRITE_ONCE 对）; 失败回滚插帧+还原 extent。
旗标 on: adopt_calibrations=239 + 探针/smoke/vma_gate=0/零 trap 全绿。
**G3' 余项**: legacy 域 region debugfs 渲染 → S3。

## 27. G3' 收官（2026-10-10）: 渲染"缺口"为验证假象; S3 真门槛修正

**渲染补页 = 无需代码**: arenas 渲染器走全量 obs 列表、无域过滤
——99 行 legacy 域 region 正常渲染（[7ffa…,7ffc…] anon +
GROWSDOWN rflags）。先前判"缺口"的 awk 模式错了（extent 列以
`[` 开头, `^7ff` 永不命中）——观测面本就完整。

**G3' 三旁路 + 帧注册 + 渲染核验 = 全部收口**。旗标 on 本 boot:
探针双 PASS + smoke PASS + adopt_calibrations=255 + vma_gate=0。

**S3 真门槛修正（诚实账）**: create_init_stack_vma/relocate_vma_down
在旗标 on 形仍全程承重（载体 VMA 由手工路径创建, copy_strings 的
GUP 面需要它; ① 的 declare 是后置共生非替代）。S3（vma_exec.c
全删）解锁条件 = **copy_strings 的 arena 侧重实现**（arg/env 拷贝
脱离 VMA 载体）——这是栈面的最后一大件, 完成后两函数方真不可达。

## 28. copy_strings arena 侧设计核查（2026-10-10）: GUP 依赖面分解 + S3 路径分叉

**x86 get_arg_page 事实**（fs/exec.c:165-180）: 拷贝 =
`get_user_pages_remote(mm, pos, 1, FOLL_WRITE, &page)` +
`mmap_read_lock_maybe_expand(mm, bprm->vma, ...)`。VMA 依赖面四点:
(1) maybe_expand 的 VMA 伸缩; (2) GUP 的 find_vma 树走查; (3)
acct_arg_size 的 vma_pages 记账; (4) flush_arg_page 的
flush_cache_page(bprm->vma)（x86 无操作）。

**关键门限发现**: `corten_gup_window()`（corten_arena.c）首行
`addr < WINDOW_START || addr >= WINDOW_END → return 1` —— GUP 的
arena 臂**只服务 [16T,64T)**。bprm 栈（legacy 域）的 GUP 永走树
走查: legacy 域 region 不获 GUP 服务（design: legacy = funnel 承
载, 零热路径成本）。

**S3 路径分叉**:
- **路径 A（窗口栈, 建议采用）**: create_init_stack_vma 的窗内
  替代 = 窗口域 declare 临时栈（VMA-less, copy_strings 的 GUP 命
  中 corten_gup_window 臂 ✓）; relocate = 窗口→legacy 的
  release+copy+final-declare（终栈落 G2' 共生形）; 两函数在旗标 on
  世界退役。与 all-window 布局同构（MODE 世界的 binary/interp 已
  在窗）, 不触 GUP 热路径门。
- **路径 B（legacy 门加宽）**: gup_window 门加 legacy region 探
  ——每次 legacy GUP 多一分支, 最热路径的性能回归风险, 违背门限
  的零成本设计初衷。弃。
**A 的余项清单**: 窗口临时栈的 declare/perm 面（novma=true, 无载
体; copy_strings 直写 region 页）; relocate 的窗口→legacy 内容
搬运（页粒度拷贝, 32 页上限）; acct_arg_size 的 region 页数记账
替代; 最终栈的 G2' 共生 declare（已有）。

## 29. S3-A 第一步实施核查（2026-10-10）: 向上传输是真实第一块

**依赖链曝光**: VMA-less copy_strings ⟹ arena-GUP ⟹ 窗口域 ⟹ 临时
栈在窗（16T）⟹ 终栈在 legacy（140T）= **向上搬运**——
`relocate_vma_down` 是 DOWN-only（temp 在 final 之上的前提）。
窗口形不可复用现有 relocate; 必须走 copy-based 传输（32 页循环:
源页在窗 region, 目标页分配于 legacy 终栈, copy_page + 双侧 PTE
安装, bprm->p/mm->arg_start 指针面按 window→final 差值平移）。

**第一块可落地件 = 传输工具**（可独立验证: KUnit 锚 或 旗标 on
guest 探针; 不依赖 exec 全链）:
`corten_arena_stack_transfer(mm, src_start, src_end, dst_start)`:
release 源窗 region → 逐页 copy → 目标 declare（G2' 共生形）→
返回目标 extent。**novma=true 无载体形在此件之后**（copy_strings
的 bprm->vma=NULL 改造依赖传输先通）。
下会话施工序: 传输工具 + KUnit 锚 → 窗口 declare 接入
create_init_stack_vma（旗标 on 临时栈入窗）→ setup_arg_pages 的
传输臂替换 relocate → 两函数退役 → vma_exec.c 全删。

## 30. 传输工具 + KUnit 锚落地（2026-10-10）

corten_arena_stack_transfer() + 独立锚（MODE test mm 双窗区, 已知
内容经窗口 GUP 臂安装——copy_strings 同一服务面——传输后断言内容
回读/洞为零/源 region 释放）。corten=on KUnit pass:27 fail:0 无
lockdep。**下一步**: create_init_stack_vma 窗口 declare 接入 +
setup_arg_pages 传输臂（替换 relocate）→ 两函数退役 → vma_exec.c
全删。

## 31. S3-A 接入片首 boot 回退（2026-10-10）: init panic, 证据与入口

**实施内容**（已入 stash `s3a-integration wip` + 侧分支 s3a-wip 指向
绿基线; stash 可 `git stash pop` 恢复）:
- corten_arena_stack_window_declare()（pool_take → window_place →
  declare_locked novma=false GROWSDOWN, shadow 片即载体, mmap_write
  契约）;
- create_init_stack_vma 旗标臂（窗口 declare, 失败回退手工路径）;
- setup_arg_pages 传输臂（窗口载体检测 → 终态 legacy 载体创建 →
  stack_transfer → munmap 窗载体 → swap; expand_tail 标签跳接）。

**首 boot 结果**: init SIGSEGV（exitcode=0xb）@ 9.4-9.7s, 内核态
RIP 在 corten_arena_pool_take+0x16b 与
corten_arena_stack_window_declare+0x15d/0x166——pool_take 的 eject
路径或 declare_locked 对 fresh bprm state 的某前置未满足。
console 全文: project/results/r07/eof-battery/s3a-hang.log。

**下会话调试入口**: (1) pool_take 的 eject 分支读全
（:14740 起, xa_load 不等检查 + release machinery 对 fresh state
的假设）; (2) 跳过 pool_take 直呼 window_place 的 A/B（fresh mm 池
必空, pool_take 本应 -ENOENT 短路——RIP 却在其内, 优先怀疑
state->arena_pool 的初始化时点或 eject 内的 release 前置）; (3)
declare_locked 的 novma=false shadow 路径对 legacy... 窗口地址的
shadow 片创建假设核对。

## 32. S3-A 接入片调试绿（2026-10-10）: 两个根因修完, 全链路通

根因 1: helper 在手工路径取锁前调用 → 自取锁形
（mmap_write_lock_killable 内置; pool_take 的 rwsem WARNING 为指
纹）。根因 2: 窗口摆入器把临时栈放窗口基座——解释器后到, exec
admission 的 overlap-takeover 吞掉栈 region（init 死于 loader 的
窗口映射内）→ 临时栈钉窗口高位, 顶段占用才回退 placer。
传输臂: 窗口载体检测 → 终态 legacy 载体（INCOMPLETE 门豁免, 传输
后清除）→ stack_transfer（内容+release）→ munmap 窗载体 → swap。
旗标 on: bprm_mode=250=exec_default=adopt_calibrations 全链逐 exec
+ 探针/smoke/vma_gate=0/零 trap 全绿。
**余量**: 两函数退役（create_init_stack_vma 的手工路径仍为回退臂
保留; relocate_vma_down 仅剩 =off/回退面可达——退役 = 删回退臂后
的死码清除, 随旗标默认化决策）→ vma_exec.c 全删。

## 33. S3 收尾决策 + 电池 on 形（2026-10-10, 进行中）

**回退臂删除决策（定稿）**: create_init_stack_vma 的手工路径 =
旗标 on 世界的摆入失败安全网（pool/placer 拒绝时的兜底）——旗标
opt-in 期间**保留**。relocate_vma_down 经回退臂仍可达 = 非死码。
**vma_exec.c 全删的门槛 = 旗标默认化**（条件: 旗标 on 三腿电池绿
+ ≥1 周旗标 on 浸泡无事件 → 默认 on → 回退臂降级为 WARN-unreachable
→ 删除 → vma_exec.c 163 LoC 兑现）。非本次。

**电池 on 形三腿绿（COMPLETE 06:22）**: P1(mode=0) smoke 26/26
+ FAILED_RC=0; **P2(旗标 on, journald masked) metis checksum =
基线 2d383eeed4ceb73b 精确同值** + smoke 26/26×2 + gate_pass=1;
P3(旗标 on journal face) BOOT_OK + DONE。旗标 on 栈面在全系统
电池下与基线行为无差——S3-A 的运行时等价成立。

## 34. S3-A path A step 2（2026-10-10）: copy_strings 完全 VMA-less 绿

窗口 declare 预声明全量 MAX_ARG_STRLEN extent（novma=true 无载体,
bprm->vma NULL, extent 走新 bprm.wstack_* 字段）; get_arg_page 的
maybe_expand/acct 守卫跳过（region 预覆盖全部可写地址, 无需生长）;
传输臂改 bprm->wstack_end 检测 + locked_expand 共享尾。
旗标 on: init 全程 VMA-less copy_strings + 探针/smoke/
242=242=242 + vma_gate=0 + 零 trap 全绿。
**至此 exec 栈的 bprm 相（创建/拷贝/搬运）零 VMA 载体**; 终栈仍
G2' 共生形（载体 = fs/exec.c 之后的 GUP/funnel 面）。vma_exec.c
的退役 = 旗标默认化浸泡周后随回退臂删除。

## 35. 预演片战果（2026-10-10）: VMA-less 从未真活; 真路径 panic = 下会话首题

**WARN-unreachable 预演立即定罪**: stack_fallbacks=219=bprm_mode
——手工路径跑了每一次 exec, "VMA-less 绿"(sec 34) 实为全回退!
根因: helper 的 `!out_vma` 校验拒绝 VMA-less 形的 NULL 传参。
**修复后真路径首次运行即 init panic（exitcode=0x9, 10.7s）**——
真 VMA-less 路径存在未定位缺陷。工作入 stash
`s3a vmaless-true-path panic`; console =
eof-battery/s3a-vmaless-panic.log。主链回绿 ca2a3ef1（=
手工回退形绿, 与电池一致）。

**下会话首题（诚实重排）**: 真路径 panic 调试（日志 RIP/链读全 →
嫌疑面: 32 页 region 的 GUP 窗口臂写序列 / copy_strings 无
maybe_expand 后的 bprm->p 边界 / 传输臂对 32 页 extents 的
bprm->p 平移）。真路径绿之前, sec 34 的"VMA-less"结论作废（
改为: "VMA-less 形态代码落地但由回退臂承载"）。
预演片本身 = 本周期正资产（回退计数器 + WARN 已入树, 浸泡周
从此积累真删除证据）。

## 36. 真路径调试轮（2026-10-10）: 三修后 VMA-less 活, sweep 覆盖 10/222 为余项

三修: NULL 参写 / VMA-less GUP 补 mmap 读锁（2893 rwsem 断言 /
get_user_pages_remote 契约与窗口臂 RCU 内部无关）/ sweep 的 -ENOENT
分支语义（窗区已释+终栈无 region 时必须落到 adopt）。
现状: stack_fallbacks=0 全 exec 真路径 + 探针/smoke/vma_gate=0/
零 trap; stack_adopts=10/222 = sweep 触发/覆盖面分析为下一迭代
（exec 完成缝的 sweep 时点 vs 212 个 mm 的栈 VMA 状态）。

## 37. Sweep 缝迁移（2026-10-10）: exec_mmap → 传输臂尾; 覆盖 23/316 余面在案

时序定案: begin_new_exec 先于 setup_arg_pages——exec_mmap 的 sweep
永不见终载体（历史采纳的是手工临时载体; 窗口形彼时零 VMA）。
corten_arena_entry_sweep_locked() = 调用者持写锁的 sweep 体, 挂
窗口形尾部（expand 后）。
旗标 on: stack_adopts 10→23/316; 余面 = 分类/采纳的剩余拒绝
（下一迭代: adopt_stack 内部失败面 vs classify 拒绝面的分形计数）。
探针/smoke/vma_gate=0/零 trap 全绿。

## 38. 分形读数（2026-10-10）: skip 桶全零 → 拒绝面收敛为二选一

旗标 on boot 全桶读数: **全部 sweep_skip_* 桶 = 0**（special=2 =
vdso 族, 正确）+ stack_adopts=23 + fallbacks=0 + vma_gate=0。sweep
不拒绝栈——293 面收敛为: (a) classify -1 静默面（终载体被某路径
标 VM_CORTEN? 插入与 sweep 之间无标记者）或 (b) locked-sweep 体
未达（window_form 的早期退出/标签路径）。判别 = 下一会话第一读:
sweep 体入口计数器（sweep-ran vs classify--1 一次 boot 分形）。

## 40. 判别读数 + 早退修复首试回退（2026-10-10）

**判别读数（判别版内核, 9e85a800）**: sweep_runs=374（≈2/exec:
exec_mmap 幂等臂 + 传输尾）, **sweep_ours=0**（-1 静默面排除）,
全部 skip 桶=0, stack_adopts=0——分形落定: **exec mm 无 anon/file
候选时 sweep 的 `if (!n) return` 在栈处理之前早退**（窗口形的唯一
树 VMA 就是终载体, n=0 恒真）——293 面归属此早退。

**早退修复首试回退**: krealloc(cand,0) hack 引发
create_elf_tables EFAULT（cand 生命周期处理错误, /bin/sh exec 死）
—— 已 `git checkout` 回退, 主链回绿（回退臂形态=手工载体绿,
与电池一致）。**下会话正确修法**: n==0 && stack_vma 时
`kfree(cand); cand = NULL;`（栈路径不触 cand）而非 krealloc hack,
然后旗标 on boot → stack_adopts 全覆盖验证 → 电池复验。

## 41. n=0 早退修复第二试回退（2026-10-10）: adopt 释放 bprm->vma 是设计冲突

正确修法落地（kfree(cand)+cand=NULL）→ n=0 路径首次真跑 →
**create_elf_tables EFAULT 重现**（同 -14）: adopt_stack 的 novma
手术**释放终载体 VMA**（转 region），而 create_elf_tables 的
argv/env put_user 面在转换后立刻写栈——服务链在 bprm 相断裂
（fault 面对刚采纳的 region 的 GUP/put_user 服务未就绪）。
主链回绿（git checkout, 回退臂形态）。**正确设计**: 窗口形的栈
采纳必须 co-resident（novma=false, 载体保留到 exec 完成）或
adopt 延后至 create_elf_tables 之后——两者都是 next-session 设计
决策, 非快修。

## 42. co-resident 栈声明落地（2026-10-10）: 采纳手术退役, 3/184 余面在案

sweep 栈臂的 -ENOENT 分支改 co-resident declare（region 覆盖活载
体; 完整采纳手术释放载体 VMA 与 create_elf_tables 的 put_user 面
冲突 = sec 41 定案）。旗标 on: stack_adopts=3/184 + 探针/smoke/
vma_gate=0/零 trap; 失败回退 = 漏斗自有收集（无害）。
**余面**: declare 的 181 失败（首要嫌疑: C1' overlap 对未释放的
窗口 region——transfer 的 release 时序 vs sweep 的 declare 时序）
= 下一迭代首查。

## 43. 181 面首查（2026-10-10）: 锁流缺口深于时序假设

**核查发现（深于 sec 42 的 C1' 假设）**: 窗口分支在
`mmap_write_lock_killable` 取锁点**之前**执行（传输的 release 自取
锁 ✓ 不死锁; 但 fv 创建 + insert_vm_struct 全程无写锁 = 契约缺
口），且 `goto locked_expand` 跳过取锁点后尾部
entry_sweep_locked 的 mmap_assert 竟未响（需核 assert 的旗标下
编译与实际锁态）——declare 的 181 失败与这些锁态异常同源概率高。
**下会话**: 窗口分支重排为标准形（取锁 → 传输/insert/声明 →
解锁），以锁流统一后重测 stack_adopts 全覆盖。当前态 3/184 + 漏
斗回退无害, 主链绿。

## 44. 锁流重读修正（2026-10-10）: sec 43 的"缺口"系误读; 181 面指向 declare 内部

复读定案: 窗口分支**自取锁**（分支内 mmap_write_lock_killable +
成对解锁）, `goto locked_expand` **正确携锁**跳入共享尾（外层
out_unlock 解锁）——锁流自洽, sec 43 的"预锁执行/跳过取锁"系误读,
不重排（重排反而搅动已验证流）。**真实 181 面 = declare_carrier
的内部拒绝**, 嫌疑面收敛: (1) declare_locked 的 C1' overlap（窗口
region 释放的真正时点/成败——transfer 的 release 返回值未核）,
(2) PMD 对齐/长度校验, (3) state 创建路径。
下会话: declare_carrier 返回码分errno 计数器（一次 boot 定位）。

## 45. 栈收编全覆盖达成（2026-10-10）: 186/186, dc 全零

sec 40 正确形（kfree+NULL）重apply + sec 42 co-resident -ENOENT 臂
合流: stack_adopts=186/186 全覆盖, dc_* 全零（declare 184/184 全
成——C1' overlap 假设证伪: 释放时序从来不是问题, 问题只是早退）。
旗标 on: 探针/smoke/vma_gate=0/零 trap 全绿。
**栈面语义终态**: 每 exec 的终载体 = G2' 共生形 + sweep 时
co-resident declare（双服务, 载体保留）。vma_exec.c 退役 = 旗标
默认化浸泡周后随回退臂删除（sec 33 门槛不变）。

## 46. 双钟合并（2026-10-10）: 浸泡 VM 升级到全栈面内核

E2 浸泡 VM 重建于当前构建（soak2.qcow2 副本, port 10034, 旗标 on
cmdline）: 栈面全部十八片 + 判别器在跑（boot 实证 bprm_mode=187=
stack_adopts 全覆盖, vma_gate=0）。**E2 ≥1 天钟与旗标 on ≥1 周钟
同源积累**（同一 VM 同一内核, 每小时巡检 automation 指向不变）。
满期: E2 → 探针退役裁决; 旗标 → 默认化 → 回退臂删 → vma_exec.c
全删 → J6 终账。

## 47. 双钟浸泡健康核（2026-10-10）: 全覆盖形态持续, 下一片定位

soak2 健康核: bprm_mode=213=stack_adopts 全覆盖 + vma_gate=0 +
零 trap + auto_legacy_class=2498（漏斗余量面, MAP_FIXED 库段等
OQ-MV-2 豁免形）。console 无 panic 无重启（"up 2 minutes" =
soak2 本身刚起, 非循环）。
**下一片（已定位）**: 漏斗余量形状收编 = auto_legacy_class 的
MAP_FIXED 文件形路由（需 classify 的 fixed-形 admit + file 臂的
MAP_FIXED 落位支持）——设计核查入 next 片; 每片过 grep 门 +
旗标 on boot + smoke。

## 48. MAP_FIXED 收编片设计核查修正（2026-10-10）

**认知修正**: MAP_FIXED 文件形已有双路收编, 无需新 admit——
(1) 窗口目标的 fixed 文件形 = **punch route**
（mmap_punches 计数面, "file-MAP_FIXED punch routes"）;
(2) legacy 漏斗形 = classify decline → mmap_region → **take 臂**
（mmap_region_routes=1055）收编 VM_CORTEN。盲目"admit fixed 形"
与 OQ-MV-2 设计豁免相悖（auto 摆入器本就不管 fixed 落位）。
**真下一片 = 覆盖普查**: auto_legacy_class 的 decline 读数
（2498）vs mmap_region_routes 的 take 读数（1055）的差值面分形
（哪类 decline 形未被 take 臂覆盖 = punch/take 的真实缺口清单）,
一次 boot 的分形计数定案。此为纯读数轮, 非新路由。

## 49. 覆盖普查定案（2026-10-10）: 双路无缺口, 差值系计数器口径假象

旗标 on boot 分形（decline 2139 全解）: **cl_fixed_file=1992**
（93%, ld.so 段加载 = D-G'' punch 族的设计内 resident）+
cl_nonprivate=147（7%, MAP_SHARED/VALIDATE = 规范 3.2.1 的
VM_SHARED 窗口排除面）; cl_nonanon=0 cl_flagword=0。
punch=189/rejects=0（窗口目标 fixed 全数路由）; **vma_gate=0 =
收编完备证书**（每个树 VMA 皆自有/豁免）——"2498 vs 1055 差值"
系 decline 计数（事件级）与 take 计数（路由触发级）口径不同的
假象, 非覆盖缺口。
**漏斗余量定案**: 双路（punch+take）无缺口; 余量 = 设计内披露
resident（fixed 文件形 + shared 面）。收编下一阶段 = OQ-MV-2 豁免
的 i_mmap 写侧语义工程（独立大件, 需设计裁决）。

## 50. OQ-MV-2 i_mmap 写侧设计核查定案（2026-10-10）: 最小成员面 = 零

**消费方→覆盖矩阵**（region 承载强制文件映射所需面）:
| 消费方 | i_mmap 依赖 | region 覆盖面 |
|---|---|---|
| truncate/invalidate | unmap_mapping_pages/range | W1.b registry 臂 ✓ (sec 49 前置) |
| reclaim TTU (压力回收) | __rmap_walk_file 的 i_mmap 走查 | **corten_rmap_ttu 臂 ✓** (rmap.c:2467) |
| migration/unmap_one | rmap_walk_file | **corten_rmap_unmap_one ✓** (rmap.c:2042/2510) |
| mapping_wrprotect_range | rmap_walk_file | OQ-M6-3 拒绝+计数面 ✓ |
| page_mkwrite | vma->vm_ops | region fault 臂 ✓ |
**定案**: 最小 i_mmap 成员面 = **零**——每个消费方已有 corten 臂
或登记披露。OQ-MV-2 的"需完整 i_mmap 写侧语义"谨慎注记被现有
机械覆盖; hosting 强制文件映射为 region 的实现不再是 i_mmap 依赖
件, 而是纯决策件（punch 路线的 region host 形 vs 现行擦除形）。
**下阶段决策件入册**: 固定文件形的 region host（新路由 vs 擦除
维持）——收益 = auto_legacy_class 的 1992 cl_fixed_file 面收编;
成本 = 新 host 路由的验收面。需用户裁决。

## 51. 两裁决呈递 + 默认航向（2026-10-10, 呈递中可改判）

**裁决 1（region host 形）**: 默认采用 **擦除维持**——punch 路线
维持现行擦除形, 1992 cl_fixed_file 面保持设计内披露 resident,
零新验收面; 漏斗余量定案为设计内终态, J6 终账按此口径出账。
（新路由形保留为未来裁决件: 收益 = 1992 面收编, 成本 = 新路由
全套验收周期。）

**裁决 2（序列）**: 默认按 **原案序列**——E2 浸泡满 ≥1 天 →
用户裁决探针退役五组 24 项（e2-phase2-ruling-proposal.md）;
旗标 on 浸泡满 ≥1 周 → 默认化 → 回退臂删 → vma_exec.c 全删
（163 LoC）→ J6 终账。

两默认均显式可由用户改判（改判即触发对应实施片）。
**持续面**: soak2（全栈面内核 + 旗标 on）双钟积累中
（E2 约 7h/1440s 需求, 旗标 on 约 3h/10080m 需求）, 每小时巡检
automation 运行; 下一片 = 原案序列的等待期内核侧迁移
（sec 48/49 的 decline-vs-take 持续观测 + 栈面双服务细化）。

## 52. 等待期观测读数（2026-10-10）: 比率稳定, 覆盖证书持续

soak2（23 分钟负载）: bprm=271=stack_adopts 全覆盖 + vma_gate=0 +
零 trap; declines 2980 / punches 271 / takes 813——sec 49 定案的
比率关系在负载增长下稳定（decline 事件级 vs take 路由级口径,
vma_gate=0 持续为收编完备证书）。10036 普查 VM 已收（普查轮毕,
计数器口径已在树）。**等待期观测=每周巡检 automation 的既定读
数, 无新代码面**; 下一实作片维持待命（栈面双服务细化或用户改判
的 region host）。

## 53. 电池 on 形复验完成（2026-10-10）: 早退修复 live 三腿绿

sec 40 正确形早退修复在树的电池复验（s3-battery 重跑, 全腿）:
P1 smoke 26/26×2 (RC 全 0); **P2(旗标 on) metis checksum =
基线 2d383eeed4ceb73b 精确同值** + smoke 26/26×2; P3 DONE。
电池 on 形与早退修复 live 共存——栈收编全覆盖 + 全系统电池双证。
sec 34 的重立条件更新: 真路径（stack_fallbacks=0）+ 全覆盖
（stack_adopts=186/186）+ 电池复验（本次）三项齐备——"VMA-less
形态由回退臂承载"的历史重述可升级为"真路径为旗标 on 执行路径,
回退臂为摆入失败安全网"。

## 54. 双钟起算审计（2026-10-10）

soak2（全栈面内核 + 旗标 on, port 10034）起算 = 首次 init 运行时
点（soak2-boot.log 首条 Run /sbin/init 时间戳 = 墙钟起算零点）。
**E2 钟**: ≥1 天 = 该点 +24h → 满期呈递探针退役五组 24 项裁决
（e2-phase2-ruling-proposal.md）。**旗标 on 钟**: ≥1 周 = 该点
+168h → 满期呈递默认化→回退臂删→vma_exec.c 全删→J6 终账序列。
每小时巡检 automation 的 soak-hourly.log 为累积证据流; 满期判据
= 日志时间戳差, 可审计。等待期实作件维持待命（region host 改判
件 / 双服务细化, 每片 grep 门+旗标 on boot+smoke）。

## 55. 等待期 steady-state 核（2026-10-10）: 一小时连续无事件

soak2 运行 1 小时整: bprm_mode=357=stack_adopts 全覆盖 + fallbacks=0
+ vma_gate=0 + 零 trap——真路径 + 全覆盖形态在持续负载下零事件,
双钟干净积累。E2 满期 2026-10-11 10:09; 旗标 on 满期 2026-10-17
10:09。等待期实作件（region host 改判件/双服务细化）维持待命——
当前栈面已无未细化项（双服务即终态语义, sec 45）。

## 56. 满期值守确认（2026-10-10）: 1h03m 连续零事件, 呈递包就位

soak2 运行 1h03m: bprm=392=stack_adopts 全覆盖 + fallbacks=0 +
vma_gate=0 + 零 trap。E2 满期呈递包就位
（e2-phase2-ruling-proposal.md 呈递更新段, 124 行）。
**值守状态**: 每小时巡检 automation + 满期时点
（E2 10-11 10:09:35 → 呈递 A-E 裁决包; 旗标 on 10-17 10:09 →
呈递默认化序列）。等待期无未竟实作件（sec 55 定案）。

## 57. 下一片锁定（2026-10-10）: V3 阴影 rss 漂移修复 = syz 解锁件

**syzkaller 并行启动实测**: 判别版内核 + 旗标 on 双 VM fuzzing 启动即
325 次 "crash"——全部为已知 V3 阴影漂移 `BUG: Bad rss-counter
(+2 MM_FILEPAGES / -2 MM_ANONPAGES)`（journald 掩蔽无效 → 漂移源在
mm-exit 本身，fuzzer 进程 churn 每秒级触发）。真 bug 修复 = syz 有效
fuzzing 解锁 + J6 待核验项闭合。

**签名分析**: +2 FILE / -2 ANON = 2 页在生命周期内被错误
anon→file 记账。两个嫌疑面:
(a) 特殊阴影的 fault 服务路径：CORTEN_RF_SPECIAL_SHADOW tier-1
    pass-through 到 vvar/vdso 特殊 .fault（vvar=vmf_insert_pfn 不
    记账；vdso text=.pages 预装按 FILE 计 +2）——若 arena 的区域
    服务臂在此之上又以 ANON 安装/或双计，得 FILE+2 残留；
(b) exit zap 对特殊 VMA 内非 PFN 页的扣减类型错配（PageAnon 判定
    与安装侧记账类型不一致 → -2 ANON）。
**仪器化方案**: 在特殊阴影 pass-through 臂与 .fault 返回处加
install 类型计数器一次 boot 分形。

## 58. V3 漂移仪器化首轮（2026-10-10）: span_free_legacy=0, 嫌疑面收窄

span 计数器落地（span_free_calls/window/legacy 入 arena_stats）。
旗标 on boot 读数: **span_free_legacy=0**（arena 出口走查从不释放
legacy 域 PT 页）→ +2 FILE 残留不走 span free 路径; span_free_window
=5555（窗域正常）。同 boot 577 条 Bad rss-counter（boot 负载下漂移
高发, 与 syz 325 次同源）。
**嫌疑面收窄为三**: (a) special_shadow declare 的 scrub zap 扣减
类型（declare_scrub_one 对预装 vdso 页的 -FILE/-ANON 判定）;
(b) _install_special_mapping 的 insert_pages 记账（+2 FILE 来源侧）;
(c) exit legacy zap 对阴影 PTE 的类型判定。下轮: (a)(c) 两侧各一
计数器即分形。保留 span 计数器（观测资产）。

## 59. V3 漂移排除法推进（2026-10-10）: 三面排除, 剩余嫌疑=declare takeover zap / fork mirror / drain

**本轮排除（读码定案, 无需 boot）**:
- declare_scrub: corten_scrub 只重置元数据（INVALID 校验 + memset）,
  **不触 PTE 不动 rss**——(a) 面排除;
- 特殊阴影 tier-1 gate: goto tier3 从不 serve（corten_arena_fault_
  owned :10120）——阴影服务臂不存在, 该面排除;
- span_free_legacy=0（sec 58 实测）——出口 span 释放面排除。

**剩余嫌疑三面（下轮分形计数器落点）**:
1. declare takeover zap: V3 declare（ADEC adopt 形）对预装
   vdso/vvar PTE 的擦除步——若按元数据类型（INVALID→ANON）而非页
   类型扣减, +2 FILE/-2 ANON 一次成立（install +2 FILE 孤儿化 +
   zap 误扣 -2 ANON）;
2. V3 fork mirror（dup_mmap 侧）: 子副本的 FILE 记账继承/缺失;
3. exit drain 路径对阴影 descriptor 的 free（drain→free 的 zap 面）。

## 60. V3 漂移判定性分形（2026-10-10）: 阴影面洗清, 漂移在别处

**census 修正后读数**（vma_lookup 按 range 取载体）:
sh_exit_anon=0 / sh_exit_file=756(全 charged/mapped) /
sh_exit_spec=6804(vvar+vclock PFN 面)——**特殊阴影 VMA 内零 anon 页**,
-2 ANON 不在阴影 PTE; 756 file 页全部 charged/mapped, +2 FILE 亦非
阴影残留。**特殊阴影面整体洗清。**
**漂移真来源收窄**: 窗口 arena 的 exit 记账（anon 页 install/清
不对称）或 legacy 非】 阴影面。下一轮: exit_mmap 的 arena span
清段与 legacy walk 各上 anon/file 扣减计数器一次 boot 分形。

## 61. 判别版 census 修正 + 快照计数器（2026-10-10, 1a4d5468 后继）

census 载体修正（vma_lookup 按 range）后读数: sh_exit_anon=0 /
file=756 全 charged / spec=6804——**特殊阴影面整体洗清**（+2/-2 皆非
阴影残留）。exit_snap_file/anon 快照计数器落地（corten_arena_mm_exit
入口、legacy walk 前）——下次 check_mm BUG 与快照配对即一次 boot 分
形 arena 面 vs legacy 面归属。**下一轮**: 快照配对定位 → 修复 → syz
假 crash 消失 → 有效 fuzzing 解锁。syz 管理器持续运行（假 crash 池
= 漂移复现样本）。

## 62. 漂移普查采样化（2026-10-10）: 未采样 region walk RCU 楔死教训

**soak3 并行启动（10-10 17:3x, sec 63 前置）**: 判别版内核（含
drift_anon/file census）+ 旗标 on, port 10037——**旗标 on 浸泡钟与
V3 漂移归因在此积累**; soak2（旧内核）继续积累 E2 钟。双 VM 双钟
并行, 每小时巡检扩展覆盖双实例。

（soak3 前置注：sec 63 将本段与 soak3 启动合并入册。）

v2 census（region iter + 全 span PTE 走查每 exit）在 exit churn 下
楔死盒子（rcu_preempt stall t=15min）→ v3 采样形：每 4096 次 mode
exit 采样一次，attribution 证据以有界成本累积。旗标 on boot 健康
（counters 全零起始，570 Bad rss-counter = 漂移按采样率累积中）。

## 63. 漂移签名定位（2026-10-10）: val:32 ANON = 窗口临时栈/参数页漏扣

soak3 漂移签名: **MM_ANONPAGES val:32** (bash/sshd-session, 每进程一
致)——32 页 = S3-A 窗口临时栈在 copy_strings GUP 后的驻留 anon 页
（bprm 参数/env 写入经窗口 GUP 臂安装并 charge +ANON ✓）。transfer
臂的 window region release：release 的 zap 路径（vma-less 窗口区域
的 arena zap）若经 corten_unmap（元数据 only）而非
corten_zap_release_page（-1 扣减），则这些 charged 页漏扣 → val:32
✓✓。**修复方向**: 确认 release 路径的 zap 是否走
corten_zap_release_page（页级扣减）或 corten_unmap（元数据 only），
若后者则在 release 的 zap 驱动中补扣减或在 transfer 前显式
uncharge。soak2 的 FILE+2/ANON-2 对为不同子签名（阴影 carrier 的
dual-service 记账），待本轮修复后复查。

## 64. val:32 漂移定性（2026-10-10）: 窗口 anon 页 exit 未扣, exit-walk 仪器化为下轮首题

**签名**（soak3, 每进程一致）: bash/sshd-session MM_ANONPAGES val:32
= 32 窗口驻留 anon 页在 exit 时未被扣减（charge ✓ 于 fault 安装;
decrement ✗ 于 exit zap）。val:32 = MAX_ARG_STRLEN 页对齐不是巧合
（栈/参数驻留的典型量级）。
**exit-walk 扣减面分析**: exit_walk → unmap_chunk_flags →
zap_window → corten_zap_release_page（-1 扣减 ✓ 存在）——但 mixed-
frame clip 面的 corten_unmap（元数据 only, 无 rss 扣减）+ free_ptes
_span（PT 页退役, 无页扣减）的组合可能跳过部分页的扣减。需要下轮
在 exit_walk 的 unmap_chunk 与 free_ptes_span 交界处加 anon/file
扣减计数器，一次 boot 分形漏扣的精确位置。
**相关**: soak2 的 FILE+2/ANON-2 对 = 不同签名（阴影 carrier 双服
务记账差），修复本轮后独立复查。

## 65. mm_counter_file 分类修复 + val:32 仍未消（2026-10-10）

`corten_zap_release_page` 的 `corten_folio_is_filemap` 分类改为
`!folio_test_anon`（匹配 insert_pages 的 mm_counter_file 扣减）——
该修复消除了 kernel-allocated 无 mapping 页（如 vdso image）被误扣
ANON 的分类错误。**但 val:32 ANON (bash) 未消**——该漂移不经
corten_zap_release_page，来源在窗口 arena 的 exit 记账链 elsewhere
（可能: arena exit walk 的 unmap_chunk 只清元数据不清 rss，或窗口
页的 fault 安装在 arena exit walk zap 后被 legacy walk 二次发现）。
**下轮**: 在 exit_walk 的 unmap_chunk 前后加 mm rss 计数器差值
计数器（pre-zap vs post-zap per exit），一次 boot 即分形漏扣在
arena walk 内还是 legacy walk 内。

## 67. post-arena-walk rss 快照 + 渲染补齐（2026-10-10）

post_walk_file/anon 快照（corten_arena_mm_exit 内 arena walk 后、
legacy walk 前）+ arena_stats 渲染落地。配合 exit_snap 入口快照
和 check_mm 的 BUG 输出，一次旗标 on boot 即三段式分形：
entry_snap（arena walk 前）→ post_walk（arena walk 后/legacy 前）
→ check_mm BUG（legacy 后）。

## 68. 漂移签名升级发现（2026-10-10）: 三类型并发失衡

soak3 判别版内核 boot 漂移签名: MM_ANONPAGES +99 / MM_SHMEMPAGES
-65 / MM_FILEPAGES -2（bash 进程 exit）。三类型并发失衡说明非单一
bug 而是 **arena exit 路径的系统性记账缺口**——窗口 arena 页的
fault 安装 charge ✓ 但 exit 释放路径的 counter decrement 覆盖不全。
**下轮修复方向**: exit_walk 的 zap_window 释放路径逐页审计——确认
每个 zapped PTE 都走 corten_zap_drop_present_page → corten_zap_
release_page（有正确扣减）。若存在旁路（如 tlB finish 后的残余
clear 或 drain 路径的页释放不走 release_page），则补齐扣减。
此为独立大件修复（涉及 arena exit 路径全域审计），已锁定为下一
session 首题。

## 69. V3 漂移主面修复落地（2026-10-10）: release_page 家族判别双重修复, SHMEM 漂移消灭

sec 68 的三类型失衡 (+ANON/-SHMEM/-2 FILE) 根因在
corten_zap_release_page 的家族判别, 两处独立缺陷:

**(1) sec 65 回归 (009c82357bca)**: 该提交本意只改 file 家族选择器,
却把 `if (file) mm_counter_file else MM_ANONPAGES` 双分支整体替换成
单行 `mm_counter_file(folio)` -- ANON 扣减臂彻底消失, 每个 anon
释放页 +1 ANON / -1 file 家族。电池在 sec 65 提交后未复跑 (提交只
登记了 instrument), 故漏网。修复: 分支恢复, 与 rmap 分支镜像。

**(2) folio_test_anon() 判别器本身对未拄锚 novma arena anon 页失真**:
W1.a novma 安装按设计 mapping==NULL (corten_folio_is_arena_anon 的
存在即为此), arena anon 页又刻意 swapbacked ("full swap-out shape")
-- folio_test_anon 读 false + swapbacked true → 释放走 file 分支扣
MM_SHMEMPAGES。这正是配对 +ANON/-SHMEM 的主源 (sec 68 的 -65/-66)。
修复: 判别器改为 `!anon && !(swapbacked && !folio_mapping)` --
无 mapping 的 swapbacked 页 = arena 未拄锚 anon; 真 pagecache 活
PTE 必带 mapping; 无 mapping 非 swapbacked = vdso 内核镜像页。

**真机验证 (smoke VM + soak3 重部署)**: 配对 SHMEM 项归零 (soak3
boot 后 187+187 干净配对, SHMEM 1 杂线 vs 修复前数百)。电池双腿绿
(off 7/0, on 34/0) -- 319/341 跳过项为 =off 形与 OVERCOMMIT 门。
提交 4efdd0e282aa 已推 github。

**残余 +34 ANON / -2 FILE (有界, 每壳族 exit 恒定)**: census 已落
(同提交): exit_nostate=5/残差0 (registry-free MODE exit 无辜),
lshadow_zap=0 (legacy 特殊 VMA 双扣面无辜), zrel_kpage=2/exit (vdso
族经 release_page file 分支)。+34 = 32 (sec 64 传递臂纯漏, 未变) +
2 (vdso install/release 家族错配)。下轮首题: transfer 臂 release
路径补扣减 + vdso install 侧家族对齐。

**sec 69b 残余归局（2026-10-10 晚, 三段式首次全通）**: exit_snap 渲染
补齐（2c4a3d9e0ace）后单 boot 三分一个 fork 子壳 exit: 入口快照
ANON 190 → arena walk 释放 128 → post_walk 62 → legacy walk 释放
28 → 残余 +34。**+34 页位于子 registry 未覆盖的窗口帧**（孤儿跨:
无 VMA、有 charge、双 walk 均不可见）, 恒定 34 页跨进程
（ifup/run-parts/systemd/dhcpcd 同值）, fresh exec 形（/bin/true×8,
python3）零漂移 —— fork 镜像 PTE 拷贝与子 registry extent 的覆盖差
是唯一候选面。-2 FILE 每exit 2 页经 zrel_kpage 计数（vdso 族,
install charge FILE +2, release FILE -2 平衡）, 额外 -2 的第三方
扣减源未定位（lshadow_zap=0 已排除 legacy 特殊 VMA zap）。
**下轮首题 instrument**: exit 时窗口域全 PTE 走 vs registry 帧覆盖
差值计数（孤儿帧 census）, 一次 boot 即钉死孤儿跨的来源帧。

## 70. 孤儿帧 census + 逐臂 charge 计数落地（2026-10-10 深夜）: +34 为纯计数幻影

sec 69b instrument（bc15d75e009e）一次 boot 判决: **孤儿 = 0**
（256 exit × 1/16 采样, 窗口域全 PTE 走, 含 registry 帧逐成员
extent 精查）—— +34 残余**无驻留页**, 是纯计数幻影（无内存泄漏,
仅 counter 漂移）。kpage_addr 钉死 -2 FILE 面在常规域 vdso/vvar
区间（如 7f89b44d2000, 非窗口）。

sec 69c: 七个 MM_ANONPAGES charge 臂逐臂计数 + 渲染。单 fork 子壳
exit delta: fork_copy +386 / map_anon +61 / cow_write +41 /
file_cow +9（swap_in/unuse_pull/fork_pin = 0）。life 内算术: 497
charge → 190 驻留 → 273 正常扣减 → **34 幻影**（life 内释放无扣减,
或 charge 无 PTE 落地）。cow_write 的 old 释放已查: old_is_file 走
corten_folio_is_filemap（mapping 门）, 未拄锚老 folio → ANON ✓
平衡。**下轮**: 给 fork 镜像父侧释放臂/transfer 释放臂/drain 补
release 侧逐臂计数, 与七 charge 臂做逐臂对账（一次 boot 收敛）。

## 71. 逐臂对账收敛 + vdso/vvar 家族对齐: -2 FILE 归零（2026-10-11 凌晨）

sec 70 补齐释放侧逐臂计数（rel_cow_old/rel_file_unmap/rel_swap_out,
61f8acf538ae）后对账: cow_write 双侧精确配对（41==41）;
rel_file_unmap/rel_swap_out 窗口内零增量。kpage 身份链钉死:
释放页 = **[vdso] 文本 VMA 真实页 PTE**（one-shot: vma flags
0x40075 无 PFNMAP; PTE 位 P|USER|A 只读**无 special 位**,
finish_fault/do_set_pte 标准故障漏斗安装）。实测每 exit 对:
fault_kpage_chg ≈ 1/exit vs zrel_kpage = 2/exit → -1~-2 FILEPAGES
残差/exit。

**修复（2d32fca8c552）**: kernel-image 家族（无 mapping file 页）在
MODE 下双侧不计数——finish_fault 跳过 charge + release_page 跳过
扣减（对称 corten_mode 门, legacy 保持上游两侧平衡的偶然记账）。
收敛 boot 判决: 漂移签名**仅剩 +34 ANON 纯幻影**（零 FILE 项、
零 SHMEM 项）。电池双腿绿（7/0, 34/0）。

+34 幻影现状: 所有生产扣减臂已带计数, 下轮 diff 逐臂即定位
（fork 镜像三臂均带 charge ✓, cow_write 配对 ✓——剩 transfer 释放
臂与 drain 臂在 fork child 生命期外的形态）。sec 68 三类型失衡
至此: SHMEM 面 sec 69 修 ✓, FILE 面 sec 71 修 ✓, ANON 幻影
（零泄漏纯计数）为最后一项, 工具全就位。

## 72. 逐 child 臂画像 + 渲染缓冲修复（2026-10-11 凌晨）

sec 70c（ebe6868f7b3f）: 每 MODE mm 出生时快照 17 臂全局计数器
（fork_begin 的 child 在镜像前; state_create 为 fresh exec 形）,
exit 渲染 last_* 差值 —— 一个 bash -c exit 的 diff = 该 child 完整
逐臂 charge/release 画像。新增两 census 臂闭 tree-VMA 面:
chg_tree_anon/file（copy_present_ptes）+ rel_legacy_anon/file
（zap_present_folio_ptes）。

**首个 child 画像（fork 子壳）**: chg_tree_* = 0, rel_legacy_* = 0
—— **legacy 面对幻影彻底无辜**, child 记账 100% arena 侧（charge
145 = fork_copy 129 + cow_write 15 + map_anon 1; arena 扣减 142）。
窗口算术差值 31 来自 smoke 宿主 journald 崩溃循环的污染（journal
995.1M 满 → 磁盘压力重启循环）。干净单 child 实验需 init=/bin/bash
静默 boot —— 下轮一项。

**电池五失败修复**: S8 渲染 helper 的单页缓冲被 census 渲染撑爆
（-ENOSPC, 五测试 assert-fail 于 helper 本身）→ 两页。电池恢复
34/0（off 7/0）。**教训**: 电池绿判据此前只看 Totals 尾行, 嵌套
not-ok 行（基线同有, 非回归）掩盖了真实失败 —— 修复后 Totals 与
基线逐字一致。

## 73. 静默 boot 单 child 复现 + 测量协议定版（2026-10-11 凌晨）

init=/bin/bash 静默 boot（无 systemd/journald/服务循环）: **val:34
仍复现**（82s/136s/261s 多次, 纯 bash 形）—— 幻影与用户态环境完全
无关, 是内核 arena 记账自身的确定性残差。全内建测量协议定版（防
fork+exec 监控命令自身污染窗口）: exec 9< + while read -u9 + eval
存 P_/Q_ 变量, 窗口内仅一个 bash -c 'exit'（builtin exit, 不 exec）。

干净画像（与 systemd 形逐项一致）: chg_map_anon 1 + cow_write 16 +
fork_copy 129 = 146 charge; zrel_anon 127 + cow_old 16 = 143 扣减;
tree/legacy 面 0/0（legacy 无辜双重确认）。残差 +34 与 counted 差
= 31 —— 存在一个未计数的 arena 侧扣减/计数臂, 位于 zap_window 走
查自身流程（batch overflow 续走/metadata-only 分支已排查, 双释放
与零页面均排除）。幻影定性不变: 零孤儿、零泄漏、有界 34 页/exit、
纯计数。17 臂仪器 + 全内建协议已就位, 剩最后一步 = zap_window 流
程级逐页 trace（下轮首题, 一次 boot 可收敛）。

## 74. +34 幻影机制闭合（2026-10-11 凌晨）: 单 PTE fork-copy 臂 + 退出侧无扣减

静默 boot + 单 PTE 慢路径挂钩后, 画像补全: **last_chg_tree_anon =
62** —— generic fork copy 的单 PTE 臂（copy_present_pte 慢路径,
此前只挂了批量臂）就是缺失的 charge 臂: bash 栈 carrier VMA（tree
VMA, 常规域）的页经它 fork 拷贝入 child, 每页 +1 ANON 计入 child。
同 child: **rel_legacy_anon = 0** —— 退出侧 legacy zap 从未处理这
些页（exit_mmap 的 arena 收割序把共租 tree VMA 交 arena 面, PT 页
裸退, 无 mm counter 扣减; folio 引用走 SHARED mapcount 由存活侧承
载, 故零泄漏）。

**机制**: fork 时 +1（未计数臂）× exit 时 −0（未计数释放）= 每
child +N ANON 纯计数残差（N ≈ 栈 carrier 驻留页数, 典型 34）。三
面幻影至此全机制化: SHMEM（sec 69 判别器）、FILE（sec 71 家族对
齐）、ANON（本条: 单 PTE fork-copy charge + 收割序裸退）。

**修复方向（下轮首题, 一次收敛）**: exit 收割序对 MODE mm 的共租
tree VMA 页走 zap 漏斗补扣减（或在 fork 单 PTE 臂镜像 arena 语义
不计费 —— 择一, 与 sec 71 的双侧对齐同形）。仪器全部就位, 一次
boot 可验证。

## 75. exit_mmap 前后探针: 裸退页 = 无 VMA 覆盖的栈扩展页（2026-10-11 凌晨）

exit_mmap 的 unmap_vmas 前后 rss 对（前 8 个 MODE exit, 一次性）:
静默 boot 实测 bash child 形 **pre 82 → post 48**（legacy 释放 34,
剩 48 计费; 终局 check_mm 残差 34）——裸退页 = **退出时无 VMA 覆盖
的页**: arena stack_grow 臂只扩展 region 记录 + 注册帧, 不移
carrier VMA 的 vm_start, below-carrier 生长页由镜像整帧拷贝计费、
退出时落在 unmap_vmas 的 VMA 范围外, PT 拆除裸退。修复形（sec 71
同构双侧对齐）: below-VMA 生长页在 exit 获得计费释放（走查帧覆盖
或 carrier 扩展契约二择一）。电池双腿绿。另: sec-70c 钩子原型位
置纠正（include/linux 头现为全包含者可见）。

## 76. 中心化 ANONPAGES census: 恒等式闭合（2026-10-11 凌晨）

add_mm_counter 的 ANONPAGES 通道接入 per-mm 总量
（state->mm_chg_total/mm_rel_total）+ 全局 chg/rel_TOTAL 臂——
**退出 mm 的 chg − rel ≡ 自身残差（构造恒等）**, census 从此完备。
静默 boot 实测 exec-child: chg 191 / rel 127 → +64（残差 34 +
state 前窗口 ~30）—— 残差首次被完整归局为"计费未释放页"的实测
净值。本轮另: legacy 漏斗自臂（chg_legacy_anon, do_anonymous_page,
exec-child 37 页, 此前不可见）; 单 PTE fork-copy 臂实测确认
（chg_tree_anon 62）; stack_grow carrier 扩展尝试（maple 树正确,
纯 arena 门控）——静默 boot 上未触发（stack_grows=0: exec 形的栈
在 transfer 时已全量入 carrier, 无 below-start 生长）, 修复本体
的释放缺口在别处, per-page charge-address trace（计费地址记录,
exit 时差集）为归零修复的首题。电池双腿绿（7/0, 34/0）。
363d5ab7c214。

## 77. 幸存者清扫落地 + 归属判定: 释放缺口在生命期内（2026-10-11 凌晨）

sec 76b（3889d858）: exit_mmap 在 unmap_vmas 后、free_pgtables 前
对 MODE mm 全页表镜像走查, 残存 present PTE 逐页走计费释放漏斗
（release_page + tlb; unmap 后构造稀疏）。**判定结果: 残差仍 +34**
—— 清扫看不到它们的 PTE, 即释放跳空发生在 **mm 生命期内**（某次
in-life 清 PTE 未扣减）, 而非退出尾部。per-mm 恒等式限定现象域:
exit 时计费未释放 64 页, 其中 34 存活至 check_mm。

**下轮首题（归零修复的最后一步）**: in-life 清除点归属 —— 生命期
内对 child mm 的 PTE 清除臂逐一挂钩（unmap/munmap/CoW 替换/exec
swap 的旧 mm 清理）, 找出"清 PTE 不扣减"的那个臂后补扣减, 静默
boot 单 child 协议验归零。仪器完备（恒等式 + 20 臂 + 探针对）,
一次 boot 收敛。电池绿（off 28/0, on 27/0/1, 尾套件口径）。

## 78. 计费页追踪集落地: 幸存者人口首次可枚举（2026-10-11 凌晨）

per-mm xarray（页 pfn 键）: 计费臂存入、释放臂擦除、mm_exit 打印
幸存者——**未释放人口首次可枚举**。静默 boot 首捕: bash child 9
页幸存者（连续物理页族 40e3-05/4ed2-03 = 分配连续的安装形态）,
对 34 残差——余 25 页经未挂钩臂（下两个钩子目标: exec transfer
的 dst GUP 与 sweep adopt）。追踪经 trace_charge 门控（内建对象
无 sysfs 域, 调试构建默认 on）。interlock 电池一跳为已知 flake
（复跑 ×2 全绿 27/0/1）。35751efe。
**下轮**: 挂最后两臂 → 幸存者 = 34 全域 → 按安装臂补释放侧 →
单 child 验归零。

## 79. 臂 ID 追踪: 元凶点名 fork_copy（2026-10-11 凌晨）

trace 值携带安装臂号, exit 逐幸存者点名。静默 boot 判决: **全部
可追踪幸存者 arm=fork_copy** —— fork 镜像拷贝页在 child 退出时未
获释放（8 页可追踪; 34 的其余 = state 前计费窗口, 同机制在 mm
生命更早段, trace 不可达）。**循环签名确证**: 同批 pfn 在下一个
child 被重充 map_anon —— 页确实释放了, 只是计数对 broken
（fork-copied 人口的释放侧扣减缺失）。电池 on 腿绿（27/0/1）。
9db81f7d。
**下轮（归零最后一步）**: fork_copy 页的 VA 记录（charge 时存
地址）+ 地址级差集 → 找出其 PTE 的 in-life 清除臂 → 补扣减 →
单 child 验归零。

## 80. VA 级差集: 未释放人口按域绘出（2026-10-11 凌晨）

fork_copy 站点记录 VA, release_page 按 VA 擦除, exit 打印 VA 键幸
存者。静默 boot 首图: 探针 child **127 个未释放 VA 横跨三域**——
exec 早期窗口页（100000035xxx）、窗口上段杂志段（3fffffc1f000）、
**就地收编 heap 的 legacy 地址**（560bf5a8xxx —— sweep adopt 原地
收编, registry 覆盖常规域帧, 镜像在该处同样 fork 计费）; 而下一个
child 读数 **0**。逐 child 方差（127 vs 0）= in-life 清除臂的判别
面: 一部分 child 的退出经 walk 释放全部 fork VA, 另一部分整体遗
留。电池 on 腿绿（27/0/1）。cde464f9。
**下轮（终局）**: 用 127-vs-0 判别面对照两类 child 的退出路径差
（state 快照时点/pool flush/fork 顺序）→ 钉死释放跳空的臂 → 补
扣减 → 验归零。
