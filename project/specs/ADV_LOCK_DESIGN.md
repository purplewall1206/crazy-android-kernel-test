# ADV_LOCK_DESIGN · CortenMM_adv 锁协议在本移植中的实现评估与设计

创建: 2026-09-22(纯设计班, 零内核树改动/零实验)。
依据: paper.txt(pdn 分栏文本, 引用按行号)/ STATE.md(r07 全量, HEAD=34f1ae661be3 含
M-V A.0)/ mm/corten.c + mm/corten_arena.c 实码 / specs/MV_VMA_FREE_SPEC.md /
REPORT.md §4.6(perf1 + G2 trace + perf2a 口径)。
任务书出处: STATE D3("先 rw 打通全链路, 再评估 _adv 作为 M4+ 可选升级")。

---

## 0. 结论速览(裁决见 §6)

1. **本移植的 fault 通道在生产视图里已经不是论文意义的 _rw, 而是一个"单层 _adv"**:
   快钩在 `do_user_addr_fault()` 任何 VMA/mmap_lock/per-VMA-lock 动作**之前**、
   **零锁**状态下完成遍历(arch/x86/mm/fault.c:1362-1382 → mm/corten_arena.c:4910,
   函数注释明言 "before any VMA/mmap_lock/per-VMA-lock action");事务体在 covering
   独占锁下执行——这正是论文 _adv 的两阶段形态(遍历无锁 + 锁相含事务体, paper.txt:570-583)。
   论文 _rw 的"路径读锁"(paper.txt:516-532, Fig 5)在本移植的生产视图里**从未存在过**:
   只有 PTE 级页带描述符(mm/corten.c:939-947, 996-1008), 走查路径长度恒为 1。
2. **任务书前提"地板部分源自写锁串行化"与实测证据冲突**: G2 trace 判定
   "fault 送达通道等价、**无竞争劣化**"(REPORT.md:780), 终态 profile 两臂同形、
   corten 侧 self 仅 3.5pp(REPORT.md:745-746, :778-779)。-14~-22%(上沿 -31)家族的
   构成 = 每-fault 事务簿记(-7.1~-11%) + take/park 簿记(3-5%) + IPI/调度地板 +
   PMD 窗 512-PTE 扫描 ~13µs/op(REPORT.md:739-744, :767-772)——**不在 adv 的可消除面内**。
3. 论文 _adv 对本移植的**真实剩余增量只有一件小事**: 删掉 `corten_txn_begin()` 里
   生产视图的 vestigial 读锁舞蹈(mm/corten.c:823-834 + 870-873)——一次同 cacheline 上的
   `read_lock_bh`→`read_unlock_bh`→`write_lock_bh` 折返, 正确性论证(corten.c:737-756
   已写明真实视图的 descent 判定不依赖路径锁)从未需要它。预期收益: 每 op 省 2 次原子
   RMW + 1 对 BH 开关, **≪ 地板的 1%**;价值是热路径微优化, 不是协议升级。
4. **裁决: 完整 _adv 不做**(结构空转 + 收益对象不存在 + 风险/收益倒挂, 论证见 §6);
   ADV-1(读锁舞蹈删除)作为独立微片**可做、低优先**;两个"更像 adv"的候选
   (DFS 阶段/seqcount 无锁 query)被证据**否决**。

---

## 1. 协议映射: 论文 _adv 每要素 → 本代码库对应物

先纠正一个任务书内的预设: **论文 _adv 不是 seqcount/版本号协议**。paper.txt §4.1 的
_adv 要素只有四个(伪码 Fig 6, paper.txt:461-499;文字 :559-625): 互斥自旋锁、无锁遍历、
stale 位重试、RCU monitor。全文无版本化读侧验证(§5.1 对 _adv 的验证方式也只是
"建模无锁遍历为可从任意 PT 页起锁" :873-881)。下面按论文实际内容逐要素映射。

