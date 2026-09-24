# M9.P3 决议草案：arm64 contpte / BBM 与 corten 单 PTE 页表页锁协议的冲突处置

    状态: **提案 —— 待主 agent 决策**。本文是决策输入，不是决议；
    采纳/修改/驳回由主 agent 裁定后回填 ARM64_PORTING.md（OQ1 终态）
    与 STATE.md。
    日期: 2026-09-22（调研班，严格只读，未改树、未跑构建）
    树: /home/ppw/linux-6.18 @ d40eae59ba76（M9.P1 `025756094542` +
        M9.P2 `93f834060cd1` 已合入；M-V A.1 之后）
    依据: specs/ARM64_PORTING.md r3（§3.3 / §10 OQ1 / §8 P3 / §9 R1-R2）
    页码引用均为 file:line，行号对 d40eae59ba76 核实。

---

## 0. 一句话结论（先行）

OQ1 在 r1 时代描述的"desc 写锁与 PTL 两把锁互斥写同一批 PTE"竞态，
在当前树（M3-M6 全量 arena 落地后）**已经不以其原始形态存在**：
arena 侧所有硬件 PTE 写都嵌套在 `pte_offset_map_lock()` 之下、且
PTL 位于 desc 写锁**内层**（§1.2）。真正剩下的 P3 工作不是"把
fold/unfold 路由进事务"，而是把两件已经成立的**隐式不变量**变成
**显式契约**：(a) desc→PTL 锁序的 lockdep/文档化验证；(b) "arena
窗口内永不 fold"的成因链（order-0 folio + VM_NOHUGEPAGE + 空窗声明
门）用一行 mm 粒度 fold 拒绝 + 检测器钉死。据此推荐 **方案③′
（限定豁免的显式化）叠加方案①的验证性收尾**，否决方案②（见 §3/§4）。

---

## 1. 冲突陈述（双方 file:line）

### 1.1 contpte 侧：整块重写 + 以 PTL 为唯一串行化假设

arm64 透明地把 `PTE_CONT`（bit 52，pgtable-hwdef.h:174）铺在
`CONT_PTES`（4K 页基 = 16）个对齐 PTE 上。对 6.18 实现
（arch/arm64/mm/contpte.c，经 arch/arm64/include/asm/pgtable.h:1672-1960
的公开 API 包装）：

- **F1 整块重写**：`contpte_convert()`（contpte.c:49-211）服务于
  fold 与 unfold 两个方向——先用 `__ptep_get_and_clear()` 清
  **全部 CONT_PTES 个** PTE 并聚合 AF/dirty（:61-69，期间整块瞬态
  为 0），再对整块做 `__flush_tlb_range()`（:207-208，除非
  BBML2-noabort），再 `__set_ptes(..., CONT_PTES)` 重绘（:210）。
  单 PTE 操作触发的邻接重写规模：4K=16 / 16K=128 / 64K=32。
- **F2 PTL 是它自己的锁假设**：contpte.c:296-299（"We are
  guaranteed to be holding the PTL…"）、pgtable.h:1752-1753（全部
  API 除 `ptep_get_lockless()` 外 "expected to be called with the
  PTL held"）。该层对 desc 锁一无所知。
- **F3 触发面在通用 API 里**：`set_ptes(nr==1)` = unfold-旧值 →
  存 → `contpte_try_fold`（pgtable.h:1799-1812）；fold 内联门条件
  = VA 对齐块尾 + pfn 对齐 + valid + !cont + !special
  （pgtable.h:1716-1723）；外体再查"整块被**单个 folio** 覆盖 +
  pfn 连续 + prot 一致"（`__contpte_try_fold`，contpte.c:213-274，
  folio 覆盖判 :248-256，一致性扫描 :264-270）。`clear_full_ptes`
  /`wrprotect_ptes`/`ptep_set_access_flags` 的部分块操作触发
  unfold（contpte.c:474-489, 537-553, 672-675）。

### 1.2 corten 侧：covering 写锁是 PTE 写的唯一红线；PTL 其实已在树中

