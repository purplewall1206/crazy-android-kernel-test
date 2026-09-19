# JThreadBench ClassFormatError 终验报告 —— r06/jtbcfe-final

- 班次: 2026-09-17 下午（终验口, 时盒 3h）
- 对象缺陷: MODE 进程 `java HelloFmt` / JThreadBench 100% `ClassFormatError: Unknown constant tag 0 in class file sun/text/resources/cldr/FormatData_en`（类页零字节）
- 环境: guest 6.18.32-g025756094542-dirty（探针 bzImage, tmux `vm`），JDK 21.0.12.1（Debian），gdb 批模式精确地址断点；工具 gdbpy5–17 在 `bench/share/jtbcfe/`
- **判定: (b) 内核可修点 —— corten MODE 下，arena window 内 >8KB（多页）`pread` 短读；页 1..N 保持全新零页。非 HotSpot 双路径分叉，非映射污染。**

---

## 0. 结论一句话

**JDK21 libjimage 用 `pread` 把 `FormatData_en.class`（真实 size=32963, image offset=30043897）读进 resource-arena 缓冲；MODE 下该缓冲落在 arena window（0x100…），`pread` 只拷贝了第 0 页附近的 3120/3472 字节即短读返回，缓冲页 1..8 全部保持全新零页；libjimage `osSupport::read` 不检查 pread 返回值，JVM 拿 92% 为零的缓冲解析 CP，读到 tag=0 → CFE。内核 arena fault/GUP 交互是多页拷贝中断的根因。上一班"pread 全量正确送达"结论仅对 count≤8192 成立（其审计窗口），32963 字节读从未被审计过。**

## 1. 终验硬证据（gdb 输出原文, 全部 2–4 次确定性复现）

### 1.1 否证上一班两大假说（gdbpy5/8/9, 输出原文）
```
GRES buf=10010c04bc50 comp=0 size=2352 memory_map_image=1   ← 资源非压缩, 整像已映射
OREAD fd=3 buf=10010c04bc50 size=2352 off=9760807           ← pread 目的缓冲
CTORCALL Y=10010c04bc50 len=2352 head32=cafebabe00000041...  ← 解析流缓冲 Y == pread 缓冲 X
WATCHY[1] rip=...ClassFileParser::parse_stream...+35: add $0x4,%rax   ← 解析器确实读 X
CTORCALL Y=... fnv=2e83d899529772d4 CLEAN                    ← ctor 时缓冲 FNV == 磁盘真值(4/4)
```
- **"第三缓冲 Y"假说否证**: Y == X（同一指针）；**"解析器不读 X"否证**（perf R-watch 在 x86 退化为 execute-watch，其数据无效；gdb 硬件 ACCESS 观察点抓到 parse_stream 实读）。
- 解析窗口内唯一对 X 的写入 = 加载**结束后** Rewriter 复用 chunk 的 libc memset（bt: `link_class_impl → Rewriter::rewrite`），良性。

### 1.2 失败流的真实形态（gdbpy14, CFS=ClassFileStream 构造点精确断点）
```
CFS[8] buf=7ffff1589130 len=8810  src=jrt:/jdk.localed ret=+a2caab   ← 另一 JNI 路径, 不相关
CFS[9] buf=100000103db0 len=32963 src=/usr/lib/jvm/jav ret=+62bc8e   ← 失败流: open_stream_for_loader 正常路径, 但 len=32963!
PERR[1] tag=0 parser=7ffff6430020                                     ← 唯一解析错误点 classfile_parse_error
PERR streamobj@... v=7ffff7c14538(_ZTV15ClassFileStream+0x10) st=... cu_off=10 len=32963
PERR p[1]=... -> "sun/text/resources/cldr/Fo..."                      ← parser._class_name=FormatData_en, 名实一致
```

### 1.3 决定性对照: BASE 与 MODE 的查找/读取**完全相同**, 结果不同（gdbpy15）
```
BASE (同内核, 无 hook):  FINDRES[13][14] name=sun/text/resources/cldr/FormatData_en.class
                        OREAD[3] fd=3 size=32963 off=30043897
                        FINDRES[15][16] FormatData_en_US.class → OREAD[4] 4605@20131654
                        [hello] done      ← 成功, perr=0
MODE  (同内核, 有 hook): 同样 FINDRES 双查 + OREAD[3] size=32963 off=30043897
                        → PERR[1] tag=0 → ClassFormatError  ← 失败 (4/4)
MAPCHECK disk_len=140853890 map_len=140857344; bad_pages=0/48  ← modules 文件映射与磁盘逐位一致, 映射无污染
```