| # | 论文 _adv 要素 | 论文出处 | 本移植对应物 | 状态 |
|---|---|---|---|---|
| A1 | **covering 独占锁**(事务体两阶段锁相, "acquire all necessary locks before performing any memory operation", :602-606) | Fig 6 L9;:592-599 | `write_lock_bh(&desc->lock)` 于 `corten_txn_begin()` L7/L8(mm/corten.c:870-879), 事务 API 全族(query/map/mark/unmap/swap_out/swap_replay/txn_slot)都在其下执行(mm/corten.c:1077-1465) | **已实现**(且是唯一形态) |
| A2 | **无锁遍历**(RCU 读侧临界区 + `atomic_read_child_page_of`, Fig 6 L3-L8) | :570-582 | 快钩零锁遍历: RCU 帧表 `corten_arena_lookup_get` + percpu_ref pin(arena.c:4929-4933, 头注 :29-32);真实视图硬件走查全 `READ_ONCE`(mm/corten.c:961-981);**生产视图无任何路径读锁**(上层页无描述符, corten_real_child 不可达 :1002-1008) | **已实现**(以帧表+pin 替代论文的 PT 子指针原子读;上层结构稳定性由帧表所有权+fill_upper 的 fill_lock 承载, M6_RMAP_SPEC D4) |
| A3 | **stale 位 + 锁下重查重试**(Fig 6 L10-13;Fig 7 :600-625) | :623-625 | `desc->stale` 在 desc 写锁下发布(`corten_ptdesc_uninstall`, mm/corten.c:435-437);begin 在锁下重查(:874-879), -EAGAIN 上抛;`corten_arena_fault_once` 有界重试(arena.c:4988, 注释直引 "Fig.7 retry cap";swap-in 臂 4 次, arena.c:4463 重锁后重验) | **已实现**(M2b/M3a interlock) |
| A4 | **RCU monitor**(先原子清父 PTE → 子树标 stale → `rcu_delay_free`;释放等读侧退出, Fig 7/6 L30/L35) | :607-619 | 同构物:**xarray 先擦除后回收** + 基准引用 + `kfree_rcu`(`corten_ptdesc_uninstall` xa_erase→写锁标 stale→put→release{kfree_rcu}, mm/corten.c:419-441, :188-197 注释;pin 协议 `corten_ptdesc_get` RCU+refcount :206-233)。论文"原子清父 PTE"对应 xa_erase 的原子性;论文"PT 页本体延迟释放"对应 desc 的 kfree_rcu(PT 页本体归 Linux tlb 漏斗管, 协议只承诺 desc 元数据不 UAF) | **已实现**(等价物, 粒度不同: 论文按 PT 页, 本移植按 desc) |
| A5 | **后代 DFS 锁相**(covering 可能是高层页, 无锁遍历可绕过被锁祖先 → 必须锁全部后代, Fig 6 L17/L22;必要性论证 :592-599) | :583-599 | **结构不可达**: ①只追踪 PTE 级页, covering 页恒为叶层, 无追踪后代;②范围被钉在单 PMD 窗(`corten_lock_range` 拒绝更宽, mm/corten.c:1057-1064);③子树退役次序由 corten.h 不变量承载("M4 retires subtrees descendant-first ... covering page included -- stale before it", include/linux/corten.h:607-611) | **不适用**(若 M4+ 真落地上层描述符再复活, 见 §6.4 翻转条件) |
| A6 | **重试环**(Fig 6 L2 `while True` + L13 `continue`) | :462-479 | 调用方重试: fault 臂 2/4 次上限(arena.c:4988)+ 空间操作路由的一次重试惯例(mprotect C1, arena.c:7400-7405;-EAGAIN 重试计数 `corten_nr_eagain_retries`) | **已实现**(策略上移到调用方, 语义同) |
| A7 | (任务书预设的)版本化 seqcount 读侧验证 | **论文无此要素** | — | **不映射**(若引入属超论文扩展, §3.3/§5 切片 ADV-4 论证否决) |

**推论**: 任务书的"_rw→_adv"叙事在论文语境里是"多级描述符树上, 路径读锁 → 无锁遍历+DFS"
的替换;本移植的生产视图只有一层描述符, **该替换的对象(路径读锁)不存在**, 论文 _adv 的
可迁移部分(A1/A3/A4/A6)在 M2b/M3a 已全部落地。HEAD 上的协议名"rw"是历史命名
(mm/corten.c:15 "the CortenMMrw locking protocol (paper Figure 5)"), 与真实视图的
实际形态不符——**命名误导, 形态已是单层 adv**。

---

## 2. 现状差距分析

### 2.1 生产视图一次事务的真实锁形状

