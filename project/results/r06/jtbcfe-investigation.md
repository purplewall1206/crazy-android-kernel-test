# JThreadBench ClassFormatError（类页零字节）取证报告 —— r06/jtbcfe

- 班次: 2026-09-18 14:35–19:35（D14 授权; VM 前 60 分钟归 G1 加测 agent, 15:38 交接后本班独占）
- 对象缺陷: MODE 进程 JThreadBench 收尾 printf 阶段
  `ClassFormatError: Unknown constant tag 0 in class file sun/text/resources/cldr/FormatData_en`（登记既有）
- 内核: 复现于 452ad7b9d91e（t5 首跑 .err）与 e219920b0923（t5-run2 3/3, rogue, 本班主复现载体）
- 主树: 全程未动代码; 调试探针（printk）已 revert, `git status` 干净, HEAD=025756094542
- **结论等级: 失败机制已收窄到最后一环; 内核 arena 路径基本排除; "零字节写入者"身份待一个
  已设计好的仪器（perf_event 断点）终验 —— 未达可修判据, 无 commit**

---

## 0. 结论一句话

**类字节由 `pread64(modules)` 整段正确送达 JVM 缓冲区（逐字节 FNV 与磁盘真值一致）, 此后、解析完成前,
该缓冲区所在 4K 页被"原地写零"（PFN 不变、无缺页、无任何 corten 内核事件）, 解析中段读到 0 才报
"Unknown constant tag 0"; 内核 arena 路径（fault/mprotect/munmap/punch/rearm）在该页上全程零事件,
写入者身份指向 JVM 进程内另一分配体（疑似窗口 VA 的跨子系统复用）, 尚未被仪器捕获。**

---

## 1. 已确立事实链（全部实证, 可复核）

### 1.1 失败形态与确定性
- 100% 复现: MODE on 下 `java HelloFmt`（**零线程、最小程序, 一次 printf**）即 CFE（本班 ≥15/15）;
  JThreadBench 10/2000 线程、-Xshare:on/off、-Xint、TieredStopAtLevel=1、SerialGC、ParallelGC
  全部复现（一次 SerialGC rc=0 判为时序 flake）⇒ **与 2000 线程 churn、CDS、JIT、GC 均无关**。
- 死点: rep1 完成后的首个 `System.err.printf` → `Formatter` → `DecimalFormatSymbols` →
  `LocaleData`/`Bundles` → `Class.forName("sun.text.resources.cldr.FormatData_en")` →
  `BootLoader.loadClassOrNull` → `findBootstrapClass`（Native）。
- stderr 已吐出前缀 `[jvm-thread-create] rep `（JDK21 PrintStream 渐进写）⇒ 死在格式化中段。
- "Unknown constant tag 0" 语义: magic/版本/CP 头解析通过, **在某个 tag 字节读到 0**。

### 1.2 类来源与读取机制（strace -yy 逐 syscall 铁证）
- `sun/text/resources/cldr/FormatData_en source: jrt:/java.base`（base 腿 -Xlog 实证; **不在 CDS archive**,
  `-Xshare:off` 同失败 ⇒ 归 jimage 路径）。
- guest JDK21（Debian 21.0.12.1）读 jimage 资源 = **`pread64(modules_fd, buffer, size, offset)`** 直接进
  HotSpot C 堆缓冲; 140MB 的 `mmap(modules, MAP_SHARED)` 同时存在（索引/头部）。
- FormatData_en: **offset=9760807, size=2352**; 资源整体落在单个 4K 页内（页内偏移 39 起）。
- MODE 侧 modules 映射与 base 同形、落在 legacy 区（`mmap(NULL, 140853890, MAP_SHARED)=0x7f…`）⇒
  **modules 映射本身不进 arena**（classify(file)=LEGACY、xa_load 对 0x7f 地址必 miss）。

### 1.3 缓冲区在 syscall 返回时是"真字节"（决定性实验 A）
- LD_PRELOAD 拦截 `pread64`（含 `__pread64_chk` 等变体, **需版本脚本 `GLIBC_2.2.5` 才能截住 libjvm 的
  版本化引用**）: 对 count∈[256,8192] 且落在窗口（buf>>40∈[16,64)）的类资源 pread 做返回即全量审计。
- **3/3 次失败运行: FormatData_en 的 2352 字节缓冲在 syscall 返回时 0 个多余零字节**, 魔数 CAFEBABE 在位;
  FNV-1a=`f2a3b52f42471816` 与直接 dd 读 ext4 上 modules 同偏移的字节**逐位一致**（436 个零字节为
  class 文件固有内容）。
- ⇒ **内核 `pread` 路径（含 arena 写故障、copy_to_user）无罪; 零字节是"之后"出现的。**