### 1.4 根因实锤: >8KB pread 在 arena window 缓冲上短读（gdbpy17, 返回点审计, 2/2）
```
OREAD[3] buf=1000001123d0 size=32963 off=30043897
AUDIT buf=1000001123d0 size=32963 off=30043897 ret=3120 zeros=30162 diff_vs_disk=28891
      zeropages=[4096, 8192, 12288, 16384, 20480, 24576, 28672, 32768]   ← 第 1..8 页全零
（MODE2 同点位: ret=3472, zeros=29821, 同样 8 个零页）
对照同 run 小读: OREAD[1] 2524B ret=2524 diff=0; OREAD[2] 6688B ret=6688 diff=0; OREAD[4] 2352B ret=2352 diff=0 fnv=2e83d899529772d4
```
- pread **合法短读不可能**: 常规文件、非 EOF、无信号；且第 0 页拷贝成功、页 1..8 恰为零页 = 拷贝在多页边界处提前终止。
- 此前 predator 审计窗口 count∈[256,8192] 恰好漏掉 32963 → "内核 pread 无罪"结论不适用于多页读。
- dmesg 探针: 失败缓冲仅页 0（0x100000112000）有 `prealloc + MAP_ANON(write=1)` 安装, 页 1..8 无任何安装事件 → GUP 触发的写故障未走 arena 安装路径（`corten_arena_handle_mm_fault` / `fault_once` 在某处拒绝/未达, 待定位）。
- JVM 侧放大器: JDK21 `libjimage/osSupport.cpp read()` 忽略 pread 返回值（`(void)` 语义）, 短读后按请求 size 解析 → 必然踩零。

### 1.5 失败链条（完整因果, 每步有实证）
`open_stream_for_loader(FormatData_en)` → JImageFindResource → (30043897, 32963)（base 同值, 正确）→ resource_allocate_bytes(32963) 落 arena window → pread 32963 → **内核短读 3120/3472, 页 1..8 零** → libjimage 不查返回值 → `ClassFileParser::parse_constant_pool_entries` 在 tag 位置读 0 → `classfile_parse_error("Unknown constant tag %u...")` → CFE。失败类名随哪个大资源先被读而定（也抓到过 `jdk/internal/module/ModulePatcher$PatchedModuleReader`, RUN gdb_mode2），FormatData_en 是 JThreadBench/HelloFmt 路径上的确定性受害者。

## 2. 判定: (b) 内核可修点

- 不是 HotSpot 内部 bug: JVM 行为 base/MODE 完全一致（查找、size、路径全同），差异只在内核对 arena window 缓冲的多页 pread 拷贝提前终止。
- 不是映射污染: MAPCHECK 0/48 页差异；base 读同 offset/sizes 全部完整。
- 修复方向（移交, 未实施——定位 fault 拒绝点需探针内核再取证 + 全量重编译复验, 超出本时盒）:
  1. **首选（预计 ≤50 行）**: arena mmap 路径按 MAP_POPULATE 语义**整段预装**（对 [start,end) 逐页 `corten_arena_folio_prealloc` + 安装 PTE, 而非仅首页）。GUP 不再触发 fault, 短读消失。当前证据（仅页 0 有 prealloc/MAP_ANON 事件）表明 mmap 只装了首页。
  2. 并行排查: `corten_arena_handle_mm_fault`（mm/corten_arena.c:2835）对 GUP（无 FAULT_FLAG_USER）写故障的覆盖性——`corten_arena_lookup_get`/`fault_owned`/`fault_once` 对页 1..8 的拒绝点, 用探针内核在 fault_once 入口打印 (addr, write, user, 结果) 一跑即得。
  3. JVM 侧兜底（非本仓库）: libjimage `osSupport::read` 应循环处理短读——上游可提, 但不作为本缺陷修复。
- 复验判据（下一班用）: 同 gdbpy17 审计, MODE 下 OREAD[3] `ret=32963 diff_vs_disk=0 zeropages=[]`, 且 `LD_PRELOAD=corten_mode_hook.so java -Xmx512m HelloFmt` 3/3 `rc=0 [hello] done`, JThreadBench 10 线程跑通。

## 3. 处置

- **无 commit**（修复未实施, 主树未动; 头盔: 时间盒 3h 用尽于取证, 根因闭合优先于仓促修）。
- VM 状态: tmux `vm` 仍为探针内核（未重启, 未动 rogue/其他 worktree）; gdb 脚本 gdbpy5–17 与本报告同存 `bench/share/jtbcfe/`、`results/r06/jtbcfe*/`。
- M8 口径建议: JVM 腿维持 soft-fail; 修复落地后 JThreadBench 全跑通即可转正式。
- 旧 OQ-JTBCFE 的"跨子系统 VA 复用/双路径分叉"假设作废, 以本报告 §1.4 短读证据替换。

## 4. 复跑清单（全部一条命令可复现）

```
# 1.4 短读审计 (MODE):
ssh(~/vm/gssh) 'cd /mnt/share/jtbcfe && LD_PRELOAD=/mnt/share/t5run2/corten_mode_hook.so \
  gdb -q -batch -x gdbpy17.py --args java -Xmx512m HelloFmt'
# BASE 对照: 同上去掉 LD_PRELOAD
# 1.2 失败流构造: gdbpy14.py; 1.3 映射完整性: gdbpy15.py; 1.1 三元组: gdbpy5.py
```