`corten_lock_range()`(mm/corten.c:1042-1067)→ `corten_txn_begin()`(:777-909)在真实视图
(`corten_real_ops`, :1033-1037)的逐步展开:

```
corten_real_root:  pgd→p4d→pud→pmd 全 READ_ONCE(无锁) → pte 页 desc pin(RCU+refcount)
                   → stale/mm 快检(:988-991, 未持锁的预检)
循环(恰一轮, PTE 级 child_covers 恒 false, corten.c:538-541):
  read_lock_bh(&cur->lock)          :823   ← vestigial
  stale 重查(未持写锁)→ -EAGAIN      :824-829
  path 压栈                          :830
  child_covers == false → break      :833-834
L7/L8 升级:
  pop → read_unlock_bh               :871-872
  write_lock_bh                      :873   ← 真正的互斥点
  stale 再查 → -EAGAIN               :874-879
  va_base 首访记录                    :887-889
  path 释放(空)                      :899
→ 事务体(query/map/mark/... 在写锁下) → corten_txn_finish write_unlock_bh(:917-935)
```

每事务锁成本 = **1 对多余 read_lock_bh/read_unlock_bh + 1 对 write_lock_bh/write_unlock_bh**。
读阶段买到的只有: ①一次锁下 stale 预检(写锁下会重做, :874 冗余);②path 槽位(真实视图
立即弹空)。**corten.c:737-756 的注释已自我证明读阶段在真实视图无用**: "the real x86
view takes at most one read lock ... so its descent decision cannot be invalidated
regardless of path locks -- what keeps the walked hierarchy stable there is the
caller-level contract"。即: 差距不是"实现落后于 adv", 而是**通用多层协议核
(KUnit 合成视图仍需路径锁, :108-113 锁类注释)给单层真实视图留了黎明的垫肩**。

### 2.2 读路径成本形状(mmbench 口径)

- **纯 fault 通道**(pf 形状, 预映射后逐页写): 五轮网格 -7.1~-11%(REPORT.md:739-742),
  构成 = fill_upper + lock_range(desc rwlock write, BH) + txn_owned xa_load + query + mark
  ——每-fault 一事务的**语义价**, 报告明言"消此成本 = 放弃 metadata 唯一真源, 不在选项内"。
  perf2a 终态 pf low t8 **净胜 +7~+22%**(REPORT.md:771)。
- **mmap-pf/mmap churn 通道**(T0 形状): G2 trace 加性度量 T0−BASE = **+206~208µs/op
  恒定**(REPORT.md:774-777);构成(每 op) = 4.7 事务 + 3.79 mark + 0.95 zapw +
  写锁段 **1.9 段(BASE 自身 1.55 段 mmap_lock 段)** + ranged(4) flush 2.0(BASE 1.6);
  profile 两臂同形(x2apic/唤醒/切换), corten self 仅 3.5pp(zap_window 2.56 + xas_load 0.61 +
  txn_meta_drop 0.33);mmap_lock success 100% vs BASE 读 75.6%,"**无竞争劣化**"
  (REPORT.md:777-781)。perf2a 把该残差机制结论定为 **PMD 窗 park/take 512-PTE 扫描
  ~13µs/op + IPI/调度地板**(REPORT.md:767-769)。
- **query 形状**: 生产代码没有"只读事务"消费者——`corten_query()` 全部出现在将要写
  metadata 的事务体内(arena.c:2852/3048/4468 快照镜像、:7087 pending perm、
  arena.c:8100+ swap-out), INV7 checker 走查也取写事务(STATE r07-m5t1b ②)。
  debugfs dump 读侧已无锁(RCU xa_for_each, corten.c:1518-1536)。

### 2.3 adv 化的改动面清单(替换点 file:line)

