# M-V A.2a/A.2b 入库前复审（r07-mva2.diff）

创建: 2026-09-22（维护者收口前最后一道门, 严格只读）
对象: `/home/ppw/cortenmm/patches/r07-mva2.diff`（2712 行, 与 worktree
`/home/ppw/linux-6.18-mva`（HEAD d40eae59ba76 + 未提交增量, 7 文件
+1249/−500）逐字节一致, 已 diff -q 核验）
基线证据对齐: 夜间矩阵全绿 + checkpatch 0E/0W/2C
（`results/r07/mva2/checkpatch-mva2.txt`）结论与之一致, 本复审未重跑。

## 判定: FAIL（1 阻断项 + 2 同批必修条件 + 3 次级条件）

核心架构判定全部通过: carrier 设计忠实于 SPEC §2.3; fork PTE 复制搬家
（copy_page_range 白名单 #1）锁形/断言/错误路径全部正确; LSM 补钩改判
（MV-4 修正）经源码核实**正确**; INV6 零违反（手滚 PTE 写整体删除而非
新增）; =n 折叠完整; legacy 零扰动; J1 前置/Dominion 栅栏门形正确;
B-4/B-5/B-1 登记口径与代码一致。阻断项集中在**锚未恢复完全**的残余角
与两个记账/协议缺口, 修法均为小改。

---

## 一、阻断项（FAIL 的理由）

### F1（阻断）: anchor-less 混合态 arena 的 fork 内容丢失——A.1 显式臂删除后该形状失去复制, 且未登记、未测试

**形状构造**（A.2 后唯一合法构造, 见 mva2-verify.md §0-T4 自述）:
`prctl(CORTEN_ARENA_DECLARE)` 建靶向窗 → munmap EXACT（park, 树影随
A.1 拆除）→ 再次 DECLARE 同窗（`declare_locked` 的 `pool_prepare_locked`
在 vma_lookup 校验**之前**短路重激活, 不需要任何树 VMA）→
`pool_reactivate(novma=false)` **不重武装 carrier**（mm/corten_arena.c:6978
`if (novma && !READ_ONCE(ar->carrier))`）→ 活窗无锚（ar->carrier==NULL ∧
ar->vma==NULL, `corten_region_record_ok` 认可, mm/corten_arena.c:963-967）。
随后 map_anon 的 NULL 容错臂（mm/corten_arena.c:4863-4872）正常落
rmap-less 页（`corten_arena_test_seed_mapped_rmapless` 即此形状）。

**fork 时发生什么**（`corten_arena_fork_mirror`, mm/corten_arena.c:3947-4141）:
1. 跳过判 `if (!any_piece && READ_ONCE(ar->vma))`（:3990）——anchor-less
   的 ar->vma==NULL → **不跳过**（连 fork_skips 都不计, 静默）;
2. `register_child` 正常注册子窗（pcarrier==NULL → 子窗同样无锚出生）;
3. `pcarrier = READ_ONCE(ar->carrier)`（:4041）== NULL → **copy_page_range
   不执行**;
4. 逐窗循环的 F2 门读**子侧** pmd（:4087-4090）——子侧无任何 PT 页 →
   `continue` → 元数据 replay 也跳过。

结果: 子进程得到真空窗, 首次 fault 走 FRESH 拿全新页——**fork 对私有
匿名内存的继承语义被破坏（子读零, 静默数据丢失）**。

**回归性**: A.1 的 `corten_arena_fork_copy_window_novma`（本片删除的
150 行, 主树 mm/corten_arena.c 可证）恰好覆盖此形状（门是
`!corten_arena_span_has_vma(oldmm,...)` = 无树 VMA 窗）; A.2 删除该臂的
论据"锚恢复后…已是残缺重写"（mva2-verify.md §0-B7）对 pool/auto 侧重
成立, 对 declare 侧重**不成立**——锚没有恢复。SPEC §4.3 MV-11 的缓解
承诺"每片 DoD 含混合态用例"在本片的 fork 消费面上未兑现: 重写后的
`corten_arena_test_fork_vma_free`（mm/corten_arena_test.c:5117 起）改走
`auto_mmap_route==2`（pool take, novma=true）→ **有** carrier, 不再测
anchor-less fork; `vma_free_shrink_pick` 造出 anchor-less 形状但只测
shrink pick 门, 不测 fork。§4 边界清单与 SPEC 偏差登记（§6）均无此条。

