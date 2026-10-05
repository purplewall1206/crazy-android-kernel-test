# MV3.c-feat dev report · exec 镜像收编 + mmap-pf 批 mark（两轴切片）

agent: mv3cf-dev（CortenMM kernel-MM dev+verification; ponytail full; DoD/验证不打折）。
基座: worktree /home/ppw/linux-6.18-mva @ 7545ed299aab（=MV3.c-debug 收口态 + mv3b
FOLL_GET 修复, 主树同步）。不 commit。工件 results/r07/mv3cfeat/（与 debug 轮 mv3c/
分开）。VM 命名空间 mv3cf: PORT 10032, qemu-mv3cf.pid, tmux mv3cf-vm——与 perf2
(10031) 及 mv3a/b/c 轮全隔离, 双方 qemu 全程并存互不触碰。build 线 #347(base)
→#348(probe1)→dbg1-3→a1→a2→a3(轴一终件)→a4→**a5(两轴终件, bzImage sha256 前 16
位 `a3f2137f77b26c16`)**。最终 diff: mv3cfeat-full.diff（3 文件 +219/-27,
checkpatch 0E/0W）。

---

## 0. 判定总览

| 项 | 判定 |
|---|---|
| 轴一 exec 镜像收编 | **绿**: ET_DYN 解释器映像全收编（同帧 4 FILE region + bss implant）, init/裸 smoke/metis 行为零回归, =on 门 gate_pass=1 违例 0 |
| 轴一红面根因 | **绿（定位+修复+锚）**: MV3.a SEGV = 三腿合成的窗口域占位真源缺口（§1.2）, 非路由本身 |
| 轴二 mmap-pf 批 mark | **绿（改善入账）**: 中位数 **+134.6%（t4）/ +135.4%（t8）**, 税 -84.3%→-63.2%（t4, dyn 配对） |
| 轴二"地板个位数"目标 | **未达（如实报）**: 剩余 -63~-77% 差距归因 + 残余设计节在案（§2.4）; **D29 的 -17~-22% 地板数字已过时**（§2.1, 独立发现） |
| KUnit on×2+off / checkpatch / =n | **绿**（194/0/6 ×2; off skip 对账精确+interlock flake 复跑绿; 0E/0W; =n 零符号） |
| =off 回归 | **绿**: dmesg 0 + smoke hook 26/26 + metis 同基准; =off hook 形 ledger 抖动 = 与内核无关（A/B 实证, §1.6） |

**一句话**: 轴一把 MV3.a 的 init-SEGV 红面拆成三条精确腿并全部闭合（exec 镜像进
region、bss 进 implant、窗口域占位真源补全）, 默认进场世界的 exec 镜像行离开
maple 树; 轴二定位到 per-op 池生命周期的页表拆建税并把它拆掉（warm park）, 拿到
+134% 的中位数改善, 同时把"D29 地板数字已过时"这一基准事实钉死入档。

---

## 1. 轴一: exec 镜像收编

### 1.1 复现（红线先行）

撤掉 MV3.a 的 `in_execve` 降级臂（#348, 无其它改动）→ =on boot 4.6s 精确复现:
`init[1]: segfault at 1000000373f6 ip 000010000001de6d error 6 in
ld-linux-x86-64.so.2` + `panic: Attempted to kill init`。从 guest 镜像提取
ld-linux（trixie glibc, 225,672B）读 program headers: 4 PT_LOAD
（0/0x1000RX/0x29000R/0x34000RW, memsz 尾 0x375e8）→ 0x373f6 = **bss 页**（第 4
段 filesz 止于 0x36b4c）。反汇编: 崩溃指令 `andb $0xdf, 0x19582(%rip)`（ip
0x1de6d）, 目标即 0x373f6——对自身 bss 的 not-present 写。

### 1.2 机制（dbg 轮 Instrumentation 全链, 工件 console-mv3cf-dbg*.log）

三腿合成, 每腿单独看都"合理":

1. **首段收编 ✓**: 解释器首 PT_LOAD（addr==0, total_size=0x38000, MAP_PRIVATE
   file）经既有 auto file 臂 → 窗口 0x100000000000, PMD-round 2MB, FILE record
   全帧声明（file_attach ok）。
