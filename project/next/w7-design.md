# W-7 设计稿 · multi-record registry（frame-sharing 收编完备）

2026-10-04。agent: w7-dev（kernel-MM architect+dev, ponytail full）。基座
/home/ppw/linux-6.18-mva @ 856416f7ceec（干净）。授权: D34 + D28 字面树归零；
任务书 next/w7-dev-brief.md。本稿为设计先行切片的交付物, **未动内核代码**。

---

## 0. 问题定界与现场证据（消费面走查结论先行）

registry = `state->arenas`（per-mm xarray, 键=2M 帧号）, 槽值三态:
NULL / `&corten_va_reserve_sentinel` / `struct corten_arena *`。记录域
[start,end) 页粒度（start PMD 对齐或页对齐——sweep 收编件继承 VMA 页对齐
起点; end 页粒度, brk 区即其例）。一帧一槽一指针 = D34 定界: 同帧多 VMA
（ELF 五段常态, r--/r-x/rw- 尾首同帧）只有首段可 declare, 邻段
`corten_arena_overlaps()` 帧级占位拒 → skip_declare=288/电池（88%）,
J4 字面树归零不可达。

**全走查后的关键事实**（决定设计空间的边界）:

1. **事务层与帧独占无关**: `corten_ptdesc_xa`（PFN 键, 每 2M 窗一
   descriptor: rwlock + mm 背链 + 4K meta 数组）、`corten_lock_range`
   事务、meta 数组全部**页粒度寻址, 无 arena 归属字段**。两个页不相交
   记录共享一帧 = 共享一个 descriptor/meta 数组, 槽位不相交, covering
   写锁本来就按窗口串行化。**1:1 假设全部住在 arena 层**。
2. 槽值三态的直接消费者集中在 11 个 helper/route（§2）; 外部文件
   （arch/x86、mm/*、fs/proc、kernel/*）**无一处直接触 state->arenas**,
   全部经 route/row-token/VM_CORTEN 旗标间接消费——外部消费面里只有
   task_mmu 的 row token 面真正逐记录渲染。
3. 窗口域记录（magazine/pool/auto）**结构性不可能帧共享**: 哨兵整帧
   占位; parked 记录页域=整帧, 邻 declare 撞其页域被 C1 拒; P4 eject
   兜底。帧共享只发生在 legacy 域（ELF file/anon 段 + brk 区相邻处,
   bss 区与 brk 区同帧是活体形状）。
4. 对照消费者多记录安全性现况: `corten_region_row` token、row 流、
   事务层**已天然多记录安全**; `corten_region_next` 指针去重、四个
   `walked_until/end_frame` 游标（mm_exit drain / exit_walk 相位A/
   fork_commit / fork_begin freeze）、`corten_mm_state_pages` 的帧遍历
   计数、25+ 处 KUnit 直读 `xa_load(->arenas)` 是全部缺口。

---

## 1. 数据形状提案（brief 要求 1）

### 案 A: 帧内记录桶（intra-frame record chain）——**择案**

共享帧的槽值从裸指针升级为**带标记的桶指针**; 非共享帧保持裸指针,
零新增开销:

```c
/* 槽值: xa_tag_pointer(bucket, 1) | 裸 ar 指针 | 哨兵 | NULL。
 * kmalloc 8 字节对齐保证 tag 位空闲; 哨兵恒不进桶（§4 不变量 I3）。*/