- 协议核心（mm/corten.c）**不碰硬件 PTE**：`corten_map()` 明示
  "metadata-layer state transition only"（mm/corten.c:1194-1197）；
  事务 = `corten_txn_begin()` 对 covering 页 `write_lock_bh`
  （:873-879），`corten_txn_finish()` 释放（:917-935）；卸装互锁
  在 free 漏斗**不持 PTL**的上下文里拿同一把 desc 写锁
  （corten_arena 侧注释见 corten.c:393-401，实拿 :435-437）。
- **r1 之后的关键演化（本文最重要的事实核对）**：硬件 PTE 写全部
  住在 mm/corten_arena.c，且**每一处**都走 `pte_offset_map_lock()`。
  全树 12 个 PTE 触达点：zap 窗扫 :6039、untracked zap :6287、
  map_anon :4466/:4526、零页 :4584/:4597、restore :4632、COW
  :4737（注释即写明 "ptl nests below the desc write lock (R2)"，
  同款注释另见 :3373）、swapin :5091、mprotect :7825、ttu :8821、
  aging :9191、fork 拷贝 :3536/:3539、declare 空窗检查 :982。
  调用的原子原语亦全部 contpte 感知（`ptep_get_and_clear`
  :6106/:6322/:7839/:7869/:8847、`set_ptes(...,1)` :4526/:4597/:4847/:5210、
  `set_pte_at` :3561-3610/:7875/:8858-8905/:9214）——无 nr>1 批量
  set_ptes，无裸 `__set_pte`。
- 因此现存锁序全局为：**arena 事务 = desc 写锁 → PTL**（PTL 内层）；
  卸装 = 仅 desc 写锁（free 漏斗 PTL 已放）；alloc 钩子
  （asm-generic/pgalloc.h:94）不取 desc 锁；`corten_ptdesc_rearm`
  仅在 `fill_lock`（mutex）下调用（corten_arena.c:4318）。**不存在
  任何 PTL→desc 的取序** → 环不存在。这正是 ARM64_PORTING.md
  §3.3 Option A 设想的形态——但它不是"待做"，是"已在 x86 树中
  为与 core-MM 互操作而被迫做成，arm64 原样继承"。

### 1.3 剩余的真实冲突面（r1 未展开、本文核实）

1. **不变量未被钉死**：①的锁序只在两处内联注释里（:3373/:4728），
   无 lockdep 专项验证记录、无文档契约；将来任何人在 arena 事务内
   加一处"先 PTL 后 desc"的路径即静默反转。
2. **"arena 窗口永不 fold"是三层巧合的合力，而非显式契约**：
   (a) folio 覆盖前置条件（contpte.c:255-256）要求整块出自**单个
   folio**；(b) arena 的 folio 全部 order-0（corten_arena.c:4111、
   :4993、:4996；COW 预分配 order-0 见 :4843 注释；回收隔离判据
   明写 "order-0" :9230 附近）；(c) shadow-VMA 带
   `VM_NOHUGEPAGE`（:688）挡住 THP/mTHP 大 folio 缺页
   （huge_mm.h:333-337 `vma_thp_disabled`），且 declare 的空窗门
   （:955-1000 `corten_arena_check_empty_locked`，mmap_write_lock
   下逐窗 PTL 扫 present）保证进 arena 前无既有（可能已折叠的）
   映射。任何一层被未来的"大 folio 优化"拆掉，F1 的 16-PTE 重写
   就会在 arena 窗口内出现——届时仍**不是**数据竞态（PTL 在手），
   但 metadata/PTE 的 AF/dirty 聚合偏差与跨 [start,end) 的邻写
   会进入 Atomic-Tree 论证未覆盖的形态。
3. **BBM 细节**（§5 范畴，与 contpte 相邻）：COW/零页升级的
   clear→flush→make（:4490、:4831）已是 BBM 序；restore 路径的
   `ptep_set_access_flags` vma 臂用 core-MM 原语（安全），**!vma 臂
   的裸 `set_pte_at` + 全页 flush**（:651）与 restore 重建
   （:4678）需在 P3 复核"valid→valid 收紧是否可能出现"。

---

## 2. 方案对比表