2. **text 腿 → punch 全记录抹除**: elf_map() 以 MAP_FIXED 把 text 装进
   [base+0x1000, base+0x29000)——落在首段 record 的 2MB 声明内部 → admission
   弃权（旧码 overlap 即拒）→ punch route CHUNK。W-7 的 punch 对单帧记录
   unpublish 全部成员, 而既有 re-anchor 只认"头被 punch"一种（start<=ar->start）;
   内侧 punch 后 **头页 [base, base+0x1000) 成 tree-free/record-free 洞**
   （phdr/AT_PHDR 读取面, 静默地雷）; 尾段 rodata/RW 随后各自 admission 收编
   （trace: mmap_route ret=1 ×2）。
3. **bss 腿绕过一切路由**: elf_load() 的 bss 用 **vm_brk_flags()**——直呼
   do_vmi_munmap/do_brk_flags, 不经过 do_mmap → 无任何 route 看见 → 窗口域内
   一棵普通匿名 VMA, 无 record 无 implant。首触即撞
   `corten_fault_window_maperr()`: 窗口地址 + lookup NULL + implant 不覆盖
   ⇒ **不查树直接 MAPERR**（S-1 前提: 注册表+implant = 窗口域占用全 truth——
   收编世界里该前提失效）。bss VMA 明明在树上（trace: win_fallback vma=
   100000037000-100000038000）, 终结器先杀了进程。

### 1.3 修复（4 件, 全在授权文件内）

1. **in_execve 降级臂移除**（corten_arena.c）——收编本体; exec 语境的 addr==0
   file 形走既有 V-B file 臂。
2. **punch 幸存头件重锚**（corten_arena_mmap_punch 新 mirror 臂）: 单帧记录的
   内侧 punch 把记录重锚到头件 [ar->start, start)（rpoff 不移——头件映射文件
   首页; 冲账 = punch 范围+孤儿尾; 起始帧 slot 重插——同帧时属被摘集合）。
   **跨帧内侧 punch 保持 D-G'' 契约不动**（尾帧 slot/extent 留在记录上——重锚会
   把尾件悬空; corten_fault_test_punch_hole 锚原样绿）。修复头页地雷, 语义=
   "punch 不吃掉记录头件"。
3. **admission 接管 file 形 overlap**（explicit_region_route 的 overlap 弃权
   收窄到 anon）: file 形的 overlap 由 admission 自带的 punch(admitted=true)
   先拆后装——text 从 implant VMA 变成**同帧第二 FILE region**, W-5"漏斗不见
   可编码形状"方向更进一步（punch route 的 file 形 implant 生产线死亡）。
4. **bss implant 登记**（mmap.c vm_brk_flags 成功腿）: 窗口域 bss VMA 以
   corten_implant_mark() 登记——与 punch/P1b 的"漏斗外来 VMA"同一生产者契约;
   maperr 终结器与 J2 白名单双双回到单一占用真源。窗口域门 = 两次比较, 其它
   vm_brk_flags 调用者（主二进制 set_brk bss、驱动）零成本。

### 1.4 终态形状与判据

- **映像形状**（/proc/maps 实证, a2/a3/a5 三核一致）: 解释器 = 同帧 4 FILE
  region（[0,0x1000)R / [0x1000,0x29000)RX / [0x29000,0x34000)R /
  [0x34000,0x37000)RW, rpoff 0/1/0x29/0x34 渲染正确）+ bss implant VMA
  [0x37000,0x38000)。混合 file/anon、同帧多段 = W-7 multi-record 在生产路径
  首次全程承载。
- **行为**: =on 全 systemd boot; 裸 smoke 26/26 PASS; metis_eq checksum
  `2d383eeed4ceb73b` = 基准精确一致; dmesg 零 corruption 签名。
- **树账**（formal-readouts-a5.log）: gate_pass=1, j2/wl violations 0, j1_hits 0。
  白名单 delta 对账（37 walk 窗口）: wl_special **恰 3/进程**（vdso/vvar/vclock）
  + wl_stack **恰 1/进程** + wl_file ~9/walk（提示装载的 libc 5 段 + 主二进制段,
  即 metis 形 = 4+5 精确对上）+ wl_implant ~0.7/进程（bss）; **收编的解释器行
  零树行**。tree_entries（live 断言载体）按次发布被审 mm 原始计数（22~45/进程,
  组成 = 上列白名单桶）。
