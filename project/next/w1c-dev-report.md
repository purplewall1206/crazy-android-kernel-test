# W1.c 开发报告 — file 装摘点翻转：rmap 调用点从 carrier 形态切到 novma wrapper（MV2 W1.c）

日期: 2026-09-22 · 树: `/home/ppw/linux-6.18-mva`（分支 mv-a0 @ 0c47a0515a8a，**未 commit**，工作区即交付物）
补丁: `/home/ppw/cortenmm/patches/r07-w1c.diff` · 日志: `/home/ppw/cortenmm/results/r07/w1c/`
规格: `specs/W1_NATIVE_RMAP_SPEC.md` §3.1 R3/R4/R7(R10)、§5 W1.c 行、R-W1-4、红线表 1（INV6）

---

## 0. 结论一句话

file 页的三个 rmap 调用点按 §3.1 翻转到 W1.a 的 novma wrapper——R4 装页
（`folio_add_file_rmap_pte` → `folio_add_file_rmap_novma`）、R3 COW 旧 file 页、
R7/R10 zap/exit file 臂（两处 `folio_remove_rmap_pte` 的 file 分家 →
`folio_remove_file_rmap_novma`）；fork dup（R9）核实为零改动、swap-out/in（R8/R6）
论证为结构上无 file 臂（W1.e 范围）。生产码 +34/−14、新 KUnit 用例 +265，
R-W1-4 的"共享 pagecache folio 双侧计数"以**真 legacy mapper**（漏斗原样 fault）
为 fixture 精确对账，三套件 on1/on2/off1 全绿，=n 折叠、checkpatch 0E/0W。

## 1. 改动点清单（2 文件，+299/−14）

### 1.1 mm/corten_arena.c +34/−14（三处调用点翻转 + 注释对齐）

| 点 | 位置 | 翻转 |
|---|---|---|
| R4 装页 | `corten_arena_file_read()` commit 段 | `folio_add_file_rmap_pte(folio, page, vma)` → **`folio_add_file_rmap_novma(folio)`**。仍在既有事务临界区内（desc W > ptl，INV6 零新增写点——rmap 调用与翻转前同位）。注释登记 order-0-only 依据：本路径 pagecache 形态恒 order-0（tmpfs huge 默认 never；FGP_CREAT fetch 分配 order 0），wrapper 的 VM_WARN 是越界 tripwire |
| R3 摘除 | `corten_arena_cow_write()` copy 分支 | `if (vma) folio_remove_rmap_pte(old,...)` 分家为 **file 臂无条件 `folio_remove_file_rmap_novma(old)`**（与翻转后装页同为 vma-free，数值同为 −1 mapper）；anon 臂保持 `else if (vma)` 门（与 map_anon 的 `if (vma)` 装页对称，翻转留 W-2）。counter 分族 `old_is_file ? mm_counter_file : MM_ANONPAGES` 原样（V-B.4） |
| R7/R10 摘除 | `corten_zap_release_page()`（chunk zap / exit walk / B.2 失效枚举的共同收口） | `if (vma) folio_remove_rmap_pte(...)` 分家为 `if (vma) { anon ? 旧调用 : **novma remove** }`。counter 分族块（`!vma || anon → ANONPAGES`）原样——vma-ness 仍是 V-A.1 无锚页（未标 anon）与 file 页的判别器。函数头过时句子（"covering VMA is resolved"旧 i_mmap 论证）改写为 W1.c 形态 |

**不翻转（核对保持）**：
- **R9 fork dup**：6.18 上游 `copy_present_ptes` 的 file 臂已是无条件 `folio_dup_file_rmap_pte`
  （try 变体在上游已消失），order-0 分支是裸 `atomic_inc(&_mapcount)`、rss 族由调用方
  `rss[mm_counter_file()]++` 记——与 novma 加/摘在数值上逐位同源，零改动即精确对账。
  现码 fork 走 carrier 对的上游复制路径保持（W-2 才 metadata 化）。