| 维度 | ① PTL 嵌套（OQ1 原推荐方向） | ② 构建级禁用 contpte | ③ 限定豁免（fold 永不进 arena） | ③′ = ③ 显式化（本文补充的第四方案） |
|---|---|---|---|---|
| 语义 | arena 事务触 PTE 时同持 desc 写锁 + PTL，contpte 假设原样成立 | `CONFIG_ARM64_CONTPTE=n`，全内核用户内存不折叠 | 依赖 folio 覆盖前置条件天然排除 arena；只加文档 | 在 `__contpte_try_fold` 顶部加 mm 粒度拒绝（`mm->corten_mode`）+ arena 侧检测器，把 ③ 从巧合变成契约 |
| 现状工程量 | **≈0（已实现）**：12 个 PTE 写点全部 `pte_offset_map_lock` 且序正确；剩余 = lockdep 验证 + 文档化，~40-80 行（注释/断言/测试为主） | ~5-15 行 Kconfig（CORTEN_MM 与 CONTPTE 互斥或 defconfig fragment）+ 文档；**无反向依赖**（全树仅 Kconfig:2404 与 pgtable.h:1939-1960 引用该符号，`#else` 全量别名到 `__` 私有版，编译面干净） | ~0 行代码 + 文档（前置条件已在 contpte.c:255-256）；但要"便宜地插排除"其实无插点——排除已经发生，缺的是**护栏** | ~15-30 行 arm64（fold 顶部 4 行 + 大注释 + `#ifdef CONFIG_CORTEN_MM` 含 include）+ ~10-20 行 arena 检测器 + ~60-120 行 KUnit/验证 |
| 正确性风险 | 低；但**纯①不设防**：安全性悬在"order-0-only"这条未成文不变量上（§1.3.2） | 无（机制整体消失）；代价是防御深度为零——将来 16K 页基移植时还得重新面对 | 低-中：三层成因链（§1.3.2）任何一层被改即失效，且**失效无告警**——这是 r1 判 "fragile" 的真原因 | **最低**：fold 在 MODE mm 内被显式拒绝（与成因链解耦，任一层被拆仍有硬闸），检测器让任何漏网 fold 变响 |
| 性能代价 | 每 fault 一次 PTL 自旋（x86 现状已有，arm64 增量=0）；MODE mm 外无影响 | **系统级**：所有进程（含非 MODE）失去 contpte → TLB reach 退化（ARM 公称典型个位数百分比，TLB 受限负载更差）；且偏离 defconfig（THP=y ⇒ CONTPTE 默认 y） | 零（arena 本来就 fold 不成） | MODE mm 的**非 arena** VMA（legacy 堆/文件映射）失去折叠；非 MODE 进程零影响。注：`__contpte_try_fold` 无 vma 可查（contpte.c:230-231 明说），mm 粒度是可达的最细粒度 |
| 与 M9 DoD（arm64 KUnit 全绿 + Image 交叉编译） | 不改配置，DoD 面不变；需补 lockdep 跑法 | DoD 面扩大：需额外一个 CONTPTE=n 的 config 变体构建 + 解释为何 defconfig 不再是验证口径 | 不改配置，DoD 面不变 | 不改配置（fold 拒绝在 `#ifdef CONFIG_CORTEN_MM` 内，CORTEN_MM=n 时 contpte.o 字节不变）；KUnit 增 1-2 用例 |
| 上游可评审性 | 好（无共享代码改动，或仅注释） | 差（为研究原型改全局内存性能开关） | 好（零代码） | 中-好：动 arm64 共享文件 4 行，但方向与"研究树"定位一致，可被 `#ifdef` 完全隔离 |

注：任务书里①的另一种表述"把 fold/unfold 整块重写**路由进** corten
事务（unfold 前取 desc 写锁）"经查**结构性不可行**：contpte 全部
调用点都在 PTL 之内（F2），在那里再取 desc 锁 = 制造 PTL→desc 取序，
与 arena 现行的 desc→PTL（§1.2）正好互逆，是自造死锁。①唯一正确
形态就是 arena 侧 PTL 嵌套——已实现。文档据此把①重新表述为
"验证 + 钉死既有锁序"。

---

## 3. 推荐方案：③′ + ①验证收尾（组合），否决②