- **锚**: `exec_interp_route`（addr==0 file 形 in_execve 下收编断言, 顶替旧降级
  臂断言）+ `exec_interp_multiseg`(MAP_FIXED 内侧腿 → 头件幸存+同帧第二 FILE
  region+不变量); exec_default_enter 锚改新契约（in_execve 下 ret=1）。KUnit
  on×2 全绿。

### 1.5 如实登记（brief 假设证伪 + 已知限）

1. **glibc 库装载不走 addr==0**: 实测 ld.so 对每个库的首段带非零 hint
   （/proc/1/maps 全库在 mmap_base 区, wl_file=~65-70/进程的主源）→ 既有路由
   门（addr==0）永远看不见它们。库收编需要"hint 迁移到窗口"的语义决定（违反
   T0"显式地址留 legacy"契约, 超本片红线）→ **登记后续片**。故"tree_entries
   只剩三桶+brk"的字面零只对无库探针成立; 本片交付的口径 = 违例 0 + 全部树行
   入白名单桶对账（上节）。
2. **static-PIE 对齐探针形**（alignment > ELF_MIN_ALIGN: addr==0 探针 map +
   munmap + MAP_FIXED_NOREPLACE 重装）: 探针的窗口 record 会让 NOREPLACE 重装
   吃 -EEXIST。guest 无此形二进制（glibc 动态世界不受影响）; 登记。
3. MV3.a 的"下次 mmap 照常路由"注释所指的降级臂语义已由收编替代, in_execve
   在路由头部不再有任何特判。

### 1.6 =off 回归与 ledger 抖动归因

=off boot（corten=on, 无 default entry）: dmesg 零签名, smoke hook 形二进制
26/26, metis 同基准, KUnit off 套件 skip 对账精确（=基线 + 恰 2 个新锚 skip,
名字集 diff 逐项核对）。**=off hook 形 SMOKE-DRIVER 的 arena ledger 断言抖动
（±1~±25）为内核无关环境噪声**: A/B 实证——base 内核同 fresh-boot 同协议复现
同形（111→112/112→87/PASS 混合）, 我的内核直呼 smoke 二进制（无 LD_PRELOAD
环境泄漏）则 0→0 干净; 抖动源 = 驱动脚本把 LD_PRELOAD 泄给 dmesg/grep 管道,
短暂 hook 进程进出搅动账面。

---

## 2. 轴二: mmap-pf 批 mark

### 2.1 基线先行——以及一个基准事实修正

协议 = mmbench_dyn（历史 T0 臂同件, /mnt/cortenmm/bench/mmbench/）+ hook 进场,
seed 公式同 t5final（20260913+b*1009+c*97+t*7+k）, min_seconds=2, 三遍取中位,
配对 boots。**静态 mmbench 吃不进 LD_PRELOAD**——首轮"hook≈stock 假 parity"即此
artifact（诚实留档: bench/ 内 dyn-前缀 JSON 为准, 后段混入的静态 late-boot 读数
已剔除, 注 a5onS-*.json=噪声污染件）。

| cell | stock（corten=on, 无 MODE） | MODE-before（a3, hook） | 税 |
|---|---|---|---|
| mmap-pf low t4 | 0.00290994 | 0.000456644 | **-84.3%** |
| mmap-pf low t1 | 0.0266 | 0.0145 | -45% |

**发现: D29 的"-17~-22% 地板"数字已过时**——t5final（b9541335, run5 报
-21.47%/-12.05%）之后, W-2（exact mapcount）/W-7（bucket decode）/M-V（carrier/
file arms）各轮累计把 fault 通道成本抬到 ~7x stock。本片未做逐轮 bisect 归因
（登记后续）, 但基线数字如实入档: **起点不是 -17~-22%, 是 -84%**。

### 2.2 定位（全部实测, 零猜测收尾）

- **fault 计数不变**: fltwrap(getrusage) = stock 176,675 minflt/52,692 ops vs
  MODE 83,684/半 ops——**~3.3 faults/op 两臂相等**, 慢在单次成本, 不是 refault。