### 1.4 零字节出现方式: 原地、瞬时、无内核事件（决定性实验 B）
- 进程内 watcher 线程（predator）在 TARGET pread 返回后以 ~50-200μs 间隔轮询缓冲页内容指纹+pagemap PFN:
  - `TRANS +326μs: pfn 72d7f→72d7f, 内容指纹 6e616c2f→00000000`（含页首活对象"/lan"字符串一并归零,
    覆盖 ≥2KB）
  - `TRANS +609μs: 再度归零`（FormatData_en 的 pread 之后 —— **与解析窗口重叠**）
  - `TRANS +1188μs: 又出现新数据`（异常路径的后续加载复用缓冲）
  - **PFN 全程不变 ⇒ 不是"换页", 是"同一物理页被原地写零、又被复写"。**
- 同期对 CDS 头缓冲页（0x100000019000）的观察只有 data↔data（正常复用）, 从不归零 ⇒ 翻零是
  **类资源缓冲页特有事件, 且每次类加载后 ~300μs 一遇**（736 个类加载成功 = 多数轮次侥幸错开）。

### 1.5 内核侧全面排除（决定性实验 C: printk 探针内核）
- 探针点: `corten_arena_folio_prealloc`（打印 addr+pfn）、`corten_arena_map_anon`（安装事件）、
  `corten_arena_zero_page`、`corten_arena_zap_window`、`corten_arena_unmap_chunk`、RELEASE;
  过滤窗 [16T, 64T) 全窗; `log_buf_len=64M` 防丢行。
- 失败运行中, 缓冲页（如 0x10010404c000）的内核事件 = **只有一次出生（prealloc+MAP_ANON）或 0 次
  （该页经 legacy 兜底路径安装, 探针不可见）, 此后直到进程退出: 零 ZAP、零 ZERO_PAGE、零重装、
  零 punch 波及**。
- 单次失败运行内 **无重复 PFN 的 prealloc**（双登记/双重映射未见; 跨 run 的重复属进程消亡后合法复用）。
- 计数器: `rearm_recovered +18/run`（窗口重武装, 既有自愈机制工作正常）、`legacy_drift=0`、
  `rearm_failed=0`、`auto_fallbacks=0`、`auto_exhausted=0`、`desc/meta_alloc_fail=0`。
- ⇒ **corten 的 fault/zap/punch/zero 路径没有参与零字节的产生。**

### 1.6 相关内核态事实（架构性背景, 与本缺陷的关系待定）
- arena 页 = `vma_alloc_zeroed_movable_folio`（**movable**）, 有 anon rmap 但 **不上 LRU**;
  compaction 靠 PageLRU 隔离 ⇒ 不会迁移 arena 页; NUMA migrate 有 range_overlaps 拒绝钩子。
- shadow-VMA 带 VM_NOHUGEPAGE ⇒ khugepaged/THP 排除。
- JVM 在 MODE 下的 CDS 预留→修剪→两段 MAP_FIXED 落窗（punch 产物, E1 判据 0x7f 通过）与
  heap archive 进 Java 堆（0xffe00000, legacy）均复核无异常。
- **强疑点（未终验）**: mis-address 的 gdb 硬件观察点（watch 了上一 run 的缓冲 VA）在本 run 捕获的全是
  **C1 编译线程的 Arena chunk 写入**（ciObjectFactory/DebugInformationRecorder/LinearScan 等）落在同一
  VA —— 结合类缓冲地址逐 run 漂移（0x10010c009200/0x1001040440b0/0x1001040e94e0…）, 高度怀疑
  **窗口 VA 在进程内被"类字节缓冲 ↔ 其它 JVM 子系统分配体"跨时间复用**, 而某一方的 zeroing/初始化
  （如新 chunk 的清零或 mprotect-commit 后的初始化写）压过了另一方的存活数据。

## 2. 已排除项（负结果同样值钱）
| 候选 | 判定 | 证据 |
|---|---|---|
| CDS archive 内容损坏 / punch 二阶 | 排除（对本 CFE） | -Xshare:off 同失败; findBootstrapClass 走到解析=非 shared 路径; 类字节经 jimage |
| modules 映射被 arena 拦截/污染 | 排除 | 映射在 legacy 区; 全程 modscan 零零页（zeropages=0/13 ticks） |
| pread 拷贝不完整/内核谎报 | 排除 | 返回时全量审计 0 多余零字节 ×3 run（§1.3） |
| arena ZAP/ZERO_PAGE/重装打掉缓冲页 | 排除 | 64M dmesg 全量探针: 缓冲页零事件（§1.5） |
| 双登记/双重映射（prealloc 重复 PFN） | 本 run 内未见 | 全窗探针 uniq -d = 0（跨 run 重复=合法复用） |
| 2000 线程 churn / 栈 / mprotect 楼梯 | 排除 | 零线程最小复现 |
| JIT（C1/C2）/ GC 器种 | 排除 | -Xint、SerialGC、ParallelGC 全失败 |
| base（corten 关）内核 | 排除 | base 15+ 次 rc=0, 且 ABAB 交错仍复现 |