**修法**（三选一, 按推荐序）:
- （a, 推荐, ~1 行 + 测试改写）`pool_reactivate` 无条件重武装:
  mm/corten_arena.c:6978 去掉 `novma &&`。锚恢复完全 → fork/回收/pick
  全部走 carrier 正路; 副作用 = anchor-less 活窗不复存在,
  `vma_free_shrink_pick` 的"pick 门为该形状存在"前提失效——该测试改
  为断言 reactivate 后 carrier 非空（pick 门放行形状已有
  `carrier_shrink_pick` 承接）, 或以测试专用注入保留 pick-closed 臂;
- （b）`fork_register_child` 对无锚父窗补一个 declare 侧 carrier 重武装
  （父窗有 rmap-less 内容时 copy_page_range 的 folio 无 rmap 锚,
  `folio_try_dup_anon_rmap_pte` 形状不闭合——**不可行**, 列此排除）;
- （c）保留形状 + 恢复一个最小复制臂（等于复活 A.1 臂, 与本片裁决矛盾）。
  结论: (a) 是唯一干净修法; 若维护者坚持保留混合态, 则必须登记为
  S 系语义变更并补 anchor-less fork 用例（断言子窗内容丢失是**故意**）,
  但这与 POSIX fork 语义冲突, 不建议。

**复现脚本（供修后验证）**: mode_enter → declare(WIN) → munmap EXACT
→ declare(WIN) → 写一页 → fork → 子读该页; 期望（修复后）子读到父
内容（M5 判据 1 口径）。

## 二、同批必修条件（不修不应收口）

### C1: 子 carrier 的孤儿 anon_vma 触发 CONFIG_DEBUG_VM 下 unlink_anon_vmas 的 VM_WARN_ON

`carrier_alloc` 对**每个** carrier 做 `anon_vma_prepare`（mm/corten_arena.c:824）;
fork 侧 `anon_vma_fork(ccarrier, pcarrier)`（mm/corten_arena.c:3702）第一行
`vma->anon_vma = NULL`（mm/rmap.c anon_vma_fork）只丢指针、不递减
prepare 侧的 `num_active_vmas`, 也不动链上的 prepare avc。子 carrier
释放时 `unlink_anon_vmas` pass-2 对该空树根执行
`VM_WARN_ON(anon_vma->num_active_vmas)`（mm/rmap.c unlink_anon_vmas,
`#ifdef CONFIG_DEBUG_VM` 下为 WARN_ON）→ **每次 fork 子 mm 的 carrier
拆除都告警**。源码注记"one bounded, unreachable pair, no ref
imbalance"（mm/corten_arena.c:3703-3708）对 **refcount** 成立（alloc 的
1 由 pass-2 put_anon_vma 归零, 无泄漏）, 但漏了 num_active_vmas 协议。
夜间四套 KUnit config 均 `# CONFIG_DEBUG_VM is not set`
（results/r07/mva2/kunit-*.log 无 rmap.c WARN 与此一致）→ 矩阵不可能
捕到; 主线 debug 配置会响, 且违反本项目自己的"dmesg 静默"门。
**修法**: fork 子 carrier 用 dup_mmap 同形的裸 carrier（skip
anon_vma_prepare——给 `carrier_alloc` 加一个 bool 或拆一个
`carrier_alloc_bare`）, 让 anon_vma_fork 从零链开始; 注记整段删除。
顺带 `fork_register_child` 错误路径 `carrier_free` 重复 unlink（fork
失败臂已 unlink 过）为良性, 裸化后自然消除。

### C2: mremap_move 丢失 may_expand_vm 门（RLIMIT_AS 记账门回归）

旧径 `do_mmap(NULL, addr2, len2, ..., MAP_FIXED)` 经 mmap_region →
`__mmap_new_vma` 强制 `may_expand_vm`（mm/vma.c:2914）; 新径
`corten_arena_declare_locked(..., perm, true)`（mm/corten_arena.c:8617）
记账（vm_stat_account）但**无门**——`corten_auto_validate` 只挂在
auto_mmap_route（mm/corten_arena.c:3133-3141）, mremap 路由在
do_mremap 顶部拦截（mm/mremap.c:2012）, 也不会经过 vma_to_resize 的
may_expand 检查（mm/mremap.c:1406）。结果: RLIMIT_AS 压力下 arena
realloc 搬家/增长成功而 legacy 同形失败, total_vm 可超限。
**修法**（1 行）: mm/corten_arena.c:8617 前补
`if (!may_expand_vm(mm, corten_take_vm_flags(perm), len2 >> PAGE_SHIFT)) { mmap_write_unlock(mm); return -ENOMEM; }`
（或在 mremap_route 复用 corten_auto_validate 并计入 vgate）。