**推荐组合拳，总量 ~130-250 行（含测试与注释），零协议层改动：**

1. **③′ 硬闸**：`__contpte_try_fold()` 顶部加
   `if (IS_ENABLED(CONFIG_CORTEN_MM) && READ_ONCE(mm->corten_mode)) return;`
   （mm_types.h:983-993，字段已在；写者持 mmap_write_lock，读者
   READ_ONCE，与现网同款配对）。一行语义：MODE 进程永不产生新折叠。
   不动 `__contpte_try_unfold`（对未折叠块本就是 no-op；保持 unfold
   通达反而让"万一存在的历史折叠"仍能被安全拆开）。
2. **检测器**：arena PTE 读写的共享窄腰（zap 扫描 :6039 与
   map/restore 的 `ptep_get` 处）加
   `WARN_ON_ONCE(IS_ENABLED(CONFIG_ARM64_CONTPTE) && pte_valid_cont(cur))`
   ——只在校验成本可忽略的点放 2-3 处，配 debugfs 计数
   `corten_nr_cont_leak`。这是"不变量被未来改动破坏"的烟雾报警。
3. **①验证收尾**：arm64 PROVE_LOCKING 下跑 arena_stress + KUnit
   （M7 已有锁预热三关流程，STATE.md r06 条目），确认 desc→PTL
   序零 splat；把该序写进 include/linux/corten.h 的事务契约注释与
   ARM64_PORTING.md §3.3（OQ1 终态回填）。
4. **BBM 复核（§5 顺带项，边界内）**：审 :651 !vma 臂与 restore
   重建的 valid→valid 写是否可能收紧 prot；若可能，改走
   get_and_clear→flush→make（每处 ~5-10 行）。

**否决②的理由**：CONTPTE=n 的禁用面是"全内核所有用户内存"，而冲突
面只是"MODE 进程的 arena 窗口（且那里本来就 fold 不成）"——用系统
级 TLB reach 换一个已被③的结构性事实消掉的竞态，代价与收益完全
不成比例；还把 M9 DoD 的验证口径从 defconfig 摘出去。②仅当③′的
mm 粒度闸被证明有未知副作用时才作为 fallback 保留在案。

**否决纯①的理由**：工程上无事可做（已实现），但把安全性留给一条
无人看守的成因链（order-0-only + NOHUGEPAGE + 空窗门），第一处
"arena 支持 mTHP"的优化 PR 就会无声重开 F1。③′的硬闸成本四行，
把它变成显式契约。

---

## 4. 实施切片（每片 ≤300 行，推荐路径）

| 片 | 内容 | 文件 | 估行 | 验收 |
|---|---|---|---|---|
| S1 | ③′硬闸：fold 顶部 mm 粒度拒绝 + `#ifdef`/include + 大注释（说明 OQ1、锁序、为何不动 unfold） | arch/arm64/mm/contpte.c（+1 include 行） | 15-30 | `make mm/… contpte.o` 两配置（CORTEN_MM y/n）0E0W；n 时对象与基线 diff 仅符号表噪音 |
| S2 | 检测器：2-3 处 `WARN_ON_ONCE(pte_valid_cont)` + debugfs 计数 | mm/corten_arena.c, mm/corten.c | 25-40 | x86 回归零警告（x86 无 CONT 位，编译期退化）；arm64 KUnit 全绿 |
| S3 | KUnit：MODE mm 下 fold-never 断言（构造对齐 16 页 folio 覆盖场景太重——改用轻量法：在 corten_arena_test 现有 fault 用例后扫窗口断言无 pte_cont）+ 锁序断言（txn 持有期间 pte_offset_map_lock 成功、反序不可达由 lockdep 承担） | mm/corten_arena_test.c（arm64 sect，M9.P1 宏沿用） | 60-120 | arm64 25+新增全绿（interlock 时钟脆弱项按 §11.5 登记口径） |
| S4 | 文档/契约回填：include/linux/corten.h 事务契约加 desc→PTL 锁序段；ARM64_PORTING.md OQ1 → 终态（③′+①）、§9 R1 降级、§8 P3 行更新；本文件升级为决议 | include/linux/corten.h, specs/ARM64_PORTING.md | 文档 | 评审过 |
| S5 | BBM 复核（可选先行独立）：:651 与 restore 的收紧性审查 + 如需改 BBM 序 | mm/corten_arena.c | 0-40 | fault 路径 KUnit 复绿；qemu 冒烟 |