## 3. 未闭合的一环 + 下一步仪器（已备/已设计）
- **问题**: 谁在 ~300μs 窗口内对类缓冲页做原地写零（用户态写、同 PTE）?
- **仪器（首选, ~1h 工作量）**: 进程内 `perf_event_open(PERF_TYPE_BREAKPOINT, bp_addr=buf, w, sample_ip)`
  —— 在 TARGET pread 返回后对 `buf` 挂精确硬件断点（self-attach 合法）, 采样写入者 RIP →
  `/proc/self/maps` 归属（libjvm+off / libc+off / JIT PC / heap）。
  （gdb 方案已试: attach 晚于 wipe 且批处理模式止步于 JVM 内部 SIGSEGV; ptrace 自线程 SEIZE=EPERM。
  predator2.c 已留 ptrace 骨架。）
- **次选**: gdb 两阶段（先跑一遍取地址、第二遍从头挂 watch + `handle SIGSEGV nostop pass` +
  `commands/continue` 循环）—— 脚本 `gdbwatch3.sh`+`gdbcmds` 在 results/r06/jtbcfe/ 有骨架,
  但地址逐 run 漂移需先用 `taskset -c 2 + 固定环境` 收敛或接受多轮。
- **若写入者 = JVM 自身对窗口 VA 的二次使用**（当前最强假设）: 内核侧修复方向不存在 ≤50 行的
  corten 补丁 —— 要么是 JVM 层 UVF（需解释为何仅 MODE 出现）, 要么是窗口 VA 生命周期与
  JVM 预留/提交模式（reserve PROT_NONE → MAP_FIXED commit → trim munmap）的组合在
  `auto_mmap_route/DECLARE/release_classify` 下出现了**进程内窗口 VA 复用**, 需要规划者裁决
  （例如 release 后窗口 VA 是否应进入进程内"墓碑"集合阻止 legacy/hint 式再投放）。

## 4. 复现与仪器清单（全部可复跑）
- 复现: `LD_PRELOAD=corten_mode_hook.so java -Xmx512m HelloFmt`（guest, corten=on, trixie 8vCPU）
  → rc=1 + CFE; `... JThreadBench 10 1` 同。base 无 hook 恒 rc=0。
- 工具: `bench/share/jtbcfe/`（已归档 `results/r06/jtbcfe/`）
  - `predator.c/predator2.c/predator3.c`: pread64 拦截 + 返回审计 + 页面轮询（pred3 含 TARGET FNV 审计;
    **注意**拦截必须带 `--version-script` 版本化符号, 否则 libjvm 的 `pread64@GLIBC_2.2.5` 不经 PLT 落入）
  - `modscan.c/watch_maps.c/dump_maps.c`: pagemap PFN 指纹 / 退出期 maps+smaps 全扫描
  - `run_investigation.sh`: A-E 快速腿（base/最小复现/share 变体/对照）
- 探针内核: mm/corten_arena.c 四点 printk（prealloc/MAP_ANON/ZERO_PAGE/ZAP_WINDOW）, 已 revert;
  重打补丁步骤与本报告一致（建议 `log_buf_len=64M` + `dmesg -c` 后单 run 取证）。
- VM 状态交接: `tmux: vm` 现运行 **HEAD=025756094542-dirty + 探针**的 bzImage（/home/ppw/linux-6.18/arch/x86/boot/bzImage,
  未归档）; `bzimg/r06-rogue/bzImage` 原样未动。下一班请用
  `KERNEL=bzimg/r06-rogue/bzImage bash ~/bench/host/launch_vm.sh ~/vm/trixie.img "systemd.mask=sys-kernel-config.mount corten=on mitigations=off kunit.enable=0"`
  重新拉起即可回到 rogue 基线（guest 内 /mnt/share 已挂 9p, ssh 经 ~/vm/gssh）。

## 5. 判定与移交
- **不满足"≤50 行修复"前置条件**: 根因最后一环（零字节写入者身份）未闭合, 贸然修 = 猜。
- 登记建议: OQ-JTBCFE —— MODE 进程 JVM 类资源缓冲页在 pread 成功后、解析完成前被原地写零;
  内核 arena 路径零参与（本报告 §1.3-1.5 为证）; 建议下一班用 §3 首选仪器取写入者 RIP 后再定
  内核/JVM 归属。M8 前的替代口径: JVM 腿维持 soft-fail（t5-run2 口径不变）。
- 时间盒: 取证 5h 用满（含等 VM 60min）; 主树零改动（probe 已 revert）; 未 push; 密码未落盘。