- **R8 swap-out / R6 swap-in**：`corten_rmap_swap_out` 的 guard 只放行 swapcache order-0
  folio（`!folio_test_swapcache` → 拒绝+计数）；窗口 file PTE 是 pagecache，结构上不可达
  （W1.b 后 ttu 也枚举不到 carrier，mapcount pin 使回收对该 folio 干净失败——过渡态
  姿势不变）。swap-in 的 SWAPPED meta ⇒ anon folio。两臂均 W1.e/W-2 范围，本片不动。
- **W1.a wrapper 主体（mm/rmap.c）与 W1.b 注册表零触碰**（diff 仅 2 文件机器可证）；
  B.2 even_cows 接线原样——失效事件经 W1.b 枚举 → `unmap_chunk_flags(KEEP_PERM)`，
  其 zap 收口自动获得翻转后的 file 摘除。

### 1.2 mm/corten_arena_test.c +265/−0（新用例 + 两个 worker op + 前置声明）

- `corten_arena_test_op_legacy_file_map/_unmap`：真 legacy mapper fixture——NOWHERE 处
  `do_mmap(MAP_SHARED|MAP_FIXED, PROT_READ)` + `handle_mm_fault()` 逐页 prefault
  （GUP faultin 形状：无 USER 位、mmap read 持锁），跑在 attached worker 上（漏斗的生产
  形态，且 munmap 类操作必须离 case 线程）。**必须先于 mode_enter**——MODE 进程的
  file mmap 会被路由进 takeover。
- `corten_arena_test_file_rmap_shared`（注册位：file_cow 之后、fork 镜像之前；corten=off
  时 kunit_skip，计入既有 78 skip）。

## 2. KUnit 锚（task 四锚对账）

| 锚 | 落点与断言 |
|---|---|
| ① 共享 pagecache 双侧计数（R-W1-4） | 新用例主轴：legacy 两页 prefault（mapcount 1/1、MM_SHMEMPAGES 2、NR_FILE_MAPPED = filestat）→ arena read 装**同一 folio** → mapcount 恰 `+1`、mm counter 恰 +1、NR_FILE_MAPPED **不动**（见下方教训：它是 mapped-folio 计数器，首映射才进账——第二映射者不动共享账正是要锚的语义）→ 注册表 size==1 而 mapcount 不含它（注册表是枚举源不是 mapper） |
| ② zap 退役清账 | 同用例：`unmap_mapping_pages(0,1,even_cows=true)` 双腿各自精确——arena 臂经翻转 zap 摘 arena mapper、legacy 臂经 i_mmap 走查摘 legacy mapper，合计 mapcount 归零、filepg −2、NR_FILE_MAPPED −1（legacy 末映射者的账，arena 的 2→1 摘除不动它）；refault 双侧各 +1、内容仍是 pat；mode_exit 只摘 arena mapper（mapcount 回 legacy 基线、registry 归零）；legacy munmap 摘到 0/0、NR_FILE_MAPPED 回 filestat−2 |
| ③ COW 后父子各自账目 | 既有 `file_fork_cow` 骑翻转后的 R3：fork 后 mapcount 2 → 父 COW 摘除（novma）→ **1**（child 独存）→ 子 COW → **0**；私有词/页缓存词三方隔离断言原样全绿。`file_fork_mirror` 的 fork dup（mapcount 1→2、ref +1）与子 exit（exit walk 经翻转 zap → mapcount 回 1）同绿 |
| ④ 注册表与 mapcount 一致 | 同用例：attach 后 registry_size==1（region 注册不加 mapcount）；demote 后 region 幸存 registry 仍 1；mode_exit 后 0；全程 `imap_stale_refuses` 恒零（W1.b 降级组）；`truncate_routes` +1（B.2 接线在环） |

**首跑教训（已修，反而加固锚）**：初版断言"arena 第二映射 NR_FILE_MAPPED +1"落空——
`__folio_add_file_rmap` 的 stat 是**首映射进账/末摘除出账**（`atomic_inc_and_test` /
`atomic_add_negative`），第二映射者不动共享 folio 的节点账。修正后的断言组
（装页不动 → demote −1 属 legacy 末映射者 → mode_exit 摘除不动 → munmap 两腿 −2）
恰好逐腿排除"翻转偷记/漏记共享账"两类 R-W1-4 错位：任何一腿错账，后续绝对值断言即漂。