| 改动点 | 现状 | adv 形态下的变化 | 面积 |
|---|---|---|---|
| `corten_txn_begin()` 真实视图路径 | read 舞蹈 + write(mm/corten.c:823-834, 870-879) | 单层直取: pin → `write_lock_bh` → stale 查 → 失败退锁重试 | **~40-60 行**(切片 ADV-1) |
| 通用多层协议核 | 路径读锁 + fill_hole 升级(corten.c:601-701) | **保留不动**(KUnit 合成视图的论文忠实覆盖面 + 上层描述符落地时的既有机制) | 0 |
| desc->lock 类型 | `rwlock_t` + 每层 lock class(corten.c:113-131, :332) | 不变(事务体互斥两协议同型;锁类机制同样服务于未来多层) | 0 |
| BH 对称 | 全链 `*_lock_bh`(corten.c:290/:435/:823/:873/:931;论证 :400-417) | **不变**(见 §3.2) | 0 |
| uninstall/stale | 已是 Fig 7 形态(corten.c:419-441) | 不变 | 0 |
| DFS 阶段 | 无 | **不新增**(A5 不可达, §1) | 0 |
| 无锁 query(seqcount) | 无 | **不新增**(无消费者, §2.2;若强加需全 meta 访问点 READ_ONCE/data_race + KCSAN 面, 负收益) | 0 |

结论: "把 HEAD 从当前形态推进到论文 _adv"的全部真实增量 = ADV-1 一片。

---

## 3. 并发正确性设计(ADV-1 与假想 adv 变体)

### 3.1 读侧重试的内存序

- **现状(也是 ADV-1 后)**: 一切共享状态读写由 desc->lock 同一侧串行化——stale 发布在
  写锁下(corten.c:435-437), 消费在同一把写锁下(:874-879), **纯锁中介序, 无需显式 barrier**。
  meta 数组发布由同一把锁护(`corten_meta_ensure_locked` 注释 :265-268: "The lock ... makes
  the publish plain");pin 协议的跨 CPU 序由 RCU 提供(xa_erase 先于 base put, kfree_rcu
  等在飞 lookup, corten.c:188-233)——这是 A4 的序论证, M3a 已用 lockdep+KUnit 验收。
- **若走 seqcount 读侧(否决项 ADV-4, 记录论证)**: 需要 per-desc `seqcount_t`, 写侧在
  事务体首尾 `write_seqcount_begin/end`(其本质仍是 spinlock+计数, **不省任何互斥**),
  读侧 `read_seqcount_retry` 重试(smp_rmb 语义)。代价: meta 全部访问点
  (corten_map/mark/unmap/swap_* 直接 `*m = *meta` 赋值, corten.c:1206-1208/:1269/:1326/:1438-1442)
  必须 READ_ONCE/data_race 化 + KCSAN 全量清噪;收益: 唯一理论消费者是"无写 query",
  而 §2.2 已证该消费者**不存在**(所有 query 都在同一事务内转为写)。**判: 负收益, 不做。**
- **论文对照**: paper _adv 读侧(遍历)无锁的序依据是 RCU 发布/CC 在 Fig 7 的
  "先原子清父 PTE 再延迟释放";本移植的等价链(xa_erase→stale→kfree_rcu)已落地,
  ADV-1 不触碰这条链。

### 3.2 write_lock_bh 在 adv 下变什么 —— 不变, 且必须不变

BH 对称的存在理由(mm/corten.c:400-417, M3a F1): 卸装者可从 RCU_SOFTIRQ 到达
(khugepaged `pte_free_defer→call_rcu(pte_free_now)→pte_free`), plain spin 会在
"同 CPU 任务态持有者被中断进 softirq"形状下自旋死锁。**论文 _adv 的 covering 锁同为
互斥自旋锁**(:559 "mutually-exclusive spin-locks"), 事务体同样在锁内, 所以:
- adv 化**不改变**互斥集合 → BH 对称论证逐字保留;
- 若未来把锁换成 `write_lock_irqsave` 或 plain `write_lock`, 该死锁形状复活——
  ADV-1 必须**保持** `write_lock_bh` 原语, 只删读阶段;
- seqcount 变体(AVD-4)的写侧同样躲不开 BH(写侧本体还是锁), 再次佐证其无意义。

### 3.3 与 M6 swap 事务 / mprotect pending perm / fork frozen 窗的交互

关键事实: **这三类写方的互斥全部由 covering desc 写锁承载, adv(任何变体)保持
covering 写锁 → 互斥零变化**。论文 _adv 的事务体本来就是两阶段锁相(:602-606),
"读方无锁"只指遍历阶段。逐项:

- **M6 swap-out**(rmap 走查者上下文, folio lock 之下): `corten_rmap_swap_out` 经
  `corten_lock_range` 取 desc 写锁再嵌 ptl(arena.c:8033-8100+, 注释 :8011 "desc->lock(W,BH)
  > ptl -- DEV-13 方向")。ADV-1 后同锁同序。
- **M6 swap-in**(无锁窗 + 重锁重验): 换入在锁外读 folio, 然后 `corten_lock_range` 重锁 +
  `corten_query` 重验 slot 未迁移(arena.c:4463-4478)——这**本身就是论文 Fig 7 的
  stale-retry 形状**在生产的实例, ADV-1 零影响。
- **mprotect pending perm**: `corten_arena_protect_window` 在 desc 写锁内先改 metadata
  (pending perm, arena.c:7086-7110)再逐 PTE 重编码(ptl 嵌套, :7092-7094 注释), 单窗单锁
  (arena.c:7176-7179 注释)。写方全在锁下 → adv 化无感。
- **fork frozen 窗**: 冻结=frozen 位 + ctl_lock + 双 mmap_write(DEV-14/15, arena.c:2695+,
  :2725-2726 assert);事务侧的互斥靠 percpu_ref tryget/drain, **从不取 ctl_lock**
  (arena.c:15-28 头注)。desc 写锁与冻结窗是正交的两级——事务若在冻结窗内到达, 由
  frozen 检查拒绝而非锁竞争;ADV-1 不触碰任一级。
- **锁序账**: DEV-13/M6-RMAP-D4 序 `mmap_write > ctl_lock > drain-wait > [folio_lock] >
  [swap锁] > fill_lock > desc->lock(W,BH) > ptl`(MV_VMA_FREE_SPEC.md §2.7 :277-291 引 D4)
  **零新增边**。注意快钩事务甚至**不在 mmap_lock 之下**(fault.c 快钩在 lock_vma_under_rcu
  之前, arena.c:4912-4915 注释)——desc->lock 的外侧只可能是 fill_lock/folio_lock 等
  更窄的锁, 这已经是今天的形状, ADV-1 不变。

### 3.4 ADV-1 后的语义等价论证(补丁草稿的正确性核心)

删读阶段的合法性与现状注释同源(corten.c:737-756, :891-898):
1. descent 判定(`child_covers_range`, :833)是纯算术, 输入 start/end/level 均不可变
   (desc->level 出生即定, :126-131), 不需要锁护;
2. 读阶段的 stale 预检(:824-829)冗余——写锁下必重查(:874), 两查之间没有事务体会"通过";
3. path 压/弹(:830/:871)在单层视图是 no-op;path 释放(:899)为空循环;
4. 互斥集合不变: 事务体仍在同一把 covering 写锁下;uninstall 互锁(M3a)不变;
5. 失败语义不变: stale/-ENOENT/-EOPNOTSUPP 的返回时机与错误码逐一同今日写锁路径
   (stale 预检删除后, "根已 stale"的失败从读阶段移到写阶段, 错误码同为 -EAGAIN,
   arena.c 重试预算不变)。

### 3.5 ADV-1 补丁草稿(原型级, 以 diff 语义伪码表达)

```c
/* mm/corten.c: corten_txn_begin() 增加单层快径(生产视图), 通用路径原样保留。 */

/* ops 增加一个位: 该视图只有一层追踪(covering = root, 无后代)。 */
struct corten_tree_ops {
        ...                      /* 既有三回调不动 */
        bool     single_level;   /* NEW: real view sets it; synthetic views don't */
};

static const struct corten_tree_ops corten_real_ops = {
        .root = corten_real_root,
        .child = corten_real_child,
        .alloc = corten_real_alloc,
        .single_level = true,            /* NEW */
};

int corten_txn_begin(...)
{
        ...
        cur = ops->root(ctx, start);
        if (!cur)  return -ENOENT;
        if (IS_ERR(cur)) return PTR_ERR(cur);

        if (ops->single_level) {         /* NEW: ADV-1 fast path */
                /* 根即 covering(范围已被 lock_range 钉在单 PMD 窗,
                 * corten.c:1057-1064)。直取写锁 = 论文 Fig 6 L9;
                 * stale 失败退锁返回, 重试策略留在调用方(与今日 -EAGAIN 同)。 */
                write_lock_bh(&cur->lock);
                if (unlikely(READ_ONCE(cur->stale))) {
                        write_unlock_bh(&cur->lock);
                        corten_ptdesc_put(cur);
                        return -EAGAIN;          /* 同今日 :874-879 语义 */
                }
                txn->covering = cur;
                txn->level = cur->level;
                if (!READ_ONCE(cur->va_base))
                        WRITE_ONCE(cur->va_base,
                                   corten_covering_va_base(start, cur->level));
                goto stats;                      /* nr_txns 计账同今日 :901-906 */
        }

        /* ...既有通用多层循环(路径读锁/fill_hole/L7L8 升级)逐字保留:
         * KUnit 合成视图的论文忠实覆盖面 + 未来上层描述符的既有机制。 */
        ...
}
```

红线: ①`txn->nr_path` 保持 0, `corten_txn_finish` 的 `WARN_ON_ONCE(txn->nr_path)`
分支不触发(finish :921-928 已覆盖 covering==NULL;covering!=NULL 时只 `write_unlock_bh`,
:930-934, 无 nr_path 依赖)②错误码/返回时机逐点对齐 §3.4-5③=n 折叠零成本(函数本体
已在 `corten_enabled_static()` 之后)。

---

## 4. 性能预期(诚实外推)

### 4.1 论文证据的确切含义

- **Fig 13(单线程, vs Linux, paper.txt:915-916)**: adv = mmap -3.1 / mmap-PF +46.8 /
  PF +53.6 / unmap-virt +76.9 / unmap +7.8(%);rw = -19.2 / +22.4 / +28.4 / +56.0 / +18.5。
  **rw 单线程就比 adv 慢 16-25pp**, 论文归因(:954-958): "acquiring read locks, which is
  more costly than RCU"——注意这是**4 级描述符树上每次事务 4 把路径读锁**的价格。
- **Fig 14(多线程, :938-967)**: adv 低争用近线性(384 核对 Linux 33×-2270×), 高争用
  64 线程封顶(**末级 PT 页争用**, :950-953)仍 3×-1489×;rw 低争用仅 1.8×-275×,
  PF 形状甚至被 Linux 反超 1.8×(:957-960)。rw-adv 差距的机制归因(:954-955):
  **"readers-writer locks vs. lockless reads in the first phase"**——即遍历阶段读锁的
  cacheline 弹跳, 在 64-384 核上爆炸。
- **适用性判据**: 该差距 ∝ (路径读锁把数 × 核数)。本移植生产视图路径读锁把数 = 0
  (§2.1, 唯一的一对读锁还是同一 cacheline 上的同锁折返, §2.3/ADV-1), 核数 = 8 vCPU KVM。

### 4.2 外推到本环境

| 论文差距源 | 本移植是否存在 | 本环境可兑现的收益 |
|---|---|---|
| 路径读锁 cacheline 弹跳(Fig 14 主因) | **不存在**(无上层描述符;唯一读折返见 ADV-1) | ADV-1: 每 op 省 ~2 次 atomic RMW + 1 对 BH 开关, 同 cacheline 无争用放大;折算 <0.1µs/op |
| 无锁遍历 vs 读锁遍历的单线程差(Fig 13 16-25pp) | 大半不存在(遍历本就无锁);剩余即 ADV-1 的折返 | 纯 pf 通道已净胜 +7~+22%(REPORT.md:771), 无 16-25pp 形状的债可还 |
| 高争用末级 PT 页封顶(Fig 14 高争用) | 同样存在且**两协议同型**(covering 写锁是 _adv 的事务体锁, 论文自己也封顶) | adv **不解决**同窗串行化;当前 8 vCPU 实测无竞争劣化(REPORT.md:780) |
| 384 核规模斜率 | 不可测(D4: 8 vCPU 方向性验证) | 0 |

### 4.3 T5 地板的预期消除幅度(逐项, REPORT.md §4.6 口径)

| 地板构成 | 量级 | adv 可消除? |
|---|---|---|
| 每-fault 事务簿记(fill_upper/xa_load/query/mark) | -7.1~-11% | **否**(metadata 唯一真源的语义价;论文 _adv 事务体同费) |
| take/park 簿记(check_empty 512-PTE 复扫等) | 3-5% | **否**(T1c 池生命周期, 与锁协议无关) |
| PMD 窗 park/take 512-PTE 扫描 | ~13µs/op | **否**(已登记杠杆 = 批 mark/窗粒度重构 §7-C1) |
| IPI/调度地板(ranged flush 2.0 vs 1.6 次/op) | 余量主体 | **否**(TLB 通道, 论文用可扩展 shootdown 另解, 非锁协议) |
| desc 写锁段计数(1.9 vs BASE 1.55 段/op) | 结构差 +0.35 段 | **部分**: ADV-1 减的是每段的锁内开销, 段数由事务数决定, 不变 |
| 读折返(ADV-1 对象) | <0.1µs/op | **是(全部)**, 但基数太小 |

**量化裁决**: mmap-pf 家族残差带宽 -14~-22%(上沿 -31, REPORT.md:546-547, :769-770),
adv 化的理论消除上限 ≈ 读折返 + 写锁段内微开销 ≈ **地板的 0.1% 量级, 绝对值 <0.5%**
——在 pf 家族 CV 15~79% 的方差(REPORT.md:747-748)下**不可测量**。期望收益若要超过
测量噪声, 需要的不是 adv 而是已登记的 C1 杠杆。

---

## 5. 实现切片

| 切片 | 内容 | diff 预估 | KUnit 锚 | 风险 | M-V 顺序依赖 |
|---|---|---|---|---|---|
| **ADV-0** | 本设计文档 + 协议命名勘误登记(REPORT 披露: HEAD 形态实为"单层 adv", "rw"为历史命名) | 0 行代码 | — | 0 | 无 |
| **ADV-1** | `corten_txn_begin` 单层快径(§3.5): ops 增加 `.single_level`, 真实视图跳过路径读锁舞蹈;通用多层路径与 KUnit 合成视图**原样保留** | ~40-60 行 + 注释 | ①既有 corten 套件全量回归(事务/stale/-EAGAIN 语义锚已覆盖);②**新增**: 合成单层视图驱动快径的 stale-retry 用例(uninstall 与 begin 竞速, 断言 -EAGAIN 且无泄漏——现套件的 stale 用例走通用路径, 快径需独立锚);③lockdep 变体过 BH 对称探针 | **低**(互斥集合/错误码逐点不变;最大风险=快径漏掉 va_base 首访记录 → 用 dump va_base 断言锚住) | **无结构依赖**(只动 mm/corten.c;M-V A.1 动 arena.c/mmap 路由)。建议**排在 M-V A.1 之后**: 单写者带宽让给在制的 V 系列;且 A.1 落地后快钩 fault 路径是快径的直接受益者, 一并回归 |
| ADV-2(可选, 数据门控) | 若做 ADV-1: mmbench pf/mmap-pf t8 双臂 + lock 段 ftrace 复测, 预登记"无回退"判据(CV 方差下不承诺正向读数) | 0 行 | — | 测量噪声误判 → 用多 boot 中位口径(STATE r07 惯例) | 跟随 ADV-1 |
| **ADV-3(否决)** | 论文 _adv 的 DFS 阶段/RCU monitor 重写 | — | — | 结构不可达(A5)/同构物已存在(A4) | 不排期 |
| **ADV-4(否决)** | seqcount 无锁 query(超论文) | — | — | 无消费者(§2.2)+ KCSAN 全量清噪成本 | 不排期 |

**与 M-V 系列的总关系**: M-V 不改变本评估的任何前提——①区域记录不是锁,
"mmap_lock 就是它的锁"(MV_VMA_FREE_SPEC.md §2.7);②fault 读侧快钩在 V-A 后行为不变
(C1/C2 行: "钩子已是 metadata 驱动;无变化在读侧");③锁序 D4 序列零新增边。
**先 M-V 后 ADV-1**(软顺序, 非依赖): V 系列是 D20 用户直接指令的主轨, ADV-1 是
 oportunistic 微优化, 不应占据单写者窗口;若 V-A.1 review 间隙空出, ADV-1 可插队
(文件不相交, rebase 风险≈0)。

---

## 6. 裁决建议

**建议: 完整 CortenMM_adv 不做;只做 ADV-1 一片(低优先, 挂 M-V A.1 之后);ADV-3/ADV-4
明确否决。**

理由(每条带前文出处):

1. **评估对象大半不存在**: 论文 _adv 的四个要素中, A1/A3/A4/A6 已在 M2b/M3a 落地
   (§1 映射表), A5(DFS)结构不可达, 任务书预设的 seqcount 要素论文里没有(A7)。
   "adv 化"在本代码库的真实含义只剩"删一次同锁读折返"(§2.3)。
2. **收益对象不存在**: 论文 rw-vs-adv 差距 = 多级路径读锁 × 核数(paper.txt:954-955);
   本移植生产视图路径读锁 = 0, 8 vCPU 实测"无竞争劣化"(REPORT.md:780)。
   T5 -14~-22% 地板的构成(事务簿记/take-park/512-PTE 扫描/IPI)没有一个成员在
   adv 的可消除面内(§4.3 表)。**adv 解决的是本移植没有的病。**
3. **风险/收益倒挂**: 唯一真正热、正确性密度最高的资产就是这把 desc 写锁下的
   事务体(25 提交全绿、lockdep/KCSAN/KUnit 三重验收, STATE r07 全程);为 <0.5%
   的理论收益重排协议, 违背本项目的验收经济学。ADV-1 恰好是"把现状注释里已经
   承认无用的锁钱停掉"(corten.c:737-756), 才值得一片。
4. **论文数字不能直译**: Fig 13 单线程 16-25pp 差距的前提(每次事务 4 把路径读锁)
   与 Fig 14 的 384 核条件在本环境均不成立(§4.1-4.2);D4 已裁定 8 vCPU 只做方向性
   验证——adv 的方向性优势本移植在结构上已经预支(遍历无锁)。

**条件翻转点(满足任一, 重新开评估, 先量测后动码)**:
- (a) 上层页描述符(pmd/pud hooks, corten.c:999-1001 预留)真实落地, 树变多层——
  路径读锁与 DFS 议题复活, 届时按论文两案重评(且多层视图的 KUnit 基建已在);
- (b) 硬件基座换 ≥64 核物理机且 workload 出现跨窗共享锁点争用;
- (c) 任何实测显示 desc 写锁段出现等待时间(而非段内开销)显著项
  (ftrace lock 段 wait 时间可测且 > 噪声)——先归因再动协议。

**一句裁决**: adv 在本移植不是"待升级的选项", 而是"已经无意中做完了大半的协议";
剩下的大半张饼(DFS/seqcount)要么不可达要么没人吃;把 ADV-1 记为热路径微优化债,
主轨仍是 M-V。

---

## 附: 引用索引(复核用)

论文(paper.txt): :141-143(两协议定义) / :401-414(并发语义两条) / :437-459(Fig 5 rw) /
:461-499(Fig 6 adv 伪码) / :559-599(adv 两阶段+DFS 必要性) / :600-625(Fig 7 RCU monitor+
stale) / :602-606(事务=两阶段锁相) / :664-668(LOC 225 vs 153) / :873-881(§5.1 adv 验证) /
:893-916(Table 3 + Fig 13 数字) / :938-967(§6.3 Fig 14 读数与归因)。
代码(mm/corten.c 除注明外): :54/:93(静态门) / :113-131(锁类) / :188-233(pin/RCU) /
:246-277(meta ensure) / :400-441(BH 对称+uninstall/stale) / :496-567(覆盖算术) /
:601-701(path 释放+fill_hole) / :704-909(begin 及 L2-L8 注释) / :737-756(真实视图无需
路径锁的自证) / :917-935(finish) / :948-1037(真实视图+不可达 child/alloc) /
:1042-1067(单窗上限) / :1077-1473(事务 API)。
mm/corten_arena.c: :15-32(并发契约头注) / :4463-4478(swap-in 重锁重验) / :4910-5032
(快钩零锁 fault) / :4988(Fig.7 retry cap 注释) / :5041+(慢钩) / :5240-5285(zap 窗
驱动出锁 flush) / :6884-7179(pending perm/protect) / :2695+(fork frozen) / :8011/:8033+
(swap-out 锁方向)。
其他: REPORT.md:733-781(§4.6+G2 trace+perf2a) / specs/MV_VMA_FREE_SPEC.md §1.2/1.3/2.7/
3.0-C1/C2 / STATE.md D3/D13/D15/D19/D20/M-V A.0 / include/linux/corten.h:50-57, 207, 287,
313, 607-611。