- **单腿全绿**: t1 分解 = mmap **MODE 3.1x 快**（0.51 vs 0.163）、pf -24%
  （0.0341 vs 0.0422）、unmap **2x 快**（0.121 vs 0.0618）、unmap-virt 2.8x 快
  ——place/park/fault 每条腿单独都不差。
- **合并 cell 崩**: mmap-pf t1 0.0145 vs 0.0266。耦合面 = **每 op 完整池生命
  周期**: arena_stats delta（28,696 ops 一轮）= pool_parks **+28,696** +
  pool_hits **+28,695** + munmap_releases +28,696——精确 1 park + 1 take 每 op。
- **税体 = re-park 臂的页表拆建**: 旧 re-park（VMA-less 窗）每次
  `corten_arena_free_ptes_novma()`——**自带一份 gather+flush 轮**（该 op 的第二
  次 shootdown 轮; stock munmap 只付一次）, 且下一 take 的首 fault 要重建
  PT 页 + PMD 页 + metadata array。

### 2.3 重构: warm park（批 mark 第一刀）

- **re-park 臂不再退役 PT 域**（park_locked 的 VMA-less 臂删 free_ptes_novma）:
  zap 仍逐槽清（PTE 全 none = 交接 pristine）, 页表留给下一 take 的 fault 直接
  用。账务不变（PT 页继续计入 mm; 退役移到注销性出口）。
- **eject 逐帧守卫退役**（pool_eject_locked 新增）: warm park 的窗口被
  P1b/prepare/P4 弹出时, 逐帧只对 **slot 已空的帧**（无同帧活 co-record）
  走 free_ptes_novma 退役——共享 PT 页在活记录头上的释放 = exit walk 的
  mixed-frame 禁忌, 守卫同款。可回收帧回收进杂志时带全 none PT 页 = 合法
  pristine。
- **契约面**: "纯预约 = frames + idle descriptor, 无 VMA、无 PT 页"收紧为
  "……+ 全 none PT 域（注销性出口退役）"; take 的"无 TLB 欠账"论证不变（park
  的 zap 已在调用者 gather 内清+flush）。锚 `pool_reuse` 改新契约
  （pt_present=true; meta 可读且全 Invalid——perf2a 的"array 存活 park"至此
  才真正成立: 旧 re-park 连 PT 页一起拆, array 的存活断言与实现矛盾, 本修复
  顺带把 perf2a 的语义落实）。

**数字（配对 boots, 同 seed 三遍中位）**:

| cell | MODE-before(a3) | MODE-after(a5) | Δ |
|---|---|---|---|
| mmap-pf low t4（dyn+hook, **同件同协议主判据**） | 0.000456644 | **0.00107134** | **+134.6%** |
| mmap-pf low t4（=on 默认进场世界, a3 静态件 vs a5 dyn 件, 指示性） | 0.000458489 | 0.0010228 | ~+123% |
| mmap-pf low t8（同上混合, 指示性） | 0.000147029 | 0.000346119 | ~+135% |
| hook 世界 t4 税（dyn 对 dyn） | -84.3% | **-63.2%** | +21.1pp |
| 默认进场世界 t4 税（dyn a5 对 dyn stock） | （静态口径 -86%） | **-64.9%** | +21pp |

（二进制口径: 静态/动态 mmbench 的绝对值不可直接互比, 表中同件配对只有第一行;
其余行各自成对、方向与幅度一致, 作指示性旁证。全部 JSON 在 bench/。）

零回归面: KUnit 194/0/6 ×2 全绿; leg cells 无恶化（unmap 0.114≈a3 0.121,
unmap-virt 0.582≈0.644, mmap 0.547≈0.51）; smoke/metis/dmesg 同轴一门。

（基准件身份披露: 上表 a5 读数采于 warm-park 首版内核; 终件 a5 与其的差异仅为
eject 路径的逐帧守卫 + pool_reuse 锚更新——bench 的稳态 park/take 热路径两版
逐指令相同, eject 为冷路径, 读数对终件有效。终件 sha 见文件头。）

### 2.4 如实报: 未达"个位数" + 残余归因与设计节

税仍 -63~-77%（t4/t8, 对 stock）。逐项归因:

1. **park zap 的 512 槽逐槽 reset 走查**（O(帧), 内容无关）: perf2a 去掉了
   wholesale drop 但保留全幅逐槽走查——16KB op 的 arena 是整 2MB 帧, walk 恒
   512 槽。**设计节（后续刀, 未动码）**: 记录脏域 rec_lo/rec_hi（corten_map
   提交时加宽, desc 写锁内 = 有序; park 时复位）, zap 的 PTE 走查与 meta reset
   都夹到脏域——依据不变式"PTE ⊆ 已 mark 范围"（PTE 写全经事务, INV6）+
   punch/implant 形状走显式范围不受影响。预期把 park 的 O(512) 变 O(dirty)。
2. **per-fault 簿记 +~1.4µs/页**（pf 腿读数）: lookup/ref/tier/desc 写锁/meta
   ensure/stats 的单页成本——批化空间在 desc 锁粒度与 stats 合并, 属独立小片。
3. TLB 形状已与 stock 同价（warm park 后每 op 单轮 flush）; 剩余差距主要是 1
   与 2 的和, 外加 t4/t8 的锁竞争放大（desc 写锁跨线程串行化同窗 fault）。
4. **逐轮 bisect 归因登记**: "地板何时从 -21% 涨到 -84%"未定（候选 W-2/W-7/
   M-V）, 对 MV3.d 的全系统电池判读有基准意义, 建议独立小片（build b9541335 +
   中间点回放本协议）。

---

## 3. 验证链（终件 a5）

| 门 | 结果 |
|---|---|
| =y 构建 | 零新增警告（唯一 warning = stock objtool cpuidle, 前代在案） |
| KUnit on×2 | **绿 ×2**: 24/0/1 + **136/0/0**（含 2 新锚 + pool_reuse 新契约）+ 34/0/5 |
| KUnit off | 绿: interlock flake 复跑 25/0/0（在案姿态）; skip 对账 = 基线 + 恰 2 新锚 skip（名字集 diff 空） |
| KUnit 噪声口径 | 24 条 pgtables WARN + 1 条 INV-MV2 WARN = **基线（mv3c-debug #344 自身 log 同值 23-24 条）随行的 harness 人造 mm 形状**, 非本片引入（a3/a4/a5 与基线四组同值） |
| checkpatch | **0E/0W**（全 delta 294 行, "no obvious style problems"） |
| =n | CONFIG_CORTEN_MM=n 构建 rc=0; 消费对象 nm 零 corten 符号（mmap/memory/exec/fork/migrate/rmap）; 陈旧 .o 归档 n-stale-objects*/ |
| guest =on | 全 systemd boot; 裸 smoke 26/26（driver ledger 抖动 = §1.6 已归因形态, 披露）; metis 同基准; dmesg 零签名; gate_pass=1 |
| guest =off | dmesg 零签名; smoke hook 26/26; metis 同基准 |

## 4. 收口清单 vs 待办

已收口: 两轴代码全部在树（未 commit, 主会话收口）; 工件全档 results/r07/mv3cfeat/
（51 件: bzImage 全线 10 枚、console 17 份、kunit 各轮、bench JSON、formal
readouts、A/B smoke 件、full.diff、n-verify、脚本 mv3cf-guest/mv3cf-kunit/
kunit-one.sh）。

待办/移交:
1. 轴二第二刀: 脏域有界的 park reset 走查（§2.4-1 设计节在案）。
2. per-fault 簿记批化（desc 锁粒度/stats 合并, §2.4-2）。
3. 地板演化 bisect（§2.4-4）。
4. 库 hint 装载收编语义裁决（§1.5-1, 超红线登记件）。
5. static-PIE 对齐探针形（§1.5-2, 低优先）。
6. eject 退役的 warm 窗口在 free_pgtables 侧的 8192 类残账口径并入既有登记
   （§3 噪声口径同源, house 排除）。

## 5. 明示不做（ponytail 边界）

- hint 迁移收编（动 T0 显式地址契约）: 红线外, 登记裁决。
- pool 元数据的 epoch/世代方案（第二阶段批 mark 的重炮）: 脏域有界走查是更小
  的等价第一步, 先量它。
- 窗口域 fault 终结器（corten_fault_window_maperr）的通用树回退改造: 本片用
  implant 登记闭合了 bss 形, 通用改造留给有真实第三类租户的片。