### C3: KUnit auto_validate 真值表 5+2 断言, SPEC 要求 ≥16 例

`corten_arena_test_auto_validate`（mm/corten_arena_test.c:5445-5501）:
5 个判例行（RW→0 / NONE→0 / def_flags VM_LOCKED→-EAGAIN /
PROT_EXEC→-EOPNOTSUPP / RLIMIT_AS=0→-ENOMEM）+ 2 个纯性断言。
代码分支全覆盖（4 个判定臂+perm_from_prot 0 值）, 但未达 SPEC
§3.1.2 "≥16 例"。**修法**: 补满输入空间（prot ∈ {NONE,R,RW,RX,RWX,
EXEC} × {def_flags 有无} × {RLIMIT_AS 正常/压线/0} 的真值表化循环,
含 len2 圆整边界 len=1 与 len=PMD_SIZE-1 的门等价性）, 或在偏差登记
里降额说明。

## 三、次级条件 / 观察项（不阻断）

- **O1（A.1 遗留, 本片未修也未加重）**: anchor-less 活窗 + 窗内植入
  洞时 RELEASE 走 any_vma 分支, 回冲以 carrier 为钥
  （mm/corten_arena.c:1627）→ 该形状 reactivate 时的 charge（
  mm/corten_arena.c:6997）**不回冲**, total_vm 超记至 mm 死亡。A.1 的
  二值分支同漏（主树可证）, 非本片引入; 若采纳 F1(a) 则此形状消失,
  建议随 F1 一并消失或登记 A.3。