**"INV7 checker 加 file mapcount 对称锚"的交付口径**：file mapcount 对称以本新用例的
逐腿对账 + 既有 file 族用例骑翻转臂实现；kernel 侧 INV7/j2 走查器的管辖面（SHARED/
MAPPED/SWAPPED 槽）不含 FILE_MAPPED 槽（无 __resv payload 可比对），维持原设计不扩。

## 3. 红线核对

| 红线 | 实测 |
|---|---|
| INV6（rmap/回收对窗口 PTE 写必经事务） | diff 内 `set_pte*/ptep_*`/`mk_pte` 等写原语新增 **0**（机器扫描）；三处翻转全部落在既有事务临界区原位；spec 红线表的新写点清单（W1.b/W1.d/W1.e）不含 W1.c，实测一致 |
| =n 折叠 | n-objects 门 RC 0、零警告、`nm` 零 corten 符号；翻转调用点全在 mm/corten_arena.c（=n 不编译），声明面走 mm/corten_arena.h 的 =y 原型/=n 桩既定接缝。.config 已恢复 =y |
| 不动 W1.a wrapper 主体 / W1.b 注册表 | diff 仅 mm/corten_arena.c + mm/corten_arena_test.c 两文件；rmap.c/memory.c 零触碰 |
| 不 commit | 分支 mv-a0 HEAD 仍为 0c47a0515a8a，交付物 = 工作区 + r07-w1c.diff |

## 4. 验证结果（无盘 qemu，mva2-verify.sh；日志 `/home/ppw/cortenmm/results/r07/w1c/`）

| 项 | 结果 |
|---|---|
| `make -j8`（=y 全量，终态二进制） | 零新增警告（全量仅 objtool cpuidle + modpost memblock 两条既有基线噪声） |
| KUnit corten=on（on1） | corten 24/0/1 · **corten_arena 102/0/0**（101+本片新用例，`ok 22 corten_arena_test_file_rmap_shared`）· corten_fault 31/0/2 |
| KUnit corten=on 复跑（on2，flake 判定，**终态注释修正后的二进制再跑**） | **24/0/1 · 102/0/0 · 31/0/2 全绿**——两连跑（加修正前后各一）零本片 flake |
| KUnit corten=off（off1） | corten 25/0/0 · corten_arena 24/0/78 · corten_fault 7/0/26（既有 mode-dependent skip 形态；新用例 `ok 22 ... # SKIP`） |
| =n 十四对象门 | RC 0、零警告、零 corten 符号 |
| checkpatch --strict（r07-w1c.diff，351 行） | **0 errors / 0 warnings / 0 checks** |
| free 路径日志 | on1/on2 `Bad page state` 命中 0 |
| bzImage | 终态已出（=y 一致态重建）；guest 门留主会话 |

日志附注：`BUG: non-zero pgtables_bytes on freeing mm: 8192` 在 on1/on2 各出现——
**W1.a/W1.b 绿基线日志同在**（各 16-17 条，合成 mm harness 的既有形态），非本片引入；
verify 脚本的 lockdep/oops 精确签名匹配零命中。

## 5. 遗留与下游接口备注

- **guest 门留主会话**（规格 W1.c 行：JVM dlopen 工作负载 memory.stat file_mapped 与
  smaps Rss:File 交叉核对不回退；本片bzImage 终态已备）。
- spec §3.1 R7 注明的"分族依据从 folio_test_anon 改为 meta/region class"未做（其目的是
  匿名侧 W-2 的前置，本片保持 `folio_test_anon` + vma-ness 判别，file 臂已不依赖 vma）。
- W1.d 落地前过渡态不变式（W1.b 报告 §1.5 口径）经本片后依然成立：窗口 file PTE 的
  mapcount 继续钉住 pagecache folio，回收/迁移干净失败；且 zap file 臂现已 vma-free，
  未来任何"anchor 先亡、PTE 后清"的重排不再依赖 zap 拿到 carrier。
- 新用例依赖 KUnit 启动期无并发 file mapper（NR_FILE_MAPPED 精确读法前提），与 W1.a
  `novma_rmap` 锚同一前提；`corten_arena_test_node_stat()` 直接复用（W1.a 报告 §5 预告
  的复用兑现）。