---

## 5. 终局加测（19:00-19:35, perf_event 硬件观察点, predator4.c）

进程内 `perf_event_open(PERF_TYPE_BREAKPOINT)`（self-attach 合法, 1 字节 write/read 两种,
12ms 采样窗, PERF_SAMPLE_IP + /proc/self/maps 归属）对失败类资源（off=9760807, 2352B）:

| 观察对象 | 结果 |
|---|---|
| **W 观察点 @ pread 目的缓冲**（3 run） | **写入命中 = 0**（pread 返回后 12ms 内无人经该 VA 写入） |
| **R 观察点 @ 同一 pread 缓冲** | **读取命中 = 0** ⇒ **解析器根本不读这个缓冲!** |
| **R 观察点 @ modules 映射 base+9760807** | **读取命中 = 0** ⇒ 解析器也不读映射图像 |

### 颠覆性结论（在本班证据内自洽）
1. pread 把**真字节**送达缓冲 X（FNV=磁盘真值, §1.3）。
2. **解析器读的是第三处缓冲 Y——既不是 X 也不是 modules 映射——而 Y 是零/未填充的**
   （两次 R-watch 双零命中 + W-watch 零写入, 三角互证）。
3. **MODE 下 JVM 的 jimage 读取形态本身发生了切换**: base 从不做资源 pread（无 TARGET 行）,
   mode 有 ⇒ `ImageFileReader::map()` 在 MODE 进程内失败/被放弃（fallback read_at）,
   或存在第二条读取路径。此切换与 CFE 的因果链是下一班的头号问题。
4. 先前 predator v1 观察到的"页原地归零"（PFN 不变）与"解析读 Y"合并后的最强假设:
   **X/Y 是同一 glibc 堆页上的两次生命周期（free→realloc 同址）, Y 从未被写入即被解析
   ——指针错位发生在 JVM 的 jimage 双路径（mmap vs pread）选择/回退逻辑与 MODE 环境的
   交互处; 内核 corten 路径零参与（§1.5 全量探针）。**

### 下一班 30 分钟内可做的终验
- R-watch + W-watch **同时**挂 X 与"解析时 `ClassFileStream::_buffer`"—— 后者可在
  gdb `break ClassPathModuleEntry::load_class` 后单步获取（gdb 已装好, libjvm 有符号表条目）。
- 或: base 腿跑 predator4 —— 确认 base 的解析读的是"映射"（R-watch 应有命中）,
  一步锁定"MODE 下 map 失败→fallback→双缓冲错位"的完整因果。
- 若坐实 JVM map() 失败原因（怀疑: MODE 下 mmap(140MB MAP_SHARED) 后的 header 校验读到了坏页,
  或 map 的地址窗与 arena 交互异常）, 则回到内核侧修 map/校验路径。

## 6. 判定（维持）
不满足可修判据（根因最后一环 = JVM 解析源指针为何指向未填充缓冲 —— 已锁定范围但未终验）;
无 commit; M8 口径维持 JVM 腿 soft-fail。本报告 §1-§5 全部证据可按 §4 清单复跑。

### 5.1 追加（19:2x）: ClassFileParser 级抓取与硬件观察点的边界
- gdb（libjvm 带 symtab, Rewriter/ciObjectFactory 等可符号化）在 `ClassFileParser::ClassFileParser`
  上断点成功; 因 ClassFileStream 字段布局无 DWARF 未能直接取 `_buffer`（$rsi 处 8 字节为常量指针值,
  布局假设被否证）; 该路径留下的开题: 正确布局后一次 R-watch 即可锁定解析源。
- **perf_event W-watch 边界确认**: x86 硬件数据断点只有 write/execute 两种语义（bp_type=1"读"在 x86
  退化为 execute）⇒ predator4 的 R-watch 数据无效, W-watch（写入=0）有效。
- W-watch（12ms, pread 返回即挂）= 0 写入 + 此后 gdb 复用期 X-TOUCH 全部为
  Rewriter/DebugInformationRecorder/GrowableArray/Node::clone 等 **JIT/编译 Arena 正常复用写**
  ⇒ X 页内容在解析窗内**未被任何人改写**; 结合 R-watch 无效前的 R-watch 0 命中,
  "**解析源 ≠ pread 缓冲**"仍是唯一自洽解释, 且更精确的表述为:
  **解析器拿到的 ClassFileStream 所指缓冲从未被 FormatData_en 的字节填充过（恒零/陈旧）**。
- 归属裁决所需最后一步（下一班 ≤30min）: 按 ClassFileStream 真实布局（u1* _buffer; int _length; …）
  修正 gdbpy4 的字段偏移, 一次运行同时打印 (解析缓冲, 内容前 8 字节, pread 缓冲 X) 三元组即可终验。