struct corten_frame_bucket {
	unsigned int		nr;		/* 成员数 */
	struct rcu_head		rcu;		/* kfree_rcu 释放 */
	struct corten_arena	*rec[] __counted_by(nr); /* 按 start 升序 */
};
```

- **语义**: 同帧多记录合法 ⇔ 页域两两不相交（帧重叠合法）。记录仍
  跨帧整体登记（start/end 不变）——"跨帧 span" 语义保留在记录上,
  变的只是槽的解析粒度: 槽 → 覆盖该帧的记录集合; 点查询在集合内按
  [start,end) 页级判定。桶内线性扫描, 成员数 = 同帧段数（ELF 常态
  ≤4, 理论上限 512=帧内页数; ponytail 天花板: 不设容量上限, 依赖
  C1' 页互斥天然封顶, 若未来出现病态多小映射再议 cap）。
- **写侧**: declare/punch/release/fork-child 逐帧发布走统一 helper
  `corten_frame_slot_remove(ar, f)` / 桶插入: 桶更新一律 **copy-update**
  （新桶分配→xa_store→kfree_rcu 旧桶）, RCU 读侧免锁。无需"枚举覆盖
  f 的全部记录"的辅助索引——**桶本身就是覆盖集**: C1' 检查逐帧访问
  恰好路过全部共帧成员, 就地收集; 移除时桶内删一项。写者锁形不变
  （全部持有 mmap_write; declare/release/fork 另持 ctl_lock）。
- **读侧 helper**（全部消费面只改这一层 + 遍历游标, 见 §2）:
  `corten_frame_slot_ar(slot)`（解 tag）; 点查询 `lookup`: 解槽→桶则
  扫描 rec[] 取 start<=addr<end（跳过 idle——防御, 不变量 I3 下不
  发生）; 成员判定 `corten_frame_slot_has(slot, ar)`。

### 案 B: 跨帧 span 记录 + 独立区间索引（maple/augmented tree）

另建 per-mm [start,end)→record 的 maple 树, 点查询走树, 帧表降级为
粗索引或废弃。**否决**, 三条:

1. **INV5 直接违反**: 热路径从一次 xa_load 变为 maple 走树（树高 +
   节点内二分）。M3 DoD"零 maple walk"是本项目对拍过 perf 的招牌
   （`[FAIL-2]` 注释明确把 vma_lookup 出热路径立为教训）, 案 B 等于
   回退标志性成果。
2. **双结构记账**: 帧表不可能废弃——哨兵/punch 擦除/occupy 判定/
   shrink aging marks（shrink_aged/shrink_cursor 帧键空间）都按帧
   寻址。案 B = 每个写点双写 + 两结构一致性论证, 爆炸半径翻倍,
   消费面却无一简化。
3. 哨兵是**槽键**语义（"帧被申领"）, 区间树须为其再造表达; 行流/
   占位判定的"空帧跳过"语义全部重推。

### 案 C: 主帧表 + 页粒度 override 副表

主帧表不动, 共享帧的页级例外进第二 xarray（页号键）。**否决**:
点查询两面化（主表命中 ≠ 答案, 还得查副表——热路径 +1 load 或
per-mm 分支）; 更致命的是**遍历流分裂**: 完全落在一帧内的记录
（小段 ELF 段常态）在主表**无任何槽位**, region_next/fork/exit/unuse
每个遍历者都必须合并两路流——15 个消费面的遍历复杂度翻倍, 换来的
只是免一个桶分配。违背 ponytail 第一级: 桶只在共享帧付费, C 无此性质。

### 择案论证（A）

- 热路径不变式保住: O(1) xa_load + tag 测试 + 有界页域比较;
  非共享帧（窗口域全体、legacy 单段帧）与今日**逐指令同价**。
- 遍历面保形: 全部 `xa_for_each/xa_find` 遍历只多一步"解槽→产出
  0..N 记录", 去重规则见 §3（无状态, 结构性杜绝 r07 mm_exit 双
  drain 类回归的复发面）。
- 写面收敛: 桶操作收敛在 3 个 helper, 每个写点改动 ≤5 行。
- 内存: 桶仅共享帧存在（每进程 ELF 边界帧数个, ~40B/个）, RCU 释放。
- 风险面: 新类型进槽值 → 全部槽值消费者必须过 helper（漏改 = 桶被
  当记录解引用）——以编译期 helper 收口 + KUnit 直读面全改走访问器
  兜住（§2.12）。

---

## 2. 消费面改动点清单（brief 要求 2）

### 2.1 fault 快路径（arch/x86/mm/fault.c:1375 → corten_arena_user_fault）

- `corten_arena_lookup()`: 解槽 + 桶扫描（§1 案 A 读 helper）。签名
  与返回契约不变 → **arch 侧零改动**。
- `corten_arena_lookup_get()`: 不变（frozen/percpu_ref 逐记录, 与桶
  形正交）。
- `corten_arena_fault_owned()`: tier1（记录页域判定）不变——多记录
  下它**从近似变精确**（今日整帧单记录时帧内他人页误归）; tier2
  `xa_load(frame)==ar` → `corten_frame_slot_has()`; tier3 不变。
- `corten_arena_txn_owned()`: 同 tier2 改成员判定（[F-B seal] 语义
  不动: 桶成员变更同样经 mmap_write 串行 + covering 锁粘滞）。

### 2.2 fault 慢路径（mm/memory.c:6609 → corten_arena_handle_mm_fault）

- vma 键控（VM_CORTEN 分流）, 内部 lookup_get 继承 2.1。零独立改动。
- `corten_fault_window_maperr()`（mmap_lock.c:451 消费）: lookup 语义
  升级自动继承——窗口域无桶, 行为不变。

### 2.3 GUP 探针（mm/gup.c:1450 → corten_gup_window）

- 窗口域门（4496）不动——窗口域无帧共享, 桶不可达。`corten_region_
  lookup` 继承 2.1; rclass/prot 逐记录读不变。**零独立改动**。
- `fixup_user_fault` / `gup_fast_fallback` 的模式窗门: 域级, 不变。

### 2.4 fork 镜像 + commit（mmap.c dup_mmap: 2059/2181/2196）

- `corten_arena_fork_begin()` freeze 扫描: `xa_for_each` → 记录一次
  迭代器（§3 规则 R1）。
- `corten_arena_fork_commit()` 镜像循环（8202, `drained_until` 游标）
  → R1。
- `corten_arena_fork_register_child()`（7523 xa_store 循环）→ declare
  同款桶插入 helper（child 与父记录在共享帧成为页不相交共租）。
- `corten_arena_fork_mirror()` / `fork_copy_window` / `fork_copy_ptes`:
  窗口循环本就按记录 [start,end) 页粒度裁剪, 共帧邻记录页在界外
  不触; SHARED 置位/meta 回放页键控 → **零改动**。
- `corten_arena_test_fork_fail_arm` 注入点不动。

### 2.5 exit walk 两相位（corten_arena_mm_exit: 4146 drain + exit_walk: 3817/3954/3998/4043）

- **drain 循环**: `drained_until` 游标 → R1（游标法在桶形下双重失效:
  同帧双记录共享 walked_until 会吞掉第二条; 桶属同一记录跨非连续桶
  出现使 last 指针去重也失效——R1 无状态, 两类皆免）。
- **相位 A 重构为帧键**（本设计的唯一结构性改动）: 现形"逐记录
  [start,end) 窗口游程 + 整帧 PT 退役"在共享帧下有 **folio 泄漏
  风险**: 记录 R1（起始较早）的游程在帧 F 收尾时按"帧独占"论证整帧
  退役（3926-3931）, 而共租 R2 的 PTE 尚未被自己的相位 zap → PT 页
  带活 PTE 被释放（恰是 3851-3854 注释警告的泄漏形状, 现由树共租
  幸免）。新形: `xa_for_each` 帧序遍历, 每帧**先访问桶内全部记录**
  各自 zap 其帧内裁剪段（现混合帧臂 3858-3876 的
  `unmap_chunk_flags(mm, ar, clip)` 泛化为每记录必经）, 后判
  `exit_span_clear && frame_ptes_empty` 决定整帧退役; 游程合并
  （TLB 批量）按帧连续性保留。done_* 游标不动。
- **相位 B1-B3**: 段键 + span-clear 守卫, 与记录形状无关, 零改动。
- **顺带清理**: 3895-3899 死代码（`continue;` 后不可达的
  `free_ptes_span` 重复臂——W-4b 合并残留, 本片路径上必改此函数,
  一并拔除并在报告披露）。

### 2.6 sweep adopt/pick（corten_arena_mode_sweep 族）

- `corten_arena_declare_locked()`: `corten_arena_overlaps()` → C1'
  （§4）逐帧逐记录页域相交判定; 帧发布循环（2061）→ 桶插入 helper;
  unwind 桶对称。
- `corten_sweep_adopt_anon/file` 的 `xa_load(start>>SHIFT)` 回读
  （6616/6701）: 桶形下歧义（两记录可同帧起始）→ declare_locked 改为
  **返出 arena 指针**（顺带删歧义, 小重构）。
- pick/surgery/finish 窗口族: 字节域裁剪本就页粒度, 共帧邻记录页在
  界外 → 零改动。meta 回填按页 → 不变量 I4 下互不干扰。
- sweep 驱动升序注释（6909-6913）与 skip_declare 桶保留: 语义收窄为
  真·页域冲突（目标读数 288→~0, 桶留作诚实计数器）。

### 2.7 j2 走查（corten_audit_j2_scan/walk, 13441）

- 树侧 + implants 数组, 与帧表无关 → **零改动**。窗口域 INV-MV2 语义
  不变（窗口域无桶）。

### 2.8 wl 走查（corten_whitelist_scan, 13668）

- 树侧分类器 → 零改动。桶外预期: 多记录收编到位后 MODE 进程
  wl_file/wl_anon → 0（skip_declare 残余消失）, 六桶读数换成
  shadow/implant/stack/special 四桶 + tree_entries==白名单内。
- `tree_entries` debugfs 载体（13737）不动——它就是终判据的读数口。

### 2.9 magazine + recycle（corten_va_seg_claim/mag_alloc_cpu/release_frame, 5301-5660）

- 哨兵槽值恒不进桶（I3, 插入 helper WARN 兜底）→ `seg_claim` 障碍
  扫描（5372）、recycle 校验（5470 `!= sentinel`）: 经解槽 helper——
  桶按 I2 不出现于窗口域, 解槽后语义不变（桶 ≠ 哨兵 → 障碍, 正确）。
- `corten_va_release_frame()`（5613 哨兵覆写/5639 裸擦除）: 加"槽非
  桶"WARN（release 的帧按 I2/I3 恒为裸记录/哨兵/NULL）。
- `window_place_global` 障碍跳（5673 lookup 读 ar->end）: 继承 2.1。

### 2.10 pool park/reactivate（11913-12400）

- `parkable`（12016 `xa_load != ar`）: 保持**严格裸指针等值**——
  parked 候选帧出现桶 = 违反 I2/I3, 保守拒 park（真 RELEASE）,
  不引入成员判定语义（park 是整帧产权主张, 桶即异常）。
- `prepare_locked`（11926-11981 `xa_load` 判 idle/extent）、`eject_
  locked`（11760 `stale != ar` WARN）、`reactivate`（全帧记录）: 解槽
  helper 过一道 + 严格语义保持。零行为变化（窗口域无桶可达）。

### 2.11 punch 路由（corten_arena_mmap_punch/punch_split, 13058-13188）

- 帧擦除循环（13153 `xa_erase` + `stale != ar` WARN）→ 桶移除 helper
  （共租存活, 桶缩员/降级裸指针/空桶擦除, copy-update）。这是**行为
  变化点**: 今日 punch 擦帧即清整帧登记, 多记录下只清被 punch 记录
  自身（页域裁剪语义, 与 unmap_chunk 的页域 zap 对齐）。
- `punch_split`（影子 VMA 手术, 仅 targeted-DECLARE 有 vma）: 不变。
- punch 后 implant_mark（D33 形）不变——implant 数组与帧表正交,
  窗口域 punch 不产生桶（I2）。

### 2.12 debugfs + KUnit 夹具（125 锚）

- `corten_arena_arenas_report`/`stats_report`: 全局 obs 链表, 记录键,
  **零改动**（桶形下自动多记录可见）。
- KUnit 25+ 处直读 `xa_load(&state->arenas, ...)`/`xa_for_each`
  （2163/2196/3935/4904/5116/5162/5319/5375/9523-9551/10213/10295/
  11253 …）: 全改走测试可见访问器（`corten_arena_test_region_of`
  扩展 + 新 `..._frame_slot()` 原语）——既是夹具迁移也是"槽值类型
  封闭"的编译期兜底。
- **新锚**（红绿, 每消费面至少一）: ①同帧双记录 declare 成功 + 双侧
  fault 解析各归其记录; ②C1' 页域相交拒（-EEXIST）而帧重叠页不相交
  收; ③sweep 同帧双段双收编（D34 电池形, 树读数下降直证）; ④fork
  共享帧桶镜像 + 子侧双侧解析; ⑤exit 共享帧 PT 页退役恰一次 + 无
  双 drain; ⑥row 流双记录渲染（maps 行数/边界与 A.1 树形逐字节对齐
  形）; ⑦punch 共租存活; ⑧哨兵/禁桶不变量（I3 WARN 路径）; ⑨
  invariants_ok 扩展检查; ⑩unuse/shrink 计数无重复计。既有 125 锚
  除直读面迁移外判定期望**全部保持**（-EEXIST 族锚在页域相交下原判
  不变——549-565/7593 等子域/跨界形即页域相交）。

---

## 3. 遍历去重规则（本设计的核心新不变量）

**R1（start-frame 规则）**: 访问帧 f 的槽时, 桶成员（或裸记录）r 仅当
`(r->start >> PMD_SHIFT) == f` 时产出。无状态、桶形无关、与对齐无关
（页对齐 start 可与前任记录共起始帧, 同帧多记录各带各的 start 全数
产出）。取代四处 `walked_until/end_frame` 游标与 region_next 的
`it->last` 指针去重; `corten_region_iter` 布局不变（`last` 字段废弃
保留或删除, 收口时定）。r07 的 end_frame 双 drain 教训由 R1 结构性
封死（不再有可算错的游标）。

## 4. 不变量逐条复核（brief 要求 3）

| 不变量 | 复核 |
|---|---|
| INV1 事务原子 | 桶发布=xa_store 原子单字, 读侧 RCU; declare 失败 unwind 桶对称恢复。保持 |
| INV2 锁序 | 零新锁; 桶操作在既有 mmap_write(+ctl_lock) 位点内; kfree_rcu 兜 RCU 读。保持 |
| INV3 desc 锁 BH 对称 | 事务层未触。保持 |
| INV4 meta/PT 同生灭 | 未触。保持 |
| INV5 热路径零 maple 零 mmap_lock | lookup 仍一次 xa_load（+tag 测试+有界页比较）; 案 B 否决理由即此。保持 |
| INV6 真源纪律 | 桶为纯元数据, 不触 PTE; 全部 PTE 写仍在事务/白名单胶水。保持 |
| INV7 metadata≡PTE | meta 页粒度, 页域不相交 → 共帧记录槽位不相交, checker 域不变。保持 |
| INV8 引脚页存活 | 未触。保持 |
| INV9 =n=原内核 | 桶类型/helper 全部 CONFIG_CORTEN_MM_ARENA 内; =n stub 已返 NULL/0, 桶不可达。保持 |
| INV-MV2 窗域树净 | 窗域无桶（I2）, j2 判定面不变。保持 |
| INV-MV3 记录配对 | may_prot>=prot、idle<=>RESERVED、FILE 四件: 逐记录性质, 桶形无涉; `corten_region_invariants_ok` 遍历改 R1 + 桶内两两页域不相交断言（新锚⑨）。保持+强化 |
| **新 INV2'（帧→页互斥）** | 同 mm 任意两活记录页域不相交; 帧重叠仅当页不相交。由 C1' 建立桩（declare/sweep adopt/fork child 三写口同过）。哨兵整帧（I3: 恒不成桶员）; parked 记录页域=整帧故天然不桶（I2: 桶仅存于 legacy 域页粒度记录间） |
| **C1 等价物** | `corten_arena_overlaps` → C1': 逐帧解槽, 对每成员做 `start < ar->end && addr+len > ar->start` 页域相交判定, 相交→-EEXIST。与 `corten_arena_range_overlaps` 现行逐记录判定同形——该 helper 反而零改动（xa_for_each_range 解槽后逐成员原判）。[C1/R1 closure] 的内容安全网（check_empty / adopt 双相位回填）逐记录页域独立, 不受共帧影响 |

## 5. 记账与统计兼容（brief 要求 4）

- **ptdescs/meta_arrays**: 帧粒度 PT 页 + 页粒度 meta——共帧记录共享
  descriptor（锁竞争微增, 正确性无涉）。语义不变。
- **sweep 计数族**: 六桶+adopts 语义全保留; skip_declare 从"帧占位"
  收窄为"页域冲突"（预期 288→~0, 非删桶——诚实计数器）。
- **j1/j2 族**: 树/registry 走查计数, 语义不变; j2_stale 预期维持 0。
- **迁移表**: 无改义计数器; 新增面仅桶（无独立计数器, 占有率可由
  obs 链表与帧槽推导, ponytail: 不加计数器, 报告以 tree_entries/
  skip_declare 收敛读数为证）。
- `corten_mm_state_pages`（shrink 计数）: 帧遍历下桶帧解槽后按帧
  一次读 desc——改写为帧键（消 `arena->start` 歧义）, 读数语义不变
  （共享帧恰计一次）。shrink walk/aging: `shrink_aged`/`shrink_cursor`
  帧键保留（粒度粗于记录但健全）; victim 选择页级（MAPPED+PTE）,
  记录窗口遍历改 R1。evict/unuse 遍历同 R1。

## 6. 迁移路径（brief 要求 5）

**一次性切换, 不设双读期。** 论证: registry 为 per-mm 内核私有结构,
无持久态（fork/exit 全量重建）, 无 ABI 面, 全部消费者在本树内可枚举
（§0.2 已穷尽）; 双读期只会让每个点查询付两倍、每个遍历面背合并逻辑,
换不来任何回滚能力（回滚=revert 整片）。顺序（每步红绿锚, 全链同
W-6b）:

1. 桶类型 + 解槽/成员/插入/移除 helper + KUnit 访问器（纯新增, =n 折叠）;
2. 读面: lookup 族 + 槽值直读点（KUnit 夹具迁移）——此步后桶仍不可达,
   零行为变化, 全锚绿;
3. 写面: declare/punch/release/eject/fork-child 桶化 + C1';
4. 遍历面: R1 替换四处游标 + region_next/row 流 + unuse/shrink/exit
   相位 A 帧键重构（含 3895-3899 死代码清理）;
5. 收编兑现: sweep 全量收编验证（tree_entries 白名单外==0 终判据,
   动态 ELF 负载 smoke 进程族 + metis 多段二进制形）。

## 7. 明示不做（ponytail 边界）

- 跨记录空间操作（munmap/mprotect/mremap 跨两记录）维持现行
  -EOPNOTSUPP fail-open 姿态: 判定表逐形对拍后结论——同 shape 今日
  亦拒（单记录臂 classify PARTIAL 同 errno）, 电池零变化; 收窄该
  姿态是独立能力面, 不入本片。
- 桶成员数上限、桶占有率计数器: 依赖 C1' 天然封顶; 观察面走既有
  debugfs。
- GUP/ptrace 对 legacy 域已收编域的姿态: 本片不触（与今日 adopted
  区域同姿势, 非本片 delta）。