依赖序：S1→S2→S3→S4；S5 独立可先行。全部切片不动 mm/Kconfig 门控
（`ARM64_4K_PAGES` 限定维持，mm/Kconfig:1432）与 defconfig。

---

## 5. 风险登记

| # | 风险 | 证据/成因 | 等级 | 缓解 |
|---|---|---|---|---|
| P3-R1 | mm 粒度闸过宽：MODE 进程的非 arena VMA（glibc 堆、文件映射）失去折叠，TLB reach 退化 | `__contpte_try_fold` 无 vma（contpte.c:230-231）；mm_types.h:984-993 | 低-中（论文工作负载以 arena 匿名 order-0 为主，本就 fold 不成） | 基准测量留 P4（qemu 6.2 无 BBML2，fold 本来每次都 flush，数字本就悲观——OQ2 口径）；若实测不可接受，退化到"仅 corten_state 非 NULL 才拒"，粒度同为 mm 但覆盖面更窄（仍非 VMA 粒度） |
| P3-R2 | 检测器漏放/误放点：WARN 放太多热路径变噪音，太少护不住 | §4 S2 | 低 | 只放窄腰 3 处；debugfs 计数先行、WARN_ON_ONCE 封顶 |
| P3-R3 | 未来"arena 大 folio"优化无声重开整块重写（在③′落地前） | §1.3.2 三层成因链 | 中 | ③′即为此；S2 检测器是第二道 |
| P3-R4 | lockdep 未能覆盖真实反序路径（审计靠 grep + 抽读 9880 行中的 12 个触点，未逐行全读） | 本文方法学 | 中 | S3 的 PROVE_LOCKING 跑法是机械证明；审计范围声明写进 S4 文档 |
| P3-R5 | `mm->corten_mode` 读取与 ENTER/EXIT 的竞争：EXIT 后立即 fold 旧窗口 | 写者持 mmap_write_lock（corten_arena.c:2820-2846）；fold 调用点持 PTL 但**不持 mmap_lock**（fault 路径持 per-VMA 锁） | 低 | 竞态窗口内最坏结果 = 一次本可避免的 fold/unfold 对（PTL 串行，无撕裂）；检测器会捕获；注释明示该良性尾态 |
| P3-R6 | arm64 TCG 下 interlock 用例时钟脆弱（M7 登记项跨架构再现，3/7 flake） | ARM64_PORTING.md §11.5 | 中（只影响验证效率，不影响正确性） | 沿用登记口径：复跑 + KVM 复核；加固建议已在 §11.5 移交测试 owner |
| P3-R7 | 16K/64K 页基未来移植时 CONT_PTES=128/32，mm 粒度闸语义不变但检测器/测试常量需重参数化 | ARM64_PORTING.md §7.4 | 低（16K/64K 目前被 Kconfig 4K 门挡住） | S1 注释里写明粒度无关性；16K 矩阵排期不变 |
| P3-R8 | BBM 复核发现 !vma 臂确有收紧写：需改序并重验 COW/restore | mm/corten_arena.c:651, :4678 | 低-中 | S5 切片独立承载；不阻塞 S1-S4 |

---

## 6. 附：与 OQ1 原文的差异声明（供主 agent 裁决时对照）

- OQ1 原推荐（Option A "PTL nesting"）判定为**已完成态**而非待办；
  本文档将其改写为验证+钉死（§3.3 组合的第 3 项）。
- OQ1 原 Option B（禁 fold 于 arena，判 "fragile"）被拆成
  ③（成因链现状）与③′（显式硬闸）；r1 对 B 的 "fragile" 批评
  针对的无告警问题由 S2 检测器解决。
- 新增事实：①的"路由进事务"变体因锁序互逆不可行（§2 注）；
  `mm->corten_mode` 字段（mm_types.h:983-993）为③′提供了零成本
  落点——r1 时代该字段尚不存在。