- **O2（checkpatch 2C 裁定）**: C1 `mm/corten_arena.c:3082` 行尾 `'('`
  （corten_take_vm_flags( 折行）**应修**（trivial reflow）; C2
  `corten_arena_test.c:5397` `spinlock_t *ptl;` 无注释——与文件内既有
  21 处同形声明一致（A.1 的 9C 同类已放行）, **可留**。
- **O3（陈旧注记）**: `corten_arena_test_fork_vma_free` 中 "a rmap-less
  folio has mapcount 0 -- reuse is impossible"（mm/corten_arena_test.c
  fork_vma_free 体, pool take 重武装后 folio 已有 rmap）——注记与行为
  脱节, 顺手改。
- **O4（记录即可）**: takeover 早退跳过 `__get_unmapped_area` 内的
  `security_mmap_addr`（mm/mmap.c:976）——窗址恒 ≥16T 远超
  mmap_min_addr, 判定恒过, 无行为差; 记录备查。

## 四、逐焦点核对结果（任务清单对齐）

1. **A.2a 三态**: ret==1（新窗, do_mmap mm/mmap.c:446-460 直接
   `auto_attach` 后 return, 无 mmap_region）/ ret==2（parked 池复活,
   novma=true 重武装）/ vgate 拒绝（auto_vgate++ + fallback(NULL)
   降级 legacy 由其自答 errno, mm/corten_arena.c:3125-3130）——三态
   齐全。**LSM 补钩改判核实为正确**: 6.18 `vm_mmap_pgoff`
   （mm/util.c:580-583）对 file==NULL 无条件调 security_mmap_file +
   fsnotify_mmap_perm, 且 auto 形态（!file ∧ addr==0）的全部入口
   （vm_mmap_pgoff/vm_mmap）必经; 唯二直连 do_mmap 的 do_shmat（file
   映射, 不可达 auto）与 remap_file_pages（自带 security_mmap_file）。
   SPEC 前提失实, patch 的"不补调防双触发"是对的, 偏差已登记
   （mva2-verify.md §6-1）——**此条不扣分**。
2. **记账口径**: total_vm/RLIMIT_AS 对称——validate 用圆整 len2 门
   （mm/corten_arena.c:3133-3136）, declare 成功尾部同口径 charge
   （:1355-1360）, may_expand 与 charge 间同锁无他充; RLIMIT_DATA/
   exec_vm 经 corten_take_vm_flags 与 legacy vm_stat_account 同形
   （含 PROT_NONE 不记 data_vm）。pgtables_bytes 经 copy_page_range
   的标准 pte_alloc 路径自记, zap 侧 free_ptes_novma 对称。
   例外 = C2（mremap 丢门）与 O1（角 case 回冲漏）。
3. **A.2b carrier 生命周期**: alloc（mmap_write 下, vm_flags_init +
   vma_start_write 幂等语义核实[include/linux/mmap_lock.h:205-211,
   detach 时 __vma_enter_locked 直返 false]）/attach（DECLARE 发布序:
   anchor 先于 xa_store 先于 register tripwire, [FAIL-2] 序保持）/
   detach-park（carrier 跨 park 存续, 描述符寿命）/free（
   corten_arena_free:546-557, unlink 先于 kfree_rcu）——除 C1 的
   num_active_vmas 协议缺口外无泄漏无悬垂; 泄漏矩阵（guest 5→25
   carriers 对账）与此一致。
4. **fork_copy_ptes 搬家**: copy_page_range(ccarrier, pcarrier)
   （mm/corten_arena.c:4048）= 白名单 #1 原形, dst/src 序正确;
   vma_start_write(pcarrier)（:4047）满足 memory.c:1536 的
   vma_assert_write_locked(src)；锁序 = dup_mmap 同形（双 mmap_write,
   D13 无新边, 父/子 desc 树锁不嵌套的纪律未破——register_child 只锁
   子 ctl_lock, mirror 的 xa_load 无锁读发生在双 mmap_write 临界区）;
   错误路径 -ENOMEM → fork_commit break → MMF_UNSTABLE 既有 unwind。
   5 臂 fault ctx: get_vma/swap_in/prealloc 全部 anchor 化
   （:4747/:5315/:5937）, map_anon/restore/cow 的 `if (vma)` NULL 容错
   保留（:4872/:5563）。
5. **红线**: INV6 无违反——本片唯一 PTE 写动作是删除手滚臂并改走
   copy_page_range 事务白名单; =n 折叠完整（7 新符号全桩,
   corten_vma_find 定义 #ifdef 包裹, =n 14 对象夜验背书;
   `"../../mm/corten_arena.h"` 相对包含有 arch/x86/boot/compressed/
   ident_map_64.c 上游先例）; legacy 零扰动——所有新门
   （j1_probe/fence/addr_in_window/NOREPLACE range_overlaps）先过
   corten_enabled_static + mode 字节再触窗口比较, range_overlaps
   自带 state 空转 false（mm/corten_arena.c range_overlaps 头部）。
6. **并发自证**: 修改点锁配对全部成立（declare_locked/  
   fork_register_child 的 ctl_lock 取放含 unwind 臂）; RCU 合规
   （arenas_report 只打印 carrier 指针值不解引用; region iter 走
   既有 xa_for_each+去重先例）; 新计数器全为 atomic_long_t +
   atomic_long_inc（corten_arena_stat_add 的 this_cpu_add 为既有
   per-mm stats 基建, 非新引入）。
7. **KUnit 锚质量**: carrier_vma（:5502）/carrier_shrink_pick（:5389）
   为真断言（refcnt==0、树 NULL、total_vm 精确值、folio_ref==1、
   skipped 不动、RELEASE 后 probe NULL+total_vm 归零）; auto_validate
   见 C3; 翻转断言（vma_lookup EXPECT_NULL）与新语义一致。
8. **登记边界对齐**: B-4（GUP-slow/ptrace/process_vm 仍 -EFAULT/0
   字节, 代码未越界收口）✓; B-5（NOREPLACE 走 range_overlaps 跳
   idle 帧, mm/mmap.c:476-486 与登记一字不差; MAP_FIXED-over-parked
   由 mark 路由+[C1] eject 兜底 = S-7）✓; B-1 残余（rmap 锚经
   carrier 恢复 = 本片主体; GUP-slow EFAULT 恢复留 V-C,
   anchor_vma 已为其备好形状）✓。

## 五、修后复验清单（最小集）

1. F1 复现脚本（declare→park→declare→写→fork→子读）+ 重写的
   vma_free_shrink_pick（若走修法 (a)）;
2. DEBUG_VM=y（或至少 CONFIG_DEBUG_VM on 的 KUnit 变体）跑
   fork_vma_free/fork_faithful/pool_fork, 断言零 WARN（C1）;
3. RLIMIT_AS 压线下的 mremap 增长/搬家用例（C2）;
4. 既有夜矩阵全量回归 + checkpatch（C1 那处 reflow 后应 0E/0W/1C）。
